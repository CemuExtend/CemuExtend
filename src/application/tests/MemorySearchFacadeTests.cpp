#include "application/MemorySearchFacade.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>

namespace
{
	class FakeBackend final : public Application::IMemoryDiagnosticBackend
	{
	  public:
		bool IsEmulationRunning() const override
		{
			return running;
		}
		Application::MemoryMapSnapshot SnapshotMemoryMap() const override
		{
			return {generation, {{0x10000000, static_cast<std::uint32_t>(memory.size()), "MEM2"}}};
		}
		bool ReadCopy(std::uint64_t expected, std::uint32_t address,
					  std::span<std::byte> destination) const override
		{
			if (!running || expected != generation || address < 0x10000000)
				return false;
			activeReads.fetch_add(1);
			struct ReadGuard
			{
				std::atomic_int& active;
				~ReadGuard()
				{
					active.fetch_sub(1);
				}
			} guard{activeReads};
			if (readDelay)
				std::this_thread::sleep_for(std::chrono::milliseconds(readDelay));
			const auto offset = address - 0x10000000;
			if (offset + destination.size() > memory.size())
				return false;
			std::memcpy(destination.data(), memory.data() + offset, destination.size());
			return true;
		}

		bool running{true};
		std::uint64_t generation{7};
		std::array<std::byte, 64> memory{};
		mutable std::atomic_int activeReads{};
		unsigned readDelay{};
	};

	Application::MemorySearchStatus Wait(Application::MemorySearchFacade& facade,
										 std::uint64_t owner, const std::string& token)
	{
		for (unsigned attempt = 0; attempt < 1000; ++attempt)
		{
			auto status = facade.Status(owner, token);
			if (status.state != Application::MemorySearchState::Scanning)
				return status;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		assert(false && "memory search timed out");
		return {};
	}
} // namespace

int main()
{
	using namespace Application;
	auto backend = std::make_unique<FakeBackend>();
	auto* backendPtr = backend.get();
	// Big-endian int32 value 42 at two aligned addresses.
	backendPtr->memory[3] = std::byte{42};
	backendPtr->memory[11] = std::byte{42};
	MemorySearchFacade facade(std::move(backend));
	const auto started = facade.Start(9, {{MemoryValueType::Int32, std::int32_t{42}}, 64});
	const auto complete = Wait(facade, 9, started.sessionToken);
	assert(complete.state == MemorySearchState::Complete);
	assert(complete.resultCount == 2);
	auto page = facade.Page(9, started.sessionToken, started.generation, 0, 1);
	assert(page.total == 2 && page.results.size() == 1);
	assert(page.results.front().address.value == 0x10000000);
	assert(std::get<std::int32_t>(page.results.front().value.value) == 42);

	bool rejected = false;
	try
	{
		(void)facade.Status(10, started.sessionToken);
	} catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	assert(rejected && "session token must be bound to its owner window");

	backendPtr->memory[11] = std::byte{43};
	const auto filtered = facade.Filter(9, started.sessionToken, started.generation,
										{MemoryValueType::Int32, std::int32_t{42}});
	const auto filteredStatus = Wait(facade, 9, started.sessionToken);
	assert(filteredStatus.resultCount == 1);
	assert(filtered.generation == started.generation + 1);
	rejected = false;
	try
	{
		(void)facade.Page(9, started.sessionToken, started.generation, 0, 10);
	} catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	assert(rejected && "stale generations must be rejected");

	facade.CloseOwner(9);
	rejected = false;
	try
	{
		(void)facade.Status(9, started.sessionToken);
	} catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	assert(rejected && "closing a window must revoke its sessions");

	auto stoppedBackend = std::make_unique<FakeBackend>();
	stoppedBackend->running = false;
	MemorySearchFacade stopped(std::move(stoppedBackend));
	rejected = false;
	try
	{
		(void)stopped.Start(3, {{MemoryValueType::Int8, std::int8_t{1}}, 64});
	} catch (const std::runtime_error&)
	{
		rejected = true;
	}
	assert(rejected && "scans must require active emulation");

	auto delayedBackend = std::make_unique<FakeBackend>();
	auto* delayed = delayedBackend.get();
	delayed->readDelay = 25;
	MemorySearchFacade shuttingDown(std::move(delayedBackend));
	(void)shuttingDown.Start(11, {{MemoryValueType::Int8, std::int8_t{0}}, 64});
	for (unsigned attempt = 0; attempt < 100 && delayed->activeReads.load() == 0; ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	assert(delayed->activeReads.load() > 0);
	shuttingDown.BeginShutdown();
	assert(delayed->activeReads.load() == 0 && "shutdown must join in-flight reads");
}
