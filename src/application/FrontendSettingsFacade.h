#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <optional>
#include <vector>

namespace Application
{
	struct FrontendSettingsSnapshot
	{
		std::uint64_t revision{};
		std::vector<std::filesystem::path> gamePaths;
		bool startFullscreen{};
		bool openPad{};
		bool checkUpdates{true};
		bool saveScreenshots{true};
		bool updateChecksSupported{true};
		bool portableMode{};
		bool titleRunning{};
		bool setupCompleted{};
		std::optional<bool> fullscreenOverride;
	};

	struct FrontendSettingsUpdate
	{
		std::uint64_t expectedRevision{};
		std::vector<std::filesystem::path> gamePaths;
		bool startFullscreen{};
		bool openPad{};
		bool checkUpdates{true};
		bool saveScreenshots{true};
		bool completeSetup{};
	};

	enum class FrontendSettingsError : std::uint8_t
	{
		None,
		Conflict,
		TitleRunning,
		FullscreenOverride,
		UpdateUnsupported,
		InvalidPath,
		StorageFailed,
		SaveFailed,
	};

	struct FrontendSettingsResult
	{
		FrontendSettingsError error{FrontendSettingsError::None};
		FrontendSettingsSnapshot snapshot;
		std::string diagnostic;

		[[nodiscard]] explicit operator bool() const
		{
			return error == FrontendSettingsError::None;
		}
	};

	class IFrontendSettingsService
	{
	  public:
		virtual ~IFrontendSettingsService() = default;
		[[nodiscard]] virtual FrontendSettingsSnapshot GetFrontendSettings() const = 0;
		[[nodiscard]] virtual FrontendSettingsResult ApplyFrontendSettings(
			const FrontendSettingsUpdate& update) = 0;
	};
} // namespace Application
