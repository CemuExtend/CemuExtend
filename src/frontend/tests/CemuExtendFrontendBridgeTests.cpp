#include "frontend/CemuExtendFrontendBridge.h"

#include <cassert>

int main()
{
	Frontend::CemuExtendFrontendBridge bridge;
	const auto visible = bridge.ApplyPointerPolicy(1, 7, 1U << 2U, true, true);
	assert(visible.ownsPointer && visible.showCursor && visible.confine);
	const auto inactive = bridge.ApplyPointerPolicy(1, 7, 1U << 2U, false, true);
	assert(inactive.ownsPointer && !inactive.confine);
	const auto noCanvas = bridge.ApplyPointerPolicy(3, 0, 0, true, false);
	assert(!noCanvas.enteringCapture && !noCanvas.requestRawMouse);
	const auto captured = bridge.ApplyPointerPolicy(3, 0, 0, true, true);
	assert(captured.enteringCapture && captured.requestRawMouse && !captured.showCursor);
	// The synthetic event that caused capture is suppressed.
	auto motion = bridge.UpdatePosition({100, 100}, {50, 50}, false);
	assert(motion.delta == Frontend::CemuExtendPoint{});
	motion = bridge.UpdatePosition({60, 45}, {50, 50}, false);
	assert((motion.delta == Frontend::CemuExtendPoint{10, -5}));
	bridge.MarkRawMouseSeen();
	motion = bridge.UpdatePosition({61, 45}, {50, 50}, true);
	assert(motion.rawRelative && motion.delta == Frontend::CemuExtendPoint{});

	auto buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Down, 1);
	assert(buttons.buttons == 1 && buttons.changed == 1);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Aggregate, 1, 0);
	assert(buttons.buttons == 0 && buttons.changed == 1);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Down, 1);
	assert(buttons.buttons == 1 && buttons.changed == 1);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Down, 1);
	assert(buttons.buttons == 1 && buttons.changed == 0);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Up, 1);
	assert(buttons.buttons == 0 && buttons.changed == 1);
	assert(bridge.NormalizeWheel(60, 120, false) == 0);
	assert(bridge.NormalizeWheel(60, 120, false) == 1);
	assert(bridge.NormalizeWheel(-60, 120, true) == 0);
	assert(bridge.NormalizeWheel(-60, 120, true) == -1);

	const auto released = bridge.ApplyPointerPolicy(0, 0, 0, true, true);
	assert(released.leavingPolicy && !released.ownsPointer && released.showCursor);
	const auto recaptured = bridge.ApplyPointerPolicy(1, 0, 0, true, true);
	assert(recaptured.ownsPointer);
	motion = bridge.UpdatePosition({10, 10}, {}, false);
	assert(motion.delta == Frontend::CemuExtendPoint{});
	motion = bridge.UpdatePosition({13, 8}, {}, false);
	assert((motion.delta == Frontend::CemuExtendPoint{3, -2}));

	assert(bridge.BeginTextInput(7));
	assert(!bridge.BeginTextInput(7));
	bridge.SetPreedit("abc");
	assert(!bridge.CanSubmitText());
	const auto composition = bridge.ComposeText("committed", 4);
	assert(composition.preedit == "abc" && composition.selectionLength == 3);
	assert(bridge.BeginTextInput(8));
	assert(bridge.CanSubmitText() && !bridge.HasPreedit());
	bridge.EndTextInput();
	assert(bridge.CanSubmitText() && bridge.TextInputSequence() == 0);
}
