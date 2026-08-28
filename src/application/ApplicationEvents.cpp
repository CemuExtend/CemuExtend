#include "application/ApplicationEvents.h"

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Application
{
	namespace Detail
	{
		struct EventState
		{
			std::mutex mutex;
			std::uint64_t nextId{1};
			std::unordered_map<std::uint64_t, ApplicationEvents::Handler> handlers;
		};
	} // namespace Detail

	EventSubscription::EventSubscription(std::weak_ptr<Detail::EventState> state,
										 std::uint64_t id) : m_state(std::move(state)), m_id(id) {}

	EventSubscription::EventSubscription(EventSubscription&& other) noexcept
		: m_state(std::move(other.m_state)), m_id(std::exchange(other.m_id, 0)) {}

	EventSubscription& EventSubscription::operator=(EventSubscription&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_state = std::move(other.m_state);
			m_id = std::exchange(other.m_id, 0);
		}
		return *this;
	}

	EventSubscription::~EventSubscription()
	{
		Reset();
	}

	void EventSubscription::Reset()
	{
		if (m_id == 0)
			return;
		if (const auto state = m_state.lock())
		{
			std::scoped_lock lock(state->mutex);
			state->handlers.erase(m_id);
		}
		m_id = 0;
		m_state.reset();
	}

	ApplicationEvents::ApplicationEvents() : m_state(std::make_shared<Detail::EventState>()) {}

	EventSubscription ApplicationEvents::Subscribe(Handler handler)
	{
		std::scoped_lock lock(m_state->mutex);
		const auto id = m_state->nextId++;
		m_state->handlers.emplace(id, std::move(handler));
		return {m_state, id};
	}

	void ApplicationEvents::Publish(const Event& event) const
	{
		std::vector<Handler> handlers;
		{
			std::scoped_lock lock(m_state->mutex);
			handlers.reserve(m_state->handlers.size());
			for (const auto& [_, handler] : m_state->handlers)
				handlers.push_back(handler);
		}
		for (const auto& handler : handlers)
			handler(event);
	}
} // namespace Application
