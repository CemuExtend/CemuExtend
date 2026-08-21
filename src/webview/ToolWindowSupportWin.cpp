#include "webview/ToolWindowSupport.h"

#if defined(_WIN32)

#include <mutex>
#include <stdexcept>
#include <utility>
#include <windows.h>

namespace WebFrontend
{
	namespace
	{
		constexpr wchar_t ToolWindowClass[] = L"CemuExtendWebToolWindow";

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
				if (IsWindow(m_window)) DestroyWindow(m_window);
				RestoreParent();
			}

			void* GetWindow() const override { return m_window; }

			void Show() override
			{
				if (m_modal && !m_parentDisabled && IsWindow(m_parent))
				{
					EnableWindow(m_parent, FALSE);
					m_parentDisabled = true;
				}
				ShowWindow(m_window, SW_SHOW);
				UpdateWindow(m_window);
				Focus();
			}

			void Focus() override
			{
				if (!IsWindow(m_window)) return;
				ShowWindow(m_window, SW_RESTORE);
				SetForegroundWindow(m_window);
				SetActiveWindow(m_window);
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
				if (!self) return DefWindowProcW(window, message, wparam, lparam);
				switch (message)
				{
				case WM_CLOSE:
					if (self->m_closeHandler) self->m_closeHandler();
					return 0;
				case WM_SIZE:
					if (const auto child = GetWindow(window, GW_CHILD))
						MoveWindow(child, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
					return 0;
				case WM_NCDESTROY:
					SetWindowLongPtrW(window, GWLP_USERDATA, 0);
					self->m_window = nullptr;
					self->RestoreParent();
					break;
				default: break;
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
			bool m_modal{};
			bool m_parentDisabled{};
			std::function<void()> m_closeHandler;
		};
	}

	std::unique_ptr<IToolWindowSupport> CreateToolWindowSupport(
		void* parent, bool modal, std::function<void()> closeHandler)
	{
		return std::make_unique<WinToolWindowSupport>(static_cast<HWND>(parent),
			modal, std::move(closeHandler));
	}
}

#endif
