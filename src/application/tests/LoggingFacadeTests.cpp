#include "Common/precompiled.h"
#include "application/LoggingFacade.h"
#include "Cemu/Logging/CemuLogging.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

int main()
{
	auto check = [](bool condition) { if (!condition) std::abort(); };
	Application::LoggingFacade facade;
	std::atomic_uint64_t callbacks{};
	auto subscription = facade.Subscribe(
		[&](const Application::LoggingEntry& entry) {
			if (entry.message.empty()) std::abort();
			callbacks.fetch_add(1, std::memory_order_relaxed);
		});

	constexpr unsigned threadCount = 4;
	constexpr unsigned entriesPerThread = 700;
	std::vector<std::thread> workers;
	for (unsigned thread = 0; thread < threadCount; ++thread)
	{
		workers.emplace_back([thread] {
			for (unsigned entry = 0; entry < entriesPerThread; ++entry)
				cemuLog_log(LogType::Force, "worker {} entry {}", thread, entry);
		});
	}
	for (auto& worker : workers) worker.join();

	const auto snapshot = facade.Snapshot();
	check(callbacks.load(std::memory_order_relaxed) == threadCount * entriesPerThread);
	check(snapshot.entries.size() <= Application::LoggingFacade::MaximumEntries);
	check(snapshot.retainedBytes <= Application::LoggingFacade::MaximumBytes);
	check(snapshot.droppedEntries > 0);
	for (std::size_t index = 1; index < snapshot.entries.size(); ++index)
		check(snapshot.entries[index - 1].sequence < snapshot.entries[index].sequence);

	const auto previousCallbacks = callbacks.load(std::memory_order_relaxed);
	subscription.Reset();
	cemuLog_log(LogType::Force, "error: subscription has been reset");
	check(callbacks.load(std::memory_order_relaxed) == previousCallbacks);
	const auto classified = facade.Snapshot(snapshot.nextSequence - 1);
	check(classified.entries.size() == 1);
	check(classified.entries.front().level == Application::LoggingLevel::Error);

	const auto clearedThrough = facade.Clear();
	check(clearedThrough == classified.entries.front().sequence);
	check(facade.Snapshot().entries.empty());
	return 0;
}
