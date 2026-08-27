#include "webview/NativeWindowHost.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

static bool CemuDispatchClose(void* context);
static void CemuDispatchMetrics(void* context);
static void CemuDispatchPadClose(void* context);
static void CemuDispatchPadMetrics(void* context);
@class CemuRenderView;
static void CemuDispatchRenderInput(CemuRenderView* view, NSEvent* event, NSInteger kind);
static void CemuDispatchRenderFocusLost(void* context, BOOL pad);
static void CemuDispatchTextComposition(void* context);
static BOOL CemuDispatchTextCommand(void* context, SEL command);

@interface CemuRenderView : NSView
{
@public
	void* inputContext;
	BOOL padSurface;
	BOOL captured;
	BOOL confined;
	BOOL rawMouseEnabled;
}
@end

@interface CemuWebWindowDelegate : NSObject <NSWindowDelegate, NSTextFieldDelegate>
{
@public
	void* context;
}
@end

@interface CemuOverlayContainer : NSView
@property(nonatomic) BOOL passesInputThrough;
@end

@implementation CemuOverlayContainer
- (NSView*)hitTest:(NSPoint)point
{
	return self.passesInputThrough ? nil : [super hitTest:point];
}
@end

@implementation CemuRenderView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)mouseMoved:(NSEvent*)event { CemuDispatchRenderInput(self, event, 0); }
- (void)mouseDragged:(NSEvent*)event { CemuDispatchRenderInput(self, event, 0); }
- (void)rightMouseDragged:(NSEvent*)event { CemuDispatchRenderInput(self, event, 0); }
- (void)otherMouseDragged:(NSEvent*)event { CemuDispatchRenderInput(self, event, 0); }
- (void)mouseDown:(NSEvent*)event { CemuDispatchRenderInput(self, event, 1); }
- (void)mouseUp:(NSEvent*)event { CemuDispatchRenderInput(self, event, 2); }
- (void)rightMouseDown:(NSEvent*)event { CemuDispatchRenderInput(self, event, 1); }
- (void)rightMouseUp:(NSEvent*)event { CemuDispatchRenderInput(self, event, 2); }
- (void)otherMouseDown:(NSEvent*)event { CemuDispatchRenderInput(self, event, 1); }
- (void)otherMouseUp:(NSEvent*)event { CemuDispatchRenderInput(self, event, 2); }
- (void)scrollWheel:(NSEvent*)event { CemuDispatchRenderInput(self, event, 3); }
- (void)keyDown:(NSEvent*)event { CemuDispatchRenderInput(self, event, 4); }
- (void)keyUp:(NSEvent*)event { CemuDispatchRenderInput(self, event, 5); }
- (void)touchesBeganWithEvent:(NSEvent*)event { CemuDispatchRenderInput(self, event, 6); }
- (void)touchesMovedWithEvent:(NSEvent*)event { CemuDispatchRenderInput(self, event, 6); }
- (void)touchesEndedWithEvent:(NSEvent*)event { CemuDispatchRenderInput(self, event, 7); }
- (void)touchesCancelledWithEvent:(NSEvent*)event { CemuDispatchRenderInput(self, event, 7); }
- (BOOL)resignFirstResponder
{
	CemuDispatchRenderFocusLost(inputContext, padSurface);
	return [super resignFirstResponder];
}
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
- (void)controlTextDidChange:(NSNotification*)notification
{
	(void)notification;
	CemuDispatchTextComposition(context);
}
- (BOOL)control:(NSControl*)control textView:(NSTextView*)textView doCommandBySelector:(SEL)command
{
	(void)control; (void)textView;
	return CemuDispatchTextCommand(context, command);
}
@end

namespace WebFrontend
{
	namespace
	{
		class CocoaRenderRegion final : public Host::IRenderRegion
		{
		public:
			explicit CocoaRenderRegion(NSView* parent, INativeWindowHost::InputHandler* inputHandler)
				: m_parent(parent)
			{
				m_view = [[CemuRenderView alloc] initWithFrame:[parent bounds]];
				m_view->inputContext = inputHandler;
				m_view->padSurface = NO;
				[m_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
				[m_view setWantsLayer:YES];
				[m_view setAcceptsTouchEvents:YES];
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
			CemuRenderView* View() const { return m_view; }
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
			CemuRenderView* m_view{};
			bool m_prepared{};
		};

		class CocoaPadRenderRegion final : public Host::IRenderRegion
		{
		public:
			CocoaPadRenderRegion(NSString* title, Host::PointerSurface surface,
				std::function<void()> closeHandler,
				std::function<void()> metricsHandler, INativeWindowHost::InputHandler* inputHandler)
				: m_closeHandler(std::move(closeHandler)),
				  m_metricsHandler(std::move(metricsHandler))
			{
				const auto style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
					NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
				const auto initialSize = surface == Host::PointerSurface::Main
					? NSMakeSize(1280, 720) : NSMakeSize(854, 480);
				m_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, initialSize.width, initialSize.height)
					styleMask:style backing:NSBackingStoreBuffered defer:NO];
				if (!m_window)
					throw std::runtime_error("failed to create the native GamePad window");
				[m_window setReleasedWhenClosed:NO];
				[m_window setTitle:title];
				m_view = [[CemuRenderView alloc] initWithFrame:[[m_window contentView] bounds]];
				m_view->inputContext = inputHandler;
				m_view->padSurface = surface == Host::PointerSurface::Pad ? YES : NO;
				[m_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
				[m_view setWantsLayer:YES];
				[m_view setAcceptsTouchEvents:YES];
				[m_window setContentView:m_view];
				m_delegate = [[CemuWebPadWindowDelegate alloc] init];
				m_delegate->context = this;
				[m_window setDelegate:m_delegate];
				[m_window center];
				[m_window makeKeyAndOrderFront:nil];
				[m_window setAcceptsMouseMovedEvents:YES];
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
			void SetFullscreen(bool fullscreen)
			{
				const bool isFullscreen = ([m_window styleMask] & NSWindowStyleMaskFullScreen) != 0;
				if (fullscreen != isFullscreen)
					[m_window toggleFullScreen:nil];
			}
			bool IsActive() const { return m_window && [m_window isKeyWindow] == YES; }
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
			CemuRenderView* View() const { return m_view; }
			NSWindow* Window() const { return m_window; }

		private:
			NSWindow* m_window{};
			CemuRenderView* m_view{};
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
				[m_window setAcceptsMouseMovedEvents:YES];
				[m_window center];
				m_delegate = [[CemuWebWindowDelegate alloc] init];
				m_delegate->context = this;
				[m_window setDelegate:m_delegate];
			}

			~CocoaWindowHost() override
			{
				for (const bool hidden : m_cursorHidden) if (hidden) [NSCursor unhide];
				CGAssociateMouseAndMouseCursorPosition(true);
				DestroyPadRenderRegion();
				DestroyMainRenderRegion();
				if (m_textInput)
				{
					[m_textInput removeFromSuperview];
					[m_textInput release];
					m_textInput = nil;
				}
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
				const auto frame = m_renderRegion
					? NSMakeRect(0, 0, m_renderRegion->GetBounds().width, m_renderRegion->GetBounds().height)
					: [[m_window contentView] bounds];
				const auto scale = m_renderRegion ? m_renderRegion->GetScaleFactor() : [m_window backingScaleFactor];
				auto metrics = Host::WindowMetricsSnapshot{
					.appActive = m_renderRegion ? m_renderRegion->IsActive() : [m_window isKeyWindow] == YES,
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
				m_webOverlay = [[CemuOverlayContainer alloc] initWithFrame:[m_root bounds]];
				[m_webOverlay setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
				[m_webView setFrame:[m_webOverlay bounds]];
				[m_webView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
				[m_window setContentView:m_root];
				[m_root addSubview:m_webOverlay];
				[m_webOverlay addSubview:m_webView];
				[m_webView release];
			}
			void PrepareWebViewDestroy(void* widget) override
			{
				if (!m_root || widget != reinterpret_cast<void*>(m_webView))
					return;
				DestroyMainRenderRegion();
				if (m_textInput)
				{
					[m_textInput removeFromSuperview];
					[m_textInput release];
					m_textInput = nil;
				}
				[m_webView retain];
				[m_webView removeFromSuperview];
				[m_window setContentView:m_webView];
				[m_webView release];
				[m_root release];
				[m_webOverlay release];
				m_root = nil;
				m_webOverlay = nil;
				m_webView = nil;
			}
			void Show() override
			{
				[m_window makeKeyAndOrderFront:nil];
				[NSApp activateIgnoringOtherApps:YES];
				ShowLibrary();
			}
			void HideLauncher() override { [m_window orderOut:nil]; }
			bool IsLauncherVisible() const override { return m_window && [m_window isVisible] == YES; }
			void ShowLibrary() override
			{
				m_runtimeOverlay = false;
				[m_webOverlay setHidden:NO];
				m_webOverlay.passesInputThrough = NO;
				[m_window makeFirstResponder:m_webView];
			}
			Host::IRenderRegion& CreateMainRenderRegion() override
			{
				if (!m_renderRegion)
					m_renderRegion = std::make_unique<CocoaPadRenderRegion>(
						@"CemuExtend Game", Host::PointerSurface::Main,
						[this] { if (m_gameCloseHandler) m_gameCloseHandler(); },
						[this] { DispatchMetrics(); }, &m_inputHandler);
				return *m_renderRegion;
			}
			void DestroyMainRenderRegion() override { m_renderRegion.reset(); }
			void ShowRenderRegion() override
			{
				auto& region = CreateMainRenderRegion();
				region.SetVisible(true);
				[m_webOverlay setHidden:YES];
				region.RequestFocus();
			}
			Host::IRenderRegion& CreatePadRenderRegion() override
			{
				if (!m_padRenderRegion)
					m_padRenderRegion = std::make_unique<CocoaPadRenderRegion>(
						@"CemuExtend GamePad", Host::PointerSurface::Pad,
						[this] { if (m_padCloseHandler) m_padCloseHandler(); },
						[this] { if (m_padMetricsEnabled) DispatchMetrics(); }, &m_inputHandler);
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
				m_fullscreen = fullscreen;
				if (m_renderRegion)
					m_renderRegion->SetFullscreen(fullscreen);
			}
			void SetCloseHandler(CloseHandler handler) override { m_closeHandler = std::move(handler); }
			void SetGameCloseHandler(GameCloseHandler handler) override { m_gameCloseHandler = std::move(handler); }
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
			void SetInputHandler(InputHandler handler) override { m_inputHandler = std::move(handler); }
			void ApplyPointerPresentation(const NativePointerPresentation& presentation) override
			{
				auto* view = presentation.surface == Host::PointerSurface::Main
					? (m_renderRegion ? m_renderRegion->View() : nil)
					: (m_padRenderRegion ? m_padRenderRegion->View() : nil);
				if (!view) return;
				const auto index = presentation.surface == Host::PointerSurface::Main ? 0U : 1U;
				const bool wasCaptured = view->captured;
				const bool captured = presentation.ownsPointer && !presentation.showCursor;
				view->captured = captured;
				view->confined = presentation.confine;
				view->rawMouseEnabled = presentation.rawMouseEnabled;
				NSCursor* cursor = [NSCursor arrowCursor];
				switch (presentation.cursor)
				{
				case 1: cursor = [NSCursor IBeamCursor]; break;
				case 3: cursor = [NSCursor resizeUpDownCursor]; break;
				case 4: cursor = [NSCursor resizeLeftRightCursor]; break;
				case 7: cursor = [NSCursor pointingHandCursor]; break;
				case 8: cursor = [NSCursor operationNotAllowedCursor]; break;
				default: break;
				}
				[cursor set];
				if (!presentation.showCursor && !m_cursorHidden[index])
				{
					[NSCursor hide];
					m_cursorHidden[index] = true;
				}
				else if (presentation.showCursor && m_cursorHidden[index])
				{
					[NSCursor unhide];
					m_cursorHidden[index] = false;
				}
				if (presentation.enteringCapture)
				{
					CGAssociateMouseAndMouseCursorPosition(false);
					const auto center = [view convertPoint:NSMakePoint(NSMidX([view bounds]), NSMidY([view bounds]))
						toView:nil];
					const auto screen = [[view window] convertPointToScreen:center];
					CGWarpMouseCursorPosition(CGPointMake(screen.x, screen.y));
				}
				if (presentation.leavingPolicy)
				{
					view->captured = NO;
					view->confined = NO;
					CGAssociateMouseAndMouseCursorPosition(true);
				}
				else if (wasCaptured && !captured)
				{
					CGAssociateMouseAndMouseCursorPosition(true);
				}
			}
			void UpdateTextInput(const NativeTextInputRequest& request) override
			{
				if (!request.active)
				{
					m_textInputSequence = 0;
					if (m_textInput) [m_textInput setHidden:YES];
					if (m_renderRegion) m_renderRegion->RequestFocus();
					return;
				}
				if (!m_renderRegion || !m_root) return;
				if (!m_textInput)
				{
					m_textInput = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 2, 24)];
					[m_textInput setDelegate:m_delegate];
					[m_textInput setBezeled:NO];
					[m_textInput setDrawsBackground:NO];
					[m_textInput setAlphaValue:0.01];
					[m_root addSubview:m_textInput positioned:NSWindowAbove relativeTo:nil];
				}
				const auto bounds = m_renderRegion->GetBounds();
				const auto x = std::clamp(request.caretX * bounds.width / 1280, 0, std::max(0, bounds.width - 1));
				const auto yTop = std::clamp(request.caretY * bounds.height / 720, 0, std::max(0, bounds.height - 1));
				const auto height = std::clamp(request.lineHeight * bounds.height / 720, 1, 64);
				[m_textInput setFrame:NSMakeRect(x, std::max(0, bounds.height - yTop - height), 2, height)];
				m_textMaximumLength = request.maximumLength;
				if (m_textInputSequence != request.sequence)
				{
					m_textInputUpdating = true;
					m_textInputSequence = request.sequence;
					NSString* text = [[[NSString alloc] initWithBytes:request.initialText.data()
						length:request.initialText.size() encoding:NSUTF8StringEncoding] autorelease];
					[m_textInput setStringValue:text ?: @""];
					m_textInputUpdating = false;
				}
				[m_textInput setHidden:NO];
				[m_window makeFirstResponder:m_textInput];
			}
			std::string GetKeyName(std::uint32_t key) const override
			{
				return "Key " + std::to_string(key);
			}
			std::pair<bool, std::string> GetClipboardText() override
			{
				NSString* value = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
				if (!value) return {false, {}};
				const char* text = [value UTF8String];
				return {text != nullptr, text ? std::string(text) : std::string{}};
			}
			bool SetClipboardText(std::string text) override
			{
				NSString* value = [[[NSString alloc] initWithBytes:text.data() length:text.size()
					encoding:NSUTF8StringEncoding] autorelease];
				if (!value) return false;
				auto* pasteboard = [NSPasteboard generalPasteboard];
				[pasteboard clearContents];
				return [pasteboard setString:value forType:NSPasteboardTypeString] == YES;
			}
			bool SetClipboardImage(std::span<const std::uint8_t> rgb,
				std::int32_t width, std::int32_t height) override
			{
				if (width <= 0 || height <= 0 || rgb.size() !=
					static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3)
					return false;
				auto* representation = [[NSBitmapImageRep alloc]
					initWithBitmapDataPlanes:nil pixelsWide:width pixelsHigh:height
					bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO
					colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:width * 3 bitsPerPixel:24];
				if (!representation) return false;
				std::memcpy([representation bitmapData], rgb.data(), rgb.size());
				auto* image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
				[image addRepresentation:representation];
				auto* pasteboard = [NSPasteboard generalPasteboard];
				[pasteboard clearContents];
				const bool success = [pasteboard writeObjects:@[image]] == YES;
				[image release];
				[representation release];
				return success;
			}
			bool OpenExternalUrl(std::string url) override
			{
				NSString* value = [[[NSString alloc] initWithBytes:url.data() length:url.size()
					encoding:NSUTF8StringEncoding] autorelease];
				NSURL* target = value ? [NSURL URLWithString:value] : nil;
				return target && [[NSWorkspace sharedWorkspace] openURL:target] == YES;
			}
			std::optional<std::string> PickTitleInstallSource() override
			{
				NSOpenPanel* panel = [NSOpenPanel openPanel];
				[panel setCanChooseFiles:YES]; [panel setCanChooseDirectories:NO];
				[panel setAllowsMultipleSelection:NO]; [panel setAllowedFileTypes:@[@"xml"]];
				[panel setTitle:@"Select title to install"];
				if ([panel runModal] != NSModalResponseOK) return std::nullopt;
				const char* path = [[[panel URL] path] UTF8String];
				return path ? std::optional<std::string>{path} : std::nullopt;
			}
			std::optional<std::string> PickWuaDestination(
				std::string suggestedFileName) override
			{
				NSSavePanel* panel = [NSSavePanel savePanel];
				[panel setAllowedFileTypes:@[@"wua"]]; [panel setCanCreateDirectories:YES];
				NSString* name = [[[NSString alloc] initWithBytes:suggestedFileName.data()
					length:suggestedFileName.size() encoding:NSUTF8StringEncoding] autorelease];
				if (name) [panel setNameFieldStringValue:name];
				[panel setTitle:@"Save Wii U game archive"];
				if ([panel runModal] != NSModalResponseOK) return std::nullopt;
				const char* path = [[[panel URL] path] UTF8String];
				return path ? std::optional<std::string>{path} : std::nullopt;
			}
			void DispatchTextComposition()
			{
				if (!m_textInput || m_textInputUpdating || !m_inputHandler || !m_textInputSequence)
					return;
				NSTextView* editor = (NSTextView*)[m_textInput currentEditor];
				NSString* full = editor ? [editor string] : [m_textInput stringValue];
				NSRange marked = editor && [editor hasMarkedText] ? [editor markedRange] : NSMakeRange(NSNotFound, 0);
				NSString* preedit = marked.location != NSNotFound ? [full substringWithRange:marked] : @"";
				NSMutableString* committed = [[full mutableCopy] autorelease];
				if (marked.location != NSNotFound) [committed deleteCharactersInRange:marked];
				if (m_textMaximumLength && [committed length] > m_textMaximumLength)
				{
					[committed deleteCharactersInRange:NSMakeRange(m_textMaximumLength,
						[committed length] - m_textMaximumLength)];
					m_textInputUpdating = true;
					[m_textInput setStringValue:committed];
					m_textInputUpdating = false;
				}
				const auto selected = editor ? [editor selectedRange] : NSMakeRange([committed length], 0);
				const auto prefixLength = std::min(selected.location, [committed length]);
				const char* committedUtf8 = [committed UTF8String];
				const char* preeditUtf8 = [preedit UTF8String];
				NSString* prefix = [committed substringToIndex:prefixLength];
				m_inputHandler({.kind = NativeInputKind::TextComposition,
					.text = committedUtf8 ? committedUtf8 : "", .preedit = preeditUtf8 ? preeditUtf8 : "",
					.textCursor = static_cast<std::uint32_t>(strlen([prefix UTF8String] ?: "")),
					.selectionLength = static_cast<std::uint32_t>(strlen(preeditUtf8 ?: "")),
					.textSequence = m_textInputSequence});
			}
			BOOL DispatchTextCommand(SEL command)
			{
				if (command != @selector(insertNewline:) && command != @selector(insertNewlineIgnoringFieldEditor:))
					return NO;
				NSTextView* editor = (NSTextView*)[m_textInput currentEditor];
				if (editor && [editor hasMarkedText]) return NO;
				if (m_inputHandler)
				{
					m_inputHandler({.kind = NativeInputKind::Key, .key = 0x24,
						.usage = 0x28, .pressed = true});
					m_inputHandler({.kind = NativeInputKind::Key, .key = 0x24,
						.usage = 0x28, .pressed = false});
				}
				return YES;
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
			NSWindow* m_window{};
			NSView* m_root{};
			NSView* m_webView{};
			CemuOverlayContainer* m_webOverlay{};
			bool m_runtimeOverlay{};
			bool m_runtimeOverlayInteractive{};
			NSTextField* m_textInput{};
			CemuWebWindowDelegate* m_delegate{};
			std::unique_ptr<CocoaPadRenderRegion> m_renderRegion;
			std::unique_ptr<CocoaPadRenderRegion> m_padRenderRegion;
			CloseHandler m_closeHandler;
			GameCloseHandler m_gameCloseHandler;
			MetricsHandler m_metricsHandler;
			PadCloseHandler m_padCloseHandler;
			InputHandler m_inputHandler;
			std::array<bool, 2> m_cursorHidden{};
			std::uint64_t m_textInputSequence{};
			std::uint32_t m_textMaximumLength{};
			bool m_textInputUpdating{};
			bool m_padMetricsEnabled{};
			bool m_fullscreen{};
		};
	}

	std::unique_ptr<INativeWindowHost> CreateNativeWindowHost()
	{
		return std::make_unique<CocoaWindowHost>();
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

	void DispatchCocoaTextComposition(void* context)
	{
		static_cast<CocoaWindowHost*>(context)->DispatchTextComposition();
	}

	BOOL DispatchCocoaTextCommand(void* context, SEL command)
	{
		return static_cast<CocoaWindowHost*>(context)->DispatchTextCommand(command);
	}
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

static void CemuDispatchRenderInput(CemuRenderView* view, NSEvent* event, NSInteger kind)
{
	if (!view || !view->inputContext || !event) return;
	auto* handler = static_cast<WebFrontend::INativeWindowHost::InputHandler*>(view->inputContext);
	if (!handler || !*handler) return;
	const auto surface = view->padSurface ? Host::PointerSurface::Pad : Host::PointerSurface::Main;
	const auto bounds = [view bounds];
	const auto physicalSize = [view convertSizeToBacking:bounds.size];
	auto emit = [&](WebFrontend::NativeInputEvent input) {
		input.surface = surface;
		input.contentWidth = static_cast<std::int32_t>(physicalSize.width);
		input.contentHeight = static_cast<std::int32_t>(physicalSize.height);
		(*handler)(input);
	};
	const auto local = [view convertPoint:[event locationInWindow] fromView:nil];
	const auto physical = [view convertPointToBacking:local];
	const auto x = static_cast<std::int32_t>(physical.x);
	const auto y = static_cast<std::int32_t>(physicalSize.height - physical.y);
	const bool inside = NSPointInRect(local, bounds) == YES;
	if (kind == 0)
	{
		if (view->captured && view->rawMouseEnabled)
		{
			const auto scale = [[view window] backingScaleFactor];
			emit({.kind = WebFrontend::NativeInputKind::RawMouse,
				.deltaX = static_cast<std::int32_t>([event deltaX] * scale),
				.deltaY = static_cast<std::int32_t>([event deltaY] * scale)});
			return;
		}
		emit({.kind = WebFrontend::NativeInputKind::PointerMove,
			.x = x, .y = y, .insideContent = inside});
		if (view->captured)
		{
			const auto center = NSMakePoint(NSMidX(bounds), NSMidY(bounds));
			const auto windowPoint = [view convertPoint:center toView:nil];
			const auto screenPoint = [[view window] convertPointToScreen:windowPoint];
			CGWarpMouseCursorPosition(CGPointMake(screenPoint.x, screenPoint.y));
			return;
		}
		if (view->confined && !inside)
		{
			const auto clamped = NSMakePoint(std::clamp(local.x, NSMinX(bounds), NSMaxX(bounds) - 1),
				std::clamp(local.y, NSMinY(bounds), NSMaxY(bounds) - 1));
			const auto windowPoint = [view convertPoint:clamped toView:nil];
			const auto screenPoint = [[view window] convertPointToScreen:windowPoint];
			CGWarpMouseCursorPosition(CGPointMake(screenPoint.x, screenPoint.y));
		}
		return;
	}
	if (kind == 1 || kind == 2)
	{
		const auto native = [event buttonNumber];
		const std::uint32_t button = native == 0 ? 1 : native == 1 ? 3 : native == 2 ? 2 : native == 3 ? 8 : 9;
		emit({.kind = WebFrontend::NativeInputKind::PointerButton, .x = x, .y = y,
			.button = button, .pressed = kind == 1, .insideContent = inside});
		return;
	}
	if (kind == 3)
	{
		emit({.kind = WebFrontend::NativeInputKind::PointerWheel,
			.wheelX = static_cast<std::int32_t>([event scrollingDeltaX] * 120.0),
			.wheelY = static_cast<std::int32_t>([event scrollingDeltaY] * 120.0)});
		return;
	}
	if (kind == 4 || kind == 5)
	{
		const auto flags = [event modifierFlags];
		const auto modifiers = static_cast<std::uint8_t>(
			((flags & NSEventModifierFlagControl) ? 1U : 0U) |
			((flags & NSEventModifierFlagShift) ? 2U : 0U) |
			((flags & NSEventModifierFlagOption) ? 4U : 0U) |
			((flags & NSEventModifierFlagCommand) ? 8U : 0U));
		emit({.kind = WebFrontend::NativeInputKind::Key,
			.key = static_cast<std::uint32_t>([event keyCode]), .modifiers = modifiers,
			.pressed = kind == 4, .repeat = [event isARepeat] == YES});
		if (kind == 4 && (flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) == 0)
		{
			NSString* characters = [event characters];
			const char* text = [characters UTF8String];
			if (text) emit({.kind = WebFrontend::NativeInputKind::Character,
				.repeat = [event isARepeat] == YES, .text = text});
		}
		return;
	}
	if (kind == 6 || kind == 7)
	{
		NSSet<NSTouch*>* touches = [event touchesMatchingPhase:NSTouchPhaseAny inView:view];
		NSTouch* touch = [touches anyObject];
		if (!touch) return;
		const auto normalized = [touch normalizedPosition];
		emit({.kind = WebFrontend::NativeInputKind::Touch,
			.x = static_cast<std::int32_t>(normalized.x * physicalSize.width),
			.y = static_cast<std::int32_t>((1.0 - normalized.y) * physicalSize.height),
			.touchId = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>([touch identity])),
			.pressed = kind == 6, .insideContent = true});
	}
}

static void CemuDispatchRenderFocusLost(void* context, BOOL pad)
{
	if (!context) return;
	auto* handler = static_cast<WebFrontend::INativeWindowHost::InputHandler*>(context);
	if (handler && *handler)
		(*handler)({.kind = WebFrontend::NativeInputKind::FocusLost,
			.surface = pad ? Host::PointerSurface::Pad : Host::PointerSurface::Main});
}

static void CemuDispatchTextComposition(void* context)
{
	if (context) WebFrontend::DispatchCocoaTextComposition(context);
}

static BOOL CemuDispatchTextCommand(void* context, SEL command)
{
	return context ? WebFrontend::DispatchCocoaTextCommand(context, command) : NO;
}

#endif
