#pragma once

#include "Cafe/HW/Espresso/ModuleExportRegistry.h"
#include "Cafe/HW/Espresso/WumsBinary.h"

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

enum class WumsModuleState : std::uint8_t
{
	Parsed,
	Mapped,
	Linked,
	Initialized,
	ApplicationStarted,
	ExitRequested,
	ApplicationEnded,
	Unloading,
	Unloaded,
	Failed,
};

struct WumsHookInvocation
{
	std::vector<std::uint32_t> argumentWords;
	bool requireZeroResult{};
	bool skip{};
};

struct WumsModuleDefinition
{
	std::string fileName;
	std::vector<std::byte> image;
	ModuleProviderKind providerKind{ModuleProviderKind::CustomModule};
	std::optional<WumsInspection> inspection;
};

using WumsImportResolver = std::function<std::optional<std::uint32_t>(
	std::string_view moduleName, std::string_view symbolName,
	WupsSymbolKind kind, std::string& error)>;

class IWumsModuleLoader
{
public:
	virtual ~IWumsModuleLoader() = default;

	[[nodiscard]] virtual bool Map(std::span<const std::byte> image,
		std::string_view moduleName, const ModuleProviderOwner& owner,
		WumsImportResolver resolver, RPLModule*& module,
		std::uint64_t& lifetimeId, std::string& error) = 0;
	[[nodiscard]] virtual bool Link(RPLModule* module,
		std::uint64_t lifetimeId, std::string& error) = 0;
	[[nodiscard]] virtual bool ResolveAddress(RPLModule* module,
		std::uint64_t lifetimeId, std::uint32_t virtualAddress,
		std::uint32_t size, WupsSymbolKind kind,
		std::uint32_t& mappedAddress, std::string& error) = 0;
	[[nodiscard]] virtual bool Invoke(RPLModule* module,
		std::uint64_t lifetimeId, std::uint32_t target,
		std::span<const std::uint32_t> arguments,
		std::uint32_t& result, std::string& error) = 0;
	[[nodiscard]] virtual bool Unload(RPLModule* module,
		std::uint64_t lifetimeId, std::string& error) = 0;
};

class IWumsRuntimeServices
{
public:
	virtual ~IWumsRuntimeServices() = default;

	[[nodiscard]] virtual bool PrepareHook(
		const WumsInspection& inspection,
		const ModuleProviderOwner& owner, WumsHookType type,
		WumsHookInvocation& invocation, std::string& error) = 0;
	virtual void ReleaseModule(const WumsInspection& inspection,
		const ModuleProviderOwner& owner) = 0;
};

class WumsDependencyGraph
{
public:
	[[nodiscard]] static bool Build(
		std::span<const WumsInspection> modules,
		const ModuleExportRegistry& registry,
		std::vector<std::size_t>& order, std::string& error);
};

class WumsModuleRuntime
{
public:
	WumsModuleRuntime(std::shared_ptr<ModuleExportRegistry> registry,
		std::shared_ptr<IWumsModuleLoader> loader,
		std::shared_ptr<IWumsRuntimeServices> services);
	~WumsModuleRuntime();

	[[nodiscard]] bool Load(std::vector<WumsModuleDefinition> modules,
		std::string& error);
	[[nodiscard]] bool Reload(std::vector<WumsModuleDefinition> modules,
		std::string& error);
	[[nodiscard]] bool AddOrReplace(WumsModuleDefinition module,
		std::string& error);
	[[nodiscard]] bool Unload(std::string& error);

	[[nodiscard]] bool OnApplicationStarts(std::string& error);
	void OnApplicationRequestsExit();
	void OnApplicationEnds();

	[[nodiscard]] std::size_t Size() const;
	[[nodiscard]] std::vector<std::string> LoadOrder() const;
	[[nodiscard]] std::optional<WumsModuleState> State(
		std::string_view moduleName) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

[[nodiscard]] std::shared_ptr<IWumsModuleLoader> CreateRplWumsModuleLoader();
[[nodiscard]] std::shared_ptr<IWumsRuntimeServices>
	CreateRplWumsRuntimeServices();
