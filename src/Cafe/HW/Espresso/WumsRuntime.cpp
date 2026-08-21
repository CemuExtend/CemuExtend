#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WumsRuntime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <mutex>
#include <set>

namespace
{
	std::optional<WupsVersion> ParseVersion(std::string_view value)
	{
		std::array<std::uint16_t, 3> parts{};
		for (std::size_t index = 0; index < parts.size(); ++index)
		{
			const auto separator = index + 1 == parts.size() ? value.size() : value.find('.');
			if (separator == std::string_view::npos || separator == 0)
				return std::nullopt;
			unsigned part{};
			const auto result = std::from_chars(
				value.data(), value.data() + separator, part);
			if (result.ec != std::errc{} ||
				result.ptr != value.data() + separator ||
				part > std::numeric_limits<std::uint16_t>::max())
				return std::nullopt;
			parts[index] = static_cast<std::uint16_t>(part);
			value.remove_prefix(separator +
								(index + 1 == parts.size() ? 0 : 1));
		}
		if (!value.empty())
			return std::nullopt;
		return WupsVersion{parts[0], parts[1], parts[2]};
	}

	bool VersionMatches(const WumsDependency& dependency,
						std::string_view providerVersion, std::string& error)
	{
		if (dependency.match == WumsDependencyMatch::Any)
			return true;
		const auto parsed = ParseVersion(providerVersion);
		if (!parsed)
		{
			error = fmt::format(
				"dependency '{}' requires semantic version {}, but provider "
				"version '{}' is not semantic",
				dependency.moduleName,
				dependency.version->ToString(), providerVersion);
			return false;
		}
		if ((dependency.match == WumsDependencyMatch::Exact &&
			 *parsed != *dependency.version) ||
			(dependency.match == WumsDependencyMatch::AtLeast &&
			 *parsed < *dependency.version))
		{
			error = fmt::format(
				"dependency '{}' version mismatch: provider is {}, constraint is {}{}",
				dependency.moduleName, parsed->ToString(),
				dependency.match == WumsDependencyMatch::Exact ? "=" : ">=",
				dependency.version->ToString());
			return false;
		}
		return true;
	}

	std::optional<WumsHookType> FiniFor(WumsHookType type)
	{
		switch (type)
		{
		case WumsHookType::InitWutMalloc:
			return WumsHookType::FiniWutMalloc;
		case WumsHookType::InitWutNewlib:
			return WumsHookType::FiniWutNewlib;
		case WumsHookType::InitWutStdcpp:
			return WumsHookType::FiniWutStdcpp;
		case WumsHookType::InitWutDevoptab:
			return WumsHookType::FiniWutDevoptab;
		case WumsHookType::InitWutSockets:
			return WumsHookType::FiniWutSockets;
		case WumsHookType::InitWrapper:
			return WumsHookType::FiniWrapper;
		case WumsHookType::Init:
			return WumsHookType::Deinit;
		default:
			return std::nullopt;
		}
	}

	constexpr std::array kInitializerOrder{
		WumsHookType::InitReentFunctions,
		WumsHookType::InitWutThread,
		WumsHookType::InitWutMalloc,
		WumsHookType::InitWutNewlib,
		WumsHookType::InitWutStdcpp,
		WumsHookType::InitWutDevoptab,
		WumsHookType::InitWutSockets,
		WumsHookType::InitWrapper,
		WumsHookType::Init,
	};
} // namespace

bool WumsDependencyGraph::Build(
	std::span<const WumsInspection> modules,
	const ModuleExportRegistry& registry,
	std::vector<std::size_t>& order, std::string& error)
{
	order.clear();
	error.clear();
	std::map<std::string, std::size_t, std::less<>> byName;
	for (std::size_t index = 0; index < modules.size(); ++index)
		if (!byName.emplace(modules[index].metadata.moduleName, index).second)
		{
			error = fmt::format(
				"duplicate WUMS module identifier '{}'",
				modules[index].metadata.moduleName);
			return false;
		}

	std::vector<std::set<std::size_t>> outgoing(modules.size());
	std::vector<std::size_t> incoming(modules.size());
	for (std::size_t index = 0; index < modules.size(); ++index)
	{
		for (const auto& dependency : modules[index].dependencies)
		{
			const auto internal = byName.find(dependency.moduleName);
			if (internal != byName.end())
			{
				const auto& provider = modules[internal->second].metadata;
				std::string versionError;
				if (!VersionMatches(
						dependency, provider.version, versionError))
				{
					error = fmt::format(
						"module '{}' {}", modules[index].metadata.moduleName,
						versionError);
					return false;
				}
				if (outgoing[internal->second].insert(index).second)
					++incoming[index];
				continue;
			}
			const auto external = registry.Provider(dependency.moduleName);
			if (external)
			{
				std::string versionError;
				if (!VersionMatches(
						dependency, external->version, versionError))
				{
					error = fmt::format(
						"module '{}' {}", modules[index].metadata.moduleName,
						versionError);
					return false;
				}
				continue;
			}
			if (!dependency.optional)
			{
				error = fmt::format(
					"module '{}' is missing mandatory dependency '{}'",
					modules[index].metadata.moduleName,
					dependency.moduleName);
				return false;
			}
		}
	}

	std::set<std::pair<std::string, std::size_t>> ready;
	for (std::size_t index = 0; index < modules.size(); ++index)
		if (incoming[index] == 0)
			ready.emplace(modules[index].metadata.moduleName, index);
	while (!ready.empty())
	{
		const auto [name, index] = *ready.begin();
		ready.erase(ready.begin());
		order.push_back(index);
		for (const auto dependent : outgoing[index])
			if (--incoming[dependent] == 0)
				ready.emplace(
					modules[dependent].metadata.moduleName, dependent);
	}
	if (order.size() != modules.size())
	{
		std::vector<std::string> cycle;
		for (std::size_t index = 0; index < modules.size(); ++index)
			if (incoming[index] != 0)
				cycle.push_back(modules[index].metadata.moduleName);
		std::ranges::sort(cycle);
		error = fmt::format(
			"WUMS dependency cycle contains: {}", fmt::join(cycle, ", "));
		order.clear();
		return false;
	}
	return true;
}

struct WumsModuleRuntime::Impl
{
	struct ImportContext
	{
		ImportContext(std::shared_ptr<ModuleExportRegistry> registry_,
					  ModuleProviderOwner owner_) : registry(std::move(registry_)), owner(owner_)
		{
		}

		std::shared_ptr<ModuleExportRegistry> registry;
		ModuleProviderOwner owner;
		std::mutex mutex;
		std::vector<ModuleExportLease> leases;

		std::optional<std::uint32_t> Resolve(
			std::string_view moduleName, std::string_view symbolName,
			WupsSymbolKind kind, std::string& error)
		{
			auto lease = registry->Resolve(
				moduleName, symbolName, kind, owner, error);
			if (!lease)
				return std::nullopt;
			const auto address = lease->Address();
			std::lock_guard lock(mutex);
			leases.push_back(std::move(*lease));
			return address;
		}

		void Clear()
		{
			std::lock_guard lock(mutex);
			leases.clear();
		}
	};

	struct Module
	{
		WumsModuleDefinition definition;
		WumsInspection inspection;
		ModuleProviderOwner owner;
		RPLModule* module{};
		std::uint64_t lifetime{};
		ModuleProviderHandle provider;
		std::shared_ptr<ImportContext> imports;
		std::map<WumsHookType, std::uint32_t> hooks;
		std::vector<WumsHookType> initialized;
		std::atomic<WumsModuleState> state{WumsModuleState::Parsed};
	};

	std::shared_ptr<ModuleExportRegistry> registry;
	std::shared_ptr<IWumsModuleLoader> loader;
	std::shared_ptr<IWumsRuntimeServices> services;
	mutable std::mutex mutex;
	std::vector<std::shared_ptr<Module>> modules;
	std::vector<WumsModuleDefinition> definitions;
	std::uint64_t nextOwner{1};
	std::uint32_t nextGeneration{1};
	std::atomic_bool applicationStarted{};
	std::atomic_bool exitRequested{};
	std::atomic_flag operation = ATOMIC_FLAG_INIT;

	struct OperationGuard
	{
		explicit OperationGuard(Impl& runtime_) : runtime(runtime_),
												  acquired(!runtime.operation.test_and_set()) {}
		~OperationGuard()
		{
			if (acquired)
				runtime.operation.clear();
		}
		Impl& runtime;
		bool acquired{};
	};

	std::vector<std::shared_ptr<Module>> SnapshotModules() const
	{
		std::lock_guard lock(mutex);
		return modules;
	}

	bool Invoke(const std::shared_ptr<Module>& module,
				WumsHookType type, bool teardown, std::string& error)
	{
		const auto found = module->hooks.find(type);
		if (found == module->hooks.end())
			return true;
		if (!teardown &&
			(module->state == WumsModuleState::Unloading ||
			 module->state == WumsModuleState::Unloaded ||
			 module->state == WumsModuleState::Failed))
		{
			error = fmt::format(
				"module '{}' rejected hook {} in stale state {}",
				module->inspection.metadata.moduleName,
				static_cast<unsigned>(type),
				static_cast<unsigned>(module->state.load()));
			return false;
		}
		WumsHookInvocation invocation;
		if (!services->PrepareHook(
				module->inspection, module->owner, type, invocation, error))
			return false;
		if (invocation.skip)
			return true;
		std::uint32_t result{};
		if (!loader->Invoke(
				module->module, module->lifetime, found->second,
				invocation.argumentWords, result, error))
			return false;
		if (invocation.requireZeroResult && result != 0)
		{
			error = fmt::format(
				"module '{}' hook {} returned 0x{:08x}",
				module->inspection.metadata.moduleName,
				static_cast<unsigned>(type), result);
			return false;
		}
		return true;
	}

	bool Initialize(const std::shared_ptr<Module>& module,
					std::string& error)
	{
		for (const auto type : kInitializerOrder)
		{
			if (type == WumsHookType::InitWrapper &&
				module->inspection.metadata.skipInitFini)
				continue;
			if (!Invoke(module, type, false, error))
				return false;
			if (module->hooks.contains(type))
				module->initialized.push_back(type);
		}
		module->state = WumsModuleState::Initialized;
		return true;
	}

	void Teardown(const std::shared_ptr<Module>& module,
				  std::string& errors)
	{
		for (const auto initializer :
			 std::ranges::reverse_view(module->initialized))
			if (const auto fini = FiniFor(initializer))
			{
				if (*fini == WumsHookType::FiniWrapper &&
					module->inspection.metadata.skipInitFini)
					continue;
				std::string error;
				if (!Invoke(module, *fini, true, error) && !error.empty())
					errors.append(errors.empty() ? "" : "; ").append(error);
			}
		module->initialized.clear();
		std::string clearError;
		if (!Invoke(module, WumsHookType::ClearAllocatedRplMemory,
					true, clearError) &&
			!clearError.empty())
			errors.append(errors.empty() ? "" : "; ").append(clearError);
		services->ReleaseModule(module->inspection, module->owner);
	}

	bool Rollback(std::vector<std::shared_ptr<Module>>& loaded,
				  std::string& error)
	{
		bool success = true;
		for (const auto& module : std::ranges::reverse_view(loaded))
		{
			module->state = WumsModuleState::Unloading;
			std::string cleanupError;
			Teardown(module, cleanupError);
			module->imports->Clear();
			if (module->provider)
			{
				std::string registryError;
				if (!registry->Unpublish(
						module->provider, module->owner, registryError))
					cleanupError.append(cleanupError.empty() ? "" : "; ")
						.append(registryError);
				else
					module->provider = {};
			}
			if (module->module)
			{
				std::string unloadError;
				if (!loader->Unload(
						module->module, module->lifetime, unloadError))
					cleanupError.append(cleanupError.empty() ? "" : "; ")
						.append(unloadError);
				else
				{
					module->module = nullptr;
					module->lifetime = 0;
				}
			}
			if (!cleanupError.empty())
			{
				success = false;
				error.append(error.empty() ? "" : "; ").append(module->inspection.metadata.moduleName).append(": ").append(cleanupError);
				module->state = WumsModuleState::Failed;
			}
			else
				module->state = WumsModuleState::Unloaded;
		}
		return success;
	}

	bool LoadDefinitions(std::vector<WumsModuleDefinition> newDefinitions,
						 std::string& error)
	{
		std::vector<WumsInspection> inspections;
		inspections.reserve(newDefinitions.size());
		for (auto& definition : newDefinitions)
		{
			if (definition.image.empty())
			{
				error = fmt::format(
					"WUMS module '{}' has an empty image",
					definition.fileName);
				return false;
			}
			if (!definition.inspection)
			{
				auto inspection =
					WumsBinaryInspector::Inspect(definition.image, error);
				if (!inspection)
					return false;
				definition.inspection = std::move(*inspection);
			}
			inspections.push_back(*definition.inspection);
		}
		std::vector<std::size_t> order;
		if (!WumsDependencyGraph::Build(
				inspections, *registry, order, error))
			return false;

		std::vector<std::shared_ptr<Module>> loaded;
		for (const auto index : order)
		{
			auto module = std::make_shared<Module>();
			module->definition = std::move(newDefinitions[index]);
			module->inspection = inspections[index];
			module->owner = {
				nextOwner++, nextGeneration++, 1};
			module->imports = std::make_shared<ImportContext>(
				registry, module->owner);
			for (const auto& hook : module->inspection.hooks)
				module->hooks.emplace(hook.type, hook.target);
			const std::weak_ptr<ImportContext> weakImports = module->imports;
			WumsImportResolver resolver =
				[weakImports](std::string_view importModule,
							  std::string_view symbol, WupsSymbolKind kind,
							  std::string& resolveError)
				-> std::optional<std::uint32_t> {
				const auto imports = weakImports.lock();
				if (!imports)
				{
					resolveError =
						"WUMS import resolver owner has expired";
					return std::nullopt;
				}
				return imports->Resolve(
					importModule, symbol, kind, resolveError);
			};
			if (!loader->Map(
					module->definition.image,
					module->inspection.metadata.moduleName + ".wms",
					module->owner, std::move(resolver),
					module->module, module->lifetime, error))
			{
				module->state = WumsModuleState::Failed;
				loaded.push_back(module);
				std::string rollbackError;
				Rollback(loaded, rollbackError);
				if (!rollbackError.empty())
					error.append("; rollback: ").append(rollbackError);
				return false;
			}
			module->owner.lifetime = module->lifetime;
			module->imports->owner = module->owner;
			module->state = WumsModuleState::Mapped;
			std::vector<ModuleExportDescriptor> exports;
			for (const auto& symbol : module->inspection.exports)
			{
				std::uint32_t mapped{};
				if (!loader->ResolveAddress(
						module->module, module->lifetime,
						symbol.address,
						symbol.kind == WupsSymbolKind::Function ? 4 : 1,
						symbol.kind, mapped, error))
				{
					loaded.push_back(module);
					std::string rollbackError;
					Rollback(loaded, rollbackError);
					if (!rollbackError.empty())
						error.append("; rollback: ").append(rollbackError);
					return false;
				}
				exports.push_back({symbol.name, symbol.kind, mapped});
			}
			const ModuleProviderDescriptor provider{
				module->definition.providerKind,
				module->inspection.metadata.moduleName,
				module->inspection.metadata.version,
				module->owner};
			if (!registry->Publish(
					provider, exports, module->provider, error))
			{
				loaded.push_back(module);
				std::string rollbackError;
				Rollback(loaded, rollbackError);
				if (!rollbackError.empty())
					error.append("; rollback: ").append(rollbackError);
				return false;
			}
			if (!loader->Link(
					module->module, module->lifetime, error))
			{
				loaded.push_back(module);
				std::string rollbackError;
				Rollback(loaded, rollbackError);
				if (!rollbackError.empty())
					error.append("; rollback: ").append(rollbackError);
				return false;
			}
			module->state = WumsModuleState::Linked;
			loaded.push_back(module);
		}

		for (const auto& module : loaded)
			if (module->inspection.metadata.initBeforeRelocationsDone &&
				!Initialize(module, error))
			{
				std::string rollbackError;
				Rollback(loaded, rollbackError);
				if (!rollbackError.empty())
					error.append("; rollback: ").append(rollbackError);
				return false;
			}
		for (const auto& module : loaded)
			if (!Invoke(module, WumsHookType::GetCustomRplAllocator,
						false, error))
			{
				std::string rollbackError;
				Rollback(loaded, rollbackError);
				if (!rollbackError.empty())
					error.append("; rollback: ").append(rollbackError);
				return false;
			}
		for (const auto& module : loaded)
			if (!Invoke(module, WumsHookType::RelocationsDone,
						false, error))
			{
				std::string rollbackError;
				Rollback(loaded, rollbackError);
				if (!rollbackError.empty())
					error.append("; rollback: ").append(rollbackError);
				return false;
			}
		for (const auto& module : loaded)
			if (!module->inspection.metadata.initBeforeRelocationsDone &&
				!Initialize(module, error))
			{
				std::string rollbackError;
				Rollback(loaded, rollbackError);
				if (!rollbackError.empty())
					error.append("; rollback: ").append(rollbackError);
				return false;
			}
		std::vector<WumsModuleDefinition> committedDefinitions;
		committedDefinitions.reserve(loaded.size());
		for (const auto& module : loaded)
			committedDefinitions.push_back(module->definition);
		{
			std::lock_guard lock(mutex);
			modules = std::move(loaded);
			definitions = std::move(committedDefinitions);
		}
		return true;
	}
};

WumsModuleRuntime::WumsModuleRuntime(
	std::shared_ptr<ModuleExportRegistry> registry,
	std::shared_ptr<IWumsModuleLoader> loader,
	std::shared_ptr<IWumsRuntimeServices> services) : m_impl(std::make_unique<Impl>())
{
	m_impl->registry = registry ? std::move(registry) : std::make_shared<ModuleExportRegistry>();
	m_impl->loader = loader ? std::move(loader) : CreateRplWumsModuleLoader();
	m_impl->services = services ? std::move(services) : CreateRplWumsRuntimeServices();
}

WumsModuleRuntime::~WumsModuleRuntime()
{
	std::string error;
	if (!Unload(error))
		cemuLog_log(LogType::Force,
					"WUMS: runtime destruction could not unload every module: {}", error);
}

bool WumsModuleRuntime::Load(
	std::vector<WumsModuleDefinition> modules, std::string& error)
{
	error.clear();
	Impl::OperationGuard operation(*m_impl);
	if (!operation.acquired)
	{
		error = "WUMS lifecycle operation rejected concurrent or reentrant load";
		return false;
	}
	if (!m_impl->SnapshotModules().empty())
	{
		error = "WUMS runtime is already loaded";
		return false;
	}
	if (!m_impl->loader || !m_impl->services)
	{
		error = "WUMS RPL loader or lifecycle service is unavailable";
		return false;
	}
	return m_impl->LoadDefinitions(std::move(modules), error);
}

bool WumsModuleRuntime::Reload(
	std::vector<WumsModuleDefinition> modules, std::string& error)
{
	std::vector<WumsModuleDefinition> rollback;
	bool restart{};
	{
		std::lock_guard lock(m_impl->mutex);
		rollback = m_impl->definitions;
		restart = m_impl->applicationStarted;
	}
	std::string unloadError;
	if (!Unload(unloadError))
	{
		error = fmt::format(
			"WUMS reload could not unload the old graph: {}", unloadError);
		return false;
	}
	if (Load(std::move(modules), error) &&
		(!restart || OnApplicationStarts(error)))
		return true;
	const auto replacementError = error;
	std::string cleanupError;
	(void)Unload(cleanupError);
	std::string rollbackError;
	if (Load(std::move(rollback), rollbackError) &&
		(!restart || OnApplicationStarts(rollbackError)))
	{
		error = fmt::format(
			"WUMS reload failed and the previous graph was restored: {}",
			replacementError);
		return false;
	}
	error = fmt::format(
		"WUMS reload failed ({}); rollback failed ({})",
		replacementError, rollbackError);
	return false;
}

bool WumsModuleRuntime::AddOrReplace(
	WumsModuleDefinition module, std::string& error)
{
	std::vector<WumsModuleDefinition> definitions;
	{
		std::lock_guard lock(m_impl->mutex);
		definitions = m_impl->definitions;
	}
	std::string inspectError;
	if (!module.inspection)
		module.inspection = WumsBinaryInspector::Inspect(
			module.image, inspectError);
	if (!module.inspection)
	{
		error = inspectError;
		return false;
	}
	const auto name = module.inspection->metadata.moduleName;
	const auto found = std::ranges::find_if(
		definitions, [&](const auto& existing) {
			return existing.inspection &&
				   existing.inspection->metadata.moduleName == name;
		});
	if (found == definitions.end())
		definitions.push_back(std::move(module));
	else
		*found = std::move(module);
	return Reload(std::move(definitions), error);
}

bool WumsModuleRuntime::Unload(std::string& error)
{
	error.clear();
	Impl::OperationGuard operation(*m_impl);
	if (!operation.acquired)
	{
		error = "WUMS lifecycle operation rejected concurrent or reentrant unload";
		return false;
	}
	auto modules = m_impl->SnapshotModules();
	if (modules.empty())
		return true;
	if (m_impl->applicationStarted.load())
	{
		for (const auto& module : modules)
		{
			std::string hookError;
			if (!m_impl->Invoke(
					module, WumsHookType::ApplicationEnds,
					true, hookError) &&
				!hookError.empty())
				error.append(error.empty() ? "" : "; ").append(hookError);
		}
		for (const auto& module : modules)
		{
			std::string hookError;
			if (!m_impl->Invoke(
					module, WumsHookType::AllApplicationEndsDone,
					true, hookError) &&
				!hookError.empty())
				error.append(error.empty() ? "" : "; ").append(hookError);
		}
		m_impl->applicationStarted = false;
	}
	const bool success = m_impl->Rollback(modules, error);
	if (success)
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->modules.clear();
		m_impl->definitions.clear();
		m_impl->exitRequested = false;
	}
	return success;
}

bool WumsModuleRuntime::OnApplicationStarts(std::string& error)
{
	error.clear();
	Impl::OperationGuard operation(*m_impl);
	if (!operation.acquired)
	{
		error = "WUMS lifecycle operation rejected concurrent or reentrant application start";
		return false;
	}
	if (m_impl->applicationStarted.load())
		return true;
	const auto modules = m_impl->SnapshotModules();
	std::vector<std::shared_ptr<Impl::Module>> started;
	for (const auto& module : modules)
		if (!m_impl->Invoke(
				module, WumsHookType::ApplicationStarts, false, error))
		{
			for (const auto& rollback : std::ranges::reverse_view(started))
			{
				std::string ignored;
				(void)m_impl->Invoke(rollback,
									 WumsHookType::ApplicationEnds, true, ignored);
			}
			return false;
		}
		else
			started.push_back(module);
	for (const auto& module : modules)
		if (!m_impl->Invoke(
				module, WumsHookType::AllApplicationStartsDone, false, error))
		{
			for (const auto& rollback : std::ranges::reverse_view(started))
			{
				std::string ignored;
				(void)m_impl->Invoke(rollback,
									 WumsHookType::ApplicationEnds, true, ignored);
			}
			return false;
		}
	for (const auto& module : modules)
		module->state = WumsModuleState::ApplicationStarted;
	m_impl->applicationStarted = true;
	m_impl->exitRequested = false;
	return true;
}

void WumsModuleRuntime::OnApplicationRequestsExit()
{
	Impl::OperationGuard operation(*m_impl);
	if (!operation.acquired || !m_impl->applicationStarted.load() ||
		m_impl->exitRequested.load())
		return;
	const auto modules = m_impl->SnapshotModules();
	for (const auto& module : modules)
	{
		std::string error;
		(void)m_impl->Invoke(
			module, WumsHookType::ApplicationRequestsExit, false, error);
	}
	for (const auto& module : modules)
	{
		std::string error;
		(void)m_impl->Invoke(
			module, WumsHookType::AllApplicationRequestsExitDone,
			false, error);
		module->state = WumsModuleState::ExitRequested;
	}
	m_impl->exitRequested = true;
}

void WumsModuleRuntime::OnApplicationEnds()
{
	Impl::OperationGuard operation(*m_impl);
	if (!operation.acquired || !m_impl->applicationStarted.load())
		return;
	const auto modules = m_impl->SnapshotModules();
	for (const auto& module : modules)
	{
		std::string error;
		(void)m_impl->Invoke(
			module, WumsHookType::ApplicationEnds, false, error);
	}
	for (const auto& module : modules)
	{
		std::string error;
		(void)m_impl->Invoke(
			module, WumsHookType::AllApplicationEndsDone, false, error);
		module->state = WumsModuleState::ApplicationEnded;
	}
	m_impl->applicationStarted = false;
}

std::size_t WumsModuleRuntime::Size() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->modules.size();
}

std::vector<std::string> WumsModuleRuntime::LoadOrder() const
{
	std::lock_guard lock(m_impl->mutex);
	std::vector<std::string> result;
	for (const auto& module : m_impl->modules)
		result.push_back(module->inspection.metadata.moduleName);
	return result;
}

std::optional<WumsModuleState> WumsModuleRuntime::State(
	std::string_view moduleName) const
{
	std::lock_guard lock(m_impl->mutex);
	const auto found = std::ranges::find_if(
		m_impl->modules, [&](const auto& module) {
			return module->inspection.metadata.moduleName == moduleName;
		});
	return found == m_impl->modules.end() ? std::nullopt : std::optional{(*found)->state.load()};
}
