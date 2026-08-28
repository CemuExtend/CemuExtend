#pragma once

#include "application/TitleCatalog.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace Application
{
	[[nodiscard]] constexpr bool IsManagedContentDeletionSupported(ManagedContentType type)
	{
		return type != ManagedContentType::Save && type != ManagedContentType::System;
	}

	struct ManagedContentPathValidation
	{
		std::filesystem::path canonicalPath;
		std::string diagnostic;
		[[nodiscard]] explicit operator bool() const
		{
			return !canonicalPath.empty();
		}
	};

	// Destructive operations accept catalog paths only when their complete path is
	// canonical (no symlink/reparse traversal) and strictly below a configured,
	// trusted content root. Equality with a root is intentionally rejected.
	inline ManagedContentPathValidation ValidateManagedContentPath(
		const std::filesystem::path& candidate,
		std::span<const std::filesystem::path> trustedRoots)
	{
		namespace fs = std::filesystem;
		if (!candidate.is_absolute())
			return {{}, "Managed-content catalog path is not absolute"};
		std::error_code error;
		const auto lexical = fs::absolute(candidate, error).lexically_normal();
		if (error)
			return {{}, error.message()};
		const auto canonical = fs::canonical(candidate, error);
		if (error)
			return {{}, error.message()};
		if (canonical != lexical)
			return {{}, "Managed-content catalog path traverses a symbolic link or reparse point"};
		const auto status = fs::symlink_status(canonical, error);
		if (error)
			return {{}, error.message()};
		if (status.type() == fs::file_type::symlink)
			return {{}, "Managed-content catalog root is a symbolic link"};
		for (const auto& root : trustedRoots)
		{
			if (root.empty())
				continue;
			const auto canonicalRoot = fs::canonical(root, error);
			if (error)
			{
				error.clear();
				continue;
			}
			const auto relative = canonical.lexically_relative(canonicalRoot);
			if (relative.empty() || relative == ".")
				continue;
			const auto first = *relative.begin();
			if (first != ".." && !relative.is_absolute())
				return {canonical, {}};
		}
		return {{}, "Managed-content catalog path is outside configured content roots"};
	}

	enum class ManagedContentDeleteError : std::uint8_t
	{
		None,
		NotFound,
		Unsupported,
		TitleRunning,
		Scanning,
		StalePlan,
		DeleteFailure,
	};

	struct ManagedContentDeletePlan
	{
		std::uint64_t locationUid{};
		std::uint64_t titleId{};
		std::uint64_t fingerprint{};
		std::string name;
		std::string displayPath;
		ManagedContentType type{ManagedContentType::Base};
		ManagedContentFormat format{ManagedContentFormat::Folder};
	};

	struct ManagedContentDeletePlanResult
	{
		ManagedContentDeleteError error{ManagedContentDeleteError::None};
		std::string diagnostic;
		std::optional<ManagedContentDeletePlan> plan;

		[[nodiscard]] explicit operator bool() const
		{
			return error == ManagedContentDeleteError::None && plan.has_value();
		}
	};

	struct ManagedContentDeleteResult
	{
		ManagedContentDeleteError error{ManagedContentDeleteError::None};
		std::string diagnostic;

		[[nodiscard]] explicit operator bool() const
		{
			return error == ManagedContentDeleteError::None;
		}
	};

	class IManagedContentService
	{
	  public:
		virtual ~IManagedContentService() = default;
		[[nodiscard]] virtual ManagedContentDeletePlanResult PlanManagedContentDelete(
			std::uint64_t locationUid) const = 0;
		[[nodiscard]] virtual ManagedContentDeleteResult DeleteManagedContent(
			const ManagedContentDeletePlan& plan) = 0;
	};
} // namespace Application
