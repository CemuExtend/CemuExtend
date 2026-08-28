#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/CemodRuntime.h"
#include "Cafe/HW/Espresso/ModExecutionContext.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/TrustedCemodLifecycle.h"
#include "Cafe/HW/Espresso/WupsPluginHeap.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/libs/cemuextend/CemodPermission.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.h"
#include "Cemu/Logging/CemuLogging.h"
#include "WupsRuntimeStub.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{

	[[noreturn]] void CheckFailed(const char* expression, int line)
	{
		std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
		std::abort();
	}
#define CHECK(condition)                       \
	do                                         \
	{                                          \
		if (!(condition))                      \
			CheckFailed(#condition, __LINE__); \
	}                                          \
	while (false)

	thread_local PPCInterpreter_t* gCurrentCpu{};

	void Be16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value)
	{
		bytes[offset] = static_cast<std::byte>(value >> 8);
		bytes[offset + 1] = static_cast<std::byte>(value);
	}

	void Be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
	{
		bytes[offset] = static_cast<std::byte>(value >> 24);
		bytes[offset + 1] = static_cast<std::byte>(value >> 16);
		bytes[offset + 2] = static_cast<std::byte>(value >> 8);
		bytes[offset + 3] = static_cast<std::byte>(value);
	}

	std::vector<std::byte> LifecycleElf(bool loopingTick = false, bool hleImport = false)
	{
		constexpr std::uint32_t codeOffset = 84;
		constexpr std::uint32_t stringsOffset = 104;
		static constexpr char stringBytes[] =
			"\0cemod_init\0cemod_tick\0cemod_event\0cemod_shutdown\0CEX2Query\0";
		constexpr std::string_view strings{stringBytes, sizeof(stringBytes) - 1};
		constexpr std::uint32_t symbolsOffset = 168;
		constexpr std::uint32_t sectionsOffset = 264;
		std::vector<std::byte> elf(sectionsOffset + 3 * 40);
		elf[0] = std::byte{0x7f};
		elf[1] = std::byte{0x45};
		elf[2] = std::byte{0x4c};
		elf[3] = std::byte{0x46};
		elf[4] = std::byte{1};
		elf[5] = std::byte{2};
		elf[6] = std::byte{1};
		Be16(elf, 16, 2);
		Be16(elf, 18, 20);
		Be32(elf, 20, 1);
		Be32(elf, 24, 0x10000000);
		Be32(elf, 28, 52);
		Be32(elf, 32, sectionsOffset);
		Be16(elf, 40, 52);
		Be16(elf, 42, 32);
		Be16(elf, 44, 1);
		Be16(elf, 46, 40);
		Be16(elf, 48, 3);
		Be32(elf, 52, 1);
		Be32(elf, 56, codeOffset);
		Be32(elf, 60, 0x10000000);
		Be32(elf, 64, 0x10000000);
		Be32(elf, 68, hleImport ? 20 : 16);
		Be32(elf, 72, hleImport ? 20 : 16);
		Be32(elf, 76, 5);
		Be32(elf, 80, 4096);
		for (std::size_t index = 0; index < 4; ++index)
			Be32(elf, codeOffset + index * 4, 0x4e800020);
		if (loopingTick)
			Be32(elf, codeOffset + 4, 0x48000000);
		if (hleImport)
			Be32(elf, codeOffset + 16, 0x0400ffff);
		std::memcpy(elf.data() + stringsOffset, strings.data(), strings.size());
		const std::array<std::uint32_t, 4> names{1, 12, 23, 35};
		for (std::uint32_t index = 0; index < 4; ++index)
		{
			const auto symbol = symbolsOffset + (index + 1) * 16;
			Be32(elf, symbol, names[index]);
			Be32(elf, symbol + 4, 0x10000000 + index * 4);
			elf[symbol + 12] = std::byte{0x12};
			Be16(elf, symbol + 14, 1);
		}
		if (hleImport)
		{
			const auto symbol = symbolsOffset + 5 * 16;
			Be32(elf, symbol, 50);
			Be32(elf, symbol + 4, 0x10000010);
			elf[symbol + 12] = std::byte{0x12};
			Be16(elf, symbol + 14, 1);
		}
		const auto stringsSection = sectionsOffset + 40;
		Be32(elf, stringsSection + 4, 3);
		Be32(elf, stringsSection + 16, stringsOffset);
		Be32(elf, stringsSection + 20, strings.size());
		const auto symbolsSection = sectionsOffset + 80;
		Be32(elf, symbolsSection + 4, 2);
		Be32(elf, symbolsSection + 16, symbolsOffset);
		Be32(elf, symbolsSection + 20, hleImport ? 96 : 80);
		Be32(elf, symbolsSection + 24, 1);
		Be32(elf, symbolsSection + 36, 16);
		return elf;
	}

	CemodPackage Package(std::string principal, bool looping = false, bool hleImport = false)
	{
		CemodPackage package;
		package.principal = std::move(principal);
		package.targetTitleId = 0x0005000012345678ULL;
		package.elf = LifecycleElf(looping, hleImport);
		package.manifest.modId = "test.mod";
		package.manifest.requestedPermissions = 7;
		package.manifest.codeBytes = 4096;
		package.manifest.privateBytes = 4096;
		package.manifest.stackBytes = 4096;
		package.manifest.instructionsPerFrame = looping ? 2 : 100;
		package.manifest.timeMicrosecondsPerFrame = 1000;
		package.manifest.entrypoint = "cemod_init";
		return package;
	}

	void TestLoadLifecycleAndPermissionIntersection()
	{
		CemodRuntime runtime;
		std::string error;
		const auto handle = runtime.Load(Package("principal:one"), 3, 5, error);
		CHECK(handle.has_value());
		CHECK(runtime.Size() == 1);
		CHECK(runtime.Context(*handle)->GrantedPermissions() == 1);
		CHECK(runtime.Cpu(*handle)->modExecutionContext == runtime.Context(*handle));
		CHECK(runtime.Cpu(*handle)->global != nullptr);
		runtime.BeginFrame();
		CHECK(runtime.Invoke(*handle, CemodLifecycle::Tick));
		CHECK(runtime.Invoke(*handle, CemodLifecycle::Event, 0, 0));
		CHECK(runtime.Unload(*handle));
		CHECK(runtime.Size() == 0);

		auto networkPackage = Package("principal:network");
		networkPackage.manifest.requestedPermissions = cemuextend_hle::kCemodNetworkPermission;
		const auto network = runtime.Load(std::move(networkPackage), cemuextend_hle::kCemodPermissionMask,
										  cemuextend_hle::kCemodPermissionMask, error);
		CHECK(network.has_value());
		CHECK(runtime.Context(*network)->GrantedPermissions() == cemuextend_hle::kCemodNetworkPermission);
		CHECK(runtime.Unload(*network));
	}

	void TestLimitsAndIsolation()
	{
		CemodRuntime runtime;
		std::string error;
		const auto looping = runtime.Load(Package("principal:loop", true), 7, 7, error);
		const auto healthy = runtime.Load(Package("principal:healthy"), 7, 7, error);
		CHECK(looping && healthy);
		CHECK(runtime.Cpu(*looping)->global != runtime.Cpu(*healthy)->global);
		runtime.Cpu(*looping)->global->tb = 1234;
		CHECK(runtime.Cpu(*healthy)->global->tb == 0);
		for (int frame = 0; frame < 3; ++frame)
		{
			runtime.BeginFrame();
			CHECK(!runtime.Invoke(*looping, CemodLifecycle::Tick));
			CHECK(runtime.Invoke(*healthy, CemodLifecycle::Tick));
		}
		CHECK(runtime.Context(*looping)->IsStopped());
		CHECK(runtime.Context(*looping)->Fault().reason == ModFaultReason::InstructionQuota);
		CHECK(!runtime.Context(*healthy)->IsStopped());

		for (std::size_t index = runtime.Size(); index < CemodRuntime::kMaximumModsPerTitle; ++index)
			CHECK(runtime.Load(Package("principal:" + std::to_string(index)), 7, 7, error).has_value());
		CHECK(!runtime.Load(Package("principal:overflow"), 7, 7, error).has_value());
		CHECK(runtime.Size() == CemodRuntime::kMaximumModsPerTitle);
	}

	void TestCex2ImportBinding()
	{
		CemodRuntime runtime;
		std::string error;
		const auto handle = runtime.Load(Package("principal:import", false, true), 7, 7, error);
		CHECK(handle.has_value());
		const auto* instruction = runtime.Context(*handle)->Resolve(0x10000010, 4, ModMemoryPermission::Execute);
		CHECK(instruction);
		CHECK(std::to_integer<std::uint8_t>(instruction[0]) == 0x04);
		CHECK(std::to_integer<std::uint8_t>(instruction[1]) == 0x00);
		CHECK(std::to_integer<std::uint8_t>(instruction[2]) == 0x00);
		CHECK(std::to_integer<std::uint8_t>(instruction[3]) == 0x2a);
	}

	CemodPackage WupsPackage(std::string principal)
	{
		auto package = Package(std::move(principal));
		package.manifest.payload.format = CemodPayloadFormat::Wups;
		package.manifest.payload.path = "plugin.wps";
		return package;
	}

	// Brings a CemodRuntime into the state it is in for a title that has, at some
	// point in this Cemu process, seen a WUPS cemod: the lazily created
	// WupsPayloadRuntime exists from here on and is never destroyed again. The stub
	// runtime rejects the load itself, which is exactly the interesting case - the
	// runtime object outlives every individual plugin.
	void EnsureWupsRuntime(CemodRuntime& runtime)
	{
		std::string error;
		CHECK(!runtime.Load(WupsPackage("principal:wups"), 7, 7, error).has_value());
	}

	// A title without WUPS plugins must not hand any of its default heap to the
	// plugin heap. Before this was gated, every title launched after the first
	// WUPS-using one reserved WupsPluginHeap::kBackingSize out of its own heap for
	// a runtime with nothing to run.
	void TestPluginHeapIsNotReservedWithoutPlugins()
	{
		wups_runtime_stub::Reset();
		auto heap = std::make_shared<WupsPluginHeap>(nullptr);
		cafe::wups::SetActivePluginHeap(heap);

		CemodRuntime runtime;
		EnsureWupsRuntime(runtime);
		wups_runtime_stub::g_pluginCount = 0;
		runtime.OnApplicationStarts();

		CHECK(wups_runtime_stub::g_applicationStartCount == 1);
		CHECK(!heap->WasInitializationAttempted());
		CHECK(!heap->IsUsable());

		// A second title start in the same process must stay just as clean.
		runtime.UnloadAll();
		runtime.OnApplicationStarts();
		CHECK(!heap->WasInitializationAttempted());

		cafe::wups::SetActivePluginHeap(nullptr);
		wups_runtime_stub::Reset();
	}

	// A title that does start plugins must still get the reservation, it must
	// happen before any plugin init hook runs, and a failed reservation must leave
	// the heap unusable instead of falling back to the game's own heap.
	void TestPluginHeapIsReservedBeforePluginStart()
	{
		wups_runtime_stub::Reset();
		auto heap = std::make_shared<WupsPluginHeap>(nullptr);
		cafe::wups::SetActivePluginHeap(heap);

		CemodRuntime runtime;
		EnsureWupsRuntime(runtime);
		wups_runtime_stub::g_pluginCount = 1;
		bool attemptedBeforePluginStart = false;
		wups_runtime_stub::g_onApplicationStarts = [&] {
			attemptedBeforePluginStart = heap->WasInitializationAttempted();
		};
		runtime.OnApplicationStarts();

		CHECK(wups_runtime_stub::g_applicationStartCount == 1);
		CHECK(attemptedBeforePluginStart);
		CHECK(heap->WasInitializationAttempted());
		// No emulated CPU thread here, so carving the backing out of the game heap
		// cannot succeed - the heap must fail closed rather than serve plugin
		// allocations from anywhere else.
		CHECK(!heap->IsUsable());
		CHECK(heap->Alloc(WupsOwnerToken{1, 1}, 16) == 0);
		CHECK(heap->AllocEx(WupsOwnerToken{1, 1}, 16, 0x40) == 0);

		// Title teardown re-arms the reservation for the next title.
		runtime.UnloadAll();
		CHECK(!heap->WasInitializationAttempted());

		cafe::wups::SetActivePluginHeap(nullptr);
		wups_runtime_stub::Reset();
	}

	void TestTrustedReleaseIsDeferredUntilLateTitleShutdown()
	{
		CemodRuntime runtime;
		std::string error;
		constexpr std::uint64_t firstTitle = 0x0005000012345678ULL;
		constexpr std::uint64_t secondTitle = 0x0005000087654321ULL;
		CHECK(runtime.ReadyForNextTitle(error));
		CHECK(runtime.BeginTrustedTitle(firstTitle, error));
		CHECK(!runtime.TitleShutdownPrepared());
		CHECK(!runtime.ReadyForNextTitle(error));
		CHECK(!runtime.ReleaseTrustedAfterTitleThreadsStopped(error));
		runtime.UnloadAll();
		CHECK(runtime.TitleShutdownPrepared());
		CHECK(runtime.Size() == 1); // retained mapping, not a callable payload
		CHECK(!runtime.ReadyForNextTitle(error));
		CHECK(!runtime.BeginTrustedTitle(secondTitle, error));
		CHECK(!runtime.ReleaseTrustedAfterTitleThreadsStopped(error));
		CHECK(runtime.MarkTrustedTitleThreadsStopped(error));
		CHECK(runtime.ReleaseTrustedAfterTitleThreadsStopped(error));
		CHECK(runtime.Size() == 0);
		CHECK(runtime.ReadyForNextTitle(error));
		CHECK(runtime.BeginTrustedTitle(secondTitle, error));
		runtime.AbandonAllForTitleShutdown();
		CHECK(runtime.TitleShutdownPrepared());
		CHECK(runtime.MarkTrustedTitleThreadsStopped(error));
		CHECK(runtime.ReleaseTrustedAfterTitleThreadsStopped(error));
	}

	void TestTitleRplUnloadPendingBlocksNextTitle()
	{
		CemodRuntime runtime;
		std::string error;
		CHECK(runtime.ReadyForNextTitle(error));
		CHECK(!runtime.TitleRplUnloadPending());
		runtime.MarkTitleRplUnloadPending();
		CHECK(runtime.TitleRplUnloadPending());
		CHECK(!runtime.ReadyForNextTitle(error));
		CHECK(error.find("RPL map") != std::string::npos);
		// Models RPLLoader_UnloadAll() succeeding after the external lease drains.
		runtime.MarkTitleRplUnloadComplete();
		CHECK(!runtime.TitleRplUnloadPending());
		CHECK(runtime.ReadyForNextTitle(error));
	}

	void TestTrustedLifecycleRejectsEarlyReleaseAndCrossTitleReuse()
	{
		TrustedCemodLifecycle lifecycle;
		std::string error;
		constexpr std::uint64_t firstTitle = 0x0005000011111111ULL;
		constexpr std::uint64_t secondTitle = 0x0005000022222222ULL;
		CHECK(lifecycle.IsReady());
		CHECK(!lifecycle.Begin(0, error));
		CHECK(lifecycle.Begin(firstTitle, error));
		CHECK(lifecycle.Accepts(firstTitle));
		CHECK(!lifecycle.Accepts(secondTitle));
		CHECK(!lifecycle.Begin(secondTitle, error));
		CHECK(!lifecycle.MarkThreadsStopped(error));
		CHECK(!lifecycle.CompleteRelease(error));
		lifecycle.RequestRelease();
		CHECK(lifecycle.ReleasePending());
		CHECK(!lifecycle.Accepts(firstTitle));
		CHECK(!lifecycle.Begin(secondTitle, error));
		CHECK(!lifecycle.CompleteRelease(error));
		// RPL callbacks and explicit shutdown may repeat the request safely.
		lifecycle.RequestRelease();
		CHECK(lifecycle.ReleasePending());
		CHECK(lifecycle.MarkThreadsStopped(error));
		CHECK(lifecycle.MarkThreadsStopped(error));
		CHECK(lifecycle.CompleteRelease(error));
		CHECK(lifecycle.IsReady());
		CHECK(lifecycle.CompleteRelease(error));
		CHECK(lifecycle.Begin(secondTitle, error));
	}

} // namespace

PPCInterpreter_t* PPCInterpreter_getCurrentInstance()
{
	return gCurrentCpu;
}
void PPCInterpreter_setCurrentInstance(PPCInterpreter_t* cpu)
{
	gCurrentCpu = cpu;
}
void PPCInterpreterSlim_executeInstruction(PPCInterpreter_t* cpu)
{
	auto* bytes = cpu->modExecutionContext->Resolve(cpu->instructionPointer, 4, ModMemoryPermission::Execute);
	if (!bytes)
		return;
	const auto instruction = (std::to_integer<std::uint32_t>(bytes[0]) << 24) |
							 (std::to_integer<std::uint32_t>(bytes[1]) << 16) | (std::to_integer<std::uint32_t>(bytes[2]) << 8) |
							 std::to_integer<std::uint32_t>(bytes[3]);
	if (instruction == 0x4e800020)
		cpu->instructionPointer = cpu->spr.LR;
	else if (instruction != 0x48000000)
		cpu->modExecutionContext->Stop(ModFaultReason::InvalidMapping, cpu->instructionPointer, ModMemoryPermission::Execute);
}

bool PPCRecompiler_executeSandboxInstruction(PPCInterpreter_t* cpu)
{
	PPCInterpreterSlim_executeInstruction(cpu);
	return true;
}
void PPCRecompiler_invalidateSandboxContext(uint64, uint32) {}
sint32 osLib_getFunctionIndex(const char* libraryName, const char* functionName)
{
	return std::strcmp(libraryName, "cemuextend") == 0 && std::strcmp(functionName, "CEX2Query") == 0 ? 42 : -1;
}

namespace cemuextend_hle
{
	void ConfigureCex2HleAccess(ModExecutionContext&) {}
} // namespace cemuextend_hle

// CemodRuntime.cpp and WupsPluginHeap.cpp reach into Cemu's logging, guest
// address space and coreinit heaps. None of that is linked into this isolated
// test, so provide the same kind of inert stand-ins the other Cafe unit tests
// use. The plugin heap must never manage to reserve a backing here, which is
// exactly the fail-closed path the plugin-heap tests assert on.
uint64 s_loggingFlagMask = 0;
bool cemuLog_log(LogType, std::string_view)
{
	return true;
}
bool cemuLog_log(LogType, std::u8string_view)
{
	return true;
}

uint8* memory_base = nullptr;
uint32 memory_readU32(uint32)
{
	return 0;
}
uint8* memory_getPointerFromVirtualOffset(uint32)
{
	return nullptr;
}
uint32 memory_getVirtualOffsetFromPointer(void*)
{
	return 0;
}

namespace coreinit
{
	MEMHeapHandle MEMCreateExpHeapEx(void*, uint32, uint32)
	{
		return nullptr;
	}
	void* MEMAllocFromExpHeapEx(MEMHeapHandle, uint32, sint32)
	{
		return nullptr;
	}
	void MEMFreeToExpHeap(MEMHeapHandle, void*) {}
	void* _weak_MEMAllocFromDefaultHeapEx(uint32, sint32)
	{
		return nullptr;
	}
} // namespace coreinit

void WupsOwnerScopedHeapTracker::OnAllocation(WupsOwnerToken, WupsHeapAllocatorKind,
											  std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
void WupsOwnerScopedHeapTracker::OnFree(WupsOwnerToken, std::uint32_t) {}

int main()
{
	TestLoadLifecycleAndPermissionIntersection();
	TestLimitsAndIsolation();
	TestCex2ImportBinding();
	TestPluginHeapIsNotReservedWithoutPlugins();
	TestPluginHeapIsReservedBeforePluginStart();
	TestTrustedReleaseIsDeferredUntilLateTitleShutdown();
	TestTitleRplUnloadPendingBlocksNextTitle();
	TestTrustedLifecycleRejectsEarlyReleaseAndCrossTitleReuse();
	return 0;
}
