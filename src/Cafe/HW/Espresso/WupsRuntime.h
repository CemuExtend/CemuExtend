#pragma once

#include "Cafe/HW/Espresso/ITrustedPayloadInstance.h"
#include "Cafe/HW/Espresso/ModuleExportRegistry.h"
#include "Cafe/HW/Espresso/WupsBinary.h"
#include "Cafe/HW/Espresso/WupsFunctionPatcher.h"

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
	bool skip{};
};

class IWupsRuntimeServices
{
public:
	virtual ~IWupsRuntimeServices() = default;

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
	virtual void ReleaseOwnerResources(std::uint64_t owner, std::uint32_t generation) = 0;
	[[nodiscard]] virtual bool IsProcessInScope(const CemodPackage& package,
		std::string& reason) const = 0;
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
	[[nodiscard]] virtual bool Invoke(RPLModule* module, std::uint64_t lifetimeId,
		std::uint32_t targetVirtualAddress, std::span<const std::uint32_t> argumentWords,
		std::uint32_t& result, std::string& error) = 0;
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
	explicit AromaCompatibilityRuntime(WupsProcessKind process = WupsProcessKind::Game,
		std::shared_ptr<ModuleExportRegistry> registry = {},
		std::shared_ptr<WupsFunctionPatchManager> patchManager = {},
		std::shared_ptr<IWupsPatchPlatform> patchPlatform = {});
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
	void ReleaseOwnerResources(std::uint64_t owner, std::uint32_t generation) override;
	[[nodiscard]] bool IsProcessInScope(const CemodPackage& package,
		std::string& reason) const override;

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
