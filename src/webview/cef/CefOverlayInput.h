#pragma once

#include "webview/NativeWindowHost.h"

#include <cstdint>

namespace WebFrontend::CefOverlay
{
	struct NativeInputRoute
	{
		bool publishGuestPhysicalInput{};
		bool processFrontendInput{true};
	};

	[[nodiscard]] constexpr bool IsPointerInput(NativeInputKind kind)
	{
		return kind == NativeInputKind::PointerMove || kind == NativeInputKind::PointerButton ||
			   kind == NativeInputKind::PointerWheel || kind == NativeInputKind::Touch ||
			   kind == NativeInputKind::RawMouse;
	}

	// Guest input subscriptions observe the physical keyboard and mouse stream.
	// An interactive browser may consume the corresponding frontend event, but
	// that must not disconnect subscribers from key releases, binding capture,
	// mouse buttons, or wheel edges. Character/composition input remains owned by
	// the focused frontend because it is already a translated text stream.
	[[nodiscard]] constexpr NativeInputRoute ResolveNativeInputRoute(
		NativeInputKind kind, bool overlayCapturesInput)
	{
		const bool publishGuest = kind == NativeInputKind::Key ||
			kind == NativeInputKind::PointerMove ||
			kind == NativeInputKind::PointerButton ||
			kind == NativeInputKind::PointerWheel ||
			kind == NativeInputKind::RawMouse ||
			kind == NativeInputKind::FocusLost;
		return {
			.publishGuestPhysicalInput = publishGuest,
			.processFrontendInput = !overlayCapturesInput || kind == NativeInputKind::DeviceChanged,
		};
	}

	// CefKeyEvent::windows_key_code uses Windows virtual-key values on every
	// platform. NativeInputEvent::key is a Win32 VK, a GDK keyval, or a macOS
	// hardware code, so it cannot be copied into that field portably. USB HID
	// usages are already normalized by WebFrontend and give the OSR browser a
	// stable mapping for navigation, shortcuts, and editable controls.
	[[nodiscard]] constexpr std::uint32_t WindowsKeyCodeFromUsbHid(std::uint16_t usage)
	{
		if (usage >= 0x04 && usage <= 0x1d)
			return 'A' + usage - 0x04;
		if (usage >= 0x1e && usage <= 0x26)
			return '1' + usage - 0x1e;
		if (usage == 0x27)
			return '0';
		if (usage >= 0x3a && usage <= 0x45)
			return 0x70 + usage - 0x3a; // F1..F12
		switch (usage)
		{
		case 0x28:
			return 0x0d; // Return
		case 0x29:
			return 0x1b; // Escape
		case 0x2a:
			return 0x08; // Backspace
		case 0x2b:
			return 0x09; // Tab
		case 0x2c:
			return 0x20; // Space
		case 0x2d:
			return 0xbd; // OEM minus
		case 0x2e:
			return 0xbb; // OEM plus
		case 0x2f:
			return 0xdb; // OEM left bracket
		case 0x30:
			return 0xdd; // OEM right bracket
		case 0x31:
			return 0xdc; // OEM backslash
		case 0x33:
			return 0xba; // OEM semicolon
		case 0x34:
			return 0xde; // OEM quote
		case 0x35:
			return 0xc0; // OEM grave
		case 0x36:
			return 0xbc; // OEM comma
		case 0x37:
			return 0xbe; // OEM period
		case 0x38:
			return 0xbf; // OEM slash
		case 0x39:
			return 0x14; // Caps Lock
		case 0x46:
			return 0x2c; // Print Screen
		case 0x47:
			return 0x91; // Scroll Lock
		case 0x48:
			return 0x13; // Pause
		case 0x49:
			return 0x2d; // Insert
		case 0x4a:
			return 0x24; // Home
		case 0x4b:
			return 0x21; // Page Up
		case 0x4c:
			return 0x2e; // Delete
		case 0x4d:
			return 0x23; // End
		case 0x4e:
			return 0x22; // Page Down
		case 0x4f:
			return 0x27; // Right
		case 0x50:
			return 0x25; // Left
		case 0x51:
			return 0x28; // Down
		case 0x52:
			return 0x26; // Up
		case 0xe0:
			return 0xa2; // Left Control
		case 0xe1:
			return 0xa0; // Left Shift
		case 0xe2:
			return 0xa4; // Left Alt
		case 0xe3:
			return 0x5b; // Left Super
		case 0xe4:
			return 0xa3; // Right Control
		case 0xe5:
			return 0xa1; // Right Shift
		case 0xe6:
			return 0xa5; // Right Alt
		case 0xe7:
			return 0x5c; // Right Super
		default:
			return 0;
		}
	}
} // namespace WebFrontend::CefOverlay
