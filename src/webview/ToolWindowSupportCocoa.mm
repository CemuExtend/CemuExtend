#include "webview/ToolWindowSupport.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>

#include <stdexcept>
#include <utility>

namespace WebFrontend
{
	struct CocoaToolCloseContext
	{
		std::function<void()> closeHandler;
	};
}

@interface CemuToolWindowDelegate : NSObject <NSWindowDelegate>
{
@public
	WebFrontend::CocoaToolCloseContext* closeContext;
}
@end

@implementation CemuToolWindowDelegate
- (BOOL)windowShouldClose:(id)sender
{
	(void)sender;
	if (closeContext && closeContext->closeHandler) closeContext->closeHandler();
	return NO;
}
@end

namespace WebFrontend
{
	namespace
	{
		class CocoaToolWindowSupport final : public IToolWindowSupport
		{
		public:
			CocoaToolWindowSupport(NSWindow* parent, bool modal,
				std::function<void()> closeHandler)
				: m_parent(parent), m_modal(modal)
			{
				if (!m_parent)
					throw std::invalid_argument("parent must be an NSWindow");
				m_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
					styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
						NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
					backing:NSBackingStoreBuffered defer:NO];
				if (!m_window)
					throw std::runtime_error("failed to create the native tool window");
				[m_window setReleasedWhenClosed:NO];
				m_context.closeHandler = std::move(closeHandler);
				m_delegate = [[CemuToolWindowDelegate alloc] init];
				m_delegate->closeContext = &m_context;
				[m_window setDelegate:m_delegate];
			}

			~CocoaToolWindowSupport() override
			{
				m_context.closeHandler = {};
				if (m_modal && [m_parent attachedSheet] == m_window)
					[m_parent endSheet:m_window];
				else if (!m_modal)
					[m_parent removeChildWindow:m_window];
				[m_window setDelegate:nil];
				[m_window orderOut:nil];
				[m_window close];
				[m_delegate release];
				[m_window release];
			}

			void* GetWindow() const override { return m_window; }

			void Show() override
			{
				if (m_modal)
					[m_parent beginSheet:m_window completionHandler:nil];
				else
					[m_parent addChildWindow:m_window ordered:NSWindowAbove];
				Focus();
			}

			void Focus() override
			{
				[m_window makeKeyAndOrderFront:nil];
				[NSApp activateIgnoringOtherApps:YES];
			}

		private:
			NSWindow* m_window{};
			NSWindow* m_parent{};
			bool m_modal{};
			CocoaToolCloseContext m_context;
			CemuToolWindowDelegate* m_delegate{};
		};
	}

	std::unique_ptr<IToolWindowSupport> CreateToolWindowSupport(
		void* parent, bool modal, std::function<void()> closeHandler)
	{
		return std::make_unique<CocoaToolWindowSupport>(static_cast<NSWindow*>(parent),
			modal, std::move(closeHandler));
	}
}

#endif
