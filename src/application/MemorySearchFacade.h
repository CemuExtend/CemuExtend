#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Application
{
	enum class MemoryValueType : std::uint8_t
	{
		Int8,
		Int16,
		Int32,
		Int64,
		Float32,
		Float64,
	};

	using MemoryScalar = std::variant<std::int8_t, std::int16_t, std::int32_t,
									  std::int64_t, float, double>;

	struct MemorySearchValue
	{
		MemoryValueType type{MemoryValueType::Int32};
		MemoryScalar value{std::int32_t{}};
	};

	struct MemoryRegion
	{
		std::uint32_t base{};
		std::uint32_t size{};
		std::string name;
	};

	struct MemoryMapSnapshot
	{
		std::uint64_t generation{};
		std::vector<MemoryRegion> regions;
	};

	class IMemoryDiagnosticBackend
	{
	  public:
		virtual ~IMemoryDiagnosticBackend() = default;
		[[nodiscard]] virtual bool IsEmulationRunning() const = 0;
		[[nodiscard]] virtual MemoryMapSnapshot SnapshotMemoryMap() const = 0;
		// Implementations must validate both the current map generation and the
		// complete guest range before copying into destination.
		[[nodiscard]] virtual bool ReadCopy(std::uint64_t mapGeneration,
											std::uint32_t address, std::span<std::byte> destination) const = 0;
	};

	struct MemorySearchRequest
	{
		MemorySearchValue value;
		std::uint64_t maximumBytes{};
	};

	struct MemorySearchSessionInfo
	{
		std::string sessionToken;
		std::uint64_t generation{};
		std::uint64_t mapGeneration{};
		std::uint64_t bytesTotal{};
	};

	enum class MemorySearchState : std::uint8_t
	{
		Scanning,
		Complete,
		Cancelled,
		Failed,
	};

	struct MemorySearchStatus
	{
		std::uint64_t generation{};
		MemorySearchState state{MemorySearchState::Scanning};
		std::uint64_t bytesScanned{};
		std::uint64_t bytesTotal{};
		std::uint32_t resultCount{};
		bool resultCapReached{};
		bool scanCapReached{};
		std::string diagnostic;
	};

	struct MemoryAddress
	{
		std::uint32_t value{};
	};

	struct MemorySearchResult
	{
		MemoryAddress address;
		MemorySearchValue value;
	};

	struct MemorySearchPage
	{
		std::uint64_t generation{};
		std::uint32_t offset{};
		std::uint32_t total{};
		std::vector<MemorySearchResult> results;
	};

	class MemorySearchFacade final
	{
	  public:
		static constexpr std::uint64_t MaximumScanBytes = 512ULL * 1024 * 1024;
		static constexpr std::uint32_t MaximumResults = 50000;
		static constexpr std::uint32_t MaximumPageSize = 200;
		static constexpr std::uint32_t MaximumRegions = 128;

		explicit MemorySearchFacade(std::unique_ptr<IMemoryDiagnosticBackend> backend);
		~MemorySearchFacade();
		MemorySearchFacade(const MemorySearchFacade&) = delete;
		MemorySearchFacade& operator=(const MemorySearchFacade&) = delete;

		[[nodiscard]] MemorySearchSessionInfo Start(std::uint64_t ownerWindow,
													const MemorySearchRequest& request);
		[[nodiscard]] MemorySearchSessionInfo Filter(std::uint64_t ownerWindow,
													 std::string_view token, std::uint64_t expectedGeneration,
													 const MemorySearchValue& value);
		[[nodiscard]] MemorySearchStatus Status(std::uint64_t ownerWindow,
												std::string_view token) const;
		[[nodiscard]] MemorySearchPage Page(std::uint64_t ownerWindow,
											std::string_view token, std::uint64_t expectedGeneration,
											std::uint32_t offset, std::uint32_t limit) const;
		void Cancel(std::uint64_t ownerWindow, std::string_view token,
					std::uint64_t expectedGeneration);
		void CloseOwner(std::uint64_t ownerWindow) noexcept;
		void BeginShutdown() noexcept;

	  private:
		struct StoredResult
		{
			std::uint32_t address{};
			std::vector<std::byte> bytes;
		};
		struct Session;

		[[nodiscard]] std::shared_ptr<Session> FindSessionLocked(
			std::uint64_t ownerWindow, std::string_view token) const;
		static std::string NewToken();
		static std::size_t ValueWidth(MemoryValueType type);
		static std::vector<std::byte> Encode(const MemorySearchValue& value);
		static MemorySearchValue Decode(MemoryValueType type,
										std::span<const std::byte> bytes);
		void RunInitial(const std::shared_ptr<Session>& session, std::stop_token stopToken,
						MemoryMapSnapshot map, std::vector<std::byte> needle);
		void RunFilter(const std::shared_ptr<Session>& session,
					   std::stop_token stopToken, std::vector<StoredResult> candidates,
					   std::vector<std::byte> needle);

		std::unique_ptr<IMemoryDiagnosticBackend> m_backend;
		mutable std::mutex m_mutex;
		std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;
		bool m_shuttingDown{};
	};

	[[nodiscard]] std::unique_ptr<IMemoryDiagnosticBackend>
	CreateCafeMemoryDiagnosticBackend();
} // namespace Application
