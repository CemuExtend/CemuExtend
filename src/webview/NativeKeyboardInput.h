#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace WebFrontend
{
	// GTK/XWayland can transiently report an inactive toplevel while it is still
	// delivering physical keyboard input to the game window. A held native key
	// is stronger evidence of gameplay focus than that stale window sample.
	// Explicit FocusLost remains authoritative and resets this tracker.
	class NativeKeyboardFocusTracker
	{
	  public:
		[[nodiscard]] bool SetKey(std::uint16_t usage, bool pressed)
		{
			if (usage >= m_pressed.size() || m_pressed[usage] == pressed)
				return false;
			m_pressed[usage] = pressed;
			if (pressed)
				++m_pressedCount;
			else
				--m_pressedCount;
			return true;
		}

		void Reset()
		{
			m_pressed.fill(false);
			m_pressedCount = 0;
		}

		[[nodiscard]] bool HasPressedKeys() const
		{
			return m_pressedCount != 0;
		}

		[[nodiscard]] bool EffectivePointerFocus(bool reportedFocused) const
		{
			return reportedFocused || HasPressedKeys();
		}

	  private:
		std::array<bool, 256> m_pressed{};
		std::size_t m_pressedCount{};
	};

	// Native window systems do not agree on the value carried by a key event.
	// Keep the small pieces of platform normalization which must happen before
	// WebFrontend sees the event here, where they can be exercised on every host.
	[[nodiscard]] constexpr std::uint16_t WindowsModifierUsbHidUsage(
		std::uint32_t virtualKey, std::uint32_t scanCode, bool extended)
	{
		constexpr std::uint32_t VkShift = 0x10;
		constexpr std::uint32_t VkControl = 0x11;
		constexpr std::uint32_t VkMenu = 0x12;
		constexpr std::uint32_t VkLeftShift = 0xa0;
		constexpr std::uint32_t VkRightShift = 0xa1;
		constexpr std::uint32_t VkLeftControl = 0xa2;
		constexpr std::uint32_t VkRightControl = 0xa3;
		constexpr std::uint32_t VkLeftMenu = 0xa4;
		constexpr std::uint32_t VkRightMenu = 0xa5;

		switch (virtualKey)
		{
		case VkShift:
			return scanCode == 0x36 ? 0xe5 : 0xe1;
		case VkControl:
			return extended ? 0xe4 : 0xe0;
		case VkMenu:
			return extended ? 0xe6 : 0xe2;
		case VkLeftShift:
			return 0xe1;
		case VkRightShift:
			return 0xe5;
		case VkLeftControl:
			return 0xe0;
		case VkRightControl:
			return 0xe4;
		case VkLeftMenu:
			return 0xe2;
		case VkRightMenu:
			return 0xe6;
		default:
			return 0;
		}
	}

	[[nodiscard]] constexpr std::uint16_t MacKeyCodeUsbHidUsage(std::uint32_t keyCode)
	{
		switch (keyCode)
		{
		case 0x00: return 0x04;
		case 0x0b: return 0x05;
		case 0x08: return 0x06;
		case 0x02: return 0x07;
		case 0x0e: return 0x08;
		case 0x03: return 0x09;
		case 0x05: return 0x0a;
		case 0x04: return 0x0b;
		case 0x22: return 0x0c;
		case 0x26: return 0x0d;
		case 0x28: return 0x0e;
		case 0x25: return 0x0f;
		case 0x2e: return 0x10;
		case 0x2d: return 0x11;
		case 0x1f: return 0x12;
		case 0x23: return 0x13;
		case 0x0c: return 0x14;
		case 0x0f: return 0x15;
		case 0x01: return 0x16;
		case 0x11: return 0x17;
		case 0x20: return 0x18;
		case 0x09: return 0x19;
		case 0x0d: return 0x1a;
		case 0x07: return 0x1b;
		case 0x10: return 0x1c;
		case 0x06: return 0x1d;
		case 0x12: return 0x1e;
		case 0x13: return 0x1f;
		case 0x14: return 0x20;
		case 0x15: return 0x21;
		case 0x17: return 0x22;
		case 0x16: return 0x23;
		case 0x1a: return 0x24;
		case 0x1c: return 0x25;
		case 0x19: return 0x26;
		case 0x1d: return 0x27;
		case 0x24: return 0x28;
		case 0x35: return 0x29;
		case 0x33: return 0x2a;
		case 0x30: return 0x2b;
		case 0x31: return 0x2c;
		case 0x1b: return 0x2d;
		case 0x18: return 0x2e;
		case 0x21: return 0x2f;
		case 0x1e: return 0x30;
		case 0x2a: return 0x31;
		case 0x29: return 0x33;
		case 0x27: return 0x34;
		case 0x32: return 0x35;
		case 0x2b: return 0x36;
		case 0x2f: return 0x37;
		case 0x2c: return 0x38;
		case 0x7a: return 0x3a;
		case 0x78: return 0x3b;
		case 0x63: return 0x3c;
		case 0x76: return 0x3d;
		case 0x60: return 0x3e;
		case 0x61: return 0x3f;
		case 0x62: return 0x40;
		case 0x64: return 0x41;
		case 0x65: return 0x42;
		case 0x6d: return 0x43;
		case 0x67: return 0x44;
		case 0x6f: return 0x45;
		case 0x72: return 0x49;
		case 0x73: return 0x4a;
		case 0x74: return 0x4b;
		case 0x75: return 0x4c;
		case 0x77: return 0x4d;
		case 0x79: return 0x4e;
		case 0x7c: return 0x4f;
		case 0x7b: return 0x50;
		case 0x7d: return 0x51;
		case 0x7e: return 0x52;
		case 0x3b:
			return 0xe0;
		case 0x38:
			return 0xe1;
		case 0x3a:
			return 0xe2;
		case 0x37:
			return 0xe3;
		case 0x3e:
			return 0xe4;
		case 0x3c:
			return 0xe5;
		case 0x3d:
			return 0xe6;
		case 0x36:
			return 0xe7;
		default:
			return 0;
		}
	}

	[[nodiscard]] constexpr std::uint16_t MacModifierUsbHidUsage(std::uint32_t keyCode)
	{
		const auto usage = MacKeyCodeUsbHidUsage(keyCode);
		return usage >= 0xe0 && usage <= 0xe7 ? usage : 0;
	}

	[[nodiscard]] constexpr std::uint8_t MacModifierBit(std::uint16_t usage)
	{
		return usage >= 0xe0 && usage <= 0xe7
			? static_cast<std::uint8_t>(1U << (usage - 0xe0))
			: 0;
	}

	[[nodiscard]] constexpr std::uint8_t MacModifierFamilyBits(std::uint16_t usage)
	{
		switch (usage)
		{
		case 0xe0:
		case 0xe4:
			return (1U << 0) | (1U << 4);
		case 0xe1:
		case 0xe5:
			return (1U << 1) | (1U << 5);
		case 0xe2:
		case 0xe6:
			return (1U << 2) | (1U << 6);
		case 0xe3:
		case 0xe7:
			return (1U << 3) | (1U << 7);
		default:
			return 0;
		}
	}

	[[nodiscard]] constexpr bool MacModifierPressed(
		std::uint8_t pressedKeys, std::uint16_t usage, std::uint8_t modifiers)
	{
		const auto bit = MacModifierBit(usage);
		const auto family = MacModifierFamilyBits(usage);
		if (!bit || !family)
			return false;
		const auto modifierFlag = static_cast<std::uint8_t>(1U << ((usage - 0xe0) & 3U));
		if ((modifiers & modifierFlag) == 0)
			return false;
		if ((pressedKeys & bit) == 0)
			return true;
		// If the family is still active while this side was already held, this
		// event is its release only when the opposite side remains held.
		return (pressedKeys & static_cast<std::uint8_t>(family & ~bit)) == 0;
	}

	// GDK's keyval includes the active Shift/AltGr level. The caller translates
	// the hardware keycode with an empty modifier state first, then maps that
	// stable base keyval here. This prevents Shift+1 ('!') and similar chords
	// from disappearing as usage 0, and makes press/release use the same usage.
	[[nodiscard]] constexpr std::uint16_t XkbBaseKeyvalUsbHidUsage(std::uint32_t key)
	{
		if (key >= 'A' && key <= 'Z')
			return static_cast<std::uint16_t>(0x04 + key - 'A');
		if (key >= 'a' && key <= 'z')
			return static_cast<std::uint16_t>(0x04 + key - 'a');
		if (key >= '1' && key <= '9')
			return static_cast<std::uint16_t>(0x1e + key - '1');
		if (key == '0')
			return 0x27;
		switch (key)
		{
		case 0xff0d: return 0x28;
		case 0xff1b: return 0x29;
		case 0xff08: return 0x2a;
		case 0xff09: return 0x2b;
		case 0x20: return 0x2c;
		case '-': return 0x2d;
		case '=': return 0x2e;
		case '[': return 0x2f;
		case ']': return 0x30;
		case '\\': return 0x31;
		case ';': return 0x33;
		case '\'': return 0x34;
		case '`': return 0x35;
		case ',': return 0x36;
		case '.': return 0x37;
		case '/': return 0x38;
		case 0xffe5: return 0x39;
		case 0xff63: return 0x49;
		case 0xff50: return 0x4a;
		case 0xff55: return 0x4b;
		case 0xffff: return 0x4c;
		case 0xff57: return 0x4d;
		case 0xff56: return 0x4e;
		case 0xff53: return 0x4f;
		case 0xff51: return 0x50;
		case 0xff54: return 0x51;
		case 0xff52: return 0x52;
		case 0xffe3: return 0xe0;
		case 0xffe1: return 0xe1;
		case 0xffe9: return 0xe2;
		case 0xffeb: return 0xe3;
		case 0xffe4: return 0xe4;
		case 0xffe2: return 0xe5;
		case 0xffea: return 0xe6;
		case 0xffec: return 0xe7;
		default:
			if (key >= 0xffbe && key <= 0xffc9)
				return static_cast<std::uint16_t>(0x3a + key - 0xffbe);
			if (key >= 0xffca && key <= 0xffd5)
				return static_cast<std::uint16_t>(0x68 + key - 0xffca);
			return 0;
		}
	}
} // namespace WebFrontend
