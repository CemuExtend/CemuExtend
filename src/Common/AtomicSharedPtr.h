#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#if defined(__cpp_lib_atomic_shared_ptr)
template<typename T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;
#else
// Apple libc++ does not implement std::atomic<std::shared_ptr<T>> yet.
template<typename T>
class AtomicSharedPtr
{
  public:
	void store(std::shared_ptr<T> value, std::memory_order = std::memory_order_seq_cst)
	{
		std::lock_guard lock(m_mutex);
		m_value = std::move(value);
	}

	std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const
	{
		std::lock_guard lock(m_mutex);
		return m_value;
	}

  private:
	mutable std::mutex m_mutex;
	std::shared_ptr<T> m_value;
};
#endif
