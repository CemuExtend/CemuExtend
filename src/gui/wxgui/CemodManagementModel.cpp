#include "CemodManagementModel.h"

#include "Cafe/HW/Espresso/CemodPackage.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{
	constexpr std::array kPermissions{
		CemodGuiPermission::NativeMemory,
		CemodGuiPermission::FunctionPatching,
		CemodGuiPermission::PhysicalAddressPatching,
		CemodGuiPermission::FilesystemRead,
		CemodGuiPermission::FilesystemWrite,
		CemodGuiPermission::Network,
		CemodGuiPermission::MappedMemory,
		CemodGuiPermission::Notifications,
		CemodGuiPermission::ContentRedirection,
		CemodGuiPermission::Modules,
		CemodGuiPermission::PluginManagement,
	};

	std::string HexDigest(const unsigned char* digest, std::size_t size)
	{
		std::ostringstream stream;
		stream << std::hex << std::setfill('0');
		for (std::size_t index = 0; index < size; ++index)
			stream << std::setw(2) << static_cast<unsigned int>(digest[index]);
		return stream.str();
	}

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
			if (index != 0) stream << ", ";
			stream << values[index];
		}
		return stream.str();
	}

	bool IsDangerous(CemodGuiPermission permission)
	{
		return permission != CemodGuiPermission::FilesystemRead &&
			permission != CemodGuiPermission::Notifications;
	}

	void AddMismatch(CemodPluginView& view, CemodGuiPermission permission, bool requested,
		bool declared)
	{
		if (requested && !declared)
			view.permissionMismatches.push_back(CemodGuiAdapter::PermissionName(permission) +
				" is required by the payload but not declared in manifest permissions");
	}
}

std::string CemodGuiAdapter::MakeApprovalKey(std::string_view modIdentity,
	std::string_view packageDigest)
{
	return std::string(modIdentity) + "|sha256:" + std::string(packageDigest);
}

std::string CemodGuiAdapter::CalculatePackageDigest(const std::filesystem::path& path,
	std::string& error)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		error = "cannot open package for approval digest";
		return {};
	}
	struct DigestContextDeleter
	{
		void operator()(EVP_MD_CTX* context) const { EVP_MD_CTX_free(context); }
	};
	std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context(EVP_MD_CTX_new());
	if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
	{
		error = "cannot initialize SHA-256 for approval digest";
		return {};
	}
	std::array<char, 64 * 1024> buffer{};
	while (input)
	{
		input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const auto count = input.gcount();
		if (count > 0 && EVP_DigestUpdate(context.get(), buffer.data(),
			static_cast<std::size_t>(count)) != 1)
		{
			error = "cannot update package approval digest";
			return {};
		}
	}
	if (!input.eof())
	{
		error = "cannot read package approval digest";
		return {};
	}
	std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
	unsigned int digestSize{};
	if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestSize) != 1)
	{
		error = "cannot finalize package approval digest";
		return {};
	}
	return HexDigest(digest.data(), digestSize);
}

std::uint64_t CemodGuiAdapter::PermissionBit(CemodGuiPermission permission)
{
	return 1ULL << static_cast<unsigned int>(permission);
}

std::string CemodGuiAdapter::PermissionName(CemodGuiPermission permission)
{
	switch (permission)
	{
	case CemodGuiPermission::NativeMemory: return "Native memory";
	case CemodGuiPermission::FunctionPatching: return "Function patching";
	case CemodGuiPermission::PhysicalAddressPatching: return "Physical-address patching";
	case CemodGuiPermission::FilesystemRead: return "Filesystem read";
	case CemodGuiPermission::FilesystemWrite: return "Filesystem write";
	case CemodGuiPermission::Network: return "Network";
	case CemodGuiPermission::MappedMemory: return "Mapped memory";
	case CemodGuiPermission::Notifications: return "Notifications";
	case CemodGuiPermission::ContentRedirection: return "Content redirection";
	case CemodGuiPermission::Modules: return "Aroma/WUMS modules";
	case CemodGuiPermission::PluginManagement: return "WUPS plugin management";
	}
	return "Unknown permission";
}

std::uint64_t CemodGuiAdapter::DefaultGrantedPermissions(std::uint64_t requested)
{
	std::uint64_t result{};
	for (const auto permission : kPermissions)
		if (!IsDangerous(permission)) result |= PermissionBit(permission);
	return result & requested;
}

CemodGuiApprovalState CemodGuiAdapter::EvaluateApproval(std::uint64_t requested,
	const std::optional<CemuExtendPermissionApproval>& approval, bool headless)
{
	CemodGuiApprovalState state{};
	state.requested = requested;
	state.granted = approval ? approval->grantedPermissions & requested :
		DefaultGrantedPermissions(requested);
	if (headless && (!approval || !approval->approved || approval->requestedPermissions != requested))
	{
		state.result = CemodGuiApprovalResult::DeniedHeadlessRequiresExplicitApproval;
		state.granted = 0;
		state.reason = "Headless mode explicitly denies permissions until this exact package digest and request set are approved in the GUI";
		return state;
	}
	if (!approval || !approval->approved)
	{
		state.result = CemodGuiApprovalResult::DeniedByDefault;
		state.reason = "No explicit approval exists; dangerous permissions are denied by default";
		return state;
	}
	if (approval->requestedPermissions != requested || approval->packageDigest.empty() ||
		approval->modIdentity.empty())
	{
		state.result = CemodGuiApprovalResult::NeedsReapproval;
		state.reason = "The package digest, mod identity, or requested permission set changed";
		return state;
	}
	state.result = CemodGuiApprovalResult::Approved;
	state.reason = "Explicit approval matches this package identity and permission set";
	return state;
}

CemodPluginView CemodGuiAdapter::InspectPlugin(const CemodGuiPackageInfo& info,
	std::uint64_t titleId, const std::optional<CemuExtendPermissionApproval>& approval,
	bool headless)
{
	CemodPluginView view{};
	view.path = info.path;
	view.modId = info.modId;
	view.principal = info.principal;
	view.modIdentity = info.modId.empty() ? info.principal : info.modId;
	view.signedPackage = info.signedPackage;
	view.lastError = info.error;
	view.payloadFormat = info.executionMode == CemodGuiExecutionMode::TrustedNative ?
		"cemod_elf or WUPS" : "cemod_elf";
	view.scope = "title " + (titleId == 0 ? std::string("all configured titles") :
		[&] { std::ostringstream s; s << std::hex << titleId; return s.str(); }());
	if (!info.error.empty())
	{
		view.loadStatus = CemodGuiLoadStatus::Rejected;
		view.statusText = "Rejected: " + info.error;
		return view;
	}

	std::string digestError;
	view.packageDigest = CalculatePackageDigest(info.path, digestError);
	if (view.packageDigest.empty()) view.compatibilityWarnings.push_back(digestError);
	std::string inspectError;
	const auto package = CemodPackage::Inspect(info.path, inspectError);
	if (!package)
	{
		view.lastError = inspectError;
		view.loadStatus = CemodGuiLoadStatus::Rejected;
		view.statusText = "Rejected: " + inspectError;
		return view;
	}
	view.modId = package->manifest.modId;
	view.modIdentity = package->manifest.modId.empty() ? info.principal : package->manifest.modId;
	view.isWups = package->manifest.payload.format == CemodPayloadFormat::Wups;
	view.payloadFormat = view.isWups ? "WUPS plugin.wps" : "Cemu ELF mod.elf";
	std::uint64_t requestedPermissions = info.requestedPermissions;
	if (view.isWups)
	{
		const auto& declared = package->manifest.nativePermissions;
		const std::array requested{
			declared.nativeMemory, declared.functionPatching,
			declared.physicalAddressPatching, declared.filesystemRead,
			declared.filesystemWrite, declared.network, declared.mappedMemory,
			declared.notifications, declared.contentRedirection, !declared.modules.empty(),
			declared.pluginManagement};
		requestedPermissions = 0;
		for (std::size_t index = 0; index < requested.size(); ++index)
			if (requested[index]) requestedPermissions |= 1ULL << index;
	}
	view.approval = EvaluateApproval(requestedPermissions, approval, headless);
	if (approval && (approval->packageDigest != view.packageDigest ||
		approval->modIdentity != view.modIdentity))
	{
		view.approval.result = headless ?
			CemodGuiApprovalResult::DeniedHeadlessRequiresExplicitApproval :
			CemodGuiApprovalResult::NeedsReapproval;
		view.approval.granted = 0;
		view.approval.reason = headless ?
			"Headless mode explicitly denies a package whose digest or mod identity changed" :
			"The package digest or mod identity does not match this installed package";
	}
	view.enabled = view.approval.result == CemodGuiApprovalResult::Approved;
	view.loadStatus = view.enabled ? CemodGuiLoadStatus::Ready : CemodGuiLoadStatus::Disabled;
	switch (package->manifest.scope.type)
	{
	case CemodScopeType::Title: view.scope = "title scope"; break;
	case CemodScopeType::Process:
		view.scope = "process scope";
		if (!package->manifest.scope.targets.empty())
			view.scope += ": " + JoinValues(package->manifest.scope.targets);
		break;
	case CemodScopeType::AromaNative: view.scope = "Aroma native scope"; break;
	}

	const auto& permissions = package->manifest.nativePermissions;
	const std::array declared{
		permissions.nativeMemory, permissions.functionPatching,
		permissions.physicalAddressPatching, permissions.filesystemRead,
		permissions.filesystemWrite, permissions.network, permissions.mappedMemory,
		permissions.notifications, permissions.contentRedirection, !permissions.modules.empty(),
		permissions.pluginManagement};
	for (const auto permission : kPermissions)
	{
		const auto bit = PermissionBit(permission);
		const auto index = static_cast<std::size_t>(permission);
		view.permissions.push_back({permission, PermissionName(permission), bit,
			(requestedPermissions & bit) != 0, (view.approval.granted & bit) != 0,
			IsDangerous(permission), false});
		if ((requestedPermissions & bit) != 0 && !declared[index])
			view.permissionMismatches.push_back(PermissionName(permission) +
				" is requested by the legacy permission mask but not declared by manifest");
	}

	if (!view.isWups)
	{
		view.runtimeAvailability = CemodGuiRuntimeAvailability::Available;
		view.statusText = view.enabled ? "Ready" : "Disabled until explicitly approved";
		view.configCategories = UnavailableConfig();
		view.notifications = UnavailableNotifications();
		return view;
	}

	view.runtimeAvailability = CemodGuiRuntimeAvailability::UnavailableRequiresRuntimeIntegration;
	view.loadStatus = CemodGuiLoadStatus::Unavailable;
	view.restartRequired = true;
	view.reloadRequired = true;
	if (package->wups)
	{
		const auto& wups = *package->wups;
		view.pluginName = wups.metadata.name;
		view.author = wups.metadata.author;
		view.pluginVersion = wups.metadata.version;
		view.license = wups.metadata.license;
		view.description = wups.metadata.description;
		view.wupsAbiVersion = wups.metadata.abiVersion.ToString();
		view.buildTimestamp = wups.metadata.buildTimestamp;
		view.storageId = wups.metadata.storageId;
		view.requiredModules = wups.requiredModules;
		for (const auto target : wups.processTargets) view.processTargets.push_back(ProcessName(target));
		view.compatibilityWarnings = wups.compatibilityWarnings;
		view.usesTls = wups.usesTls;
		view.usesFixedAddressPatches = wups.usesFixedAddressPatches;
		view.abiWarning = wups.metadata.abiVersion == WupsVersion{0, 9, 1} ?
			std::string{} : "Legacy WUPS ABI; runtime compatibility handling is not connected";
		for (const auto& module : wups.requiredModules)
			if (std::ranges::find(permissions.modules, module) == permissions.modules.end())
				view.permissionMismatches.push_back("Required module '" + module +
					"' is not declared in manifest permissions");
		AddMismatch(view, CemodGuiPermission::FunctionPatching, !wups.replacements.empty(),
			permissions.functionPatching);
		AddMismatch(view, CemodGuiPermission::PhysicalAddressPatching, wups.usesFixedAddressPatches,
			permissions.physicalAddressPatching);
		AddMismatch(view, CemodGuiPermission::NativeMemory, wups.usesTls, permissions.nativeMemory);
		if (!wups.requiredModules.empty()) view.permissions.back().manifestMismatch = permissions.modules.empty();
	}
	view.configCategories = UnavailableConfig();
	view.notifications = UnavailableNotifications();
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
