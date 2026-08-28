#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>

class CemuUpdateWorkerMailbox
{
  public:
	enum class Work
	{
		CheckVersion,
		UpdateVersion,
	};

	explicit CemuUpdateWorkerMailbox(Work initialWork)
		: m_pendingWork(initialWork) {}

	[[nodiscard]] bool Request(Work work)
	{
		{
			std::lock_guard lock(m_mutex);
			if (m_stopRequested.load(std::memory_order_acquire))
				return false;
			m_pendingWork = work;
		}
		m_condition.notify_one();
		return true;
	}

	void RequestStop()
	{
		m_stopRequested.store(true, std::memory_order_release);
		m_condition.notify_all();
	}

	[[nodiscard]] bool StopRequested() const
	{
		return m_stopRequested.load(std::memory_order_acquire);
	}

	[[nodiscard]] std::optional<Work> Wait()
	{
		std::unique_lock lock(m_mutex);
		m_condition.wait(lock, [this] {
			return StopRequested() || m_pendingWork.has_value();
		});
		if (StopRequested())
			return std::nullopt;
		auto work = m_pendingWork;
		m_pendingWork.reset();
		return work;
	}

  private:
	mutable std::mutex m_mutex;
	std::condition_variable m_condition;
	std::optional<Work> m_pendingWork;
	std::atomic_bool m_stopRequested{};
};
