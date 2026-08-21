#include "application/LoggingFacade.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace Application
{
	namespace
	{
		std::string BoundedCopy(std::string_view value, std::size_t maximum)
		{
			if (value.size() <= maximum)
				return std::string(value);
			constexpr std::string_view suffix = "... [truncated]";
			std::string result(value.substr(0, maximum - suffix.size()));
			result += suffix;
			return result;
		}

		void AppendUtf8(std::string& output, std::uint32_t codepoint)
		{
			if (codepoint <= 0x7f) output.push_back(static_cast<char>(codepoint));
			else if (codepoint <= 0x7ff)
			{
				output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
				output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
			}
			else if (codepoint <= 0xffff)
			{
				output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
				output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
				output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
			}
			else
			{
				output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
				output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
				output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
				output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
			}
		}

		std::string WideToUtf8(std::wstring_view value)
		{
			std::string result;
			result.reserve(std::min(value.size() * 2, LoggingFacade::MaximumEntryBytes));
			for (std::size_t index = 0; index < value.size() &&
				result.size() < LoggingFacade::MaximumEntryBytes; ++index)
			{
				std::uint32_t codepoint = static_cast<std::uint32_t>(value[index]);
				if constexpr (sizeof(wchar_t) == 2)
				{
					if (codepoint >= 0xd800 && codepoint <= 0xdbff && index + 1 < value.size())
					{
						const auto low = static_cast<std::uint32_t>(value[index + 1]);
						if (low >= 0xdc00 && low <= 0xdfff)
						{
							codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
							++index;
						}
					}
				}
				if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
					codepoint = 0xfffd;
				AppendUtf8(result, codepoint);
			}
			return BoundedCopy(result, LoggingFacade::MaximumEntryBytes);
		}

		LoggingLevel Classify(std::string_view message)
		{
			std::string prefix(message.substr(0, std::min<std::size_t>(message.size(), 96)));
			std::ranges::transform(prefix, prefix.begin(),
				[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
			if (prefix.find("error") != std::string::npos || prefix.find("failed") != std::string::npos ||
				prefix.find("fatal") != std::string::npos)
				return LoggingLevel::Error;
			if (prefix.find("warning") != std::string::npos || prefix.find("warn:") != std::string::npos)
				return LoggingLevel::Warning;
			return LoggingLevel::Info;
		}
	}

	namespace Detail
	{
		struct LoggingState
		{
			std::mutex mutex;
			std::deque<LoggingEntry> entries;
			std::unordered_map<std::uint64_t, LoggingFacade::Handler> handlers;
			std::uint64_t nextHandlerId{1};
			std::uint64_t nextSequence{1};
			std::uint64_t droppedEntries{};
			std::size_t retainedBytes{};
		};
	}

	LoggingSubscription::LoggingSubscription(std::weak_ptr<Detail::LoggingState> state,
		std::uint64_t id) : m_state(std::move(state)), m_id(id) {}

	LoggingSubscription::LoggingSubscription(LoggingSubscription&& other) noexcept
		: m_state(std::move(other.m_state)), m_id(std::exchange(other.m_id, 0)) {}

	LoggingSubscription& LoggingSubscription::operator=(LoggingSubscription&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_state = std::move(other.m_state);
			m_id = std::exchange(other.m_id, 0);
		}
		return *this;
	}

	LoggingSubscription::~LoggingSubscription() { Reset(); }

	void LoggingSubscription::Reset()
	{
		if (m_id == 0) return;
		if (const auto state = m_state.lock())
		{
			std::scoped_lock lock(state->mutex);
			state->handlers.erase(m_id);
		}
		m_id = 0;
		m_state.reset();
	}

	LoggingFacade::LoggingFacade() : m_state(std::make_shared<Detail::LoggingState>())
	{
		cemuLog_setCallbacks(this);
	}

	LoggingFacade::~LoggingFacade()
	{
		cemuLog_clearCallbacks();
	}

	LoggingSubscription LoggingFacade::Subscribe(Handler handler)
	{
		if (!handler) return {};
		std::scoped_lock lock(m_state->mutex);
		const auto id = m_state->nextHandlerId++;
		m_state->handlers.emplace(id, std::move(handler));
		return {m_state, id};
	}

	LoggingSnapshot LoggingFacade::Snapshot(std::uint64_t afterSequence,
		std::size_t maximumEntries) const
	{
		std::scoped_lock lock(m_state->mutex);
		LoggingSnapshot result;
		result.nextSequence = m_state->nextSequence;
		result.droppedEntries = m_state->droppedEntries;
		result.retainedBytes = m_state->retainedBytes;
		result.firstAvailableSequence = m_state->entries.empty() ? m_state->nextSequence :
			m_state->entries.front().sequence;
		maximumEntries = std::min(maximumEntries, MaximumEntries);
		for (const auto& entry : m_state->entries)
		{
			if (entry.sequence <= afterSequence) continue;
			if (result.entries.size() == maximumEntries)
			{
				result.truncated = true;
				break;
			}
			result.entries.push_back(entry);
		}
		return result;
	}

	std::uint64_t LoggingFacade::Clear()
	{
		std::scoped_lock lock(m_state->mutex);
		const auto clearedThroughSequence = m_state->nextSequence - 1;
		m_state->entries.clear();
		m_state->retainedBytes = 0;
		return clearedThroughSequence;
	}

	void LoggingFacade::Log(std::string_view category, std::string_view message)
	{
		Publish(BoundedCopy(category, 256), BoundedCopy(message, MaximumEntryBytes));
	}

	void LoggingFacade::Log(std::string_view category, std::wstring_view message)
	{
		Publish(BoundedCopy(category, 256), WideToUtf8(message));
	}

	void LoggingFacade::Publish(std::string category, std::string message)
	{
		std::vector<Handler> handlers;
		LoggingEntry published;
		{
			std::scoped_lock lock(m_state->mutex);
			published = {m_state->nextSequence++, Classify(message), std::move(category),
				std::move(message)};
			const auto bytes = published.category.size() + published.message.size();
			m_state->entries.push_back(published);
			m_state->retainedBytes += bytes;
			while (m_state->entries.size() > MaximumEntries ||
				m_state->retainedBytes > MaximumBytes)
			{
				const auto& oldest = m_state->entries.front();
				m_state->retainedBytes -= oldest.category.size() + oldest.message.size();
				m_state->entries.pop_front();
				++m_state->droppedEntries;
			}
			handlers.reserve(m_state->handlers.size());
			for (const auto& [_, handler] : m_state->handlers) handlers.push_back(handler);
		}
		for (const auto& handler : handlers) handler(published);
	}
}
