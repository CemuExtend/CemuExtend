#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsServices.h"
#include "Cafe/HW/Espresso/ModExecutionContext.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cemu/Logging/CemuLogging.h"

#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>

namespace
{
	std::array<uint8, 0x40000> s_memory{};
	PPCInterpreter_t* s_cpu{};
	HLECALL s_hle{};
	std::uint32_t s_nextCodeCave{0x10000};
	std::mutex s_fakeMutex;
	std::set<std::uint32_t> s_allocations;
	OSThread_t s_thread{};
}

uint64 s_loggingFlagMask = 1ULL << static_cast<uint64>(LogType::Force);
bool cemuLog_log(LogType, std::string_view) { return true; }
bool cemuLog_log(LogType, std::u8string_view) { return true; }

PPCInterpreter_t* PPCInterpreter_getCurrentInstance() { return s_cpu; }
namespace coreinit { OSThread_t* OSGetCurrentThread() { return &s_thread; } }
HLEIDX PPCInterpreter_registerHLECall(HLECALL callback, std::string)
{
	s_hle = callback;
	return 7;
}
bool memory_isAddressRangeAccessible(MPTR address, uint32 size)
{
	return size != 0 && address < s_memory.size() &&
		size <= s_memory.size() - address;
}
uint8* memory_getPointerFromVirtualOffset(uint32 address)
{
	return memory_isAddressRangeAccessible(address, 1) ?
		s_memory.data() + address : nullptr;
}
uint32 memory_getVirtualOffsetFromPointer(void* pointer)
{
	return pointer == &s_thread ? 0x5000 : 0;
}
uint32 memory_readU32(uint32 address)
{
	return (static_cast<uint32>(s_memory[address]) << 24) |
		(static_cast<uint32>(s_memory[address + 1]) << 16) |
		(static_cast<uint32>(s_memory[address + 2]) << 8) |
		static_cast<uint32>(s_memory[address + 3]);
}
void memory_writeU32(uint32 address, uint32 value)
{
	s_memory[address] = static_cast<uint8>(value >> 24);
	s_memory[address + 1] = static_cast<uint8>(value >> 16);
	s_memory[address + 2] = static_cast<uint8>(value >> 8);
	s_memory[address + 3] = static_cast<uint8>(value);
}
bool RPLLoader_QueryMappedAddress(uint32, uint32, RPLMappedAddressInfo& info)
{
	info = {};
	return false;
}
MEMPTR<void> RPLLoader_AllocateCodeCaveMem(uint32, uint32 size)
{
	std::lock_guard lock(s_fakeMutex);
	const auto result = s_nextCodeCave;
	s_nextCodeCave += (size + 0xffU) & ~0xffU;
	if (s_nextCodeCave >= s_memory.size()) return {};
	s_allocations.insert(result);
	return MEMPTR<void>{result};
}
void RPLLoader_ReleaseCodeCaveMem(MEMPTR<void> address)
{
	std::lock_guard lock(s_fakeMutex);
	s_allocations.erase(address.GetMPTR());
}
void PPCRecompiler_invalidateRange(uint32, uint32) {}
void osLib_returnFromFunction(PPCInterpreter_t* cpu, uint32 result)
{
	cpu->gpr[3] = result;
}

namespace
{
	[[noreturn]] void Failed(const char* expression, int line)
	{
		std::cerr << "CHECK failed at line " << line << ": "
			<< expression << '\n';
		std::abort();
	}
#define CHECK(value) do { if (!(value)) Failed(#value, __LINE__); } while (false)
}

int main()
{
	auto platform = CreateCemuWupsPlatform();
	CHECK(platform);
	CHECK(!platform->SupportsMappedMemory());
	CHECK(!platform->SupportsOwnerScopedHeapPointers());
	CHECK(!platform->ValidateGuestRange(
		0xfffffff0U, 0x20, WupsGuestAccess::Read));

	bool executed{};
	std::string error;
	CHECK(!platform->QueueCpuTask({71, 2}, [&] { executed = true; }, error));
	CHECK(!executed);
	CHECK(error.find("cross-thread") != std::string::npos);
	error.clear();
	CHECK(!platform->QueueCpuTask({71, 1}, [&] { executed = true; }, error));
	CHECK(!executed);
	CHECK(error.find("stale") != std::string::npos);

	error.clear();
	CHECK(!platform->AllocateGuestData({72, 1}, 16, 4, error));
	CHECK(error.find("CPU thread") != std::string::npos);

	PPCInterpreter_t cpu{};
	s_cpu = &cpu;
	s_thread.stackEnd = MEMPTR<void>{0x2000};
	s_thread.stackBase = MEMPTR<void>{0x3000};
	cpu.gpr[1] = 0x2800;
	for (std::uint32_t index = 0; index < 8; ++index)
		cpu.gpr[3 + index] = 0x10 + index;
	for (std::uint32_t index = 0; index < 24; ++index)
		memory_writeU32(cpu.gpr[1] + 8 + index * 4, 0x80 + index);

	const auto data = platform->AllocateGuestData({72, 1}, 16, 16, error);
	CHECK(data);
	CHECK(platform->ValidateGuestRange(*data, 16, WupsGuestAccess::Read));
	CHECK(platform->ValidateGuestRange(*data, 16, WupsGuestAccess::Write));
	CHECK(!platform->ValidateGuestRange(*data, 4, WupsGuestAccess::Execute));

	std::size_t calls{};
	const auto callable = platform->RegisterFunction({72, 1},
		"homebrew_test", "Export",
		[&](std::span<const std::uint32_t> arguments, std::string&) {
			++calls;
			CHECK(arguments.size() == 32);
			CHECK(arguments[0] == 0x10 && arguments[7] == 0x17);
			CHECK(arguments[8] == 0x80 && arguments[31] == 0x97);
			return 0x1234;
		}, error);
	CHECK(callable && s_hle);
	cpu.instructionPointer = *callable;
	{
		WupsGuestOwnerScope ownerScope{{72, 1}};
		s_hle(&cpu);
	}
	CHECK(calls == 1 && cpu.gpr[3] == 0x1234);

	cpu.instructionPointer = *callable;
	s_hle(&cpu);
	CHECK(calls == 1);
	CHECK(static_cast<std::int32_t>(cpu.gpr[3]) ==
		static_cast<std::int32_t>(WupsServiceStatus::OwnerMismatch));

	ModExecutionContext wrongOwner(99, 1, "wrong", 0x10000000, 4096);
	cpu.modExecutionContext = &wrongOwner;
	cpu.instructionPointer = *callable;
	{
		WupsGuestOwnerScope ownerScope{{72, 1}};
		s_hle(&cpu);
	}
	CHECK(static_cast<std::int32_t>(cpu.gpr[3]) ==
		static_cast<std::int32_t>(WupsServiceStatus::OwnerMismatch));
	cpu.modExecutionContext = nullptr;

	platform->ReleaseOwnerExports({72, 1});
	cpu.instructionPointer = *callable;
	s_hle(&cpu);
	CHECK(calls == 1);
	CHECK(static_cast<std::int32_t>(cpu.gpr[3]) ==
		static_cast<std::int32_t>(WupsServiceStatus::StaleGeneration));
	platform->FreeGuestData({72, 1}, *data);
	CHECK(s_allocations.empty());

	// A release racing a running handler cannot reclaim its code cave until the
	// handler drops its execution pin.
	std::promise<void> entered;
	std::promise<void> resume;
	auto resumeFuture = resume.get_future().share();
	const auto pinned = platform->RegisterFunction({72, 2},
		"homebrew_test", "Pinned",
		[&](std::span<const std::uint32_t>, std::string&) {
			entered.set_value();
			resumeFuture.wait();
			return 7;
		}, error);
	CHECK(pinned);
	PPCInterpreter_t workerCpu = cpu;
	workerCpu.instructionPointer = *pinned;
	auto executing = std::async(std::launch::async, [&] {
		WupsGuestOwnerScope ownerScope{{72, 2}};
		s_hle(&workerCpu);
	});
	entered.get_future().wait();
	auto releasing = std::async(std::launch::async, [&] {
		platform->ReleaseOwnerExports({72, 2});
	});
	CHECK(releasing.wait_for(std::chrono::milliseconds(20)) ==
		std::future_status::timeout);
	resume.set_value();
	executing.get();
	releasing.get();
	CHECK(workerCpu.gpr[3] == 7);
	CHECK(s_allocations.empty());
	s_cpu = nullptr;
	error.clear();
	CHECK(!platform->RegisterFunction({72, 1}, "homebrew_test", "Export",
		[](std::span<const std::uint32_t>, std::string&) { return 0; }, error));
	CHECK(error.find("CPU thread") != std::string::npos);

	WupsMappedMemoryInfo allocation;
	error.clear();
	CHECK(!platform->AllocateMappedMemory({72, 1}, 4096, 4096, true,
		WupsMappedMemoryPurpose::Cpu, error));
	CHECK(error.find("unavailable") != std::string::npos);

	platform->CancelCpuTasks({71, 2});
	platform->ReleaseOwnerExports({71, 2});
	std::cout << "WUPS production adapter tests passed\n";
	return 0;
}
