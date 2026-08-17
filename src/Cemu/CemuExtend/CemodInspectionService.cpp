#include "Cemu/CemuExtend/CemodInspectionService.h"

#include "Cemu/CemuExtend/Formats/CemodPackage.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <ranges>
#include <sstream>

namespace CemuExtend
{
	namespace
	{
		std::string HexDigest(const unsigned char* digest, std::size_t size)
		{
			std::ostringstream stream;
			stream << std::hex << std::setfill('0');
			for (std::size_t index = 0; index < size; ++index)
				stream << std::setw(2) << static_cast<unsigned int>(digest[index]);
			return stream.str();
		}

		void AddMismatch(CemodInspection& inspection, CemodPermission permission,
			bool requested, bool declared, std::string message)
		{
			if (!requested || declared)
				return;
			inspection.permissionMismatches.emplace_back(std::move(message));
			const auto found = std::ranges::find_if(inspection.permissions,
				[permission](const auto& item) { return item.permission == permission; });
			if (found != inspection.permissions.end())
				found->manifestMismatch = true;
		}
	}

	std::string CemodInspectionService::MakeApprovalKey(std::string_view modIdentity,
		std::string_view packageDigest)
	{
		return std::string(modIdentity) + "|sha256:" + std::string(packageDigest);
	}

	std::string CemodInspectionService::CalculatePackageDigest(
		const std::filesystem::path& path, std::string& error)
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

	std::uint64_t CemodInspectionService::PermissionBit(CemodPermission permission)
	{
		return 1ULL << static_cast<unsigned int>(permission);
	}

	bool CemodInspectionService::IsDangerous(CemodPermission permission)
	{
		return permission != CemodPermission::FilesystemRead &&
			permission != CemodPermission::Notifications;
	}

	std::uint64_t CemodInspectionService::DefaultGrantedPermissions(std::uint64_t requested)
	{
		std::uint64_t granted{};
		for (const auto permission : kCemodPermissions)
			if (!IsDangerous(permission)) granted |= PermissionBit(permission);
		return granted & requested;
	}

	CemodApprovalState CemodInspectionService::EvaluateApproval(std::uint64_t requested,
		const std::optional<CemodApproval>& approval, bool headless)
	{
		CemodApprovalState state;
		state.requested = requested;
		state.granted = approval ? approval->grantedPermissions & requested :
			DefaultGrantedPermissions(requested);
		if (headless && (!approval || !approval->approved ||
			approval->requestedPermissions != requested))
		{
			state.result = CemodApprovalResult::DeniedHeadlessRequiresExplicitApproval;
			state.granted = 0;
			state.reason = "Headless mode explicitly denies permissions until this exact package digest and request set are approved in the GUI";
			return state;
		}
		if (!approval || !approval->approved)
		{
			state.result = CemodApprovalResult::DeniedByDefault;
			state.reason = "No explicit approval exists; dangerous permissions are denied by default";
			return state;
		}
		if (approval->requestedPermissions != requested || approval->packageDigest.empty() ||
			approval->modIdentity.empty())
		{
			state.result = CemodApprovalResult::NeedsReapproval;
			state.reason = "The package digest, mod identity, or requested permission set changed";
			return state;
		}
		state.result = CemodApprovalResult::Approved;
		state.reason = "Explicit approval matches this package identity and permission set";
		return state;
	}

	CemodInspection CemodInspectionService::Inspect(const CemodPackageDescriptor& descriptor,
		const std::optional<CemodApproval>& approval, bool headless)
	{
		CemodInspection result;
		result.path = descriptor.path;
		result.modId = descriptor.modId;
		result.principal = descriptor.principal;
		result.modIdentity = descriptor.modId.empty() ? descriptor.principal : descriptor.modId;
		result.signedPackage = descriptor.signedPackage;
		result.error = descriptor.discoveryError;
		if (!result.error.empty())
			return result;

		std::string digestError;
		result.packageDigest = CalculatePackageDigest(descriptor.path, digestError);
		if (!digestError.empty())
			result.compatibilityWarnings.push_back(digestError);
		std::string inspectError;
		const auto package = CemodPackage::Inspect(descriptor.path, inspectError);
		if (!package)
		{
			result.error = std::move(inspectError);
			return result;
		}

		result.modId = package->manifest.modId;
		result.modIdentity = result.modId.empty() ? descriptor.principal : result.modId;
		result.isWups = package->manifest.payload.format == CemodPayloadFormat::Wups;
		std::uint64_t requested = descriptor.requestedPermissions;
		const auto& declaredPermissions = package->manifest.nativePermissions;
		const std::array declared{
			declaredPermissions.nativeMemory, declaredPermissions.functionPatching,
			declaredPermissions.physicalAddressPatching, declaredPermissions.filesystemRead,
			declaredPermissions.filesystemWrite, declaredPermissions.network,
			declaredPermissions.mappedMemory, declaredPermissions.notifications,
			declaredPermissions.contentRedirection, !declaredPermissions.modules.empty(),
			declaredPermissions.pluginManagement};
		if (result.isWups)
		{
			requested = 0;
			for (std::size_t index = 0; index < declared.size(); ++index)
				if (declared[index]) requested |= 1ULL << index;
		}
		result.approval = EvaluateApproval(requested, approval, headless);
		if (approval && (approval->packageDigest != result.packageDigest ||
			approval->modIdentity != result.modIdentity))
		{
			result.approval.result = headless ?
				CemodApprovalResult::DeniedHeadlessRequiresExplicitApproval :
				CemodApprovalResult::NeedsReapproval;
			result.approval.granted = 0;
			result.approval.reason = headless ?
				"Headless mode explicitly denies a package whose digest or mod identity changed" :
				"The package digest or mod identity does not match this installed package";
		}

		switch (package->manifest.scope.type)
		{
		case CemodScopeType::Title: result.scope = CemodScope::Title; break;
		case CemodScopeType::Process:
			result.scope = CemodScope::Process;
			result.scopeTargets = package->manifest.scope.targets;
			break;
		case CemodScopeType::AromaNative: result.scope = CemodScope::AromaNative; break;
		}

		for (const auto permission : kCemodPermissions)
		{
			const auto bit = PermissionBit(permission);
			const auto index = static_cast<std::size_t>(permission);
			result.permissions.push_back({permission, bit, (requested & bit) != 0,
				(result.approval.granted & bit) != 0, IsDangerous(permission),
				(requested & bit) != 0 && !declared[index]});
		}

		if (package->wups)
		{
			const auto& wups = *package->wups;
			result.pluginName = wups.metadata.name;
			result.author = wups.metadata.author;
			result.pluginVersion = wups.metadata.version;
			result.license = wups.metadata.license;
			result.description = wups.metadata.description;
			result.wupsAbiVersion = wups.metadata.abiVersion.ToString();
			result.buildTimestamp = wups.metadata.buildTimestamp;
			result.storageId = wups.metadata.storageId;
			result.requiredModules = wups.requiredModules;
			result.processTargets = wups.processTargets;
			result.compatibilityWarnings.insert(result.compatibilityWarnings.end(),
				wups.compatibilityWarnings.begin(), wups.compatibilityWarnings.end());
			result.usesTls = wups.usesTls;
			result.usesFixedAddressPatches = wups.usesFixedAddressPatches;
			for (const auto& module : wups.requiredModules)
				if (std::ranges::find(declaredPermissions.modules, module) ==
					declaredPermissions.modules.end())
				{
					result.permissionMismatches.push_back("Required module '" + module +
						"' is not declared in manifest permissions");
					const auto modules = std::ranges::find_if(result.permissions,
						[](const auto& item) {
							return item.permission == CemodPermission::Modules;
						});
					if (modules != result.permissions.end())
						modules->manifestMismatch = true;
				}
			AddMismatch(result, CemodPermission::FunctionPatching, !wups.replacements.empty(),
				declaredPermissions.functionPatching,
				"Function patching is required by the payload but not declared in manifest permissions");
			AddMismatch(result, CemodPermission::PhysicalAddressPatching,
				wups.usesFixedAddressPatches, declaredPermissions.physicalAddressPatching,
				"Physical-address patching is required by the payload but not declared in manifest permissions");
			AddMismatch(result, CemodPermission::NativeMemory, wups.usesTls,
				declaredPermissions.nativeMemory,
				"Native memory is required by the payload but not declared in manifest permissions");
		}
		return result;
	}
}
