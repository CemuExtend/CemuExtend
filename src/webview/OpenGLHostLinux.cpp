#include "Common/precompiled.h"

#include "webview/OpenGLHost.h"

#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "config/CemuConfig.h"

#include <dlfcn.h>
#include <stdexcept>

#include <X11/Xlib.h>

#ifdef HAS_WAYLAND
#include <wayland-client.h>
struct wl_egl_window;
#endif

namespace WebFrontend
{
	namespace
	{
		template<typename Function>
		Function Load(void* library, const char* name)
		{
			auto function = reinterpret_cast<Function>(dlsym(library, name));
			if (!function)
				throw std::runtime_error(std::string("required native graphics symbol is missing: ") + name);
			return function;
		}

		class EglOpenGLHost final : public INativeOpenGLHost
		{
		public:
			EglOpenGLHost(Host::NativeWindowHandle handle,
				std::shared_ptr<Host::IWindowMetrics> windowMetrics)
				: m_handle(handle), m_windowMetrics(std::move(windowMetrics))
			{
				try
				{
				m_eglLibrary = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
				if (!m_eglLibrary)
					m_eglLibrary = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
				if (!m_eglLibrary)
					throw std::runtime_error("failed to load libEGL");

				m_getDisplay = Load<PFNEGLGETDISPLAYPROC>(m_eglLibrary, "eglGetDisplay");
				m_initialize = Load<PFNEGLINITIALIZEPROC>(m_eglLibrary, "eglInitialize");
				m_bindApi = Load<PFNEGLBINDAPIPROC>(m_eglLibrary, "eglBindAPI");
				m_getConfigs = Load<PFNEGLGETCONFIGSPROC>(m_eglLibrary, "eglGetConfigs");
				m_getConfigAttrib = Load<PFNEGLGETCONFIGATTRIBPROC>(m_eglLibrary, "eglGetConfigAttrib");
				m_createWindowSurface = Load<PFNEGLCREATEWINDOWSURFACEPROC>(
					m_eglLibrary, "eglCreateWindowSurface");
				m_createContext = Load<PFNEGLCREATECONTEXTPROC>(m_eglLibrary, "eglCreateContext");
				m_makeCurrent = Load<PFNEGLMAKECURRENTPROC>(m_eglLibrary, "eglMakeCurrent");
				m_swapBuffers = Load<PFNEGLSWAPBUFFERSPROC>(m_eglLibrary, "eglSwapBuffers");
				m_swapInterval = Load<PFNEGLSWAPINTERVALPROC>(m_eglLibrary, "eglSwapInterval");
				m_destroyContext = Load<PFNEGLDESTROYCONTEXTPROC>(m_eglLibrary, "eglDestroyContext");
				m_destroySurface = Load<PFNEGLDESTROYSURFACEPROC>(m_eglLibrary, "eglDestroySurface");
				m_terminate = Load<PFNEGLTERMINATEPROC>(m_eglLibrary, "eglTerminate");

				m_display = m_getDisplay(static_cast<EGLNativeDisplayType>(handle.display));
				if (m_display == EGL_NO_DISPLAY || !m_initialize(m_display, nullptr, nullptr))
					throw std::runtime_error("failed to initialize EGL for the native display");
				if (!m_bindApi(EGL_OPENGL_API))
					throw std::runtime_error("the EGL implementation does not support desktop OpenGL");

				m_config = SelectConfig();
				EGLNativeWindowType nativeWindow{};
				if (handle.backend == Host::NativeWindowBackend::X11)
					nativeWindow = static_cast<EGLNativeWindowType>(
						reinterpret_cast<std::uintptr_t>(handle.surface));
#ifdef HAS_WAYLAND
				else if (handle.backend == Host::NativeWindowBackend::Wayland)
				{
					m_waylandEglLibrary = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
					if (!m_waylandEglLibrary)
						throw std::runtime_error("failed to load libwayland-egl");
					m_waylandCreate = Load<WaylandCreate>(m_waylandEglLibrary, "wl_egl_window_create");
					m_waylandDestroy = Load<WaylandDestroy>(m_waylandEglLibrary, "wl_egl_window_destroy");
					m_waylandResize = Load<WaylandResize>(m_waylandEglLibrary, "wl_egl_window_resize");
					const auto metrics = m_windowMetrics->GetWindowMetrics();
					m_waylandWindow = m_waylandCreate(static_cast<wl_surface*>(handle.surface),
						std::max(1, metrics.physicalWidth), std::max(1, metrics.physicalHeight));
					if (!m_waylandWindow)
						throw std::runtime_error("failed to create the Wayland EGL window");
					nativeWindow = reinterpret_cast<EGLNativeWindowType>(m_waylandWindow);
				}
#endif
				else
					throw std::runtime_error("EGL received an unsupported native window backend");

				m_surface = m_createWindowSurface(m_display, m_config, nativeWindow, nullptr);
				if (m_surface == EGL_NO_SURFACE)
					throw std::runtime_error("failed to create the EGL window surface");
				const EGLint contextAttributes[] = {
					EGL_CONTEXT_MAJOR_VERSION, 4,
					EGL_CONTEXT_MINOR_VERSION, 1,
					EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
					EGL_NONE,
				};
				m_context = m_createContext(m_display, m_config, EGL_NO_CONTEXT, contextAttributes);
				if (m_context == EGL_NO_CONTEXT)
					throw std::runtime_error("failed to create the EGL OpenGL context");
				SetOpenGLCanvasCallbacks(this);
				m_callbacksInstalled = true;
				}
				catch (...)
				{
					Cleanup();
					throw;
				}
			}

			~EglOpenGLHost() override { Cleanup(); }

			bool HasPadViewOpen() const override
			{
				return m_padActive.load(std::memory_order_acquire);
			}

			void AttachPad(Host::NativeWindowHandle handle) override
			{
				if (m_padSurface != EGL_NO_SURFACE || HasPadViewOpen())
					throw std::logic_error("the OpenGL GamePad surface is already attached");
				if (handle.backend != m_handle.backend || handle.display != m_handle.display)
					throw std::runtime_error("the OpenGL GamePad surface uses a different native display");
				EGLNativeWindowType nativeWindow{};
				if (handle.backend == Host::NativeWindowBackend::X11)
					nativeWindow = static_cast<EGLNativeWindowType>(
						reinterpret_cast<std::uintptr_t>(handle.surface));
#ifdef HAS_WAYLAND
				else if (handle.backend == Host::NativeWindowBackend::Wayland)
				{
					const auto metrics = m_windowMetrics->GetWindowMetrics();
					m_padWaylandWindow = m_waylandCreate(static_cast<wl_surface*>(handle.surface),
						std::max(1, metrics.physicalPadWidth),
						std::max(1, metrics.physicalPadHeight));
					if (!m_padWaylandWindow)
						throw std::runtime_error("failed to create the GamePad Wayland EGL window");
					nativeWindow = reinterpret_cast<EGLNativeWindowType>(m_padWaylandWindow);
				}
#endif
				else
					throw std::runtime_error("OpenGL GamePad received an unsupported native backend");
				m_padSurface = m_createWindowSurface(m_display, m_config, nativeWindow, nullptr);
				if (m_padSurface == EGL_NO_SURFACE)
				{
#ifdef HAS_WAYLAND
					if (m_padWaylandWindow)
					{
						m_waylandDestroy(m_padWaylandWindow);
						m_padWaylandWindow = nullptr;
					}
#endif
					throw std::runtime_error("failed to create the GamePad EGL window surface");
				}
			}

			void ActivatePad() override
			{
				if (m_padSurface == EGL_NO_SURFACE)
					throw std::logic_error("the EGL GamePad surface is not attached");
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
				if (m_padSurface == EGL_NO_SURFACE)
					return;
				(void)m_makeCurrent(m_display, m_surface, m_surface, m_context);
				(void)m_destroySurface(m_display, m_padSurface);
				m_padSurface = EGL_NO_SURFACE;
#ifdef HAS_WAYLAND
				if (m_padWaylandWindow)
				{
					m_waylandDestroy(m_padWaylandWindow);
					m_padWaylandWindow = nullptr;
				}
#endif
			}

			bool MakeCurrent(bool padView) override
			{
				if (padView && !HasPadViewOpen())
					return false;
				const auto surface = padView ? m_padSurface : m_surface;
				return surface != EGL_NO_SURFACE &&
					m_makeCurrent(m_display, surface, surface, m_context) == EGL_TRUE;
			}

			void SwapBuffers(bool swapTV, bool swapDRC) override
			{
				if (swapTV)
					Swap(false);
				if (swapDRC && HasPadViewOpen())
					Swap(true);
				(void)MakeCurrent(false);
			}

		private:
			void Swap(bool padView)
			{
				if (!MakeCurrent(padView))
					return;
#ifdef HAS_WAYLAND
				auto* waylandWindow = padView ? m_padWaylandWindow : m_waylandWindow;
				if (waylandWindow)
				{
					const auto metrics = m_windowMetrics->GetWindowMetrics();
					const auto width = std::max(1,
						padView ? metrics.physicalPadWidth : metrics.physicalWidth);
					const auto height = std::max(1,
						padView ? metrics.physicalPadHeight : metrics.physicalHeight);
					auto& previousWidth = padView ? m_padWaylandWidth : m_waylandWidth;
					auto& previousHeight = padView ? m_padWaylandHeight : m_waylandHeight;
					if (width != previousWidth || height != previousHeight)
					{
						m_waylandResize(waylandWindow, width, height, 0, 0);
						previousWidth = width;
						previousHeight = height;
					}
				}
#endif
				const int requestedVsync = GetConfig().vsync.GetValue() > 0 ? 1 : 0;
				if (requestedVsync != m_vsync)
				{
					(void)m_swapInterval(m_display, requestedVsync);
					m_vsync = requestedVsync;
				}
				(void)m_swapBuffers(m_display, padView ? m_padSurface : m_surface);
			}

			void Cleanup() noexcept
			{
				if (std::exchange(m_cleanedUp, true))
					return;
				if (m_callbacksInstalled)
					ClearOpenGLCanvasCallbacks();
				DetachPad();
				if (m_display != EGL_NO_DISPLAY && m_makeCurrent)
				{
					(void)m_makeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
					if (m_context != EGL_NO_CONTEXT && m_destroyContext)
						(void)m_destroyContext(m_display, m_context);
					if (m_surface != EGL_NO_SURFACE && m_destroySurface)
						(void)m_destroySurface(m_display, m_surface);
					if (m_terminate)
						(void)m_terminate(m_display);
				}
#ifdef HAS_WAYLAND
				if (m_waylandWindow && m_waylandDestroy)
					m_waylandDestroy(m_waylandWindow);
				if (m_waylandEglLibrary)
					dlclose(m_waylandEglLibrary);
#endif
				if (m_eglLibrary)
					dlclose(m_eglLibrary);
			}

			EGLConfig SelectConfig()
			{
				EGLint count{};
				if (!m_getConfigs(m_display, nullptr, 0, &count) || count <= 0)
					throw std::runtime_error("EGL did not report any framebuffer configurations");
				std::vector<EGLConfig> configs(static_cast<std::size_t>(count));
				if (!m_getConfigs(m_display, configs.data(), count, &count))
					throw std::runtime_error("failed to enumerate EGL framebuffer configurations");
				unsigned long x11Visual{};
				if (m_handle.backend == Host::NativeWindowBackend::X11)
				{
					XWindowAttributes attributes{};
					if (!XGetWindowAttributes(static_cast<Display*>(m_handle.display),
						static_cast<Window>(reinterpret_cast<std::uintptr_t>(m_handle.surface)),
						&attributes))
						throw std::runtime_error("failed to inspect the X11 render window visual");
					x11Visual = XVisualIDFromVisual(attributes.visual);
				}
				for (const auto config : configs)
				{
					EGLint surfaceType{}, renderable{}, red{}, green{}, blue{}, depth{}, stencil{};
					EGLint visual{};
					m_getConfigAttrib(m_display, config, EGL_SURFACE_TYPE, &surfaceType);
					m_getConfigAttrib(m_display, config, EGL_RENDERABLE_TYPE, &renderable);
					m_getConfigAttrib(m_display, config, EGL_RED_SIZE, &red);
					m_getConfigAttrib(m_display, config, EGL_GREEN_SIZE, &green);
					m_getConfigAttrib(m_display, config, EGL_BLUE_SIZE, &blue);
					m_getConfigAttrib(m_display, config, EGL_DEPTH_SIZE, &depth);
					m_getConfigAttrib(m_display, config, EGL_STENCIL_SIZE, &stencil);
					m_getConfigAttrib(m_display, config, EGL_NATIVE_VISUAL_ID, &visual);
					if ((surfaceType & EGL_WINDOW_BIT) && (renderable & EGL_OPENGL_BIT) &&
						red >= 8 && green >= 8 && blue >= 8 && depth >= 16 && stencil >= 8 &&
						(!x11Visual || static_cast<unsigned long>(visual) == x11Visual))
						return config;
				}
				throw std::runtime_error("no EGL framebuffer configuration matches the native render window");
			}

#ifdef HAS_WAYLAND
			using WaylandCreate = wl_egl_window*(*)(wl_surface*, int, int);
			using WaylandDestroy = void(*)(wl_egl_window*);
			using WaylandResize = void(*)(wl_egl_window*, int, int, int, int);
#endif
			Host::NativeWindowHandle m_handle;
			std::shared_ptr<Host::IWindowMetrics> m_windowMetrics;
			void* m_eglLibrary{};
			EGLDisplay m_display{EGL_NO_DISPLAY};
			EGLSurface m_surface{EGL_NO_SURFACE};
			EGLSurface m_padSurface{EGL_NO_SURFACE};
			EGLContext m_context{EGL_NO_CONTEXT};
			EGLConfig m_config{};
			std::atomic_bool m_padActive{};
			PFNEGLGETDISPLAYPROC m_getDisplay{};
			PFNEGLINITIALIZEPROC m_initialize{};
			PFNEGLBINDAPIPROC m_bindApi{};
			PFNEGLGETCONFIGSPROC m_getConfigs{};
			PFNEGLGETCONFIGATTRIBPROC m_getConfigAttrib{};
			PFNEGLCREATEWINDOWSURFACEPROC m_createWindowSurface{};
			PFNEGLCREATECONTEXTPROC m_createContext{};
			PFNEGLMAKECURRENTPROC m_makeCurrent{};
			PFNEGLSWAPBUFFERSPROC m_swapBuffers{};
			PFNEGLSWAPINTERVALPROC m_swapInterval{};
			PFNEGLDESTROYCONTEXTPROC m_destroyContext{};
			PFNEGLDESTROYSURFACEPROC m_destroySurface{};
			PFNEGLTERMINATEPROC m_terminate{};
			bool m_callbacksInstalled{};
			bool m_cleanedUp{};
			int m_vsync{-1};
#ifdef HAS_WAYLAND
			void* m_waylandEglLibrary{};
			wl_egl_window* m_waylandWindow{};
			wl_egl_window* m_padWaylandWindow{};
			WaylandCreate m_waylandCreate{};
			WaylandDestroy m_waylandDestroy{};
			WaylandResize m_waylandResize{};
			int m_waylandWidth{};
			int m_waylandHeight{};
			int m_padWaylandWidth{};
			int m_padWaylandHeight{};
#endif
		};
	}

	std::unique_ptr<INativeOpenGLHost> CreateNativeOpenGLHost(
		Host::NativeWindowHandle surface,
		std::shared_ptr<Host::IWindowMetrics> windowMetrics)
	{
		return std::make_unique<EglOpenGLHost>(surface, std::move(windowMetrics));
	}
}
