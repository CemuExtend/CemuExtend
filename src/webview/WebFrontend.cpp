#include "Common/precompiled.h"

#include "application/ApplicationRuntime.h"
#include "application/ApplicationHost.h"
#include "application/EmulationController.h"
#include "audio/IAudioAPI.h"
#include "frontend/CemuExtendFrontendBridge.h"
#include "frontend/FrontendRuntime.h"
#include "input/InputManager.h"
#include "webview/MainWindowState.h"
#include "webview/NativeWindowHost.h"
#include "webview/RendererHost.h"
#include "webview/RpcDispatcher.h"
#include "webview/WebHostState.h"
#include "webview/WebHostServices.h"
#include "webview/generated/WebAssets.h"
#include "webview/generated/RpcMethods.h"
#include "util/helpers/helpers.h"

#include <array>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <webview/webview.h>

namespace
{
	using WebFrontend::MainWindowState;
	using WebFrontend::RpcDispatcher;
	using WebFrontend::CreateNativeWindowHost;
	using WebFrontend::INativeWindowHost;
	using WebFrontend::MenuCommand;
	using WebFrontend::CreateRendererHost;
	using WebFrontend::IRendererHost;
	using WebFrontend::WebHostState;
	using WebFrontend::WebHostServices;
	class Runtime;
	struct RuntimeCallbackGate
	{
		std::mutex mutex;
		Runtime* target{};
	};

	std::string JsonString(std::string_view value)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string Base64(std::string_view value)
	{
		static constexpr std::string_view alphabet =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string output;
		output.reserve((value.size() + 2) / 3 * 4);
		for (std::size_t offset = 0; offset < value.size(); offset += 3)
		{
			const auto remaining = value.size() - offset;
			const auto first = static_cast<unsigned char>(value[offset]);
			const auto second = remaining > 1 ? static_cast<unsigned char>(value[offset + 1]) : 0;
			const auto third = remaining > 2 ? static_cast<unsigned char>(value[offset + 2]) : 0;
			const std::uint32_t bits = (first << 16) | (second << 8) | third;
			output.push_back(alphabet[(bits >> 18) & 0x3f]);
			output.push_back(alphabet[(bits >> 12) & 0x3f]);
			output.push_back(remaining > 1 ? alphabet[(bits >> 6) & 0x3f] : '=');
			output.push_back(remaining > 2 ? alphabet[bits & 0x3f] : '=');
		}
		return output;
	}

	std::string TitleIdString(std::uint64_t titleId)
	{
		std::array<char, 17> text{};
		std::snprintf(text.data(), text.size(), "%016llx",
			static_cast<unsigned long long>(titleId));
		return text.data();
	}

	std::uint64_t ParseTitleId(const rapidjson::Value& params)
	{
		const auto found = params.FindMember("titleId");
		if (found == params.MemberEnd() || !found->value.IsString() ||
			found->value.GetStringLength() != 16)
			throw std::invalid_argument("titleId must contain exactly 16 hexadecimal digits");
		std::uint64_t value{};
		const std::string_view text(found->value.GetString(), found->value.GetStringLength());
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw std::invalid_argument("titleId must contain exactly 16 hexadecimal digits");
		return value;
	}

	std::uint16_t UsbHidUsage(std::uint32_t key)
	{
		if (key >= 'A' && key <= 'Z') return static_cast<std::uint16_t>(0x04 + key - 'A');
		if (key >= 'a' && key <= 'z') return static_cast<std::uint16_t>(0x04 + key - 'a');
		if (key >= '1' && key <= '9') return static_cast<std::uint16_t>(0x1e + key - '1');
		if (key == '0') return 0x27;
#if BOOST_OS_WINDOWS
		switch (key)
		{
		case VK_RETURN: return 0x28; case VK_ESCAPE: return 0x29; case VK_BACK: return 0x2a;
		case VK_TAB: return 0x2b; case VK_SPACE: return 0x2c; case VK_CAPITAL: return 0x39;
		case VK_F1: return 0x3a; case VK_F2: return 0x3b; case VK_F3: return 0x3c;
		case VK_F4: return 0x3d; case VK_F5: return 0x3e; case VK_F6: return 0x3f;
		case VK_F7: return 0x40; case VK_F8: return 0x41; case VK_F9: return 0x42;
		case VK_F10: return 0x43; case VK_F11: return 0x44; case VK_F12: return 0x45;
		case VK_INSERT: return 0x49; case VK_HOME: return 0x4a; case VK_PRIOR: return 0x4b;
		case VK_DELETE: return 0x4c; case VK_END: return 0x4d; case VK_NEXT: return 0x4e;
		case VK_RIGHT: return 0x4f; case VK_LEFT: return 0x50; case VK_DOWN: return 0x51;
		case VK_UP: return 0x52; case VK_NUMLOCK: return 0x53;
		case VK_LCONTROL: return 0xe0; case VK_LSHIFT: return 0xe1; case VK_LMENU: return 0xe2;
		case VK_LWIN: return 0xe3; case VK_RCONTROL: return 0xe4; case VK_RSHIFT: return 0xe5;
		case VK_RMENU: return 0xe6; case VK_RWIN: return 0xe7;
		default: return 0;
		}
#elif BOOST_OS_MACOS
		switch (key)
		{
		case 0x00: return 0x04; case 0x0b: return 0x05; case 0x08: return 0x06;
		case 0x02: return 0x07; case 0x0e: return 0x08; case 0x03: return 0x09;
		case 0x05: return 0x0a; case 0x04: return 0x0b; case 0x22: return 0x0c;
		case 0x26: return 0x0d; case 0x28: return 0x0e; case 0x25: return 0x0f;
		case 0x2e: return 0x10; case 0x2d: return 0x11; case 0x1f: return 0x12;
		case 0x23: return 0x13; case 0x0c: return 0x14; case 0x0f: return 0x15;
		case 0x01: return 0x16; case 0x11: return 0x17; case 0x20: return 0x18;
		case 0x09: return 0x19; case 0x0d: return 0x1a; case 0x07: return 0x1b;
		case 0x10: return 0x1c; case 0x06: return 0x1d;
		case 0x12: return 0x1e; case 0x13: return 0x1f; case 0x14: return 0x20;
		case 0x15: return 0x21; case 0x17: return 0x22; case 0x16: return 0x23;
		case 0x1a: return 0x24; case 0x1c: return 0x25; case 0x19: return 0x26;
		case 0x1d: return 0x27; case 0x1b: return 0x2d; case 0x18: return 0x2e;
		case 0x21: return 0x2f; case 0x1e: return 0x30; case 0x2a: return 0x31;
		case 0x29: return 0x33; case 0x27: return 0x34; case 0x32: return 0x35;
		case 0x2b: return 0x36; case 0x2f: return 0x37; case 0x2c: return 0x38;
		default: break;
		}
		static constexpr std::array<std::pair<std::uint32_t, std::uint16_t>, 33> usages{{
			{0x24,0x28},{0x35,0x29},{0x33,0x2a},{0x30,0x2b},{0x31,0x2c},
			{0x7a,0x3a},{0x78,0x3b},{0x63,0x3c},{0x76,0x3d},{0x60,0x3e},{0x61,0x3f},
			{0x62,0x40},{0x64,0x41},{0x65,0x42},{0x6d,0x43},{0x67,0x44},{0x6f,0x45},
			{0x72,0x49},{0x73,0x4a},{0x74,0x4b},{0x75,0x4c},{0x77,0x4d},{0x79,0x4e},
			{0x7c,0x4f},{0x7b,0x50},{0x7d,0x51},{0x7e,0x52},
			{0x3b,0xe0},{0x38,0xe1},{0x3a,0xe2},{0x37,0xe3},{0x3e,0xe4},{0x3c,0xe5}
		}};
		for (const auto& [native, usage] : usages) if (native == key) return usage;
		return 0;
#else
		switch (key)
		{
		case 0xff0d: return 0x28; case 0xff1b: return 0x29; case 0xff08: return 0x2a;
		case 0xff09: return 0x2b; case 0x20: return 0x2c; case 0xffe5: return 0x39;
		case 0xffbe: return 0x3a; case 0xffbf: return 0x3b; case 0xffc0: return 0x3c;
		case 0xffc1: return 0x3d; case 0xffc2: return 0x3e; case 0xffc3: return 0x3f;
		case 0xffc4: return 0x40; case 0xffc5: return 0x41; case 0xffc6: return 0x42;
		case 0xffc7: return 0x43; case 0xffc8: return 0x44; case 0xffc9: return 0x45;
		case 0xff63: return 0x49; case 0xff50: return 0x4a; case 0xff55: return 0x4b;
		case 0xffff: return 0x4c; case 0xff57: return 0x4d; case 0xff56: return 0x4e;
		case 0xff53: return 0x4f; case 0xff51: return 0x50; case 0xff54: return 0x51;
		case 0xff52: return 0x52; case 0xffe3: return 0xe0; case 0xffe1: return 0xe1;
		case 0xffe9: return 0xe2; case 0xffeb: return 0xe3; case 0xffe4: return 0xe4;
		case 0xffe2: return 0xe5; case 0xffea: return 0xe6; case 0xffec: return 0xe7;
		default: return 0;
		}
#endif
	}

	std::vector<std::uint32_t> DecodeUtf8(std::string_view text)
	{
		std::vector<std::uint32_t> result;
		for (std::size_t i = 0; i < text.size();)
		{
			const auto first = static_cast<unsigned char>(text[i++]);
			std::uint32_t codepoint{};
			std::size_t continuation{};
			if (first < 0x80) codepoint = first;
			else if ((first & 0xe0) == 0xc0) { codepoint = first & 0x1f; continuation = 1; }
			else if ((first & 0xf0) == 0xe0) { codepoint = first & 0x0f; continuation = 2; }
			else if ((first & 0xf8) == 0xf0) { codepoint = first & 0x07; continuation = 3; }
			else continue;
			if (i + continuation > text.size()) break;
			bool valid = true;
			for (std::size_t offset = 0; offset < continuation; ++offset)
			{
				const auto byte = static_cast<unsigned char>(text[i++]);
				if ((byte & 0xc0) != 0x80) { valid = false; break; }
				codepoint = (codepoint << 6) | (byte & 0x3f);
			}
			if (valid && codepoint <= 0x10ffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff))
				result.push_back(codepoint);
		}
		return result;
	}

	class Runtime final
	{
	public:
		Runtime()
			: m_nativeWindow(CreateNativeWindowHost())
		{
			m_webview = webview_create(
#if defined(NDEBUG)
				0,
#else
				1,
#endif
				m_nativeWindow->GetNativeWindow());
			try
			{
				if (!m_webview)
					throw std::runtime_error("failed to create the native webview window");
				m_webViewWidget = webview_get_native_handle(
					m_webview, WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET);
				if (!m_webViewWidget)
					throw std::runtime_error("failed to acquire the native webview widget");
				m_nativeWindow->AttachWebView(m_webViewWidget);
				m_nativeWindow->SetCloseHandler([this] { (void)RequestShutdown(); });
				m_nativeWindow->SetMenuHandler(
					[this](MenuCommand command) { HandleMenu(command); });
				m_nativeWindow->SetMetricsHandler(
					[this](Host::WindowMetricsSnapshot metrics) { HandleMetrics(metrics); });
				m_nativeWindow->SetInputHandler(
					[this](const WebFrontend::NativeInputEvent& event) { HandleNativeInput(event); });
				m_nativeWindow->SetPadCloseHandler([this] {
					const auto expectedGeneration = m_padGeneration;
					PostToUi([this, expectedGeneration] {
						(void)ClosePadRenderRegion(expectedGeneration);
					});
				});
				m_mainWindowPublication = m_hostState->PublishMainWindow(
					m_nativeWindow->GetMainWindowHandle());
				m_rendererHost = CreateRendererHost(m_hostState, m_hostState, m_hostState);
				m_hostServices = std::make_shared<WebHostServices>(m_hostState, *m_nativeWindow,
					[this](std::function<void()> action) { return PostToUi(std::move(action)); },
					[this] { return RecreateCanvasForHost(); });
				Application::ConnectHost({
					.windowMetrics = m_hostServices,
					.clipboard = m_hostServices,
					.externalLauncher = m_hostServices,
					.inputFocus = m_hostServices,
					.canvas = m_hostServices,
				});
				InputManager::instance().ConfigureHost(*m_hostServices, *m_hostServices,
					*m_hostServices, *m_hostServices);
				IAudioAPI::ConfigureNativeSurfaceProvider(m_hostServices.get());
				m_hostConnected = true;
				m_windowState = std::make_unique<MainWindowState>(reinterpret_cast<std::uintptr_t>(
					m_nativeWindow->GetNativeWindow()));
				m_callbackGate->target = this;
				RegisterRpc();
				m_applicationEvents = m_controller.Events().Subscribe(
					[gate = m_callbackGate](const Application::Event& event) {
						std::scoped_lock lock(gate->mutex);
						if (gate->target)
							gate->target->ForwardEvent(event);
					});
				m_titleEvents = m_controller.SubscribeTitleCatalog(
					[gate = m_callbackGate](const Application::TitleCatalogEvent&) {
						std::scoped_lock lock(gate->mutex);
						if (gate->target)
							gate->target->Emit("titles.changed", "{}");
					});
				if (webview_bind(m_webview, "cemuInvoke", &Runtime::Invoke, this) != WEBVIEW_ERROR_OK)
					throw std::runtime_error("failed to install the native RPC binding");
				m_rpcBound = true;
				webview_set_title(m_webview, "CemuExtend");
				webview_set_size(m_webview, 1100, 720, WEBVIEW_HINT_NONE);
			}
			catch (...)
			{
				Cleanup();
				throw;
			}
		}

		~Runtime() { Cleanup(); }

		void Run()
		{
			if (const char* devUrl = std::getenv("CEMU_WEB_UI_DEV_URL"); devUrl && *devUrl)
			{
				const std::string_view url(devUrl);
				if (!url.starts_with("http://127.0.0.1:") && !url.starts_with("http://localhost:"))
					throw std::runtime_error("CEMU_WEB_UI_DEV_URL must use a loopback HTTP origin");
				webview_navigate(m_webview, devUrl);
			}
			else
			{
				const std::string html(reinterpret_cast<const char*>(WebAssets::html),
					WebAssets::htmlSize);
				webview_set_html(m_webview, html.c_str());
			}
			m_nativeWindow->Show();
			webview_run(m_webview);
		}

	private:
		void Cleanup() noexcept
		{
			if (std::exchange(m_cleanedUp, true))
				return;
			m_stopping.store(true, std::memory_order_release);
			m_eventStopping->store(true, std::memory_order_release);
			{
				std::scoped_lock lock(m_callbackGate->mutex);
				m_callbackGate->target = nullptr;
			}
			m_titleEvents.Reset();
			m_applicationEvents.Reset();
			m_rpc.BeginShutdown();
			constexpr unsigned maximumShutdownAttempts = 500;
			unsigned shutdownAttempt{};
			while (!TryShutdownApplication(false) &&
				++shutdownAttempt < maximumShutdownAttempts)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			if (!m_applicationShutdown)
			{
				cemuLog_log(LogType::Force,
					"Cemu could not safely release emulation resources during final shutdown; terminating without destroying native renderer surfaces");
				std::_Exit(EXIT_FAILURE);
			}
			if (!DestroyMainRenderRegion())
			{
				cemuLog_log(LogType::Force,
					"Cemu could not safely detach native renderer surfaces during final shutdown");
				std::_Exit(EXIT_FAILURE);
			}
			if (m_windowState)
				(void)m_windowState->BeginShutdown();
			if (!m_webview)
				return;
			m_nativeWindow->SetCloseHandler({});
			m_nativeWindow->SetMenuHandler({});
			m_nativeWindow->SetMetricsHandler({});
			m_nativeWindow->SetPadCloseHandler({});
			m_nativeWindow->SetInputHandler({});
			m_nativeWindow->UpdateTextInput({});
			if (m_hostServices)
				m_hostServices->Deactivate();
			if (m_hostConnected)
			{
				InputManager::instance().Shutdown();
				Application::DisconnectHost();
				InputManager::instance().ClearHost();
				IAudioAPI::ConfigureNativeSurfaceProvider(nullptr);
				m_hostConnected = false;
			}
			if (m_webViewWidget)
				m_nativeWindow->PrepareWebViewDestroy(m_webViewWidget);
			if (m_rpcBound)
				webview_unbind(m_webview, "cemuInvoke");
			webview_destroy(m_webview);
			m_webview = nullptr;
			m_webViewWidget = nullptr;
			if (m_mainWindowPublication)
			{
				m_hostState->ClearMainWindow(m_mainWindowPublication);
				m_mainWindowPublication = {};
			}
		}

		bool RequestShutdown()
		{
			if (m_rpc.IsShuttingDown())
				return true;
			if (!TryShutdownApplication())
			{
				Emit("system.diagnostic",
					std::string(R"({"message":)") +
					JsonString("Cemu could not stop the running title; shutdown was cancelled") + "}");
				return false;
			}
			if (!DestroyMainRenderRegion())
				return false;
			m_nativeWindow->ShowLibrary();
			(void)m_windowState->FinishEmulation();
			m_rpc.BeginShutdown();
			(void)m_windowState->BeginShutdown();
			webview_terminate(m_webview);
			return true;
		}

		bool TryShutdownApplication(bool reportFailure = true)
		{
			if (m_applicationShutdown)
				return true;
			const auto result = m_controller.ShutdownApplication();
			if (result.stopped)
			{
				m_applicationShutdown = true;
				return true;
			}
			if (reportFailure)
				cemuLog_log(LogType::Force,
					"Web frontend shutdown could not release emulation resources: {}",
					result.diagnostic);
			return false;
		}

		void StopEmulation()
		{
			const auto result = m_controller.Stop();
			if (!result.stopped)
				return;
			if (!DestroyMainRenderRegion())
				return;
			m_nativeWindow->ShowLibrary();
			(void)m_windowState->FinishEmulation();
		}

		bool DestroyMainRenderRegion()
		{
			if (!ClosePadRenderRegion())
				return false;
			ReleaseNativeInput(true);
			if (m_rendererHost)
				m_rendererHost->PrepareMainDestroy();
			m_nativeWindow->DestroyMainRenderRegion();
			return true;
		}

		bool ClosePadRenderRegion(std::optional<std::uint64_t> expectedGeneration = {})
		{
			if (expectedGeneration && *expectedGeneration != m_padGeneration)
				return true;
			if (!m_nativeWindow->IsPadRenderRegionOpen())
				return true;
			m_nativeWindow->SetPadMetricsEnabled(false);
			auto metrics = m_nativeWindow->GetMetrics();
			metrics.padOpen = false;
			metrics.padWidth = 0;
			metrics.padHeight = 0;
			metrics.physicalPadWidth = 0;
			metrics.physicalPadHeight = 0;
			m_hostState->UpdateMetrics(metrics);
			ReleasePointerSurface(Host::PointerSurface::Pad);
			try
			{
				if (m_rendererHost)
					m_rendererHost->PreparePadDestroy();
			}
			catch (const std::exception& error)
			{
				m_nativeWindow->SetPadMetricsEnabled(true);
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				cemuLog_log(LogType::Force, "Unable to safely close the GamePad surface: {}",
					error.what());
				Emit("system.diagnostic", std::string(R"({"message":)") +
					JsonString(std::string("Unable to safely close GamePad view: ") + error.what()) + "}");
				return false;
			}
			m_nativeWindow->DestroyPadRenderRegion();
			++m_padGeneration;
			return true;
		}

		void TogglePadRenderRegion()
		{
			if (m_controller.State() != Application::EmulationState::Running)
			{
				Emit("system.diagnostic", R"({"message":"GamePad view is only available while a title is running"})");
				return;
			}
			if (m_nativeWindow->IsPadRenderRegionOpen())
			{
				(void)ClosePadRenderRegion();
				return;
			}
			try
			{
				++m_padGeneration;
				auto& region = m_nativeWindow->CreatePadRenderRegion();
				m_rendererHost->InitializePad(region);
				m_nativeWindow->SetPadMetricsEnabled(true);
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				region.SetVisible(true);
				region.RequestFocus();
			}
			catch (const std::exception& error)
			{
				(void)ClosePadRenderRegion();
				Emit("system.diagnostic", std::string(R"({"message":)") +
					JsonString(std::string("Unable to open GamePad view: ") + error.what()) + "}");
			}
		}

		void HandleMetrics(Host::WindowMetricsSnapshot metrics)
		{
			const auto previous = m_hostState->GetWindowMetrics();
			m_hostState->UpdateMetrics(metrics);
			if (previous.appActive != metrics.appActive)
			{
				m_controller.PointerFocusChanged(metrics.appActive);
				if (!metrics.appActive && m_hostServices)
				{
					ReleaseNativeInput(false);
				}
			}
		}

		Frontend::CemuExtendFrontendBridge& PointerBridge(Host::PointerSurface surface)
		{
			return m_pointerBridges[surface == Host::PointerSurface::Main ? 0 : 1];
		}

		std::uint32_t PointerButtonMask(std::uint32_t nativeButton) const
		{
			using Button = Frontend::CemuExtendMouseButton;
			switch (nativeButton)
			{
			case 1: return static_cast<std::uint32_t>(Button::Left);
			case 3: return static_cast<std::uint32_t>(Button::Right);
			case 2: return static_cast<std::uint32_t>(Button::Middle);
			case 8: return static_cast<std::uint32_t>(Button::X1);
			case 9: return static_cast<std::uint32_t>(Button::X2);
			default: return 0;
			}
		}

		Frontend::CemuExtendPointerDecision RefreshPointerPolicy(Host::PointerSurface surface)
		{
			auto& bridge = PointerBridge(surface);
			const auto policy = m_controller.GetPointerPolicy();
			const auto metrics = m_hostState->GetWindowMetrics();
			const bool hasCanvas = surface == Host::PointerSurface::Main
				? m_windowState && m_windowState->Snapshot().mode == WebFrontend::MainWindowContentMode::Playing
				: m_nativeWindow->IsPadRenderRegionOpen();
			const auto decision = bridge.ApplyPointerPolicy(policy.mode, policy.cursor,
				policy.flags, metrics.appActive, hasCanvas);
			m_nativeWindow->ApplyPointerPresentation({
				.surface = surface,
				.ownsPointer = decision.ownsPointer,
				.showCursor = decision.showCursor,
				.confine = decision.confine,
				.enteringCapture = decision.enteringCapture,
				.leavingPolicy = decision.leavingPolicy,
				.requestRawMouse = decision.requestRawMouse,
				.rawMouseEnabled = bridge.RawMouseRequested(),
				.cursor = decision.cursor,
			});
			return decision;
		}

		void SubmitPointer(const WebFrontend::NativeInputEvent& event,
			std::int32_t deltaX, std::int32_t deltaY, std::int32_t wheelX,
			std::int32_t wheelY, std::uint32_t changedButtons, bool raw)
		{
			auto& state = m_pointerStates[event.surface == Host::PointerSurface::Main ? 0 : 1];
			const auto metrics = m_hostState->GetWindowMetrics();
			m_controller.SubmitMouse({
				.surface = event.surface == Host::PointerSurface::Main
					? Application::PointerSurface::Tv : Application::PointerSurface::Drc,
				.x = state.x, .y = state.y, .deltaX = deltaX, .deltaY = deltaY,
				.wheelX = wheelX, .wheelY = wheelY,
				.buttons = PointerBridge(event.surface).MouseButtons(),
				.changedButtons = changedButtons,
				.contentWidth = state.width, .contentHeight = state.height,
				.insideContent = state.inside, .focused = metrics.appActive,
				.flags = raw
					? static_cast<std::uint8_t>(Frontend::CemuExtendMouseEventFlag::RawRelative)
					: static_cast<std::uint8_t>(0),
			});
		}

		void ReleasePointerSurface(Host::PointerSurface surface)
		{
			const auto index = surface == Host::PointerSurface::Main ? 0U : 1U;
			auto& bridge = m_pointerBridges[index];
			const auto released = bridge.UpdateButtons(
				Frontend::CemuExtendMouseTransition::Aggregate, 0xffffffffU, 0);
			if (released.changed)
			{
				WebFrontend::NativeInputEvent event{.kind = WebFrontend::NativeInputKind::PointerButton,
					.surface = surface};
				SubmitPointer(event, 0, 0, 0, 0, released.changed, false);
			}
			(void)bridge.ApplyPointerPolicy(0, 0, 0, false, false);
			bridge.ResetPointerPosition();
			m_pointerStates[index] = {};
			m_nativeWindow->ApplyPointerPresentation({.surface = surface,
				.showCursor = true, .leavingPolicy = true});
		}

		void ReleaseNativeInput(bool resetTextInput)
		{
			if (m_hostServices) m_hostServices->ReleaseKeys();
			m_controller.KeyboardFocusLost();
			ReleasePointerSurface(Host::PointerSurface::Main);
			ReleasePointerSurface(Host::PointerSurface::Pad);
			if (resetTextInput)
			{
				m_textInputSequence = 0;
				m_nativeWindow->UpdateTextInput({});
			}
		}

		void HandleNativeInput(const WebFrontend::NativeInputEvent& event)
		{
			if (m_stopping.load(std::memory_order_acquire) || !m_hostServices)
				return;
			auto& state = m_pointerStates[event.surface == Host::PointerSurface::Main ? 0 : 1];
			auto& bridge = PointerBridge(event.surface);
			switch (event.kind)
			{
			case WebFrontend::NativeInputKind::PointerMove:
			{
				(void)RefreshPointerPolicy(event.surface);
				state = {event.x, event.y, event.contentWidth, event.contentHeight, event.insideContent};
				m_hostServices->UpdateMousePosition(event.surface, {event.x, event.y});
				const auto motion = bridge.UpdatePosition({event.x, event.y},
					{event.contentWidth / 2, event.contentHeight / 2}, bridge.RawMouseRequested());
				SubmitPointer(event, motion.delta.x, motion.delta.y, 0, 0, 0, motion.rawRelative);
				break;
			}
			case WebFrontend::NativeInputKind::PointerButton:
			{
				(void)RefreshPointerPolicy(event.surface);
				state = {event.x, event.y, event.contentWidth, event.contentHeight, event.insideContent};
				const auto mask = PointerButtonMask(event.button);
				const auto update = bridge.UpdateButtons(event.pressed
					? Frontend::CemuExtendMouseTransition::Down
					: Frontend::CemuExtendMouseTransition::Up, mask);
				if (event.button == 1 || event.button == 3)
					m_hostServices->UpdateMouseButton(event.surface,
						event.button == 1 ? Host::PointerButton::Left : Host::PointerButton::Right,
						event.pressed, {event.x, event.y});
				SubmitPointer(event, 0, 0, 0, 0, update.changed, false);
				break;
			}
			case WebFrontend::NativeInputKind::PointerWheel:
			{
				const auto wheelX = bridge.NormalizeWheel(event.wheelX, 120, true);
				const auto wheelY = bridge.NormalizeWheel(event.wheelY, 120, false);
				m_hostServices->UpdateMouseWheel(static_cast<float>(event.wheelY) / 120.0f,
					wheelY);
				SubmitPointer(event, 0, 0, wheelX, wheelY, 0, false);
				break;
			}
			case WebFrontend::NativeInputKind::RawMouse:
				(void)RefreshPointerPolicy(event.surface);
				if (!bridge.RawMouseRequested())
					break;
				bridge.MarkRawMouseSeen();
				bridge.RecordRawPosition({state.x, state.y});
				SubmitPointer(event, event.deltaX, event.deltaY, 0, 0, 0, true);
				break;
			case WebFrontend::NativeInputKind::Touch:
				m_hostServices->UpdateTouch(event.surface, {event.x, event.y}, event.pressed);
				break;
			case WebFrontend::NativeInputKind::Key:
			{
				m_hostServices->UpdateKey(event.key, event.pressed);
				const auto usage = event.usage ? event.usage : UsbHidUsage(event.key);
				if (usage)
					m_controller.SubmitKeyboard(usage, event.pressed, event.modifiers);
				break;
			}
			case WebFrontend::NativeInputKind::Character:
				if (!m_textInputSequence)
					for (const auto codepoint : DecodeUtf8(event.text))
						if (codepoint >= 0x20 && codepoint != 0x7f)
							m_controller.SubmitText(codepoint, event.repeat);
				break;
			case WebFrontend::NativeInputKind::FocusLost:
				m_controller.PointerFocusChanged(false);
				ReleaseNativeInput(false);
				break;
			case WebFrontend::NativeInputKind::DeviceChanged:
				m_hostServices->NotifyDeviceChanged();
				break;
			case WebFrontend::NativeInputKind::TextComposition:
				if (event.textSequence == m_textInputSequence && m_textInputSequence != 0)
					m_controller.SubmitTextComposition(event.text, event.preedit,
						event.textCursor, event.selectionLength);
				break;
			}
		}

		void RefreshTextInput()
		{
			const auto state = m_controller.GetTextInputState();
			m_textInputSequence = state.active ? state.sequence : 0;
			m_nativeWindow->UpdateTextInput({
				.active = state.active, .sequence = state.sequence,
				.initialText = state.initialText, .maximumLength = state.maximumLength,
				.caretX = state.caretX, .caretY = state.caretY,
				.lineHeight = state.lineHeight,
			});
			if (state.active)
				m_controller.KeyboardFocusLost();
		}

		bool RecreateCanvasForHost()
		{
			if (!m_windowState || m_windowState->Snapshot().mode !=
				WebFrontend::MainWindowContentMode::Playing)
				return false;
			if (!ClosePadRenderRegion())
				return false;
			ReleaseNativeInput(true);
			try
			{
				m_rendererHost->PrepareMainDestroy();
				m_nativeWindow->DestroyMainRenderRegion();
				auto& region = m_nativeWindow->CreateMainRenderRegion();
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				m_rendererHost->InitializeMain(region);
				m_nativeWindow->ShowRenderRegion();
				RefreshTextInput();
				return true;
			}
			catch (const std::exception& error)
			{
				cemuLog_log(LogType::Force, "Native canvas recreation failed: {}", error.what());
				try { m_rendererHost->AbandonMainInitialization(); }
				catch (...) {}
				m_nativeWindow->DestroyMainRenderRegion();
				m_nativeWindow->ShowLibrary();
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				return false;
			}
		}

		void HandleMenu(MenuCommand command)
		{
			switch (command)
			{
			case MenuCommand::EndEmulation: StopEmulation(); break;
			case MenuCommand::Exit: (void)RequestShutdown(); break;
			case MenuCommand::ToggleFullscreen:
				m_fullscreen = !m_fullscreen;
				m_nativeWindow->SetFullscreen(m_fullscreen);
				break;
			case MenuCommand::Load: Emit("menu.command", R"({"command":"load"})"); break;
			case MenuCommand::TogglePadView: TogglePadRenderRegion(); break;
			case MenuCommand::GeneralSettings: Emit("menu.command", R"({"command":"generalSettings"})"); break;
			case MenuCommand::InputSettings: Emit("menu.command", R"({"command":"inputSettings"})"); break;
			case MenuCommand::GraphicPacks: Emit("menu.command", R"({"command":"graphicPacks"})"); break;
			case MenuCommand::TitleManager: Emit("menu.command", R"({"command":"titleManager"})"); break;
			case MenuCommand::Logging: Emit("menu.command", R"({"command":"logging"})"); break;
			case MenuCommand::About: Emit("menu.command", R"({"command":"about"})"); break;
			}
		}

		struct PendingEvent
		{
			std::shared_ptr<std::atomic_bool> stopping;
			std::function<void()> beforeDispatch;
			std::string script;
		};

		static void DispatchEvent(webview_t webview, void* argument)
		{
			std::unique_ptr<PendingEvent> pending(static_cast<PendingEvent*>(argument));
			if (pending->stopping->load(std::memory_order_acquire))
				return;
			if (pending->beforeDispatch)
				pending->beforeDispatch();
			if (!pending->script.empty())
				webview_eval(webview, pending->script.c_str());
		}

		bool PostToUi(std::function<void()> action)
		{
			if (m_stopping.load(std::memory_order_acquire))
				return false;
			auto pending = std::make_unique<PendingEvent>();
			pending->stopping = m_eventStopping;
			pending->beforeDispatch = std::move(action);
			if (webview_dispatch(m_webview, &Runtime::DispatchEvent, pending.get()) == WEBVIEW_ERROR_OK)
			{
				pending.release();
				return true;
			}
			return false;
		}

		void Emit(std::string_view type, std::string_view payloadJson,
			std::function<void()> beforeDispatch = {})
		{
			std::scoped_lock eventLock(m_eventMutex);
			if (m_stopping.load(std::memory_order_acquire))
				return;
			const auto sequence = ++m_eventSequence;
			const auto event = std::string(R"({"type":)") + JsonString(type) +
				R"(,"sequence":)" + std::to_string(sequence) + R"(,"payload":)" +
				std::string(payloadJson) + "}";
			auto pending = std::make_unique<PendingEvent>();
			pending->stopping = m_eventStopping;
			pending->beforeDispatch = std::move(beforeDispatch);
			pending->script = "window.__cemuDispatchEvent?.(JSON.parse(new TextDecoder().decode(Uint8Array.from(atob('" +
				Base64(event) + "'),c=>c.charCodeAt(0)))));";
			if (webview_dispatch(m_webview, &Runtime::DispatchEvent, pending.get()) == WEBVIEW_ERROR_OK)
				pending.release();
		}

		void ForwardEvent(const Application::Event& event)
		{
			switch (event.type)
			{
			case Application::EventType::LoadingStarted: Emit("emulation.loading", "{}"); break;
			case Application::EventType::GameLoaded: Emit("emulation.loaded", "{}"); break;
			case Application::EventType::GameExited:
			{
				const auto expectedGeneration = m_windowState->Snapshot().generation;
				Emit("emulation.exited", "{}", [this, expectedGeneration] {
					const auto state = m_windowState->Snapshot();
					if (state.mode != WebFrontend::MainWindowContentMode::Playing ||
						state.generation != expectedGeneration)
						return;
					if (!DestroyMainRenderRegion())
						return;
					m_nativeWindow->ShowLibrary();
					(void)m_windowState->FinishEmulation();
				});
				break;
			}
			case Application::EventType::PpcProcessExited:
				Emit("emulation.processExited", std::string(R"({"status":)") +
					std::to_string(event.processStatus) + "}");
				break;
			case Application::EventType::PerformanceUpdated:
				Emit("emulation.performance", std::string(R"({"fps":)") +
					std::to_string(event.framesPerSecond) + "}");
				break;
			case Application::EventType::Diagnostic:
				Emit("system.diagnostic", std::string(R"({"message":)") +
					JsonString(event.diagnostic) + "}");
				break;
			case Application::EventType::GameListRefreshRequested: Emit("titles.changed", "{}"); break;
			case Application::EventType::TextInputWakeRequested:
				Emit("input.textWakeRequested", "{}", [this] { RefreshTextInput(); });
				break;
			}
		}

		static void Invoke(const char* sequence, const char* arguments, void* context)
		{
			auto& self = *static_cast<Runtime*>(context);
			rapidjson::Document array;
			array.Parse(arguments);
			std::string response;
			if (!array.IsArray() || array.Size() != 1 || !array[0].IsString())
				response = R"({"id":"","ok":false,"error":{"code":"invalid_binding_call","message":"cemuInvoke expects one JSON string"}})";
			else
				response = self.m_rpc.Dispatch(
					std::string_view(array[0].GetString(), array[0].GetStringLength()));
			const auto encoded = JsonString(response);
			webview_return(self.m_webview, sequence, 0, encoded.c_str());
		}

		void RegisterRpc()
		{
			m_rpc.Register("system.bootstrap", [this](const rapidjson::Value&) {
				return std::string(R"({"windowRole":"main-library","appVersion":"2.0","platform":")") +
#if BOOST_OS_WINDOWS
					"windows"
#elif BOOST_OS_MACOS
					"macos"
#else
					"linux"
#endif
					+ R"(","theme":"system","shuttingDown":)" +
					(m_rpc.IsShuttingDown() ? "true}" : "false}");
			});
			m_rpc.Register("system.quit", [this](const rapidjson::Value&) {
				if (!RequestShutdown())
					throw std::runtime_error("the running title could not be stopped; shutdown was cancelled");
				return "{}";
			});
			m_rpc.Register("window.close", [this](const rapidjson::Value&) {
				if (!RequestShutdown())
					throw std::runtime_error("the running title could not be stopped; window close was cancelled");
				return "{}";
			});
			m_rpc.Register("window.getModel", [this](const rapidjson::Value& params) {
				const auto role = params.FindMember("role");
				if (role == params.MemberEnd() || !role->value.IsString())
					throw std::invalid_argument("role is required");
				const auto state = m_windowState->Snapshot();
				return std::string(R"({"role":)") +
					JsonString({role->value.GetString(), role->value.GetStringLength()}) +
					R"(,"emulationState":)" + std::to_string(static_cast<unsigned>(m_controller.State())) +
					R"(,"mainWindowGeneration":)" + std::to_string(state.generation) + "}";
			});
			m_rpc.Register("titles.list", [this](const rapidjson::Value&) {
				rapidjson::Document document(rapidjson::kArrayType);
				auto& allocator = document.GetAllocator();
				for (const auto& game : m_controller.ListGames())
				{
					rapidjson::Value item(rapidjson::kObjectType);
					const auto titleId = TitleIdString(game.titleId);
					item.AddMember("titleId", rapidjson::Value(titleId.c_str(), allocator), allocator);
					item.AddMember("name", rapidjson::Value(game.name.c_str(), allocator), allocator);
					const auto path = _pathToUtf8(game.basePath);
					item.AddMember("path", rapidjson::Value(path.c_str(), allocator), allocator);
					item.AddMember("region", rapidjson::Value(game.regionName.c_str(), allocator), allocator);
					item.AddMember("version", game.version, allocator);
					item.AddMember("playTimeMinutes", game.playStats.minutesPlayed, allocator);
					if (game.playStats.available)
					{
						std::array<char, 11> date{};
						std::snprintf(date.data(), date.size(), "%04u-%02u-%02u",
							game.playStats.lastPlayedYear, game.playStats.lastPlayedMonth,
							game.playStats.lastPlayedDay);
						item.AddMember("lastPlayed", rapidjson::Value(date.data(), allocator), allocator);
					}
					else
						item.AddMember("lastPlayed", rapidjson::Value(rapidjson::kNullType), allocator);
					document.PushBack(item, allocator);
				}
				rapidjson::StringBuffer buffer;
				rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
				document.Accept(writer);
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("titles.refresh", [this](const rapidjson::Value&) {
				m_controller.RefreshTitles();
				return "{}";
			});
			m_rpc.Register("titles.launch", [this](const rapidjson::Value& params) {
				const auto game = m_controller.GetGame(ParseTitleId(params));
				if (!game)
					throw std::invalid_argument("titleId is not present in the game library");
				return Launch(game->basePath);
			});
			m_rpc.Register("emulation.stop", [this](const rapidjson::Value&) {
				const auto result = m_controller.Stop();
				if (!result.stopped)
					throw std::runtime_error(result.diagnostic);
				if (!DestroyMainRenderRegion())
					throw std::runtime_error("native renderer surfaces could not be detached safely");
				m_nativeWindow->ShowLibrary();
				(void)m_windowState->FinishEmulation();
				return "{}";
			});
			m_rpc.VerifyMethods(WebFrontend::Generated::RpcMethods);
		}

		std::string Launch(const fs::path& path)
		{
			if (!m_windowState->BeginLaunch())
				throw std::runtime_error("main window is not ready to launch a title");
			const auto result = m_controller.Launch({path},
				[this](const Application::LaunchResult&) {
					auto& region = m_nativeWindow->CreateMainRenderRegion();
					m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
					m_rendererHost->InitializeMain(region);
					if (!m_windowState->CommitLaunch())
						throw std::runtime_error("main window content transition failed");
					m_nativeWindow->ShowRenderRegion();
				},
				[this] {
					m_rendererHost->AbandonMainInitialization();
					DestroyMainRenderRegion();
					m_nativeWindow->ShowLibrary();
					(void)m_windowState->RollbackLaunch();
				});
			if (!result)
			{
				if (m_controller.State() == Application::EmulationState::Running)
				{
					(void)m_nativeWindow->CreateMainRenderRegion();
					(void)m_windowState->CommitLaunch();
					m_nativeWindow->ShowRenderRegion();
				}
				else
				{
					DestroyMainRenderRegion();
					m_nativeWindow->ShowLibrary();
					(void)m_windowState->RollbackLaunch();
				}
				throw std::runtime_error(result.diagnostic.empty() ? "title launch failed" : result.diagnostic);
			}
			return std::string(R"({"titleId":")") + TitleIdString(result.titleId) + "\"}";
		}

		std::unique_ptr<INativeWindowHost> m_nativeWindow;
		webview_t m_webview{};
		void* m_webViewWidget{};
		RpcDispatcher m_rpc;
		std::shared_ptr<WebHostState> m_hostState{std::make_shared<WebHostState>()};
		std::shared_ptr<WebHostServices> m_hostServices;
		std::unique_ptr<IRendererHost> m_rendererHost;
		Application::EmulationController m_controller;
		std::unique_ptr<MainWindowState> m_windowState;
		std::shared_ptr<RuntimeCallbackGate> m_callbackGate{std::make_shared<RuntimeCallbackGate>()};
		Application::EventSubscription m_applicationEvents;
		Application::TitleCatalogSubscription m_titleEvents;
		std::atomic_bool m_stopping{};
		std::shared_ptr<std::atomic_bool> m_eventStopping{std::make_shared<std::atomic_bool>()};
		std::mutex m_eventMutex;
		std::uint64_t m_eventSequence{};
		std::uint64_t m_padGeneration{};
		struct PointerState
		{
			std::int32_t x{};
			std::int32_t y{};
			std::int32_t width{};
			std::int32_t height{};
			bool inside{};
		};
		std::array<PointerState, 2> m_pointerStates;
		std::array<Frontend::CemuExtendFrontendBridge, 2> m_pointerBridges;
		std::uint64_t m_textInputSequence{};
		bool m_fullscreen{};
		bool m_rpcBound{};
		bool m_cleanedUp{};
		bool m_applicationShutdown{};
		bool m_hostConnected{};
		Host::NativeSurfacePublication m_mainWindowPublication{};
	};
}

void Frontend::Run()
{
	CemuCommonInit();
#if BOOST_OS_WINDOWS
	std::exception_ptr uiFailure;
	std::thread uiThread([&uiFailure] {
		SetThreadName("cemu-web-ui");
		const auto initialized = CoInitializeEx(nullptr,
			COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		if (FAILED(initialized))
		{
			uiFailure = std::make_exception_ptr(
				std::runtime_error("failed to initialize the webview STA UI thread"));
			return;
		}
		try
		{
			Runtime runtime;
			runtime.Run();
		}
		catch (...)
		{
			uiFailure = std::current_exception();
		}
		CoUninitialize();
	});
	uiThread.join();
	if (uiFailure)
		std::rethrow_exception(uiFailure);
#else
	SetThreadName("cemu-web-ui");
	Runtime runtime;
	runtime.Run();
#endif
}
