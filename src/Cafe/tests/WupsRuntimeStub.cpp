#include "Cafe/HW/Espresso/WupsRuntime.h"

struct WupsPayloadRuntime::Impl {};

WupsPayloadRuntime::WupsPayloadRuntime(
	std::shared_ptr<IWupsRuntimeServices>,
	std::shared_ptr<IWupsModuleLoader>,
	std::shared_ptr<WupsBackendManagementRuntime>) :
	m_impl(std::make_unique<Impl>())
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

bool WupsPayloadRuntime::Unload(std::uint64_t) { return false; }

bool WupsPayloadRuntime::Unload(std::uint64_t, std::string& error)
{
	error = "WUPS runtime is not linked into isolated runtime unit tests";
	return false;
}

void WupsPayloadRuntime::UnloadAll() {}
bool WupsPayloadRuntime::UnloadAll(std::string&) { return true; }
void WupsPayloadRuntime::AbandonAllForTitleShutdown() {}
bool WupsPayloadRuntime::OnApplicationStarts(std::string&) { return true; }
void WupsPayloadRuntime::OnReleaseForeground() {}
void WupsPayloadRuntime::OnAcquiredForeground() {}
void WupsPayloadRuntime::OnApplicationRequestsExit() {}
void WupsPayloadRuntime::OnApplicationEnds() {}
void WupsPayloadRuntime::SetProcessKey(WupsProcessKind, std::uint64_t) {}

std::shared_ptr<WupsPluginRuntime> WupsPayloadRuntime::Find(std::uint64_t) const
{
	return {};
}

std::size_t WupsPayloadRuntime::Size() const { return 0; }

std::shared_ptr<IWupsModuleLoader> CreateRplWupsModuleLoader() { return {}; }

std::shared_ptr<IWupsRuntimeServices> CreateRplAromaCompatibilityRuntime()
{
	return {};
}

std::shared_ptr<IWupsRuntimeServices> CreateRplAromaCompatibilityRuntime(
	std::shared_ptr<WupsBackendManagementRuntime>)
{
	return {};
}
