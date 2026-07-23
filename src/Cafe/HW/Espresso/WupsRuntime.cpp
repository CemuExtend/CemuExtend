#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsRuntime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>

namespace
{
	thread_local const void* s_activeWupsRuntime{};

	std::string HookName(WupsHookType type)
	{
		switch (type)
		{
		case WupsHookType::InitWutMalloc: return "INIT_WUT_MALLOC";
		case WupsHookType::FiniWutMalloc: return "FINI_WUT_MALLOC";
		case WupsHookType::InitWutNewlib: return "INIT_WUT_NEWLIB";
		case WupsHookType::FiniWutNewlib: return "FINI_WUT_NEWLIB";
		case WupsHookType::InitWutStdcpp: return "INIT_WUT_STDCPP";
		case WupsHookType::FiniWutStdcpp: return "FINI_WUT_STDCPP";
		case WupsHookType::InitWutDevoptab: return "INIT_WUT_DEVOPTAB";
		case WupsHookType::FiniWutDevoptab: return "FINI_WUT_DEVOPTAB";
		case WupsHookType::InitWutSockets: return "INIT_WUT_SOCKETS";
		case WupsHookType::FiniWutSockets: return "FINI_WUT_SOCKETS";
		case WupsHookType::InitWrapper: return "INIT_WRAPPER";
		case WupsHookType::FiniWrapper: return "FINI_WRAPPER";
		case WupsHookType::GetConfigDeprecated: return "GET_CONFIG_DEPRECATED";
		case WupsHookType::ConfigClosedDeprecated: return "CONFIG_CLOSED_DEPRECATED";
		case WupsHookType::InitStorageDeprecated: return "INIT_STORAGE_DEPRECATED";
		case WupsHookType::InitPlugin: return "INIT_PLUGIN";
		case WupsHookType::DeinitPlugin: return "DEINIT_PLUGIN";
		case WupsHookType::ApplicationStarts: return "APPLICATION_STARTS";
		case WupsHookType::ReleaseForeground: return "RELEASE_FOREGROUND";
		case WupsHookType::AcquiredForeground: return "ACQUIRED_FOREGROUND";
		case WupsHookType::ApplicationRequestsExit: return "APPLICATION_REQUESTS_EXIT";
		case WupsHookType::ApplicationEnds: return "APPLICATION_ENDS";
		case WupsHookType::InitStorage: return "INIT_STORAGE";
		case WupsHookType::InitConfig: return "INIT_CONFIG";
		case WupsHookType::InitButtonCombo: return "INIT_BUTTON_COMBO";
		case WupsHookType::InitWutThread: return "INIT_WUT_THREAD";
		case WupsHookType::InitReentFunctions: return "INIT_REENT_FUNCTIONS";
		}
		return fmt::format("HOOK_{}", static_cast<std::uint32_t>(type));
	}

	std::optional<WupsHookType> FiniFor(WupsHookType type)
	{
		switch (type)
		{
		case WupsHookType::InitWutMalloc: return WupsHookType::FiniWutMalloc;
		case WupsHookType::InitWutNewlib: return WupsHookType::FiniWutNewlib;
		case WupsHookType::InitWutStdcpp: return WupsHookType::FiniWutStdcpp;
		case WupsHookType::InitWutDevoptab: return WupsHookType::FiniWutDevoptab;
		case WupsHookType::InitWutSockets: return WupsHookType::FiniWutSockets;
		case WupsHookType::InitWrapper: return WupsHookType::FiniWrapper;
		case WupsHookType::InitPlugin: return WupsHookType::DeinitPlugin;
		default: return std::nullopt;
		}
	}

	std::string Lower(std::string_view value)
	{
		std::string result(value);
		std::ranges::transform(result, result.begin(), [](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
		return result;
	}
}

struct WupsPluginRuntime::Impl
{
	CemodPackage package;
	WupsInspection inspection;
	const std::uint64_t owner;
	const std::uint32_t resourceGeneration;
	std::uint32_t generation;
	std::shared_ptr<IWupsRuntimeServices> services;
	std::shared_ptr<IWupsModuleLoader> moduleLoader;
	RPLModule* module{};
	std::uint64_t moduleLifetime{};
	std::map<WupsHookType, std::uint32_t> hooks;
	std::vector<WupsHookType> completedInitializers;
	mutable std::mutex mutex;
	std::condition_variable callbacksFinished;
	WupsPluginState state{WupsPluginState::Installed};
	std::uint32_t callbacksInFlight{};
	bool foregroundReleased{};
	bool exitRequested{};
	bool backendResourcesReleased{};
	bool pluginDeactivated{true};
	bool teardownInProgress{};
	bool deferredUnload{};
	bool applicationEndsPending{};
	bool deferredApplicationEnd{};
	bool lifecycleTransition{};
	bool foregroundTransition{};
	bool exitRequestInProgress{};
	std::string lastError;

	Impl(CemodPackage package_, WupsInspection inspection_, std::uint64_t owner_,
		std::uint32_t generation_, std::shared_ptr<IWupsRuntimeServices> services_,
		std::shared_ptr<IWupsModuleLoader> loader_) :
		package(std::move(package_)),
		inspection(std::move(inspection_)),
		owner(owner_),
		resourceGeneration(generation_),
		generation(generation_),
		services(std::move(services_)),
		moduleLoader(std::move(loader_))
	{
		for (const auto& hook : inspection.hooks)
			hooks.emplace(hook.type, hook.target);
	}

	struct InvocationActivity
	{
		explicit InvocationActivity(Impl& runtime_) :
			runtime(runtime_),
			previousRuntime(s_activeWupsRuntime)
		{
			s_activeWupsRuntime = &runtime;
		}

		~InvocationActivity()
		{
			s_activeWupsRuntime = previousRuntime;
			bool finishDeferredApplicationEnd{};
			bool finishDeferredUnload{};
			{
				std::lock_guard lock(runtime.mutex);
				cemu_assert_debug(runtime.callbacksInFlight != 0);
				if (runtime.callbacksInFlight != 0)
					--runtime.callbacksInFlight;
				runtime.callbacksFinished.notify_all();
				finishDeferredApplicationEnd = runtime.deferredApplicationEnd &&
					runtime.applicationEndsPending &&
					runtime.callbacksInFlight == 0 && !runtime.teardownInProgress;
				finishDeferredUnload = !finishDeferredApplicationEnd &&
					runtime.deferredUnload &&
					runtime.callbacksInFlight == 0 && !runtime.teardownInProgress;
			}
			if (finishDeferredApplicationEnd)
				runtime.FinishApplicationEnd();
			else if (finishDeferredUnload)
				runtime.FinishUnload(false);
		}

		Impl& runtime;
		const void* previousRuntime;
	};

	bool Invoke(WupsHookType type, bool allowTeardown, bool& invoked, std::string& error)
	{
		invoked = false;
		const auto found = hooks.find(type);
		if (found == hooks.end())
			return true;
		RPLModule* callbackModule{};
		std::uint64_t callbackLifetime{};
		std::uint32_t callbackGeneration{};
		{
			std::lock_guard lock(mutex);
			const bool permitted = allowTeardown ?
				(state == WupsPluginState::Unloading || state == WupsPluginState::Failed ||
					state == WupsPluginState::Active || state == WupsPluginState::Initialized ||
					state == WupsPluginState::Relocated || state == WupsPluginState::Deinitialized) :
				(state != WupsPluginState::Unloading && state != WupsPluginState::Unloaded &&
					state != WupsPluginState::Failed);
			if (!permitted || !module)
			{
				error = fmt::format("{} rejected for stale lifecycle state {}",
					HookName(type), static_cast<unsigned>(state));
				return false;
			}
			callbackModule = module;
			callbackLifetime = moduleLifetime;
			callbackGeneration = generation;
			++callbacksInFlight;
		}
		InvocationActivity activity(*this);

		WupsHookInvocation invocation;
		bool prepared{};
		try
		{
			prepared = services->PrepareHookInvocation(package, inspection.metadata,
				owner, resourceGeneration, type, invocation, error);
		}
		catch (const std::exception& exception)
		{
			error = fmt::format("{} preparation threw an exception: {}",
				HookName(type), exception.what());
		}
		catch (...)
		{
			error = fmt::format("{} preparation threw a non-standard exception",
				HookName(type));
		}
		std::uint32_t result{};
		bool called = false;
		bool authorized = allowTeardown;
		if (prepared && !allowTeardown)
		{
			std::lock_guard lock(mutex);
			authorized = generation == callbackGeneration &&
				state != WupsPluginState::Unloading &&
				state != WupsPluginState::Unloaded &&
				state != WupsPluginState::Failed;
			if (!authorized && error.empty())
				error = fmt::format(
					"{} preparation completed after its owner generation was revoked",
					HookName(type));
		}
		if (prepared && authorized && !invocation.skip)
		{
			try
			{
				WupsGuestOwnerScope ownerScope{{owner, resourceGeneration}};
				called = moduleLoader->Invoke(callbackModule, callbackLifetime,
					found->second, invocation.argumentWords, result, error);
			}
			catch (const std::exception& exception)
			{
				error = fmt::format("{} guest callback threw an exception: {}",
					HookName(type), exception.what());
			}
			catch (...)
			{
				error = fmt::format("{} guest callback threw a non-standard exception",
					HookName(type));
			}
			if (called && invocation.requireZeroResult && result != 0)
			{
				error = fmt::format("{} returned error 0x{:08x}", HookName(type), result);
				called = false;
			}
		}

		{
			std::lock_guard lock(mutex);
			if (prepared && authorized && (invocation.skip || called) &&
				(allowTeardown || (generation == callbackGeneration &&
					state != WupsPluginState::Unloading)))
				invoked = !invocation.skip;
			else if (prepared && authorized && called && error.empty())
				error = fmt::format("{} completed after its owner generation was revoked",
					HookName(type));
		}
		return prepared && authorized && (invocation.skip || called) &&
			(allowTeardown || invoked);
	}

	bool Invoke(WupsHookType type, bool allowTeardown, std::string& error)
	{
		bool invoked{};
		return Invoke(type, allowTeardown, invoked, error);
	}

	bool DeactivateBackend(std::string& error)
	{
		bool deactivate{};
		{
			std::lock_guard lock(mutex);
			deactivate = !pluginDeactivated;
		}
		if (!deactivate)
			return true;
		try
		{
			if (!services->DeactivatePlugin(owner, resourceGeneration, error))
				return false;
		}
		catch (const std::exception& exception)
		{
			error = fmt::format(
				"plugin deactivation threw an exception: {}", exception.what());
			return false;
		}
		catch (...)
		{
			error = "plugin deactivation threw a non-standard exception";
			return false;
		}
		std::lock_guard lock(mutex);
		pluginDeactivated = true;
		return true;
	}

	bool ReleaseBackendResources(std::string& error)
	{
		{
			std::lock_guard lock(mutex);
			if (backendResourcesReleased)
				return true;
		}
		try
		{
			if (!services->ReleaseOwnerResources(
				owner, resourceGeneration, error))
				return false;
		}
		catch (const std::exception& exception)
		{
			error = fmt::format(
				"owner resource release threw an exception: {}", exception.what());
			return false;
		}
		catch (...)
		{
			error = "owner resource release threw a non-standard exception";
			return false;
		}
		std::lock_guard lock(mutex);
		backendResourcesReleased = true;
		return true;
	}

	void TeardownInitializers()
	{
		std::vector<WupsHookType> completed;
		{
			std::lock_guard lock(mutex);
			completed.swap(completedInitializers);
		}
		std::string cleanupError;
		for (const auto initializer : std::ranges::reverse_view(completed))
			if (const auto fini = FiniFor(initializer))
			{
				std::string hookError;
				if (!Invoke(*fini, true, hookError) && !hookError.empty())
					cleanupError.append(cleanupError.empty() ? "" : "; ").append(hookError);
			}
		if (!cleanupError.empty())
		{
			std::lock_guard lock(mutex);
			lastError = std::move(cleanupError);
		}
	}

	void FinishApplicationEnd()
	{
		{
			std::unique_lock lock(mutex);
			if (!applicationEndsPending)
				return;
			if (s_activeWupsRuntime == this)
			{
				deferredApplicationEnd = true;
				return;
			}
			callbacksFinished.wait(lock, [this] { return callbacksInFlight == 0; });
			applicationEndsPending = false;
			deferredApplicationEnd = false;
		}

		std::string applicationEndError;
		if (!Invoke(WupsHookType::ApplicationEnds, true, applicationEndError) &&
			!applicationEndError.empty())
		{
			std::lock_guard lock(mutex);
			lastError = applicationEndError;
		}
		TeardownInitializers();
		std::string deactivateError;
		const bool deactivated = DeactivateBackend(deactivateError);

		bool finishDeferredUnload{};
		{
			std::lock_guard lock(mutex);
			if (!deactivated)
			{
				state = WupsPluginState::Failed;
				lastError.append(lastError.empty() ? "" : "; ")
					.append(deactivateError);
			}
			else if (state != WupsPluginState::Unloading &&
				state != WupsPluginState::Unloaded)
				state = WupsPluginState::Deinitialized;
			lifecycleTransition = false;
			foregroundReleased = false;
			finishDeferredUnload = deferredUnload &&
				callbacksInFlight == 0 && !teardownInProgress;
		}
		if (finishDeferredUnload)
			FinishUnload(false);
	}

	bool FinishUnload(bool preserveFailure, std::string* errorOut = nullptr)
	{
		bool callApplicationEnds{};
		{
			std::unique_lock lock(mutex);
			if (state == WupsPluginState::Unloaded || teardownInProgress)
			{
				if (state == WupsPluginState::Unloaded)
					return true;
				if (errorOut)
					*errorOut = "WUPS unload is already in progress";
				return false;
			}
			teardownInProgress = true;
			state = preserveFailure ? WupsPluginState::Failed : WupsPluginState::Unloading;
			callbacksFinished.wait(lock, [this] { return callbacksInFlight == 0; });
			callApplicationEnds = applicationEndsPending;
			applicationEndsPending = false;
			deferredApplicationEnd = false;
		}
		std::string cleanupError;
		if (callApplicationEnds)
		{
			std::string hookError;
			if (!Invoke(WupsHookType::ApplicationEnds, true, hookError) &&
				!hookError.empty())
				cleanupError = std::move(hookError);
		}
		TeardownInitializers();
		std::string deactivateError;
		if (!DeactivateBackend(deactivateError))
		{
			std::lock_guard lock(mutex);
			teardownInProgress = false;
			deferredUnload = false;
			lifecycleTransition = false;
			state = WupsPluginState::Failed;
			lastError.append(lastError.empty() ? "" : "; ")
				.append(deactivateError);
			if (errorOut)
				*errorOut = lastError;
			return false;
		}
		std::string releaseError;
		if (!ReleaseBackendResources(releaseError))
		{
			std::lock_guard lock(mutex);
			teardownInProgress = false;
			deferredUnload = false;
			lifecycleTransition = false;
			state = WupsPluginState::Failed;
			lastError.append(lastError.empty() ? "" : "; ")
				.append(releaseError);
			if (errorOut)
				*errorOut = lastError;
			return false;
		}
		RPLModule* unloadModule{};
		std::uint64_t unloadLifetime{};
		{
			std::lock_guard lock(mutex);
			unloadModule = module;
			unloadLifetime = moduleLifetime;
		}
		std::string unloadError;
		bool unloaded = true;
		if (unloadModule)
		{
			try
			{
				unloaded = moduleLoader->Unload(
					unloadModule, unloadLifetime, unloadError);
			}
			catch (const std::exception& exception)
			{
				unloaded = false;
				unloadError = fmt::format(
					"WUPS module unload threw an exception: {}", exception.what());
			}
			catch (...)
			{
				unloaded = false;
				unloadError = "WUPS module unload threw a non-standard exception";
			}
			if (!unloaded && unloadError.empty())
				unloadError = "WUPS module loader rejected unload";
		}
		{
			std::lock_guard lock(mutex);
			if (unloaded)
			{
				module = nullptr;
				moduleLifetime = 0;
			}
			teardownInProgress = false;
			deferredUnload = false;
			lifecycleTransition = false;
			foregroundTransition = false;
			exitRequestInProgress = false;
			state = unloaded ?
				(preserveFailure ? WupsPluginState::Failed : WupsPluginState::Unloaded) :
				WupsPluginState::Failed;
			if (!cleanupError.empty())
				lastError = cleanupError;
			if (!unloaded)
			{
				lastError.append(lastError.empty() ? "" : "; ").append(unloadError);
				unloadError = lastError;
			}
		}
		if (!unloaded && errorOut)
			*errorOut = unloadError;
		return unloaded;
	}

	bool FailStart(std::string& error)
	{
		{
			std::lock_guard lock(mutex);
			lastError = error;
			state = WupsPluginState::Failed;
			++generation;
			lifecycleTransition = false;
			foregroundTransition = false;
			exitRequestInProgress = false;
		}
		std::string unloadError;
		if (!FinishUnload(true, &unloadError))
			error.append(error.empty() ? "" : "; ")
				.append("WUPS unload failed: ").append(unloadError);
		return false;
	}
};

WupsPluginRuntime::WupsPluginRuntime(std::unique_ptr<Impl> impl) :
	m_impl(std::move(impl))
{
}

std::shared_ptr<WupsPluginRuntime> WupsPluginRuntime::Create(
	CemodPackage package, std::uint64_t owner, std::uint32_t generation,
	std::shared_ptr<IWupsRuntimeServices> services,
	std::shared_ptr<IWupsModuleLoader> moduleLoader, std::string& error)
{
	error.clear();
	if (!package.IsTrustedNative() ||
		package.manifest.payload.format != CemodPayloadFormat::Wups)
	{
		error = "WupsPluginRuntime requires a trusted_native WUPS payload";
		return {};
	}
	if (package.PayloadBytes().empty())
	{
		error = "WUPS payload is empty";
		return {};
	}
	WupsInspection inspection;
	if (package.wups)
		inspection = *package.wups;
	else
	{
		const auto parsed = WupsBinaryInspector::Inspect(package.PayloadBytes(), error);
		if (!parsed)
			return {};
		inspection = *parsed;
	}
	if (!services)
	{
		services = CreateRplAromaCompatibilityRuntime();
		if (!services)
			services = std::make_shared<AromaCompatibilityRuntime>();
	}
	if (!moduleLoader)
		moduleLoader = CreateRplWupsModuleLoader();
	if (!moduleLoader)
	{
		error = "WUPS RPL module loader is unavailable";
		return {};
	}
	return std::shared_ptr<WupsPluginRuntime>(new WupsPluginRuntime(
		std::make_unique<Impl>(std::move(package), std::move(inspection), owner,
			generation, std::move(services), std::move(moduleLoader))));
}

WupsPluginRuntime::~WupsPluginRuntime()
{
	std::string error;
	if (!UnloadChecked(error))
		cemuLog_log(LogType::Force,
			"WUPS: destructor could not unload package '{}' plugin '{}' owner {} "
			"generation {}; its RPL remains registered for title-wide cleanup: {}",
			m_impl->package.manifest.modId, m_impl->inspection.metadata.name,
			m_impl->owner, m_impl->resourceGeneration, error);
}

CemodPayloadFormat WupsPluginRuntime::Format() const
{
	return CemodPayloadFormat::Wups;
}

std::uint64_t WupsPluginRuntime::OwnerHandle() const
{
	return m_impl->owner;
}

std::uint32_t WupsPluginRuntime::Generation() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->generation;
}

WupsPluginState WupsPluginRuntime::State() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->state;
}

WupsMetadata WupsPluginRuntime::Metadata() const
{
	return m_impl->inspection.metadata;
}

std::string WupsPluginRuntime::LastError() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->lastError;
}

CemodPackage WupsPluginRuntime::PackageCopy() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->package;
}

bool WupsPluginRuntime::OnApplicationStarts(std::string& error)
{
	error.clear();
	std::string scopeReason;
	if (!m_impl->services->IsProcessInScope(m_impl->package, scopeReason))
		return true;
	if (!m_impl->inspection.replacements.empty() &&
		!m_impl->package.manifest.nativePermissions.functionPatching)
	{
		error = fmt::format(
			"package '{}' plugin '{}' contains function replacements but does "
			"not declare function_patching permission",
			m_impl->package.manifest.modId,
			m_impl->inspection.metadata.name);
		return m_impl->FailStart(error);
	}
	if (m_impl->inspection.usesFixedAddressPatches &&
		!m_impl->package.manifest.nativePermissions.physicalAddressPatching)
	{
		error = fmt::format(
			"package '{}' plugin '{}' contains fixed-address replacements but "
			"does not declare physical_address_patching permission",
			m_impl->package.manifest.modId,
			m_impl->inspection.metadata.name);
		return m_impl->FailStart(error);
	}

	WupsPluginState initialState;
	std::uint32_t startGeneration{};
	{
		std::lock_guard lock(m_impl->mutex);
		initialState = m_impl->state;
		if (initialState == WupsPluginState::Active)
			return true;
		if (initialState != WupsPluginState::Installed &&
			initialState != WupsPluginState::Deinitialized)
		{
			error = fmt::format("APPLICATION_STARTS is invalid in WUPS state {}",
				static_cast<unsigned>(initialState));
			m_impl->lastError = error;
			return false;
		}
		if (m_impl->lifecycleTransition)
		{
			error = "APPLICATION_STARTS rejected a concurrent lifecycle transition";
			m_impl->lastError = error;
			return false;
		}
		m_impl->lifecycleTransition = true;
		startGeneration = m_impl->generation;
		m_impl->backendResourcesReleased = false;
		m_impl->foregroundReleased = false;
		m_impl->exitRequested = false;
	}

	if (initialState == WupsPluginState::Installed)
	{
		RPLModule* mappedModule{};
		std::uint64_t lifetime{};
		const auto moduleName = fmt::format("cemod_wups_{:016x}.wps", m_impl->owner);
		if (!m_impl->services->BeginOwner(m_impl->package,
			m_impl->inspection.metadata, m_impl->owner,
			m_impl->resourceGeneration, error))
			return m_impl->FailStart(error);
		if (!m_impl->moduleLoader->Map(m_impl->package.PayloadBytes(), moduleName,
			m_impl->owner, m_impl->resourceGeneration, m_impl->package,
			m_impl->inspection.metadata, m_impl->services,
			mappedModule, lifetime, error))
			return m_impl->FailStart(error);
		bool revoked{};
		{
			std::lock_guard lock(m_impl->mutex);
			m_impl->module = mappedModule;
			m_impl->moduleLifetime = lifetime;
			revoked = m_impl->generation != startGeneration ||
				m_impl->state == WupsPluginState::Unloading;
			if (!revoked)
				m_impl->state = WupsPluginState::Mapped;
		}
		if (revoked)
		{
			error = "WUPS mapping completed after its lifecycle generation was revoked";
			std::string unloadError;
			if (!m_impl->FinishUnload(false, &unloadError))
				error.append("; WUPS unload failed: ").append(unloadError);
			return false;
		}
		if (!m_impl->services->BindGuestInvoker(
			m_impl->owner, m_impl->resourceGeneration,
			[loader = m_impl->moduleLoader, mappedModule, lifetime,
				scopedOwner = WupsOwnerToken{
					m_impl->owner, m_impl->resourceGeneration}](
				std::uint32_t target, std::span<const std::uint32_t> arguments,
				std::uint32_t& result, std::string& invokeError) {
				WupsGuestOwnerScope ownerScope{scopedOwner};
				return loader->Invoke(mappedModule, lifetime, target,
					arguments, result, invokeError);
				}, error))
			return m_impl->FailStart(error);
		{
			WupsGuestOwnerScope ownerScope{{
				m_impl->owner, m_impl->resourceGeneration}};
			if (!m_impl->moduleLoader->Relocate(mappedModule, lifetime, error))
				return m_impl->FailStart(error);
		}
		{
			std::lock_guard lock(m_impl->mutex);
			revoked = m_impl->generation != startGeneration ||
				m_impl->state == WupsPluginState::Unloading;
			if (!revoked)
				m_impl->state = WupsPluginState::Relocated;
		}
		if (revoked)
		{
			error = "WUPS relocation completed after its lifecycle generation was revoked";
			std::string unloadError;
			if (!m_impl->FinishUnload(false, &unloadError))
				error.append("; WUPS unload failed: ").append(unloadError);
			return false;
		}
	}

	std::vector<WupsPatchRequest> patchRequests;
	patchRequests.reserve(m_impl->inspection.replacements.size());
	for (std::size_t index = 0;
		index < m_impl->inspection.replacements.size(); ++index)
	{
		const auto& replacement = m_impl->inspection.replacements[index];
		if (replacement.entryType == WupsLoadEntryType::LegacyExport)
		{
			error = fmt::format(
				"package '{}' plugin '{}' uses unsupported legacy export "
				"replacement '{}'; it is not treated as successful",
				m_impl->package.manifest.modId,
				m_impl->inspection.metadata.name, replacement.name);
			return m_impl->FailStart(error);
		}
		std::uint32_t replacementAddress{};
		std::uint32_t callThroughStorage{};
		if (!m_impl->moduleLoader->ResolveAddress(
			m_impl->module, m_impl->moduleLifetime,
			replacement.target, 4, WupsSymbolKind::Function,
			replacementAddress, error) ||
			!m_impl->moduleLoader->ResolveAddress(
				m_impl->module, m_impl->moduleLifetime,
				replacement.callThroughStorage, 4, WupsSymbolKind::Data,
				callThroughStorage, error))
		{
			error = fmt::format(
				"package '{}' plugin '{}' replacement descriptor {} has a "
				"stale or wrongly typed guest address: {}",
				m_impl->package.manifest.modId,
				m_impl->inspection.metadata.name, index, error);
			return m_impl->FailStart(error);
		}
		WupsPatchRequest request;
		request.owner = {
			m_impl->owner, m_impl->resourceGeneration};
		request.descriptorIndex = index;
		request.mandatory = replacement.mandatory;
		request.functionName = replacement.name;
		request.replacementAddress = replacementAddress;
		request.callThroughStorage = callThroughStorage;
		request.process =
			static_cast<WupsPatchProcess>(replacement.processTarget);
		if (replacement.physicalAddress != 0)
		{
			request.targetKind = WupsPatchTargetKind::PhysicalAddress;
			request.physicalAddress = replacement.physicalAddress;
			request.virtualAddress = replacement.virtualAddress;
		}
		else if (replacement.virtualAddress != 0)
		{
			request.targetKind = WupsPatchTargetKind::VirtualAddress;
			request.virtualAddress = replacement.virtualAddress;
		}
		else
		{
			request.targetKind = WupsPatchTargetKind::NamedFunction;
			const auto library = WupsPatchLibraryName(replacement.library);
			if (!library)
			{
				error = fmt::format(
					"package '{}' plugin '{}' replacement descriptor {} has "
					"unsupported library {}",
					m_impl->package.manifest.modId,
					m_impl->inspection.metadata.name,
					index, replacement.library);
				return m_impl->FailStart(error);
			}
			request.moduleName = *library;
		}
		patchRequests.push_back(std::move(request));
	}
	if (!m_impl->services->ActivatePlugin(
		m_impl->package, m_impl->inspection.metadata,
		m_impl->owner, m_impl->resourceGeneration,
		patchRequests, error))
		return m_impl->FailStart(error);
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->pluginDeactivated = false;
	}

	static constexpr std::array initializerOrder{
		WupsHookType::InitReentFunctions,
		WupsHookType::InitWutMalloc,
		WupsHookType::InitWutNewlib,
		WupsHookType::InitWutStdcpp,
		WupsHookType::InitWutThread,
		WupsHookType::InitWutDevoptab,
		WupsHookType::InitWutSockets,
		WupsHookType::InitWrapper,
		WupsHookType::InitButtonCombo,
		WupsHookType::InitConfig,
		WupsHookType::InitStorageDeprecated,
		WupsHookType::InitStorage,
		WupsHookType::InitPlugin,
	};
	for (const auto type : initializerOrder)
	{
		bool invoked{};
		if (!m_impl->Invoke(type, false, invoked, error))
			return m_impl->FailStart(error);
		if (invoked)
		{
			std::lock_guard lock(m_impl->mutex);
			m_impl->completedInitializers.push_back(type);
		}
	}
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->generation != startGeneration ||
			m_impl->state == WupsPluginState::Unloading)
		{
			error = "WUPS initialization completed after its lifecycle generation was revoked";
		}
		else
			m_impl->state = WupsPluginState::Initialized;
	}
	if (!error.empty())
	{
		std::string unloadError;
		if (!m_impl->FinishUnload(false, &unloadError))
			error.append("; WUPS unload failed: ").append(unloadError);
		return false;
	}
	if (!m_impl->Invoke(WupsHookType::ApplicationStarts, false, error))
		return m_impl->FailStart(error);
	bool revoked{};
	{
		std::lock_guard lock(m_impl->mutex);
		revoked = m_impl->generation != startGeneration ||
			m_impl->state == WupsPluginState::Unloading;
		if (!revoked)
		{
			m_impl->state = WupsPluginState::Active;
			m_impl->lifecycleTransition = false;
		}
	}
	if (revoked)
	{
		error = "APPLICATION_STARTS completed after its lifecycle generation was revoked";
		std::string unloadError;
		if (!m_impl->FinishUnload(false, &unloadError))
			error.append("; WUPS unload failed: ").append(unloadError);
		return false;
	}
	return true;
}

void WupsPluginRuntime::OnReleaseForeground()
{
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->state != WupsPluginState::Active || m_impl->foregroundReleased ||
			m_impl->foregroundTransition)
			return;
		m_impl->foregroundTransition = true;
	}
	std::string error;
	if (m_impl->Invoke(WupsHookType::ReleaseForeground, false, error))
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->foregroundReleased = true;
		m_impl->foregroundTransition = false;
	}
	else
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->lastError = std::move(error);
		m_impl->foregroundTransition = false;
	}
}

void WupsPluginRuntime::OnAcquiredForeground()
{
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->state != WupsPluginState::Active || !m_impl->foregroundReleased ||
			m_impl->foregroundTransition)
			return;
		m_impl->foregroundTransition = true;
	}
	std::string error;
	if (m_impl->Invoke(WupsHookType::AcquiredForeground, false, error))
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->foregroundReleased = false;
		m_impl->foregroundTransition = false;
	}
	else
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->lastError = std::move(error);
		m_impl->foregroundTransition = false;
	}
}

void WupsPluginRuntime::OnApplicationRequestsExit()
{
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->state != WupsPluginState::Active || m_impl->exitRequested ||
			m_impl->exitRequestInProgress)
			return;
		m_impl->exitRequestInProgress = true;
	}
	std::string error;
	if (m_impl->Invoke(WupsHookType::ApplicationRequestsExit, false, error))
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->exitRequested = true;
		m_impl->exitRequestInProgress = false;
	}
	else
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->lastError = std::move(error);
		m_impl->exitRequestInProgress = false;
	}
}

void WupsPluginRuntime::OnApplicationEnds()
{
	{
		std::unique_lock lock(m_impl->mutex);
		if (m_impl->state != WupsPluginState::Active &&
			m_impl->state != WupsPluginState::Initialized)
			return;
		if (m_impl->lifecycleTransition)
			return;
		m_impl->lifecycleTransition = true;
		m_impl->state = WupsPluginState::Deinitialized;
		m_impl->applicationEndsPending = true;
		if (s_activeWupsRuntime == m_impl.get())
		{
			m_impl->deferredApplicationEnd = true;
			return;
		}
	}
	m_impl->FinishApplicationEnd();
}

void WupsPluginRuntime::Unload()
{
	std::string error;
	if (!UnloadChecked(error))
		cemuLog_log(LogType::Force,
			"WUPS: unload of package '{}' plugin '{}' owner {} generation {} "
			"did not complete: {}",
			m_impl->package.manifest.modId, m_impl->inspection.metadata.name,
			m_impl->owner, m_impl->resourceGeneration, error);
}

bool WupsPluginRuntime::UnloadChecked(std::string& error)
{
	error.clear();
	if (!m_impl)
		return true;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->state == WupsPluginState::Unloaded)
			return true;
		if (m_impl->teardownInProgress)
		{
			error = "WUPS unload is already in progress";
			return false;
		}
		const auto previousState = m_impl->state;
		if (previousState != WupsPluginState::Unloading)
		{
			m_impl->state = WupsPluginState::Unloading;
			++m_impl->generation;
			if (previousState == WupsPluginState::Active ||
				previousState == WupsPluginState::Initialized)
				m_impl->applicationEndsPending = true;
		}
		if (s_activeWupsRuntime == m_impl.get() || m_impl->lifecycleTransition)
		{
			m_impl->deferredUnload = true;
			error =
				"WUPS unload was deferred until the active lifecycle operation completes";
			return false;
		}
	}
	return m_impl->FinishUnload(false, &error);
}

struct WupsPayloadRuntime::Impl
{
	mutable std::mutex mutex;
	std::map<std::uint64_t, std::shared_ptr<WupsPluginRuntime>> plugins;
	std::shared_ptr<IWupsRuntimeServices> services;
	std::shared_ptr<IWupsModuleLoader> moduleLoader;
	std::uint64_t nextHandle{1};
	std::uint32_t nextGeneration{1};
	bool applicationActive{};
};

WupsPayloadRuntime::WupsPayloadRuntime(
	std::shared_ptr<IWupsRuntimeServices> services,
	std::shared_ptr<IWupsModuleLoader> moduleLoader) :
	m_impl(std::make_unique<Impl>())
{
	m_impl->services = services ? std::move(services) :
		CreateRplAromaCompatibilityRuntime();
	if (!m_impl->services)
		m_impl->services = std::make_shared<AromaCompatibilityRuntime>();
	m_impl->moduleLoader = moduleLoader ? std::move(moduleLoader) :
		CreateRplWupsModuleLoader();
}

WupsPayloadRuntime::~WupsPayloadRuntime()
{
	std::string error;
	if (!UnloadAll(error))
		cemuLog_log(LogType::Force,
			"WUPS: payload-runtime destruction left retryable RPL modules for "
			"title-wide cleanup: {}", error);
}

std::optional<std::uint64_t> WupsPayloadRuntime::Load(CemodPackage package,
	std::string& error)
{
	std::uint64_t handle{};
	std::uint32_t generation{};
	bool start{};
	{
		std::lock_guard lock(m_impl->mutex);
		for (const auto& [existingHandle, plugin] : m_impl->plugins)
			if (plugin->PackageCopy().manifest.modId == package.manifest.modId)
			{
				error = "duplicate WUPS mod_id";
				return std::nullopt;
			}
		handle = m_impl->nextHandle++;
		generation = m_impl->nextGeneration++;
		start = m_impl->applicationActive;
	}
	auto plugin = WupsPluginRuntime::Create(std::move(package), handle, generation,
		m_impl->services, m_impl->moduleLoader, error);
	if (!plugin)
		return std::nullopt;
	if (start && !plugin->OnApplicationStarts(error))
	{
		const std::string startError = error;
		std::string cleanupError;
		if (!plugin->UnloadChecked(cleanupError))
		{
			std::lock_guard lock(m_impl->mutex);
			m_impl->plugins.emplace(handle, std::move(plugin));
			error = fmt::format(
				"WUPS load failed ({}) and its failed runtime remains owned for "
				"unload retry because cleanup failed ({})",
				startError, cleanupError);
		}
		return std::nullopt;
	}
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->plugins.emplace(handle, std::move(plugin));
	}
	return handle;
}

bool WupsPayloadRuntime::Reload(std::uint64_t handle, CemodPackage package,
	std::string& error)
{
	error.clear();
	auto oldPlugin = Find(handle);
	if (!oldPlugin)
	{
		error = "WUPS reload handle does not exist";
		return false;
	}
	const auto rollbackPackage = oldPlugin->PackageCopy();
	bool start{};
	std::uint32_t generation{};
	{
		std::lock_guard lock(m_impl->mutex);
		start = m_impl->applicationActive;
	}
	std::string unloadError;
	if (!oldPlugin->UnloadChecked(unloadError))
	{
		error = fmt::format(
			"WUPS reload could not unload the previous generation; it remains retryable: {}",
			unloadError);
		return false;
	}
	{
		std::lock_guard lock(m_impl->mutex);
		const auto found = m_impl->plugins.find(handle);
		if (found == m_impl->plugins.end() || found->second != oldPlugin)
		{
			error = "WUPS reload handle changed during previous-generation unload";
			return false;
		}
		m_impl->plugins.erase(found);
		generation = m_impl->nextGeneration++;
	}
	auto replacement = WupsPluginRuntime::Create(std::move(package), handle, generation,
		m_impl->services, m_impl->moduleLoader, error);
	if (replacement && (!start || replacement->OnApplicationStarts(error)))
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->plugins.emplace(handle, std::move(replacement));
		return true;
	}
	const std::string replacementError = error;
	if (replacement)
	{
		std::string cleanupError;
		if (!replacement->UnloadChecked(cleanupError))
		{
			std::lock_guard lock(m_impl->mutex);
			m_impl->plugins.emplace(handle, std::move(replacement));
			error = fmt::format(
				"WUPS reload failed ({}) and the failed replacement remains retryable "
				"because unload failed ({})",
				replacementError, cleanupError);
			return false;
		}
	}
	std::string rollbackError;
	std::uint32_t rollbackGeneration{};
	{
		std::lock_guard lock(m_impl->mutex);
		rollbackGeneration = m_impl->nextGeneration++;
	}
	auto rollback = WupsPluginRuntime::Create(rollbackPackage, handle,
		rollbackGeneration, m_impl->services, m_impl->moduleLoader, rollbackError);
	if (rollback && (!start || rollback->OnApplicationStarts(rollbackError)))
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->plugins.emplace(handle, std::move(rollback));
		error = fmt::format("WUPS reload failed and previous generation was restored: {}",
			replacementError);
		return false;
	}
	if (rollback)
	{
		std::string cleanupError;
		if (!rollback->UnloadChecked(cleanupError))
		{
			std::lock_guard lock(m_impl->mutex);
			m_impl->plugins.emplace(handle, std::move(rollback));
			error = fmt::format(
				"WUPS reload failed ({}), rollback failed ({}), and the failed rollback "
				"remains retryable because unload failed ({})",
				replacementError, rollbackError, cleanupError);
			return false;
		}
	}
	error = fmt::format("WUPS reload failed ({}) and rollback failed ({})",
		replacementError, rollbackError);
	return false;
}

bool WupsPayloadRuntime::Unload(std::uint64_t handle)
{
	std::string error;
	return Unload(handle, error);
}

bool WupsPayloadRuntime::Unload(std::uint64_t handle, std::string& error)
{
	error.clear();
	std::shared_ptr<WupsPluginRuntime> plugin;
	{
		std::lock_guard lock(m_impl->mutex);
		const auto found = m_impl->plugins.find(handle);
		if (found == m_impl->plugins.end())
		{
			error = "WUPS unload handle does not exist";
			return false;
		}
		plugin = found->second;
	}
	if (!plugin->UnloadChecked(error))
		return false;
	{
		std::lock_guard lock(m_impl->mutex);
		const auto found = m_impl->plugins.find(handle);
		if (found != m_impl->plugins.end() && found->second == plugin)
			m_impl->plugins.erase(found);
	}
	return true;
}

void WupsPayloadRuntime::UnloadAll()
{
	std::string error;
	if (!UnloadAll(error))
		cemuLog_log(LogType::Force,
			"WUPS: unload-all did not complete; failed plugins remain retryable: {}",
			error);
}

bool WupsPayloadRuntime::UnloadAll(std::string& error)
{
	std::vector<std::pair<std::uint64_t, std::shared_ptr<WupsPluginRuntime>>> plugins;
	{
		std::lock_guard lock(m_impl->mutex);
		for (auto& [handle, plugin] : std::ranges::reverse_view(m_impl->plugins))
			plugins.emplace_back(handle, plugin);
		m_impl->applicationActive = false;
	}
	error.clear();
	bool success = true;
	for (const auto& [handle, plugin] : plugins)
	{
		std::string pluginError;
		if (!plugin->UnloadChecked(pluginError))
		{
			success = false;
			error.append(error.empty() ? "" : "; ").append(plugin->Metadata().name)
				.append(": ").append(pluginError);
			continue;
		}
		std::lock_guard lock(m_impl->mutex);
		const auto found = m_impl->plugins.find(handle);
		if (found != m_impl->plugins.end() && found->second == plugin)
			m_impl->plugins.erase(found);
	}
	return success;
}

bool WupsPayloadRuntime::OnApplicationStarts(std::string& error)
{
	std::vector<std::shared_ptr<WupsPluginRuntime>> plugins;
	{
		std::lock_guard lock(m_impl->mutex);
		for (const auto& [handle, plugin] : m_impl->plugins)
			plugins.push_back(plugin);
		m_impl->applicationActive = true;
	}
	bool success = true;
	error.clear();
	for (const auto& plugin : plugins)
	{
		std::string pluginError;
		if (!plugin->OnApplicationStarts(pluginError))
		{
			success = false;
			error.append(error.empty() ? "" : "; ").append(plugin->Metadata().name)
				.append(": ").append(pluginError);
		}
	}
	return success;
}

void WupsPayloadRuntime::OnReleaseForeground()
{
	std::vector<std::shared_ptr<WupsPluginRuntime>> plugins;
	{
		std::lock_guard lock(m_impl->mutex);
		for (const auto& [handle, plugin] : m_impl->plugins)
			plugins.push_back(plugin);
	}
	for (const auto& plugin : plugins)
		plugin->OnReleaseForeground();
}

void WupsPayloadRuntime::OnAcquiredForeground()
{
	std::vector<std::shared_ptr<WupsPluginRuntime>> plugins;
	{
		std::lock_guard lock(m_impl->mutex);
		for (const auto& [handle, plugin] : m_impl->plugins)
			plugins.push_back(plugin);
	}
	for (const auto& plugin : plugins)
		plugin->OnAcquiredForeground();
}

void WupsPayloadRuntime::OnApplicationRequestsExit()
{
	std::vector<std::shared_ptr<WupsPluginRuntime>> plugins;
	{
		std::lock_guard lock(m_impl->mutex);
		for (const auto& [handle, plugin] : m_impl->plugins)
			plugins.push_back(plugin);
	}
	for (const auto& plugin : plugins)
		plugin->OnApplicationRequestsExit();
}

void WupsPayloadRuntime::OnApplicationEnds()
{
	std::vector<std::shared_ptr<WupsPluginRuntime>> plugins;
	{
		std::lock_guard lock(m_impl->mutex);
		for (const auto& [handle, plugin] : std::ranges::reverse_view(m_impl->plugins))
			plugins.push_back(plugin);
		m_impl->applicationActive = false;
	}
	for (const auto& plugin : plugins)
		plugin->OnApplicationEnds();
}

std::shared_ptr<WupsPluginRuntime> WupsPayloadRuntime::Find(std::uint64_t handle) const
{
	std::lock_guard lock(m_impl->mutex);
	const auto found = m_impl->plugins.find(handle);
	return found == m_impl->plugins.end() ? nullptr : found->second;
}

std::size_t WupsPayloadRuntime::Size() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->plugins.size();
}
