#pragma once

#include <chrono>
#include <functional>

namespace WebFrontend::CefNative
{
	using NativeUiTask = std::function<void()>;

	// Initialize and shut down on the thread that owns the native UI. Initialization
	// is idempotent on that thread; calling it from another thread fails.
	bool InitializeNativeUiLoop();
	void ShutdownNativeUiLoop();

	// RunNativeUiLoop blocks until QuitNativeUiLoop is requested. Both quit and post
	// are safe to call from any thread after successful initialization.
	void RunNativeUiLoop();
	void QuitNativeUiLoop();
	bool PostNativeUi(NativeUiTask task);

	// Schedule CEF external-pump work on the native UI thread. Requests are capped
	// to a 30 Hz watchdog cadence and nested calls are reposted instead of re-entered.
	void ScheduleCefMessagePump(std::chrono::milliseconds delay, NativeUiTask callback);

	bool IsNativeUiThread();
} // namespace WebFrontend::CefNative
