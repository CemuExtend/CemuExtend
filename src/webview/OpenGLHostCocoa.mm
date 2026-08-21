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
		class CocoaOpenGLHost final : public INativeOpenGLHost
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
					m_mainContext = [[NSOpenGLContext alloc] initWithFormat:m_format shareContext:nil];
					if (!m_mainContext)
						throw std::runtime_error("failed to create the Cocoa OpenGL 4.1 context");
					[m_mainContext setView:reinterpret_cast<NSView*>(handle.surface)];
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
			bool HasPadViewOpen() const override
			{
				return m_padActive.load(std::memory_order_acquire);
			}
			void AttachPad(Host::NativeWindowHandle handle) override
			{
				if (m_padContext || HasPadViewOpen())
					throw std::logic_error("the Cocoa OpenGL GamePad surface is already attached");
				if (handle.backend != Host::NativeWindowBackend::Cocoa || !handle.surface)
					throw std::runtime_error("Cocoa OpenGL received an invalid GamePad render view");
				m_padContext = [[NSOpenGLContext alloc] initWithFormat:m_format
					shareContext:m_mainContext];
				if (!m_padContext)
					throw std::runtime_error("failed to create the Cocoa GamePad OpenGL context");
				[m_padContext setView:reinterpret_cast<NSView*>(handle.surface)];
			}
			void ActivatePad() override
			{
				if (!m_padContext)
					throw std::logic_error("the Cocoa GamePad OpenGL context is not attached");
				m_padActive.store(true, std::memory_order_release);
			}
			void DeactivatePad() override
			{
				m_padActive.store(false, std::memory_order_release);
				(void)MakeCurrent(false);
			}
			void DetachPad() override
			{
				m_padActive.store(false, std::memory_order_release);
				if (!m_padContext)
					return;
				[m_padContext clearDrawable];
				[m_padContext release];
				m_padContext = nil;
			}

			bool MakeCurrent(bool padView) override
			{
				if (!m_mainContext || (padView && (!HasPadViewOpen() || !m_padContext)))
					return false;
				auto* context = padView ? m_padContext : m_mainContext;
				[context makeCurrentContext];
				return [NSOpenGLContext currentContext] == context;
			}

			void SwapBuffers(bool swapTV, bool swapDRC) override
			{
				if ((!swapTV && !swapDRC) || !m_mainContext)
					return;
				const GLint requestedVsync = GetConfig().vsync.GetValue() > 0 ? 1 : 0;
				if (swapTV && MakeCurrent(false))
				{
					if (requestedVsync != m_mainVsync)
					{
						[m_mainContext setValues:&requestedVsync forParameter:NSOpenGLCPSwapInterval];
						m_mainVsync = requestedVsync;
					}
					[m_mainContext update];
					[m_mainContext flushBuffer];
				}
				if (swapDRC && HasPadViewOpen() && MakeCurrent(true))
				{
					if (requestedVsync != m_padVsync)
					{
						[m_padContext setValues:&requestedVsync forParameter:NSOpenGLCPSwapInterval];
						m_padVsync = requestedVsync;
					}
					[m_padContext update];
					[m_padContext flushBuffer];
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
				if ([NSOpenGLContext currentContext] == m_mainContext)
					[NSOpenGLContext clearCurrentContext];
				[m_mainContext clearDrawable];
				[m_mainContext release];
				[m_format release];
				m_mainContext = nil;
				m_format = nil;
			}

			NSOpenGLPixelFormat* m_format{};
			NSOpenGLContext* m_mainContext{};
			NSOpenGLContext* m_padContext{};
			std::atomic_bool m_padActive{};
			GLint m_mainVsync{-1};
			GLint m_padVsync{-1};
			bool m_callbacksInstalled{};
			bool m_cleanedUp{};
		};
	}

	std::unique_ptr<INativeOpenGLHost> CreateNativeOpenGLHost(
		Host::NativeWindowHandle surface,
		std::shared_ptr<Host::IWindowMetrics>)
	{
		return std::make_unique<CocoaOpenGLHost>(surface);
	}
}
