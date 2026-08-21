#include "webview/NativeWindowHost.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

static void CemuDispatchMenu(void* context, NSInteger tag);
static bool CemuDispatchClose(void* context);
static void CemuDispatchMetrics(void* context);
static void CemuDispatchPadClose(void* context);
static void CemuDispatchPadMetrics(void* context);

@interface CemuWebWindowDelegate : NSObject <NSWindowDelegate>
{
@public
	void* context;
}
- (void)onMenu:(id)sender;
@end

@interface CemuWebPadWindowDelegate : NSObject <NSWindowDelegate>
{
@public
	void* context;
}
@end

@implementation CemuWebPadWindowDelegate
- (BOOL)windowShouldClose:(id)sender
{
	(void)sender;
	CemuDispatchPadClose(context);
	return NO;
}
- (void)windowDidResize:(NSNotification*)notification
{
	(void)notification;
	CemuDispatchPadMetrics(context);
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
	(void)notification;
	CemuDispatchPadMetrics(context);
}
@end

@implementation CemuWebWindowDelegate
- (void)onMenu:(id)sender
{
	CemuDispatchMenu(context, [sender tag]);
}
- (BOOL)windowShouldClose:(id)sender
{
	(void)sender;
	return CemuDispatchClose(context) ? YES : NO;
}
- (void)windowDidResize:(NSNotification*)notification
{
	(void)notification;
	CemuDispatchMetrics(context);
}
- (void)windowDidBecomeKey:(NSNotification*)notification
{
	(void)notification;
	CemuDispatchMetrics(context);
}
- (void)windowDidResignKey:(NSNotification*)notification
{
	(void)notification;
	CemuDispatchMetrics(context);
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
	(void)notification;
	CemuDispatchMetrics(context);
}
@end

namespace WebFrontend
{
	namespace
	{
		class CocoaRenderRegion final : public Host::IRenderRegion
		{
		public:
			explicit CocoaRenderRegion(NSView* parent)
				: m_parent(parent)
			{
				m_view = [[NSView alloc] initWithFrame:[parent bounds]];
				[m_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
				[m_view setWantsLayer:YES];
				[m_view setHidden:YES];
				[parent addSubview:m_view];
			}
			~CocoaRenderRegion() override { PrepareForDestroy(); }
			Host::NativeWindowHandle GetWindowHandle() const override
			{
				return {Host::NativeWindowBackend::Cocoa, nullptr,
					reinterpret_cast<void*>([m_view window])};
			}
			Host::NativeWindowHandle GetSurfaceHandle() const override
			{
				return {Host::NativeWindowBackend::Cocoa, nullptr,
					reinterpret_cast<void*>(m_view)};
			}
			Host::RenderRegionBounds GetBounds() const override
			{
				const auto frame = [m_view frame];
				return {static_cast<std::int32_t>(frame.origin.x),
					static_cast<std::int32_t>(frame.origin.y),
					static_cast<std::int32_t>(frame.size.width),
					static_cast<std::int32_t>(frame.size.height)};
			}
			void SetBounds(Host::RenderRegionBounds bounds) override
			{
				[m_view setFrame:NSMakeRect(bounds.x, bounds.y,
					std::max(1, bounds.width), std::max(1, bounds.height))];
			}
			void SetVisible(bool visible) override { [m_view setHidden:visible ? NO : YES]; }
			void RequestFocus() override { [[m_view window] makeFirstResponder:m_view]; }
			void PrepareForDestroy() override
			{
				if (std::exchange(m_prepared, true) || !m_view)
					return;
				[m_view removeFromSuperview];
				[m_view release];
				m_view = nil;
			}

		private:
			NSView* m_parent{};
			NSView* m_view{};
			bool m_prepared{};
		};

		class CocoaPadRenderRegion final : public Host::IRenderRegion
		{
		public:
			CocoaPadRenderRegion(std::function<void()> closeHandler,
				std::function<void()> metricsHandler)
				: m_closeHandler(std::move(closeHandler)),
				  m_metricsHandler(std::move(metricsHandler))
			{
				const auto style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
					NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
				m_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 854, 480)
					styleMask:style backing:NSBackingStoreBuffered defer:NO];
				if (!m_window)
					throw std::runtime_error("failed to create the native GamePad window");
				[m_window setReleasedWhenClosed:NO];
				[m_window setTitle:@"CemuExtend GamePad"];
				m_view = [[NSView alloc] initWithFrame:[[m_window contentView] bounds]];
				[m_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
				[m_view setWantsLayer:YES];
				[m_window setContentView:m_view];
				m_delegate = [[CemuWebPadWindowDelegate alloc] init];
				m_delegate->context = this;
				[m_window setDelegate:m_delegate];
				[m_window center];
				[m_window makeKeyAndOrderFront:nil];
			}

			~CocoaPadRenderRegion() override { PrepareForDestroy(); }
			Host::NativeWindowHandle GetWindowHandle() const override
			{
				return {Host::NativeWindowBackend::Cocoa, nullptr,
					reinterpret_cast<void*>(m_window)};
			}
			Host::NativeWindowHandle GetSurfaceHandle() const override
			{
				return {Host::NativeWindowBackend::Cocoa, nullptr,
					reinterpret_cast<void*>(m_view)};
			}
			Host::RenderRegionBounds GetBounds() const override
			{
				const auto bounds = [m_view bounds];
				return {0, 0, static_cast<std::int32_t>(bounds.size.width),
					static_cast<std::int32_t>(bounds.size.height)};
			}
			void SetBounds(Host::RenderRegionBounds bounds) override
			{
				[m_window setContentSize:NSMakeSize(std::max(1, bounds.width),
					std::max(1, bounds.height))];
			}
			void SetVisible(bool visible) override
			{
				if (visible)
					[m_window orderFront:nil];
				else
					[m_window orderOut:nil];
			}
			void RequestFocus() override { [m_window makeKeyAndOrderFront:nil]; }
			void PrepareForDestroy() override
			{
				if (std::exchange(m_prepared, true) || !m_window)
					return;
				m_closeHandler = {};
				m_metricsHandler = {};
				m_delegate->context = nullptr;
				[m_window setDelegate:nil];
				[m_window close];
				[m_window release];
				[m_view release];
				[m_delegate release];
				m_window = nil;
				m_view = nil;
				m_delegate = nil;
			}
			void DispatchClose() { if (m_closeHandler) m_closeHandler(); }
			void DispatchMetrics() { if (m_metricsHandler) m_metricsHandler(); }
			[[nodiscard]] double GetScaleFactor() const
			{
				return m_window ? [m_window backingScaleFactor] : 1.0;
			}

		private:
			NSWindow* m_window{};
			NSView* m_view{};
			CemuWebPadWindowDelegate* m_delegate{};
			std::function<void()> m_closeHandler;
			std::function<void()> m_metricsHandler;
			bool m_prepared{};
		};

		class CocoaWindowHost final : public INativeWindowHost
		{
		public:
			CocoaWindowHost()
			{
				[NSApplication sharedApplication];
				[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
				const auto style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
					NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
				m_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1100, 720)
					styleMask:style backing:NSBackingStoreBuffered defer:NO];
				if (!m_window)
					throw std::runtime_error("failed to create native Cocoa main window");
				[m_window setReleasedWhenClosed:NO];
				[m_window setTitle:@"CemuExtend"];
				[m_window center];
				m_delegate = [[CemuWebWindowDelegate alloc] init];
				m_delegate->context = this;
				[m_window setDelegate:m_delegate];
				BuildMenu();
			}

			~CocoaWindowHost() override
			{
				DestroyPadRenderRegion();
				DestroyMainRenderRegion();
				m_delegate->context = nullptr;
				[m_window setDelegate:nil];
				[m_window close];
				[m_window release];
				[m_delegate release];
			}

			void* GetNativeWindow() const override { return reinterpret_cast<void*>(m_window); }
			Host::NativeWindowHandle GetMainWindowHandle() const override
			{
				return {Host::NativeWindowBackend::Cocoa, nullptr,
					reinterpret_cast<void*>(m_window)};
			}
			Host::WindowMetricsSnapshot GetMetrics() const override
			{
				const auto frame = [[m_window contentView] bounds];
				const auto scale = [m_window backingScaleFactor];
				auto metrics = Host::WindowMetricsSnapshot{
					.appActive = [m_window isKeyWindow] == YES,
					.fullscreen = m_fullscreen,
					.width = static_cast<std::int32_t>(frame.size.width),
					.height = static_cast<std::int32_t>(frame.size.height),
					.physicalWidth = static_cast<std::int32_t>(frame.size.width * scale),
					.physicalHeight = static_cast<std::int32_t>(frame.size.height * scale),
					.dpiScale = scale,
				};
				if (m_padRenderRegion)
				{
					const auto pad = m_padRenderRegion->GetBounds();
					const auto padScale = m_padRenderRegion->GetScaleFactor();
					metrics.padOpen = true;
					metrics.padWidth = pad.width;
					metrics.padHeight = pad.height;
					metrics.physicalPadWidth = static_cast<std::int32_t>(pad.width * padScale);
					metrics.physicalPadHeight = static_cast<std::int32_t>(pad.height * padScale);
					metrics.padDpiScale = padScale;
				}
				return metrics;
			}
			void AttachWebView(void* widget) override
			{
				if (m_root || !widget)
					throw std::logic_error("webview widget cannot be attached");
				m_webView = reinterpret_cast<NSView*>(widget);
				[m_webView retain];
				m_root = [[NSView alloc] initWithFrame:[[m_window contentView] bounds]];
				[m_root setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
				[m_webView setFrame:[m_root bounds]];
				[m_webView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
				[m_window setContentView:m_root];
				[m_root addSubview:m_webView];
				[m_webView release];
			}
			void PrepareWebViewDestroy(void* widget) override
			{
				if (!m_root || widget != reinterpret_cast<void*>(m_webView))
					return;
				DestroyMainRenderRegion();
				[m_webView retain];
				[m_webView removeFromSuperview];
				[m_window setContentView:m_webView];
				[m_webView release];
				[m_root release];
				m_root = nil;
				m_webView = nil;
			}
			void Show() override
			{
				[m_window makeKeyAndOrderFront:nil];
				[NSApp activateIgnoringOtherApps:YES];
				ShowLibrary();
			}
			void ShowLibrary() override
			{
				if (m_renderRegion)
					m_renderRegion->SetVisible(false);
				[m_webView setHidden:NO];
				[m_window makeFirstResponder:m_webView];
			}
			Host::IRenderRegion& CreateMainRenderRegion() override
			{
				if (!m_root)
					throw std::logic_error("webview content host is not attached");
				if (!m_renderRegion)
					m_renderRegion = std::make_unique<CocoaRenderRegion>(m_root);
				return *m_renderRegion;
			}
			void DestroyMainRenderRegion() override { m_renderRegion.reset(); }
			void ShowRenderRegion() override
			{
				auto& region = CreateMainRenderRegion();
				[m_webView setHidden:YES];
				region.SetVisible(true);
				region.RequestFocus();
			}
			Host::IRenderRegion& CreatePadRenderRegion() override
			{
				if (!m_padRenderRegion)
					m_padRenderRegion = std::make_unique<CocoaPadRenderRegion>(
						[this] { if (m_padCloseHandler) m_padCloseHandler(); },
						[this] { if (m_padMetricsEnabled) DispatchMetrics(); });
				return *m_padRenderRegion;
			}
			void DestroyPadRenderRegion() override
			{
				m_padMetricsEnabled = false;
				m_padRenderRegion.reset();
				DispatchMetrics();
			}
			bool IsPadRenderRegionOpen() const override { return m_padRenderRegion != nullptr; }
			void SetFullscreen(bool fullscreen) override
			{
				if (m_fullscreen == fullscreen)
					return;
				m_fullscreen = fullscreen;
				[m_window toggleFullScreen:nil];
			}
			void SetCloseHandler(CloseHandler handler) override { m_closeHandler = std::move(handler); }
			void SetMenuHandler(MenuHandler handler) override { m_menuHandler = std::move(handler); }
			void SetMetricsHandler(MetricsHandler handler) override
			{
				m_metricsHandler = std::move(handler);
				DispatchMetrics();
			}
			void SetPadCloseHandler(PadCloseHandler handler) override
			{
				m_padCloseHandler = std::move(handler);
			}
			void SetPadMetricsEnabled(bool enabled) override
			{
				m_padMetricsEnabled = enabled;
			}
			void DispatchMenu(NSInteger tag)
			{
				if (m_menuHandler && tag >= 0 && tag <= static_cast<NSInteger>(MenuCommand::About))
					m_menuHandler(static_cast<MenuCommand>(tag));
			}
			bool DispatchClose()
			{
				if (m_closeHandler)
					m_closeHandler();
				return false;
			}
			void DispatchMetrics()
			{
				if (m_metricsHandler && m_window)
					m_metricsHandler(GetMetrics());
			}

		private:
			NSMenuItem* Item(NSString* title, MenuCommand command)
			{
				auto* item = [[[NSMenuItem alloc] initWithTitle:title action:@selector(onMenu:)
					keyEquivalent:@""] autorelease];
				[item setTag:static_cast<NSInteger>(command)];
				[item setTarget:m_delegate];
				return item;
			}
			NSMenu* Submenu(NSMenu* bar, NSString* title)
			{
				auto* root = [[[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""] autorelease];
				auto* menu = [[[NSMenu alloc] initWithTitle:title] autorelease];
				[root setSubmenu:menu];
				[bar addItem:root];
				return menu;
			}
			void BuildMenu()
			{
				auto* bar = [[[NSMenu alloc] initWithTitle:@"CemuExtend"] autorelease];
				auto* app = Submenu(bar, @"CemuExtend");
				[app addItem:Item(@"About CemuExtend", MenuCommand::About)];
				[app addItem:[NSMenuItem separatorItem]];
				[app addItem:Item(@"Quit CemuExtend", MenuCommand::Exit)];
				auto* file = Submenu(bar, @"File");
				[file addItem:Item(@"Load…", MenuCommand::Load)];
				[file addItem:Item(@"End emulation", MenuCommand::EndEmulation)];
				auto* options = Submenu(bar, @"Options");
				[options addItem:Item(@"Fullscreen", MenuCommand::ToggleFullscreen)];
				[options addItem:Item(@"Separate GamePad view", MenuCommand::TogglePadView)];
				[options addItem:Item(@"General Settings", MenuCommand::GeneralSettings)];
				[options addItem:Item(@"Input Settings", MenuCommand::InputSettings)];
				auto* tools = Submenu(bar, @"Tools");
				[tools addItem:Item(@"Graphic Packs", MenuCommand::GraphicPacks)];
				[tools addItem:Item(@"Title Manager", MenuCommand::TitleManager)];
				(void)Submenu(bar, @"CPU");
				(void)Submenu(bar, @"NFC");
				auto* debug = Submenu(bar, @"Debug");
				[debug addItem:Item(@"Logging", MenuCommand::Logging)];
				[NSApp setMainMenu:bar];
			}

			NSWindow* m_window{};
			NSView* m_root{};
			NSView* m_webView{};
			CemuWebWindowDelegate* m_delegate{};
			std::unique_ptr<CocoaRenderRegion> m_renderRegion;
			std::unique_ptr<CocoaPadRenderRegion> m_padRenderRegion;
			CloseHandler m_closeHandler;
			MenuHandler m_menuHandler;
			MetricsHandler m_metricsHandler;
			PadCloseHandler m_padCloseHandler;
			bool m_padMetricsEnabled{};
			bool m_fullscreen{};
		};
	}

	std::unique_ptr<INativeWindowHost> CreateNativeWindowHost()
	{
		return std::make_unique<CocoaWindowHost>();
	}

	void DispatchCocoaMenu(void* context, NSInteger tag)
	{
		static_cast<CocoaWindowHost*>(context)->DispatchMenu(tag);
	}

	bool DispatchCocoaClose(void* context)
	{
		return static_cast<CocoaWindowHost*>(context)->DispatchClose();
	}

	void DispatchCocoaMetrics(void* context)
	{
		static_cast<CocoaWindowHost*>(context)->DispatchMetrics();
	}

	void DispatchCocoaPadClose(void* context)
	{
		static_cast<CocoaPadRenderRegion*>(context)->DispatchClose();
	}

	void DispatchCocoaPadMetrics(void* context)
	{
		static_cast<CocoaPadRenderRegion*>(context)->DispatchMetrics();
	}
}

static void CemuDispatchMenu(void* context, NSInteger tag)
{
	if (context)
		WebFrontend::DispatchCocoaMenu(context, tag);
}

static bool CemuDispatchClose(void* context)
{
	return context ? WebFrontend::DispatchCocoaClose(context) : false;
}

static void CemuDispatchMetrics(void* context)
{
	if (context)
		WebFrontend::DispatchCocoaMetrics(context);
}

static void CemuDispatchPadClose(void* context)
{
	if (context)
		WebFrontend::DispatchCocoaPadClose(context);
}

static void CemuDispatchPadMetrics(void* context)
{
	if (context)
		WebFrontend::DispatchCocoaPadMetrics(context);
}

#endif
