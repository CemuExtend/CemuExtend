#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Application
{
	using CemodCancellationCheck = std::function<bool()>;
	struct CemodManagerPermission
	{
		std::string name;
		std::uint64_t bit{};
		bool requested{};
		bool granted{};
		bool dangerous{};
		bool manifestMismatch{};
	};

	struct CemodManagerPackage
	{
		std::string packageKey;
		std::vector<std::uint64_t> titleIds;
		std::string modId;
		std::string principal;
		std::string modIdentity;
		std::string packageDigest;
		std::string pluginName;
		std::string author;
		std::string version;
		std::string description;
		std::string scope;
		std::string status;
		std::string approvalReason;
		std::vector<std::string> warnings;
		std::vector<CemodManagerPermission> permissions;
		std::uint64_t requestedPermissions{};
		std::uint64_t grantedPermissions{};
		bool approved{};
		bool signedPackage{};
		bool trustedNative{};
		bool wups{};
		bool headless{};
		bool runtimeAvailable{};
		bool valid{};
		// Whether this package participates at all. A disabled package is neither
		// loaded nor asked about at launch, for every title it covers.
		bool enabled{true};
		// Whether the user asked to trust this mod's future updates for the selected
		// title, so a rebuilt package is not sent back to the approval dialog.
		bool trustUpdates{};
	};

	struct CemodManagerSnapshot
	{
		std::uint64_t generation{};
		std::optional<std::uint64_t> selectedTitleId;
		std::vector<CemodManagerPackage> packages;
		bool cancelled{};
	};

	enum class CemodManagerError : std::uint8_t
	{
		None,
		Conflict,
		NotFound,
		InvalidPermissions,
		InspectionFailed,
		SaveFailed,
		ImportFailed,
	};

	struct CemodApprovalUpdate
	{
		std::uint64_t generation{};
		std::uint64_t titleId{};
		std::string packageKey;
		std::uint64_t grantedPermissions{};
		bool approved{};
		bool trustUpdates{};
	};

	struct CemodEnableUpdate
	{
		std::string packageKey;
		bool enabled{};
	};

	struct CemodManagerResult
	{
		CemodManagerError error{CemodManagerError::None};
		std::string diagnostic;
		CemodManagerSnapshot snapshot;
		[[nodiscard]] explicit operator bool() const
		{
			return error == CemodManagerError::None;
		}
	};

	struct CemodLaunchApproval
	{
		std::uint64_t generation{};
		std::uint64_t titleId{};
		std::string packageKey;
		std::string packageDigest;
		std::string modIdentity;
		std::uint64_t requestedPermissions{};
	};

	struct CemodLaunchPreflight
	{
		std::uint64_t generation{};
		std::uint64_t titleId{};
		std::vector<CemodLaunchApproval> pendingApprovals;
	};

	class ICemodManagerService
	{
	  public:
		virtual ~ICemodManagerService() = default;
		[[nodiscard]] virtual CemodManagerSnapshot GetCemodManagerSnapshot(
			std::optional<std::uint64_t> titleId, CemodCancellationCheck cancelled = {}) = 0;
		[[nodiscard]] virtual CemodManagerResult SaveCemodApproval(
			const CemodApprovalUpdate& update) = 0;
		[[nodiscard]] virtual CemodManagerResult ImportLegacyCemodPackageData(
			std::uint64_t generation, std::uint64_t titleId,
			std::string_view packageKey) = 0;
		[[nodiscard]] virtual CemodManagerResult SetCemodEnabled(
			const CemodEnableUpdate& update) = 0;
	};
} // namespace Application
