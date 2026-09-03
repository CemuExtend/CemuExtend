#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CemuExtend
{
	enum class CemodExecutionMode : std::uint8_t
	{
		Isolated,
		TrustedNative,
	};

	enum class CemodPermission : std::uint8_t
	{
		NativeMemory,
		FunctionPatching,
		PhysicalAddressPatching,
		FilesystemRead,
		FilesystemWrite,
		Network,
		MappedMemory,
		Notifications,
		ContentRedirection,
		Modules,
		PluginManagement,
		WebUi,
		// The CEX2 services a package reaches through the host bridge. A package
		// declares these in its manifest as read/write/inject/clipboard/capture, and
		// they are approved here like every other permission so the user decides
		// which ones a package actually gets.
		ServiceRead,
		ServiceWrite,
		ServiceInject,
		ServiceClipboard,
		ServiceCapture,
		ServiceMicrophone,
	};

	inline constexpr std::array kCemodPermissions{
		CemodPermission::NativeMemory,
		CemodPermission::FunctionPatching,
		CemodPermission::PhysicalAddressPatching,
		CemodPermission::FilesystemRead,
		CemodPermission::FilesystemWrite,
		CemodPermission::Network,
		CemodPermission::MappedMemory,
		CemodPermission::Notifications,
		CemodPermission::ContentRedirection,
		CemodPermission::Modules,
		CemodPermission::PluginManagement,
		CemodPermission::WebUi,
		CemodPermission::ServiceRead,
		CemodPermission::ServiceWrite,
		CemodPermission::ServiceInject,
		CemodPermission::ServiceClipboard,
		CemodPermission::ServiceCapture,
		CemodPermission::ServiceMicrophone,
	};

	// The exact-approval dialog refuses to approve anything unless it receives
	// this entire set, so it mirrors the size in CEMOD_PERMISSION_MODEL_SIZE
	// (ui/src/windows/CemodPermissionsWindow.tsx). Growing this list without
	// updating that constant leaves the dialog's save button permanently
	// disabled, which is why the two are pinned together here.
	static_assert(kCemodPermissions.size() == 18,
				  "update CEMOD_PERMISSION_MODEL_SIZE in the permissions dialog to match");

	struct CemodPackageDescriptor
	{
		std::filesystem::path path;
		std::string modId;
		std::string principal;
		std::uint64_t requestedPermissions{};
		CemodExecutionMode executionMode{CemodExecutionMode::Isolated};
		bool signedPackage{};
		std::vector<std::uint64_t> titleIds;
		std::string discoveryError;
	};

	struct CemodApproval
	{
		std::string packageDigest;
		std::string modIdentity;
		std::uint64_t requestedPermissions{};
		std::uint64_t grantedPermissions{};
		bool approved{};
		bool headless{};
	};

	enum class CemodApprovalResult : std::uint8_t
	{
		Approved,
		NeedsReapproval,
		DeniedByDefault,
		DeniedHeadlessRequiresExplicitApproval,
	};

	struct CemodApprovalState
	{
		CemodApprovalResult result{CemodApprovalResult::DeniedByDefault};
		std::uint64_t requested{};
		std::uint64_t granted{};
		std::string reason;
	};

	struct CemodPermissionAssessment
	{
		CemodPermission permission{};
		std::uint64_t bit{};
		bool requested{};
		bool granted{};
		bool dangerous{};
		bool manifestMismatch{};
	};

	enum class CemodScope : std::uint8_t
	{
		Title,
		Process,
		AromaNative,
	};

	struct CemodInspection
	{
		std::filesystem::path path;
		std::string modId;
		std::string principal;
		std::string modIdentity;
		std::string packageDigest;
		std::string pluginName;
		std::string author;
		std::string pluginVersion;
		std::string license;
		std::string description;
		std::string wupsAbiVersion;
		std::string buildTimestamp;
		std::string storageId;
		std::vector<std::string> requiredModules;
		std::vector<std::string> scopeTargets;
		std::vector<std::uint32_t> processTargets;
		std::vector<std::string> compatibilityWarnings;
		std::vector<std::string> permissionMismatches;
		std::vector<CemodPermissionAssessment> permissions;
		std::string error;
		CemodApprovalState approval;
		CemodScope scope{CemodScope::Title};
		bool isWups{};
		bool signedPackage{};
		bool usesTls{};
		bool usesFixedAddressPatches{};

		[[nodiscard]] bool Valid() const
		{
			return error.empty();
		}
	};

	class CemodInspectionService final
	{
	  public:
		[[nodiscard]] static std::string MakeApprovalKey(std::string_view modIdentity,
														 std::string_view packageDigest);
		[[nodiscard]] static std::string CalculatePackageDigest(
			const std::filesystem::path& path, std::string& error);
		[[nodiscard]] static std::uint64_t PermissionBit(CemodPermission permission);
		[[nodiscard]] static bool IsDangerous(CemodPermission permission);
		[[nodiscard]] static std::uint64_t DefaultGrantedPermissions(std::uint64_t requested);
		[[nodiscard]] static CemodApprovalState EvaluateApproval(std::uint64_t requested,
																 const std::optional<CemodApproval>& approval, bool headless);
		[[nodiscard]] static CemodInspection Inspect(const CemodPackageDescriptor& package,
													 const std::optional<CemodApproval>& approval, bool headless = false);
	};
} // namespace CemuExtend
