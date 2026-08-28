#include "webview/ToolWindowSupport.h"

#if defined(_WIN32)

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif
#include <windows.h>
#include <shobjidl.h>

namespace WebFrontend
{
	namespace
	{
		constexpr wchar_t ToolWindowClass[] = L"CemuExtendWebToolWindow";

		std::wstring Wide(std::string_view text)
		{
			if (text.empty())
				return {};
			const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
				text.data(), static_cast<int>(text.size()), nullptr, 0);
			if (length <= 0)
				throw std::invalid_argument("tool window title must be valid UTF-8");
			std::wstring result(static_cast<std::size_t>(length), L'\0');
			if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
					static_cast<int>(text.size()), result.data(), length) != length)
				throw std::runtime_error("failed to convert the tool window title");
			return result;
		}

		class WinToolWindowSupport final : public IToolWindowSupport
		{
		  public:
			WinToolWindowSupport(HWND parent, bool modal,
								 std::function<void()> closeHandler)
				: m_parent(parent), m_modal(modal), m_closeHandler(std::move(closeHandler))
			{
				if (!IsWindow(m_parent))
					throw std::invalid_argument("parent must be a valid HWND");
				RegisterWindowClass();
				m_window = CreateWindowExW(WS_EX_APPWINDOW, ToolWindowClass, L"",
										   WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
										   m_parent, nullptr, GetModuleHandleW(nullptr), this);
				if (!m_window)
					throw std::runtime_error("failed to create the native tool window");
			}

			~WinToolWindowSupport() override
			{
				m_closeHandler = {};
				if (IsWindow(m_window))
					DestroyWindow(m_window);
				RestoreParent();
			}

			void* GetWindow() const override
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
					throw std::logic_error("browser widget is not owned by the native tool window");
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
			void SetSize(std::int32_t width, std::int32_t height) override
			{
				if (!m_window || !IsWindow(m_window))
					return;
				RECT frame{0, 0,
						   static_cast<LONG>(std::max<std::int32_t>(1, width)),
						   static_cast<LONG>(std::max<std::int32_t>(1, height))};
				const auto style = static_cast<DWORD>(GetWindowLongPtrW(m_window, GWL_STYLE));
				const auto extendedStyle = static_cast<DWORD>(
					GetWindowLongPtrW(m_window, GWL_EXSTYLE));
				if (!AdjustWindowRectExForDpi(&frame, style, GetMenu(m_window) != nullptr,
						extendedStyle, GetDpiForWindow(m_window)))
					AdjustWindowRectEx(&frame, style, GetMenu(m_window) != nullptr, extendedStyle);
				SetWindowPos(m_window, nullptr, 0, 0, frame.right - frame.left,
						 frame.bottom - frame.top,
						 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
				ResizeBrowser();
			}
			void SetTitle(std::string_view title) override
			{
				if (!m_window || !IsWindow(m_window))
					return;
				const auto wide = Wide(title);
				if (!SetWindowTextW(m_window, wide.c_str()))
					throw std::runtime_error("failed to set the native tool window title");
			}

			void Show() override
			{
				if (m_modal && !m_parentDisabled && IsWindow(m_parent))
				{
					EnableWindow(m_parent, FALSE);
					m_parentDisabled = true;
				}
				ShowWindow(m_window, SW_SHOW);
				UpdateWindow(m_window);
				ResizeBrowser();
				Focus();
			}

			void Focus() override
			{
				if (!IsWindow(m_window))
					return;
				ShowWindow(m_window, SW_RESTORE);
				SetForegroundWindow(m_window);
				SetActiveWindow(m_window);
				FocusBrowser();
			}

			std::optional<std::filesystem::path> PickDirectory(std::string_view) override
			{
				IFileOpenDialog* dialog{};
				const auto created = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
													  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
				if (FAILED(created))
					throw std::runtime_error("failed to create the folder picker");
				auto releaseDialog = std::unique_ptr<IFileOpenDialog, decltype([](IFileOpenDialog* value) {
														 if (value)
															 value->Release();
													 })>(dialog);
				DWORD options{};
				if (FAILED(dialog->GetOptions(&options)) || FAILED(dialog->SetOptions(
																options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST)))
					throw std::runtime_error("failed to configure the folder picker");
				const auto shown = dialog->Show(m_window);
				if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
					return std::nullopt;
				if (FAILED(shown))
					throw std::runtime_error("folder picker failed");
				IShellItem* item{};
				if (FAILED(dialog->GetResult(&item)) || !item)
					throw std::runtime_error("folder picker returned no selection");
				auto releaseItem = std::unique_ptr<IShellItem, decltype([](IShellItem* value) {
													   if (value)
														   value->Release();
												   })>(item);
				PWSTR path{};
				if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path)
					throw std::runtime_error("selected folder has no filesystem path");
				std::filesystem::path result(path);
				CoTaskMemFree(path);
				return result;
			}

		  private:
			static void RegisterWindowClass()
			{
				static std::once_flag flag;
				std::call_once(flag, [] {
					WNDCLASSEXW descriptor{};
					descriptor.cbSize = sizeof(descriptor);
					descriptor.hInstance = GetModuleHandleW(nullptr);
					descriptor.lpfnWndProc = &WindowProc;
					descriptor.lpszClassName = ToolWindowClass;
					descriptor.hCursor = LoadCursorW(nullptr, IDC_ARROW);
					descriptor.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
					if (!RegisterClassExW(&descriptor) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
						throw std::runtime_error("failed to register the native tool window class");
				});
			}

			static LRESULT CALLBACK WindowProc(HWND window, UINT message,
											   WPARAM wparam, LPARAM lparam)
			{
				WinToolWindowSupport* self{};
				if (message == WM_NCCREATE)
				{
					const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
					self = static_cast<WinToolWindowSupport*>(create->lpCreateParams);
					self->m_window = window;
					SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
				}
				else
					self = reinterpret_cast<WinToolWindowSupport*>(
						GetWindowLongPtrW(window, GWLP_USERDATA));
				if (!self)
					return DefWindowProcW(window, message, wparam, lparam);
				switch (message)
				{
				case WM_CLOSE:
					if (self->m_closeHandler)
						self->m_closeHandler();
					return 0;
				case WM_SIZE:
					if (!self->m_browserWindow)
						self->m_browserWindow = GetWindow(window, GW_CHILD);
					self->ResizeBrowser();
					return 0;
				case WM_DPICHANGED:
				{
					const auto* suggested = reinterpret_cast<const RECT*>(lparam);
					SetWindowPos(window, nullptr, suggested->left, suggested->top,
							 suggested->right - suggested->left,
							 suggested->bottom - suggested->top,
							 SWP_NOACTIVATE | SWP_NOZORDER);
					self->ResizeBrowser();
					return 0;
				}
				case WM_NCDESTROY:
					SetWindowLongPtrW(window, GWLP_USERDATA, 0);
					self->m_browserWindow = nullptr;
					self->m_window = nullptr;
					self->RestoreParent();
					break;
				default:
					break;
				}
				return DefWindowProcW(window, message, wparam, lparam);
			}

			void RestoreParent()
			{
				if (std::exchange(m_parentDisabled, false) && IsWindow(m_parent))
				{
					EnableWindow(m_parent, TRUE);
					SetForegroundWindow(m_parent);
				}
			}

			HWND m_window{};
			HWND m_parent{};
			HWND m_browserWindow{};
			bool m_modal{};
			bool m_parentDisabled{};
			std::function<void()> m_closeHandler;
		};
	} // namespace

	std::unique_ptr<IToolWindowSupport> CreateToolWindowSupport(
		void* parent, bool modal, std::function<void()> closeHandler)
	{
		return std::make_unique<WinToolWindowSupport>(static_cast<HWND>(parent),
													  modal, std::move(closeHandler));
	}
} // namespace WebFrontend

#endif
