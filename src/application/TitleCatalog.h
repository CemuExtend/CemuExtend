#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Application
{
	struct TitleSummary
	{
		std::uint64_t titleId{};
		std::string name;
		std::filesystem::path path;
	};

	struct TitlePlayStats
	{
		bool available{};
		std::uint32_t minutesPlayed{};
		std::uint32_t lastPlayedYear{};
		std::uint32_t lastPlayedMonth{};
		std::uint32_t lastPlayedDay{};
	};

	struct GameSummary
	{
		std::uint64_t titleId{};
		std::string name;
		std::filesystem::path basePath;
		std::filesystem::path savePath;
		std::optional<std::filesystem::path> updatePath;
		std::optional<std::filesystem::path> aocPath;
		std::uint16_t version{};
		std::uint16_t aocVersion{};
		std::uint32_t region{};
		std::string regionName;
		std::string productCode;
		std::string companyCode;
		bool systemData{};
		TitlePlayStats playStats;
	};

	enum class TitleCatalogEventType : std::uint8_t
	{
		Discovered,
		Removed,
		ScanFinished,
	};

	struct TitleCatalogEvent
	{
		TitleCatalogEventType type{};
		std::uint64_t titleId{};
	};

	using TitleCatalogHandler = std::function<void(const TitleCatalogEvent&)>;

	namespace Detail
	{
		class TitleSubscriptionState
		{
		public:
			virtual ~TitleSubscriptionState() = default;
			virtual void Stop() = 0;
		};
	}

	class TitleCatalogSubscription final
	{
	public:
		TitleCatalogSubscription() = default;
		explicit TitleCatalogSubscription(
			std::shared_ptr<Detail::TitleSubscriptionState> state)
			: m_state(std::move(state)) {}
		TitleCatalogSubscription(TitleCatalogSubscription&&) noexcept = default;
		TitleCatalogSubscription& operator=(TitleCatalogSubscription&& other) noexcept
		{
			if (this != &other)
			{
				Reset();
				m_state = std::move(other.m_state);
			}
			return *this;
		}
		~TitleCatalogSubscription() { Reset(); }

		TitleCatalogSubscription(const TitleCatalogSubscription&) = delete;
		TitleCatalogSubscription& operator=(const TitleCatalogSubscription&) = delete;

		void Reset()
		{
			if (!m_state)
				return;
			auto state = std::move(m_state);
			state->Stop();
		}

	private:
		std::shared_ptr<Detail::TitleSubscriptionState> m_state;
	};

	// Application-owned title query/command boundary. Frontends receive copied
	// values and never retain Cafe TitleInfo objects or callback payloads.
	class ITitleCatalog
	{
	public:
		virtual ~ITitleCatalog() = default;

		[[nodiscard]] virtual std::vector<TitleSummary> ListTitles() const = 0;
		[[nodiscard]] virtual std::optional<TitleSummary> ResolveBaseTitle(
			std::uint64_t titleId) const = 0;
		[[nodiscard]] virtual std::vector<GameSummary> ListGames() const = 0;
		[[nodiscard]] virtual std::optional<GameSummary> GetGame(
			std::uint64_t titleId) const = 0;
		[[nodiscard]] virtual bool IsTitleScanning() const = 0;
		[[nodiscard]] virtual std::optional<std::vector<std::uint8_t>> LoadTitleIcon(
			std::uint64_t titleId) const = 0;
		[[nodiscard]] virtual TitleCatalogSubscription SubscribeTitleCatalogEvents(
			TitleCatalogHandler handler) = 0;
		virtual void ReplaceScanPaths(
			std::span<const std::filesystem::path> paths) = 0;
		virtual void RefreshTitles() = 0;
		virtual void AddTitleFromPath(const std::filesystem::path& path) = 0;
	};
}
