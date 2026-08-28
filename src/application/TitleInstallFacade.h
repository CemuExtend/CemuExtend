#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace Application
{
	enum class TitleInstallKind : std::uint8_t
	{
		Unknown,
		Base,
		Demo,
		Update,
		Dlc,
		SystemTitle,
		SystemData,
	};

	enum class TitleInstallConflict : std::uint8_t
	{
		None,
		DifferentType,
		SameVersion,
		NewerVersionInstalled,
	};

	enum class TitleInstallError : std::uint8_t
	{
		None,
		InvalidSource,
		MissingContent,
		NotEnoughSpace,
		StalePlan,
		Cancelled,
		CopyFailure,
		CommitFailure,
		RestoreFailure,
		ConflictNotAccepted,
	};

	enum class TitleInstallDecision : std::uint8_t
	{
		Proceed,
		AcceptConflict,
	};

	struct InstalledTitleSnapshot
	{
		bool exists{};
		bool valid{};
		std::uint64_t titleId{};
		std::uint32_t version{};
		TitleInstallKind kind{TitleInstallKind::Unknown};
		std::uint64_t fingerprint{};
	};

	struct TitleInstallPlan
	{
		std::filesystem::path sourcePath;
		std::filesystem::path targetPath;
		std::uint64_t titleId{};
		std::uint32_t version{};
		std::string titleName;
		TitleInstallKind kind{TitleInstallKind::Unknown};
		TitleInstallConflict conflict{TitleInstallConflict::None};
		std::uint64_t requiredBytes{};
		std::uint64_t availableBytes{};
		std::uint64_t sourceFingerprint{};
		InstalledTitleSnapshot installed;
	};

	struct TitleInstallPlanResult
	{
		TitleInstallError error{TitleInstallError::None};
		std::string diagnostic;
		std::optional<TitleInstallPlan> plan;

		[[nodiscard]] explicit operator bool() const
		{
			return error == TitleInstallError::None && plan.has_value();
		}
	};

	struct TitleInstallProgress
	{
		std::uint64_t bytesCompleted{};
		std::uint64_t bytesTotal{};
		std::filesystem::path currentPath;
	};

	using TitleInstallProgressHandler = std::function<void(const TitleInstallProgress&)>;
	using TitleInstallCancellationCheck = std::function<bool()>;

	struct TitleInstallResult
	{
		TitleInstallError error{TitleInstallError::None};
		std::string diagnostic;
		std::filesystem::path installedPath;

		[[nodiscard]] explicit operator bool() const
		{
			return error == TitleInstallError::None;
		}
	};

	class ITitleInstallService
	{
	  public:
		virtual ~ITitleInstallService() = default;
		[[nodiscard]] virtual TitleInstallPlanResult PlanTitleInstall(
			const std::filesystem::path& sourcePath) const = 0;
		[[nodiscard]] virtual TitleInstallResult InstallTitle(
			const TitleInstallPlan& plan, TitleInstallDecision decision,
			TitleInstallProgressHandler progress,
			TitleInstallCancellationCheck cancelled) = 0;
	};
} // namespace Application
