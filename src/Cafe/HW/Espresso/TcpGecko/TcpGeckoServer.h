#pragma once

#include "Common/socket.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class TcpGeckoServer
{
  public:
	explicit TcpGeckoServer(uint16_t port, bool allowLan);
	~TcpGeckoServer();

	bool Initialize();

	bool IsConnected() const
	{
		return m_clientConnected.load();
	}

  private:
	void ThreadFunc();
	void ProcessClient(SOCKET clientSocket, const std::string& clientIp);

	bool RecvExact(SOCKET s, void* buffer, size_t size);
	bool SendExact(SOCKET s, const void* buffer, size_t size);
	bool SendByteValue(SOCKET s, uint8_t value);
	bool SendU32(SOCKET s, uint32_t value);
	bool RecvLengthPrefixedString(SOCKET s, std::vector<uint8_t>& out, uint32_t maxSize);

	bool CmdWrite(SOCKET s, int size);
	bool CmdReadMemory(SOCKET s);
	bool CmdValidateAddressRange(SOCKET s);
	bool CmdDisassemble(SOCKET s);
	bool CmdUploadMemory(SOCKET s);
	bool CmdServerStatus(SOCKET s);
	bool CmdGetDataBufferSize(SOCKET s);
	bool CmdReadFile(SOCKET s);
	bool CmdReadDirectory(SOCKET s);
	bool CmdReplaceFile(SOCKET s);
	bool CmdGetCodeHandlerAddress(SOCKET s);
	bool CmdReadThreads(SOCKET s);
	bool CmdAccountIdentifier(SOCKET s);
	bool CmdFollowPointer(SOCKET s);
	bool CmdRemoteProcedureCall(SOCKET s);
	bool CmdGetSymbol(SOCKET s);
	bool CmdMemorySearch32(SOCKET s);
	bool CmdAdvancedMemorySearch(SOCKET s);
	bool CmdExecuteAssembly(SOCKET s);
	bool CmdPauseConsole(SOCKET s);
	bool CmdResumeConsole(SOCKET s);
	bool CmdIsConsolePaused(SOCKET s);
	bool CmdServerVersion(SOCKET s);
	bool CmdGetOsVersion(SOCKET s);
	bool CmdSetDataBreakpoint(SOCKET s);
	bool CmdSetInstructionBreakpoint(SOCKET s);
	bool CmdToggleBreakpoint(SOCKET s);
	bool CmdRemoveAllBreakpoints(SOCKET s);
	bool CmdGetStackTrace(SOCKET s);
	bool CmdPokeRegisters(SOCKET s);
	bool CmdGetEntryPointAddress(SOCKET s);
	bool CmdGetVersionHash(SOCKET s);
	bool CmdPersistAssembly(SOCKET s);
	bool CmdClearAssembly(SOCKET s);
	bool CmdTakeScreenShot(SOCKET s);

	const uint16_t m_port;
	const bool m_allowLan;

	std::thread m_thread;
	std::atomic_bool m_stopRequested{false};
	std::atomic_bool m_initialized{false};
	std::atomic_bool m_clientConnected{false};

	SOCKET m_serverSocket = INVALID_SOCKET;
	SOCKET m_clientSocket = INVALID_SOCKET;

	std::vector<uint8_t> m_buffer;
};

extern std::unique_ptr<TcpGeckoServer> g_tcpGeckoServer;

namespace TcpGecko
{
	void OnTitleBoot();
	void OnTitleShutdown();

	void Tick();
} // namespace TcpGecko
