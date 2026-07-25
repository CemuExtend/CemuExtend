#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsRuntime.h"
#include "Cafe/OS/RPL/RPLExternalModulePolicy.h"
#include "Cafe/OS/RPL/RPLTLSMapping.h"
#include "Cemu/Logging/CemuLogging.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

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
		std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
		std::abort();
	}
#define CHECK(condition) do { if (!(condition)) CheckFailed(#condition, __LINE__); } while (false)

	constexpr std::uint32_t HookTarget(WupsHookType type)
	{
		return 0x1000U + static_cast<std::uint32_t>(type) * 4U;
	}

	struct RuntimeLog
	{
			std::vector<std::uint64_t> mappedOwners;
			std::vector<std::uint32_t> invokedTargets;
			std::vector<std::uint64_t> unloadLifetimes;
		std::vector<std::pair<std::uint64_t, std::uint32_t>> releases;
		std::uint32_t relocations{};
			std::uint32_t unloads{};
			std::uint32_t failNextMaps{};
			std::uint32_t failNextUnloads{};
			std::atomic_uint32_t releaseCalls{};
			std::atomic_uint32_t unloadCalls{};
			std::atomic_uint32_t deactivationCalls{};
			std::uint32_t failNextDeactivations{};
			std::optional<std::uint32_t> failTarget;
			std::function<void()> onMap;
			std::function<void(WupsHookType)> onPrepare;
			std::function<void(std::uint32_t)> onInvoke;
			bool throwNextPrepare{};
			bool throwNextInvoke{};
			bool inScope{true};
	};

	class FakeServices final : public IWupsRuntimeServices
	{
	public:
		explicit FakeServices(std::shared_ptr<RuntimeLog> log) : m_log(std::move(log)) {}

		std::optional<std::uint32_t> ResolveImport(
			const CemodPackage&, const WupsMetadata&, std::uint64_t,
			std::uint32_t, std::string_view, std::string_view,
			WupsSymbolKind, std::string&) override
		{
			return std::nullopt;
		}

		bool PrepareHookInvocation(const CemodPackage&, const WupsMetadata&,
				std::uint64_t, std::uint32_t, WupsHookType type,
				WupsHookInvocation& invocation, std::string&) override
			{
				if (m_log->onPrepare)
					m_log->onPrepare(type);
				if (m_log->throwNextPrepare)
				{
					m_log->throwNextPrepare = false;
					throw std::runtime_error("injected prepare exception");
				}
				invocation = {};
				return true;
		}

		bool ActivatePlugin(const CemodPackage&, const WupsMetadata&,
			std::uint64_t, std::uint32_t,
			std::span<const WupsPatchRequest>, std::string&) override
		{
			return true;
		}

		bool DeactivatePlugin(std::uint64_t, std::uint32_t,
			std::string& error) override
		{
			++m_log->deactivationCalls;
			if (m_log->failNextDeactivations != 0)
			{
				--m_log->failNextDeactivations;
				error = "injected patch restoration failure";
				return false;
			}
			return true;
		}

		bool ReleaseOwnerResources(std::uint64_t owner,
			std::uint32_t generation, std::string&) override
		{
			++m_log->releaseCalls;
			m_log->releases.emplace_back(owner, generation);
			return true;
		}

		bool IsProcessInScope(const CemodPackage&, std::string& reason) const override
		{
			if (!m_log->inScope)
				reason = "test process is outside scope";
			return m_log->inScope;
		}

	private:
		std::shared_ptr<RuntimeLog> m_log;
	};

	class FakeModuleLoader final : public IWupsModuleLoader
	{
	public:
		explicit FakeModuleLoader(std::shared_ptr<RuntimeLog> log) : m_log(std::move(log)) {}

		bool Map(std::span<const std::byte>, std::string_view,
			std::uint64_t owner, std::uint32_t generation,
			const CemodPackage&, const WupsMetadata&,
			const std::shared_ptr<IWupsRuntimeServices>&,
			RPLModule*& module, std::uint64_t& lifetimeId, std::string& error) override
		{
			m_log->mappedOwners.push_back(owner);
			if (m_log->failNextMaps != 0)
			{
				--m_log->failNextMaps;
				error = "injected map failure";
				return false;
			}
			// The runtime treats this as an opaque token; the fake never dereferences it.
			module = reinterpret_cast<RPLModule*>(
				static_cast<std::uintptr_t>(0x10000U + owner * 0x100U + generation));
			lifetimeId = (owner << 32U) | generation;
			if (m_log->onMap)
				m_log->onMap();
			return true;
		}

		bool Relocate(RPLModule*, std::uint64_t, std::string&) override
		{
			++m_log->relocations;
			return true;
		}

		bool Invoke(RPLModule*, std::uint64_t, std::uint32_t target,
			std::span<const std::uint32_t>, std::uint32_t& result,
			std::string& error) override
		{
			m_log->invokedTargets.push_back(target);
			result = 0;
				if (m_log->onInvoke)
					m_log->onInvoke(target);
				if (m_log->throwNextInvoke)
				{
					m_log->throwNextInvoke = false;
					throw std::runtime_error("injected invoke exception");
				}
				if (m_log->failTarget == target)
			{
				error = "injected guest callback failure";
				return false;
			}
			return true;
		}

		bool ResolveAddress(RPLModule*, std::uint64_t,
			std::uint32_t virtualAddress, std::uint32_t,
			WupsSymbolKind, std::uint32_t& mappedAddress,
			std::string&) override
		{
			mappedAddress = virtualAddress;
			return true;
		}

			bool Unload(RPLModule*, std::uint64_t lifetimeId, std::string& error) override
			{
				++m_log->unloadCalls;
				++m_log->unloads;
				m_log->unloadLifetimes.push_back(lifetimeId);
				if (m_log->failNextUnloads != 0)
				{
					--m_log->failNextUnloads;
					error = "injected unload failure";
					return false;
				}
				return true;
		}

	private:
		std::shared_ptr<RuntimeLog> m_log;
	};

	std::vector<WupsHookType> LifecycleHooks()
	{
		return {
			WupsHookType::InitReentFunctions,
			WupsHookType::InitWutMalloc,
			WupsHookType::FiniWutMalloc,
			WupsHookType::InitWutNewlib,
			WupsHookType::FiniWutNewlib,
			WupsHookType::InitWutStdcpp,
			WupsHookType::FiniWutStdcpp,
			WupsHookType::InitWutThread,
			WupsHookType::InitWutDevoptab,
			WupsHookType::FiniWutDevoptab,
			WupsHookType::InitWutSockets,
			WupsHookType::FiniWutSockets,
			WupsHookType::InitWrapper,
			WupsHookType::FiniWrapper,
			WupsHookType::InitButtonCombo,
			WupsHookType::InitConfig,
			WupsHookType::InitStorageDeprecated,
			WupsHookType::InitStorage,
			WupsHookType::InitPlugin,
			WupsHookType::DeinitPlugin,
			WupsHookType::ApplicationStarts,
			WupsHookType::ReleaseForeground,
			WupsHookType::AcquiredForeground,
			WupsHookType::ApplicationRequestsExit,
			WupsHookType::ApplicationEnds,
		};
	}

	CemodPackage Package(std::string id, std::string name,
		std::span<const WupsHookType> hooks = {})
	{
		CemodPackage package;
		package.manifest.packageVersion = 2;
		package.manifest.apiVersion = 2;
		package.manifest.executionMode = CemodExecutionMode::TrustedNative;
		package.manifest.payload.format = CemodPayloadFormat::Wups;
		package.manifest.payload.path = "plugin.wps";
		package.manifest.modId = std::move(id);
		package.payload.push_back(std::byte{0x7f});
		WupsInspection inspection;
		inspection.metadata.name = std::move(name);
		inspection.metadata.abiVersion = {0, 9, 1};
		for (const auto type : hooks)
			inspection.hooks.push_back({type, HookTarget(type)});
		package.wups = std::move(inspection);
		return package;
	}

	void CheckTargets(const std::vector<std::uint32_t>& actual,
		std::span<const WupsHookType> expected)
	{
		CHECK(actual.size() == expected.size());
		for (std::size_t index = 0; index < expected.size(); ++index)
			CHECK(actual[index] == HookTarget(expected[index]));
	}

	void TestLifecycleOrderAndUnloadBarrier()
	{
		const auto allHooks = LifecycleHooks();
		auto log = std::make_shared<RuntimeLog>();
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("lifecycle", "Lifecycle", allHooks), 7, 11,
			services, loader, error);
		CHECK(runtime);
		CHECK(runtime->State() == WupsPluginState::Installed);
		CHECK(runtime->OnApplicationStarts(error));
		CHECK(runtime->State() == WupsPluginState::Active);
		CHECK(log->relocations == 1);

		const std::vector<WupsHookType> startOrder{
			WupsHookType::InitWutMalloc,
			WupsHookType::InitReentFunctions,
			WupsHookType::InitWutNewlib,
			WupsHookType::InitWutStdcpp,
			WupsHookType::InitWutDevoptab,
			WupsHookType::InitWutSockets,
			WupsHookType::InitWutThread,
			WupsHookType::InitWrapper,
			WupsHookType::InitButtonCombo,
			WupsHookType::InitConfig,
			WupsHookType::InitStorageDeprecated,
			WupsHookType::InitStorage,
			WupsHookType::InitPlugin,
			WupsHookType::ApplicationStarts,
		};
		CheckTargets(log->invokedTargets, startOrder);

		runtime->OnReleaseForeground();
		runtime->OnReleaseForeground();
		runtime->OnAcquiredForeground();
		runtime->OnApplicationRequestsExit();
		runtime->OnApplicationRequestsExit();
		runtime->OnApplicationEnds();
		CHECK(runtime->State() == WupsPluginState::Deinitialized);

		std::vector<WupsHookType> completeOrder = startOrder;
		completeOrder.insert(completeOrder.end(), {
			WupsHookType::ReleaseForeground,
			WupsHookType::AcquiredForeground,
			WupsHookType::ApplicationRequestsExit,
			WupsHookType::ApplicationEnds,
			WupsHookType::DeinitPlugin,
			WupsHookType::FiniWrapper,
			WupsHookType::FiniWutSockets,
			WupsHookType::FiniWutDevoptab,
			WupsHookType::FiniWutStdcpp,
			WupsHookType::FiniWutNewlib,
			WupsHookType::FiniWutMalloc,
		});
		CheckTargets(log->invokedTargets, completeOrder);
		CHECK(log->releases.empty());

		runtime->Unload();
		CHECK(runtime->State() == WupsPluginState::Unloaded);
		CHECK(runtime->Generation() == 12);
		CHECK(log->unloads == 1);
		CHECK(log->releases.size() == 1);
		CHECK(log->releases[0].first == 7);
		CHECK(log->releases[0].second == 11);
		const auto callbacksBeforeStaleEvents = log->invokedTargets.size();
		runtime->OnReleaseForeground();
		runtime->OnAcquiredForeground();
		runtime->OnApplicationRequestsExit();
		runtime->OnApplicationEnds();
		CHECK(log->invokedTargets.size() == callbacksBeforeStaleEvents);
		CHECK(!runtime->OnApplicationStarts(error));
		CHECK(log->invokedTargets.size() == callbacksBeforeStaleEvents);
	}

	void TestLifecycleRollback()
	{
		const auto allHooks = LifecycleHooks();
		auto log = std::make_shared<RuntimeLog>();
		log->failTarget = HookTarget(WupsHookType::InitWutSockets);
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("rollback", "Rollback", allHooks), 8, 20,
			services, loader, error);
		CHECK(runtime);
		CHECK(!runtime->OnApplicationStarts(error));
		CHECK(error.find("injected guest callback failure") != std::string::npos);
		CHECK(runtime->State() == WupsPluginState::Failed);
		CHECK(runtime->Generation() == 21);
		CHECK(log->unloads == 1);
		CHECK(log->releases.size() == 1);
		CHECK(log->releases[0].first == 8);
		CHECK(log->releases[0].second == 20);
		const std::vector<WupsHookType> expected{
			WupsHookType::InitReentFunctions,
			WupsHookType::InitWutMalloc,
			WupsHookType::InitWutNewlib,
			WupsHookType::InitWutStdcpp,
			WupsHookType::InitWutThread,
			WupsHookType::InitWutDevoptab,
			WupsHookType::InitWutSockets,
			WupsHookType::FiniWutDevoptab,
			WupsHookType::FiniWutStdcpp,
			WupsHookType::FiniWutNewlib,
			WupsHookType::FiniWutMalloc,
		};
		CheckTargets(log->invokedTargets, expected);
	}

	void TestApplicationRestartKeepsOwnerResources()
	{
		auto log = std::make_shared<RuntimeLog>();
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("restart", "Restart"), 71, 12, services, loader, error);
		CHECK(runtime);
		CHECK(runtime->OnApplicationStarts(error));
		CHECK(log->mappedOwners.size() == 1);
		runtime->OnApplicationEnds();
		CHECK(runtime->State() == WupsPluginState::Deinitialized);
		CHECK(log->deactivationCalls == 1);
		CHECK(log->releaseCalls == 0);
		CHECK(runtime->OnApplicationStarts(error));
		CHECK(log->mappedOwners.size() == 1);
		runtime->OnApplicationEnds();
		CHECK(log->deactivationCalls == 2);
		CHECK(runtime->UnloadChecked(error));
		CHECK(log->releaseCalls == 1);
		CHECK(log->unloadCalls == 1);
	}

	void TestPatchRestoreFailureBlocksModuleUnload()
	{
		auto log = std::make_shared<RuntimeLog>();
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("patch_retry", "Patch Retry"), 72, 13,
			services, loader, error);
		CHECK(runtime);
		CHECK(runtime->OnApplicationStarts(error));
		log->failNextDeactivations = 1;
		CHECK(!runtime->UnloadChecked(error));
		CHECK(error.find("patch restoration failure") != std::string::npos);
		CHECK(runtime->State() == WupsPluginState::Failed);
		CHECK(log->releaseCalls == 0);
		CHECK(log->unloadCalls == 0);
		CHECK(runtime->UnloadChecked(error));
		CHECK(log->releaseCalls == 1);
		CHECK(log->unloadCalls == 1);
	}

	void TestReentrantUnloadIsDeferred()
	{
		const std::array hooks{
			WupsHookType::ApplicationStarts,
			WupsHookType::ApplicationEnds,
		};
		auto log = std::make_shared<RuntimeLog>();
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("reentrant", "Reentrant", hooks), 81, 22,
			services, loader, error);
		CHECK(runtime);
		CHECK(runtime->OnApplicationStarts(error));
		log->onInvoke = [&runtime](std::uint32_t target) {
			if (target == HookTarget(WupsHookType::ApplicationEnds))
				runtime->Unload();
		};
		runtime->OnApplicationEnds();
		log->onInvoke = {};
		CHECK(runtime->State() == WupsPluginState::Unloaded);
		CHECK(log->unloads == 1);
	}

	void TestUnloadRevokesAndDrainsOtherThreadCallback()
	{
		const std::array hooks{
			WupsHookType::InitWutMalloc,
			WupsHookType::FiniWutMalloc,
			WupsHookType::ApplicationStarts,
			WupsHookType::ReleaseForeground,
			WupsHookType::ApplicationEnds,
		};
		auto log = std::make_shared<RuntimeLog>();
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("callback_drain", "Callback Drain", hooks), 86, 27,
			services, loader, error);
		CHECK(runtime);
		CHECK(runtime->OnApplicationStarts(error));

		std::mutex callbackMutex;
		std::condition_variable callbackChanged;
		bool callbackEntered{};
		bool releaseCallback{};
		log->onInvoke = [&](std::uint32_t target) {
			if (target != HookTarget(WupsHookType::ReleaseForeground))
				return;
			std::unique_lock lock(callbackMutex);
			callbackEntered = true;
			callbackChanged.notify_all();
			callbackChanged.wait(lock, [&] { return releaseCallback; });
		};

		std::thread callbackThread([&] { runtime->OnReleaseForeground(); });
		{
			std::unique_lock lock(callbackMutex);
			callbackChanged.wait(lock, [&] { return callbackEntered; });
		}

		std::atomic_bool unloadFinished{};
		bool unloadResult{};
		std::string unloadError;
		std::thread unloadThread([&] {
			unloadResult = runtime->UnloadChecked(unloadError);
			unloadFinished = true;
		});
		while (runtime->State() != WupsPluginState::Unloading)
			std::this_thread::yield();
		CHECK(!unloadFinished.load());
		CHECK(log->releaseCalls.load() == 0);
		CHECK(log->unloadCalls.load() == 0);

		// State revocation must reject new callbacks while the prior callback is
		// draining.
		runtime->OnAcquiredForeground();
		runtime->OnApplicationRequestsExit();

		{
			std::lock_guard lock(callbackMutex);
			releaseCallback = true;
		}
		callbackChanged.notify_all();
		callbackThread.join();
		unloadThread.join();
		log->onInvoke = {};

		CHECK(unloadResult);
		CHECK(unloadError.empty());
		CHECK(runtime->State() == WupsPluginState::Unloaded);
		CHECK(log->releaseCalls.load() == 1);
		CHECK(log->unloadCalls.load() == 1);
		const std::vector<WupsHookType> expected{
			WupsHookType::InitWutMalloc,
			WupsHookType::ApplicationStarts,
			WupsHookType::ReleaseForeground,
			WupsHookType::ApplicationEnds,
			WupsHookType::FiniWutMalloc,
		};
		CheckTargets(log->invokedTargets, expected);
	}

	void TestUnloadDuringMappingDoesNotPublishStaleModule()
	{
		auto log = std::make_shared<RuntimeLog>();
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("map_race", "Map Race"), 82, 23,
			services, loader, error);
		CHECK(runtime);
		log->onMap = [&runtime] { runtime->Unload(); };
		CHECK(!runtime->OnApplicationStarts(error));
		log->onMap = {};
		CHECK(error.find("generation was revoked") != std::string::npos);
		CHECK(runtime->State() == WupsPluginState::Unloaded);
		CHECK(log->unloads == 1);
	}

	void TestPrepareReentrantUnloadAndExceptionBalance()
	{
		const std::array hooks{WupsHookType::ReleaseForeground};
		auto log = std::make_shared<RuntimeLog>();
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("prepare_reentrant", "Prepare Reentrant", hooks), 83, 24,
			services, loader, error);
		CHECK(runtime);
		CHECK(runtime->OnApplicationStarts(error));
		log->onPrepare = [&runtime](WupsHookType type) {
			if (type == WupsHookType::ReleaseForeground)
				runtime->Unload();
		};
		runtime->OnReleaseForeground();
		log->onPrepare = {};
		CHECK(runtime->State() == WupsPluginState::Unloaded);
		CHECK(log->unloads == 1);
		CHECK(log->invokedTargets.empty());

		auto exceptionRuntime = WupsPluginRuntime::Create(
			Package("callback_exceptions", "Callback Exceptions", hooks), 84, 25,
			services, loader, error);
		CHECK(exceptionRuntime);
		CHECK(exceptionRuntime->OnApplicationStarts(error));
		log->throwNextPrepare = true;
		exceptionRuntime->OnReleaseForeground();
		CHECK(exceptionRuntime->State() == WupsPluginState::Active);
		CHECK(exceptionRuntime->LastError().find("prepare exception") != std::string::npos);
		log->throwNextInvoke = true;
		exceptionRuntime->OnReleaseForeground();
		CHECK(exceptionRuntime->State() == WupsPluginState::Active);
		CHECK(exceptionRuntime->LastError().find("invoke exception") != std::string::npos);
		CHECK(exceptionRuntime->UnloadChecked(error));
		CHECK(exceptionRuntime->State() == WupsPluginState::Unloaded);
	}

	void TestUnloadFailurePreservesRetryAuthority()
	{
		auto log = std::make_shared<RuntimeLog>();
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("unload_retry", "Unload Retry"), 85, 26,
			services, loader, error);
		CHECK(runtime);
		CHECK(runtime->OnApplicationStarts(error));
		log->failNextUnloads = 1;
		CHECK(!runtime->UnloadChecked(error));
		CHECK(error.find("injected unload failure") != std::string::npos);
		CHECK(runtime->State() == WupsPluginState::Failed);
		CHECK(runtime->LastError().find("injected unload failure") != std::string::npos);
		CHECK(log->unloads == 1);
		CHECK(runtime->UnloadChecked(error));
		CHECK(runtime->State() == WupsPluginState::Unloaded);
		CHECK(log->unloads == 2);
		CHECK(log->unloadLifetimes[0] == ((std::uint64_t{85} << 32U) | 26U));
		CHECK(log->unloadLifetimes[1] == log->unloadLifetimes[0]);

		WupsPayloadRuntime manager(services, loader);
		const auto unloadHandle = manager.Load(Package("manager_unload", "Manager Unload"),
			error);
		CHECK(unloadHandle);
		CHECK(manager.OnApplicationStarts(error));
		log->failNextUnloads = 1;
		CHECK(!manager.Unload(*unloadHandle, error));
		CHECK(error.find("injected unload failure") != std::string::npos);
		CHECK(manager.Size() == 1);
		CHECK(manager.Find(*unloadHandle)->State() == WupsPluginState::Failed);
		CHECK(manager.Unload(*unloadHandle, error));
		CHECK(manager.Size() == 0);

		WupsPayloadRuntime failedLoadManager(services, loader);
		CHECK(failedLoadManager.OnApplicationStarts(error));
		const std::array applicationStartHook{WupsHookType::ApplicationStarts};
		log->failTarget = HookTarget(WupsHookType::ApplicationStarts);
		log->failNextUnloads = 2;
		CHECK(!failedLoadManager.Load(
			Package("failed_load_cleanup", "Failed Load Cleanup", applicationStartHook),
			error));
		CHECK(error.find("remains owned for unload retry") != std::string::npos);
		CHECK(failedLoadManager.Size() == 1);
		log->failTarget.reset();
		CHECK(failedLoadManager.UnloadAll(error));
		CHECK(failedLoadManager.Size() == 0);

		const auto reloadHandle = manager.Load(Package("manager_reload", "Reload Original"),
			error);
		CHECK(reloadHandle);
		CHECK(manager.OnApplicationStarts(error));
		log->failNextUnloads = 1;
		CHECK(!manager.Reload(*reloadHandle,
			Package("manager_reload", "Reload Replacement"), error));
		CHECK(error.find("remains retryable") != std::string::npos);
		CHECK(manager.Size() == 1);
		CHECK(manager.Find(*reloadHandle)->Metadata().name == "Reload Original");
		CHECK(manager.Find(*reloadHandle)->State() == WupsPluginState::Failed);
		CHECK(manager.Unload(*reloadHandle, error));

		const auto first = manager.Load(Package("unload_all_first", "Unload All First"),
			error);
		const auto second = manager.Load(Package("unload_all_second", "Unload All Second"),
			error);
		CHECK(first && second);
		log->failNextUnloads = 1;
		CHECK(!manager.UnloadAll(error));
		CHECK(error.find("injected unload failure") != std::string::npos);
		CHECK(manager.Size() == 1);
		CHECK(manager.UnloadAll(error));
		CHECK(manager.Size() == 0);
	}

	void TestContiguousTlsTemplateMapping()
	{
		using RPLLoaderInternal::ResolveContiguousTLSMapping;
		using RPLLoaderInternal::TLSSectionMapping;
		const std::array adjacent{
			TLSSectionMapping{0x1000, 0x18, 0x5000},
			TLSSectionMapping{0x1018, 0x28, 0x5018},
		};
		const auto mapping = ResolveContiguousTLSMapping(adjacent);
		CHECK(mapping);
		CHECK(mapping->virtualAddress == 0x1000);
		CHECK(mapping->mappedAddress == 0x5000);
		CHECK(mapping->size == 0x40);

		const std::array virtualGap{
			TLSSectionMapping{0x1000, 0x18, 0x5000},
			TLSSectionMapping{0x1020, 0x20, 0x5018},
		};
		CHECK(!ResolveContiguousTLSMapping(virtualGap));
		const std::array mappedGap{
			TLSSectionMapping{0x1000, 0x18, 0x5000},
			TLSSectionMapping{0x1018, 0x28, 0x5020},
		};
		CHECK(!ResolveContiguousTLSMapping(mappedGap));
	}

	void TestExternalModulePolicy()
	{
		using namespace RPLLoaderInternal;
		CHECK(IsVisibleThroughOrdinaryModuleScan(false, false));
		CHECK(IsVisibleThroughOrdinaryModuleScan(true, true));
		CHECK(!IsVisibleThroughOrdinaryModuleScan(true, false));

		CHECK(ClassifyExternalSectionMapping({
			1, kSectionFlagAlloc | kSectionFlagExecute, 0x02000000, 4}) ==
			ExternalMappingRegion::Text);
		CHECK(ClassifyExternalSectionMapping({
			kSectionTypeImports, kSectionFlagAlloc | kSectionFlagExecute,
			0xc0000000, 16}) == ExternalMappingRegion::Loader);
		CHECK(ClassifyExternalSectionMapping({
			kSectionTypeExports,
			kSectionFlagAlloc | kSectionFlagWrite | kSectionFlagExecute,
			0x10000000, 16}) == ExternalMappingRegion::Data);

		const std::array sections{
			ExternalSectionMapping{
				1, kSectionFlagAlloc | kSectionFlagExecute, 0x02000000, 0x100},
			ExternalSectionMapping{
				1, kSectionFlagAlloc | kSectionFlagWrite, 0x10000000, 0x80},
			ExternalSectionMapping{
				1, kSectionFlagAlloc | kSectionFlagWrite, 0x10000180, 0x80},
			ExternalSectionMapping{
				kSectionTypeImports, kSectionFlagAlloc | kSectionFlagExecute,
				0xc0000000, 0x20},
		};
		const ExternalFileInfoMapping exactBoundary{
			0x180, 0x200, 0x30, 0x80, 0x10};
		CHECK(!FindExternalMappingViolation(sections, exactBoundary));

		auto shortDataRegion = exactBoundary;
		--shortDataRegion.dataRegionSize;
		const auto dataViolation =
			FindExternalMappingViolation(sections, shortDataRegion);
		CHECK(dataViolation);
		CHECK(dataViolation->sectionIndex == 2);
		CHECK(dataViolation->region == ExternalMappingRegion::Data);
		CHECK(dataViolation->sectionEnd == 0x10000200);
		CHECK(dataViolation->regionEnd == 0x100001ff);

		auto shortLoaderRegion = exactBoundary;
		++shortLoaderRegion.loaderAdjustment;
		const auto loaderViolation =
			FindExternalMappingViolation(sections, shortLoaderRegion);
		CHECK(loaderViolation);
		CHECK(loaderViolation->sectionIndex == 3);
		CHECK(loaderViolation->region == ExternalMappingRegion::Loader);

		const std::array highSection{
			ExternalSectionMapping{
				1, kSectionFlagAlloc | kSectionFlagWrite, 0xffffff00, 0x10},
		};
		const ExternalFileInfoMapping wrappingRegion{0, 0x100, 0, 0, 0};
		const auto overflowViolation =
			FindExternalMappingViolation(highSection, wrappingRegion);
		CHECK(overflowViolation);
		CHECK(overflowViolation->reason ==
			ExternalMappingViolation::Reason::RegionAddressOverflow);
	}

	void TestMissingBackendPermissionIsAnError()
	{
		const std::array hooks{WupsHookType::InitStorage};
		auto log = std::make_shared<RuntimeLog>();
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("no_backend", "No Backend", hooks), 9, 30,
			std::make_shared<AromaCompatibilityRuntime>(), loader, error);
		CHECK(runtime);
		CHECK(!runtime->OnApplicationStarts(error));
		CHECK(error.find("INIT_STORAGE requires homebrew_wupsbackend permission") !=
			std::string::npos);
		CHECK(runtime->State() == WupsPluginState::Failed);
		CHECK(log->unloads == 1);
	}

	void TestProcessScopeSkipsMapping()
	{
		auto log = std::make_shared<RuntimeLog>();
		log->inScope = false;
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		std::string error;
		auto runtime = WupsPluginRuntime::Create(
			Package("scope", "Scope"), 10, 40, services, loader, error);
		CHECK(runtime);
		CHECK(runtime->OnApplicationStarts(error));
		CHECK(runtime->State() == WupsPluginState::Installed);
		CHECK(log->mappedOwners.empty());
	}

	void TestPluginIsolationAndReloadRollback()
	{
		auto log = std::make_shared<RuntimeLog>();
		auto services = std::make_shared<FakeServices>(log);
		auto loader = std::make_shared<FakeModuleLoader>(log);
		WupsPayloadRuntime manager(services, loader);
		std::string error;
		const auto first = manager.Load(Package("first", "First"), error);
		const auto second = manager.Load(Package("second", "Second"), error);
		CHECK(first && second);
		log->failNextMaps = 1;
		CHECK(!manager.OnApplicationStarts(error));
		CHECK(manager.Find(*first)->State() == WupsPluginState::Failed);
		CHECK(manager.Find(*second)->State() == WupsPluginState::Active);
		manager.UnloadAll();
		CHECK(manager.Size() == 0);

		WupsPayloadRuntime reloadManager(services, loader);
		const auto handle = reloadManager.Load(Package("reload", "Original"), error);
		CHECK(handle);
		CHECK(reloadManager.OnApplicationStarts(error));
		const auto previousGeneration = reloadManager.Find(*handle)->Generation();
		log->failNextMaps = 1;
		CHECK(!reloadManager.Reload(*handle, Package("reload", "Replacement"), error));
		CHECK(error.find("previous generation was restored") != std::string::npos);
		const auto restored = reloadManager.Find(*handle);
		CHECK(restored);
		CHECK(restored->State() == WupsPluginState::Active);
		CHECK(restored->Metadata().name == "Original");
		CHECK(restored->Generation() > previousGeneration);
	}
}

// WupsRuntime.cpp references the production factory even when tests inject a
// loader. Keep the test binary independent of Cemu's guest-memory subsystem.
std::shared_ptr<IWupsModuleLoader> CreateRplWupsModuleLoader()
{
	return {};
}

std::shared_ptr<IWupsRuntimeServices> CreateRplAromaCompatibilityRuntime()
{
	return {};
}

int main()
{
	TestLifecycleOrderAndUnloadBarrier();
	TestLifecycleRollback();
	TestApplicationRestartKeepsOwnerResources();
	TestPatchRestoreFailureBlocksModuleUnload();
	TestReentrantUnloadIsDeferred();
	TestUnloadRevokesAndDrainsOtherThreadCallback();
	TestUnloadDuringMappingDoesNotPublishStaleModule();
	TestPrepareReentrantUnloadAndExceptionBalance();
	TestUnloadFailurePreservesRetryAuthority();
	TestContiguousTlsTemplateMapping();
	TestExternalModulePolicy();
	TestMissingBackendPermissionIsAnError();
	TestProcessScopeSkipsMapping();
	TestPluginIsolationAndReloadRollback();
	std::cout << "WUPS runtime tests passed\n";
	return 0;
}
