#pragma once

#include <cstdint>
#include <string>

// Host-only state machine for the lifetime of title-scoped trusted code. It is
// deliberately independent from PPC/MMU state so every transition can be unit
// tested without mapping an RPX.
class TrustedCemodLifecycle
{
  public:
	enum class State : std::uint8_t
	{
		Idle,
		Live,
		ReleasePending,
		ThreadsStopped,
	};

	[[nodiscard]] bool Begin(std::uint64_t titleId, std::string& error)
	{
		error.clear();
		if (titleId == 0)
		{
			error = "trusted runtime cannot begin an unknown title";
			return false;
		}
		if (m_state != State::Idle)
		{
			error = "trusted runtime still retains the previous title";
			return false;
		}
		m_titleId = titleId;
		m_state = State::Live;
		return true;
	}

	[[nodiscard]] bool Accepts(std::uint64_t titleId) const
	{
		return m_state == State::Live && m_titleId == titleId;
	}

	void RequestRelease()
	{
		if (m_state == State::Live)
			m_state = State::ReleasePending;
	}

	[[nodiscard]] bool MarkThreadsStopped(std::string& error)
	{
		error.clear();
		if (m_state == State::Idle || m_state == State::ThreadsStopped)
			return true;
		if (m_state != State::ReleasePending)
		{
			error = "trusted title shutdown was not prepared";
			return false;
		}
		m_state = State::ThreadsStopped;
		return true;
	}

	[[nodiscard]] bool CompleteRelease(std::string& error)
	{
		error.clear();
		if (m_state == State::Idle)
			return true;
		if (m_state != State::ThreadsStopped)
		{
			error = m_state == State::ReleasePending ? "trusted title PPC threads have not been marked stopped" : "trusted title shutdown was not prepared";
			return false;
		}
		m_titleId = 0;
		m_state = State::Idle;
		return true;
	}

	[[nodiscard]] bool IsReady() const
	{
		return m_state == State::Idle;
	}
	[[nodiscard]] bool IsLive() const
	{
		return m_state == State::Live;
	}
	[[nodiscard]] bool ReleasePending() const
	{
		return m_state == State::ReleasePending || m_state == State::ThreadsStopped;
	}
	[[nodiscard]] bool ThreadsStopped() const
	{
		return m_state == State::ThreadsStopped;
	}
	[[nodiscard]] State CurrentState() const
	{
		return m_state;
	}

  private:
	std::uint64_t m_titleId{};
	State m_state{State::Idle};
};
