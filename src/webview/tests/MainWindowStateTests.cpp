#include "webview/MainWindowState.h"

#include <cassert>

int main()
{
	using namespace WebFrontend;
	MainWindowState state(0x1234);
	auto initial = state.Snapshot();
	assert(initial.mode == MainWindowContentMode::Library);
	assert(initial.webViewVisible && !initial.renderSurfaceVisible);
	assert(state.BeginLaunch());
	assert(state.Snapshot().webViewVisible);
	assert(state.CommitLaunch());
	auto playing = state.Snapshot();
	assert(playing.mode == MainWindowContentMode::Playing);
	assert(!playing.webViewVisible && playing.renderSurfaceVisible);
	assert(playing.mainWindowIdentity == initial.mainWindowIdentity);
	assert(state.FinishEmulation());
	auto restored = state.Snapshot();
	assert(restored.mode == MainWindowContentMode::Library);
	assert(restored.webViewVisible && !restored.renderSurfaceVisible);
	assert(restored.mainWindowIdentity == initial.mainWindowIdentity);
	assert(state.BeginLaunch());
	assert(state.RollbackLaunch());
	assert(state.Snapshot().mode == MainWindowContentMode::Library);
	assert(state.BeginShutdown());
	assert(!state.Snapshot().webViewVisible && !state.Snapshot().renderSurfaceVisible);
}
