#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsRuntime.h"
#include "Cafe/HW/Espresso/WupsBackendAbi.h"
#include "Cafe/HW/Espresso/WupsBackendManagement.h"
#include "Cemu/Logging/CemuLogging.h"

#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <random>
#include <unordered_map>

uint64 s_loggingFlagMask = 1ULL << static_cast<uint64>(LogType::Force);
bool cemuLog_log(LogType, std::string_view) { return true; }
bool cemuLog_log(LogType, std::u8string_view) { return true; }

namespace
{
	[[noreturn]] void Failed(const char* expression, int line)
	{
		std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
		std::abort();
	}
#define CHECK(value) do { if (!(value)) Failed(#value, __LINE__); } while (false)

	class FakePlatform final : public IWupsPlatform
	{
	public:
		bool ValidateGuestRange(std::uint32_t address, std::uint32_t size,
			WupsGuestAccess) const override
		{
			return address >= 0x1000 && static_cast<std::uint64_t>(address) + size <= 0x20000;
		}
		bool ReadGuest(std::uint32_t address, std::span<std::byte> output) const override
		{
			if (!ValidateGuestRange(address, output.size(), WupsGuestAccess::Read)) return false;
			std::ranges::copy(std::span(memory).subspan(address, output.size()), output.begin());
			return true;
		}
		bool WriteGuest(std::uint32_t address, std::span<const std::byte> input) override
		{
			if (failWriteAddress == address) return false;
			if (!ValidateGuestRange(address, input.size(), WupsGuestAccess::Write)) return false;
			std::ranges::copy(input, memory.begin() + address);
			return true;
		}
		std::optional<std::uint32_t> AllocateGuestData(WupsOwnerToken,
			std::uint32_t size, std::uint32_t alignment, std::string&) override
		{
			nextData = (nextData + alignment - 1) & ~(alignment - 1);
			const auto result = nextData; nextData += size; return result;
		}
		void FreeGuestData(WupsOwnerToken, std::uint32_t) override { ++guestFrees; }
		std::optional<std::uint32_t> RegisterFunction(WupsOwnerToken,
			std::string_view, std::string_view, WupsHostExportHandler handler,
			std::string&) override
		{
			std::uint32_t address;
			{
				std::lock_guard lock(functionMutex);
				address = nextFunction += 4;
				functions.emplace(address, std::move(handler));
			}
			if (registerHook)
			{
				auto hook = std::exchange(registerHook, {});
				registrationInProgress = true;
				hook();
				registrationInProgress = false;
			}
			return address;
		}
		void ReleaseOwnerExports(WupsOwnerToken) override
		{
			if (registrationInProgress) prematureExportRelease = true;
			++exportReleases;
		}
		std::uint64_t CurrentGuestThreadId() const override { return 7; }
		bool QueueCpuTask(WupsOwnerToken, std::function<void()> task, std::string&) override
		{
			std::lock_guard lock(mutex); tasks.push_back(std::move(task)); return true;
		}
		void CancelCpuTasks(WupsOwnerToken) override
		{
			{
				std::lock_guard lock(mutex); tasks.clear();
			}
			++taskCancellations;
		}
		bool SupportsMappedMemory() const override { return supportsMappedMemory; }
		bool SupportsOwnerScopedHeapPointers() const override
		{
			return supportsOwnerScopedHeapPointers;
		}
		std::optional<WupsMappedMemoryInfo> AllocateMappedMemory(WupsOwnerToken,
			std::uint32_t size, std::uint32_t alignment, bool writable,
			WupsMappedMemoryPurpose purpose, std::string&) override
		{
			if (allocateHook) allocateHook();
			std::lock_guard lock(mappingMutex);
			WupsMappedMemoryInfo result{nextMapping, nextPhysical,
				size + mappingSizeExtra, alignment, writable, purpose};
			if (!forceOverlap) { nextMapping += 0x10000; nextPhysical += 0x10000; }
			return result;
		}
		bool FreeMappedMemory(WupsOwnerToken, const WupsMappedMemoryInfo&, std::string&) override
		{
			if (freeHook) freeHook();
			++mappingFrees; return !failFree;
		}
		void ShowNotification(WupsOwnerToken, const WupsNotificationModel&) override {}
		void Log(WupsOwnerToken, WupsLogLevel, std::string_view,
			std::string_view, std::string_view) override { ++logs; }

		std::int32_t Dispatch(std::uint32_t address,
			std::span<const std::uint32_t> arguments, std::string& error)
		{
			WupsHostExportHandler handler;
			{
				std::lock_guard lock(functionMutex);
				handler = functions.at(address);
			}
			return handler(arguments, error);
		}
		void RunTasks()
		{
			std::vector<std::function<void()>> copy;
			{ std::lock_guard lock(mutex); copy.swap(tasks); }
			for (auto& task : copy) task();
		}

		std::array<std::byte, 0x20000> memory{};
		std::unordered_map<std::uint32_t, WupsHostExportHandler> functions;
		std::mutex functionMutex;
		std::function<void()> registerHook;
		std::atomic_bool registrationInProgress{};
		std::atomic_bool prematureExportRelease{};
		std::mutex mutex;
		std::vector<std::function<void()>> tasks;
		std::mutex mappingMutex;
		std::function<void()> allocateHook;
		std::function<void()> freeHook;
		std::uint32_t nextData{0x18000}, nextFunction{0x4000};
		std::uint32_t nextMapping{0x8000000}, nextPhysical{0x1000000};
		bool forceOverlap{};
		bool supportsMappedMemory{true};
		bool supportsOwnerScopedHeapPointers{true};
		std::optional<std::uint32_t> failWriteAddress;
		std::uint32_t mappingSizeExtra{};
		std::atomic_bool failFree{};
		std::atomic_size_t mappingFrees{}, guestFrees{}, exportReleases{}, logs{},
			taskCancellations{};
	};

	class FakeFunctionPatcher final : public IWupsFunctionPatcherFacade
	{
	public:
		std::uint32_t ApiVersion() const override { return 2; }
		WupsServiceStatus AddPatch(WupsOwnerToken, std::uint32_t, bool,
			std::uint32_t& handle, bool& applied, std::string&) override
		{
			++adds;
			handle = 0x12345678;
			applied = true;
			live = true;
			return WupsServiceStatus::Success;
		}
		WupsServiceStatus RemovePatch(WupsOwnerToken, std::uint32_t handle,
			std::string&) override
		{
			CHECK(handle == 0x12345678);
			++removes;
			live = false;
			return WupsServiceStatus::Success;
		}
		WupsServiceStatus IsPatchApplied(WupsOwnerToken, std::uint32_t,
			bool& applied, std::string&) const override
		{
			applied = true;
			return WupsServiceStatus::Success;
		}
		void ReleaseOwner(WupsOwnerToken) override { live = false; }

		std::size_t adds{}, removes{};
		bool live{};
	};

	std::uint32_t ReadGuestU32(const FakePlatform& platform,
		std::uint32_t address)
	{
		return (std::to_integer<std::uint32_t>(platform.memory[address]) << 24) |
			(std::to_integer<std::uint32_t>(platform.memory[address + 1]) << 16) |
			(std::to_integer<std::uint32_t>(platform.memory[address + 2]) << 8) |
			std::to_integer<std::uint32_t>(platform.memory[address + 3]);
	}

	CemodPackage Package(std::string id, std::vector<std::string> modules)
	{
		CemodPackage package;
		package.manifest.packageVersion = 2;
		package.manifest.apiVersion = 2;
		package.manifest.executionMode = CemodExecutionMode::TrustedNative;
		package.manifest.payload.format = CemodPayloadFormat::Wups;
		package.manifest.payload.path = "plugin.wps";
		package.manifest.modId = std::move(id);
		package.manifest.nativePermissions.modules = std::move(modules);
		return package;
	}

	void TestStorageAndOwnerGeneration(const std::filesystem::path& root)
	{
		auto platform = std::make_shared<FakePlatform>();
		AromaRuntimeOptions options{.storageRoot = root, .platform = platform,
			.maximumStorageBytes = 64, .maximumStorageItems = 2};
		AromaCompatibilityRuntime runtime(options);
		auto package = Package("storage", {"homebrew_wupsbackend"});
		WupsMetadata metadata; metadata.name = "Storage"; metadata.storageId = "safe-id";
		std::string error; const WupsOwnerToken owner{1, 1};
		CHECK(runtime.RegisterOwner(package, metadata, owner, error));
		WupsStorageValue value{WupsStorageValueType::String,
			{std::byte{'o'}, std::byte{'k'}}};
		CHECK(runtime.StorageStore(owner, 0, "value", value) == WupsServiceStatus::Success);
		CHECK(runtime.StorageStore(owner, 0, "../escape", value) == WupsServiceStatus::InvalidArgument);
		std::uint32_t child{};
		CHECK(runtime.StorageCreateSubItem(owner, 0, "child", child) == WupsServiceStatus::Success);
		CHECK(runtime.StorageStore(owner, 0, "third", value) == WupsServiceStatus::LimitExceeded);
		CHECK(runtime.StorageSave(owner, true, error) == WupsServiceStatus::Success);
		auto reloadTask = std::async(std::launch::async, [&] {
			std::string reloadError;
			return runtime.StorageForceReload(owner, reloadError);
		});
		CHECK(reloadTask.wait_for(std::chrono::seconds(2)) ==
			std::future_status::ready);
		CHECK(reloadTask.get() == WupsServiceStatus::Success);
		WupsStorageValue loaded;
		CHECK(runtime.StorageGet(owner, 0, "value", WupsStorageValueType::String, loaded) == WupsServiceStatus::Success);
		CHECK(loaded.bytes == value.bytes);
		CHECK(runtime.ReleaseOwnerResources(
			owner.owner, owner.generation, error));
		CHECK(!runtime.IsOwnerActive(owner));
		CHECK(runtime.StorageGet(owner, 0, "value", WupsStorageValueType::String, loaded) == WupsServiceStatus::StaleGeneration);
		std::filesystem::path storageFile;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
			if (entry.path().extension() == ".wups-storage") storageFile = entry.path();
		CHECK(!storageFile.empty());
		{ std::ofstream stream(storageFile, std::ios::binary | std::ios::trunc); stream << "corrupt"; }
		AromaCompatibilityRuntime reload(options);
		CHECK(reload.RegisterOwner(package, metadata, {1, 2}, error));
		CHECK(reload.StorageGet({1, 2}, 0, "value", WupsStorageValueType::String,
			loaded) == WupsServiceStatus::CorruptData);
	}

	void TestBackendDescriptorAndPendingPlan()
	{
		const auto descriptors = WupsBackendExportDescriptors();
		CHECK(descriptors.size() == 18);
		std::set<std::string_view> names;
		for (const auto& descriptor : descriptors)
		{
			CHECK(descriptor.kind == WupsSymbolKind::Function);
			CHECK(names.insert(descriptor.name).second);
			CHECK(FindWupsBackendExport(descriptor.name) == &descriptor);
		}

		WupsBackendManagementRuntime management;
		auto first = Package("dynamic.first", {"homebrew_wupsbackend"});
		first.manifest.packageVersion = 3;
		first.manifest.nativePermissions.pluginManagement = true;
		first.payload = {std::byte{1}, std::byte{2}};
		first.wups.emplace();
		first.wups->metadata.name = "First";
		first.wups->metadata.author = "Author";
		auto duplicate = first;
		duplicate.manifest.modId = "dynamic.duplicate";
		duplicate.payload = {std::byte{3}};
		auto second = first;
		second.manifest.modId = "dynamic.second";
		second.wups->metadata.name = "Second";
		const auto firstHandle = management.CreatePluginData(std::move(first));
		const auto duplicateHandle = management.CreatePluginData(std::move(duplicate));
		const auto secondHandle = management.CreatePluginData(std::move(second));
		CHECK(firstHandle && duplicateHandle && secondHandle);
		const std::array handles{*firstHandle, 0xffffffffU,
			*duplicateHandle, *secondHandle};
		const WupsProcessKey key{2, 0x0005000012345678ULL};
		CHECK(management.ScheduleNextLaunch(key, handles));
		CHECK(management.HasPendingPlan(key));
		const std::array deleted{*firstHandle, *duplicateHandle, *secondHandle};
		management.DeletePluginData(deleted);
		CHECK(!management.FindPluginData(*firstHandle));
		auto plan = management.ConsumePendingPlan(key);
		CHECK(plan && plan->size() == 2);
		CHECK((*plan)[0].wups->metadata.name == "First");
		CHECK((*plan)[1].wups->metadata.name == "Second");
		CHECK(!management.HasPendingPlan(key));
		CHECK(!management.ConsumePendingPlan(key));
	}

	void TestBackendExportsAndVersionDispatch(const std::filesystem::path& root)
	{
		auto platform = std::make_shared<FakePlatform>();
		auto management = std::make_shared<WupsBackendManagementRuntime>();
		AromaRuntimeOptions options{.storageRoot = root, .platform = platform,
			.backendManagement = management};
		AromaCompatibilityRuntime runtime(options);
		auto package = Package("backend", {"homebrew_wupsbackend"});
		package.manifest.packageVersion = 3;
		package.manifest.nativePermissions.pluginManagement = true;
		package.targetTitleId = 0x0005000012345678ULL;
		WupsMetadata metadata; metadata.name = "Backend";
		std::string error;
		const WupsOwnerToken owner{90, 1};
		CHECK(runtime.RegisterOwner(package, metadata, owner, error));
		std::uint32_t apiVersionExport{};
		for (const auto& descriptor : WupsBackendExportDescriptors())
		{
			const auto address = runtime.ResolveRuntimeModuleExport(owner,
				"homebrew_wupsbackend", descriptor.name,
				WupsSymbolKind::Function, error);
			CHECK(address);
			if (descriptor.id == WupsBackendExportId::GetApiVersion)
				apiVersionExport = *address;
		}
		CHECK(apiVersionExport != 0);
		const std::array<std::uint32_t, 1> versionArguments{0x1100};
		CHECK(platform->Dispatch(apiVersionExport, versionArguments, error) == 0);
		CHECK(ReadGuestU32(*platform, 0x1100) == kWupsBackendApiVersion);

		auto denied = Package("backend-denied", {"homebrew_wupsbackend"});
		CHECK(runtime.RegisterOwner(denied, metadata, {91, 1}, error));
		CHECK(!runtime.ResolveRuntimeModuleExport({91, 1},
			"homebrew_wupsbackend", "WUPSGetAPIVersion",
			WupsSymbolKind::Function, error));
		CHECK(error.find("plugin_management") != std::string::npos);
	}

	void TestPermissionsMappingAndDispatch(const std::filesystem::path& root)
	{
		auto platform = std::make_shared<FakePlatform>();
		AromaRuntimeOptions options{.storageRoot = root, .platform = platform};
		AromaCompatibilityRuntime runtime(options);
		WupsMetadata metadata; metadata.name = "Services"; metadata.abiVersion = {0, 9, 1};
		std::string error;
		auto denied = Package("denied", {});
		CHECK(runtime.RegisterOwner(denied, metadata, {2, 1}, error));
		WupsMappedMemoryInfo allocation;
		CHECK(runtime.MappedMemoryAllocate({2, 1}, 4096, 4096, true,
			WupsMappedMemoryPurpose::Cpu, allocation, error) == WupsServiceStatus::PermissionDenied);

		auto allowed = Package("allowed", {"homebrew_memorymapping", "homebrew_logging"});
		allowed.manifest.nativePermissions.mappedMemory = true;
		CHECK(runtime.RegisterOwner(allowed, metadata, {3, 1}, error));
		CHECK(runtime.MappedMemoryAllocate({3, 1}, 4096, 4096, true,
			WupsMappedMemoryPurpose::Cpu, allocation, error) == WupsServiceStatus::Success);
		platform->forceOverlap = true;
		WupsMappedMemoryInfo second;
		CHECK(runtime.MappedMemoryAllocate({3, 1}, 4096, 4096, true,
			WupsMappedMemoryPurpose::Cpu, second, error) == WupsServiceStatus::Success);
		WupsMappedMemoryInfo overlap;
		CHECK(runtime.MappedMemoryAllocate({3, 1}, 4096, 4096, true,
			WupsMappedMemoryPurpose::Cpu, overlap, error) == WupsServiceStatus::Conflict);
		const auto address = runtime.ResolveImport(allowed, metadata, 3, 1,
			"homebrew_logging", "WUMSLogWrite", WupsSymbolKind::Function, error);
		CHECK(address);
		const std::array text{std::byte{'h'}, std::byte{'i'}};
		CHECK(platform->WriteGuest(0x1000, text));
		const std::array args{0x1000U, 2U};
		CHECK(platform->Dispatch(*address, args, error) == 0);
		CHECK(platform->logs == 1);
		CHECK(runtime.ReleaseOwnerResources(3, 1, error));
		CHECK(platform->mappingFrees >= 3);

		auto unsupportedPlatform = std::make_shared<FakePlatform>();
		unsupportedPlatform->supportsMappedMemory = false;
		AromaRuntimeOptions unsupportedOptions{
			.storageRoot = root, .platform = unsupportedPlatform};
		AromaCompatibilityRuntime unsupportedRuntime(unsupportedOptions);
		CHECK(!unsupportedRuntime.ResolveImport(allowed, metadata, 30, 1,
			"homebrew_memorymapping", "MEMAllocFromMappedMemory",
			WupsSymbolKind::Data, error));
		CHECK(error.find("no safe guest effective/physical") !=
			std::string::npos);

		auto unscopedHeapPlatform = std::make_shared<FakePlatform>();
		unscopedHeapPlatform->supportsOwnerScopedHeapPointers = false;
		AromaCompatibilityRuntime unscopedHeapRuntime({
			.storageRoot = root, .platform = unscopedHeapPlatform});
		CHECK(!unscopedHeapRuntime.ResolveImport(allowed, metadata, 31, 1,
			"homebrew_logging", "WUMSLogWrite", WupsSymbolKind::Function,
			error));
		CHECK(error.find("heap-taking ABI is explicitly unsupported") !=
			std::string::npos);
	}

	void TestFunctionPatcherAbiOutputs(const std::filesystem::path& root)
	{
		auto platform = std::make_shared<FakePlatform>();
		auto patcher = std::make_shared<FakeFunctionPatcher>();
		AromaCompatibilityRuntime runtime({
			.storageRoot = root, .platform = platform, .functionPatcher = patcher});
		auto package = Package("function-patcher", {"homebrew_functionpatcher"});
		package.manifest.nativePermissions.functionPatching = true;
		WupsMetadata metadata;
		metadata.name = "Function patcher ABI";
		const WupsOwnerToken owner{32, 1};
		std::string error;
		const auto add = runtime.ResolveImport(package, metadata,
			owner.owner, owner.generation, "homebrew_functionpatcher",
			"FPAddFunctionPatch", WupsSymbolKind::Function, error);
		const auto isPatched = runtime.ResolveImport(package, metadata,
			owner.owner, owner.generation, "homebrew_functionpatcher",
			"FPIsFunctionPatched", WupsSymbolKind::Function, error);
		CHECK(add && isPatched);

		platform->memory[0x1204] = std::byte{0xa5};
		platform->memory[0x1205] = std::byte{0xa5};
		platform->memory[0x1206] = std::byte{0xa5};
		platform->memory[0x1207] = std::byte{0xa5};
		const std::array addArguments{0x1100U, 0x1200U, 0x1204U};
		CHECK(platform->Dispatch(*add, addArguments, error) == 0);
		CHECK(ReadGuestU32(*platform, 0x1200) == 0x12345678);
		CHECK(platform->memory[0x1204] == std::byte{1});
		CHECK(platform->memory[0x1205] == std::byte{0xa5});
		CHECK(platform->memory[0x1206] == std::byte{0xa5});
		CHECK(platform->memory[0x1207] == std::byte{0xa5});

		const std::array nullOutputs{0x1100U, 0U, 0U};
		CHECK(platform->Dispatch(*add, nullOutputs, error) == 0);
		CHECK(patcher->adds == 2);
		const std::array invalidOutput{0x1100U, 0x20000U, 0x1204U};
		CHECK(platform->Dispatch(*add, invalidOutput, error) == -0x10);
		CHECK(patcher->adds == 2);

		platform->failWriteAddress = 0x1210;
		const std::array failedWrite{0x1100U, 0x1210U, 0x1214U};
		CHECK(platform->Dispatch(*add, failedWrite, error) == -0x10);
		CHECK(patcher->adds == 3);
		CHECK(patcher->removes == 1);
		CHECK(!patcher->live);
		platform->failWriteAddress.reset();

		platform->memory[0x1221] = std::byte{0xa5};
		platform->memory[0x1222] = std::byte{0xa5};
		platform->memory[0x1223] = std::byte{0xa5};
		const std::array isArguments{0x12345678U, 0x1220U};
		CHECK(platform->Dispatch(*isPatched, isArguments, error) == 0);
		CHECK(platform->memory[0x1220] == std::byte{1});
		CHECK(platform->memory[0x1221] == std::byte{0xa5});
		CHECK(platform->memory[0x1222] == std::byte{0xa5});
		CHECK(platform->memory[0x1223] == std::byte{0xa5});
	}

	void TestContentTraversalAndCleanup(const std::filesystem::path& root)
	{
		const auto content = root / "content";
		std::filesystem::create_directories(content / "safe");
		auto platform = std::make_shared<FakePlatform>();
		AromaRuntimeOptions options{.storageRoot = root, .contentRoots = {content}, .platform = platform};
		AromaCompatibilityRuntime runtime(options);
		auto package = Package("content", {"homebrew_content_redirection"});
		package.manifest.nativePermissions.filesystemRead = true;
		package.manifest.nativePermissions.contentRedirection = true;
		WupsMetadata metadata; metadata.name = "Content";
		std::string error; CHECK(runtime.RegisterOwner(package, metadata, {4, 1}, error));
		std::uint32_t handle{};
		CHECK(runtime.ContentRedirectAdd({4, 1}, "/vol/content", content / "safe", 1,
			false, handle, error) == WupsServiceStatus::Success);
		CHECK(runtime.ResolveContentPath({4, 1}, "/vol/content/file.bin", false, error));
		CHECK(!runtime.ResolveContentPath({4, 1}, "/vol/content/../escape", false, error));
		std::error_code code;
		std::filesystem::create_directory_symlink(root, content / "safe" / "link", code);
		if (!code)
			CHECK(!runtime.ResolveContentPath({4, 1}, "/vol/content/link/escape", false, error));
		CHECK(runtime.ReleaseOwnerResources(4, 1, error));
		CHECK(runtime.ResourceCounts({4, 1}).contentRedirects == 0);
	}

	void TestQueuedCallbackCancelledAtUnload(const std::filesystem::path& root)
	{
		auto platform = std::make_shared<FakePlatform>();
		AromaRuntimeOptions options{.storageRoot = root, .platform = platform};
		AromaCompatibilityRuntime runtime(options);
		auto package = Package("callback", {"homebrew_notifications"});
		package.manifest.nativePermissions.notifications = true;
		WupsMetadata metadata; metadata.name = "Callback";
		std::string error; const WupsOwnerToken owner{5, 1};
		CHECK(runtime.RegisterOwner(package, metadata, owner, error));
		std::size_t invocations{};
		CHECK(runtime.BindGuestInvoker(owner.owner, owner.generation,
			[&](std::uint32_t, std::span<const std::uint32_t>, std::uint32_t&,
				std::string&) { ++invocations; return true; }, error));
		WupsNotificationModel notification;
		notification.text = "done"; notification.dynamic = true;
		notification.callback = 0x1200; notification.duration = std::chrono::seconds(1);
		std::uint32_t handle{};
		CHECK(runtime.NotificationAdd(owner, notification, handle, error) == WupsServiceStatus::Success);
		CHECK(runtime.NotificationFinish(owner, handle, error) == WupsServiceStatus::Success);
		CHECK(runtime.ResourceCounts(owner).pendingCallbacks == 1);
		CHECK(runtime.ReleaseOwnerResources(
			owner.owner, owner.generation, error));
		platform->RunTasks();
		CHECK(invocations == 0);
		CHECK(!runtime.IsOwnerActive(owner));
	}

	void TestReentrantExportsAndCallbacks(const std::filesystem::path& root)
	{
		auto platform = std::make_shared<FakePlatform>();
		AromaRuntimeOptions options{.storageRoot = root, .platform = platform};
		AromaCompatibilityRuntime runtime(options);
		auto package = Package("reentrant", {"homebrew_wupsbackend",
			"homebrew_logging", "homebrew_notifications"});
		package.manifest.nativePermissions.notifications = true;
		WupsMetadata metadata; metadata.name = "Reentrant";
		std::string error; const WupsOwnerToken owner{6, 1};
		CHECK(runtime.RegisterOwner(package, metadata, owner, error));

		std::optional<std::uint32_t> nestedExport;
		platform->registerHook = [&] {
			std::string nestedError;
			nestedExport = runtime.ResolveImport(package, metadata,
				owner.owner, owner.generation, "homebrew_wupsbackend",
				"WUPSConfigAPI_GetVersion", WupsSymbolKind::Function,
				nestedError);
		};
		auto resolve = std::async(std::launch::async, [&] {
			std::string resolveError;
			return runtime.ResolveImport(package, metadata,
				owner.owner, owner.generation, "homebrew_logging",
				"WUMSLogWrite", WupsSymbolKind::Function, resolveError);
		});
		CHECK(resolve.wait_for(std::chrono::seconds(2)) ==
			std::future_status::ready);
		const auto loggingExport = resolve.get();
		CHECK(loggingExport && nestedExport);

		const std::array text{std::byte{'o'}, std::byte{'k'}};
		CHECK(platform->WriteGuest(0x1000, text));
		std::atomic_size_t invocations{};
		CHECK(runtime.BindGuestInvoker(owner.owner, owner.generation,
			[&](std::uint32_t, std::span<const std::uint32_t>, std::uint32_t&,
				std::string& invokeError) {
				const std::array arguments{0x1000U, 2U};
				if (platform->Dispatch(*loggingExport, arguments, invokeError) != 0)
					return false;
				++invocations;
				return true;
			}, error));
		WupsNotificationModel notification;
		notification.text = "reenter";
		notification.dynamic = true;
		notification.callback = 0x1200;
		notification.duration = std::chrono::seconds(1);
		std::uint32_t handle{};
		CHECK(runtime.NotificationAdd(owner, notification, handle, error) ==
			WupsServiceStatus::Success);
		CHECK(runtime.NotificationFinish(owner, handle, error) ==
			WupsServiceStatus::Success);
		auto run = std::async(std::launch::async, [&] { platform->RunTasks(); });
		CHECK(run.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
		run.get();
		CHECK(invocations == 1);
		CHECK(runtime.ResourceCounts(owner).pendingCallbacks == 0);

		CHECK(runtime.NotificationAdd(owner, notification, handle, error) ==
			WupsServiceStatus::Success);
		CHECK(runtime.NotificationFinish(owner, handle, error) ==
			WupsServiceStatus::Success);
		CHECK(runtime.ResourceCounts(owner).pendingCallbacks == 1);
		platform->CancelCpuTasks(owner);
		CHECK(runtime.ResourceCounts(owner).pendingCallbacks == 0);
	}

	void TestRuntimeDestructionWithQueuedCallback(
		const std::filesystem::path& root)
	{
		auto platform = std::make_shared<FakePlatform>();
		std::atomic_size_t invocations{};
		AromaRuntimeOptions options{.storageRoot = root, .platform = platform};
		auto runtime = std::make_unique<AromaCompatibilityRuntime>(options);
		auto package = Package("destroy-callback", {"homebrew_notifications"});
		package.manifest.nativePermissions.notifications = true;
		WupsMetadata metadata; metadata.name = "Destroy callback";
		std::string error; const WupsOwnerToken owner{7, 1};
		CHECK(runtime->RegisterOwner(package, metadata, owner, error));
		std::mutex callbackMutex;
		std::condition_variable callbackCondition;
		bool callbackEntered{};
		bool releaseCallback{};
		CHECK(runtime->BindGuestInvoker(owner.owner, owner.generation,
			[&](std::uint32_t, std::span<const std::uint32_t>,
				std::uint32_t&, std::string&) {
				std::unique_lock lock(callbackMutex);
				callbackEntered = true;
				callbackCondition.notify_all();
				callbackCondition.wait(lock, [&] { return releaseCallback; });
				++invocations;
				return true;
			}, error));
		WupsNotificationModel notification;
		notification.text = "destroy";
		notification.dynamic = true;
		notification.callback = 0x1200;
		notification.duration = std::chrono::seconds(1);
		std::uint32_t handle{};
		CHECK(runtime->NotificationAdd(owner, notification, handle, error) ==
			WupsServiceStatus::Success);
		CHECK(runtime->NotificationFinish(owner, handle, error) ==
			WupsServiceStatus::Success);
		CHECK(runtime->ResourceCounts(owner).pendingCallbacks == 1);
		auto run = std::async(std::launch::async, [&] { platform->RunTasks(); });
		{
			std::unique_lock lock(callbackMutex);
			CHECK(callbackCondition.wait_for(lock, std::chrono::seconds(2),
				[&] { return callbackEntered; }));
		}
		auto destroy = std::async(std::launch::async, [&] { runtime.reset(); });
		CHECK(destroy.wait_for(std::chrono::milliseconds(50)) ==
			std::future_status::timeout);
		{
			std::lock_guard lock(callbackMutex);
			releaseCallback = true;
		}
		callbackCondition.notify_all();
		run.get();
		destroy.get();
		CHECK(invocations == 1);
	}

	void TestConcurrentMappingAccounting(const std::filesystem::path& root)
	{
		auto platform = std::make_shared<FakePlatform>();
		AromaRuntimeOptions options{.storageRoot = root, .platform = platform,
			.maximumMappedBytes = 4096};
		AromaCompatibilityRuntime runtime(options);
		auto package = Package("mapping-races", {"homebrew_memorymapping"});
		package.manifest.nativePermissions.mappedMemory = true;
		WupsMetadata metadata; metadata.name = "Mapping races";
		std::string error; const WupsOwnerToken owner{8, 1};
		CHECK(runtime.RegisterOwner(package, metadata, owner, error));
		const auto allocCell = runtime.ResolveImport(package, metadata,
			owner.owner, owner.generation, "homebrew_memorymapping",
			"MEMAllocFromMappedMemory", WupsSymbolKind::Data, error);
		const auto freeCell = runtime.ResolveImport(package, metadata,
			owner.owner, owner.generation, "homebrew_memorymapping",
			"MEMFreeToMappedMemory", WupsSymbolKind::Data, error);
		CHECK(allocCell && freeCell);
		const auto allocExport = ReadGuestU32(*platform, *allocCell);
		const auto freeExport = ReadGuestU32(*platform, *freeCell);

		std::mutex waitMutex;
		std::condition_variable waitCondition;
		bool allocateEntered{};
		bool releaseAllocate{};
		std::atomic_size_t allocateCalls{};
		platform->allocateHook = [&] {
			++allocateCalls;
			std::unique_lock lock(waitMutex);
			allocateEntered = true;
			waitCondition.notify_all();
			waitCondition.wait(lock, [&] { return releaseAllocate; });
		};
		WupsMappedMemoryInfo allocation;
		auto allocate = std::async(std::launch::async, [&] {
			std::string allocateError;
			return runtime.MappedMemoryAllocate(owner, 4096, 4, true,
				WupsMappedMemoryPurpose::Cpu, allocation, allocateError);
		});
		{
			std::unique_lock lock(waitMutex);
			CHECK(waitCondition.wait_for(lock, std::chrono::seconds(2),
				[&] { return allocateEntered; }));
		}
		const std::array allocArguments{4096U};
		CHECK(platform->Dispatch(allocExport, allocArguments, error) == 0);
		CHECK(allocateCalls == 1);
		{
			std::lock_guard lock(waitMutex);
			releaseAllocate = true;
		}
		waitCondition.notify_all();
		CHECK(allocate.get() == WupsServiceStatus::Success);
		platform->allocateHook = {};

		bool freeEntered{};
		bool releaseFree{};
		std::atomic_size_t freeCalls{};
		platform->freeHook = [&] {
			++freeCalls;
			std::unique_lock lock(waitMutex);
			freeEntered = true;
			waitCondition.notify_all();
			waitCondition.wait(lock, [&] { return releaseFree; });
		};
		auto free = std::async(std::launch::async, [&] {
			std::string freeError;
			return runtime.MappedMemoryFree(owner, allocation.address, freeError);
		});
		{
			std::unique_lock lock(waitMutex);
			CHECK(waitCondition.wait_for(lock, std::chrono::seconds(2),
				[&] { return freeEntered; }));
		}
		const std::array freeArguments{allocation.address};
		CHECK(platform->Dispatch(freeExport, freeArguments, error) == 0);
		CHECK(freeCalls == 1);
		{
			std::lock_guard lock(waitMutex);
			releaseFree = true;
		}
		waitCondition.notify_all();
		CHECK(free.get() == WupsServiceStatus::Success);
		platform->freeHook = {};
		CHECK(runtime.ResourceCounts(owner).mappedAllocations == 0);

		platform->mappingSizeExtra = 4096;
		CHECK(runtime.MappedMemoryAllocate(owner, 4096, 4, true,
			WupsMappedMemoryPurpose::Cpu, allocation, error) ==
			WupsServiceStatus::LimitExceeded);
		platform->mappingSizeExtra = 0;
		CHECK(runtime.MappedMemoryAllocate(owner, 4096, 4, true,
			WupsMappedMemoryPurpose::Cpu, allocation, error) ==
			WupsServiceStatus::Success);
		platform->failFree = true;
		CHECK(runtime.MappedMemoryFree(owner, allocation.address, error) ==
			WupsServiceStatus::IoError);
		CHECK(runtime.ResourceCounts(owner).mappedAllocations == 1);
		platform->failFree = false;
		CHECK(runtime.MappedMemoryFree(owner, allocation.address, error) ==
			WupsServiceStatus::Success);
	}

	void TestExportRegistrationCleanupRace(const std::filesystem::path& root)
	{
		auto platform = std::make_shared<FakePlatform>();
		AromaRuntimeOptions options{.storageRoot = root, .platform = platform};
		AromaCompatibilityRuntime runtime(options);
		auto package = Package("registration-race", {"homebrew_logging"});
		WupsMetadata metadata; metadata.name = "Registration race";
		std::string error; const WupsOwnerToken owner{9, 1};
		CHECK(runtime.RegisterOwner(package, metadata, owner, error));
		std::mutex registrationMutex;
		std::condition_variable registrationCondition;
		bool registrationEntered{};
		bool releaseRegistration{};
		platform->registerHook = [&] {
			std::unique_lock lock(registrationMutex);
			registrationEntered = true;
			registrationCondition.notify_all();
			registrationCondition.wait(lock,
				[&] { return releaseRegistration; });
		};
		auto resolve = std::async(std::launch::async, [&] {
			std::string resolveError;
			return runtime.ResolveImport(package, metadata,
				owner.owner, owner.generation, "homebrew_logging",
				"WUMSLogWrite", WupsSymbolKind::Function, resolveError);
		});
		{
			std::unique_lock lock(registrationMutex);
			CHECK(registrationCondition.wait_for(lock, std::chrono::seconds(2),
				[&] { return registrationEntered; }));
		}
		auto release = std::async(std::launch::async, [&] {
			std::string releaseError;
			CHECK(runtime.ReleaseOwnerResources(
				owner.owner, owner.generation, releaseError));
		});
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(2);
		while (platform->taskCancellations == 0 &&
			std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		CHECK(platform->taskCancellations == 1);
		CHECK(platform->exportReleases == 0);
		CHECK(!platform->prematureExportRelease);
		{
			std::lock_guard lock(registrationMutex);
			releaseRegistration = true;
		}
		registrationCondition.notify_all();
		CHECK(resolve.get());
		release.get();
		CHECK(platform->exportReleases == 1);
		CHECK(!platform->prematureExportRelease);
	}
}

std::shared_ptr<IWupsModuleLoader> CreateRplWupsModuleLoader() { return {}; }
std::shared_ptr<IWupsRuntimeServices> CreateRplAromaCompatibilityRuntime()
{
	return {};
}

int main()
{
	const auto root = std::filesystem::temp_directory_path() /
		fmt::format("cemuext-wups-services-{}", std::uint64_t{std::random_device{}()});
	std::filesystem::create_directories(root);
	TestStorageAndOwnerGeneration(root);
	TestPermissionsMappingAndDispatch(root);
	TestFunctionPatcherAbiOutputs(root);
	TestContentTraversalAndCleanup(root);
	TestQueuedCallbackCancelledAtUnload(root);
	TestReentrantExportsAndCallbacks(root);
	TestRuntimeDestructionWithQueuedCallback(root);
	TestConcurrentMappingAccounting(root);
	TestExportRegistrationCleanupRace(root);
	TestBackendDescriptorAndPendingPlan();
	TestBackendExportsAndVersionDispatch(root);
	std::error_code code; std::filesystem::remove_all(root, code);
	std::cout << "WUPS services tests passed\n";
}
