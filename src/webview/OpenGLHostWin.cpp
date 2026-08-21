#include "Common/precompiled.h"

#include "webview/OpenGLHost.h"

#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"

#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace WebFrontend
{
	namespace
	{
		class WglOpenGLHost final : public INativeOpenGLHost
		{
		public:
			explicit WglOpenGLHost(Host::NativeWindowHandle handle)
				: m_window(static_cast<HWND>(handle.surface))
			{
				try
				{
					if (handle.backend != Host::NativeWindowBackend::Windows || !m_window)
						throw std::runtime_error("WGL received an invalid native render window");
					m_device = GetDC(m_window);
					if (!m_device)
						throw std::runtime_error("failed to acquire the OpenGL render window device context");
					m_descriptor.nSize = sizeof(m_descriptor);
					m_descriptor.nVersion = 1;
					m_descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
					m_descriptor.iPixelType = PFD_TYPE_RGBA;
					m_descriptor.cColorBits = 32;
					m_descriptor.cAlphaBits = 8;
					m_descriptor.cDepthBits = 16;
					m_descriptor.cStencilBits = 8;
					m_descriptor.iLayerType = PFD_MAIN_PLANE;
					m_pixelFormat = ChoosePixelFormat(m_device, &m_descriptor);
					if (!m_pixelFormat || !SetPixelFormat(m_device, m_pixelFormat, &m_descriptor))
						throw std::runtime_error("failed to set the OpenGL render window pixel format");
					m_context = wglCreateContext(m_device);
					if (!m_context || !wglMakeCurrent(m_device, m_context))
						throw std::runtime_error("failed to create the bootstrap WGL context");
					auto createContext = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
						wglGetProcAddress("wglCreateContextAttribsARB"));
					if (!createContext)
						(void)0;
					else
					{
						const int attributes[] = {
							WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
							WGL_CONTEXT_MINOR_VERSION_ARB, 1,
							WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
							0,
						};
						auto modernContext = createContext(m_device, nullptr, attributes);
						(void)wglMakeCurrent(nullptr, nullptr);
						if (!modernContext)
							throw std::runtime_error("failed to create the OpenGL 4.1 WGL context");
						(void)wglDeleteContext(m_context);
						m_context = modernContext;
					}
					(void)wglMakeCurrent(nullptr, nullptr);
					SetOpenGLCanvasCallbacks(this);
					m_callbacksInstalled = true;
				}
				catch (...)
				{
					Cleanup();
					throw;
				}
			}

			~WglOpenGLHost() override { Cleanup(); }
			bool HasPadViewOpen() const override
			{
				return m_padActive.load(std::memory_order_acquire);
			}

			void AttachPad(Host::NativeWindowHandle handle) override
			{
				if (m_padDevice || HasPadViewOpen())
					throw std::logic_error("the WGL GamePad surface is already attached");
				if (handle.backend != Host::NativeWindowBackend::Windows || !handle.surface)
					throw std::runtime_error("WGL received an invalid GamePad render window");
				m_padWindow = static_cast<HWND>(handle.surface);
				m_padDevice = GetDC(m_padWindow);
				if (!m_padDevice || !SetPixelFormat(m_padDevice, m_pixelFormat, &m_descriptor))
				{
					DetachPad();
					throw std::runtime_error("failed to set the GamePad WGL pixel format");
				}
			}

			void ActivatePad() override
			{
				if (!m_padDevice)
					throw std::logic_error("the WGL GamePad surface is not attached");
				m_padActive.store(true, std::memory_order_release);
			}

			void DeactivatePad() override
			{
				m_padActive.store(false, std::memory_order_release);
				(void)MakeCurrent(false);
			}

			void DetachPad() override
			{
				DeactivatePad();
				if (!m_padDevice)
					return;
				(void)wglMakeCurrent(m_device, m_context);
				(void)ReleaseDC(m_padWindow, m_padDevice);
				m_padDevice = nullptr;
				m_padWindow = nullptr;
			}

			bool MakeCurrent(bool padView) override
			{
				if (padView && !HasPadViewOpen())
					return false;
				const auto device = padView ? m_padDevice : m_device;
				return device && wglMakeCurrent(device, m_context) == TRUE;
			}

			void SwapBuffers(bool swapTV, bool swapDRC) override
			{
				if (swapTV)
				{
					(void)MakeCurrent(false);
					(void)::SwapBuffers(m_device);
				}
				if (swapDRC && HasPadViewOpen())
				{
					(void)MakeCurrent(true);
					(void)::SwapBuffers(m_padDevice);
				}
				(void)MakeCurrent(false);
			}

		private:
			void Cleanup() noexcept
			{
				if (std::exchange(m_cleanedUp, true))
					return;
				if (m_callbacksInstalled)
					ClearOpenGLCanvasCallbacks();
				DetachPad();
				(void)wglMakeCurrent(nullptr, nullptr);
				if (m_context)
					(void)wglDeleteContext(m_context);
				if (m_device && m_window)
					(void)ReleaseDC(m_window, m_device);
			}

			HWND m_window{};
			HDC m_device{};
			HGLRC m_context{};
			HWND m_padWindow{};
			HDC m_padDevice{};
			PIXELFORMATDESCRIPTOR m_descriptor{};
			int m_pixelFormat{};
			std::atomic_bool m_padActive{};
			bool m_callbacksInstalled{};
			bool m_cleanedUp{};
		};
	}

	std::unique_ptr<INativeOpenGLHost> CreateNativeOpenGLHost(
		Host::NativeWindowHandle surface,
		std::shared_ptr<Host::IWindowMetrics>)
	{
		return std::make_unique<WglOpenGLHost>(surface);
	}
}
