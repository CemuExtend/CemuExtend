#include "webview/NativeWindowHost.h"

#if defined(_WIN32)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbt.h>
#include <commdlg.h>
#include <imm.h>
#include <shellapi.h>

namespace WebFrontend
{
	namespace
	{
		constexpr wchar_t MainWindowClass[] = L"CemuExtendWebMainWindow";
		constexpr wchar_t RenderWindowClass[] = L"CemuExtendRenderRegion";
		constexpr wchar_t PadWindowClass[] = L"CemuExtendPadRenderRegion";

		std::string Utf8(std::wstring_view text)
		{
			if (text.empty())
				return {};
			const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
													text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			if (length <= 0)
				return {};
			std::string result(length, '\0');
			WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
								static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
			return result;
		}

		std::wstring Wide(std::string_view text)
		{
			if (text.empty())
				return {};
			const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
													text.data(), static_cast<int>(text.size()), nullptr, 0);
			if (length <= 0)
				return {};
			std::wstring result(length, L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
								static_cast<int>(text.size()), result.data(), length);
			return result;
		}

		struct WinInputBinding
		{
			INativeWindowHost::InputHandler* handler{};
			Host::PointerSurface surface{Host::PointerSurface::Main};
			HCURSOR cursor{LoadCursorW(nullptr, IDC_ARROW)};
			bool hideCursor{};
			bool captured{};
			bool rawRegistered{};
			wchar_t highSurrogate{};
		};

		std::uint8_t KeyModifiers()
		{
			return static_cast<std::uint8_t>(
				((GetKeyState(VK_CONTROL) & 0x8000) ? 1U : 0U) |
				((GetKeyState(VK_SHIFT) & 0x8000) ? 2U : 0U) |
				((GetKeyState(VK_MENU) & 0x8000) ? 4U : 0U) |
				(((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? 8U : 0U));
		}

		LRESULT DispatchInput(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
							  WinInputBinding* binding)
		{
			if (!binding || !binding->handler || !*binding->handler)
				return DefWindowProcW(window, message, wparam, lparam);
			RECT client{};
			GetClientRect(window, &client);
			auto emit = [&](NativeInputEvent event) {
				event.surface = binding->surface;
				event.contentWidth = client.right;
				event.contentHeight = client.bottom;
				(*binding->handler)(event);
			};
			auto pointer = [&](NativeInputKind kind, std::uint32_t button, bool pressed) {
				const auto x = static_cast<std::int16_t>(LOWORD(lparam));
				const auto y = static_cast<std::int16_t>(HIWORD(lparam));
				emit({.kind = kind, .x = x, .y = y, .button = button, .pressed = pressed, .insideContent = x >= 0 && y >= 0 && x < client.right && y < client.bottom});
			};
			switch (message)
			{
			case WM_MOUSEMOVE:
				if (binding->captured)
				{
					if (!binding->rawRegistered)
					{
						const auto x = static_cast<std::int16_t>(LOWORD(lparam));
						const auto y = static_cast<std::int16_t>(HIWORD(lparam));
						const auto centerX = client.right / 2, centerY = client.bottom / 2;
						if (x != centerX || y != centerY)
						{
							emit({.kind = NativeInputKind::RawMouse,
								  .deltaX = x - centerX,
								  .deltaY = y - centerY});
							POINT center{centerX, centerY};
							ClientToScreen(window, &center);
							SetCursorPos(center.x, center.y);
						}
					}
					return 0;
				}
				pointer(NativeInputKind::PointerMove, 0, false);
				return 0;
			case WM_LBUTTONDOWN:
				SetFocus(window);
				pointer(NativeInputKind::PointerButton, 1, true);
				return 0;
			case WM_LBUTTONUP:
				pointer(NativeInputKind::PointerButton, 1, false);
				return 0;
			case WM_RBUTTONDOWN:
				SetFocus(window);
				pointer(NativeInputKind::PointerButton, 3, true);
				return 0;
			case WM_RBUTTONUP:
				pointer(NativeInputKind::PointerButton, 3, false);
				return 0;
			case WM_MBUTTONDOWN:
				SetFocus(window);
				pointer(NativeInputKind::PointerButton, 2, true);
				return 0;
			case WM_MBUTTONUP:
				pointer(NativeInputKind::PointerButton, 2, false);
				return 0;
			case WM_XBUTTONDOWN:
				pointer(NativeInputKind::PointerButton,
						GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 8 : 9, true);
				return TRUE;
			case WM_XBUTTONUP:
				pointer(NativeInputKind::PointerButton,
						GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 8 : 9, false);
				return TRUE;
			case WM_MOUSEWHEEL:
			case WM_MOUSEHWHEEL:
			{
				POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
				ScreenToClient(window, &point);
				emit({.kind = NativeInputKind::PointerWheel,
					  .x = point.x,
					  .y = point.y,
					  .wheelX = message == WM_MOUSEHWHEEL ? GET_WHEEL_DELTA_WPARAM(wparam) : 0,
					  .wheelY = message == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(wparam) : 0,
					  .insideContent = true});
				return 0;
			}
			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYUP:
				emit({.kind = NativeInputKind::Key, .key = static_cast<std::uint32_t>(wparam), .modifiers = KeyModifiers(), .pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN, .repeat = (lparam & (1LL << 30)) != 0});
				return 0;
			case WM_CHAR:
			{
				const auto character = static_cast<wchar_t>(wparam);
				if (IS_HIGH_SURROGATE(character))
				{
					binding->highSurrogate = character;
					return 0;
				}
				std::wstring text;
				if (binding->highSurrogate && IS_LOW_SURROGATE(character))
					text = {binding->highSurrogate, character};
				else
					text = {character};
				binding->highSurrogate = 0;
				emit({.kind = NativeInputKind::Character,
					  .repeat = (lparam & (1LL << 30)) != 0,
					  .text = Utf8(text)});
				return 0;
			}
			case WM_KILLFOCUS:
				emit({.kind = NativeInputKind::FocusLost});
				return 0;
			case WM_INPUT:
			{
				UINT size{};
				if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr,
									&size, sizeof(RAWINPUTHEADER)) != 0 ||
					size < sizeof(RAWINPUTHEADER))
					return 0;
				std::vector<std::byte> storage(size);
				if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, storage.data(),
									&size, sizeof(RAWINPUTHEADER)) != size)
					return 0;
				const auto& raw = *reinterpret_cast<const RAWINPUT*>(storage.data());
				if (raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
					emit({.kind = NativeInputKind::RawMouse,
						  .deltaX = raw.data.mouse.lLastX,
						  .deltaY = raw.data.mouse.lLastY});
				return 0;
			}
			case WM_POINTERDOWN:
			case WM_POINTERUPDATE:
			case WM_POINTERUP:
			{
				POINTER_INFO info{};
				if (!GetPointerInfo(GET_POINTERID_WPARAM(wparam), &info))
					return 0;
				POINT point = info.ptPixelLocation;
				ScreenToClient(window, &point);
				emit({.kind = NativeInputKind::Touch, .x = point.x, .y = point.y, .touchId = GET_POINTERID_WPARAM(wparam), .pressed = message != WM_POINTERUP, .insideContent = true});
				return 0;
			}
			case WM_SETCURSOR:
				SetCursor(binding->hideCursor ? nullptr : binding->cursor);
				return TRUE;
			default:
				return DefWindowProcW(window, message, wparam, lparam);
			}
		}

		LRESULT CALLBACK RenderWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
		{
			auto* binding = reinterpret_cast<WinInputBinding*>(GetWindowLongPtrW(window, GWLP_USERDATA));
			if (message == WM_NCCREATE)
			{
				binding = static_cast<WinInputBinding*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
				SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(binding));
			}
			if (message == WM_NCDESTROY)
				SetWindowLongPtrW(window, GWLP_USERDATA, 0);
			return DispatchInput(window, message, wparam, lparam, binding);
		}

		void RegisterWindowClasses()
		{
			static const bool registered = [] {
				const auto instance = GetModuleHandleW(nullptr);
				WNDCLASSEXW render{sizeof(render)};
				render.hInstance = instance;
				render.lpfnWndProc = RenderWindowProc;
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
			explicit WinRenderRegion(HWND parent, INativeWindowHost::InputHandler* handler)
			{
				m_binding.handler = handler;
				m_binding.surface = Host::PointerSurface::Main;
				RegisterWindowClasses();
				m_window = CreateWindowExW(0, RenderWindowClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 1, 1, parent, nullptr,
										   GetModuleHandleW(nullptr), &m_binding);
				if (!m_window)
					throw std::runtime_error("failed to create native render region");
			}

			~WinRenderRegion() override
			{
				PrepareForDestroy();
			}
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
			void RequestFocus() override
			{
				SetFocus(m_window);
			}
			void PrepareForDestroy() override
			{
				if (std::exchange(m_prepared, true) || !m_window)
					return;
				DestroyWindow(m_window);
				m_window = nullptr;
			}
			HWND Window() const
			{
				return m_window;
			}
			WinInputBinding& InputBinding()
			{
				return m_binding;
			}

		  private:
			HWND m_window{};
			WinInputBinding m_binding;
			bool m_prepared{};
		};

		class WinPadRenderRegion final : public Host::IRenderRegion
		{
		  public:
			WinPadRenderRegion(const wchar_t* title, Host::PointerSurface surface,
							   std::function<void()> closeHandler,
							   std::function<void()> metricsHandler, INativeWindowHost::InputHandler* handler)
				: m_closeHandler(std::move(closeHandler)),
				  m_metricsHandler(std::move(metricsHandler))
			{
				m_binding.handler = handler;
				m_binding.surface = surface;
				WNDCLASSEXW windowClass{sizeof(windowClass)};
				windowClass.hInstance = GetModuleHandleW(nullptr);
				windowClass.lpfnWndProc = &WinPadRenderRegion::WindowProc;
				windowClass.lpszClassName = PadWindowClass;
				windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
				windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
				if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
					throw std::runtime_error("failed to register the GamePad render window class");
				const int initialWidth = surface == Host::PointerSurface::Main ? 1296 : 870;
				const int initialHeight = surface == Host::PointerSurface::Main ? 759 : 520;
				m_window = CreateWindowExW(0, PadWindowClass, title,
										   WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
										   initialWidth, initialHeight, nullptr, nullptr, GetModuleHandleW(nullptr), this);
				if (!m_window)
					throw std::runtime_error("failed to create the native GamePad render window");
				ShowWindow(m_window, SW_SHOW);
				UpdateWindow(m_window);
			}

			~WinPadRenderRegion() override
			{
				PrepareForDestroy();
			}
			Host::NativeWindowHandle GetWindowHandle() const override
			{
				return {Host::NativeWindowBackend::Windows, nullptr, m_window};
			}
			Host::NativeWindowHandle GetSurfaceHandle() const override
			{
				return GetWindowHandle();
			}
			Host::RenderRegionBounds GetBounds() const override
			{
				RECT area{};
				GetClientRect(m_window, &area);
				return {0, 0, area.right, area.bottom};
			}
			void SetBounds(Host::RenderRegionBounds bounds) override
			{
				RECT frame{0, 0, std::max(1, bounds.width), std::max(1, bounds.height)};
				AdjustWindowRectEx(&frame, static_cast<DWORD>(GetWindowLongW(m_window, GWL_STYLE)),
								   FALSE, static_cast<DWORD>(GetWindowLongW(m_window, GWL_EXSTYLE)));
				SetWindowPos(m_window, nullptr, 0, 0, frame.right - frame.left,
							 frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			}
			void SetVisible(bool visible) override
			{
				ShowWindow(m_window, visible ? SW_SHOW : SW_HIDE);
			}
			void RequestFocus() override
			{
				SetForegroundWindow(m_window);
				SetFocus(m_window);
			}
			HWND Window() const
			{
				return m_window;
			}
			void SetFullscreen(bool fullscreen)
			{
				if (!m_window || fullscreen == m_fullscreen)
					return;
				m_fullscreen = fullscreen;
				if (fullscreen)
				{
					m_windowPlacement.length = sizeof(m_windowPlacement);
					GetWindowPlacement(m_window, &m_windowPlacement);
					m_windowStyle = GetWindowLongW(m_window, GWL_STYLE);
					MONITORINFO monitor{sizeof(monitor)};
					GetMonitorInfoW(MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST), &monitor);
					SetWindowLongW(m_window, GWL_STYLE, m_windowStyle & ~WS_OVERLAPPEDWINDOW);
					SetWindowPos(m_window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
								 monitor.rcMonitor.right - monitor.rcMonitor.left,
								 monitor.rcMonitor.bottom - monitor.rcMonitor.top,
								 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
				}
				else
				{
					SetWindowLongW(m_window, GWL_STYLE, m_windowStyle);
					SetWindowPlacement(m_window, &m_windowPlacement);
					SetWindowPos(m_window, nullptr, 0, 0, 0, 0,
								 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
									 SWP_NOZORDER | SWP_NOOWNERZORDER);
				}
			}
			void PrepareForDestroy() override
			{
				if (std::exchange(m_prepared, true) || !m_window)
					return;
				m_closeHandler = {};
				m_metricsHandler = {};
				DestroyWindow(m_window);
				m_window = nullptr;
			}
			WinInputBinding& InputBinding()
			{
				return m_binding;
			}

		  private:
			static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
			{
				auto* self = reinterpret_cast<WinPadRenderRegion*>(
					GetWindowLongPtrW(window, GWLP_USERDATA));
				if (message == WM_NCCREATE)
				{
					self = static_cast<WinPadRenderRegion*>(
						reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
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
					if (self->m_metricsHandler)
						self->m_metricsHandler();
					return 0;
				case WM_DPICHANGED:
				{
					const auto* suggested = reinterpret_cast<const RECT*>(lparam);
					SetWindowPos(window, nullptr, suggested->left, suggested->top,
								 suggested->right - suggested->left, suggested->bottom - suggested->top,
								 SWP_NOACTIVATE | SWP_NOZORDER);
					if (self->m_metricsHandler)
						self->m_metricsHandler();
					return 0;
				}
				case WM_NCDESTROY:
					SetWindowLongPtrW(window, GWLP_USERDATA, 0);
					self->m_window = nullptr;
					break;
				}
				return DispatchInput(window, message, wparam, lparam, &self->m_binding);
			}

			HWND m_window{};
			std::function<void()> m_closeHandler;
			std::function<void()> m_metricsHandler;
			WinInputBinding m_binding;
			WINDOWPLACEMENT m_windowPlacement{sizeof(m_windowPlacement)};
			LONG m_windowStyle{};
			bool m_fullscreen{};
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
				m_window = CreateWindowExW(0, MainWindowClass, L"CemuExtend",
										   WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
										   1120, 760, nullptr, nullptr, GetModuleHandleW(nullptr), this);
				if (!m_window)
					throw std::runtime_error("failed to create native main window");
			}

			~WinWindowHost() override
			{
				DestroyPadRenderRegion();
				DestroyMainRenderRegion();
				if (m_window)
					DestroyWindow(m_window);
			}

			void* GetNativeWindow() const override
			{
				return m_window;
			}
			void* GetBrowserParentWindow() const override
			{
				return m_window;
			}
			Host::RenderRegionBounds GetBrowserBounds() const override
			{
				RECT area{};
				if (m_window)
					GetClientRect(m_window, &area);
				return {0, 0,
						static_cast<std::int32_t>(std::max<LONG>(0, area.right - area.left)),
						static_cast<std::int32_t>(std::max<LONG>(0, area.bottom - area.top))};
			}
			double GetBrowserDpiScale() const override
			{
				return m_window ? static_cast<double>(GetDpiForWindow(m_window)) / 96.0 : 1.0;
			}
			void AttachBrowser(void* widget) override
			{
				auto browserWindow = static_cast<HWND>(widget);
				if (!browserWindow || !IsWindow(browserWindow))
					throw std::invalid_argument("browser widget must be a valid HWND");
				if (GetParent(browserWindow) != m_window)
					throw std::logic_error("browser widget is not owned by the native main window");
				m_browserWindow = browserWindow;
				ResizeBrowser();
			}
			void ResizeBrowser() override
			{
				if (!m_window || !m_browserWindow || !IsWindow(m_browserWindow))
					return;
				const auto bounds = GetBrowserBounds();
				MoveWindow(m_browserWindow, bounds.x, bounds.y, std::max(1, bounds.width),
						   std::max(1, bounds.height), TRUE);
			}
			void FocusBrowser() override
			{
				if (m_browserWindow && IsWindow(m_browserWindow))
					SetFocus(m_browserWindow);
			}
			void DetachBrowser(void* widget) override
			{
				if (widget == m_browserWindow)
					m_browserWindow = nullptr;
			}
			Host::NativeWindowHandle GetMainWindowHandle() const override
			{
				return {Host::NativeWindowBackend::Windows, nullptr, m_window};
			}
			Host::WindowMetricsSnapshot GetMetrics() const override
			{
				RECT area{};
				const auto metricsWindow = m_renderRegion ? m_renderRegion->Window() : m_window;
				GetClientRect(metricsWindow, &area);
				const auto dpi = GetDpiForWindow(metricsWindow);
				const auto scale = static_cast<double>(dpi) / 96.0;
				auto metrics = Host::WindowMetricsSnapshot{
					.appActive = GetForegroundWindow() == metricsWindow,
					.fullscreen = m_fullscreen,
					.width = static_cast<std::int32_t>(std::lround(area.right / scale)),
					.height = static_cast<std::int32_t>(std::lround(area.bottom / scale)),
					.physicalWidth = area.right,
					.physicalHeight = area.bottom,
					.dpiScale = scale,
				};
				if (m_padRenderRegion)
				{
					const auto pad = m_padRenderRegion->GetBounds();
					const auto padDpi = GetDpiForWindow(static_cast<HWND>(
						m_padRenderRegion->GetWindowHandle().surface));
					const auto padScale = static_cast<double>(padDpi) / 96.0;
					metrics.padOpen = true;
					metrics.padWidth = static_cast<std::int32_t>(std::lround(pad.width / padScale));
					metrics.padHeight = static_cast<std::int32_t>(std::lround(pad.height / padScale));
					metrics.physicalPadWidth = pad.width;
					metrics.physicalPadHeight = pad.height;
					metrics.padDpiScale = padScale;
				}
				return metrics;
			}
			void Show() override
			{
				ShowWindow(m_window, SW_SHOW);
				UpdateWindow(m_window);
				ShowLibrary();
			}
			void HideLauncher() override
			{
				ShowWindow(m_window, SW_HIDE);
			}
			bool IsLauncherVisible() const override
			{
				return m_window && IsWindowVisible(m_window) != FALSE;
			}
			void ShowLibrary() override
			{
				m_runtimeOverlay = false;
				if (m_browserWindow)
				{
					EnableWindow(m_browserWindow, TRUE);
					ShowWindow(m_browserWindow, SW_SHOW);
					FocusBrowser();
				}
			}
			Host::IRenderRegion& CreateMainRenderRegion() override
			{
				if (!m_renderRegion)
				{
					m_renderRegion = std::make_unique<WinPadRenderRegion>(
						L"CemuExtend Game", Host::PointerSurface::Main,
						[this] { if (m_gameCloseHandler) m_gameCloseHandler(); },
						[this] { NotifyMetrics(); }, &m_inputHandler);
				}
				return *m_renderRegion;
			}
			void DestroyMainRenderRegion() override
			{
				m_renderRegion.reset();
			}
			void ShowRenderRegion() override
			{
				auto& region = CreateMainRenderRegion();
				region.SetVisible(true);
				if (m_browserWindow)
				{
					EnableWindow(m_browserWindow, FALSE);
					ShowWindow(m_browserWindow, SW_HIDE);
				}
				region.RequestFocus();
			}
			Host::IRenderRegion& CreatePadRenderRegion() override
			{
				if (!m_padRenderRegion)
					m_padRenderRegion = std::make_unique<WinPadRenderRegion>(
						L"CemuExtend GamePad", Host::PointerSurface::Pad,
						[this] { if (m_padCloseHandler) m_padCloseHandler(); },
						[this] { if (m_padMetricsEnabled) NotifyMetrics(); }, &m_inputHandler);
				return *m_padRenderRegion;
			}
			void DestroyPadRenderRegion() override
			{
				m_padMetricsEnabled = false;
				m_padRenderRegion.reset();
				NotifyMetrics();
			}
			bool IsPadRenderRegionOpen() const override
			{
				return m_padRenderRegion != nullptr;
			}
			void SetFullscreen(bool fullscreen) override
			{
				m_fullscreen = fullscreen;
				if (m_renderRegion)
					m_renderRegion->SetFullscreen(fullscreen);
			}
			void SetCloseHandler(CloseHandler handler) override
			{
				m_closeHandler = std::move(handler);
			}
			void SetGameCloseHandler(GameCloseHandler handler) override
			{
				m_gameCloseHandler = std::move(handler);
			}
			void SetMetricsHandler(MetricsHandler handler) override
			{
				m_metricsHandler = std::move(handler);
				NotifyMetrics();
			}
			void SetPadCloseHandler(PadCloseHandler handler) override
			{
				m_padCloseHandler = std::move(handler);
			}
			void SetPadMetricsEnabled(bool enabled) override
			{
				m_padMetricsEnabled = enabled;
			}
			void SetInputHandler(InputHandler handler) override
			{
				m_inputHandler = std::move(handler);
			}
			void ApplyPointerPresentation(const NativePointerPresentation& presentation) override
			{
				WinInputBinding* binding{};
				HWND target{};
				if (presentation.surface == Host::PointerSurface::Main && m_renderRegion)
				{
					binding = &m_renderRegion->InputBinding();
					target = m_renderRegion->Window();
				}
				else if (presentation.surface == Host::PointerSurface::Pad && m_padRenderRegion)
				{
					binding = &m_padRenderRegion->InputBinding();
					target = static_cast<HWND>(m_padRenderRegion->GetSurfaceHandle().surface);
				}
				if (!binding || !target)
					return;
				const bool wasCaptured = binding->captured;
				const bool captured = presentation.ownsPointer && !presentation.showCursor;
				binding->hideCursor = !presentation.showCursor;
				binding->captured = captured;
				const wchar_t* cursor = IDC_ARROW;
				switch (presentation.cursor)
				{
				case 1:
					cursor = IDC_IBEAM;
					break;
				case 2:
					cursor = IDC_SIZEALL;
					break;
				case 3:
					cursor = IDC_SIZENS;
					break;
				case 4:
					cursor = IDC_SIZEWE;
					break;
				case 5:
					cursor = IDC_SIZENESW;
					break;
				case 6:
					cursor = IDC_SIZENWSE;
					break;
				case 7:
					cursor = IDC_HAND;
					break;
				case 8:
					cursor = IDC_NO;
					break;
				}
				binding->cursor = LoadCursorW(nullptr, cursor);
				if (presentation.rawMouseEnabled && captured && m_rawInputBinding != binding)
				{
					RAWINPUTDEVICE device{0x01, 0x02, RIDEV_INPUTSINK, target};
					if (RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE)
					{
						if (m_rawInputBinding)
							m_rawInputBinding->rawRegistered = false;
						m_rawInputBinding = binding;
						binding->rawRegistered = true;
					}
				}
				else if ((!captured || !presentation.rawMouseEnabled) && m_rawInputBinding == binding)
				{
					RAWINPUTDEVICE device{0x01, 0x02, RIDEV_REMOVE, nullptr};
					(void)RegisterRawInputDevices(&device, 1, sizeof(device));
					binding->rawRegistered = false;
					m_rawInputBinding = nullptr;
				}
				if (presentation.enteringCapture)
				{
					RECT area{};
					GetClientRect(target, &area);
					POINT center{area.right / 2, area.bottom / 2};
					ClientToScreen(target, &center);
					SetCursorPos(center.x, center.y);
					SetCapture(target);
				}
				if (presentation.confine)
				{
					RECT area{};
					GetClientRect(target, &area);
					POINT upper{area.left, area.top}, lower{area.right, area.bottom};
					ClientToScreen(target, &upper);
					ClientToScreen(target, &lower);
					RECT screen{upper.x, upper.y, lower.x, lower.y};
					ClipCursor(&screen);
				}
				else if (!presentation.confine)
				{
					ClipCursor(nullptr);
				}
				if (wasCaptured && !captured && GetCapture() == target)
					ReleaseCapture();
				InvalidateRect(target, nullptr, FALSE);
			}
			void UpdateTextInput(const NativeTextInputRequest& request) override
			{
				if (!request.active)
				{
					m_textInputSequence = 0;
					if (m_textInput)
						ShowWindow(m_textInput, SW_HIDE);
					if (m_renderRegion)
						m_renderRegion->RequestFocus();
					return;
				}
				if (!m_renderRegion)
					return;
				if (!m_textInput)
				{
					m_textInput = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
												  0, 0, 1, 1, m_window, nullptr, GetModuleHandleW(nullptr), nullptr);
					if (!m_textInput)
						return;
					SetWindowLongPtrW(m_textInput, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
					m_textInputProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(m_textInput,
																				  GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WinWindowHost::TextInputProc)));
				}
				const auto bounds = m_renderRegion->GetBounds();
				const auto x = std::clamp(request.caretX * bounds.width / 1280, 0, std::max(0, bounds.width - 1));
				const auto y = std::clamp(request.caretY * bounds.height / 720, 0, std::max(0, bounds.height - 1));
				MoveWindow(m_textInput, x, y, 1, std::max(1, request.lineHeight * bounds.height / 720), TRUE);
				if (m_textInputSequence != request.sequence)
				{
					m_textInputUpdating = true;
					m_textInputSequence = request.sequence;
					const auto text = Wide(request.initialText);
					SetWindowTextW(m_textInput, text.c_str());
					SendMessageW(m_textInput, EM_SETLIMITTEXT, request.maximumLength, 0);
					SendMessageW(m_textInput, EM_SETSEL, text.size(), text.size());
					m_textInputUpdating = false;
				}
				COMPOSITIONFORM composition{CFS_POINT, {x, y}};
				if (auto context = ImmGetContext(m_textInput))
				{
					ImmSetCompositionWindow(context, &composition);
					ImmReleaseContext(m_textInput, context);
				}
				ShowWindow(m_textInput, SW_SHOW);
				SetFocus(m_textInput);
			}
			std::string GetKeyName(std::uint32_t key) const override
			{
				const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16;
				std::array<wchar_t, 128> text{};
				const auto length = GetKeyNameTextW(scan, text.data(), static_cast<int>(text.size()));
				return length > 0 ? Utf8({text.data(), static_cast<std::size_t>(length)}) : std::string{};
			}
			std::pair<bool, std::string> GetClipboardText() override
			{
				if (!OpenClipboard(m_window))
					return {false, {}};
				const auto handle = GetClipboardData(CF_UNICODETEXT);
				const auto* text = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
				const auto result = text ? std::pair{true, Utf8(text)} : std::pair{false, std::string{}};
				if (text)
					GlobalUnlock(handle);
				CloseClipboard();
				return result;
			}
			bool SetClipboardText(std::string text) override
			{
				const auto wide = Wide(text);
				if (!OpenClipboard(m_window))
					return false;
				EmptyClipboard();
				const auto bytes = (wide.size() + 1) * sizeof(wchar_t);
				auto handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
				if (!handle)
				{
					CloseClipboard();
					return false;
				}
				auto* target = static_cast<wchar_t*>(GlobalLock(handle));
				std::memcpy(target, wide.c_str(), bytes);
				GlobalUnlock(handle);
				const bool success = SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
				if (!success)
					GlobalFree(handle);
				CloseClipboard();
				return success;
			}
			bool SetClipboardImage(std::span<const std::uint8_t> rgb,
								   std::int32_t width, std::int32_t height) override
			{
				if (width <= 0 || height <= 0 || rgb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 ||
					!OpenClipboard(m_window))
					return false;
				const auto stride = (static_cast<std::size_t>(width) * 3 + 3) & ~std::size_t(3);
				const auto pixelBytes = stride * static_cast<std::size_t>(height);
				auto handle = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes);
				if (!handle)
				{
					CloseClipboard();
					return false;
				}
				auto* memory = static_cast<std::uint8_t*>(GlobalLock(handle));
				auto* header = reinterpret_cast<BITMAPINFOHEADER*>(memory);
				*header = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = width, .biHeight = height, .biPlanes = 1, .biBitCount = 24, .biCompression = BI_RGB, .biSizeImage = static_cast<DWORD>(pixelBytes)};
				auto* pixels = memory + sizeof(BITMAPINFOHEADER);
				for (std::int32_t row = 0; row < height; ++row)
				{
					auto* destination = pixels + static_cast<std::size_t>(height - 1 - row) * stride;
					const auto* source = rgb.data() + static_cast<std::size_t>(row) *
														  static_cast<std::size_t>(width) * 3;
					for (std::int32_t column = 0; column < width; ++column)
					{
						destination[column * 3] = source[column * 3 + 2];
						destination[column * 3 + 1] = source[column * 3 + 1];
						destination[column * 3 + 2] = source[column * 3];
					}
				}
				GlobalUnlock(handle);
				EmptyClipboard();
				const bool success = SetClipboardData(CF_DIB, handle) != nullptr;
				if (!success)
					GlobalFree(handle);
				CloseClipboard();
				return success;
			}
			bool OpenExternalUrl(std::string url) override
			{
				const auto wide = Wide(url);
				return reinterpret_cast<std::intptr_t>(ShellExecuteW(m_window, L"open", wide.c_str(),
																	 nullptr, nullptr, SW_SHOWNORMAL)) > 32;
			}
			std::optional<std::string> PickTitleInstallSource() override
			{
				std::array<wchar_t, 32768> path{};
				OPENFILENAMEW dialog{};
				dialog.lStructSize = sizeof(dialog);
				dialog.hwndOwner = m_window;
				dialog.lpstrFilter = L"Wii U title metadata (meta.xml)\0meta.xml\0\0";
				dialog.lpstrFile = path.data();
				dialog.nMaxFile = static_cast<DWORD>(path.size());
				dialog.lpstrTitle = L"Select title to install";
				dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
				if (!GetOpenFileNameW(&dialog))
					return std::nullopt;
				return Utf8(path.data());
			}
			std::optional<std::string> PickWuaDestination(
				std::string suggestedFileName) override
			{
				std::array<wchar_t, 32768> path{};
				const auto suggested = Wide(suggestedFileName);
				std::copy_n(suggested.data(), std::min(suggested.size(), path.size() - 1), path.data());
				OPENFILENAMEW dialog{};
				dialog.lStructSize = sizeof(dialog);
				dialog.hwndOwner = m_window;
				dialog.lpstrFilter = L"Wii U archives (*.wua)\0*.wua\0\0";
				dialog.lpstrFile = path.data();
				dialog.nMaxFile = static_cast<DWORD>(path.size());
				dialog.lpstrDefExt = L"wua";
				dialog.lpstrTitle = L"Save Wii U game archive";
				dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
				if (!GetSaveFileNameW(&dialog))
					return std::nullopt;
				return Utf8(path.data());
			}

		  private:
			static LRESULT CALLBACK TextInputProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
			{
				auto* self = reinterpret_cast<WinWindowHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
				if (!self || !self->m_textInputProc)
					return DefWindowProcW(window, message, wparam, lparam);
				const auto result = CallWindowProcW(self->m_textInputProc, window, message, wparam, lparam);
				if (message == WM_IME_COMPOSITION || message == WM_CHAR || message == WM_PASTE ||
					message == WM_CUT || message == WM_CLEAR || message == WM_UNDO)
					self->DispatchTextComposition();
				if ((message == WM_KEYDOWN || message == WM_KEYUP) && wparam == VK_RETURN &&
					self->m_textPreedit.empty() && self->m_inputHandler)
					self->m_inputHandler({.kind = NativeInputKind::Key, .key = VK_RETURN, .usage = 0x28, .modifiers = KeyModifiers(), .pressed = message == WM_KEYDOWN});
				return result;
			}

			void DispatchTextComposition()
			{
				if (!m_textInput || m_textInputUpdating || !m_inputHandler || !m_textInputSequence)
					return;
				const auto length = GetWindowTextLengthW(m_textInput);
				std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
				if (length > 0)
					GetWindowTextW(m_textInput, text.data(), length + 1);
				text.resize(static_cast<std::size_t>(length));
				m_textPreedit.clear();
				if (auto context = ImmGetContext(m_textInput))
				{
					const auto bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
					if (bytes > 0)
					{
						std::wstring preedit(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
						ImmGetCompositionStringW(context, GCS_COMPSTR, preedit.data(), bytes);
						m_textPreedit = Utf8(preedit);
					}
					ImmReleaseContext(m_textInput, context);
				}
				DWORD selectionStart{}, selectionEnd{};
				SendMessageW(m_textInput, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart),
							 reinterpret_cast<LPARAM>(&selectionEnd));
				selectionStart = std::min<DWORD>(selectionStart, static_cast<DWORD>(text.size()));
				const auto cursor = Utf8(std::wstring_view(text).substr(0, selectionStart)).size();
				m_inputHandler({.kind = NativeInputKind::TextComposition,
								.text = Utf8(text),
								.preedit = m_textPreedit,
								.textCursor = static_cast<std::uint32_t>(cursor),
								.selectionLength = static_cast<std::uint32_t>(m_textPreedit.size()),
								.textSequence = m_textInputSequence});
			}

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
				case WM_ACTIVATE:
					self->ResizeChildren();
					self->NotifyMetrics();
					return 0;
				case WM_DPICHANGED:
				{
					const auto* suggested = reinterpret_cast<const RECT*>(lparam);
					SetWindowPos(window, nullptr, suggested->left, suggested->top,
								 suggested->right - suggested->left, suggested->bottom - suggested->top,
								 SWP_NOACTIVATE | SWP_NOZORDER);
					self->ResizeChildren();
					self->NotifyMetrics();
					return 0;
				}
				case WM_DEVICECHANGE:
					if (wparam == DBT_DEVNODES_CHANGED && self->m_inputHandler)
						self->m_inputHandler({.kind = NativeInputKind::DeviceChanged});
					return 0;
				case WM_NCDESTROY:
					SetWindowLongPtrW(window, GWLP_USERDATA, 0);
					self->m_browserWindow = nullptr;
					self->m_window = nullptr;
					break;
				}
				return DefWindowProcW(window, message, wparam, lparam);
			}

			void ResizeChildren()
			{
				if (!m_window)
					return;
				ResizeBrowser();
			}

			void NotifyMetrics()
			{
				if (m_metricsHandler && m_window)
					m_metricsHandler(GetMetrics());
			}

			HWND m_window{};
			HWND m_browserWindow{};
			bool m_runtimeOverlay{};
			bool m_runtimeOverlayInteractive{};
			HWND m_textInput{};
			WNDPROC m_textInputProc{};
			std::unique_ptr<WinPadRenderRegion> m_renderRegion;
			std::unique_ptr<WinPadRenderRegion> m_padRenderRegion;
			CloseHandler m_closeHandler;
			GameCloseHandler m_gameCloseHandler;
			MetricsHandler m_metricsHandler;
			PadCloseHandler m_padCloseHandler;
			InputHandler m_inputHandler;
			WinInputBinding* m_rawInputBinding{};
			std::uint64_t m_textInputSequence{};
			std::string m_textPreedit;
			bool m_textInputUpdating{};
			bool m_padMetricsEnabled{};
			WINDOWPLACEMENT m_windowPlacement{sizeof(m_windowPlacement)};
			LONG m_windowStyle{};
			bool m_fullscreen{};
		};
	} // namespace

	std::unique_ptr<INativeWindowHost> CreateNativeWindowHost()
	{
		return std::make_unique<WinWindowHost>();
	}
} // namespace WebFrontend

#endif
