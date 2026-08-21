#include "Common/precompiled.h"

#include "webview/OpenGLHost.h"

#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "config/CemuConfig.h"

#import <Cocoa/Cocoa.h>

#include <stdexcept>

namespace WebFrontend
{
	namespace
	{
		class CocoaOpenGLHost final : public OpenGLCanvasCallbacks
		{
		public:
			explicit CocoaOpenGLHost(Host::NativeWindowHandle handle)
			{
				try
				{
					if (handle.backend != Host::NativeWindowBackend::Cocoa || !handle.surface)
						throw std::runtime_error("Cocoa OpenGL received an invalid native render view");
					NSOpenGLPixelFormatAttribute attributes[] = {
						NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
						NSOpenGLPFAAccelerated,
						NSOpenGLPFADoubleBuffer,
						NSOpenGLPFAColorSize, 32,
						NSOpenGLPFAAlphaSize, 8,
						NSOpenGLPFADepthSize, 16,
						NSOpenGLPFAStencilSize, 8,
						0,
					};
					m_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
					if (!m_format)
						throw std::runtime_error("failed to select a Cocoa OpenGL pixel format");
					m_context = [[NSOpenGLContext alloc] initWithFormat:m_format shareContext:nil];
					if (!m_context)
						throw std::runtime_error("failed to create the Cocoa OpenGL 4.1 context");
					[m_context setView:reinterpret_cast<NSView*>(handle.surface)];
					SetOpenGLCanvasCallbacks(this);
					m_callbacksInstalled = true;
				}
				catch (...)
				{
					Cleanup();
					throw;
				}
			}

			~CocoaOpenGLHost() override { Cleanup(); }

			bool MakeCurrent(bool padView) override
			{
				if (padView || !m_context)
					return false;
				[m_context makeCurrentContext];
				return [NSOpenGLContext currentContext] == m_context;
			}

			void SwapBuffers(bool swapTV, bool) override
			{
				if (!swapTV || !m_context)
					return;
				const GLint requestedVsync = GetConfig().vsync.GetValue() > 0 ? 1 : 0;
				if (requestedVsync != m_vsync)
				{
					[m_context setValues:&requestedVsync forParameter:NSOpenGLCPSwapInterval];
					m_vsync = requestedVsync;
				}
				[m_context update];
				[m_context flushBuffer];
			}

		private:
			void Cleanup() noexcept
			{
				if (std::exchange(m_cleanedUp, true))
					return;
				if (m_callbacksInstalled)
					ClearOpenGLCanvasCallbacks();
				if ([NSOpenGLContext currentContext] == m_context)
					[NSOpenGLContext clearCurrentContext];
				[m_context clearDrawable];
				[m_context release];
				[m_format release];
				m_context = nil;
				m_format = nil;
			}

			NSOpenGLPixelFormat* m_format{};
			NSOpenGLContext* m_context{};
			GLint m_vsync{-1};
			bool m_callbacksInstalled{};
			bool m_cleanedUp{};
		};
	}

	std::unique_ptr<OpenGLCanvasCallbacks> CreateNativeOpenGLHost(
		Host::NativeWindowHandle surface,
		std::shared_ptr<Host::IWindowMetrics>)
	{
		return std::make_unique<CocoaOpenGLHost>(surface);
	}
}
