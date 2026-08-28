#include "webview/WebHostState.h"

#include <cassert>

int main()
{
	auto state = std::make_shared<WebFrontend::WebHostState>();
	Host::WindowMetricsSnapshot metrics;
	metrics.appActive = true;
	metrics.width = 1280;
	metrics.height = 720;
	metrics.physicalWidth = 2560;
	metrics.physicalHeight = 1440;
	metrics.dpiScale = 2.0;
	state->UpdateMetrics(metrics);
	const auto currentMetrics = state->GetWindowMetrics();
	assert(currentMetrics.appActive);
	assert(currentMetrics.width == 1280 && currentMetrics.height == 720);
	assert(currentMetrics.physicalWidth == 2560 && currentMetrics.physicalHeight == 1440);
	assert(currentMetrics.dpiScale == 2.0);

	const Host::NativeWindowHandle firstWindow{
		Host::NativeWindowBackend::X11, reinterpret_cast<void*>(1), reinterpret_cast<void*>(2)};
	const Host::NativeWindowHandle secondWindow{
		Host::NativeWindowBackend::X11, reinterpret_cast<void*>(3), reinterpret_cast<void*>(4)};
	const auto firstWindowPublication = state->PublishMainWindow(firstWindow);
	const auto secondWindowPublication = state->PublishMainWindow(secondWindow);
	state->ClearMainWindow(firstWindowPublication);
	assert(state->GetNativeSurfaces().mainWindow == secondWindow);

	const Host::NativeWindowHandle surface{
		Host::NativeWindowBackend::X11, reinterpret_cast<void*>(5), reinterpret_cast<void*>(6)};
	const auto surfacePublication = state->PublishCanvas(true, surface);
	{
		const auto snapshot = state->GetNativeSurfaces();
		assert(snapshot.mainWindow == secondWindow);
		assert(snapshot.mainSurface == surface);
		assert(snapshot.lifetime);
	}
	state->ClearCanvas(true, surfacePublication);
	assert(state->GetNativeSurfaces().mainSurface.backend == Host::NativeWindowBackend::Unknown);
	state->ClearMainWindow(secondWindowPublication);
	assert(state->GetNativeSurfaces().mainWindow.backend == Host::NativeWindowBackend::Unknown);
}
