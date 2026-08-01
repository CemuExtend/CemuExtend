// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from TCPGecko-Plugin (https://github.com/wiiu-env/TCPGecko-Plugin,
// GPLv3, Copyright (C) Maschell, BullyWiiPlaza, Katope and contributors).
// See src/Cafe/HW/Espresso/TcpGecko/LICENSE for the full license text.
#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/TcpGecko/TcpGeckoGuestJobs.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/MMU/MMU.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace TcpGecko::GuestJobs
{
	namespace
	{
		uint32_t ScratchAddressFor(uint32_t size)
		{
			return 0x10000000u - size;
		}

		constexpr auto kJobTimeout = std::chrono::seconds(2);

		enum class JobKind { None, Rpc, ExecuteOnce };

		std::mutex s_mutex;
		std::condition_variable s_cv;
		JobKind s_pendingKind = JobKind::None;
		bool s_jobDone = false;

		uint32_t s_rpcFunction = 0;
		std::array<uint32_t, 8> s_rpcArgs{};
		uint64_t s_rpcResult = 0;
		std::vector<uint8_t> s_executeOnceBlob;

		void WriteScratch(const std::vector<uint8_t>& code, uint32_t address)
		{
			uint8* ptr = memory_getPointerFromVirtualOffset(address);
			std::memcpy(ptr, code.data(), code.size());
			PPCRecompiler_invalidateRange(address, address + (uint32_t)code.size());
		}

		void ClearScratch(uint32_t address, uint32_t size)
		{
			if (size == 0)
				return;
			uint8* ptr = memory_getPointerFromVirtualOffset(address);
			std::memset(ptr, 0, size);
			PPCRecompiler_invalidateRange(address, address + size);
		}

		void RunRpcJob()
		{
			PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
			const uint32_t r3 = PPCCoreCallback((MPTR)s_rpcFunction,
				s_rpcArgs[0], s_rpcArgs[1], s_rpcArgs[2], s_rpcArgs[3],
				s_rpcArgs[4], s_rpcArgs[5], s_rpcArgs[6], s_rpcArgs[7]);
			const uint32_t r4 = hCPU ? hCPU->gpr[4] : 0;
			s_rpcResult = (static_cast<uint64_t>(r3) << 32) | r4;
		}

		void RunExecuteOnceJob()
		{
			const uint32_t address = ScratchAddressFor((uint32_t)s_executeOnceBlob.size());
			WriteScratch(s_executeOnceBlob, address);
			PPCCoreCallback((MPTR)address);
			ClearScratch(address, (uint32_t)s_executeOnceBlob.size());
		}
	}

	bool CallRemoteProcedure(uint32_t function, const std::array<uint32_t, 8>& args, uint64_t& result)
	{
		if (!CafeSystem::IsTitleRunning())
			return false;
		std::unique_lock lock(s_mutex);
		s_cv.wait(lock, [] { return s_pendingKind == JobKind::None; });
		s_pendingKind = JobKind::Rpc;
		s_jobDone = false;
		s_rpcFunction = function;
		s_rpcArgs = args;
		s_cv.notify_all();
		const bool completed = s_cv.wait_for(lock, kJobTimeout, [] { return s_jobDone; });
		if (!completed)
		{
			s_pendingKind = JobKind::None;
			return false;
		}
		result = s_rpcResult;
		return true;
	}

	bool ExecuteAssemblyBlob(const std::vector<uint8_t>& code)
	{
		if (!CafeSystem::IsTitleRunning())
			return false;
		std::unique_lock lock(s_mutex);
		s_cv.wait(lock, [] { return s_pendingKind == JobKind::None; });
		s_pendingKind = JobKind::ExecuteOnce;
		s_jobDone = false;
		s_executeOnceBlob = code;
		s_cv.notify_all();
		const bool completed = s_cv.wait_for(lock, kJobTimeout, [] { return s_jobDone; });
		if (!completed)
		{
			s_pendingKind = JobKind::None;
			return false;
		}
		return true;
	}

	void Tick()
	{
		std::unique_lock lock(s_mutex);
		if (s_pendingKind == JobKind::None)
			return;
		const JobKind kind = s_pendingKind;
		if (kind == JobKind::Rpc)
			RunRpcJob();
		else if (kind == JobKind::ExecuteOnce)
			RunExecuteOnceJob();
		s_pendingKind = JobKind::None;
		s_jobDone = true;
		s_cv.notify_all();
	}
}
