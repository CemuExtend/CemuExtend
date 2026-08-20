#include "input/HotkeySettings.h"
#include "input/InputManager.h"
#include "audio/IAudioAPI.h"
#include "application/ApplicationHost.h"
#include "frontend/FrontendRuntime.h"
#include "interface/WindowSystem.h"

#include "helpers/wxHelpers.h"

#if BOOST_OS_LINUX || BOOST_OS_BSD
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkwindow.h>
#include <gdk/gdkx.h>
#ifdef HAS_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#endif

#if BOOST_OS_MACOS
#include <Carbon/Carbon.h>
#endif

#include "wxgui/wxgui.h"
#include "wxgui/CemuApp.h"
#include "wxgui/MainWindow.h"
#include "wxgui/WxWindowState.h"
#include "wxgui/WxFrontendRuntime.h"
#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"
#include "config/CemuConfig.h"
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/utils.h>

#include <chrono>
#include <future>
#include <unordered_map>

namespace
{
	std::atomic_bool s_wxFrontendStopping{false};
	std::mutex s_wxDispatchMutex;
	std::mutex s_runtimeObjectsMutex;
	std::mutex s_pendingUiMutex;
	std::uint64_t s_nextPendingUiId{1};
	std::unordered_map<std::uint64_t, std::function<void()>> s_pendingUiCancellation;
	std::shared_ptr<class WxHostServices> s_wxHostServices;
	std::shared_ptr<WxWindowState> s_wxWindowState;
	std::shared_ptr<WxMainWindowRegistry> s_mainWindowRegistry;

	std::optional<std::uint64_t> RegisterPendingUi(std::function<void()> cancel)
	{
		std::scoped_lock lock(s_pendingUiMutex);
		if (s_wxFrontendStopping.load(std::memory_order_acquire))
			return std::nullopt;
		const auto id = s_nextPendingUiId++;
		s_pendingUiCancellation.emplace(id, std::move(cancel));
		return id;
	}

	bool CompletePendingUi(std::uint64_t id)
	{
		std::scoped_lock lock(s_pendingUiMutex);
		return s_pendingUiCancellation.erase(id) != 0;
	}

	bool IsPendingUi(std::uint64_t id)
	{
		std::scoped_lock lock(s_pendingUiMutex);
		return s_pendingUiCancellation.contains(id);
	}

	void CancelPendingUi(std::optional<std::uint64_t> only = std::nullopt)
	{
		std::vector<std::function<void()>> callbacks;
		{
			std::scoped_lock lock(s_pendingUiMutex);
			if (only)
			{
				const auto found = s_pendingUiCancellation.find(*only);
				if (found != s_pendingUiCancellation.end())
				{
					callbacks.push_back(std::move(found->second));
					s_pendingUiCancellation.erase(found);
				}
			}
			else
			{
				for (auto& [_, callback] : s_pendingUiCancellation)
					callbacks.push_back(std::move(callback));
				s_pendingUiCancellation.clear();
			}
		}
		for (auto& callback : callbacks)
			callback();
	}

	std::optional<uint32> ResolvePlatformKeyCode(WindowSystem::PlatformKeyCodes key)
	{
		switch (key)
		{
#if BOOST_OS_WINDOWS
		case WindowSystem::PlatformKeyCodes::LCONTROL: return VK_LCONTROL;
		case WindowSystem::PlatformKeyCodes::RCONTROL: return VK_RCONTROL;
		case WindowSystem::PlatformKeyCodes::TAB: return VK_TAB;
		case WindowSystem::PlatformKeyCodes::ESCAPE: return VK_ESCAPE;
#elif BOOST_OS_LINUX || BOOST_OS_BSD
		case WindowSystem::PlatformKeyCodes::LCONTROL: return GDK_KEY_Control_L;
		case WindowSystem::PlatformKeyCodes::RCONTROL: return GDK_KEY_Control_R;
		case WindowSystem::PlatformKeyCodes::TAB: return GDK_KEY_Tab;
		case WindowSystem::PlatformKeyCodes::ESCAPE: return GDK_KEY_Escape;
#elif BOOST_OS_MACOS
		case WindowSystem::PlatformKeyCodes::LCONTROL: return kVK_Control;
		case WindowSystem::PlatformKeyCodes::RCONTROL: return kVK_RightControl;
		case WindowSystem::PlatformKeyCodes::TAB: return kVK_Tab;
		case WindowSystem::PlatformKeyCodes::ESCAPE: return kVK_Escape;
#endif
		}
		return std::nullopt;
	}

	template<typename Callback>
	bool QueueFrameCallback(Callback&& callback)
	{
		auto registry = WxFrontendRuntime::GetMainWindowRegistry();
		if (!registry)
			return false;
		auto sharedCallback = std::make_shared<std::decay_t<Callback>>(
			std::forward<Callback>(callback));
		auto invoke = [registry = std::move(registry),
			sharedCallback = std::move(sharedCallback)] {
			return registry->InvokeForUi(
				[&](MainWindow& frame) { (*sharedCallback)(frame); });
		};
		if (wxIsMainThread())
			return !s_wxFrontendStopping.load(std::memory_order_acquire) && invoke();
		return WindowSystem::QueueUi(
			[invoke = std::move(invoke)]() mutable { (void)invoke(); });
	}

	class WxHostServices final : public Host::IWindowMetrics,
		public Host::INativeSurfaceProvider,
		public Host::INativeSurfacePublisher,
		public Host::IKeyboardState,
		public Host::IClipboard,
		public Host::IExternalLauncher,
		public Host::IInputFocus,
		public Host::ICanvasHost,
		public Input::IControllerStateObserver
	{
	public:
		explicit WxHostServices(std::shared_ptr<WxWindowState> state,
			std::shared_ptr<WxMainWindowRegistry> mainWindowRegistry)
			: m_state(std::move(state)),
			  m_mainWindowRegistry(std::move(mainWindowRegistry)) {}

		Host::WindowMetricsSnapshot GetWindowMetrics() const override
		{
			return m_state->Metrics();
		}

		Host::NativeSurfaceSnapshot GetNativeSurfaces() const override
		{
			return m_state->NativeSurfaces();
		}

		Host::NativeSurfacePublication PublishMainWindow(
			Host::NativeWindowHandle handle) override
		{
			return m_state->PublishMainWindow(handle);
		}

		void ClearMainWindow(Host::NativeSurfacePublication publication) override
		{
			m_state->ClearMainWindow(publication);
		}

		Host::NativeSurfacePublication PublishPadWindow(
			Host::NativeWindowHandle handle) override
		{
			return m_state->PublishPadWindow(handle);
		}

		void ClearPadWindow(Host::NativeSurfacePublication publication) override
		{
			m_state->ClearPadWindow(publication);
		}

		Host::NativeSurfacePublication PublishCanvas(bool mainWindow,
			Host::NativeWindowHandle handle) override
		{
			return m_state->PublishCanvas(mainWindow, handle);
		}

		void ClearCanvas(bool mainWindow,
			Host::NativeSurfacePublication publication) override
		{
			m_state->ClearCanvas(mainWindow, publication);
		}

		bool IsKeyDown(Host::Key key) const override
		{
			using Host::Key;
			WindowSystem::PlatformKeyCodes platformKey;
			switch (key)
			{
			case Key::LeftControl:
				platformKey = WindowSystem::PlatformKeyCodes::LCONTROL;
				break;
			case Key::RightControl:
				platformKey = WindowSystem::PlatformKeyCodes::RCONTROL;
				break;
			case Key::Tab:
				platformKey = WindowSystem::PlatformKeyCodes::TAB;
				break;
			case Key::Escape:
				platformKey = WindowSystem::PlatformKeyCodes::ESCAPE;
				break;
			default:
				return false;
			}
			const auto nativeKey = ResolvePlatformKeyCode(platformKey);
			return nativeKey && m_state->IsKeyDown(*nativeKey);
		}

		std::string GetKeyName(std::uint32_t key) const override
		{
			return WindowSystem::GetKeyCodeName(key);
		}

		std::vector<Host::KeyState> GetKeyStates() const override
		{
			return m_state->KeyStates();
		}

		void OnControllerState(const ControllerState& current,
			const ControllerState& previous) override
		{
			HotkeySettings::CaptureInput(current, previous);
		}

		void GetTextAsync(std::function<void(bool, std::string)> callback) override
		{
			WindowSystem::GetClipboardTextAsync(std::move(callback));
		}

		void SetTextAsync(std::string text, std::function<void(bool)> callback) override
		{
			WindowSystem::SetClipboardTextAsync(std::move(text), std::move(callback));
		}

		bool OpenUrl(std::string url) override
		{
			return WindowSystem::QueueUi([url = std::move(url)] {
				if (!wxLaunchDefaultBrowser(wxString::FromUTF8(url)))
					cemuLog_log(LogType::Force, "Failed to open host browser URL: {}", url);
			});
		}

		bool InputConfigurationHasFocus() const override
		{
			return WindowSystem::InputConfigWindowHasFocus();
		}

		bool RecreateCanvas() override
		{
			if (wxIsMainThread())
			{
				if (s_wxFrontendStopping.load(std::memory_order_acquire))
					return false;
				return m_mainWindowRegistry->InvokeForUi(
					[](MainWindow& frame) { frame.RecreateCanvasForHost(); });
			}
			auto result = std::make_shared<std::promise<bool>>();
			auto future = result->get_future();
			const auto pending = RegisterPendingUi([result] { result->set_value(false); });
			if (!pending)
				return false;
			auto mainWindowRegistry = m_mainWindowRegistry;
			if (!WindowSystem::QueueUi([id = *pending, result,
				mainWindowRegistry = std::move(mainWindowRegistry)] {
				if (!IsPendingUi(id))
					return;
				const bool available =
					!s_wxFrontendStopping.load(std::memory_order_acquire) &&
					mainWindowRegistry->InvokeForUi(
						[](MainWindow& frame) { frame.RecreateCanvasForHost(); });
				if (CompletePendingUi(id))
					result->set_value(available);
			}))
				CancelPendingUi(*pending);
			// Keep the request registered until recreation finishes. BeginShutdown()
			// cancels queued work and resolves this future; timing out after the UI
			// has started recreating would race title rollback against canvas creation.
			return future.get();
		}

	private:
		std::shared_ptr<WxWindowState> m_state;
		std::shared_ptr<WxMainWindowRegistry> m_mainWindowRegistry;
	};

	void InstallWxHostServices()
	{
		s_wxFrontendStopping.store(false, std::memory_order_release);
		auto windowState = std::make_shared<WxWindowState>();
		auto mainWindowRegistry = std::make_shared<WxMainWindowRegistry>();
		auto hostServices = std::make_shared<WxHostServices>(
			windowState, mainWindowRegistry);
		{
			std::scoped_lock lock(s_runtimeObjectsMutex);
			s_wxWindowState = windowState;
			s_mainWindowRegistry = mainWindowRegistry;
			s_wxHostServices = hostServices;
		}
		Application::ConnectHost({
			.windowMetrics = std::static_pointer_cast<Host::IWindowMetrics>(hostServices),
			.clipboard = std::static_pointer_cast<Host::IClipboard>(hostServices),
			.externalLauncher = std::static_pointer_cast<Host::IExternalLauncher>(hostServices),
			.inputFocus = std::static_pointer_cast<Host::IInputFocus>(hostServices),
			.canvas = std::static_pointer_cast<Host::ICanvasHost>(hostServices),
		});
		InputManager::instance().ConfigureHost(*hostServices, *hostServices,
			*hostServices, *hostServices);
		InputManager::instance().Start();
		IAudioAPI::ConfigureNativeSurfaceProvider(hostServices.get());
	}
}

#if BOOST_OS_WINDOWS
void _wxLaunch()
{
	SetThreadName("MainThread_UI");
	wxEntry();
}
#endif

void Frontend::Run()
{
	SetThreadName("cemu");
	InstallWxHostServices();
#if BOOST_OS_WINDOWS
	// on Windows wxWidgets there is a bug where wxDirDialog->ShowModal will deadlock in Windows internals somehow
	// moving the UI thread off the main thread fixes this
	std::thread t = std::thread(_wxLaunch);
	t.join();
#else
	int argc = 0;
	char* argv[1]{};
	wxEntry(argc, argv);
#endif
	WindowSystem::ReleaseHostServices();
}

std::shared_ptr<Host::IWindowMetrics> WindowSystem::GetWindowMetricsHost()
{
	std::scoped_lock lock(s_runtimeObjectsMutex);
	return s_wxHostServices;
}

std::shared_ptr<Host::INativeSurfaceProvider> WindowSystem::GetNativeSurfaceHost()
{
	std::scoped_lock lock(s_runtimeObjectsMutex);
	return s_wxHostServices;
}

std::shared_ptr<Host::INativeSurfacePublisher> WindowSystem::GetNativeSurfacePublisher()
{
	std::scoped_lock lock(s_runtimeObjectsMutex);
	return s_wxHostServices;
}

std::shared_ptr<WxWindowState> WxFrontendRuntime::GetWindowState()
{
	std::scoped_lock lock(s_runtimeObjectsMutex);
	return s_wxWindowState;
}

std::shared_ptr<WxMainWindowRegistry> WxFrontendRuntime::GetMainWindowRegistry()
{
	std::scoped_lock lock(s_runtimeObjectsMutex);
	return s_mainWindowRegistry;
}

bool WindowSystem::IsShuttingDown()
{
	return s_wxFrontendStopping.load(std::memory_order_acquire);
}

void WindowSystem::BeginShutdown()
{
	{
		std::scoped_lock lock(s_wxDispatchMutex);
		s_wxFrontendStopping.store(true, std::memory_order_release);
	}
	CancelPendingUi();
}

void WindowSystem::ResumeAfterFailedShutdown()
{
	std::scoped_lock lock(s_wxDispatchMutex);
	s_wxFrontendStopping.store(false, std::memory_order_release);
}

bool WindowSystem::QueueUi(std::function<void()> callback)
{
	std::scoped_lock lock(s_wxDispatchMutex);
	if (s_wxFrontendStopping.load(std::memory_order_acquire) || !wxTheApp)
		return false;
	wxTheApp->CallAfter([callback = std::move(callback)]() mutable {
		if (!s_wxFrontendStopping.load(std::memory_order_acquire))
			callback();
	});
	return true;
}

bool WindowSystem::QueueUi(std::function<void()> callback,
	std::function<void()> cancelled)
{
	struct Completion
	{
		std::atomic_bool completed{};
		std::function<void()> callback;
		std::function<void()> cancelled;
	};
	auto completion = std::make_shared<Completion>();
	completion->callback = std::move(callback);
	completion->cancelled = std::move(cancelled);
	auto finish = [completion](bool runCallback) {
		if (completion->completed.exchange(true, std::memory_order_acq_rel))
			return;
		(runCallback ? completion->callback : completion->cancelled)();
	};
	const auto pending = RegisterPendingUi([finish] { finish(false); });
	if (!pending)
	{
		finish(false);
		return false;
	}
	bool rejected{};
	{
		std::scoped_lock lock(s_wxDispatchMutex);
		if (s_wxFrontendStopping.load(std::memory_order_acquire) || !wxTheApp)
			rejected = true;
		else
			wxTheApp->CallAfter([id = *pending, finish] {
				if (CompletePendingUi(id))
					finish(true);
			});
	}
	if (rejected)
	{
		CancelPendingUi(*pending);
		return false;
	}
	return true;
}

void WindowSystem::ReleaseHostServices()
{
	BeginShutdown();
	std::shared_ptr<WxHostServices> hostServices;
	{
		std::scoped_lock lock(s_runtimeObjectsMutex);
		if (!s_wxHostServices)
			return;
		hostServices = std::move(s_wxHostServices);
		s_mainWindowRegistry.reset();
		s_wxWindowState.reset();
	}
	InputManager::instance().Shutdown();
	Application::DisconnectHost();
	InputManager::instance().ClearHost();
	IAudioAPI::ConfigureNativeSurfaceProvider(nullptr);
}

void WindowSystem::ShowErrorDialog(std::string_view message, std::string_view title, std::optional<WindowSystem::ErrorCategory> /*errorId*/)
{
	wxString caption;
	if (title.empty())
		caption = wxASCII_STR(wxMessageBoxCaptionStr);
	else
		caption = wxString::FromUTF8(title);
	wxMessageBox(wxString::FromUTF8(message), caption, wxOK | wxCENTRE | wxICON_ERROR);
}

void WindowSystem::GetClipboardTextAsync(std::function<void(bool, std::string)> callback)
{
	auto sharedCallback = std::make_shared<decltype(callback)>(std::move(callback));
	const auto pending = RegisterPendingUi([sharedCallback] { (*sharedCallback)(false, {}); });
	if (!pending)
	{
		(*sharedCallback)(false, {});
		return;
	}
	if (!QueueUi([id = *pending, sharedCallback]() mutable {
		if (!CompletePendingUi(id))
			return;
		if (!wxTheClipboard->Open())
		{
			(*sharedCallback)(false, {});
			return;
		}
		wxTextDataObject data;
		const bool success = wxTheClipboard->GetData(data);
		wxTheClipboard->Close();
		(*sharedCallback)(success, success ? data.GetText().utf8_string() : std::string{});
	}))
		CancelPendingUi(*pending);
}

void WindowSystem::SetClipboardTextAsync(std::string text, std::function<void(bool)> callback)
{
	auto sharedCallback = std::make_shared<decltype(callback)>(std::move(callback));
	const auto pending = RegisterPendingUi([sharedCallback] { (*sharedCallback)(false); });
	if (!pending)
	{
		(*sharedCallback)(false);
		return;
	}
	if (!QueueUi([id = *pending, text = std::move(text), sharedCallback]() mutable {
		if (!CompletePendingUi(id))
			return;
		if (!wxTheClipboard->Open())
		{
			(*sharedCallback)(false);
			return;
		}
		const bool success = wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text)));
		if (success)
			wxTheClipboard->Flush();
		wxTheClipboard->Close();
		(*sharedCallback)(success);
	}))
		CancelPendingUi(*pending);
}

void WindowSystem::UpdateWindowTitles(bool isIdle, bool isLoading, double fps,
	std::optional<Application::WindowTitlePresentation> presentation)
{
	std::string windowText;
	windowText = BUILD_VERSION_WITH_NAME_STRING;

	if (isIdle)
	{
		(void)QueueFrameCallback([windowText = std::move(windowText)](MainWindow& frame) {
			frame.AsyncSetTitle(windowText);
		});
		return;
	}
	if (isLoading)
	{
		windowText.append(" - Loading...");
		(void)QueueFrameCallback([windowText = std::move(windowText)](MainWindow& frame) {
			frame.AsyncSetTitle(windowText);
		});
		return;
	}

	const char* renderer = "";
	const char* graphicMode = "[Generic]";
	if (presentation)
	{
		switch (presentation->renderer)
		{
		case Application::PresentationRenderer::OpenGL:
			renderer = "[OpenGL]";
			break;
		case Application::PresentationRenderer::Vulkan:
			renderer = "[Vulkan]";
			break;
		case Application::PresentationRenderer::Metal:
			renderer = "[Metal]";
			break;
		default: break;
		}
		switch (presentation->gpuVendor)
		{
		case Application::PresentationGpuVendor::Amd: graphicMode = "[AMD GPU]"; break;
		case Application::PresentationGpuVendor::Intel: graphicMode = "[Intel GPU]"; break;
		case Application::PresentationGpuVendor::Nvidia:
			graphicMode = "[NVIDIA GPU]";
			break;
		case Application::PresentationGpuVendor::Apple:
			graphicMode = "[Apple GPU]";
			break;
		default: break;
		}

		windowText.append(fmt::format(
			" - FPS: {:.2f} {} {} [TitleId: {:08x}-{:08x}]", fps, renderer,
			graphicMode, static_cast<std::uint32_t>(presentation->titleId >> 32),
			static_cast<std::uint32_t>(presentation->titleId & 0xFFFFFFFF)));
		if (ActiveSettings::IsOnlineEnabled())
		{
			if (ActiveSettings::GetNetworkService() == NetworkService::Nintendo)
				windowText.append(" [Online]");
			else if (ActiveSettings::GetNetworkService() == NetworkService::Pretendo)
				windowText.append(" [Online-Pretendo]");
			else if (ActiveSettings::GetNetworkService() == NetworkService::Plasma)
				windowText.append(" [Online-Plasma]");
			else if (ActiveSettings::GetNetworkService() == NetworkService::Custom)
				windowText.append(" [Online-" + GetNetworkConfig().networkname.GetValue() + "]");
		}
		windowText.append(" ");
		windowText.append(presentation->titleName);
		switch (presentation->region)
		{
		case Application::TitleRegion::Japan:
			windowText.append(fmt::format(" [JP v{}]", presentation->version));
			break;
		case Application::TitleRegion::UnitedStates:
			windowText.append(fmt::format(" [US v{}]", presentation->version));
			break;
		case Application::TitleRegion::Europe:
			windowText.append(fmt::format(" [EU v{}]", presentation->version));
			break;
		default:
			windowText.append(fmt::format(" [v{}]", presentation->version));
			break;
		}
	}
	else
		windowText.append(fmt::format(" - FPS: {:.2f}", fps));

	(void)QueueFrameCallback([windowText = std::move(windowText), fps](MainWindow& frame) {
		frame.AsyncSetTitle(windowText);
		auto* pad = frame.GetPadView();
		if (pad)
			pad->AsyncSetTitle(fmt::format("{} - FPS: {:.02f}", _("GamePad View").utf8_string(), fps));
	});
}

void WindowSystem::GetWindowSize(int& w, int& h)
{
	auto state = WxFrontendRuntime::GetWindowState();
	w = state ? state->width.load() : 0;
	h = state ? state->height.load() : 0;
}

void WindowSystem::GetPadWindowSize(int& w, int& h)
{
	auto state = WxFrontendRuntime::GetWindowState();
	if (state && state->pad_open.load())
	{
		w = state->pad_width.load();
		h = state->pad_height.load();
	}
	else
	{
		w = 0;
		h = 0;
	}
}

void WindowSystem::GetWindowPhysSize(int& w, int& h)
{
	auto state = WxFrontendRuntime::GetWindowState();
	w = state ? state->phys_width.load() : 0;
	h = state ? state->phys_height.load() : 0;
}

void WindowSystem::GetPadWindowPhysSize(int& w, int& h)
{
	auto state = WxFrontendRuntime::GetWindowState();
	if (state && state->pad_open.load())
	{
		w = state->phys_pad_width.load();
		h = state->phys_pad_height.load();
	}
	else
	{
		w = 0;
		h = 0;
	}
}

double WindowSystem::GetWindowDPIScale()
{
	auto state = WxFrontendRuntime::GetWindowState();
	return state ? state->dpi_scale.load() : 1.0;
}

double WindowSystem::GetPadDPIScale()
{
	auto state = WxFrontendRuntime::GetWindowState();
	return state && state->pad_open.load() ? state->pad_dpi_scale.load() : 1.0;
}

bool WindowSystem::IsPadWindowOpen()
{
	auto state = WxFrontendRuntime::GetWindowState();
	return state && state->pad_open.load();
}

bool WindowSystem::IsKeyDown(uint32 key)
{
	auto state = WxFrontendRuntime::GetWindowState();
	return state && state->IsKeyDown(key);
}

bool WindowSystem::IsKeyDown(PlatformKeyCodes platformKey)
{
	const auto key = ResolvePlatformKeyCode(platformKey);
	return key && WindowSystem::IsKeyDown(*key);
}

std::string WindowSystem::GetKeyCodeName(uint32 button)
{
#if BOOST_OS_WINDOWS
	LONG scan_code = MapVirtualKeyA((UINT)button, MAPVK_VK_TO_VSC_EX);
	if (HIBYTE(scan_code))
		scan_code |= 0x100;

	// because MapVirtualKey strips the extended bit for some keys
	switch (button)
	{
	case VK_LEFT:
	case VK_UP:
	case VK_RIGHT:
	case VK_DOWN: // arrow keys
	case VK_PRIOR:
	case VK_NEXT: // page up and page down
	case VK_END:
	case VK_HOME:
	case VK_INSERT:
	case VK_DELETE:
	case VK_DIVIDE: // numpad slash
	case VK_NUMLOCK:
	{
		scan_code |= 0x100; // set extended bit
		break;
	}
	}

	scan_code <<= 16;

	char key_name[128];
	if (GetKeyNameTextA(scan_code, key_name, std::size(key_name)) != 0)
		return key_name;
	else
		return fmt::format("key_{}", button);
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	return gdk_keyval_name(button);
#else
	return fmt::format("key_{}", button);
#endif
}

bool WindowSystem::InputConfigWindowHasFocus()
{
	return g_inputConfigWindowHasFocus;
}

void WindowSystem::NotifyGameLoaded()
{
	(void)QueueFrameCallback([](MainWindow& frame) {
		frame.OnGameLoaded();
		frame.UpdateSettingsAfterGameLaunch();
	});
}

void WindowSystem::NotifyGameExited()
{
	(void)QueueFrameCallback([](MainWindow& frame) {
		frame.RestoreSettingsAfterGameExited();
	});
}

void WindowSystem::RefreshGameList()
{
	(void)QueueFrameCallback([](MainWindow& frame) {
		frame.RequestGameListRefresh();
	});
}

void WindowSystem::CaptureInput(const ControllerState& currentState, const ControllerState& lastState)
{
	HotkeySettings::CaptureInput(currentState, lastState);
}

bool WindowSystem::IsFullScreen()
{
	auto state = WxFrontendRuntime::GetWindowState();
	return state && state->is_fullscreen.load();
}
