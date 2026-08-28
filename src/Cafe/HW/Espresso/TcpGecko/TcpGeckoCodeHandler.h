#pragma once

#include "config/CemuConfig.h"

#include <cstdint>

namespace TcpGecko::CodeHandler
{
	constexpr uint32_t kCodeHandlerInstallAddress = 0x010F4000;
	constexpr uint32_t kCodeListStartAddress = 0x01133000;
	constexpr uint32_t kCodeListEndAddress = 0x0113D600;
	constexpr uint32_t kCodeListSize = kCodeListEndAddress - kCodeListStartAddress;

	void Install();
	void Uninstall();

	bool IsInstalled();
	TcpGeckoHandlerVersion InstalledVersion();

	void SetEnabled(bool enabled);

	void Tick();

	void NotifyMemoryUploaded(uint32_t destinationAddress, uint32_t length);
} // namespace TcpGecko::CodeHandler
