#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/ModuleExportRegistry.h"
#include "Cafe/HW/Espresso/WumsBinary.h"
#include "Cafe/HW/Espresso/WumsRuntime.h"
#include "Cafe/HW/Espresso/WupsFunctionPatcher.h"
#include "Cafe/HW/Espresso/WupsServices.h"
#include "Cafe/tests/WupsTestImage.h"
#include "Cemu/Logging/CemuLogging.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>

uint64 s_loggingFlagMask = 1ULL << static_cast<uint64>(LogType::Force);

bool cemuLog_log(LogType, std::string_view text)
{
	std::cerr << text << '\n';
	return true;
}

bool cemuLog_log(LogType, std::u8string_view)
{
	return true;
}

namespace
{
	[[noreturn]] void CheckFailed(const char* expression, int line)
	{
		std::cerr << "CHECK failed at line " << line << ": "
			<< expression << '\n';
		std::abort();
	}
#define CHECK(condition) do { if (!(condition)) CheckFailed(#condition, __LINE__); } while (false)

	ModuleProviderDescriptor Provider(
		ModuleProviderKind kind, std::string name,
		std::uint64_t owner, std::uint32_t generation = 1,
		std::uint64_t lifetime = 1, std::string version = "1.0.0")
	{
		return {
			kind, std::move(name), std::move(version),
			{owner, generation, lifetime}};
	}

	void TestRegistryCollisionKindsAndLifetime()
	{
		ModuleExportRegistry registry;
		ModuleProviderHandle backend;
		std::string error;
		const std::array exports{
			ModuleExportDescriptor{
				"Open", WupsSymbolKind::Function, 0x02001000},
			ModuleExportDescriptor{
				"state", WupsSymbolKind::Data, 0x10001000},
		};
		const auto descriptor = Provider(
			ModuleProviderKind::WupsBackend,
			"homebrew_wupsbackend", 10);
		CHECK(registry.Publish(descriptor, exports, backend, error));
		CHECK(backend);
		CHECK(registry.ProviderCount() == 1);

		auto lease = registry.Resolve(
			"homebrew_wupsbackend", "Open",
			WupsSymbolKind::Function, {99, 3, 1}, error);
		CHECK(lease);
		CHECK(lease->Address() == 0x02001000);
		CHECK(lease->Provider().owner == descriptor.owner);
		CHECK(registry.PinCount(backend) == 1);
		CHECK(!registry.Unpublish(backend, descriptor.owner, error));
		CHECK(error.find("pinned") != std::string::npos);

		auto wrongKind = registry.Resolve(
			"homebrew_wupsbackend", "Open",
			WupsSymbolKind::Data, {99, 3, 1}, error);
		CHECK(!wrongKind);
		CHECK(error.find("not the requested data") != std::string::npos);

		ModuleProviderHandle collision;
		const std::array collisionExports{
			ModuleExportDescriptor{
				"Open", WupsSymbolKind::Function, 0x02002000}};
		CHECK(!registry.Publish(
			Provider(ModuleProviderKind::CustomModule,
				"homebrew_wupsbackend", 11),
			collisionExports, collision, error));
		CHECK(error.find("ambiguous") != std::string::npos);

		lease.reset();
		CHECK(registry.Unpublish(backend, descriptor.owner, error));
		CHECK(registry.ProviderCount() == 0);
		CHECK(!registry.Unpublish(backend, descriptor.owner, error));
		CHECK(error.find("stale") != std::string::npos);
	}

	class FakePatchPlatform final : public IWupsPatchPlatform
	{
	public:
		WupsPatchProcess CurrentProcess() const override { return process; }
		void SetCurrentProcess(WupsPatchProcess value) override { process = value; }

		std::optional<WupsResolvedPatchTarget> ResolveFunction(
			std::string_view module, std::string_view function,
			std::string& error) override
		{
			error.clear();
			const auto found = symbols.find(
				{std::string(module), std::string(function)});
			if (found == symbols.end())
				return std::nullopt;
			return WupsResolvedPatchTarget{
				found->second, std::string(module), moduleLifetime};
		}

		std::optional<std::uint32_t> VirtualForPhysical(
			std::uint32_t physical, std::string& error) override
		{
			error.clear();
			if (physical == 0)
			{
				error = "physical address is zero";
				return std::nullopt;
			}
			return physical;
		}

		bool IsExecutable(std::uint32_t address,
			std::uint32_t size) const override
		{
			return Contains(executable, address, size);
		}

		bool IsWritable(std::uint32_t address,
			std::uint32_t size) const override
		{
			return Contains(writable, address, size);
		}

		bool ReadWords(std::uint32_t address,
			std::span<std::uint32_t> output,
			std::string& error) override
		{
			for (std::size_t index = 0; index < output.size(); ++index)
			{
				const auto found = words.find(
					address + static_cast<std::uint32_t>(index * 4));
				if (found == words.end())
				{
					error = "read from missing test memory";
					return false;
				}
				output[index] = found->second;
			}
			return true;
		}

		bool WriteWords(std::uint32_t address,
			std::span<const std::uint32_t> input,
			std::string& error) override
		{
			if (failWriteAddress == address)
			{
				error = "injected write failure";
				return false;
			}
			for (std::size_t index = 0; index < input.size(); ++index)
				words[address + static_cast<std::uint32_t>(index * 4)] =
					input[index];
			writes.push_back(address);
			return true;
		}

		bool AllocateExecutableNear(std::uint32_t,
			std::uint32_t size, std::uint32_t,
			std::uint32_t& address, std::string&) override
		{
			address = nextAllocation;
			nextAllocation += size;
			allocations.emplace(address, size);
			executable.emplace_back(address, size);
			for (std::uint32_t offset = 0; offset < size; offset += 4)
				words[address + offset] = 0;
			return true;
		}

		void FreeExecutable(
			std::uint32_t address, std::uint32_t size) override
		{
			const auto found = allocations.find(address);
			CHECK(found != allocations.end());
			CHECK(found->second == size);
			allocations.erase(found);
		}

		void InvalidateCode(
			std::uint32_t address, std::uint32_t size) override
		{
			invalidations.emplace_back(address, size);
		}

		void AddFunction(std::string module, std::string function,
			std::uint32_t address, std::uint32_t instruction)
		{
			symbols.emplace(
				std::pair{std::move(module), std::move(function)}, address);
			words[address] = instruction;
			executable.emplace_back(address, 4);
		}

		void AddReplacement(std::uint32_t address)
		{
			words[address] = 0x4e800020;
			executable.emplace_back(address, 4);
		}

		void AddStorage(std::uint32_t address, std::uint32_t value = 0)
		{
			words[address] = value;
			writable.emplace_back(address, 4);
		}

		static bool Contains(
			const std::vector<std::pair<std::uint32_t, std::uint32_t>>& ranges,
			std::uint32_t address, std::uint32_t size)
		{
			for (const auto& [base, rangeSize] : ranges)
				if (address >= base && address - base <= rangeSize &&
					size <= rangeSize - (address - base))
					return true;
			return false;
		}

		WupsPatchProcess process{WupsPatchProcess::Game};
		std::uint64_t moduleLifetime{44};
		std::map<std::pair<std::string, std::string>, std::uint32_t> symbols;
		std::map<std::uint32_t, std::uint32_t> words;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> executable;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> writable;
		std::map<std::uint32_t, std::uint32_t> allocations;
		std::vector<std::uint32_t> writes;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> invalidations;
		std::uint32_t nextAllocation{0x01800000};
		std::optional<std::uint32_t> failWriteAddress;
	};

	WupsPatchRequest NamedPatch(
		WupsPatchOwner owner, std::size_t index,
		std::string function, std::uint32_t replacement,
		std::uint32_t storage, bool mandatory = true)
	{
		WupsPatchRequest result;
		result.owner = owner;
		result.descriptorIndex = index;
		result.targetKind = WupsPatchTargetKind::NamedFunction;
		result.mandatory = mandatory;
		result.moduleName = "coreinit";
		result.functionName = std::move(function);
		result.replacementAddress = replacement;
		result.callThroughStorage = storage;
		result.process = WupsPatchProcess::GameAndMenu;
		return result;
	}

	void TestPpcRelocationBranches()
	{
		std::vector<std::uint32_t> relocated;
		std::size_t consumed{};
		std::string error;
		const std::array nearBranch{
			0x48000011U, // bl + 0x10
			0x60000000U,
		};
		CHECK(PpcFunctionRelocator::Relocate(
			nearBranch, 0x02000000, 0x02001000, 8,
			relocated, consumed, error));
		CHECK(consumed == 8);
		CHECK((relocated[0] >> 26) == 18);
		CHECK((relocated[0] & 1U) == 1U);

		const std::array farConditional{
			0x41820040U, // beq outside REL14 from the relocated address
		};
		CHECK(PpcFunctionRelocator::Relocate(
			farConditional, 0x02000000, 0x08000000, 4,
			relocated, consumed, error));
		CHECK(relocated.size() >= 6);
		CHECK((relocated[0] >> 26) == 16);
		CHECK((relocated.back() >> 26) == 19 ||
			relocated.back() == 0x4e800420U);

		CHECK(!PpcFunctionRelocator::Relocate(
			farConditional, 0x02000001, 0x08000000, 4,
			relocated, consumed, error));
		CHECK(error.find("alignment") != std::string::npos);
	}

	void TestPatchApplyConflictRollbackAndDynamicEvents()
	{
		auto platform = std::make_shared<FakePatchPlatform>();
		platform->AddFunction(
			"coreinit", "OSReport", 0x02010000, 0x9421fff0);
		platform->AddFunction(
			"coreinit", "OSFatal", 0x02010100, 0x7c0802a6);
		platform->AddReplacement(0x09000000);
		platform->AddReplacement(0x09000100);
		platform->AddStorage(0x10001000, 0xaaaaaaaa);
		platform->AddStorage(0x10001004, 0xbbbbbbbb);
		WupsFunctionPatchManager manager(platform);
		std::string error;

		const WupsPatchOwner firstOwner{1, 2};
		const auto first = NamedPatch(
			firstOwner, 0, "OSReport", 0x09000000, 0x10001000);
		CHECK(manager.Apply(std::span{&first, 1}, error));
		CHECK(manager.Applied().size() == 1);
		CHECK(platform->words[0x02010000] != 0x9421fff0);
		CHECK(platform->words[0x10001000] == 0x01800000);
		CHECK(!platform->invalidations.empty());

		const auto conflict = NamedPatch(
			WupsPatchOwner{2, 1}, 0, "OSReport",
			0x09000100, 0x10001004);
		CHECK(!manager.Apply(std::span{&conflict, 1}, error));
		CHECK(error.find("overlaps") != std::string::npos);
		CHECK(manager.Applied().size() == 1);

		CHECK(manager.RemoveOwner(firstOwner, error));
		CHECK(platform->words[0x02010000] == 0x9421fff0);
		CHECK(platform->words[0x10001000] == 0xaaaaaaaa);
		CHECK(platform->allocations.empty());

		CHECK(manager.Apply(std::span{&first, 1}, error));
		platform->failWriteAddress = 0x02010000;
		CHECK(!manager.RemoveOwner(firstOwner, error));
		const auto blockedWhileRestoreFails = NamedPatch(
			{6, 1}, 0, "OSReport", 0x09000100, 0x10001004);
		CHECK(!manager.Apply(
			std::span{&blockedWhileRestoreFails, 1}, error));
		CHECK(error.find("overlaps") != std::string::npos);
		platform->failWriteAddress.reset();
		CHECK(manager.RemoveOwner(firstOwner, error));

		auto virtualPatch = NamedPatch(
			{7, 1}, 0, "", 0x09000000, 0x10001000);
		virtualPatch.targetKind = WupsPatchTargetKind::VirtualAddress;
		virtualPatch.virtualAddress = 0x02010000;
		CHECK(manager.Apply(std::span{&virtualPatch, 1}, error));
		CHECK(manager.RemoveOwner({7, 1}, error));
		CHECK(platform->words[0x02010000] == 0x9421fff0);

		auto physicalPatch = NamedPatch(
			{8, 1}, 0, "", 0x09000100, 0x10001004);
		physicalPatch.targetKind = WupsPatchTargetKind::PhysicalAddress;
		physicalPatch.physicalAddress = 0x02010100;
		CHECK(manager.Apply(std::span{&physicalPatch, 1}, error));
		CHECK(manager.RemoveOwner({8, 1}, error));
		CHECK(platform->words[0x02010100] == 0x7c0802a6);

		const std::array transaction{
			NamedPatch({3, 1}, 0, "OSReport",
				0x09000000, 0x10001000),
			NamedPatch({3, 1}, 1, "OSFatal",
				0x09000100, 0x10001004),
		};
		platform->failWriteAddress = 0x02010100;
		CHECK(!manager.Apply(transaction, error));
		CHECK(error.find("injected write failure") != std::string::npos);
		CHECK(platform->words[0x02010000] == 0x9421fff0);
		CHECK(platform->words[0x10001000] == 0xaaaaaaaa);
		CHECK(manager.Applied().empty());
		platform->failWriteAddress.reset();

		platform->symbols.erase({"coreinit", "OSFatal"});
		const auto pending = NamedPatch(
			{4, 1}, 0, "OSFatal", 0x09000100, 0x10001004, false);
		CHECK(manager.Apply(std::span{&pending, 1}, error));
		CHECK(manager.PendingCount() == 1);
		platform->symbols.emplace(
			std::pair{std::string("coreinit"), std::string("OSFatal")},
			0x02010100);
		CHECK(manager.OnModuleLoaded({"coreinit", 44}, error));
		CHECK(manager.PendingCount() == 0);
		CHECK(manager.Applied().size() == 1);
		CHECK(manager.OnModuleUnloading({"coreinit", 44}, error));
		CHECK(platform->words[0x02010100] == 0x7c0802a6);
		CHECK(manager.PendingCount() == 1);
		CHECK(manager.RemoveOwner({4, 1}, error));
	}

	class FacadeGuestPlatform final : public IWupsPlatform
	{
	public:
		bool ValidateGuestRange(std::uint32_t address, std::uint32_t size,
			WupsGuestAccess access) const override
		{
			if (size == 0 || address > 0x20000000U - size) return false;
			if (access == WupsGuestAccess::Execute)
				return address >= 0x09000000 && address + size <= 0x09001000;
			if (access == WupsGuestAccess::Write)
				return address >= 0x10001000 && address + size <= 0x10002000;
			return memory.contains(address) &&
				memory.lower_bound(address + size - 1) != memory.end();
		}
		bool ReadGuest(std::uint32_t address,
			std::span<std::byte> output) const override
		{
			for (std::size_t i = 0; i < output.size(); ++i)
			{
				const auto found = memory.find(address + i);
				if (found == memory.end()) return false;
				output[i] = found->second;
			}
			return true;
		}
		bool WriteGuest(std::uint32_t,
			std::span<const std::byte>) override { return true; }
		std::optional<std::uint32_t> AllocateGuestData(WupsOwnerToken,
			std::uint32_t, std::uint32_t, std::string&) override { return {}; }
		void FreeGuestData(WupsOwnerToken, std::uint32_t) override {}
		std::optional<std::uint32_t> RegisterFunction(WupsOwnerToken,
			std::string_view, std::string_view, WupsHostExportHandler,
			std::string&) override { return {}; }
		void ReleaseOwnerExports(WupsOwnerToken) override {}
		std::uint64_t CurrentGuestThreadId() const override { return 1; }
		bool QueueCpuTask(WupsOwnerToken, std::function<void()> task,
			std::string&) override { task(); return true; }
		void CancelCpuTasks(WupsOwnerToken) override {}
		std::optional<WupsMappedMemoryInfo> AllocateMappedMemory(WupsOwnerToken,
			std::uint32_t, std::uint32_t, bool, WupsMappedMemoryPurpose,
			std::string&) override { return {}; }
		bool FreeMappedMemory(WupsOwnerToken, const WupsMappedMemoryInfo&,
			std::string&) override { return false; }
		void ShowNotification(WupsOwnerToken,
			const WupsNotificationModel&) override {}
		void Log(WupsOwnerToken, WupsLogLevel, std::string_view,
			std::string_view, std::string_view) override {}

		void U32(std::uint32_t address, std::uint32_t value)
		{
			for (unsigned shift = 0; shift < 32; shift += 8)
				memory[address + shift / 8] = std::byte{
					static_cast<unsigned char>(value >> (24 - shift))};
		}
		void String(std::uint32_t address, std::string_view value)
		{
			for (std::size_t i = 0; i < value.size(); ++i)
				memory[address + i] = std::byte{
					static_cast<unsigned char>(value[i])};
			memory[address + value.size()] = std::byte{};
		}

		std::map<std::uint32_t, std::byte> memory;
	};

	void TestFunctionPatcherFacadeIncrementalDescriptors()
	{
		auto patchPlatform = std::make_shared<FakePatchPlatform>();
		patchPlatform->AddFunction("coreinit", "OSReport",
			0x02010000, 0x9421fff0);
		patchPlatform->AddFunction("coreinit", "OSFatal",
			0x02010100, 0x7c0802a6);
		patchPlatform->AddReplacement(0x09000000);
		patchPlatform->AddReplacement(0x09000100);
		patchPlatform->AddStorage(0x10001000);
		patchPlatform->AddStorage(0x10001004);
		auto manager = std::make_shared<WupsFunctionPatchManager>(patchPlatform);
		const WupsPatchOwner patchOwner{41, 3};
		const auto staticPatch = NamedPatch(patchOwner, 0, "OSFatal",
			0x09000100, 0x10001004);
		std::string error;
		CHECK(manager->Apply(std::span{&staticPatch, 1}, error));

		auto guest = std::make_shared<FacadeGuestPlatform>();
		constexpr std::uint32_t descriptor = 0x1000;
		constexpr std::uint32_t name = 0x1100;
		guest->U32(descriptor + 0, 3); // v3
		guest->U32(descriptor + 4, 0); // RPL by library/name
		guest->U32(descriptor + 8, 0);
		guest->U32(descriptor + 12, 0);
		guest->U32(descriptor + 16, 0x09000000);
		guest->U32(descriptor + 20, 0x10001000);
		guest->U32(descriptor + 24,
			static_cast<std::uint32_t>(WupsPatchProcess::Game));
		guest->U32(descriptor + 28, name);
		guest->U32(descriptor + 32, 2); // LIBRARY_COREINIT
		guest->String(name, "OSReport");

		auto facade = CreateWupsFunctionPatcherFacade(guest, manager);
		CHECK(facade && facade->ApiVersion() == 2);
		std::uint32_t handle{};
		bool applied{};
		CHECK(facade->AddPatch({41, 3}, descriptor, false,
			handle, applied, error) == WupsServiceStatus::Success);
		CHECK(handle != 0 && applied);
		CHECK(manager->Applied().size() == 2);
		CHECK(facade->IsPatchApplied({41, 3}, handle, applied, error) ==
			WupsServiceStatus::Success && applied);
		CHECK(facade->IsPatchApplied({41, 2}, handle, applied, error) ==
			WupsServiceStatus::StaleGeneration);
		CHECK(facade->RemovePatch({99, 3}, handle, error) ==
			WupsServiceStatus::OwnerMismatch);
		CHECK(facade->RemovePatch({41, 3}, handle, error) ==
			WupsServiceStatus::Success);
		CHECK(manager->Applied().size() == 1);
		facade->ReleaseOwner({41, 3});
		CHECK(manager->RemoveOwner(patchOwner, error));
	}

	WumsInspection Module(std::string name, std::string version = "1.0.0")
	{
		WumsInspection result;
		result.metadata.moduleName = std::move(name);
		result.metadata.version = std::move(version);
		result.metadata.abiVersion = {0, 3, 6};
		return result;
	}

	void TestWumsParserAndDependencyGraph()
	{
		WupsTestImageOptions options;
		options.wums = true;
		constexpr char metadata[] =
			"export_name=homebrew_test\0wums=0.3.6\0version=1.2.0\0"
			"author=Test\0";
		options.metadata.assign(metadata, sizeof(metadata) - 1);
		options.hookType =
			static_cast<std::uint32_t>(WumsHookType::ApplicationStarts);
		constexpr char dependencies[] =
			"homebrew_base@>=1.0.0\0?homebrew_optional\0";
		options.dependencies.assign(dependencies, sizeof(dependencies) - 1);
		auto image = BuildWupsTestImage(options);
		std::string error;
		const auto parsed = WumsBinaryInspector::Inspect(image, error);
		CHECK(parsed);
		CHECK(parsed->metadata.moduleName == "homebrew_test");
		CHECK(parsed->metadata.abiVersion == WupsVersion(0, 3, 6));
		CHECK(parsed->dependencies.size() == 2);
		CHECK(parsed->dependencies[0].match ==
			WumsDependencyMatch::AtLeast);
		CHECK(parsed->dependencies[1].optional);
		CHECK(parsed->exports.size() == 1);
		CHECK(parsed->exports[0].kind == WupsSymbolKind::Function);

		image[9] = std::byte{'X'};
		CHECK(!WumsBinaryInspector::Inspect(image, error));
		CHECK(error.find("WUMS RPL") != std::string::npos);

		ModuleExportRegistry registry;
		auto base = Module("homebrew_base", "1.1.0");
		auto middle = Module("homebrew_middle");
		middle.dependencies.push_back({
			"homebrew_base", false,
			WumsDependencyMatch::AtLeast, WupsVersion{1, 0, 0}});
		auto leaf = Module("homebrew_leaf");
		leaf.dependencies.push_back({
			"homebrew_middle", false,
			WumsDependencyMatch::Any, std::nullopt});
		const std::array modules{leaf, middle, base};
		std::vector<std::size_t> order;
		CHECK(WumsDependencyGraph::Build(modules, registry, order, error));
		CHECK(order.size() == 3);
		CHECK(modules[order[0]].metadata.moduleName == "homebrew_base");
		CHECK(modules[order[2]].metadata.moduleName == "homebrew_leaf");

		auto cycleA = Module("cycle_a");
		auto cycleB = Module("cycle_b");
		cycleA.dependencies.push_back({
			"cycle_b", false, WumsDependencyMatch::Any, std::nullopt});
		cycleB.dependencies.push_back({
			"cycle_a", false, WumsDependencyMatch::Any, std::nullopt});
		const std::array cycle{cycleA, cycleB};
		CHECK(!WumsDependencyGraph::Build(cycle, registry, order, error));
		CHECK(error.find("cycle") != std::string::npos);

		auto missing = Module("missing_user");
		missing.dependencies.push_back({
			"not_present", false,
			WumsDependencyMatch::Any, std::nullopt});
		CHECK(!WumsDependencyGraph::Build(
			std::span{&missing, 1}, registry, order, error));
		CHECK(error.find("missing mandatory") != std::string::npos);

		auto mismatch = Module("mismatch");
		mismatch.dependencies.push_back({
			"homebrew_base", false,
			WumsDependencyMatch::AtLeast, WupsVersion{2, 0, 0}});
		const std::array mismatchModules{base, mismatch};
		CHECK(!WumsDependencyGraph::Build(
			mismatchModules, registry, order, error));
		CHECK(error.find("version mismatch") != std::string::npos);
	}

	struct WumsLog
	{
		std::vector<std::string> mapped;
		std::vector<std::pair<std::string, WumsHookType>> hooks;
		std::vector<std::string> unloaded;
		std::optional<std::string> failLink;
		std::optional<std::pair<std::string, WumsHookType>> failHook;
	};

	class FakeWumsLoader final : public IWumsModuleLoader
	{
	public:
		explicit FakeWumsLoader(std::shared_ptr<WumsLog> log) :
			m_log(std::move(log)) {}

		bool Map(std::span<const std::byte>, std::string_view name,
			const ModuleProviderOwner& owner, WumsImportResolver,
			RPLModule*& module, std::uint64_t& lifetime,
			std::string&) override
		{
			m_log->mapped.emplace_back(name);
			module = reinterpret_cast<RPLModule*>(
				static_cast<std::uintptr_t>(0x10000 + owner.owner * 0x100));
			lifetime = owner.owner + 1000;
			m_names.emplace(module, std::string(name));
			return true;
		}

		bool Link(RPLModule* module, std::uint64_t,
			std::string& error) override
		{
			if (m_log->failLink &&
				m_names[module].starts_with(*m_log->failLink))
			{
				error = "injected WUMS link failure";
				return false;
			}
			return true;
		}

		bool ResolveAddress(RPLModule* module, std::uint64_t,
			std::uint32_t virtualAddress, std::uint32_t,
			WupsSymbolKind, std::uint32_t& mapped,
			std::string&) override
		{
			mapped = virtualAddress +
				static_cast<std::uint32_t>(
					reinterpret_cast<std::uintptr_t>(module) & 0xffff);
			return true;
		}

		bool Invoke(RPLModule* module, std::uint64_t,
			std::uint32_t target, std::span<const std::uint32_t>,
			std::uint32_t& result, std::string&) override
		{
			result = 0;
			m_invocations.emplace_back(module, target);
			return true;
		}

		bool Unload(RPLModule* module, std::uint64_t,
			std::string&) override
		{
			m_log->unloaded.push_back(m_names[module]);
			m_names.erase(module);
			return true;
		}

		std::vector<std::pair<RPLModule*, std::uint32_t>> m_invocations;

	private:
		std::shared_ptr<WumsLog> m_log;
		std::map<RPLModule*, std::string> m_names;
	};

	class FakeWumsServices final : public IWumsRuntimeServices
	{
	public:
		explicit FakeWumsServices(std::shared_ptr<WumsLog> log) :
			m_log(std::move(log)) {}

		bool PrepareHook(const WumsInspection& inspection,
			const ModuleProviderOwner&, WumsHookType type,
			WumsHookInvocation& invocation, std::string& error) override
		{
			invocation = {};
			m_log->hooks.emplace_back(
				inspection.metadata.moduleName, type);
			if (m_log->failHook == std::pair{
				inspection.metadata.moduleName, type})
			{
				error = "injected WUMS hook failure";
				return false;
			}
			return true;
		}

		void ReleaseModule(
			const WumsInspection&, const ModuleProviderOwner&) override {}

	private:
		std::shared_ptr<WumsLog> m_log;
	};

	WumsModuleDefinition Definition(
		WumsInspection inspection, std::uint32_t address)
	{
		inspection.hooks.push_back({
			WumsHookType::InitWutMalloc, address});
		inspection.hooks.push_back({
			WumsHookType::FiniWutMalloc, address + 4});
		inspection.hooks.push_back({
			WumsHookType::ApplicationStarts, address + 8});
		inspection.hooks.push_back({
			WumsHookType::ApplicationEnds, address + 12});
		inspection.exports.push_back({
			inspection.metadata.moduleName, "Export",
			WupsSymbolKind::Function, address + 16, true});
		WumsModuleDefinition result;
		result.fileName = inspection.metadata.moduleName + ".wms";
		result.image = {std::byte{1}};
		result.inspection = std::move(inspection);
		return result;
	}

	void TestWumsRuntimeOrderingRollbackAndUnload()
	{
		auto registry = std::make_shared<ModuleExportRegistry>();
		auto log = std::make_shared<WumsLog>();
		auto loader = std::make_shared<FakeWumsLoader>(log);
		auto services = std::make_shared<FakeWumsServices>(log);
		WumsModuleRuntime runtime(registry, loader, services);

		auto base = Module("homebrew_base");
		auto dependent = Module("homebrew_dependent");
		dependent.dependencies.push_back({
			"homebrew_base", false,
			WumsDependencyMatch::Any, std::nullopt});
		std::vector definitions{
			Definition(std::move(dependent), 0x02002000),
			Definition(std::move(base), 0x02001000),
		};
		std::string error;
		CHECK(runtime.Load(std::move(definitions), error));
		CHECK(runtime.LoadOrder() ==
			std::vector<std::string>({
				"homebrew_base", "homebrew_dependent"}));
		CHECK(registry->ProviderCount() == 2);
		CHECK(runtime.OnApplicationStarts(error));
		runtime.OnApplicationRequestsExit();
		runtime.OnApplicationEnds();
		CHECK(runtime.Unload(error));
		CHECK(runtime.Size() == 0);
		CHECK(registry->ProviderCount() == 0);
		CHECK(log->unloaded.size() == 2);
		CHECK(log->unloaded[0].starts_with("homebrew_dependent"));
		CHECK(log->unloaded[1].starts_with("homebrew_base"));

		log->failLink = "homebrew_dependent";
		auto rollbackBase = Module("homebrew_base");
		auto rollbackDependent = Module("homebrew_dependent");
		rollbackDependent.dependencies.push_back({
			"homebrew_base", false,
			WumsDependencyMatch::Any, std::nullopt});
		std::vector rollbackDefinitions{
			Definition(std::move(rollbackDependent), 0x02004000),
			Definition(std::move(rollbackBase), 0x02003000),
		};
		CHECK(!runtime.Load(std::move(rollbackDefinitions), error));
		CHECK(error.find("injected WUMS link failure") != std::string::npos);
		CHECK(runtime.Size() == 0);
		CHECK(registry->ProviderCount() == 0);

		log->failLink.reset();
		log->failHook = std::pair{
			std::string("homebrew_dependent"),
			WumsHookType::ApplicationStarts};
		auto startBase = Module("homebrew_base");
		auto startDependent = Module("homebrew_dependent");
		startDependent.dependencies.push_back({
			"homebrew_base", false,
			WumsDependencyMatch::Any, std::nullopt});
		std::vector startDefinitions{
			Definition(std::move(startDependent), 0x02006000),
			Definition(std::move(startBase), 0x02005000),
		};
		CHECK(runtime.Load(std::move(startDefinitions), error));
		CHECK(!runtime.OnApplicationStarts(error));
		CHECK(error.find("injected WUMS hook failure") != std::string::npos);
		CHECK(std::ranges::find(log->hooks,
			std::pair{std::string("homebrew_base"),
				WumsHookType::ApplicationEnds}) != log->hooks.end());
		log->failHook.reset();
		CHECK(runtime.Unload(error));
	}
}

std::shared_ptr<IWumsModuleLoader> CreateRplWumsModuleLoader()
{
	return {};
}

std::shared_ptr<IWumsRuntimeServices> CreateRplWumsRuntimeServices()
{
	return {};
}

std::shared_ptr<IWupsPatchPlatform> CreateCemuWupsPatchPlatform()
{
	return {};
}

int main()
{
	TestRegistryCollisionKindsAndLifetime();
	TestPpcRelocationBranches();
	TestPatchApplyConflictRollbackAndDynamicEvents();
	TestFunctionPatcherFacadeIncrementalDescriptors();
	TestWumsParserAndDependencyGraph();
	TestWumsRuntimeOrderingRollbackAndUnload();
	std::cout << "WUPS compatibility tests passed\n";
	return 0;
}
