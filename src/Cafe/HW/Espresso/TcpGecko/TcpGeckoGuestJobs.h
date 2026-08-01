#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace TcpGecko::GuestJobs
{
	// Calls `function(args[0..7])`, returns its 64-bit result. Blocks until the next tick or timeout.
	bool CallRemoteProcedure(uint32_t function, const std::array<uint32_t, 8>& args, uint64_t& result);

	// Writes `code` to the scratch address, executes once, clears it. Blocks until executed or timeout.
	bool ExecuteAssemblyBlob(const std::vector<uint8_t>& code);

	// Called once per emulated frame from a live PPC core thread. Drains one pending job.
	void Tick();
}
