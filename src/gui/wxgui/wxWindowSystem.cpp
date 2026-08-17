#include "input/HotkeySettings.h"
#include "input/InputManager.h"
#include "audio/IAudioAPI.h"
#include "application/ApplicationHost.h"
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
#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"
#include "config/CemuConfig.h"
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/utils.h>

#include <chrono>
#include <future>
#include <unordered_map>

WindowSystem::WindowInfo g_window_info{};

std::shared_mutex g_mutex;
MainWindow* g_mainFrame = nullptr;

namespace
{
	std::atomic_bool s_wxFrontendStopping{false};
	struct NativeHandleLease
	{
		explicit NativeHandleLease(std::shared_mutex& mutex) : lock(mutex) {}
		std::shared_lock<std::shared_mutex> lock;
	};
	std::mutex s_pendingUiMutex;
	std::uint64_t s_nextPendingUiId{1};
	std::unordered_map<std::uint64_t, std::function<void()>> s_pendingUiCancellation;

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

	template<typename Callback>
	bool QueueFrameCallback(Callback&& callback)
	{
		if (s_wxFrontendStopping.load(std::memory_order_acquire) || !wxTheApp)
			return false;
		wxTheApp->CallAfter([callback = std::forward<Callback>(callback)]() mutable {
			if (s_wxFrontendStopping.load(std::memory_order_acquire))
				return;
			MainWindow* frame{};
			{
				std::shared_lock lock(g_mutex);
				frame = g_mainFrame;
			}
			// Frame destruction and CallAfter callbacks are serialized by the wx UI
			// thread, so the pointer remains valid for this callback invocation.
			if (frame)
				callback(*frame);
		});
		return true;
	}

	class WxHostServices final : public Host::IWindowMetrics,
		public Host::INativeSurfaceProvider,
		public Host::IKeyboardState,
		public Host::IClipboard,
		public Host::IExternalLauncher,
		public Host::IInputFocus,
		public Host::ICanvasHost,
		public Input::IControllerStateObserver
	{
	public:
		Host::WindowMetricsSnapshot GetWindowMetrics() const override
		{
			std::shared_lock lock(g_mutex);
			return {
				.appActive = g_window_info.app_active,
				.padOpen = g_window_info.pad_open,
				.fullscreen = g_window_info.is_fullscreen,
				.debuggerFocused = g_window_info.debugger_focused,
				.width = g_window_info.width,
				.height = g_window_info.height,
				.physicalWidth = g_window_info.phys_width,
				.physicalHeight = g_window_info.phys_height,
				.padWidth = g_window_info.pad_width,
				.padHeight = g_window_info.pad_height,
				.physicalPadWidth = g_window_info.phys_pad_width,
				.physicalPadHeight = g_window_info.phys_pad_height,
				.dpiScale = g_window_info.dpi_scale,
				.padDpiScale = g_window_info.pad_dpi_scale,
			};
		}

		Host::NativeSurfaceSnapshot GetNativeSurfaces() const override
		{
			auto lease = std::make_shared<NativeHandleLease>(g_mutex);
			return {
				.mainWindow = g_window_info.window_main,
				.padWindow = g_window_info.window_pad,
				.mainSurface = g_window_info.canvas_main,
				.padSurface = g_window_info.canvas_pad,
				.lifetime = std::move(lease),
			};
		}

		bool IsKeyDown(Host::Key key) const override
		{
			using Host::Key;
			switch (key)
			{
			case Key::LeftControl:
				return WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::LCONTROL);
			case Key::RightControl:
				return WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::RCONTROL);
			case Key::Tab:
				return WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::TAB);
			case Key::Escape:
				return WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::ESCAPE);
			}
			return false;
		}

		std::string GetKeyName(std::uint32_t key) const override
		{
			return WindowSystem::GetKeyCodeName(key);
		}

		std::vector<Host::KeyState> GetKeyStates() const override
		{
			std::vector<Host::KeyState> states;
			g_window_info.iter_keystates([&states](const auto& state) {
				states.push_back({state.first, state.second});
			});
			return states;
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
			if (s_wxFrontendStopping.load(std::memory_order_acquire) || !wxTheApp)
				return false;
			wxTheApp->CallAfter([url = std::move(url)] {
				if (s_wxFrontendStopping.load(std::memory_order_acquire))
					return;
				if (!wxLaunchDefaultBrowser(wxString::FromUTF8(url)))
					cemuLog_log(LogType::Force, "Failed to open host browser URL: {}", url);
			});
			return true;
		}

		bool InputConfigurationHasFocus() const override
		{
			return WindowSystem::InputConfigWindowHasFocus();
		}

		bool RecreateCanvas() override
		{
			if (wxIsMainThread())
			{
				MainWindow* frame{};
				{
					std::shared_lock lock(g_mutex);
					frame = g_mainFrame;
				}
				const bool available = frame &&
					!s_wxFrontendStopping.load(std::memory_order_acquire);
				if (available)
					frame->RecreateCanvasForHost();
				return available;
			}
			auto result = std::make_shared<std::promise<bool>>();
			auto future = result->get_future();
			if (!wxTheApp)
				return false;
			const auto pending = RegisterPendingUi([result] { result->set_value(false); });
			if (!pending)
				return false;
			wxTheApp->CallAfter([id = *pending, result] {
				if (!IsPendingUi(id))
					return;
				MainWindow* frame{};
				{
					std::shared_lock lock(g_mutex);
					frame = g_mainFrame;
				}
				const bool available = frame &&
					!s_wxFrontendStopping.load(std::memory_order_acquire);
				if (available)
					frame->RecreateCanvasForHost();
				if (CompletePendingUi(id))
					result->set_value(available);
			});
			// Keep the request registered until recreation finishes. BeginShutdown()
			// cancels queued work and resolves this future; timing out after the UI
			// has started recreating would race title rollback against canvas creation.
			return future.get();
		}

	};

	std::shared_ptr<WxHostServices> s_wxHostServices;

	void InstallWxHostServices()
	{
		s_wxFrontendStopping.store(false, std::memory_order_release);
		s_wxHostServices = std::make_shared<WxHostServices>();
		Application::ConnectHost({
			.windowMetrics = std::static_pointer_cast<Host::IWindowMetrics>(s_wxHostServices),
			.clipboard = std::static_pointer_cast<Host::IClipboard>(s_wxHostServices),
			.externalLauncher = std::static_pointer_cast<Host::IExternalLauncher>(s_wxHostServices),
			.inputFocus = std::static_pointer_cast<Host::IInputFocus>(s_wxHostServices),
			.canvas = std::static_pointer_cast<Host::ICanvasHost>(s_wxHostServices),
		});
		InputManager::instance().ConfigureHost(*s_wxHostServices, *s_wxHostServices,
			*s_wxHostServices, *s_wxHostServices);
		InputManager::instance().Start();
		IAudioAPI::ConfigureNativeSurfaceProvider(s_wxHostServices.get());
	}
}

#if BOOST_OS_WINDOWS
void _wxLaunch()
{
	SetThreadName("MainThread_UI");
	wxEntry();
}
#endif

void WindowSystem::Create()
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
	InputManager::instance().Shutdown();
	Application::DisconnectHost();
	InputManager::instance().ClearHost();
	IAudioAPI::ConfigureNativeSurfaceProvider(nullptr);
	s_wxHostServices.reset();
}

std::shared_ptr<Host::IWindowMetrics> WindowSystem::GetWindowMetricsHost()
{
	return s_wxHostServices;
}

std::shared_ptr<Host::INativeSurfaceProvider> WindowSystem::GetNativeSurfaceHost()
{
	return s_wxHostServices;
}

bool WindowSystem::IsShuttingDown()
{
	return s_wxFrontendStopping.load(std::memory_order_acquire);
}

void WindowSystem::BeginShutdown()
{
	s_wxFrontendStopping.store(true, std::memory_order_release);
	CancelPendingUi();
}

void WindowSystem::ResumeAfterFailedShutdown()
{
	s_wxFrontendStopping.store(false, std::memory_order_release);
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

WindowSystem::WindowInfo& WindowSystem::GetWindowInfo()
{
	return g_window_info;
}

void WindowSystem::PublishMainWindowHandle(WindowHandleInfo handle)
{
	std::unique_lock lock(g_mutex);
	g_window_info.window_main = handle;
}

void WindowSystem::PublishPadWindowHandle(WindowHandleInfo handle)
{
	std::unique_lock lock(g_mutex);
	g_window_info.window_pad = handle;
}

void WindowSystem::PublishCanvasHandle(bool mainWindow, WindowHandleInfo handle)
{
	std::unique_lock lock(g_mutex);
	(mainWindow ? g_window_info.canvas_main : g_window_info.canvas_pad) = handle;
}

void WindowSystem::GetClipboardTextAsync(std::function<void(bool, std::string)> callback)
{
	auto sharedCallback = std::make_shared<decltype(callback)>(std::move(callback));
	if (!wxTheApp)
	{
		(*sharedCallback)(false, {});
		return;
	}
	const auto pending = RegisterPendingUi([sharedCallback] { (*sharedCallback)(false, {}); });
	if (!pending)
	{
		(*sharedCallback)(false, {});
		return;
	}
	wxTheApp->CallAfter([id = *pending, sharedCallback]() mutable {
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
	});
}

void WindowSystem::SetClipboardTextAsync(std::string text, std::function<void(bool)> callback)
{
	auto sharedCallback = std::make_shared<decltype(callback)>(std::move(callback));
	if (!wxTheApp)
	{
		(*sharedCallback)(false);
		return;
	}
	const auto pending = RegisterPendingUi([sharedCallback] { (*sharedCallback)(false); });
	if (!pending)
	{
		(*sharedCallback)(false);
		return;
	}
	wxTheApp->CallAfter([id = *pending, text = std::move(text), sharedCallback]() mutable {
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
	});
}

void WindowSystem::UpdateWindowTitles(bool isIdle, bool isLoading, double fps,
	std::optional<Application::WindowTitlePresentation> presentation)
{
	std::string windowText;
	windowText = BUILD_VERSION_WITH_NAME_STRING;

	if (isIdle)
	{
		std::shared_lock lock(g_mutex);
		if (g_mainFrame)
			g_mainFrame->AsyncSetTitle(windowText);
		return;
	}
	if (isLoading)
	{
		windowText.append(" - Loading...");
		std::shared_lock lock(g_mutex);
		if (g_mainFrame)
			g_mainFrame->AsyncSetTitle(windowText);
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

	std::shared_lock lock(g_mutex);
	if (g_mainFrame)
	{
		g_mainFrame->AsyncSetTitle(windowText);
		auto* pad = g_mainFrame->GetPadView();
		if (pad)
			pad->AsyncSetTitle(fmt::format("{} - FPS: {:.02f}", _("GamePad View").utf8_string(), fps));
	}
}

void WindowSystem::GetWindowSize(int& w, int& h)
{
	w = g_window_info.width;
	h = g_window_info.height;
}

void WindowSystem::GetPadWindowSize(int& w, int& h)
{
	if (g_window_info.pad_open)
	{
		w = g_window_info.pad_width;
		h = g_window_info.pad_height;
	}
	else
	{
		w = 0;
		h = 0;
	}
}

void WindowSystem::GetWindowPhysSize(int& w, int& h)
{
	w = g_window_info.phys_width;
	h = g_window_info.phys_height;
}

void WindowSystem::GetPadWindowPhysSize(int& w, int& h)
{
	if (g_window_info.pad_open)
	{
		w = g_window_info.phys_pad_width;
		h = g_window_info.phys_pad_height;
	}
	else
	{
		w = 0;
		h = 0;
	}
}

double WindowSystem::GetWindowDPIScale()
{
	return g_window_info.dpi_scale;
}

double WindowSystem::GetPadDPIScale()
{
	return g_window_info.pad_open ? g_window_info.pad_dpi_scale.load() : 1.0;
}

bool WindowSystem::IsPadWindowOpen()
{
	return g_window_info.pad_open;
}

bool WindowSystem::IsKeyDown(uint32 key)
{
	return g_window_info.get_keystate(key);
}

bool WindowSystem::IsKeyDown(PlatformKeyCodes platformKey)
{
	uint32 key = 0;

	switch (platformKey)
	{
#if BOOST_OS_WINDOWS
	case PlatformKeyCodes::LCONTROL:
		key = VK_LCONTROL;
		break;
	case PlatformKeyCodes::RCONTROL:
		key = VK_RCONTROL;
		break;
	case PlatformKeyCodes::TAB:
		key = VK_TAB;
		break;
	case PlatformKeyCodes::ESCAPE:
		key = VK_ESCAPE;
		break;
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	case PlatformKeyCodes::LCONTROL:
		key = GDK_KEY_Control_L;
		break;
	case PlatformKeyCodes::RCONTROL:
		key = GDK_KEY_Control_R;
		break;
	case PlatformKeyCodes::TAB:
		key = GDK_KEY_Tab;
		break;
	case PlatformKeyCodes::ESCAPE:
		key = GDK_KEY_Escape;
		break;
#elif BOOST_OS_MACOS
	case PlatformKeyCodes::LCONTROL:
		key = kVK_Control;
		break;
	case PlatformKeyCodes::RCONTROL:
		key = kVK_RightControl;
		break;
	case PlatformKeyCodes::TAB:
		key = kVK_Tab;
		break;
	case PlatformKeyCodes::ESCAPE:
		key = kVK_Escape;
		break;
#endif
	default:
		return false;
	}

	return WindowSystem::IsKeyDown(key);
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
	std::shared_lock lock(g_mutex);
	if (g_mainFrame)
	{
		g_mainFrame->OnGameLoaded();
		g_mainFrame->UpdateSettingsAfterGameLaunch();
	}
}

void WindowSystem::NotifyGameExited()
{
	std::shared_lock lock(g_mutex);
	if (g_mainFrame)
		g_mainFrame->RestoreSettingsAfterGameExited();
}

void WindowSystem::RefreshGameList()
{
	std::shared_lock lock(g_mutex);
	if (g_mainFrame)
	{
		g_mainFrame->RequestGameListRefresh();
	}
}

void WindowSystem::CaptureInput(const ControllerState& currentState, const ControllerState& lastState)
{
	HotkeySettings::CaptureInput(currentState, lastState);
}

bool WindowSystem::IsFullScreen()
{
	return g_window_info.is_fullscreen;
}
