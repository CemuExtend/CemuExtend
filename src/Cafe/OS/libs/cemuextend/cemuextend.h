#pragma once

#include "Cafe/HW/Espresso/CemodPackage.h"
#include "Cafe/OS/RPL/COSModule.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ModExecutionContext;
class CemodRuntime;
struct CemuExtendModGrant;

struct CemodPackageInfo
{
	std::filesystem::path path;
	std::string modId;
	std::string principal;
	std::uint32_t requestedPermissions{};
	CemodExecutionMode executionMode{CemodExecutionMode::Isolated};
	bool signedPackage{};
	std::vector<std::uint64_t> titleIds;
	std::string error;
	std::uint32_t mem2ExpansionBytes{};
	bool mappedMemory{};
};

struct CemodPermissionRequest
{
	std::string modId;
	std::string principal;
	std::uint32_t requestedPermissions{};
	std::uint32_t grantedPermissions{};
	CemodExecutionMode executionMode{CemodExecutionMode::Isolated};
	bool signedPackage{};
	bool headless{};
};

struct CemodInspectionApproval
{
	std::string packageDigest;
	std::string modIdentity;
	std::uint64_t requestedPermissions{};
	std::uint64_t grantedPermissions{};
	bool approved{};
	bool headless{};
};

struct CemodInspectionPermission
{
	std::string name;
	std::uint64_t bit{};
	bool requested{};
	bool granted{};
	bool dangerous{};
	bool manifestMismatch{};
};

struct CemodInspectionInfo
{
	std::string modId;
	std::string principal;
	std::string modIdentity;
	std::string packageDigest;
	std::string pluginName;
	std::string author;
	std::string version;
	std::string description;
	std::string scope;
	std::string approvalReason;
	std::vector<std::string> warnings;
	std::vector<CemodInspectionPermission> permissions;
	std::uint64_t requestedPermissions{};
	std::uint64_t grantedPermissions{};
	bool approved{};
	bool signedPackage{};
	bool trustedNative{};
	bool wups{};
	bool headless{};
	bool valid{};
};

namespace cemuextend_hle
{
	COSModule* GetModule();
	void ConfigureCex2HleAccess(ModExecutionContext& context);
	CemodRuntime& GetCemodRuntime();
	::CemuExtendModGrant ResolveCemodGrant(std::uint64_t titleId, const std::string& modId,
										   const std::string& principal, std::uint32_t requestedPermissions);
	std::vector<CemodPackageInfo> DiscoverCemodCatalog();
	std::vector<CemodPackageInfo> DiscoverCemods(std::uint64_t titleId);
	CemodInspectionInfo InspectCemodPackage(const CemodPackageInfo& package,
											const std::optional<CemodInspectionApproval>& approval = std::nullopt);
	std::optional<CemodInspectionInfo> InspectConfiguredCemodPackage(
		std::uint64_t titleId, const CemodPackageInfo& package);
	std::string MakeCemodApprovalKey(std::string_view modIdentity,
									 std::string_view packageDigest);
	std::vector<CemodPermissionRequest> PendingCemodPermissionRequests(std::uint64_t titleId);
	void ConfigureMemoryForTitle(std::uint64_t titleId);
	void LoadCemodsForTitle(std::uint64_t titleId);
	// Called from coreinit_start on the application's PPC thread, once heaps are ready
	// and before the title entrypoint runs, to start WUPS plugins in guest context.
	void NotifyApplicationStarts();
	void TickCemods();
	void ReloadCemodPermissions(std::uint64_t titleId, std::string_view principal);
	void ReloadCemodTitlePermissions(std::uint64_t titleId);
	bool ImportLegacyData(std::uint64_t titleId, std::string_view principal, std::string& error);
} // namespace cemuextend_hle
