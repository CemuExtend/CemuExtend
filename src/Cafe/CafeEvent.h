#pragma once

#include <cstdint>
#include <string>

namespace CafeSystem
{
	enum class EventType : std::uint8_t
	{
		LoadingStarted,
		GameLoaded,
		GameExited,
		PpcProcessExited,
		PerformanceUpdated,
		Diagnostic,
	};

	enum class DiagnosticCode : std::uint8_t
	{
		DamagedExecutable,
		KeyFileCreateFailed,
		KeyFileInvalidLine,
		GraphicPackInvalid,
		MemoryAllocationFailed,
		MemoryReservationFailed,
	};

	struct Event
	{
		EventType type{};
		DiagnosticCode diagnosticCode{};
		std::int32_t processStatus{};
		double framesPerSecond{};
		std::string diagnostic;
	};

	class IEventSink
	{
	  public:
		virtual ~IEventSink() = default;
		// Called on the emitting core thread and sometimes while a Cafe lifecycle
		// transition is active. Implementations must only enqueue/copy the event;
		// they must not synchronously re-enter CafeSystem.
		virtual void OnCafeEvent(const Event& event) = 0;
	};
} // namespace CafeSystem
