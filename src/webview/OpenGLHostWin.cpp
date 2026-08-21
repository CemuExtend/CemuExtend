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
		class WglOpenGLHost final : public OpenGLCanvasCallbacks
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
					PIXELFORMATDESCRIPTOR descriptor{};
					descriptor.nSize = sizeof(descriptor);
					descriptor.nVersion = 1;
					descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
					descriptor.iPixelType = PFD_TYPE_RGBA;
					descriptor.cColorBits = 32;
					descriptor.cAlphaBits = 8;
					descriptor.cDepthBits = 16;
					descriptor.cStencilBits = 8;
					descriptor.iLayerType = PFD_MAIN_PLANE;
					const auto format = ChoosePixelFormat(m_device, &descriptor);
					if (!format || !SetPixelFormat(m_device, format, &descriptor))
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

			bool MakeCurrent(bool padView) override
			{
				return !padView && wglMakeCurrent(m_device, m_context) == TRUE;
			}

			void SwapBuffers(bool swapTV, bool) override
			{
				if (swapTV)
					(void)::SwapBuffers(m_device);
			}

		private:
			void Cleanup() noexcept
			{
				if (std::exchange(m_cleanedUp, true))
					return;
				if (m_callbacksInstalled)
					ClearOpenGLCanvasCallbacks();
				(void)wglMakeCurrent(nullptr, nullptr);
				if (m_context)
					(void)wglDeleteContext(m_context);
				if (m_device && m_window)
					(void)ReleaseDC(m_window, m_device);
			}

			HWND m_window{};
			HDC m_device{};
			HGLRC m_context{};
			bool m_callbacksInstalled{};
			bool m_cleanedUp{};
		};
	}

	std::unique_ptr<OpenGLCanvasCallbacks> CreateNativeOpenGLHost(
		Host::NativeWindowHandle surface,
		std::shared_ptr<Host::IWindowMetrics>)
	{
		return std::make_unique<WglOpenGLHost>(surface);
	}
}
