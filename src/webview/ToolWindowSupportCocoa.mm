#include "webview/ToolWindowSupport.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>

#include <algorithm>
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
				m_browserContainer = [[NSView alloc] initWithFrame:[[m_window contentView] bounds]];
				[m_browserContainer setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
				[m_window setContentView:m_browserContainer];
			}

			~CocoaToolWindowSupport() override
			{
				m_context.closeHandler = {};
				if (m_modal && [m_parent attachedSheet] == m_window)
					[m_parent endSheet:m_window];
				else if (!m_modal)
					[m_parent removeChildWindow:m_window];
				DetachBrowser(reinterpret_cast<void*>(m_browserView));
				[m_window setDelegate:nil];
				[m_window orderOut:nil];
				[m_window close];
				[m_delegate release];
				[m_window release];
				[m_browserContainer release];
			}

			void* GetWindow() const override { return m_window; }
			void* GetBrowserParentWindow() const override
			{
				return reinterpret_cast<void*>(m_browserContainer);
			}
			Host::RenderRegionBounds GetBrowserBounds() const override
			{
				const auto bounds = m_browserContainer ? [m_browserContainer bounds] : NSZeroRect;
				return {0, 0, static_cast<std::int32_t>(bounds.size.width),
					static_cast<std::int32_t>(bounds.size.height)};
			}
			double GetBrowserDpiScale() const override
			{
				return m_window ? static_cast<double>([m_window backingScaleFactor]) : 1.0;
			}
			void AttachBrowser(void* widget) override
			{
				if (!widget || m_browserView)
					throw std::logic_error("CEF tool browser view cannot be attached");
				m_browserView = reinterpret_cast<NSView*>(widget);
				[m_browserView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
				ResizeBrowser();
				if ([m_browserView superview] != m_browserContainer)
					[m_browserContainer addSubview:m_browserView];
			}
			void ResizeBrowser() override
			{
				if (m_browserView && m_browserContainer)
					[m_browserView setFrame:[m_browserContainer bounds]];
			}
			void FocusBrowser() override
			{
				if (m_browserView)
					[m_window makeFirstResponder:m_browserView];
			}
			void DetachBrowser(void* widget) override
			{
				if (!m_browserView || widget != reinterpret_cast<void*>(m_browserView))
					return;
				[m_browserView removeFromSuperview];
				m_browserView = nil;
			}
			void SetSize(std::int32_t width, std::int32_t height) override
			{
				[m_window setContentSize:NSMakeSize(std::max<std::int32_t>(1, width),
					std::max<std::int32_t>(1, height))];
				ResizeBrowser();
			}
			void SetTitle(std::string_view title) override
			{
				NSString* value = [[NSString alloc] initWithBytes:title.data()
					length:title.size() encoding:NSUTF8StringEncoding];
				[m_window setTitle:value ?: @""];
				[value release];
			}

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
				FocusBrowser();
				[NSApp activateIgnoringOtherApps:YES];
			}

			std::optional<std::filesystem::path> PickDirectory(std::string_view title) override
			{
				NSOpenPanel* panel = [NSOpenPanel openPanel];
				[panel setCanChooseDirectories:YES];
				[panel setCanChooseFiles:NO];
				[panel setAllowsMultipleSelection:NO];
				NSString* prompt = [[NSString alloc] initWithBytes:title.data()
					length:title.size() encoding:NSUTF8StringEncoding];
				[panel setTitle:prompt];
				[prompt release];
				if ([panel runModal] != NSModalResponseOK) return std::nullopt;
				NSURL* selected = [[panel URLs] firstObject];
				if (!selected || ![selected isFileURL])
					throw std::runtime_error("selected folder has no filesystem path");
				return std::filesystem::path([[selected path] fileSystemRepresentation]);
			}

		private:
			NSWindow* m_window{};
			NSWindow* m_parent{};
			NSView* m_browserContainer{};
			NSView* m_browserView{};
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
