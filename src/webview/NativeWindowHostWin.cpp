#include "webview/NativeWindowHost.h"

#if defined(_WIN32)

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace WebFrontend
{
	namespace
	{
		constexpr wchar_t MainWindowClass[] = L"CemuExtendWebMainWindow";
		constexpr wchar_t RenderWindowClass[] = L"CemuExtendRenderRegion";
		constexpr UINT FirstCommandId = 0x2000;

		void RegisterWindowClasses()
		{
			static const bool registered = [] {
				const auto instance = GetModuleHandleW(nullptr);
				WNDCLASSEXW render{sizeof(render)};
				render.hInstance = instance;
				render.lpfnWndProc = DefWindowProcW;
				render.lpszClassName = RenderWindowClass;
				render.hCursor = LoadCursorW(nullptr, IDC_ARROW);
				render.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
				if (!RegisterClassExW(&render) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
					return false;
				return true;
			}();
			if (!registered)
				throw std::runtime_error("failed to register native render window class");
		}

		class WinRenderRegion final : public Host::IRenderRegion
		{
		public:
			explicit WinRenderRegion(HWND parent)
			{
				RegisterWindowClasses();
				m_window = CreateWindowExW(0, RenderWindowClass, L"", WS_CHILD | WS_CLIPSIBLINGS |
					WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 1, 1, parent, nullptr,
					GetModuleHandleW(nullptr), nullptr);
				if (!m_window)
					throw std::runtime_error("failed to create native render region");
			}

			~WinRenderRegion() override { PrepareForDestroy(); }
			Host::NativeWindowHandle GetWindowHandle() const override
			{
				return {Host::NativeWindowBackend::Windows, nullptr, GetParent(m_window)};
			}
			Host::NativeWindowHandle GetSurfaceHandle() const override
			{
				return {Host::NativeWindowBackend::Windows, nullptr, m_window};
			}
			Host::RenderRegionBounds GetBounds() const override
			{
				RECT bounds{};
				GetClientRect(m_window, &bounds);
				POINT origin{};
				ClientToScreen(m_window, &origin);
				ScreenToClient(GetParent(m_window), &origin);
				return {origin.x, origin.y, bounds.right, bounds.bottom};
			}
			void SetBounds(Host::RenderRegionBounds bounds) override
			{
				MoveWindow(m_window, bounds.x, bounds.y, std::max(1, bounds.width),
					std::max(1, bounds.height), TRUE);
			}
			void SetVisible(bool visible) override
			{
				ShowWindow(m_window, visible ? SW_SHOW : SW_HIDE);
			}
			void RequestFocus() override { SetFocus(m_window); }
			void PrepareForDestroy() override
			{
				if (std::exchange(m_prepared, true) || !m_window)
					return;
				DestroyWindow(m_window);
				m_window = nullptr;
			}
			HWND Window() const { return m_window; }

		private:
			HWND m_window{};
			bool m_prepared{};
		};

		class WinWindowHost final : public INativeWindowHost
		{
		public:
			WinWindowHost()
			{
				RegisterWindowClasses();
				WNDCLASSEXW windowClass{sizeof(windowClass)};
				windowClass.hInstance = GetModuleHandleW(nullptr);
				windowClass.lpfnWndProc = &WinWindowHost::WindowProc;
				windowClass.lpszClassName = MainWindowClass;
				windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
				windowClass.hIcon = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(1));
				windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
				if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
					throw std::runtime_error("failed to register native main window class");
				m_menu = BuildMenu();
				m_window = CreateWindowExW(0, MainWindowClass, L"CemuExtend",
					WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
					1120, 760, nullptr, m_menu, GetModuleHandleW(nullptr), this);
				if (!m_window)
					throw std::runtime_error("failed to create native main window");
			}

			~WinWindowHost() override
			{
				DestroyMainRenderRegion();
				if (m_window)
					DestroyWindow(m_window);
				if (m_menu)
					DestroyMenu(m_menu);
			}

			void* GetNativeWindow() const override { return m_window; }
			Host::NativeWindowHandle GetMainWindowHandle() const override
			{
				return {Host::NativeWindowBackend::Windows, nullptr, m_window};
			}
			Host::WindowMetricsSnapshot GetMetrics() const override
			{
				RECT area{};
				GetClientRect(m_window, &area);
				const auto dpi = GetDpiForWindow(m_window);
				return {
					.appActive = GetForegroundWindow() == m_window,
					.fullscreen = m_fullscreen,
					.width = area.right,
					.height = area.bottom,
					.physicalWidth = area.right,
					.physicalHeight = area.bottom,
					.dpiScale = static_cast<double>(dpi) / 96.0,
				};
			}
			void AttachWebView(void* widget) override
			{
				m_webView = static_cast<HWND>(widget);
				if (!m_webView || GetParent(m_webView) != m_window)
					throw std::logic_error("webview widget is not owned by the native main window");
				ResizeChildren();
			}
			void PrepareWebViewDestroy(void* widget) override
			{
				if (widget == m_webView)
					m_webView = nullptr;
			}
			void Show() override
			{
				ShowWindow(m_window, SW_SHOW);
				UpdateWindow(m_window);
				ShowLibrary();
			}
			void ShowLibrary() override
			{
				if (m_renderRegion)
					m_renderRegion->SetVisible(false);
				if (m_webView)
				{
					EnableWindow(m_webView, TRUE);
					ShowWindow(m_webView, SW_SHOW);
					SetFocus(m_webView);
				}
			}
			Host::IRenderRegion& CreateMainRenderRegion() override
			{
				if (!m_renderRegion)
				{
					m_renderRegion = std::make_unique<WinRenderRegion>(m_window);
					ResizeChildren();
				}
				return *m_renderRegion;
			}
			void DestroyMainRenderRegion() override { m_renderRegion.reset(); }
			void ShowRenderRegion() override
			{
				auto& region = CreateMainRenderRegion();
				if (m_webView)
				{
					EnableWindow(m_webView, FALSE);
					ShowWindow(m_webView, SW_HIDE);
				}
				region.SetVisible(true);
				region.RequestFocus();
			}
			void SetFullscreen(bool fullscreen) override
			{
				if (fullscreen == m_fullscreen)
					return;
				m_fullscreen = fullscreen;
				if (fullscreen)
				{
					m_windowPlacement.length = sizeof(m_windowPlacement);
					GetWindowPlacement(m_window, &m_windowPlacement);
					m_windowStyle = GetWindowLongW(m_window, GWL_STYLE);
					MONITORINFO monitor{sizeof(monitor)};
					GetMonitorInfoW(MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST), &monitor);
					SetMenu(m_window, nullptr);
					SetWindowLongW(m_window, GWL_STYLE, m_windowStyle & ~WS_OVERLAPPEDWINDOW);
					SetWindowPos(m_window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
						monitor.rcMonitor.right - monitor.rcMonitor.left,
						monitor.rcMonitor.bottom - monitor.rcMonitor.top,
						SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
				}
				else
				{
					SetWindowLongW(m_window, GWL_STYLE, m_windowStyle);
					SetMenu(m_window, m_menu);
					SetWindowPlacement(m_window, &m_windowPlacement);
					SetWindowPos(m_window, nullptr, 0, 0, 0, 0,
						SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
				}
			}
			void SetCloseHandler(CloseHandler handler) override { m_closeHandler = std::move(handler); }
			void SetMenuHandler(MenuHandler handler) override { m_menuHandler = std::move(handler); }
			void SetMetricsHandler(MetricsHandler handler) override
			{
				m_metricsHandler = std::move(handler);
				NotifyMetrics();
			}

		private:
			static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
			{
				auto* self = reinterpret_cast<WinWindowHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
				if (message == WM_NCCREATE)
				{
					self = static_cast<WinWindowHost*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
					self->m_window = window;
					SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
				}
				if (!self)
					return DefWindowProcW(window, message, wparam, lparam);
				switch (message)
				{
				case WM_CLOSE:
					if (self->m_closeHandler)
						self->m_closeHandler();
					return 0;
				case WM_SIZE:
				case WM_DPICHANGED:
				case WM_ACTIVATE:
					self->ResizeChildren();
					self->NotifyMetrics();
					return 0;
				case WM_COMMAND:
					if (HIWORD(wparam) == 0 && LOWORD(wparam) >= FirstCommandId &&
						LOWORD(wparam) < FirstCommandId + 12 && self->m_menuHandler)
						self->m_menuHandler(static_cast<MenuCommand>(LOWORD(wparam) - FirstCommandId));
					return 0;
				case WM_NCDESTROY:
					SetWindowLongPtrW(window, GWLP_USERDATA, 0);
					self->m_window = nullptr;
					break;
				}
				return DefWindowProcW(window, message, wparam, lparam);
			}

			void ResizeChildren()
			{
				if (!m_window)
					return;
				RECT area{};
				GetClientRect(m_window, &area);
				if (m_webView)
					MoveWindow(m_webView, 0, 0, area.right, area.bottom, TRUE);
				if (m_renderRegion)
					m_renderRegion->SetBounds({0, 0, area.right, area.bottom});
			}

			void NotifyMetrics()
			{
				if (m_metricsHandler && m_window)
					m_metricsHandler(GetMetrics());
			}

			HMENU BuildMenu()
			{
				auto* bar = CreateMenu();
				auto append = [](HMENU menu, const wchar_t* text, MenuCommand command) {
					AppendMenuW(menu, MF_STRING, FirstCommandId + static_cast<UINT>(command), text);
				};
				auto* file = CreatePopupMenu();
				append(file, L"&Load...", MenuCommand::Load);
				append(file, L"&End emulation", MenuCommand::EndEmulation);
				append(file, L"E&xit", MenuCommand::Exit);
				AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
				auto* options = CreatePopupMenu();
				append(options, L"&Fullscreen", MenuCommand::ToggleFullscreen);
				append(options, L"Separate &GamePad view", MenuCommand::TogglePadView);
				append(options, L"General Settings", MenuCommand::GeneralSettings);
				append(options, L"Input Settings", MenuCommand::InputSettings);
				AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(options), L"&Options");
				auto* tools = CreatePopupMenu();
				append(tools, L"Graphic Packs", MenuCommand::GraphicPacks);
				append(tools, L"Title Manager", MenuCommand::TitleManager);
				AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"&Tools");
				AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(CreatePopupMenu()), L"&CPU");
				AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(CreatePopupMenu()), L"&NFC");
				auto* debug = CreatePopupMenu();
				append(debug, L"Logging", MenuCommand::Logging);
				AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(debug), L"&Debug");
				auto* help = CreatePopupMenu();
				append(help, L"About", MenuCommand::About);
				AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
				return bar;
			}

			HWND m_window{};
			HWND m_webView{};
			HMENU m_menu{};
			std::unique_ptr<WinRenderRegion> m_renderRegion;
			CloseHandler m_closeHandler;
			MenuHandler m_menuHandler;
			MetricsHandler m_metricsHandler;
			WINDOWPLACEMENT m_windowPlacement{sizeof(m_windowPlacement)};
			LONG m_windowStyle{};
			bool m_fullscreen{};
		};
	}

	std::unique_ptr<INativeWindowHost> CreateNativeWindowHost()
	{
		return std::make_unique<WinWindowHost>();
	}
}

#endif
