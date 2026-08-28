#include "frontend/ArchiveInstallPolicy.h"

#include <algorithm>
#include <cctype>

namespace Frontend::ArchiveInstallPolicy
{
	std::optional<std::filesystem::path> NormalizeRelativePath(std::string_view name)
	{
		if (name.empty() || name.front() == '/' || name.front() == '\\')
			return std::nullopt;
		std::string portableName(name);
		std::replace(portableName.begin(), portableName.end(), '\\', '/');
		if (portableName.size() >= 2 &&
			std::isalpha(static_cast<unsigned char>(portableName[0])) &&
			portableName[1] == ':')
			return std::nullopt;

		const std::u8string utf8Name(portableName.begin(), portableName.end());
		const std::filesystem::path rawPath(utf8Name);
		if (rawPath.is_absolute() || rawPath.has_root_name() || rawPath.has_root_directory())
			return std::nullopt;
		for (const auto& component : rawPath)
			if (component == "..")
				return std::nullopt;

		const auto normalized = rawPath.lexically_normal();
		if (normalized.empty() || normalized == ".")
			return std::nullopt;
		return normalized;
	}

	CommitResult CommitStagedDirectory(const std::filesystem::path& staging,
									   const std::filesystem::path& target, const std::filesystem::path& backup,
									   bool replaceExisting)
	{
		CommitResult result;
		std::error_code error;
		const bool hadTarget = std::filesystem::exists(target, error);
		if (error)
		{
			result.error = CommitError::BackupMoveFailed;
			result.filesystemError = error;
			return result;
		}
		if (hadTarget && !replaceExisting)
		{
			result.error = CommitError::TargetExists;
			return result;
		}
		if (hadTarget)
		{
			std::filesystem::rename(target, backup, error);
			if (error)
			{
				result.error = CommitError::BackupMoveFailed;
				result.filesystemError = error;
				return result;
			}
		}

		std::filesystem::rename(staging, target, error);
		if (error)
		{
			result.error = CommitError::CommitMoveFailed;
			result.filesystemError = error;
			if (!hadTarget)
				return result;
			std::error_code rollbackError;
			std::filesystem::rename(backup, target, rollbackError);
			result.rollbackSucceeded = !rollbackError;
			if (rollbackError)
			{
				result.error = CommitError::RollbackFailed;
				result.filesystemError = rollbackError;
			}
			return result;
		}

		result.committed = true;
		result.rollbackSucceeded = true;
		if (hadTarget)
		{
			std::filesystem::remove_all(backup, error);
			if (error)
			{
				result.error = CommitError::BackupCleanupFailed;
				result.filesystemError = error;
			}
		}
		return result;
	}
} // namespace Frontend::ArchiveInstallPolicy
