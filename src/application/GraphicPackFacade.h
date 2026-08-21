#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <functional>

namespace Application
{
	enum class GraphicPackError : std::uint8_t
	{
		None,
		NotFound,
		InvalidPreset,
		TitleRunning,
		BackendFailure,
	};

	struct GraphicPackPreset
	{
		std::string category;
		std::string name;
		bool active{};
		bool visible{};
	};

	struct GraphicPackInfo
	{
		std::string key;
		std::string virtualPath;
		std::string name;
		std::string description;
		std::int32_t version{};
		bool universal{};
		bool enabled{};
		bool activated{};
		bool defaultEnabled{};
		bool hasShaders{};
		bool hasPatches{};
		bool hasCustomVsync{};
		bool supportedVersion{};
		std::vector<std::uint64_t> titleIds;
		std::vector<std::string> presetOrder;
		std::vector<GraphicPackPreset> presets;
	};

	struct GraphicPackResult
	{
		GraphicPackError error{GraphicPackError::None};
		bool changed{};
		bool titleRunning{};
		bool requiresRestart{};
		bool applied{};
		bool reloaded{};
		std::string diagnostic;
		std::optional<GraphicPackInfo> info;

		[[nodiscard]] explicit operator bool() const
		{
			return error == GraphicPackError::None;
		}
	};

	struct GraphicPackRefreshResult
	{
		GraphicPackError error{GraphicPackError::None};
		std::vector<std::string> removedEnabledPaths;
		std::string diagnostic;

		[[nodiscard]] explicit operator bool() const
		{
			return error == GraphicPackError::None;
		}
	};

	enum class GraphicPackInstallKind : std::uint8_t
	{
		Community,
		CustomUrl,
	};

	enum class GraphicPackInstallPhase : std::uint8_t
	{
		Checking,
		Downloading,
		Extracting,
		Refreshing,
	};

	enum class GraphicPackInstallError : std::uint8_t
	{
		None,
		ConfirmationRequired,
		Cancelled,
		InvalidUrl,
		ConnectionFailed,
		InvalidArchive,
		Conflict,
		IoFailure,
	};

	struct GraphicPackInstallRequest
	{
		GraphicPackInstallKind kind{GraphicPackInstallKind::Community};
		std::string url;
		bool replaceExisting{};
	};

	struct GraphicPackInstallProgress
	{
		GraphicPackInstallPhase phase{GraphicPackInstallPhase::Checking};
		std::uint64_t completed{};
		std::uint64_t total{};
		std::string currentPath;
	};

	using GraphicPackInstallProgressHandler =
		std::function<void(const GraphicPackInstallProgress&)>;
	using GraphicPackInstallCancellationCheck = std::function<bool()>;

	struct GraphicPackInstallResult
	{
		GraphicPackInstallError error{GraphicPackInstallError::None};
		std::string diagnostic;
		bool upToDate{};
		std::vector<std::string> removedEnabledPaths;

		[[nodiscard]] explicit operator bool() const
		{
			return error == GraphicPackInstallError::None;
		}
	};

	class IGraphicPackService
	{
	public:
		virtual ~IGraphicPackService() = default;
		[[nodiscard]] virtual std::vector<GraphicPackInfo> ListGraphicPacks() const = 0;
		[[nodiscard]] virtual GraphicPackResult SetGraphicPackEnabled(
			std::string_view key, bool enabled) = 0;
		[[nodiscard]] virtual GraphicPackResult SetGraphicPackPreset(
			std::string_view key, std::string_view category, std::string_view preset) = 0;
		[[nodiscard]] virtual GraphicPackResult ReloadGraphicPack(std::string_view key) = 0;
		[[nodiscard]] virtual GraphicPackRefreshResult RefreshGraphicPacks() = 0;
		[[nodiscard]] virtual GraphicPackInstallResult InstallGraphicPacks(
			const GraphicPackInstallRequest& request,
			GraphicPackInstallProgressHandler progress,
			GraphicPackInstallCancellationCheck cancelled) = 0;
		virtual void SaveGraphicPackState() = 0;
	};
}
