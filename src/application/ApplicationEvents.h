#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Application
{
	enum class EventType : std::uint8_t
	{
		LoadingStarted,
		GameLoaded,
		GameExited,
		PpcProcessExited,
		PerformanceUpdated,
		Diagnostic,
		GameListRefreshRequested,
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

	namespace Detail { struct EventState; }

	class EventSubscription final
	{
	public:
		EventSubscription() = default;
		EventSubscription(EventSubscription&& other) noexcept;
		EventSubscription& operator=(EventSubscription&& other) noexcept;
		~EventSubscription();

		EventSubscription(const EventSubscription&) = delete;
		EventSubscription& operator=(const EventSubscription&) = delete;
		void Reset();

	private:
		friend class ApplicationEvents;
		EventSubscription(std::weak_ptr<Detail::EventState> state, std::uint64_t id);
		std::weak_ptr<Detail::EventState> m_state;
		std::uint64_t m_id{};
	};

	class ApplicationEvents final
	{
	public:
		using Handler = std::function<void(const Event&)>;
		ApplicationEvents();
		[[nodiscard]] EventSubscription Subscribe(Handler handler);
		void Publish(const Event& event) const;

	private:
		std::shared_ptr<Detail::EventState> m_state;
	};
}
