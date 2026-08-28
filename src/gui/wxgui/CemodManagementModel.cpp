#include "CemodManagementModel.h"

#include <iomanip>
#include <sstream>

namespace
{
	std::string ProcessName(std::uint32_t target)
	{
		std::ostringstream stream;
		stream << "0x" << std::hex << std::setfill('0') << std::setw(8) << target;
		return stream.str();
	}

	std::string JoinValues(const std::vector<std::string>& values)
	{
		std::ostringstream stream;
		for (std::size_t index = 0; index < values.size(); ++index)
		{
			if (index != 0)
				stream << ", ";
			stream << values[index];
		}
		return stream.str();
	}
} // namespace

std::string CemodGuiAdapter::MakeApprovalKey(std::string_view modIdentity,
											 std::string_view packageDigest)
{
	return CemuExtend::CemodInspectionService::MakeApprovalKey(modIdentity, packageDigest);
}

std::string CemodGuiAdapter::CalculatePackageDigest(const std::filesystem::path& path,
													std::string& error)
{
	return CemuExtend::CemodInspectionService::CalculatePackageDigest(path, error);
}

std::uint64_t CemodGuiAdapter::PermissionBit(CemodGuiPermission permission)
{
	return CemuExtend::CemodInspectionService::PermissionBit(permission);
}

std::string CemodGuiAdapter::PermissionName(CemodGuiPermission permission)
{
	switch (permission)
	{
	case CemodGuiPermission::NativeMemory:
		return "Native memory";
	case CemodGuiPermission::FunctionPatching:
		return "Function patching";
	case CemodGuiPermission::PhysicalAddressPatching:
		return "Physical-address patching";
	case CemodGuiPermission::FilesystemRead:
		return "Filesystem read";
	case CemodGuiPermission::FilesystemWrite:
		return "Filesystem write";
	case CemodGuiPermission::Network:
		return "Network";
	case CemodGuiPermission::MappedMemory:
		return "Mapped memory";
	case CemodGuiPermission::Notifications:
		return "Notifications";
	case CemodGuiPermission::ContentRedirection:
		return "Content redirection";
	case CemodGuiPermission::Modules:
		return "Aroma/WUMS modules";
	case CemodGuiPermission::PluginManagement:
		return "WUPS plugin management";
	}
	return "Unknown permission";
}

std::uint64_t CemodGuiAdapter::DefaultGrantedPermissions(std::uint64_t requested)
{
	return CemuExtend::CemodInspectionService::DefaultGrantedPermissions(requested);
}

CemodGuiApprovalState CemodGuiAdapter::EvaluateApproval(std::uint64_t requested,
														const std::optional<CemuExtend::CemodApproval>& approval, bool headless)
{
	return CemuExtend::CemodInspectionService::EvaluateApproval(requested, approval, headless);
}

CemodPluginView CemodGuiAdapter::InspectPlugin(const CemodGuiPackageInfo& info,
											   std::uint64_t titleId, const std::optional<CemuExtend::CemodApproval>& approval,
											   bool headless)
{
	const auto inspection = CemuExtend::CemodInspectionService::Inspect({info.path, info.modId, info.principal, info.requestedPermissions, info.executionMode,
																		 info.signedPackage, info.titleIds, info.error},
																		approval, headless);

	CemodPluginView view;
	view.path = inspection.path;
	view.modId = inspection.modId;
	view.principal = inspection.principal;
	view.modIdentity = inspection.modIdentity;
	view.packageDigest = inspection.packageDigest;
	view.pluginName = inspection.pluginName;
	view.author = inspection.author;
	view.pluginVersion = inspection.pluginVersion;
	view.license = inspection.license;
	view.description = inspection.description;
	view.wupsAbiVersion = inspection.wupsAbiVersion;
	view.buildTimestamp = inspection.buildTimestamp;
	view.storageId = inspection.storageId;
	view.requiredModules = inspection.requiredModules;
	view.compatibilityWarnings = inspection.compatibilityWarnings;
	view.permissionMismatches = inspection.permissionMismatches;
	view.lastError = inspection.error;
	view.approval = inspection.approval;
	view.isWups = inspection.isWups;
	view.signedPackage = inspection.signedPackage;
	view.usesTls = inspection.usesTls;
	view.usesFixedAddressPatches = inspection.usesFixedAddressPatches;
	view.payloadFormat = inspection.isWups ? "WUPS plugin.wps" : (info.executionMode == CemodGuiExecutionMode::TrustedNative ? "Cemu ELF mod.elf" : "cemod_elf");
	for (const auto target : inspection.processTargets)
		view.processTargets.push_back(ProcessName(target));

	if (!inspection.Valid())
	{
		view.loadStatus = CemodGuiLoadStatus::Rejected;
		view.statusText = "Rejected: " + inspection.error;
		return view;
	}

	switch (inspection.scope)
	{
	case CemuExtend::CemodScope::Title:
		view.scope = "title " + (titleId == 0 ? std::string("all configured titles") : [&] {
						 std::ostringstream stream;
						 stream << std::hex << titleId;
						 return stream.str();
					 }());
		break;
	case CemuExtend::CemodScope::Process:
		view.scope = "process scope";
		if (!inspection.scopeTargets.empty())
			view.scope += ": " + JoinValues(inspection.scopeTargets);
		break;
	case CemuExtend::CemodScope::AromaNative:
		view.scope = "Aroma native scope";
		break;
	}

	for (const auto& permission : inspection.permissions)
	{
		view.permissions.push_back({permission.permission, PermissionName(permission.permission),
									permission.bit, permission.requested, permission.granted, permission.dangerous,
									permission.manifestMismatch});
		if (permission.manifestMismatch && !inspection.isWups)
			view.permissionMismatches.push_back(PermissionName(permission.permission) +
												" is requested by the legacy permission mask but not declared by manifest");
	}

	view.enabled = view.approval.result == CemodGuiApprovalResult::Approved;
	view.loadStatus = view.enabled ? CemodGuiLoadStatus::Ready : CemodGuiLoadStatus::Disabled;
	view.configCategories = UnavailableConfig();
	view.notifications = UnavailableNotifications();
	if (!view.isWups)
	{
		view.runtimeAvailability = CemodGuiRuntimeAvailability::Available;
		view.statusText = view.enabled ? "Ready" : "Disabled until explicitly approved";
		return view;
	}

	view.runtimeAvailability = CemodGuiRuntimeAvailability::UnavailableRequiresRuntimeIntegration;
	view.loadStatus = CemodGuiLoadStatus::Unavailable;
	view.restartRequired = true;
	view.reloadRequired = true;
	view.abiWarning = view.wupsAbiVersion == "0.9.1" ? std::string{} : "Legacy WUPS ABI; runtime compatibility handling is not connected";
	view.statusText = "Unavailable — Requires runtime integration";
	if (!view.permissionMismatches.empty())
		view.statusText += "; manifest permission mismatch";
	return view;
}

std::vector<CemodGuiConfigCategory> CemodGuiAdapter::UnavailableConfig()
{
	return {{"runtime", "Plugin configuration", {}, false}};
}

std::vector<CemodGuiNotification> CemodGuiAdapter::UnavailableNotifications()
{
	return {{"cemuextend", "Notifications", "Unavailable — Requires runtime integration",
			 CemodGuiNotificationSeverity::Warning, 0, false}};
}
