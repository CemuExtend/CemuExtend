#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "cemuextend/services.hpp"

namespace Host { class IClipboard; class IWindowMetrics; }

namespace cemuextend_hle { class Cex2Owner; }
struct VPADStatus;

namespace cemuextend_hle {

struct Cex2HostTextInputState
{
	bool active{};
	std::uint64_t sequence{};
	std::uint32_t requestId{};
	std::uint32_t maximumLength{};
	std::int32_t caretX{};
	std::int32_t caretY{};
	std::int32_t lineHeight{};
	std::string initialText;
};

class Cex2Host
{
public:
	static Cex2Host& Instance();
	void ConfigureHost(std::shared_ptr<Host::IClipboard> clipboard,
		std::shared_ptr<Host::IWindowMetrics> windowMetrics);

	std::int32_t Query(Cex2Owner& owner, std::uint32_t query,
		std::span<std::byte> output);
	std::int32_t Open(Cex2Owner& owner, std::span<const std::byte> options,
		std::uint32_t& sessionId);
	std::int32_t Submit(Cex2Owner& owner, std::uint32_t sessionId,
		std::span<const std::byte> request);
	std::int32_t Poll(Cex2Owner& owner, std::uint32_t sessionId,
		std::span<std::byte> output, std::uint32_t& outputSize);
	std::int32_t Cancel(Cex2Owner& owner, std::uint32_t sessionId,
		std::uint32_t correlationId);
	std::int32_t Close(Cex2Owner& owner, std::uint32_t sessionId);
	void CloseOwner(Cex2Owner& owner);
	void CloseAll();
	void ObserveVpad(std::int32_t channel, const VPADStatus& status, std::int32_t error,
		std::int32_t sampleCount);
	void ApplyMappedVpad(std::int32_t channel, VPADStatus& status);
	void KeyboardEvent(std::uint16_t usbHidUsage, bool pressed, std::uint8_t modifiers);
	void TextEvent(std::uint32_t codepoint, bool repeat);
	[[nodiscard]] Cex2HostTextInputState EffectiveTextInput();
	void TextCompositionEvent(std::string_view text,
		std::string_view preedit = {}, std::uint32_t preeditStart = 0,
		std::uint32_t preeditCursor = 0);
	void SetTextInputWakeCallback(void (*callback)());
	void KeyboardFocusLost();
	void MouseEvent(cemuextend::wire::PointerSurface surface,
		std::int32_t x, std::int32_t y, std::int32_t deltaX, std::int32_t deltaY,
		std::int32_t wheelX, std::int32_t wheelY, std::uint32_t buttons,
		std::uint32_t changedButtons, std::int32_t contentWidth,
		std::int32_t contentHeight, bool insideContent, bool focused,
		std::uint8_t flags = 0);
	void PointerFocusChanged(bool focused);
	[[nodiscard]] cemuextend::wire::PointerPolicyPayload EffectivePointerPolicy();
	void PermissionsChanged(Cex2Owner& owner, std::uint32_t permissions);
#ifdef CEMU_CEX2_TESTING
	void ShutdownForTesting();
#endif

private:
	struct Impl;
	Cex2Host();
	~Cex2Host();
	Cex2Host(const Cex2Host&) = delete;
	Cex2Host& operator=(const Cex2Host&) = delete;
	std::shared_ptr<Impl> m_impl;
};

} // namespace cemuextend_hle
