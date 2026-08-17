#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Frontend
{
	struct CemuExtendPoint
	{
		std::int32_t x{};
		std::int32_t y{};

		friend bool operator==(const CemuExtendPoint&, const CemuExtendPoint&) = default;
	};

	enum class CemuExtendMouseTransition : std::uint8_t
	{
		None,
		Down,
		Up,
		Aggregate,
	};

	struct CemuExtendButtonUpdate
	{
		std::uint32_t buttons{};
		std::uint32_t changed{};
	};

	struct CemuExtendMotionUpdate
	{
		CemuExtendPoint delta;
		bool rawRelative{};
	};

	struct CemuExtendPointerDecision
	{
		bool ownsPointer{};
		bool showCursor{true};
		bool confine{};
		bool enteringCapture{};
		bool leavingPolicy{};
		bool requestRawMouse{};
		std::uint8_t mode{};
		std::uint8_t cursor{};
	};

	struct CemuExtendTextComposition
	{
		std::string committed;
		std::string preedit;
		std::uint32_t cursor{};
		std::uint32_t selectionLength{};
	};

	// Owns frontend-neutral CEX2 input state. wx code is responsible only for
	// obtaining native events and applying the returned cursor/capture decision.
	class CemuExtendFrontendBridge final
	{
	public:
		[[nodiscard]] CemuExtendPointerDecision ApplyPointerPolicy(std::uint8_t mode,
			std::uint8_t cursor, std::uint32_t flags, bool appActive, bool hasCanvas);
		[[nodiscard]] CemuExtendButtonUpdate UpdateButtons(CemuExtendMouseTransition transition,
			std::uint32_t changedMask, std::uint32_t aggregateButtons = 0);
		[[nodiscard]] CemuExtendMotionUpdate UpdatePosition(CemuExtendPoint position,
			CemuExtendPoint captureCenter, bool rawMouseAvailable);
		void RecordRawPosition(CemuExtendPoint position);
		void MarkRawMouseSeen();
		void ResetPointerPosition();
		[[nodiscard]] std::int32_t NormalizeWheel(std::int32_t rotation,
			std::int32_t reportedDelta, bool horizontal);

		[[nodiscard]] std::uint8_t PointerMode() const { return m_pointerMode; }
		[[nodiscard]] std::uint32_t MouseButtons() const { return m_mouseButtons; }
		[[nodiscard]] bool RawMouseRequested() const { return m_rawMouseRequested; }
		[[nodiscard]] bool RawMouseSeen() const { return m_rawMouseSeen; }

		[[nodiscard]] bool BeginTextInput(std::uint64_t sequence);
		void EndTextInput();
		void SetPreedit(std::string_view preedit);
		[[nodiscard]] bool CanSubmitText() const { return m_preedit.empty(); }
		[[nodiscard]] bool HasPreedit() const { return !m_preedit.empty(); }
		[[nodiscard]] std::uint64_t TextInputSequence() const { return m_textInputSequence; }
		[[nodiscard]] CemuExtendTextComposition ComposeText(std::string committed,
			std::uint32_t cursor) const;

	private:
		CemuExtendPoint m_lastPosition;
		bool m_positionValid{};
		std::uint8_t m_pointerMode{};
		std::uint32_t m_mouseButtons{};
		std::int32_t m_wheelRemainderX{};
		std::int32_t m_wheelRemainderY{};
		bool m_rawMouseRequested{true};
		bool m_rawMouseSeen{};
		bool m_suppressNextCapturedMotion{};
		std::uint64_t m_textInputSequence{};
		std::string m_preedit;
	};
}
