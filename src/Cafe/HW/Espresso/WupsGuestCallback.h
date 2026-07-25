#pragma once

#include <cstdint>
#include <span>
#include <string>

struct RPLModule;

// Executes a mapped guest PPC function through Cemu's normal callback path.
// targetVirtualAddress is the address from the WPS image, not a host pointer.
class WupsGuestCallback
{
public:
	// aggregateByReference marshals argumentWords as one struct in guest memory and
	// passes its address in r3, which is how devkitPPC hands a WUPS hook its
	// wups_loader_*_args_t. Plain guest callbacks take scalar arguments and must
	// leave it false.
	[[nodiscard]] static bool Invoke(RPLModule* module, std::uint64_t lifetimeId,
		std::uint32_t targetVirtualAddress, std::span<const std::uint32_t> argumentWords,
		std::uint32_t& result, std::string& error,
		bool aggregateByReference = false);
};
