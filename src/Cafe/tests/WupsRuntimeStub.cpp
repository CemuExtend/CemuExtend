#include "Cafe/HW/Espresso/WupsRuntime.h"

#include "WupsRuntimeStub.h"

namespace wups_runtime_stub
{
	std::size_t g_pluginCount = 0;
	std::function<void()> g_onApplicationStarts;
	std::size_t g_applicationStartCount = 0;

	void Reset()
	{
		g_pluginCount = 0;
		g_onApplicationStarts = {};
		g_applicationStartCount = 0;
	}
} // namespace wups_runtime_stub

struct WupsPayloadRuntime::Impl
{
};

WupsPayloadRuntime::WupsPayloadRuntime(
	std::shared_ptr<IWupsRuntimeServices>,
	std::shared_ptr<IWupsModuleLoader>,
	std::shared_ptr<WupsBackendManagementRuntime>) : m_impl(std::make_unique<Impl>())
{
}

WupsPayloadRuntime::~WupsPayloadRuntime() = default;

std::optional<std::uint64_t> WupsPayloadRuntime::Load(
	CemodPackage, std::string& error)
{
	error = "WUPS runtime is not linked into isolated runtime unit tests";
	return std::nullopt;
}

bool WupsPayloadRuntime::Reload(
	std::uint64_t, CemodPackage, std::string& error)
{
	error = "WUPS runtime is not linked into isolated runtime unit tests";
	return false;
}

bool WupsPayloadRuntime::Unload(std::uint64_t)
{
	return false;
}

bool WupsPayloadRuntime::Unload(std::uint64_t, std::string& error)
{
	error = "WUPS runtime is not linked into isolated runtime unit tests";
	return false;
}

void WupsPayloadRuntime::UnloadAll()
{
	wups_runtime_stub::g_pluginCount = 0;
}
bool WupsPayloadRuntime::UnloadAll(std::string&)
{
	wups_runtime_stub::g_pluginCount = 0;
	return true;
}
bool WupsPayloadRuntime::PrepareTitleShutdown(std::string&)
{
	return true;
}
bool WupsPayloadRuntime::TitleShutdownPrepared() const
{
	return true;
}
bool WupsPayloadRuntime::ReleaseAfterTitleThreadsStopped(std::string&)
{
	return true;
}
void WupsPayloadRuntime::AbandonAllForTitleShutdown() {}
bool WupsPayloadRuntime::OnApplicationStarts(std::string&)
{
	if (wups_runtime_stub::g_onApplicationStarts)
		wups_runtime_stub::g_onApplicationStarts();
	++wups_runtime_stub::g_applicationStartCount;
	return true;
}

bool WupsPayloadRuntime::WillStartPlugins() const
{
	return wups_runtime_stub::g_pluginCount != 0;
}
void WupsPayloadRuntime::OnReleaseForeground() {}
void WupsPayloadRuntime::OnAcquiredForeground() {}
void WupsPayloadRuntime::OnApplicationRequestsExit() {}
void WupsPayloadRuntime::OnApplicationEnds() {}
void WupsPayloadRuntime::SetProcessKey(WupsProcessKind, std::uint64_t) {}

std::shared_ptr<WupsPluginRuntime> WupsPayloadRuntime::Find(std::uint64_t) const
{
	return {};
}

std::size_t WupsPayloadRuntime::Size() const
{
	return wups_runtime_stub::g_pluginCount;
}

std::shared_ptr<IWupsModuleLoader> CreateRplWupsModuleLoader()
{
	return {};
}

std::shared_ptr<IWupsRuntimeServices> CreateRplAromaCompatibilityRuntime()
{
	return {};
}

std::shared_ptr<IWupsRuntimeServices> CreateRplAromaCompatibilityRuntime(
	std::shared_ptr<WupsBackendManagementRuntime>)
{
	return {};
}
