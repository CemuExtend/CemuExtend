#include "frontend/CemuExtendFrontendBridge.h"

#include <algorithm>

namespace Frontend
{
	namespace
	{
		constexpr std::uint8_t kDefaultPointerMode = 0;
		constexpr std::uint8_t kVisibleAbsolutePointerMode = 1;
		constexpr std::uint8_t kCapturedRelativePointerMode = 3;
		constexpr std::uint32_t kDisableRawMouse = 1U << 1U;
		constexpr std::uint32_t kConfineToContent = 1U << 2U;
	}

	CemuExtendPointerDecision CemuExtendFrontendBridge::ApplyPointerPolicy(
		std::uint8_t mode, std::uint8_t cursor, std::uint32_t flags,
		bool appActive, bool hasCanvas)
	{
		const auto previousMode = m_pointerMode;
		m_pointerMode = mode;
		m_rawMouseRequested = (flags & kDisableRawMouse) == 0;

		CemuExtendPointerDecision decision;
		decision.mode = mode;
		decision.cursor = cursor;
		decision.ownsPointer = mode != kDefaultPointerMode;
		decision.showCursor = mode == kDefaultPointerMode ||
			mode == kVisibleAbsolutePointerMode;
		decision.confine = decision.ownsPointer && (flags & kConfineToContent) != 0 &&
			appActive && hasCanvas;
		decision.enteringCapture = mode == kCapturedRelativePointerMode &&
			previousMode != kCapturedRelativePointerMode && hasCanvas;
		decision.leavingPolicy = mode == kDefaultPointerMode &&
			previousMode != kDefaultPointerMode;
		decision.requestRawMouse = decision.enteringCapture && m_rawMouseRequested;

		if (decision.leavingPolicy)
			ResetPointerPosition();
		if (mode == kDefaultPointerMode)
			m_suppressNextCapturedMotion = false;
		if (decision.enteringCapture)
		{
			m_rawMouseSeen = false;
			m_suppressNextCapturedMotion = true;
		}
		return decision;
	}

	CemuExtendButtonUpdate CemuExtendFrontendBridge::UpdateButtons(
		CemuExtendMouseTransition transition, std::uint32_t changedMask,
		std::uint32_t aggregateButtons)
	{
		auto next = m_mouseButtons;
		switch (transition)
		{
		case CemuExtendMouseTransition::Down: next |= changedMask; break;
		case CemuExtendMouseTransition::Up: next &= ~changedMask; break;
		case CemuExtendMouseTransition::Aggregate:
			next = (next & ~changedMask) | (aggregateButtons & changedMask);
			break;
		case CemuExtendMouseTransition::None: break;
		}
		const auto actualChanged = (m_mouseButtons ^ next) & changedMask;
		m_mouseButtons = next;
		return {m_mouseButtons, actualChanged};
	}

	CemuExtendMotionUpdate CemuExtendFrontendBridge::UpdatePosition(
		CemuExtendPoint position, CemuExtendPoint captureCenter, bool rawMouseAvailable)
	{
		CemuExtendMotionUpdate update;
		if (m_pointerMode == kCapturedRelativePointerMode)
		{
			if (m_suppressNextCapturedMotion)
				m_suppressNextCapturedMotion = false;
			else if (!rawMouseAvailable || !m_rawMouseSeen)
				update.delta = {position.x - captureCenter.x, position.y - captureCenter.y};
		}
		else if (m_positionValid)
			update.delta = {position.x - m_lastPosition.x, position.y - m_lastPosition.y};
		m_lastPosition = position;
		m_positionValid = true;
		update.rawRelative = m_rawMouseRequested && m_rawMouseSeen;
		return update;
	}

	void CemuExtendFrontendBridge::RecordRawPosition(CemuExtendPoint position)
	{
		m_lastPosition = position;
		m_positionValid = true;
	}

	void CemuExtendFrontendBridge::MarkRawMouseSeen()
	{
		m_rawMouseSeen = true;
	}

	void CemuExtendFrontendBridge::ResetPointerPosition()
	{
		m_positionValid = false;
	}

	std::int32_t CemuExtendFrontendBridge::NormalizeWheel(std::int32_t rotation,
		std::int32_t reportedDelta, bool horizontal)
	{
		const auto wheelDelta = reportedDelta > 0 ? reportedDelta : 120;
		auto& remainder = horizontal ? m_wheelRemainderX : m_wheelRemainderY;
		const auto accumulated = static_cast<std::int64_t>(remainder) + rotation;
		const auto steps = static_cast<std::int32_t>(accumulated / wheelDelta);
		remainder = static_cast<std::int32_t>(accumulated % wheelDelta);
		return steps;
	}

	bool CemuExtendFrontendBridge::BeginTextInput(std::uint64_t sequence)
	{
		if (sequence == m_textInputSequence)
			return false;
		m_textInputSequence = sequence;
		m_preedit.clear();
		return true;
	}

	void CemuExtendFrontendBridge::EndTextInput()
	{
		m_textInputSequence = 0;
		m_preedit.clear();
	}

	void CemuExtendFrontendBridge::SetPreedit(std::string_view preedit)
	{
		m_preedit.assign(preedit);
	}

	CemuExtendTextComposition CemuExtendFrontendBridge::ComposeText(
		std::string committed, std::uint32_t cursor) const
	{
		return {std::move(committed), m_preedit, cursor,
			static_cast<std::uint32_t>(m_preedit.size())};
	}
}
