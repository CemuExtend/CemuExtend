#include "webview/NativeKeyboardInput.h"

#include <cassert>

int main()
{
	using namespace WebFrontend;
	NativeKeyboardFocusTracker focus;
	assert(!focus.HasPressedKeys());
	assert(!focus.EffectivePointerFocus(false));
	bool cachedWindowFocus{};
	assert(focus.SetKey(0x1a, true));
	// ConfirmNativeKeyboardFocus stores this effective gain immediately. A
	// periodic WindowGet before the next GTK metrics callback must see true.
	cachedWindowFocus = focus.EffectivePointerFocus(cachedWindowFocus);
	assert(cachedWindowFocus);
	assert(!focus.SetKey(0x1a, true));
	assert(focus.SetKey(0x04, true));
	assert(focus.HasPressedKeys());
	// A transient false mouse/window sample while W+A is held must not release
	// either gameplay input. Releasing A leaves W authoritative.
	assert(focus.EffectivePointerFocus(false));
	assert(focus.SetKey(0x04, false));
	assert(focus.HasPressedKeys());
	assert(focus.EffectivePointerFocus(false));
	assert(focus.SetKey(0x1a, false));
	assert(!focus.HasPressedKeys());
	assert(!focus.EffectivePointerFocus(false));
	assert(focus.SetKey(0x1a, true));
	assert(focus.SetKey(0x07, true));
	// HandleMetrics must cache the effective value for WindowGet rather than
	// publishing a transient GTK false behind the live MouseV2 correction.
	cachedWindowFocus = focus.EffectivePointerFocus(false);
	assert(cachedWindowFocus);
	// Explicit native focus loss (alt-tab/window close) is authoritative.
	focus.Reset();
	cachedWindowFocus = focus.EffectivePointerFocus(false);
	assert(!focus.HasPressedKeys());
	assert(!cachedWindowFocus);

	assert(WindowsModifierUsbHidUsage(0x10, 0x2a, false) == 0xe1);
	assert(WindowsModifierUsbHidUsage(0x10, 0x36, false) == 0xe5);
	assert(WindowsModifierUsbHidUsage(0x11, 0x1d, false) == 0xe0);
	assert(WindowsModifierUsbHidUsage(0x11, 0x1d, true) == 0xe4);
	assert(WindowsModifierUsbHidUsage(0x12, 0x38, false) == 0xe2);
	assert(WindowsModifierUsbHidUsage(0x12, 0x38, true) == 0xe6);
	assert(WindowsModifierUsbHidUsage('W', 0x11, false) == 0);
	assert(WindowsConsumerUsbHidUsage(0xaf) == 0x00e9);
	assert(WindowsConsumerUsbHidUsage(0xb3) == 0x00cd);

	assert(MacModifierUsbHidUsage(0x38) == 0xe1);
	assert(MacModifierUsbHidUsage(0x3c) == 0xe5);
	assert(MacModifierUsbHidUsage(0x3b) == 0xe0);
	assert(MacModifierUsbHidUsage(0x3e) == 0xe4);
	assert(MacKeyCodeUsbHidUsage(0x00) == 0x04);
	assert(MacKeyCodeUsbHidUsage(0x0d) == 0x1a);
	// These hardware codes numerically collide with ASCII punctuation. They
	// must be mapped as macOS keycodes before any logical-key fallback.
	assert(MacKeyCodeUsbHidUsage(0x2d) == 0x11);
	assert(MacKeyCodeUsbHidUsage(0x2e) == 0x10);
	assert(MacModifierBit(0xe5) == (1U << 5));
	assert(MacModifierBit(0x04) == 0);
	assert(MacModifierPressed(0, 0xe1, 2));
	assert(MacModifierPressed(1U << 1, 0xe1, 2));
	assert(!MacModifierPressed((1U << 1) | (1U << 5), 0xe1, 2));
	assert(!MacModifierPressed(1U << 1, 0xe1, 0));

	assert(XkbBaseKeyvalUsbHidUsage('w') == 0x1a);
	assert(XkbBaseKeyvalUsbHidUsage('1') == 0x1e);
	assert(XkbBaseKeyvalUsbHidUsage('!') == 0);
	assert(XkbBaseKeyvalUsbHidUsage(0xffe2) == 0xe5);
	assert(XkbBaseKeyvalUsbHidUsage(0xffc2) == 0x3e);
	assert(XkbConsumerUsbHidUsage(0x1008ff13) == 0x00e9);
	assert(XkbConsumerUsbHidUsage(0x1008ff17) == 0x00b5);
	assert(MacConsumerUsbHidUsage(0x48) == 0x00e9);
}
