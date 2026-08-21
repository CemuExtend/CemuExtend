#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace Frontend::ArchiveInstallPolicy
{
	[[nodiscard]] std::optional<std::filesystem::path> NormalizeRelativePath(
		std::string_view name);

	enum class CommitError
	{
		None,
		TargetExists,
		BackupMoveFailed,
		CommitMoveFailed,
		RollbackFailed,
		BackupCleanupFailed,
	};

	struct CommitResult
	{
		bool committed{};
		bool rollbackSucceeded{};
		CommitError error{CommitError::None};
		std::error_code filesystemError;
	};

	[[nodiscard]] CommitResult CommitStagedDirectory(
		const std::filesystem::path& staging,
		const std::filesystem::path& target,
		const std::filesystem::path& backup,
		bool replaceExisting);
}
