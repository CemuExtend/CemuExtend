#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/TcpGecko/TcpGeckoServer.h"
#include "Cafe/HW/Espresso/TcpGecko/TcpGeckoCodeHandler.h"
#include "Cafe/HW/Espresso/TcpGecko/TcpGeckoGuestJobs.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_set>

#if BOOST_OS_UNIX
#include <netinet/tcp.h>
#endif

std::unique_ptr<TcpGeckoServer> g_tcpGeckoServer;

namespace // TCPGecko commands
{
	constexpr uint8_t COMMAND_WRITE_8 = 0x01;
	constexpr uint8_t COMMAND_WRITE_16 = 0x02;
	constexpr uint8_t COMMAND_WRITE_32 = 0x03;
	constexpr uint8_t COMMAND_READ_MEMORY = 0x04;
	constexpr uint8_t COMMAND_READ_MEMORY_KERNEL = 0x05;
	constexpr uint8_t COMMAND_VALIDATE_ADDRESS_RANGE = 0x06;
	constexpr uint8_t COMMAND_MEMORY_DISASSEMBLE = 0x08;
	constexpr uint8_t COMMAND_KERNEL_WRITE = 0x0B;
	constexpr uint8_t COMMAND_KERNEL_READ = 0x0C;
	constexpr uint8_t COMMAND_TAKE_SCREEN_SHOT = 0x0D;
	constexpr uint8_t COMMAND_UPLOAD_MEMORY = 0x41;
	constexpr uint8_t COMMAND_SERVER_STATUS = 0x50;
	constexpr uint8_t COMMAND_GET_DATA_BUFFER_SIZE = 0x51;
	constexpr uint8_t COMMAND_READ_FILE = 0x52;
	constexpr uint8_t COMMAND_READ_DIRECTORY = 0x53;
	constexpr uint8_t COMMAND_REPLACE_FILE = 0x54;
	constexpr uint8_t COMMAND_GET_CODE_HANDLER_ADDRESS = 0x55;
	constexpr uint8_t COMMAND_READ_THREADS = 0x56;
	constexpr uint8_t COMMAND_ACCOUNT_IDENTIFIER = 0x57;
	constexpr uint8_t COMMAND_FOLLOW_POINTER = 0x60;
	constexpr uint8_t COMMAND_REMOTE_PROCEDURE_CALL = 0x70;
	constexpr uint8_t COMMAND_GET_SYMBOL = 0x71;
	constexpr uint8_t COMMAND_MEMORY_SEARCH_32 = 0x72;
	constexpr uint8_t COMMAND_ADVANCED_MEMORY_SEARCH = 0x73;
	constexpr uint8_t COMMAND_EXECUTE_ASSEMBLY = 0x81;
	constexpr uint8_t COMMAND_PAUSE_CONSOLE = 0x82;
	constexpr uint8_t COMMAND_RESUME_CONSOLE = 0x83;
	constexpr uint8_t COMMAND_IS_CONSOLE_PAUSED = 0x84;
	constexpr uint8_t COMMAND_SERVER_VERSION = 0x99;
	constexpr uint8_t COMMAND_GET_OS_VERSION = 0x9A;
	constexpr uint8_t COMMAND_SET_DATA_BREAKPOINT = 0xA0;
	constexpr uint8_t COMMAND_SET_INSTRUCTION_BREAKPOINT = 0xA2;
	constexpr uint8_t COMMAND_TOGGLE_BREAKPOINT = 0xA5;
	constexpr uint8_t COMMAND_REMOVE_ALL_BREAKPOINTS = 0xA6;
	constexpr uint8_t COMMAND_POKE_REGISTERS = 0xA7;
	constexpr uint8_t COMMAND_GET_STACK_TRACE = 0xA8;
	constexpr uint8_t COMMAND_GET_ENTRY_POINT_ADDRESS = 0xB1;
	constexpr uint8_t COMMAND_RUN_KERNEL_COPY_SERVICE = 0xCD;
	constexpr uint8_t COMMAND_IOSU_HAX_READ_FILE = 0xD0;
	constexpr uint8_t COMMAND_GET_VERSION_HASH = 0xE0;
	constexpr uint8_t COMMAND_PERSIST_ASSEMBLY = 0xE1;
	constexpr uint8_t COMMAND_CLEAR_ASSEMBLY = 0xE2;

	constexpr uint32_t kDataBufferSize = 0x5000;
	constexpr uint32_t kVersionHash = 0x7FB223;
	constexpr uint8_t kOnlyZerosRead = 0xB0;
	constexpr uint8_t kNonZerosRead = 0xBD;

	uint32_t ReadBE32(const uint8_t* p)
	{
		return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
	}
	uint16_t ReadBE16(const uint8_t* p)
	{
		return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
	}
	void WriteBE32(uint8_t* p, uint32_t v)
	{
		p[0] = uint8_t(v >> 24);
		p[1] = uint8_t(v >> 16);
		p[2] = uint8_t(v >> 8);
		p[3] = uint8_t(v);
	}

	std::mutex s_pauseMutex;
	TcpGecko::PauseOwnership s_pauseOwners;
	bool s_guestPauseApplied{};

	OSThread_t* ThreadById(MPTR threadId)
	{
		return (OSThread_t*)memory_getPointerFromPhysicalOffset(threadId);
	}

	void SuspendAllGuestThreads()
	{
		__OSLockScheduler();
		for (sint32 i = 0; i < activeThreadCount; i++)
			coreinit::__OSSuspendThreadNolock(ThreadById(activeThread[i]));
		__OSUnlockScheduler();
	}

	void ResumeAllGuestThreads()
	{
		__OSLockScheduler();
		for (sint32 i = 0; i < activeThreadCount; i++)
			coreinit::__OSResumeThreadInternal(ThreadById(activeThread[i]), 1);
		__OSUnlockScheduler();
	}

	void ResetGuestPause()
	{
		std::scoped_lock lock(s_pauseMutex);
		s_pauseOwners.Reset();
		if (s_guestPauseApplied && coreinit::OSIsSchedulerActive())
			ResumeAllGuestThreads();
		s_guestPauseApplied = false;
	}

	OSThread_t* FirstActiveThread()
	{
		__OSLockScheduler();
		OSThread_t* result = activeThreadCount > 0 ? ThreadById(activeThread[0]) : nullptr;
		__OSUnlockScheduler();
		return result;
	}

	std::mutex s_breakpointMutex;
	std::unordered_set<uint32_t> s_breakpointAddresses;

} // namespace

TcpGeckoServer::TcpGeckoServer(uint16_t port, bool allowLan)
	: m_port(port), m_allowLan(allowLan)
{
#if BOOST_OS_WINDOWS
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

TcpGeckoServer::~TcpGeckoServer()
{
	m_stopRequested.store(true);
	if (m_clientSocket != INVALID_SOCKET)
	{
		shutdown(m_clientSocket, 2 /* SHUT_RDWR / SD_BOTH */);
		closesocket(m_clientSocket);
		m_clientSocket = INVALID_SOCKET;
	}
	if (m_serverSocket != INVALID_SOCKET)
	{
		closesocket(m_serverSocket);
		m_serverSocket = INVALID_SOCKET;
	}
	if (m_thread.joinable())
		m_thread.join();
#if BOOST_OS_WINDOWS
	WSACleanup();
#endif
}

bool TcpGeckoServer::Initialize()
{
	if (m_initialized.load())
		return true;

	m_serverSocket = socket(PF_INET, SOCK_STREAM, 0);
	if (m_serverSocket == INVALID_SOCKET)
		return false;

	int reuseEnabled = 1;
	setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseEnabled, sizeof(reuseEnabled));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(m_port);
	addr.sin_addr.s_addr = m_allowLan ? htonl(INADDR_ANY) : htonl(0x7F000001u); // 127.0.0.1

	if (bind(m_serverSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
	{
		cemuLog_log(LogType::Force, "TCPGecko: failed to bind port {}", m_port);
		closesocket(m_serverSocket);
		m_serverSocket = INVALID_SOCKET;
		return false;
	}
	if (listen(m_serverSocket, 4) == SOCKET_ERROR)
	{
		closesocket(m_serverSocket);
		m_serverSocket = INVALID_SOCKET;
		return false;
	}

	m_buffer.resize(kDataBufferSize);
	m_thread = std::thread(&TcpGeckoServer::ThreadFunc, this);
	m_initialized.store(true);
	cemuLog_log(LogType::Force, "TCPGecko: listening on {}:{}", m_allowLan ? "0.0.0.0" : "127.0.0.1", m_port);
	return true;
}

void TcpGeckoServer::ThreadFunc()
{
	while (!m_stopRequested.load())
	{
		sockaddr_in clientAddr{};
		socklen_t clientAddrLen = sizeof(clientAddr);
		SOCKET clientSocket = accept(m_serverSocket, (sockaddr*)&clientAddr, &clientAddrLen);
		if (clientSocket == INVALID_SOCKET)
		{
			if (m_stopRequested.load())
				break;
			continue;
		}
		if (m_stopRequested.load())
		{
			closesocket(clientSocket);
			break;
		}

		int nodelayEnabled = 1;
		setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelayEnabled, sizeof(nodelayEnabled));

		m_clientSocket = clientSocket;
		m_clientConnected.store(true);

		char ipStr[64] = {};
		snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u",
				 (unsigned)(uint8_t)(clientAddr.sin_addr.s_addr), (unsigned)(uint8_t)(clientAddr.sin_addr.s_addr >> 8),
				 (unsigned)(uint8_t)(clientAddr.sin_addr.s_addr >> 16), (unsigned)(uint8_t)(clientAddr.sin_addr.s_addr >> 24));

		LatteOverlay_pushNotification(fmt::format("TCPGecko is now connected to {}!", ipStr), 3000);
		ProcessClient(clientSocket, ipStr);

		closesocket(clientSocket);
		m_clientSocket = INVALID_SOCKET;
		m_clientConnected.store(false);

		if (!m_stopRequested.load())
			LatteOverlay_pushNotification(fmt::format("TCPGecko disconnected from {}", ipStr), 3000);
	}
}

bool TcpGeckoServer::RecvExact(SOCKET s, void* buffer, size_t size)
{
	auto* cursor = static_cast<uint8_t*>(buffer);
	size_t remaining = size;
	while (remaining > 0)
	{
		int received = recv(s, (char*)cursor, (int)remaining, 0);
		if (received <= 0)
			return false;
		cursor += received;
		remaining -= (size_t)received;
	}
	return true;
}

bool TcpGeckoServer::SendExact(SOCKET s, const void* buffer, size_t size)
{
	const auto* cursor = static_cast<const uint8_t*>(buffer);
	size_t remaining = size;
	while (remaining > 0)
	{
		int sent = send(s, (const char*)cursor, (int)remaining, 0);
		if (sent <= 0)
			return false;
		cursor += sent;
		remaining -= (size_t)sent;
	}
	return true;
}

bool TcpGeckoServer::SendByteValue(SOCKET s, uint8_t value)
{
	return SendExact(s, &value, 1);
}

bool TcpGeckoServer::SendU32(SOCKET s, uint32_t value)
{
	uint8_t buffer[4];
	WriteBE32(buffer, value);
	return SendExact(s, buffer, 4);
}

bool TcpGeckoServer::RecvLengthPrefixedString(SOCKET s, std::vector<uint8_t>& out, uint32_t maxSize)
{
	uint8_t lengthBuffer[4];
	if (!RecvExact(s, lengthBuffer, 4))
		return false;
	uint32_t length = ReadBE32(lengthBuffer);
	if (length > maxSize)
		return false;
	out.resize(length);
	if (length > 0 && !RecvExact(s, out.data(), length))
		return false;
	return true;
}

void TcpGeckoServer::ProcessClient(SOCKET clientSocket, const std::string& clientIp)
{
	while (!m_stopRequested.load())
	{
		uint8_t command = 0;
		int ret = recv(clientSocket, (char*)&command, 1, 0);
		if (ret <= 0)
			return;

		bool ok = true;
		switch (command)
		{
		case COMMAND_WRITE_8:
			ok = CmdWrite(clientSocket, 1);
			break;
		case COMMAND_WRITE_16:
			ok = CmdWrite(clientSocket, 2);
			break;
		case COMMAND_WRITE_32:
			ok = CmdWrite(clientSocket, 4);
			break;
		case COMMAND_READ_MEMORY:
		case COMMAND_READ_MEMORY_KERNEL:
			ok = CmdReadMemory(clientSocket);
			break;
		case COMMAND_VALIDATE_ADDRESS_RANGE:
			ok = CmdValidateAddressRange(clientSocket);
			break;
		case COMMAND_MEMORY_DISASSEMBLE:
			ok = CmdDisassemble(clientSocket);
			break;
		case COMMAND_KERNEL_WRITE:
			ok = CmdWrite(clientSocket, 4);
			break;
		case COMMAND_KERNEL_READ:
		{
			uint8_t addrBuf[4];
			if (!RecvExact(clientSocket, addrBuf, 4))
			{
				ok = false;
				break;
			}
			uint32_t address = ReadBE32(addrBuf);
			uint32_t value = memory_isAddressRangeAccessible(address, 4) ? memory_readU32(address) : 0;
			ok = SendU32(clientSocket, value);
			break;
		}
		case COMMAND_TAKE_SCREEN_SHOT:
			ok = CmdTakeScreenShot(clientSocket);
			break;
		case COMMAND_UPLOAD_MEMORY:
			ok = CmdUploadMemory(clientSocket);
			break;
		case COMMAND_SERVER_STATUS:
			ok = CmdServerStatus(clientSocket);
			break;
		case COMMAND_GET_DATA_BUFFER_SIZE:
			ok = CmdGetDataBufferSize(clientSocket);
			break;
		case COMMAND_READ_FILE:
			ok = CmdReadFile(clientSocket);
			break;
		case COMMAND_READ_DIRECTORY:
			ok = CmdReadDirectory(clientSocket);
			break;
		case COMMAND_REPLACE_FILE:
			ok = CmdReplaceFile(clientSocket);
			break;
		case COMMAND_GET_CODE_HANDLER_ADDRESS:
			ok = CmdGetCodeHandlerAddress(clientSocket);
			break;
		case COMMAND_READ_THREADS:
			ok = CmdReadThreads(clientSocket);
			break;
		case COMMAND_ACCOUNT_IDENTIFIER:
			ok = CmdAccountIdentifier(clientSocket);
			break;
		case COMMAND_FOLLOW_POINTER:
			ok = CmdFollowPointer(clientSocket);
			break;
		case COMMAND_REMOTE_PROCEDURE_CALL:
			ok = CmdRemoteProcedureCall(clientSocket);
			break;
		case COMMAND_GET_SYMBOL:
			ok = CmdGetSymbol(clientSocket);
			break;
		case COMMAND_MEMORY_SEARCH_32:
			ok = CmdMemorySearch32(clientSocket);
			break;
		case COMMAND_ADVANCED_MEMORY_SEARCH:
			ok = CmdAdvancedMemorySearch(clientSocket);
			break;
		case COMMAND_EXECUTE_ASSEMBLY:
			ok = CmdExecuteAssembly(clientSocket);
			break;
		case COMMAND_PAUSE_CONSOLE:
			ok = CmdPauseConsole(clientSocket);
			break;
		case COMMAND_RESUME_CONSOLE:
			ok = CmdResumeConsole(clientSocket);
			break;
		case COMMAND_IS_CONSOLE_PAUSED:
			ok = CmdIsConsolePaused(clientSocket);
			break;
		case COMMAND_SERVER_VERSION:
			ok = CmdServerVersion(clientSocket);
			break;
		case COMMAND_GET_OS_VERSION:
			ok = CmdGetOsVersion(clientSocket);
			break;
		case COMMAND_SET_DATA_BREAKPOINT:
			ok = CmdSetDataBreakpoint(clientSocket);
			break;
		case COMMAND_SET_INSTRUCTION_BREAKPOINT:
			ok = CmdSetInstructionBreakpoint(clientSocket);
			break;
		case COMMAND_TOGGLE_BREAKPOINT:
			ok = CmdToggleBreakpoint(clientSocket);
			break;
		case COMMAND_REMOVE_ALL_BREAKPOINTS:
			ok = CmdRemoveAllBreakpoints(clientSocket);
			break;
		case COMMAND_POKE_REGISTERS:
			ok = CmdPokeRegisters(clientSocket);
			break;
		case COMMAND_GET_STACK_TRACE:
			ok = CmdGetStackTrace(clientSocket);
			break;
		case COMMAND_GET_ENTRY_POINT_ADDRESS:
			ok = CmdGetEntryPointAddress(clientSocket);
			break;
		case COMMAND_RUN_KERNEL_COPY_SERVICE:
			break;
		case COMMAND_IOSU_HAX_READ_FILE:
			break;
		case COMMAND_GET_VERSION_HASH:
			ok = SendU32(clientSocket, kVersionHash);
			break;
		case COMMAND_PERSIST_ASSEMBLY:
			ok = CmdPersistAssembly(clientSocket);
			break;
		case COMMAND_CLEAR_ASSEMBLY:
			ok = CmdClearAssembly(clientSocket);
			break;
		default:
			cemuLog_log(LogType::Force, "TCPGecko: unknown command byte {:#04x} from {}, closing connection", command, clientIp);
			return;
		}
		if (!ok)
			return;
	}
}

bool TcpGeckoServer::CmdWrite(SOCKET s, int size)
{
	uint8_t buffer[8];
	if (!RecvExact(s, buffer, 8))
		return false;
	uint32_t address = ReadBE32(buffer);
	if (!memory_isAddressRangeAccessible(address, (uint32_t)size))
		return true;
	if (size == 1)
		memory_writeU8(address, buffer[7]);
	else if (size == 2)
		memory_writeU16(address, ReadBE16(buffer + 6));
	else
		memory_writeU32(address, ReadBE32(buffer + 4));
	PPCRecompiler_invalidateRange(address, address + size);
	return true;
}

bool TcpGeckoServer::CmdReadMemory(SOCKET s)
{
	uint8_t buffer[8];
	if (!RecvExact(s, buffer, 8))
		return false;
	uint32_t start = ReadBE32(buffer);
	uint32_t end = ReadBE32(buffer + 4);
	while (start != end)
	{
		if (end <= start)
			break;
		uint32_t remaining = end - start;
		uint32_t length = std::min(remaining, kDataBufferSize);

		bool allZero = true;
		if (memory_isAddressRangeAccessible(start, length))
		{
			const uint8_t* src = memory_getPointerFromVirtualOffset(start);
			for (uint32_t i = 0; i < length; i++)
			{
				if (src[i] != 0)
				{
					allZero = false;
					break;
				}
			}
			if (!allZero)
			{
				m_buffer[0] = kNonZerosRead;
				std::memcpy(m_buffer.data() + 1, src, length);
				if (!SendExact(s, m_buffer.data(), length + 1))
					return false;
			}
		}
		if (allZero)
		{
			if (!SendByteValue(s, kOnlyZerosRead))
				return false;
		}
		start += length;
	}
	return true;
}

bool TcpGeckoServer::CmdValidateAddressRange(SOCKET s)
{
	uint8_t buffer[8];
	if (!RecvExact(s, buffer, 8))
		return false;
	uint32_t start = ReadBE32(buffer);
	uint32_t end = ReadBE32(buffer + 4);
	bool valid = end > start && memory_isAddressRangeAccessible(start, end - start);
	return SendByteValue(s, valid ? 1 : 0);
}

bool TcpGeckoServer::CmdDisassemble(SOCKET s)
{
	uint8_t buffer[12];
	if (!RecvExact(s, buffer, 12))
		return false;
	return SendU32(s, 0);
}

bool TcpGeckoServer::CmdUploadMemory(SOCKET s)
{
	uint8_t buffer[8];
	if (!RecvExact(s, buffer, 8))
		return false;
	uint32_t start = ReadBE32(buffer);
	uint32_t end = ReadBE32(buffer + 4);
	uint32_t current = start;
	while (current != end)
	{
		if (end <= current)
			break;
		uint32_t length = std::min(end - current, kDataBufferSize);
		if (!RecvExact(s, m_buffer.data(), length))
			return false;
		if (memory_isAddressRangeAccessible(current, length))
			std::memcpy(memory_getPointerFromVirtualOffset(current), m_buffer.data(), length);
		current += length;
	}
	if (end > start)
		PPCRecompiler_invalidateRange(start, end);
	TcpGecko::CodeHandler::NotifyMemoryUploaded(start, end);
	return true;
}

bool TcpGeckoServer::CmdServerStatus(SOCKET s)
{
	return SendByteValue(s, 1);
}

bool TcpGeckoServer::CmdGetDataBufferSize(SOCKET s)
{
	return SendU32(s, kDataBufferSize);
}

bool TcpGeckoServer::CmdReadFile(SOCKET s)
{
	std::vector<uint8_t> pathBytes;
	if (!RecvLengthPrefixedString(s, pathBytes, 1024))
		return false;
	std::string path(pathBytes.begin(), pathBytes.end());
	if (auto nul = path.find('\0'); nul != std::string::npos)
		path.resize(nul);

	sint32 status = FSC_STATUS_UNDEFINED;
	FSCVirtualFile* file = fsc_open(path.c_str(), FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status);
	if (!file)
		return SendU32(s, (uint32_t)(int32_t)(status == FSC_STATUS_OK ? -1 : status));

	if (!SendU32(s, 0))
	{
		fsc_close(file);
		return false;
	}
	uint32_t totalBytes = fsc_getFileSize(file);
	if (!SendU32(s, totalBytes))
	{
		fsc_close(file);
		return false;
	}

	uint32_t totalRead = 0;
	while (totalRead < totalBytes)
	{
		uint32_t chunk = std::min(totalBytes - totalRead, kDataBufferSize);
		uint32_t bytesRead = fsc_readFile(file, m_buffer.data(), chunk);
		if (bytesRead == 0)
			break;
		if (!SendExact(s, m_buffer.data(), bytesRead))
		{
			fsc_close(file);
			return false;
		}
		totalRead += bytesRead;
	}
	fsc_close(file);
	return true;
}

bool TcpGeckoServer::CmdReadDirectory(SOCKET s)
{
	std::vector<uint8_t> pathBytes;
	if (!RecvLengthPrefixedString(s, pathBytes, 1024))
		return false;
	std::string path(pathBytes.begin(), pathBytes.end());
	if (auto nul = path.find('\0'); nul != std::string::npos)
		path.resize(nul);

	sint32 status = FSC_STATUS_UNDEFINED;
	FSCVirtualFile* dir = fsc_openDirIterator(path.c_str(), &status);
	if (!dir)
		return SendU32(s, (uint32_t)(int32_t)(status == FSC_STATUS_OK ? -1 : status));

	if (!SendU32(s, 0))
	{
		fsc_close(dir);
		return false;
	}
	FSCDirEntry entry{};
	while (fsc_nextDir(dir, &entry))
	{
		char nameBuffer[256] = {};
		strncpy(nameBuffer, entry.path, sizeof(nameBuffer) - 1);
		if (!SendExact(s, nameBuffer, sizeof(nameBuffer)))
		{
			fsc_close(dir);
			return false;
		}
		if (!SendU32(s, entry.isDirectory ? 1u : 0u))
		{
			fsc_close(dir);
			return false;
		}
		if (!SendU32(s, entry.fileSize))
		{
			fsc_close(dir);
			return false;
		}
	}
	fsc_close(dir);
	char terminator[256] = {};
	if (!SendExact(s, terminator, sizeof(terminator)))
		return false;
	if (!SendU32(s, 0))
		return false;
	if (!SendU32(s, 0))
		return false;
	return true;
}

bool TcpGeckoServer::CmdReplaceFile(SOCKET s)
{
	std::vector<uint8_t> pathBytes;
	if (!RecvLengthPrefixedString(s, pathBytes, 1024))
		return false;
	std::string path(pathBytes.begin(), pathBytes.end());
	if (auto nul = path.find('\0'); nul != std::string::npos)
		path.resize(nul);

	sint32 status = FSC_STATUS_UNDEFINED;
	FSCVirtualFile* file = fsc_open(path.c_str(),
									FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::WRITE_PERMISSION | FSC_ACCESS_FLAG::FILE_ALLOW_CREATE | FSC_ACCESS_FLAG::FILE_ALWAYS_CREATE,
									&status);
	if (!file)
		return SendU32(s, (uint32_t)(int32_t)(status == FSC_STATUS_OK ? -1 : status));

	if (!SendU32(s, 0))
	{
		fsc_close(file);
		return false;
	}
	if (!SendU32(s, kDataBufferSize))
	{
		fsc_close(file);
		return false;
	}

	while (true)
	{
		uint8_t lenBuf[4];
		if (!RecvExact(s, lenBuf, 4))
		{
			fsc_close(file);
			return false;
		}
		uint32_t dataLength = ReadBE32(lenBuf);
		if (dataLength == 0)
			break;
		if (dataLength > kDataBufferSize)
		{
			fsc_close(file);
			return false;
		}
		if (!RecvExact(s, m_buffer.data(), dataLength))
		{
			fsc_close(file);
			return false;
		}
		fsc_writeFile(file, m_buffer.data(), dataLength);
	}
	fsc_close(file);
	return true;
}

bool TcpGeckoServer::CmdGetCodeHandlerAddress(SOCKET s)
{
	return SendU32(s, TcpGecko::CodeHandler::kCodeHandlerInstallAddress);
}

bool TcpGeckoServer::CmdReadThreads(SOCKET s)
{
	__OSLockScheduler();
	std::vector<MPTR> threads(activeThread, activeThread + activeThreadCount);
	__OSUnlockScheduler();

	if (!SendU32(s, (uint32_t)threads.size()))
		return false;
	for (MPTR threadId : threads)
	{
		OSThread_t* thread = ThreadById(threadId);
		if (!SendU32(s, (uint32_t)threadId))
			return false;
		const auto& ctx = thread->context;
		bool ok = true;
		for (int i = 0; ok && i < 32; i++)
			ok = SendU32(s, ctx.gpr[i]);
		ok = ok && SendU32(s, ctx.cr) && SendU32(s, ctx.lr) && SendU32(s, ctx.ctr) && SendU32(s, ctx.xer);
		if (!ok)
			return false;
	}
	return true;
}

bool TcpGeckoServer::CmdAccountIdentifier(SOCKET s)
{
	return SendU32(s, ActiveSettings::GetPersistentId());
}

bool TcpGeckoServer::CmdFollowPointer(SOCKET s)
{
	uint8_t header[8];
	if (!RecvExact(s, header, 8))
		return false;
	uint32_t destinationAddress = ReadBE32(header);
	uint32_t offsetsCount = ReadBE32(header + 4);

	std::vector<int32_t> offsets(offsetsCount);
	if (offsetsCount > 0)
	{
		std::vector<uint8_t> offsetBytes(offsetsCount * 4);
		if (!RecvExact(s, offsetBytes.data(), offsetBytes.size()))
			return false;
		for (uint32_t i = 0; i < offsetsCount; i++)
			offsets[i] = (int32_t)ReadBE32(&offsetBytes[i * 4]);
	}

	constexpr uint32_t kInvalidAddress = 0xFFFFFFFFu;
	bool baseValid = memory_isAddressRangeAccessible(destinationAddress, 4);
	if (baseValid)
	{
		for (uint32_t i = 0; i < offsetsCount; i++)
		{
			uint32_t pointerValue = memory_readU32(destinationAddress);
			destinationAddress = (uint32_t)((int32_t)pointerValue + offsets[i]);
			if (!memory_isAddressRangeAccessible(destinationAddress, 4))
			{
				destinationAddress = kInvalidAddress;
				break;
			}
		}
	}
	else if (offsetsCount > 0)
	{
		destinationAddress = kInvalidAddress;
	}
	return SendU32(s, destinationAddress);
}

bool TcpGeckoServer::CmdRemoteProcedureCall(SOCKET s)
{
	uint8_t buffer[4 + 8 * 4];
	if (!RecvExact(s, buffer, sizeof(buffer)))
		return false;
	uint32_t function = ReadBE32(buffer);
	std::array<uint32_t, 8> args{};
	for (int i = 0; i < 8; i++)
		args[i] = ReadBE32(buffer + 4 + i * 4);

	uint64_t result = 0;
	TcpGecko::GuestJobs::CallRemoteProcedure(function, args, result);

	uint8_t response[8];
	WriteBE32(response, (uint32_t)(result >> 32));
	WriteBE32(response + 4, (uint32_t)result);
	return SendExact(s, response, 8);
}

bool TcpGeckoServer::CmdGetSymbol(SOCKET s)
{
	uint8_t sizeByte = 0;
	if (!RecvExact(s, &sizeByte, 1))
		return false;
	std::vector<uint8_t> buffer(sizeByte);
	if (sizeByte > 0 && !RecvExact(s, buffer.data(), sizeByte))
		return false;
	uint8_t exportTypeByte = 0;
	if (!RecvExact(s, &exportTypeByte, 1))
		return false;

	uint32_t address = 0;
	if (buffer.size() >= 8)
	{
		uint32_t symbolNameOffset = ReadBE32(buffer.data() + 4);
		const char* rplName = (const char*)buffer.data() + 8;
		size_t rplNameMaxLen = buffer.size() - 8;
		if (symbolNameOffset < buffer.size() && strnlen(rplName, rplNameMaxLen) < rplNameMaxLen)
		{
			const char* symbolName = (const char*)buffer.data() + symbolNameOffset;
			size_t symbolNameMaxLen = buffer.size() - symbolNameOffset;
			if (strnlen(symbolName, symbolNameMaxLen) < symbolNameMaxLen)
			{
				RPLResolvedExport result;
				bool isData = exportTypeByte == 1;
				if (RPLLoader_FindLoadedExport(rplName, symbolName, isData, result))
					address = result.address;
			}
		}
	}
	return SendU32(s, address);
}

bool TcpGeckoServer::CmdMemorySearch32(SOCKET s)
{
	uint8_t buffer[12];
	if (!RecvExact(s, buffer, 12))
		return false;
	uint32_t address = ReadBE32(buffer);
	uint32_t value = ReadBE32(buffer + 4);
	uint32_t length = ReadBE32(buffer + 8);

	uint32_t found = 0;
	for (uint32_t index = address; index < address + length; index += 4)
	{
		if (!memory_isAddressRangeAccessible(index, 4))
			continue;
		if (memory_readU32(index) == value)
		{
			found = index;
			break;
		}
	}
	return SendU32(s, found);
}

bool TcpGeckoServer::CmdAdvancedMemorySearch(SOCKET s)
{
	uint8_t header[24];
	if (!RecvExact(s, header, 24))
		return false;
	uint32_t startingAddress = ReadBE32(header);
	uint32_t length = ReadBE32(header + 4);
	uint32_t resultsLimit = ReadBE32(header + 12);
	uint32_t aligned = ReadBE32(header + 16);
	uint32_t searchBytesCount = ReadBE32(header + 20);

	if (searchBytesCount == 0 || searchBytesCount > kDataBufferSize)
		return false;
	std::vector<uint8_t> pattern(searchBytesCount);
	if (!RecvExact(s, pattern.data(), searchBytesCount))
		return false;

	uint32_t step = aligned ? searchBytesCount : 1;
	std::vector<uint32_t> found;
	const uint32_t capacity = (kDataBufferSize / 4) - 1;
	for (uint32_t current = startingAddress; current < startingAddress + length; current += step)
	{
		if (!memory_isAddressRangeAccessible(current, searchBytesCount))
			continue;
		if (std::memcmp(memory_getPointerFromVirtualOffset(current), pattern.data(), searchBytesCount) == 0)
		{
			found.push_back(current);
			if ((resultsLimit != 0 && found.size() >= resultsLimit) || found.size() >= capacity)
				break;
		}
	}

	if (!SendU32(s, (uint32_t)(found.size() * 4)))
		return false;
	for (uint32_t addr : found)
	{
		if (!SendU32(s, addr))
			return false;
	}
	return true;
}

bool TcpGeckoServer::CmdExecuteAssembly(SOCKET s)
{
	std::vector<uint8_t> blob;
	if (!RecvLengthPrefixedString(s, blob, kDataBufferSize))
		return false;
	TcpGecko::GuestJobs::ExecuteAssemblyBlob(blob);
	return true;
}

bool TcpGeckoServer::CmdPauseConsole(SOCKET s)
{
	TcpGecko::SetGuestPaused(TcpGecko::PauseOwner::RemoteDebugger, true);
	return true;
}

bool TcpGeckoServer::CmdResumeConsole(SOCKET s)
{
	TcpGecko::SetGuestPaused(TcpGecko::PauseOwner::RemoteDebugger, false);
	return true;
}

bool TcpGeckoServer::CmdIsConsolePaused(SOCKET s)
{
	return SendByteValue(s, TcpGecko::IsGuestPaused() ? 1 : 0);
}

bool TcpGeckoServer::CmdServerVersion(SOCKET s)
{
	static const std::string version = fmt::format("CemuExtend built-in TCPGecko (protocol-compatible reimplementation)");
	if (!SendU32(s, (uint32_t)version.size()))
		return false;
	return SendExact(s, version.data(), version.size());
}

bool TcpGeckoServer::CmdGetOsVersion(SOCKET s)
{
	return SendU32(s, (uint32_t)CafeSystem::GetForegroundTitleSDKVersion());
}

bool TcpGeckoServer::CmdSetDataBreakpoint(SOCKET s)
{
	uint8_t buffer[6];
	if (!RecvExact(s, buffer, 6))
		return false;
	return true;
}

bool TcpGeckoServer::CmdSetInstructionBreakpoint(SOCKET s)
{
	uint8_t buffer[4];
	if (!RecvExact(s, buffer, 4))
		return false;
	std::lock_guard lock(s_breakpointMutex);
	s_breakpointAddresses.insert(ReadBE32(buffer));
	return true;
}

bool TcpGeckoServer::CmdToggleBreakpoint(SOCKET s)
{
	uint8_t buffer[4];
	if (!RecvExact(s, buffer, 4))
		return false;
	uint32_t address = ReadBE32(buffer);
	std::lock_guard lock(s_breakpointMutex);
	auto it = s_breakpointAddresses.find(address);
	if (it != s_breakpointAddresses.end())
		s_breakpointAddresses.erase(it);
	else
		s_breakpointAddresses.insert(address);
	return true;
}

bool TcpGeckoServer::CmdRemoveAllBreakpoints(SOCKET s)
{
	std::lock_guard lock(s_breakpointMutex);
	s_breakpointAddresses.clear();
	return true;
}

bool TcpGeckoServer::CmdGetStackTrace(SOCKET s)
{
	std::vector<uint32_t> frames;
	if (OSThread_t* thread = FirstActiveThread())
	{
		uint32_t sp = thread->context.gpr[1];
		for (int i = 0; i < 128 && sp != 0; i++)
		{
			if (!memory_isAddressRangeAccessible(sp, 8))
				break;
			uint32_t backChain = memory_readU32(sp);
			uint32_t lr = memory_readU32(sp + 4);
			frames.push_back(lr);
			if (backChain <= sp)
				break;
			sp = backChain;
		}
	}
	if (!SendU32(s, (uint32_t)frames.size()))
		return false;
	for (uint32_t frame : frames)
	{
		if (!SendU32(s, frame))
			return false;
	}
	return true;
}

bool TcpGeckoServer::CmdPokeRegisters(SOCKET s)
{
	constexpr int gprSize = 4 * 32;
	constexpr int fprSize = 8 * 32;
	std::vector<uint8_t> buffer(gprSize + fprSize);
	if (!RecvExact(s, buffer.data(), buffer.size()))
		return false;
	if (OSThread_t* thread = FirstActiveThread())
	{
		for (int i = 0; i < 32; i++)
			thread->context.gpr[i] = ReadBE32(buffer.data() + i * 4);
		for (int i = 0; i < 32; i++)
		{
			uint64_t hi = ReadBE32(buffer.data() + gprSize + i * 8);
			uint64_t lo = ReadBE32(buffer.data() + gprSize + i * 8 + 4);
			thread->context.fp_ps0[i] = (hi << 32) | lo;
		}
	}
	return true;
}

bool TcpGeckoServer::CmdGetEntryPointAddress(SOCKET s)
{
	uint32_t entry = applicationRPX ? RPLLoader_GetModuleEntrypoint(applicationRPX) : 0;
	return SendU32(s, entry);
}

bool TcpGeckoServer::CmdPersistAssembly(SOCKET s)
{
	std::vector<uint8_t> blob;
	return RecvLengthPrefixedString(s, blob, kDataBufferSize);
}

bool TcpGeckoServer::CmdClearAssembly(SOCKET s)
{
	return true;
}

bool TcpGeckoServer::CmdTakeScreenShot(SOCKET s)
{
	struct ScreenshotResult
	{
		std::mutex mutex;
		std::condition_variable cv;
		bool ready = false;
		std::vector<uint8> data;
		int width = 0, height = 0;
	};
	auto result = std::make_shared<ScreenshotResult>();

	auto requestId = g_renderer ? g_renderer->RequestScreenshot(
									  [result](const std::vector<uint8>& rgb, int width, int height, bool mainWindow) -> std::optional<std::string> {
										  std::lock_guard lock(result->mutex);
										  result->data = rgb;
										  result->width = width;
										  result->height = height;
										  result->ready = true;
										  result->cv.notify_all();
										  return std::nullopt;
									  })
								: std::nullopt;

	if (!requestId.has_value())
		return SendU32(s, 0);

	std::unique_lock lock(result->mutex);
	if (!result->cv.wait_for(lock, std::chrono::seconds(2), [&] { return result->ready; }))
		return SendU32(s, 0);

	uint32_t totalSize = (uint32_t)result->data.size();
	if (!SendU32(s, totalSize))
		return false;
	uint32_t sent = 0;
	while (sent < totalSize)
	{
		uint32_t chunk = std::min(totalSize - sent, kDataBufferSize);
		if (!SendU32(s, chunk))
			return false;
		if (!SendExact(s, result->data.data() + sent, chunk))
			return false;
		sent += chunk;
	}
	return true;
}

namespace TcpGecko
{
	void SetGuestPaused(PauseOwner owner, bool paused, std::uint64_t sequence)
	{
		std::scoped_lock lock(s_pauseMutex);
		if (!s_pauseOwners.Set(owner, paused, sequence))
			return;
		const bool shouldPause = s_pauseOwners.IsPaused();
		if (!coreinit::OSIsSchedulerActive())
		{
			s_guestPauseApplied = false;
			return;
		}
		if (shouldPause)
		{
			SuspendAllGuestThreads();
			s_guestPauseApplied = true;
		}
		else if (s_guestPauseApplied)
		{
			ResumeAllGuestThreads();
			s_guestPauseApplied = false;
		}
	}

	bool IsGuestPaused()
	{
		std::scoped_lock lock(s_pauseMutex);
		return s_pauseOwners.IsPaused();
	}

	void OnTitleBoot()
	{
		ResetGuestPause();
		if (!GetConfig().tcpgecko.enabled.GetValue())
			return;
		if (!g_tcpGeckoServer)
			g_tcpGeckoServer = std::make_unique<TcpGeckoServer>(GetConfig().tcpgecko.port.GetValue(), GetConfig().tcpgecko.allow_lan.GetValue());
		g_tcpGeckoServer->Initialize();
		CodeHandler::Install();
	}

	void OnTitleShutdown()
	{
		ResetGuestPause();
		CodeHandler::Uninstall();
	}

	void Tick()
	{
		CodeHandler::Tick();
		GuestJobs::Tick();
	}
} // namespace TcpGecko
