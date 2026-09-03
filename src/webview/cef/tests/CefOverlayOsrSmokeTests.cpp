#include "webview/cef/CefOverlayRuntime.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace
{
	std::string Response(std::uint64_t windowId, std::string_view request)
	{
		const auto idKey = request.find(R"("id":")");
		if (idKey == std::string_view::npos)
			return R"({"id":"","ok":false,"error":{"code":"malformed","message":"missing id"}})";
		const auto idStart = idKey + 6;
		const auto idEnd = request.find('"', idStart);
		if (idEnd == std::string_view::npos)
			return R"({"id":"","ok":false,"error":{"code":"malformed","message":"bad id"}})";
		const std::string id(request.substr(idStart, idEnd - idStart));
		constexpr std::string_view snapshot = R"({
			"sequence":"1",
			"overlayStyle":{"position":"topLeft","color":4294967295,"scale":100},
			"notificationStyle":{"position":"topRight","color":4294967295,"scale":100},
			"visibility":{"fps":true,"drawCalls":false,"cpuUsage":false,"cpuPerCore":false,"ramUsage":false,"vramUsage":false,"debug":false},
			"stats":{"fps":60.0,"drawCalls":0,"fastDrawCalls":0,"cpuUsage":0.0,"cpuPerCore":[],"ramUsageMb":0,"vramUsageMb":-1,"vramTotalMb":-1,"debugLines":[]},
			"notices":[{"id":"1","kind":"message","text":"CEF OSR smoke","remainingMs":0}],
			"shaderProgress":{"generation":"0","visible":false,"pipelines":false,"current":0,"total":0,"vertexShaders":0,"pixelShaders":0,"geometryShaders":0,"backgroundImageAvailable":false},
			"keyboard":{"generation":"0","active":false,"keyboardOnly":false,"shifted":false,"maximumLength":0,"text":""},
			"errorDialog":{"generation":"0","active":false,"title":"","message":"","leftButton":"","rightButton":"","opacity":1.0},
			"interaction":"passive"
		})";
		std::string result;
		if (request.find(R"("method":"system.bootstrap")") != std::string_view::npos)
		{
			result = "{\"windowId\":\"" + std::to_string(windowId) +
					 "\",\"windowRole\":\"runtime-overlay\",\"appVersion\":\"smoke\","
					 "\"platform\":\"linux\",\"activeAccountName\":\"\",\"theme\":\"dark\","
					 "\"themeRevision\":\"0\",\"language\":\"en\",\"languageRevision\":\"0\","
					 "\"shuttingDown\":false}";
		}
		else if (request.find(R"("method":"theme.get")") != std::string_view::npos)
			result = R"({"theme":"dark","revision":"0"})";
		else if (request.find(R"("method":"overlay.getSnapshot")") != std::string_view::npos)
			result = std::string(snapshot);
		else
			return "{\"id\":\"" + id +
				   "\",\"ok\":false,\"error\":{\"code\":\"unknown_method\",\"message\":\"unsupported smoke RPC\"}}";
		return "{\"id\":\"" + id + "\",\"ok\":true,\"result\":" + result + "}";
	}

	struct PixelState
	{
		bool frameSeen{};
		bool transparent{};
		bool painted{};

		bool Complete() const
		{
			return frameSeen && transparent && painted;
		}
	};

	void AccumulatePixels(const Host::OverlayFrameSnapshot& frame, PixelState& state)
	{
		if (!frame.bgra || frame.bgra->size() < 4)
			return;
		state.frameSeen = true;
		for (std::size_t offset = 0; offset + 3 < frame.bgra->size(); offset += 4)
		{
			const auto alpha = (*frame.bgra)[offset + 3];
			state.transparent |= alpha == 0;
			state.painted |= alpha != 0 && ((*frame.bgra)[offset] != 0 ||
											(*frame.bgra)[offset + 1] != 0 || (*frame.bgra)[offset + 2] != 0);
		}
	}

	std::optional<std::array<std::uint8_t, 4>> CenterPixel(
		const Host::OverlayFrameSnapshot& frame)
	{
		if (!frame.bgra || frame.width <= 0 || frame.height <= 0 || frame.stride < frame.width * 4)
			return std::nullopt;
		const auto offset = static_cast<std::size_t>(frame.height / 2) * frame.stride +
							static_cast<std::size_t>(frame.width / 2) * 4U;
		if (offset + 4U > frame.bgra->size())
			return std::nullopt;
		return std::array{frame.bgra->at(offset), frame.bgra->at(offset + 1U),
						  frame.bgra->at(offset + 2U), frame.bgra->at(offset + 3U)};
	}

} // namespace

int main(int argc, char* argv[])
{
	using namespace WebFrontend::CefOverlay;
	const int subprocess = ExecuteSubprocess(argc, argv);
	if (subprocess >= 0)
		return subprocess;
	if (!InitializeProcessRuntime())
	{
		std::cerr << "CefInitialize failed\n";
		return 1;
	}
	auto runtime = CreateBrowserRuntime(
		[](std::uint64_t windowId, std::string_view request) { return Response(windowId, request); },
		[](Host::PointerSurface) {});
	if (!runtime || !runtime->Create(Host::PointerSurface::Main, 1, 640, 360, 1.0) ||
		!runtime->Create(Host::PointerSurface::Pad, 2, 480, 270, 1.0))
	{
		std::cerr << "CEF browser creation failed\n";
		if (runtime)
			runtime->CloseAll();
		runtime.reset();
		ShutdownProcessRuntime();
		return 1;
	}

	PixelState mainState;
	PixelState padState;
	bool inputSuspensionFailed{};
	runtime->SetInteractive(Host::PointerSurface::Main, true);
	InputIntent dockIntent;
	dockIntent.generation = 1;
	dockIntent.revision = 1;
	dockIntent.visible = true;
	dockIntent.keyboardFocus = KeyboardFocus::Navigation;
	dockIntent.interactiveRects.push_back({0, 0, 640, 360});
	WebFrontend::NativeInputEvent keyEvent{.kind = WebFrontend::NativeInputKind::Key,
										   .surface = Host::PointerSurface::Main};
	if (!runtime->UpdateInputIntent(1, dockIntent) ||
		(runtime->ResolveInput(keyEvent).ownership.keyboard != InputOwner::WebUi ||
		 runtime->ResolveInput(keyEvent).ownership.pointer != InputOwner::WebUi))
	{
		std::cerr << "CEF OSR input intent did not acquire keyboard ownership\n";
		inputSuspensionFailed = true;
	}
	runtime->SetInputSuspended(Host::PointerSurface::Main, true);
	if (runtime->ResolveInput(keyEvent).ownership.keyboard != InputOwner::Title ||
		runtime->ResolveInput(keyEvent).ownership.pointer != InputOwner::Title)
	{
		std::cerr << "CEF OSR input suspension did not release keyboard ownership\n";
		inputSuspensionFailed = true;
	}
	runtime->SetInputSuspended(Host::PointerSurface::Main, false);
	if (runtime->ResolveInput(keyEvent).ownership.keyboard != InputOwner::WebUi ||
		runtime->ResolveInput(keyEvent).ownership.pointer != InputOwner::WebUi)
	{
		std::cerr << "CEF OSR input intent was not restored after suspension\n";
		inputSuspensionFailed = true;
	}
	runtime->ResetInputIntent(1, 2);
	runtime->SetInteractive(Host::PointerSurface::Main, false);
	std::optional<Host::OverlayFrameSnapshot> mainBeforeResize;
	std::optional<Host::OverlayFrameSnapshot> padBeforeResize;
	const auto initialPaintDeadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while ((!mainState.Complete() || !padState.Complete()) &&
		   std::chrono::steady_clock::now() < initialPaintDeadline)
	{
		DoProcessMessageLoopWork();
		if (auto frame = runtime->AcquireLatestOverlayFrame(Host::PointerSurface::Main, 0))
		{
			mainBeforeResize = std::move(frame);
			AccumulatePixels(*mainBeforeResize, mainState);
		}
		if (auto frame = runtime->AcquireLatestOverlayFrame(Host::PointerSurface::Pad, 0))
		{
			padBeforeResize = std::move(frame);
			AccumulatePixels(*padBeforeResize, padState);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (!mainState.Complete() || !padState.Complete())
	{
		std::cerr << "CEF OSR initial paint timed out (main=" << mainState.Complete()
				  << ", pad=" << padState.Complete() << ")\n";
		runtime->CloseAll();
		runtime.reset();
		ShutdownProcessRuntime();
		return 1;
	}

	// Focus and activation events refresh window metrics without changing the
	// render size. A redundant Resize must retain the composed frame instead of
	// synchronously publishing a transparent one while CEF schedules a repaint.
	if (auto latest = runtime->AcquireLatestOverlayFrame(Host::PointerSurface::Main, 0))
		mainBeforeResize = std::move(latest);
	if (auto latest = runtime->AcquireLatestOverlayFrame(Host::PointerSurface::Pad, 0))
		padBeforeResize = std::move(latest);
	bool redundantResizeFailed{};
	if (!mainBeforeResize || !padBeforeResize)
	{
		std::cerr << "CEF OSR frame disappeared before redundant resize check\n";
		redundantResizeFailed = true;
	}
	else
	{
		runtime->Resize(Host::PointerSurface::Main, 640, 360, 1.0);
		runtime->Resize(Host::PointerSurface::Pad, 480, 270, 1.0);
		if (runtime->AcquireLatestOverlayFrame(Host::PointerSurface::Main,
											   mainBeforeResize->sequence) ||
			runtime->AcquireLatestOverlayFrame(Host::PointerSurface::Pad,
											   padBeforeResize->sequence))
		{
			std::cerr << "Redundant CEF OSR resize published a replacement frame\n";
			redundantResizeFailed = true;
		}
	}

	// OSR animations must be paced by Blink's monotonic clock rather than guest
	// presents. Simulate a paused guest by pumping CEF without acquiring frames:
	// the transition must still reach its final state in the latest-only mailbox.
	bool animationCadenceFailed{};
	std::uint64_t mainSequence = mainBeforeResize ? mainBeforeResize->sequence : 0;
	runtime->ExecuteScript(Host::PointerSurface::Main, R"JS(
		(() => {
			const old = document.getElementById('__cemu_osr_cadence_test');
			old?.remove();
			const cover = document.createElement('div');
			cover.id = '__cemu_osr_cadence_test';
			Object.assign(cover.style, {
				position: 'fixed', inset: '0', zIndex: '2147483647',
				backgroundColor: 'rgb(255, 0, 0)', pointerEvents: 'none'
			});
			document.documentElement.appendChild(cover);
			cover.animate(
				[{backgroundColor: 'rgb(255, 0, 0)'}, {backgroundColor: 'rgb(0, 255, 0)'}],
				{duration: 240, easing: 'linear', fill: 'forwards'});
		})();
	)JS");
	const auto pausedUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
	while (std::chrono::steady_clock::now() < pausedUntil)
	{
		DoProcessMessageLoopWork();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	auto afterPausedPresent = runtime->AcquireLatestOverlayFrame(
		Host::PointerSurface::Main, mainSequence);
	if (!afterPausedPresent)
	{
		std::cerr << "CEF OSR animation did not repaint while guest presents were paused\n";
		animationCadenceFailed = true;
	}
	else
	{
		mainSequence = afterPausedPresent->sequence;
		const auto pixel = CenterPixel(*afterPausedPresent);
		if (!pixel || (*pixel)[1] < 220 || (*pixel)[2] > 35 || (*pixel)[3] < 250)
		{
			std::cerr << "CEF OSR animation did not advance to its final state while presents were paused\n";
			animationCadenceFailed = true;
		}
	}

	// Sample a second linear transition at a 30 Hz guest-present cadence. It is
	// expected to skip intermediate mailbox sequence numbers, but the sampled
	// pixels must retain intermediate states and finish on wall-clock time.
	runtime->ExecuteScript(Host::PointerSurface::Main, R"JS(
		(() => {
			const cover = document.getElementById('__cemu_osr_cadence_test');
			cover.getAnimations().forEach(animation => animation.cancel());
			cover.animate(
				[{backgroundColor: 'rgb(0, 255, 0)'}, {backgroundColor: 'rgb(255, 0, 0)'}],
				{duration: 300, easing: 'linear', fill: 'forwards'});
		})();
	)JS");
	bool sawIntermediate{};
	bool sawFinalRed{};
	const auto cadenceStarted = std::chrono::steady_clock::now();
	auto nextGuestPresent = cadenceStarted;
	while (std::chrono::steady_clock::now() - cadenceStarted < std::chrono::milliseconds(750))
	{
		DoProcessMessageLoopWork();
		const auto now = std::chrono::steady_clock::now();
		if (now >= nextGuestPresent)
		{
			nextGuestPresent += std::chrono::milliseconds(33);
			if (auto frame = runtime->AcquireLatestOverlayFrame(
					Host::PointerSurface::Main, mainSequence))
			{
				mainSequence = frame->sequence;
				if (const auto pixel = CenterPixel(*frame))
				{
					const auto green = (*pixel)[1];
					const auto red = (*pixel)[2];
					sawIntermediate |= green > 25 && green < 230 && red > 25 && red < 230;
					sawFinalRed |= red > 220 && green < 35 && (*pixel)[3] >= 250;
				}
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	if (!sawIntermediate || !sawFinalRed)
	{
		std::cerr << "CEF OSR animation lost its linear cadence at a 30 Hz guest sample rate\n";
		animationCadenceFailed = true;
	}
	runtime->CloseAll();
	runtime.reset();
	ShutdownProcessRuntime();
	return redundantResizeFailed || animationCadenceFailed || inputSuspensionFailed ? 1 : 0;
}
