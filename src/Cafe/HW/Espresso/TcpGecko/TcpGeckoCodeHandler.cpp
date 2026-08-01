#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/TcpGecko/TcpGeckoCodeHandler.h"
#include "Cafe/HW/Espresso/TcpGecko/TcpGeckoCodeHandlerBinaries.h"

#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/CemuConfig.h"

#include <atomic>
#include <cstring>

namespace TcpGecko::CodeHandler
{
	namespace
	{
		std::atomic_bool s_installed{false};
		std::atomic<TcpGeckoHandlerVersion> s_installedVersion{TcpGeckoHandlerVersion::Latest};
		std::atomic_bool s_hasReceivedUpload{false};

		uint32_t EnabledFlagAddress(TcpGeckoHandlerVersion version)
		{
			return version == TcpGeckoHandlerVersion::General ? 0x10014CFCu : 0x10014EFCu;
		}

		void WriteCodeBytes(uint32_t address, const unsigned char* data, uint32_t size)
		{
			uint8* ptr = memory_getPointerFromVirtualOffset(address);
			std::memcpy(ptr, data, size);
			PPCRecompiler_invalidateRange(address, address + size);
		}
	}

	void Install()
	{
		const auto version = GetConfig().tcpgecko.handler_version.GetValue();
		const unsigned char* handlerData = version == TcpGeckoHandlerVersion::General ? generalCodeHandler : latestCodeHandler;
		const uint32_t handlerSize = version == TcpGeckoHandlerVersion::General ? generalCodeHandlerLength : latestCodeHandlerLength;

		WriteCodeBytes(kCodeHandlerInstallAddress, handlerData, handlerSize);

		uint8* codeList = memory_getPointerFromVirtualOffset(kCodeListStartAddress);
		std::memset(codeList, 0, kCodeListSize);
		static constexpr uint8_t kTerminatorCodeLine[8] = {0xD0, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xCA, 0xFE};
		std::memcpy(codeList, kTerminatorCodeLine, sizeof(kTerminatorCodeLine));
		PPCRecompiler_invalidateRange(kCodeListStartAddress, kCodeListStartAddress + 8);

		s_installedVersion.store(version);
		s_hasReceivedUpload.store(false);
		s_installed.store(true);
		SetEnabled(true);

		cemuLog_log(LogType::Force, "TCPGecko: installed {} code handler at {:#010x}",
			version == TcpGeckoHandlerVersion::General ? "general" : "latest", kCodeHandlerInstallAddress);
	}

	void Uninstall()
	{
		s_installed.store(false);
	}

	bool IsInstalled()
	{
		return s_installed.load();
	}

	TcpGeckoHandlerVersion InstalledVersion()
	{
		return s_installedVersion.load();
	}

	void SetEnabled(bool enabled)
	{
		if (!s_installed.load())
			return;
		memory_writeU32(EnabledFlagAddress(s_installedVersion.load()), enabled ? 1u : 0u);
	}

	void Tick()
	{
		if (!s_installed.load() || !s_hasReceivedUpload.load())
			return;
		PPCCoreCallback((MPTR)kCodeHandlerInstallAddress);
	}

	void NotifyMemoryUploaded(uint32_t destinationAddress, uint32_t endAddress)
	{
		if (destinationAddress >= kCodeListStartAddress && destinationAddress < kCodeListEndAddress)
			s_hasReceivedUpload.store(true);

		if (destinationAddress != kCodeListStartAddress || endAddress == kCodeListEndAddress)
			return;
		const uint32_t lineCount = (endAddress - destinationAddress) / 8;
		LatteOverlay_pushNotification(fmt::format("TCPGecko: {} lines of code applied!", lineCount), 3000);
	}
}
