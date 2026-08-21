#include "Common/precompiled.h"

#include "application/MemorySearchFacade.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/MMU/MMU.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iomanip>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Application
{
	namespace
	{
		constexpr std::size_t kReadChunk = 1024 * 1024;

		std::uint64_t MapGeneration(const std::vector<MemoryRegion>& regions)
		{
			std::uint64_t hash = 1469598103934665603ULL;
			for (const auto& region : regions)
			{
				for (const auto value : {region.base, region.size})
				{
					hash ^= value;
					hash *= 1099511628211ULL;
				}
			}
			return hash;
		}

		class CafeMemoryDiagnosticBackend final : public IMemoryDiagnosticBackend
		{
		public:
			bool IsEmulationRunning() const override { return CafeSystem::IsTitleRunning(); }

			MemoryMapSnapshot SnapshotMemoryMap() const override
			{
				MemoryMapSnapshot snapshot;
				if (!IsEmulationRunning()) return snapshot;
				for (const auto* range : memory_getMMURanges())
				{
					if (!range->isMapped() || range->getSize() == 0) continue;
					snapshot.regions.push_back({range->getBase(), range->getSize(),
						std::string(range->getName())});
				}
				snapshot.generation = MapGeneration(snapshot.regions);
				return snapshot;
			}

			bool ReadCopy(std::uint64_t generation, std::uint32_t address,
				std::span<std::byte> destination) const override
			{
				if (!IsEmulationRunning() || destination.empty()) return false;
				const auto snapshot = SnapshotMemoryMap();
				if (snapshot.generation != generation) return false;
				const auto end = static_cast<std::uint64_t>(address) + destination.size();
				const auto found = std::ranges::find_if(snapshot.regions,
					[address, end](const MemoryRegion& region) {
						return address >= region.base && end <=
							static_cast<std::uint64_t>(region.base) + region.size;
					});
				if (found == snapshot.regions.end() ||
					!memory_isAddressRangeAccessible(address,
						static_cast<std::uint32_t>(destination.size()))) return false;
				const auto* source = memory_getPointerFromVirtualOffsetAllowNull(address);
				if (!source) return false;
				std::memcpy(destination.data(), source, destination.size());
				return true;
			}
		};

		template<typename T> T ByteswapIfNeeded(T value)
		{
			if constexpr (sizeof(T) == 1) return value;
			else
			{
				auto bits = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
				if constexpr (std::endian::native == std::endian::little)
					std::ranges::reverse(bits);
				return std::bit_cast<T>(bits);
			}
		}

		template<typename T> std::vector<std::byte> EncodeScalar(T value)
		{
			value = ByteswapIfNeeded(value);
			auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
			return {bytes.begin(), bytes.end()};
		}

		template<typename T> T DecodeScalar(std::span<const std::byte> bytes)
		{
			if (bytes.size() != sizeof(T)) throw std::logic_error("invalid scalar width");
			std::array<std::byte, sizeof(T)> copy{};
			std::ranges::copy(bytes, copy.begin());
			return ByteswapIfNeeded(std::bit_cast<T>(copy));
		}
	}

	struct MemorySearchFacade::Session
	{
		mutable std::mutex mutex;
		std::uint64_t ownerWindow{};
		std::string token;
		std::uint64_t generation{1};
		std::uint64_t mapGeneration{};
		MemoryValueType type{MemoryValueType::Int32};
		MemorySearchState state{MemorySearchState::Scanning};
		std::uint64_t bytesScanned{};
		std::uint64_t bytesTotal{};
		bool resultCapReached{};
		bool scanCapReached{};
		bool revoked{};
		std::string diagnostic;
		std::vector<StoredResult> results;
		std::jthread worker;
	};

	MemorySearchFacade::MemorySearchFacade(std::unique_ptr<IMemoryDiagnosticBackend> backend)
		: m_backend(std::move(backend))
	{
		if (!m_backend) throw std::invalid_argument("memory diagnostic backend is required");
	}

	MemorySearchFacade::~MemorySearchFacade() { BeginShutdown(); }

	std::string MemorySearchFacade::NewToken()
	{
		std::array<std::uint64_t, 2> words{};
		std::random_device random;
		for (auto& word : words)
		{
			word = (static_cast<std::uint64_t>(random()) << 32) ^ random();
		}
		std::ostringstream stream;
		stream << std::hex << std::setfill('0');
		for (const auto word : words) stream << std::setw(16) << word;
		return stream.str();
	}

	std::size_t MemorySearchFacade::ValueWidth(MemoryValueType type)
	{
		switch (type)
		{
		case MemoryValueType::Int8: return 1;
		case MemoryValueType::Int16: return 2;
		case MemoryValueType::Int32: case MemoryValueType::Float32: return 4;
		case MemoryValueType::Int64: case MemoryValueType::Float64: return 8;
		}
		throw std::invalid_argument("unsupported memory value type");
	}

	std::vector<std::byte> MemorySearchFacade::Encode(const MemorySearchValue& value)
	{
		switch (value.type)
		{
		case MemoryValueType::Int8: return EncodeScalar(std::get<std::int8_t>(value.value));
		case MemoryValueType::Int16: return EncodeScalar(std::get<std::int16_t>(value.value));
		case MemoryValueType::Int32: return EncodeScalar(std::get<std::int32_t>(value.value));
		case MemoryValueType::Int64: return EncodeScalar(std::get<std::int64_t>(value.value));
		case MemoryValueType::Float32: return EncodeScalar(std::get<float>(value.value));
		case MemoryValueType::Float64: return EncodeScalar(std::get<double>(value.value));
		}
		throw std::invalid_argument("memory value does not match its type");
	}

	MemorySearchValue MemorySearchFacade::Decode(MemoryValueType type,
		std::span<const std::byte> bytes)
	{
		switch (type)
		{
		case MemoryValueType::Int8: return {type, DecodeScalar<std::int8_t>(bytes)};
		case MemoryValueType::Int16: return {type, DecodeScalar<std::int16_t>(bytes)};
		case MemoryValueType::Int32: return {type, DecodeScalar<std::int32_t>(bytes)};
		case MemoryValueType::Int64: return {type, DecodeScalar<std::int64_t>(bytes)};
		case MemoryValueType::Float32: return {type, DecodeScalar<float>(bytes)};
		case MemoryValueType::Float64: return {type, DecodeScalar<double>(bytes)};
		}
		throw std::invalid_argument("unsupported memory value type");
	}

	std::shared_ptr<MemorySearchFacade::Session> MemorySearchFacade::FindSessionLocked(
		std::uint64_t ownerWindow, std::string_view token) const
	{
		const auto found = m_sessions.find(std::string(token));
		if (found == m_sessions.end() || found->second->ownerWindow != ownerWindow)
			throw std::invalid_argument("memory search session is not valid for this window");
		return found->second;
	}

	MemorySearchSessionInfo MemorySearchFacade::Start(std::uint64_t ownerWindow,
		const MemorySearchRequest& request)
	{
		if (ownerWindow == 0) throw std::invalid_argument("memory search requires a tool window");
		const auto needle = Encode(request.value);
		if (request.maximumBytes == 0 || request.maximumBytes > MaximumScanBytes)
			throw std::invalid_argument("maximumBytes is outside the supported range");
		if (!m_backend->IsEmulationRunning())
			throw std::runtime_error("memory search requires a running title");
		auto map = m_backend->SnapshotMemoryMap();
		if (map.regions.empty()) throw std::runtime_error("no emulated memory is mapped");
		if (map.regions.size() > MaximumRegions)
			throw std::runtime_error("emulated memory map exceeds the diagnostic range cap");

		auto session = std::make_shared<Session>();
		session->ownerWindow = ownerWindow;
		session->token = NewToken();
		session->mapGeneration = map.generation;
		session->type = request.value.type;
		const auto availableBytes = std::accumulate(map.regions.begin(), map.regions.end(),
			std::uint64_t{}, [](std::uint64_t total, const MemoryRegion& region) {
				return total + region.size;
			});
		std::uint64_t remaining = request.maximumBytes;
		for (auto& region : map.regions)
		{
			const auto accepted = std::min<std::uint64_t>(region.size, remaining);
			region.size = static_cast<std::uint32_t>(accepted);
			session->bytesTotal += accepted;
			remaining -= accepted;
			if (remaining == 0) break;
		}
		map.regions.erase(std::remove_if(map.regions.begin(), map.regions.end(),
			[](const MemoryRegion& region) { return region.size == 0; }), map.regions.end());
		session->scanCapReached = availableBytes > request.maximumBytes;
		std::vector<std::shared_ptr<Session>> replaced;
		{
			std::scoped_lock lock(m_mutex);
			if (m_shuttingDown) throw std::runtime_error("memory diagnostics are shutting down");
			for (auto it = m_sessions.begin(); it != m_sessions.end();)
			{
				if (it->second->ownerWindow == ownerWindow)
				{
					it->second->worker.request_stop();
					replaced.push_back(it->second);
					it = m_sessions.erase(it);
				}
				else ++it;
			}
			m_sessions.emplace(session->token, session);
		}
		for (const auto& previous : replaced)
		{
			{
				std::scoped_lock lock(previous->mutex);
				previous->revoked = true;
			}
			previous->worker.request_stop();
			if (previous->worker.joinable()) previous->worker.join();
		}
		{
			std::scoped_lock lock(m_mutex);
			if (m_shuttingDown || !m_sessions.contains(session->token))
				throw std::runtime_error("memory diagnostics are shutting down");
			session->worker = std::jthread([this, session, map = std::move(map),
				needle](std::stop_token stopToken) mutable {
				RunInitial(session, stopToken, std::move(map), needle);
			});
		}
		return {session->token, session->generation, session->mapGeneration,
			session->bytesTotal};
	}

	void MemorySearchFacade::RunInitial(const std::shared_ptr<Session>& session,
		std::stop_token stopToken, MemoryMapSnapshot map, std::vector<std::byte> needle)
	{
		try
		{
			const auto width = needle.size();
			for (const auto& region : map.regions)
			{
				for (std::uint64_t offset = 0; offset + width <= region.size;)
				{
					if (stopToken.stop_requested())
					{
						std::scoped_lock lock(session->mutex);
						session->state = MemorySearchState::Cancelled;
						return;
					}
					const auto payload = static_cast<std::size_t>(std::min<std::uint64_t>(
						kReadChunk, region.size - offset));
					std::vector<std::byte> bytes(payload);
					if (!m_backend->ReadCopy(map.generation,
						region.base + static_cast<std::uint32_t>(offset), bytes))
						throw std::runtime_error("emulation stopped or its memory map changed");
					for (std::size_t local = 0; local + width <= bytes.size(); local += width)
					{
						if (std::equal(needle.begin(), needle.end(), bytes.begin() + local))
						{
							std::scoped_lock lock(session->mutex);
							if (session->results.size() == MaximumResults)
							{
								session->resultCapReached = true;
								session->state = MemorySearchState::Complete;
								return;
							}
							session->results.push_back({region.base +
								static_cast<std::uint32_t>(offset + local), needle});
						}
					}
					offset += payload - (payload % width);
					std::scoped_lock lock(session->mutex);
					session->bytesScanned = std::min(session->bytesTotal,
						session->bytesScanned + payload);
				}
			}
			std::scoped_lock lock(session->mutex);
			session->bytesScanned = session->bytesTotal;
			session->state = MemorySearchState::Complete;
		}
		catch (const std::exception& error)
		{
			std::scoped_lock lock(session->mutex);
			session->state = MemorySearchState::Failed;
			session->diagnostic = error.what();
		}
	}

	MemorySearchSessionInfo MemorySearchFacade::Filter(std::uint64_t ownerWindow,
		std::string_view token, std::uint64_t expectedGeneration,
		const MemorySearchValue& value)
	{
		const auto needle = Encode(value);
		std::shared_ptr<Session> session;
		{
			std::scoped_lock lock(m_mutex);
			if (m_shuttingDown) throw std::runtime_error("memory diagnostics are shutting down");
			session = FindSessionLocked(ownerWindow, token);
		}
		std::vector<StoredResult> candidates;
		{
			std::scoped_lock lock(session->mutex);
			if (session->revoked)
				throw std::invalid_argument("memory search session has been revoked");
			if (session->generation != expectedGeneration)
				throw std::invalid_argument("memory search generation is stale");
			if (session->state == MemorySearchState::Scanning)
				throw std::runtime_error("memory search is still running");
			if (session->type != value.type)
				throw std::invalid_argument("filter value type must match the session type");
			if (!m_backend->IsEmulationRunning())
				throw std::runtime_error("memory search requires a running title");
			candidates = session->results;
			session->generation++;
			session->state = MemorySearchState::Scanning;
			session->bytesScanned = 0;
			session->bytesTotal = candidates.size() * needle.size();
			session->results.clear();
			session->resultCapReached = false;
			session->diagnostic.clear();
		}
		{
			std::scoped_lock lock(m_mutex, session->mutex);
			if (m_shuttingDown || session->revoked || !m_sessions.contains(session->token))
				throw std::runtime_error("memory search session has been closed");
			session->worker = std::jthread([this, session,
				candidates = std::move(candidates), needle](std::stop_token stopToken) mutable {
				RunFilter(session, stopToken, std::move(candidates), needle);
			});
		}
		return {session->token, session->generation, session->mapGeneration,
			session->bytesTotal};
	}

	void MemorySearchFacade::RunFilter(const std::shared_ptr<Session>& session,
		std::stop_token stopToken, std::vector<StoredResult> candidates,
		std::vector<std::byte> needle)
	{
		try
		{
			for (const auto& candidate : candidates)
			{
				if (stopToken.stop_requested())
				{
					std::scoped_lock lock(session->mutex);
					session->state = MemorySearchState::Cancelled;
					return;
				}
				std::vector<std::byte> current(needle.size());
				if (!m_backend->ReadCopy(session->mapGeneration, candidate.address, current))
					throw std::runtime_error("emulation stopped or its memory map changed");
				std::scoped_lock lock(session->mutex);
				if (current == needle) session->results.push_back({candidate.address, current});
				session->bytesScanned += needle.size();
			}
			std::scoped_lock lock(session->mutex);
			session->state = MemorySearchState::Complete;
		}
		catch (const std::exception& error)
		{
			std::scoped_lock lock(session->mutex);
			session->state = MemorySearchState::Failed;
			session->diagnostic = error.what();
		}
	}

	MemorySearchStatus MemorySearchFacade::Status(std::uint64_t ownerWindow,
		std::string_view token) const
	{
		std::shared_ptr<Session> session;
		{
			std::scoped_lock lock(m_mutex);
			session = FindSessionLocked(ownerWindow, token);
		}
		std::scoped_lock lock(session->mutex);
		if (session->revoked)
			throw std::invalid_argument("memory search session has been revoked");
		return {session->generation, session->state, session->bytesScanned,
			session->bytesTotal, static_cast<std::uint32_t>(session->results.size()),
			session->resultCapReached, session->scanCapReached, session->diagnostic};
	}

	MemorySearchPage MemorySearchFacade::Page(std::uint64_t ownerWindow,
		std::string_view token, std::uint64_t expectedGeneration,
		std::uint32_t offset, std::uint32_t limit) const
	{
		if (limit == 0 || limit > MaximumPageSize)
			throw std::invalid_argument("page limit is outside the supported range");
		std::shared_ptr<Session> session;
		{
			std::scoped_lock lock(m_mutex);
			session = FindSessionLocked(ownerWindow, token);
		}
		std::scoped_lock lock(session->mutex);
		if (session->revoked)
			throw std::invalid_argument("memory search session has been revoked");
		if (session->generation != expectedGeneration)
			throw std::invalid_argument("memory search generation is stale");
		if (session->state == MemorySearchState::Scanning)
			throw std::runtime_error("memory search results are not ready");
		MemorySearchPage page{session->generation, offset,
			static_cast<std::uint32_t>(session->results.size()), {}};
		if (offset >= session->results.size()) return page;
		const auto end = std::min<std::size_t>(session->results.size(), offset + limit);
		page.results.reserve(end - offset);
		for (std::size_t index = offset; index < end; ++index)
			page.results.push_back({{session->results[index].address},
				Decode(session->type, session->results[index].bytes)});
		return page;
	}

	void MemorySearchFacade::Cancel(std::uint64_t ownerWindow, std::string_view token,
		std::uint64_t expectedGeneration)
	{
		std::shared_ptr<Session> session;
		{
			std::scoped_lock lock(m_mutex);
			session = FindSessionLocked(ownerWindow, token);
		}
		std::scoped_lock lock(session->mutex);
		if (session->revoked)
			throw std::invalid_argument("memory search session has been revoked");
		if (session->generation != expectedGeneration)
			throw std::invalid_argument("memory search generation is stale");
		session->worker.request_stop();
	}

	void MemorySearchFacade::CloseOwner(std::uint64_t ownerWindow) noexcept
	{
		std::vector<std::shared_ptr<Session>> removed;
		{
			std::scoped_lock lock(m_mutex);
			for (auto it = m_sessions.begin(); it != m_sessions.end();)
			{
				if (it->second->ownerWindow == ownerWindow)
				{
					removed.push_back(it->second);
					it = m_sessions.erase(it);
				}
				else ++it;
			}
		}
		for (const auto& session : removed)
		{
			{
				std::scoped_lock lock(session->mutex);
				session->revoked = true;
			}
			session->worker.request_stop();
			if (session->worker.joinable()) session->worker.join();
		}
	}

	void MemorySearchFacade::BeginShutdown() noexcept
	{
		std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
		{
			std::scoped_lock lock(m_mutex);
			if (std::exchange(m_shuttingDown, true)) return;
			sessions.swap(m_sessions);
		}
		for (const auto& [token, session] : sessions)
		{
			(void)token;
			{
				std::scoped_lock lock(session->mutex);
				session->revoked = true;
			}
			session->worker.request_stop();
			if (session->worker.joinable()) session->worker.join();
		}
	}

	std::unique_ptr<IMemoryDiagnosticBackend> CreateCafeMemoryDiagnosticBackend()
	{
		return std::make_unique<CafeMemoryDiagnosticBackend>();
	}
}
