#include "wxgui/MacRelativePointer.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <utility>

namespace
{
	MacRelativePointer::MotionHandler s_handler;
	MacRelativePointer::HoverHandler s_hoverHandler;
	id s_motionMonitor = nil;
	id s_resignObserver = nil;
	bool s_captured = false;
	bool s_skipNextDelta = false;

	void ApplyCaptured(bool captured)
	{
		// Re-assert every call: wxWidgets' WarpPointer silently re-associates the cursor.
		if (captured)
			CGAssociateMouseAndMouseCursorPosition(false);
		if (captured == s_captured)
			return;
		s_captured = captured;
		if (!captured)
			CGAssociateMouseAndMouseCursorPosition(true);
		if (captured)
		{
			// mouseMoved is only delivered to windows that opt in.
			[[NSApp keyWindow] setAcceptsMouseMovedEvents:YES];
			// A cursor warp otherwise suppresses local mouse events for ~0.25s.
			CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
			if (source)
			{
				CGEventSourceSetLocalEventsSuppressionInterval(source, 0.0);
				CFRelease(source);
			}
		}
	}
} // namespace

void MacRelativePointer::Install(MotionHandler motion, HoverHandler hover)
{
	s_handler = std::move(motion);
	s_hoverHandler = std::move(hover);
	if (s_motionMonitor)
		return;

	const NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
							 NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged;
	s_motionMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
															handler:^NSEvent*(NSEvent* event) {
		if (s_captured)
		{
			// The first event after a warp reports the warp distance as motion.
			if (s_skipNextDelta)
				s_skipNextDelta = false;
			else if (s_handler)
				s_handler(event.deltaX, event.deltaY);
		}
		// wx never sees button-less mouseMoved over the render canvas.
		else if (event.type == NSEventTypeMouseMoved && s_hoverHandler)
			s_hoverHandler();
		return event;
	}];
	// Never leave the cursor detached while another app is in front.
	s_resignObserver = [[NSNotificationCenter defaultCenter]
		addObserverForName:NSApplicationDidResignActiveNotification
					object:nil
					 queue:nil
				usingBlock:^(NSNotification*) {
		  ApplyCaptured(false);
		}];
}

void MacRelativePointer::SetCaptured(bool captured)
{
	ApplyCaptured(captured && [NSApp isActive]);
}

void MacRelativePointer::WarpTo(int screenX, int screenY)
{
	CGWarpMouseCursorPosition(CGPointMake(screenX, screenY));
	s_skipNextDelta = true;
}
