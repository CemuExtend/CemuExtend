#pragma once

#include "cemuextend/services.hpp"

#include <cstdint>
#include <string_view>

#include "Cafe/OS/libs/cemuextend/Cex2Host.h"

struct VPADStatus;

namespace cemuextend_hle
{
	constexpr uint32 kDefaultReadMask =
		(1U << (static_cast<uint32>(cemuextend::wire::ServiceId::Core) - 1U)) |
		(1U << (static_cast<uint32>(cemuextend::wire::ServiceId::Input) - 1U)) |
		(1U << (static_cast<uint32>(cemuextend::wire::ServiceId::Configuration) - 1U)) |
		(1U << (static_cast<uint32>(cemuextend::wire::ServiceId::File) - 1U)) |
		(1U << (static_cast<uint32>(cemuextend::wire::ServiceId::Window) - 1U)) |
		(1U << (static_cast<uint32>(cemuextend::wire::ServiceId::Diagnostics) - 1U));
	constexpr uint32 kDefaultWriteMask =
		(1U << (static_cast<uint32>(cemuextend::wire::ServiceId::Input) - 1U)) |
		(1U << (static_cast<uint32>(cemuextend::wire::ServiceId::Logging) - 1U));
	constexpr uint32 kDefaultInjectMask = 0;

	inline void ObserveVpad(sint32 channel, const VPADStatus& status, sint32 error, sint32 sampleCount)
	{
		Cex2Host::Instance().ObserveVpad(channel, status, error, sampleCount);
	}

	inline void ApplyMappedVpad(sint32 channel, VPADStatus& status)
	{
		Cex2Host::Instance().ApplyMappedVpad(channel, status);
	}

	inline void KeyboardEvent(uint16 usbHidUsage, bool pressed, uint8 modifiers)
	{
		Cex2Host::Instance().KeyboardEvent(usbHidUsage, pressed, modifiers);
	}

	inline void KeyboardFocusLost()
	{
		Cex2Host::Instance().KeyboardFocusLost();
	}

	inline void TextEvent(uint32 codepoint, bool repeat)
	{
		Cex2Host::Instance().TextEvent(codepoint, repeat);
	}

	inline Cex2HostTextInputState EffectiveTextInput()
	{
		return Cex2Host::Instance().EffectiveTextInput();
	}

	inline void TextCompositionEvent(std::string_view text,
									 std::string_view preedit = {}, uint32 preeditStart = 0,
									 uint32 preeditCursor = 0)
	{
		Cex2Host::Instance().TextCompositionEvent(
			text, preedit, preeditStart, preeditCursor);
	}

	inline void MouseEvent(cemuextend::wire::PointerSurface surface,
						   sint32 x, sint32 y, sint32 deltaX, sint32 deltaY,
						   sint32 wheelX, sint32 wheelY, uint32 buttons, uint32 changedButtons,
						   sint32 contentWidth, sint32 contentHeight, bool insideContent, bool focused,
						   uint8 flags = 0)
	{
		Cex2Host::Instance().MouseEvent(surface, x, y, deltaX, deltaY,
										wheelX, wheelY, buttons, changedButtons, contentWidth, contentHeight,
										insideContent, focused, flags);
	}

	inline void PointerFocusChanged(bool focused)
	{
		Cex2Host::Instance().PointerFocusChanged(focused);
	}

	[[nodiscard]] inline cemuextend::wire::PointerPolicyPayload EffectivePointerPolicy()
	{
		return Cex2Host::Instance().EffectivePointerPolicy();
	}

} // namespace cemuextend_hle
