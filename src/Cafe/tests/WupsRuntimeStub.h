#pragma once

#include <cstddef>
#include <functional>

// Test seam for the WupsPayloadRuntime stub in WupsRuntimeStub.cpp. Unit tests
// that link the stub instead of the real WUPS runtime use these to describe
// what the runtime would do for a title, and to observe when CemodRuntime calls
// into it.
namespace wups_runtime_stub
{
	// Controls WupsPayloadRuntime::Size()/WillStartPlugins(): the number of plugins
	// the stub pretends are loaded for the current title.
	extern std::size_t g_pluginCount;
	// Invoked at the top of WupsPayloadRuntime::OnApplicationStarts(), before any
	// plugin would run its init hooks.
	extern std::function<void()> g_onApplicationStarts;
	// Number of completed WupsPayloadRuntime::OnApplicationStarts() calls.
	extern std::size_t g_applicationStartCount;

	void Reset();
} // namespace wups_runtime_stub
