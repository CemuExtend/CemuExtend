#pragma once

#include "Cafe/HW/Espresso/ITrustedPayloadInstance.h"
#include "Cafe/HW/Espresso/ModuleExportRegistry.h"
#include "Cafe/HW/Espresso/WupsBinary.h"
#include "Cafe/HW/Espresso/WupsFunctionPatcher.h"
#include "Cafe/HW/Espresso/WupsServices.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct RPLModule;

enum class WupsPluginState : std::uint8_t
{
	Installed,
	Mapped,
	Relocated,
	Initialized,
	Active,
	Deinitialized,
	Unloading,
	Unloaded,
	Failed,
};

enum class WupsProcessKind : std::uint8_t
{
	RootRpx,
	WiiUMenu,
	Game,
};

struct WupsHookInvocation
{
	std::vector<std::uint32_t> argumentWords;
	bool requireZeroResult{};
	// Logs a non-zero result instead of failing the hook, for optional
	// subsystems whose absence must not abort plugin start-up.
	bool reportNonZeroResult{};
	bool skip{};
};

class IWupsRuntimeServices
{
public:
	virtual ~IWupsRuntimeServices() = default;
	[[nodiscard]] virtual bool BeginOwner(const CemodPackage&,
		const WupsMetadata&, std::uint64_t, std::uint32_t, std::string&)
	{
		return true;
	}

	[[nodiscard]] virtual std::optional<std::uint32_t> ResolveImport(
		const CemodPackage& package, const WupsMetadata& metadata,
		std::uint64_t owner, std::uint32_t generation,
		std::string_view moduleName, std::string_view symbolName,
		WupsSymbolKind kind, std::string& error) = 0;
	[[nodiscard]] virtual bool PrepareHookInvocation(const CemodPackage& package,
		const WupsMetadata& metadata, std::uint64_t owner, std::uint32_t generation,
		WupsHookType type, WupsHookInvocation& invocation, std::string& error) = 0;
	[[nodiscard]] virtual bool ActivatePlugin(const CemodPackage& package,
		const WupsMetadata& metadata, std::uint64_t owner,
		std::uint32_t generation, std::span<const WupsPatchRequest> patches,
		std::string& error) = 0;
	[[nodiscard]] virtual bool DeactivatePlugin(std::uint64_t,
		std::uint32_t, std::string&)
	{
		return true;
	}
	[[nodiscard]] virtual bool ReleaseOwnerResources(std::uint64_t owner,
		std::uint32_t generation, std::string& error) = 0;
	[[nodiscard]] virtual bool IsProcessInScope(const CemodPackage& package,
		std::string& reason) const = 0;
	[[nodiscard]] virtual bool BindGuestInvoker(std::uint64_t,
		std::uint32_t, WupsGuestInvoker, std::string&)
	{
		return true;
	}
};

class IWupsModuleLoader
{
public:
	virtual ~IWupsModuleLoader() = default;

	[[nodiscard]] virtual bool Map(std::span<const std::byte> image,
		std::string_view moduleName, std::uint64_t owner, std::uint32_t generation,
		const CemodPackage& package, const WupsMetadata& metadata,
		const std::shared_ptr<IWupsRuntimeServices>& services,
		RPLModule*& module, std::uint64_t& lifetimeId, std::string& error) = 0;
	[[nodiscard]] virtual bool Relocate(RPLModule* module, std::uint64_t lifetimeId,
		std::string& error) = 0;
	// aggregateByReference is set for WUPS hook invocations, whose argument words
	// form a single wups_loader_*_args_t struct that devkitPPC passes by address.
	[[nodiscard]] virtual bool Invoke(RPLModule* module, std::uint64_t lifetimeId,
		std::uint32_t targetVirtualAddress, std::span<const std::uint32_t> argumentWords,
		std::uint32_t& result, std::string& error,
		bool aggregateByReference = false) = 0;
	[[nodiscard]] virtual bool ResolveAddress(RPLModule* module,
		std::uint64_t lifetimeId, std::uint32_t virtualAddress,
		std::uint32_t size, WupsSymbolKind kind,
		std::uint32_t& mappedAddress, std::string& error) = 0;
	[[nodiscard]] virtual bool Unload(RPLModule* module, std::uint64_t lifetimeId,
		std::string& error) = 0;
};

// Task 2 owns lifecycle and import routing. Task 3/4 providers derive from this
// class or replace IWupsRuntimeServices; unavailable APIs fail explicitly.
class AromaCompatibilityRuntime final : public IWupsRuntimeServices
{
public:
	explicit AromaCompatibilityRuntime(WupsProcessKind process = WupsProcessKind::Game);
	explicit AromaCompatibilityRuntime(AromaRuntimeOptions options,
		WupsProcessKind process = WupsProcessKind::Game);
	~AromaCompatibilityRuntime() override;

	void SetCurrentProcess(WupsProcessKind process);
	[[nodiscard]] WupsProcessKind CurrentProcess() const;

	[[nodiscard]] std::optional<std::uint32_t> ResolveImport(
		const CemodPackage& package, const WupsMetadata& metadata,
		std::uint64_t owner, std::uint32_t generation,
		std::string_view moduleName, std::string_view symbolName,
		WupsSymbolKind kind, std::string& error) override;
	[[nodiscard]] bool PrepareHookInvocation(const CemodPackage& package,
		const WupsMetadata& metadata, std::uint64_t owner, std::uint32_t generation,
		WupsHookType type, WupsHookInvocation& invocation, std::string& error) override;
	[[nodiscard]] bool ActivatePlugin(const CemodPackage& package,
		const WupsMetadata& metadata, std::uint64_t owner,
		std::uint32_t generation, std::span<const WupsPatchRequest> patches,
		std::string& error) override;
	[[nodiscard]] bool DeactivatePlugin(std::uint64_t owner,
		std::uint32_t generation, std::string& error) override;
	[[nodiscard]] bool ReleaseOwnerResources(std::uint64_t owner,
		std::uint32_t generation, std::string& error) override;
	[[nodiscard]] bool IsProcessInScope(const CemodPackage& package,
		std::string& reason) const override;
	[[nodiscard]] bool BindGuestInvoker(std::uint64_t owner,
		std::uint32_t generation, WupsGuestInvoker invoker,
		std::string& error) override;
	[[nodiscard]] bool BeginOwner(const CemodPackage& package,
		const WupsMetadata& metadata, std::uint64_t owner,
		std::uint32_t generation, std::string& error) override;

	[[nodiscard]] bool RegisterOwner(const CemodPackage& package,
		const WupsMetadata& metadata, WupsOwnerToken owner, std::string& error);
	[[nodiscard]] bool IsOwnerActive(WupsOwnerToken owner) const;

	[[nodiscard]] WupsServiceStatus StorageCreateSubItem(WupsOwnerToken owner,
		std::uint32_t parent, std::string_view key, std::uint32_t& handle);
	[[nodiscard]] WupsServiceStatus StorageGetSubItem(WupsOwnerToken owner,
		std::uint32_t parent, std::string_view key, std::uint32_t& handle) const;
	[[nodiscard]] WupsServiceStatus StorageStore(WupsOwnerToken owner,
		std::uint32_t parent, std::string_view key, const WupsStorageValue& value);
	[[nodiscard]] WupsServiceStatus StorageGet(WupsOwnerToken owner,
		std::uint32_t parent, std::string_view key, WupsStorageValueType type,
		WupsStorageValue& value) const;
	[[nodiscard]] WupsServiceStatus StorageDelete(WupsOwnerToken owner,
		std::uint32_t parent, std::string_view key);
	[[nodiscard]] WupsServiceStatus StorageSave(WupsOwnerToken owner, bool force,
		std::string& error);
	[[nodiscard]] WupsServiceStatus StorageForceReload(WupsOwnerToken owner,
		std::string& error);
	[[nodiscard]] WupsServiceStatus StorageWipe(WupsOwnerToken owner,
		std::string& error);

	[[nodiscard]] WupsServiceStatus ConfigRegisterCallbacks(WupsOwnerToken owner,
		std::string_view name, std::uint32_t openCallback,
		std::uint32_t closeCallback);
	[[nodiscard]] WupsServiceStatus ConfigCreateCategory(WupsOwnerToken owner,
		std::string_view name, std::uint32_t& handle);
	[[nodiscard]] WupsServiceStatus ConfigCreateItem(WupsOwnerToken owner,
		WupsConfigItemModel item, std::uint32_t& handle);
	[[nodiscard]] WupsServiceStatus ConfigAddCategory(WupsOwnerToken owner,
		std::uint32_t parent, std::uint32_t child);
	[[nodiscard]] WupsServiceStatus ConfigAddItem(WupsOwnerToken owner,
		std::uint32_t category, std::uint32_t item);
	[[nodiscard]] WupsServiceStatus ConfigDestroyCategory(WupsOwnerToken owner,
		std::uint32_t handle);
	[[nodiscard]] WupsServiceStatus ConfigDestroyItem(WupsOwnerToken owner,
		std::uint32_t handle);
	[[nodiscard]] WupsServiceStatus ConfigOpen(WupsOwnerToken owner,
		std::string& error);
	[[nodiscard]] WupsServiceStatus ConfigClose(WupsOwnerToken owner,
		std::string& error);
	[[nodiscard]] std::optional<WupsConfigModel> ConfigSnapshot(
		WupsOwnerToken owner) const;

	[[nodiscard]] WupsServiceStatus ButtonComboCreate(WupsOwnerToken owner,
		const WupsButtonComboDefinition& definition, std::uint32_t& handle,
		WupsButtonComboStatus& status);
	[[nodiscard]] WupsServiceStatus ButtonComboUpdate(WupsOwnerToken owner,
		std::uint32_t handle, const WupsButtonComboDefinition& definition,
		WupsButtonComboStatus& status);
	[[nodiscard]] WupsServiceStatus ButtonComboRemove(WupsOwnerToken owner,
		std::uint32_t handle);
	[[nodiscard]] WupsServiceStatus ButtonComboGet(WupsOwnerToken owner,
		std::uint32_t handle, WupsButtonComboDefinition& definition,
		WupsButtonComboStatus& status) const;
	[[nodiscard]] WupsServiceStatus ButtonComboCheckAvailable(WupsOwnerToken owner,
		const WupsButtonComboDefinition& definition,
		WupsButtonComboStatus& status) const;
	void SubmitButtonSample(const WupsButtonSample& sample);

	[[nodiscard]] WupsServiceStatus ReentGet(WupsOwnerToken owner,
		std::uint32_t pluginId, std::uint32_t& context) const;
	[[nodiscard]] WupsServiceStatus ReentRegister(WupsOwnerToken owner,
		std::uint32_t pluginId, std::uint32_t context,
		std::uint32_t cleanupCallback);
	[[nodiscard]] WupsServiceStatus ReentUnregisterThread(WupsOwnerToken owner,
		std::uint64_t threadId, std::string& error);

	[[nodiscard]] WupsServiceStatus MappedMemoryAllocate(WupsOwnerToken owner,
		std::uint32_t size, std::uint32_t alignment, bool writable,
		WupsMappedMemoryPurpose purpose, WupsMappedMemoryInfo& allocation,
		std::string& error);
	[[nodiscard]] WupsServiceStatus MappedMemoryFree(WupsOwnerToken owner,
		std::uint32_t address, std::string& error);
	[[nodiscard]] WupsServiceStatus MappedMemoryEffectiveToPhysical(
		WupsOwnerToken owner, std::uint32_t effective,
		std::uint32_t& physical) const;
	[[nodiscard]] WupsServiceStatus MappedMemoryPhysicalToEffective(
		WupsOwnerToken owner, std::uint32_t physical,
		std::uint32_t& effective) const;

	[[nodiscard]] WupsServiceStatus NotificationAdd(WupsOwnerToken owner,
		WupsNotificationModel notification, std::uint32_t& handle,
		std::string& error);
	[[nodiscard]] WupsServiceStatus NotificationUpdate(WupsOwnerToken owner,
		std::uint32_t handle, std::string_view text, std::string& error);
	[[nodiscard]] WupsServiceStatus NotificationFinish(WupsOwnerToken owner,
		std::uint32_t handle, std::string& error);
	[[nodiscard]] std::vector<WupsNotificationModel> NotificationSnapshot(
		WupsOwnerToken owner) const;

	[[nodiscard]] WupsServiceStatus Log(WupsOwnerToken owner, WupsLogLevel level,
		std::string_view moduleName, std::string_view source,
		std::string_view message);

	[[nodiscard]] WupsServiceStatus ContentRedirectAdd(WupsOwnerToken owner,
		std::string_view virtualPath, const std::filesystem::path& sourcePath,
		std::int32_t priority, bool writable, std::uint32_t& handle,
		std::string& error);
	[[nodiscard]] WupsServiceStatus ContentRedirectRemove(WupsOwnerToken owner,
		std::uint32_t handle);
	[[nodiscard]] WupsServiceStatus ContentRedirectSetActive(WupsOwnerToken owner,
		std::uint32_t handle, bool active);
	[[nodiscard]] std::optional<std::filesystem::path> ResolveContentPath(
		WupsOwnerToken owner, std::string_view virtualPath, bool write,
		std::string& error) const;

	[[nodiscard]] WupsOwnerResourceCounts ResourceCounts(
		WupsOwnerToken owner) const;

	[[nodiscard]] std::shared_ptr<ModuleExportRegistry> ExportRegistry() const;
	[[nodiscard]] std::shared_ptr<WupsFunctionPatchManager> PatchManager() const;
	void OnModuleLoaded(std::string_view moduleName, std::uint64_t lifetimeId);
	void OnModuleUnloading(std::string_view moduleName, std::uint64_t lifetimeId);
	void SetModuleEventDetach(std::function<void()> detach);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

class WupsPluginRuntime final : public ITrustedPayloadInstance
{
public:
	[[nodiscard]] static std::shared_ptr<WupsPluginRuntime> Create(
		CemodPackage package, std::uint64_t owner, std::uint32_t generation,
		std::shared_ptr<IWupsRuntimeServices> services,
		std::shared_ptr<IWupsModuleLoader> moduleLoader, std::string& error);
	~WupsPluginRuntime() override;

	[[nodiscard]] CemodPayloadFormat Format() const override;
	[[nodiscard]] std::uint64_t OwnerHandle() const override;
	[[nodiscard]] std::uint32_t Generation() const override;
	[[nodiscard]] WupsPluginState State() const;
	[[nodiscard]] WupsMetadata Metadata() const;
	[[nodiscard]] std::string LastError() const;
	[[nodiscard]] CemodPackage PackageCopy() const;

	bool OnApplicationStarts(std::string& error) override;
	void OnReleaseForeground() override;
	void OnAcquiredForeground() override;
	void OnApplicationRequestsExit() override;
	void OnApplicationEnds() override;
	void Unload() override;
	[[nodiscard]] bool UnloadChecked(std::string& error);

private:
	struct Impl;
	explicit WupsPluginRuntime(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> m_impl;
};

class WupsPayloadRuntime
{
public:
	explicit WupsPayloadRuntime(
		std::shared_ptr<IWupsRuntimeServices> services = {},
		std::shared_ptr<IWupsModuleLoader> moduleLoader = {});
	~WupsPayloadRuntime();

	[[nodiscard]] std::optional<std::uint64_t> Load(CemodPackage package,
		std::string& error);
	[[nodiscard]] bool Reload(std::uint64_t handle, CemodPackage package,
		std::string& error);
	[[nodiscard]] bool Unload(std::uint64_t handle);
	[[nodiscard]] bool Unload(std::uint64_t handle, std::string& error);
	void UnloadAll();
	[[nodiscard]] bool UnloadAll(std::string& error);

	[[nodiscard]] bool OnApplicationStarts(std::string& error);
	void OnReleaseForeground();
	void OnAcquiredForeground();
	void OnApplicationRequestsExit();
	void OnApplicationEnds();

	[[nodiscard]] std::shared_ptr<WupsPluginRuntime> Find(std::uint64_t handle) const;
	[[nodiscard]] std::size_t Size() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// Production adapter implemented in WupsRplLoader.cpp. Tests can inject a
// deterministic loader without initializing Cemu's guest address space.
[[nodiscard]] std::shared_ptr<IWupsModuleLoader> CreateRplWupsModuleLoader();
[[nodiscard]] std::shared_ptr<IWupsRuntimeServices>
	CreateRplAromaCompatibilityRuntime();
