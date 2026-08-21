#pragma once

#include "Cemu/Logging/LoggingCallbacks.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Application
{
	enum class LoggingLevel : std::uint8_t
	{
		Info,
		Warning,
		Error,
	};

	struct LoggingEntry
	{
		std::uint64_t sequence{};
		LoggingLevel level{};
		std::string category;
		std::string message;
	};

	struct LoggingSnapshot
	{
		std::vector<LoggingEntry> entries;
		std::uint64_t firstAvailableSequence{};
		std::uint64_t nextSequence{};
		std::uint64_t droppedEntries{};
		std::size_t retainedBytes{};
		bool truncated{};
	};

	namespace Detail { struct LoggingState; }

	class LoggingSubscription final
	{
	public:
		LoggingSubscription() = default;
		LoggingSubscription(LoggingSubscription&& other) noexcept;
		LoggingSubscription& operator=(LoggingSubscription&& other) noexcept;
		~LoggingSubscription();

		LoggingSubscription(const LoggingSubscription&) = delete;
		LoggingSubscription& operator=(const LoggingSubscription&) = delete;
		void Reset();

	private:
		friend class LoggingFacade;
		LoggingSubscription(std::weak_ptr<Detail::LoggingState> state, std::uint64_t id);
		std::weak_ptr<Detail::LoggingState> m_state;
		std::uint64_t m_id{};
	};

	// Owns the only Cemu logging callback used by the web frontend. Values are copied
	// into a bounded buffer; callers never receive a core global or borrowed pointer.
	class LoggingFacade final : private LoggingCallbacks
	{
	public:
		using Handler = std::function<void(const LoggingEntry&)>;
		static constexpr std::size_t MaximumEntries = 2048;
		static constexpr std::size_t MaximumBytes = 1024 * 1024;
		static constexpr std::size_t MaximumEntryBytes = 8 * 1024;

		LoggingFacade();
		~LoggingFacade() override;

		LoggingFacade(const LoggingFacade&) = delete;
		LoggingFacade& operator=(const LoggingFacade&) = delete;

		[[nodiscard]] LoggingSubscription Subscribe(Handler handler);
		[[nodiscard]] LoggingSnapshot Snapshot(std::uint64_t afterSequence = 0,
			std::size_t maximumEntries = MaximumEntries) const;
		// Atomically clears retained entries and returns the last sequence covered.
		[[nodiscard]] std::uint64_t Clear();

	private:
		void Log(std::string_view category, std::string_view message) override;
		void Log(std::string_view category, std::wstring_view message) override;
		void Publish(std::string category, std::string message);
		std::shared_ptr<Detail::LoggingState> m_state;
	};
}
