#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Application
{
	struct TitleSummary
	{
		std::uint64_t titleId{};
		std::string name;
		std::filesystem::path path;
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
		virtual void ReplaceScanPaths(
			std::span<const std::filesystem::path> paths) = 0;
		virtual void RefreshTitles() = 0;
		virtual void AddTitleFromPath(const std::filesystem::path& path) = 0;
	};
}
