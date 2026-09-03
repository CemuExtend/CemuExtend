#include "Common/precompiled.h"

#include "Cafe/OS/libs/cemuextend/cemuextend.h"
#include "Cafe/OS/libs/cemuextend/CemodPermission.h"
#include "Cafe/OS/libs/cemuextend/BridgeHost.h"
#include "Cafe/OS/libs/cemuextend/BuildId.h"
#include "Cafe/OS/libs/cemuextend/Cex2Host.h"
#include "Cafe/OS/libs/cemuextend/Cex2Owner.h"
#include "Cafe/OS/libs/cemuextend/Cex2Storage.h"
#include "Cemu/CemuExtend/CemodInspectionService.h"

#include "Cafe/HW/Espresso/ModExecutionContext.h"
#include "Cafe/HW/Espresso/WupsDynLoadInterception.h"
#include "Cafe/HW/Espresso/CemodRuntime.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl.h"
#include "cemuextend/transport.hpp"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>

namespace cemuextend_hle
{
	namespace
	{
		constexpr const char* kLibraryName = "cemuextend";

		template<ModMemoryPermission Permission>
		std::byte* ResolveGuest(PPCInterpreter_t* hCPU, uint32 address, uint32 size)
		{
			if (!address || !size)
				return nullptr;
			if (hCPU->modExecutionContext)
				return hCPU->modExecutionContext->Resolve(address, size, Permission);
			// Trusted ELF and WUPS callers are authenticated by CurrentOwner before
			// any CEX2 entrypoint reaches this helper. Both use the title's normal
			// Cafe address space, so requiring the legacy trusted-ELF owner here
			// incorrectly rejects valid WUPS buffers.
			if (!memory_isAddressRangeAccessible(address, size))
				return nullptr;
			return reinterpret_cast<std::byte*>(memory_getPointerFromVirtualOffset(address));
		}

		Cex2Owner* CurrentOwner(PPCInterpreter_t* hCPU)
		{
			if (hCPU->modExecutionContext)
				return hCPU->modExecutionContext;
			thread_local std::shared_ptr<Cex2Owner> wupsOwner;
			wupsOwner = cafe::wups::ResolveCurrentCex2Owner();
			if (wupsOwner && wupsOwner->TitleId() == CafeSystem::GetForegroundTitleId() &&
				!wupsOwner->IsStopped())
				return wupsOwner.get();
			auto* owner = GetCemodRuntime().TrustedOwner();
			return owner && owner->TitleId() == CafeSystem::GetForegroundTitleId() && !owner->IsStopped() ? owner : nullptr;
		}

		void Return(PPCInterpreter_t* hCPU, cemuextend::wire::Error result)
		{
			osLib_returnFromFunction(hCPU, static_cast<uint32>(static_cast<sint32>(result)));
		}

		void CEX2Query(PPCInterpreter_t* hCPU)
		{
			using namespace cemuextend;
			auto* owner = CurrentOwner(hCPU);
			if (!owner)
				return Return(hCPU, wire::Error::PermissionDenied);
			std::uint32_t outputSize{};
			switch (static_cast<transport::Query>(hCPU->gpr[3]))
			{
			case transport::Query::Info:
				outputSize = sizeof(transport::Info);
				break;
			case transport::Query::MemoryLayout:
				outputSize = sizeof(transport::MemoryLayout);
				break;
			case transport::Query::InfoV3:
				outputSize = sizeof(transport::InfoV3);
				break;
			default:
				return Return(hCPU, wire::Error::NotSupported);
			}
			if (hCPU->gpr[5] < outputSize)
				return Return(hCPU, wire::Error::InvalidArgument);
			auto* output = ResolveGuest<ModMemoryPermission::Write>(
				hCPU, hCPU->gpr[4], outputSize);
			if (!output)
				return Return(hCPU, wire::Error::InvalidArgument);
			std::array<std::byte, sizeof(transport::InfoV3)> hostOutput{};
			const auto result = static_cast<wire::Error>(Cex2Host::Instance().Query(
				*owner, hCPU->gpr[3], {hostOutput.data(), outputSize}));
			if (result == wire::Error::Ok)
				std::memcpy(output, hostOutput.data(), outputSize);
			Return(hCPU, result);
		}

		void CEX2Open(PPCInterpreter_t* hCPU)
		{
			using namespace cemuextend;
			auto* owner = CurrentOwner(hCPU);
			if (!owner)
				return Return(hCPU, wire::Error::PermissionDenied);
			const auto optionsSize = hCPU->gpr[4];
			if (optionsSize != sizeof(transport::OpenOptions) &&
				optionsSize != sizeof(transport::OpenOptionsV3))
				return Return(hCPU, wire::Error::InvalidArgument);
			auto* options =
				ResolveGuest<ModMemoryPermission::Read>(hCPU, hCPU->gpr[3], optionsSize);
			auto* output = ResolveGuest<ModMemoryPermission::Write>(hCPU, hCPU->gpr[5], sizeof(wire::Be32));
			if (!options || !output)
				return Return(hCPU, wire::Error::InvalidArgument);
			std::array<std::byte, sizeof(transport::OpenOptionsV3)> hostOptions{};
			std::memcpy(hostOptions.data(), options, optionsSize);
			uint32 session{};
			const auto result = static_cast<wire::Error>(Cex2Host::Instance().Open(
				*owner,
				{hostOptions.data(), optionsSize}, session));
			if (result == wire::Error::Ok)
			{
				const wire::Be32 encodedSession{session};
				std::memcpy(output, &encodedSession, sizeof(encodedSession));
			}
			Return(hCPU, result);
		}

		void CEX2Submit(PPCInterpreter_t* hCPU)
		{
			using namespace cemuextend;
			auto* owner = CurrentOwner(hCPU);
			if (!owner)
				return Return(hCPU, wire::Error::PermissionDenied);
			if (hCPU->gpr[5] > transport::kMaximumMessageSize)
				return Return(hCPU, wire::Error::TooLarge);
			auto* request = ResolveGuest<ModMemoryPermission::Read>(hCPU, hCPU->gpr[4], hCPU->gpr[5]);
			if (!request)
				return Return(hCPU, wire::Error::InvalidArgument);
			std::vector<std::byte> hostRequest(hCPU->gpr[5]);
			std::memcpy(hostRequest.data(), request, hostRequest.size());
			Return(hCPU, static_cast<wire::Error>(Cex2Host::Instance().Submit(
							 *owner, hCPU->gpr[3], hostRequest)));
		}

		void CEX2Poll(PPCInterpreter_t* hCPU)
		{
			using namespace cemuextend;
			auto* owner = CurrentOwner(hCPU);
			if (!owner)
				return Return(hCPU, wire::Error::PermissionDenied);
			if (hCPU->gpr[5] > transport::kMaximumMessageSize)
				return Return(hCPU, wire::Error::TooLarge);
			auto* output = ResolveGuest<ModMemoryPermission::Write>(hCPU, hCPU->gpr[4], hCPU->gpr[5]);
			auto* sizeOutput = ResolveGuest<ModMemoryPermission::Write>(hCPU, hCPU->gpr[6], sizeof(wire::Be32));
			if (!output || !sizeOutput)
				return Return(hCPU, wire::Error::InvalidArgument);
			std::vector<std::byte> hostOutput(hCPU->gpr[5]);
			uint32 outputSize{};
			const auto result = static_cast<wire::Error>(Cex2Host::Instance().Poll(
				*owner, hCPU->gpr[3], hostOutput, outputSize));
			if (result == wire::Error::Ok && outputSize <= hostOutput.size())
				std::memcpy(output, hostOutput.data(), outputSize);
			const wire::Be32 encodedSize{outputSize};
			std::memcpy(sizeOutput, &encodedSize, sizeof(encodedSize));
			Return(hCPU, result);
		}

		void CEX2Cancel(PPCInterpreter_t* hCPU)
		{
			using namespace cemuextend;
			auto* owner = CurrentOwner(hCPU);
			if (!owner)
				return Return(hCPU, wire::Error::PermissionDenied);
			Return(hCPU, static_cast<wire::Error>(Cex2Host::Instance().Cancel(
							 *owner, hCPU->gpr[3], hCPU->gpr[4])));
		}

		void CEX2Close(PPCInterpreter_t* hCPU)
		{
			using namespace cemuextend;
			auto* owner = CurrentOwner(hCPU);
			if (!owner)
				return Return(hCPU, wire::Error::PermissionDenied);
			Return(hCPU, static_cast<wire::Error>(Cex2Host::Instance().Close(
							 *owner, hCPU->gpr[3])));
		}

		sint32 CEXQuery(uint32, void*, uint32)
		{
			return static_cast<sint32>(cemuextend::wire::Error::AbiMismatch);
		}

		sint32 CEXRegister(uint32, void*, uint32, uint32be*)
		{
			return static_cast<sint32>(cemuextend::wire::Error::AbiMismatch);
		}

		sint32 CEXNotify(uint32, uint32)
		{
			return static_cast<sint32>(cemuextend::wire::Error::AbiMismatch);
		}

		sint32 CEXUnregister(uint32)
		{
			return static_cast<sint32>(cemuextend::wire::Error::AbiMismatch);
		}
	} // namespace

	// PermissionBit() is not constexpr, so mirror its shift here.
	static_assert((1ULL << static_cast<unsigned int>(CemuExtend::CemodPermission::ServiceRead)) ==
						  kCemodServiceReadApproval &&
					  (1ULL << static_cast<unsigned int>(CemuExtend::CemodPermission::ServiceCapture)) ==
						  kCemodServiceCaptureApproval,
				  "CemodPermission::Service* must match the kCemodService*Approval bits");

	CemodInspectionInfo InspectCemodPackage(const CemodPackageInfo& package,
											const std::optional<CemodInspectionApproval>& approval)
	{
		// The native/WUPS permission namespace is deliberately independent from
		// the six CEX2 service bits in manifest.requestedPermissions.  WUPS
		// inspection derives this mask itself; trusted ELF packages need the same
		// treatment here so native bit 0 can never be mistaken for CEX2 read.
		std::uint64_t inspectionPermissions = package.requestedPermissions;
		if (package.executionMode == CemodExecutionMode::TrustedNative)
		{
			std::string error;
			if (const auto inspected = CemodPackage::Inspect(package.path, error))
			{
				const auto& native = inspected->manifest.nativePermissions;
				const std::array declared{native.nativeMemory, native.functionPatching,
										  native.physicalAddressPatching, native.filesystemRead,
										  native.filesystemWrite, native.network, native.mappedMemory,
										  native.notifications, native.contentRedirection,
										  !native.modules.empty(), native.pluginManagement};
				inspectionPermissions = 0;
				for (std::size_t index = 0; index < declared.size(); ++index)
					if (declared[index])
						inspectionPermissions |= 1ULL << index;
			}
		}
		std::optional<CemuExtend::CemodApproval> domainApproval;
		if (approval)
			domainApproval = CemuExtend::CemodApproval{approval->packageDigest,
													   approval->modIdentity, approval->requestedPermissions,
													   approval->grantedPermissions, approval->approved, approval->headless};
		const auto inspection = CemuExtend::CemodInspectionService::Inspect({package.path, package.modId, package.principal, inspectionPermissions,
																			 package.executionMode == CemodExecutionMode::TrustedNative ? CemuExtend::CemodExecutionMode::TrustedNative : CemuExtend::CemodExecutionMode::Isolated,
																			 package.signedPackage, package.titleIds, package.error},
																			domainApproval,
																			approval && approval->headless);
		CemodInspectionInfo result;
		result.modId = inspection.modId;
		result.principal = inspection.principal;
		result.modIdentity = inspection.modIdentity;
		result.packageDigest = inspection.packageDigest;
		result.pluginName = inspection.pluginName;
		result.author = inspection.author;
		result.version = inspection.pluginVersion;
		result.description = inspection.description;
		switch (inspection.scope)
		{
		case CemuExtend::CemodScope::Process:
			result.scope = "process";
			break;
		case CemuExtend::CemodScope::AromaNative:
			result.scope = "aromaNative";
			break;
		default:
			result.scope = "title";
			break;
		}
		result.approvalReason = inspection.approval.reason;
		result.requestedPermissions = inspection.approval.requested;
		result.grantedPermissions = inspection.approval.granted;
		result.approved = inspection.approval.result == CemuExtend::CemodApprovalResult::Approved;
		result.signedPackage = inspection.signedPackage;
		result.trustedNative = package.executionMode == CemodExecutionMode::TrustedNative;
		result.wups = inspection.isWups;
		result.valid = inspection.Valid() && !inspection.packageDigest.empty();
		result.headless = approval && approval->headless;
		result.warnings = inspection.compatibilityWarnings;
		result.warnings.insert(result.warnings.end(), inspection.permissionMismatches.begin(),
							   inspection.permissionMismatches.end());
		if (!inspection.error.empty())
			result.warnings.push_back(inspection.error);
		auto permissionName = [](CemuExtend::CemodPermission permission) {
			switch (permission)
			{
			case CemuExtend::CemodPermission::NativeMemory:
				return "Native memory";
			case CemuExtend::CemodPermission::FunctionPatching:
				return "Function patching";
			case CemuExtend::CemodPermission::PhysicalAddressPatching:
				return "Physical-address patching";
			case CemuExtend::CemodPermission::FilesystemRead:
				return "Filesystem read";
			case CemuExtend::CemodPermission::FilesystemWrite:
				return "Filesystem write";
			case CemuExtend::CemodPermission::Network:
				return "Network";
			case CemuExtend::CemodPermission::MappedMemory:
				return "Mapped memory";
			case CemuExtend::CemodPermission::Notifications:
				return "Notifications";
			case CemuExtend::CemodPermission::ContentRedirection:
				return "Content redirection";
			case CemuExtend::CemodPermission::ServiceRead:
				return "Service: read";
			case CemuExtend::CemodPermission::ServiceWrite:
				return "Service: write";
			case CemuExtend::CemodPermission::ServiceInject:
				return "Service: inject";
			case CemuExtend::CemodPermission::ServiceClipboard:
				return "Service: clipboard";
			case CemuExtend::CemodPermission::ServiceCapture:
				return "Service: screen capture";
			case CemuExtend::CemodPermission::Modules:
				return "Aroma/WUMS modules";
			case CemuExtend::CemodPermission::PluginManagement:
				return "WUPS plugin management";
			case CemuExtend::CemodPermission::WebUi:
				return "Web UI";
			}
			return "Unknown permission";
		};
		for (const auto& permission : inspection.permissions)
			result.permissions.push_back({permissionName(permission.permission), permission.bit,
										  permission.requested, permission.granted, permission.dangerous,
										  permission.manifestMismatch});
		return result;
	}

	std::string MakeCemodApprovalKey(std::string_view modIdentity,
									 std::string_view packageDigest)
	{
		return CemuExtend::CemodInspectionService::MakeApprovalKey(modIdentity, packageDigest);
	}

	std::optional<CemodInspectionInfo> InspectConfiguredCemodPackage(
		std::uint64_t titleId, const CemodPackageInfo& package)
	{
		auto inspection = InspectCemodPackage(package);
		if (!inspection.valid || inspection.modIdentity.empty() || inspection.packageDigest.empty())
			return std::nullopt;
		const auto key = MakeCemodApprovalKey(inspection.modIdentity, inspection.packageDigest);
		auto configured = GetConfig().GetCemuExtendPermissionApproval(titleId, key);
		if (!configured && package.executionMode == CemodExecutionMode::Isolated &&
			(inspection.requestedPermissions & ~static_cast<std::uint64_t>(kCemodPermissionMask)) == 0)
		{
			// One-time compatibility migration.  A legacy principal grant is never
			// consulted as runtime fallback: it is copied only when it covers this
			// request, and the copy is bound to the currently inspected digest,
			// identity, and request set.  An existing exact deny always wins above.
			const auto legacy = GetConfig().GetCemuExtendModGrant(titleId, package.principal);
			if (legacy && legacy->approved &&
				(inspection.requestedPermissions & ~legacy->approved_request_mask) == 0)
			{
				CemuExtendPermissionApproval migrated{inspection.packageDigest,
													  inspection.modIdentity, inspection.requestedPermissions,
													  legacy->permissions & inspection.requestedPermissions, true, false};
				auto configLock = GetConfigHandle().Lock();
				// Re-check under the config lock so a concurrent exact deny cannot be
				// overwritten by migration.
				configured = GetConfig().GetCemuExtendPermissionApproval(titleId, key);
				if (!configured)
				{
					GetConfig().SetCemuExtendPermissionApproval(titleId, key, migrated);
					if (GetConfigHandle().Save())
						configured = std::move(migrated);
					else
						GetConfig().RemoveCemuExtendPermissionApproval(titleId, key);
				}
			}
		}
		if (!configured)
		{
			// The user approved this mod and asked to trust its updates. A rebuilt
			// package has a new digest and therefore no exact approval, so renew one
			// against the new digest - but only while the new build asks for nothing
			// beyond what was granted. A version that wants more is asked about again.
			const auto trusted =
				GetConfig().GetCemuExtendModUpdateTrust(titleId, inspection.modIdentity);
			if (trusted && (inspection.requestedPermissions & ~*trusted) == 0)
			{
				CemuExtendPermissionApproval renewed{inspection.packageDigest,
													 inspection.modIdentity, inspection.requestedPermissions,
													 *trusted & inspection.requestedPermissions, true, false};
				auto configLock = GetConfigHandle().Lock();
				configured = GetConfig().GetCemuExtendPermissionApproval(titleId, key);
				if (!configured)
				{
					GetConfig().SetCemuExtendPermissionApproval(titleId, key, renewed);
					if (GetConfigHandle().Save())
						configured = std::move(renewed);
					else
						GetConfig().RemoveCemuExtendPermissionApproval(titleId, key);
				}
			}
		}
		if (!configured)
			return std::nullopt;
		return InspectCemodPackage(package, CemodInspectionApproval{
												configured->packageDigest, configured->modIdentity,
												configured->requestedPermissions, configured->grantedPermissions,
												configured->approved, configured->explicitHeadlessDenial});
	}

	class CemuExtendModule final : public COSModule
	{
	  public:
		std::string_view GetName() override
		{
			return "cemuextend";
		}

		void RPLMapped() override
		{
			cafeExportRegister("cemuextend", CEXQuery, LogType::Placeholder);
			cafeExportRegister("cemuextend", CEXRegister, LogType::Placeholder);
			cafeExportRegister("cemuextend", CEXNotify, LogType::Placeholder);
			cafeExportRegister("cemuextend", CEXUnregister, LogType::Placeholder);
			osLib_addFunctionInternal(kLibraryName, "CEX2Query", CEX2Query);
			osLib_addFunctionInternal(kLibraryName, "CEX2Open", CEX2Open);
			osLib_addFunctionInternal(kLibraryName, "CEX2Submit", CEX2Submit);
			osLib_addFunctionInternal(kLibraryName, "CEX2Poll", CEX2Poll);
			osLib_addFunctionInternal(kLibraryName, "CEX2Cancel", CEX2Cancel);
			osLib_addFunctionInternal(kLibraryName, "CEX2Close", CEX2Close);
			// Packages are activated after Graphic Packs so both systems share the
			// codecave allocator deterministically.
		}

		void RPLUnmapped() override
		{
			// This callback may run while title PPC threads are live. UnloadAll only
			// revokes trusted access; CafeSystem owns its post-thread memory release.
			GetCemodRuntime().UnloadAll();
			Cex2Host::Instance().CloseAll();
		}

		void rpl_entry(uint32, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Unloaded)
			{
				GetCemodRuntime().UnloadAll();
				Cex2Host::Instance().CloseAll();
			}
		}
	};

	COSModule* GetModule()
	{
		static CemuExtendModule module;
		return &module;
	}

	void ConfigureCex2HleAccess(ModExecutionContext& context)
	{
		constexpr std::array names{"CEX2Query", "CEX2Open", "CEX2Submit", "CEX2Poll", "CEX2Cancel", "CEX2Close"};
		for (const auto* name : names)
		{
			const auto index = osLib_getFunctionIndex("cemuextend", name);
			if (index >= 0)
				context.AllowHle(static_cast<std::uint16_t>(index));
		}
	}

	CemodRuntime& GetCemodRuntime()
	{
		// Construct the session host first so the runtime is destroyed before it.
		(void)Cex2Host::Instance();
		static CemodRuntime runtime;
		return runtime;
	}

	CemuExtendModGrant ResolveCemodGrant(std::uint64_t titleId, const std::string& modId,
										 const std::string& principal, std::uint32_t requestedPermissions)
	{
		if (const auto exact = GetConfig().GetCemuExtendModGrant(titleId, principal))
			return *exact;
		const auto anchor = GetConfig().GetCemuExtendModTrustAnchor(titleId, modId);
		if (!anchor || !CemodTrustAnchorCoversRequest(requestedPermissions, anchor->approved_request_mask))
			return {};
		const CemuExtendModGrant grant{anchor->permissions & requestedPermissions & kCemodPermissionMask,
									   anchor->approved_request_mask, true};
		GetConfig().SetCemuExtendModGrant(titleId, principal, grant);
		return grant;
	}

	std::vector<CemodPackageInfo> DiscoverCemodCatalog()
	{
		namespace fs = std::filesystem;
		std::vector<CemodPackageInfo> result;
		const auto root = ActiveSettings::GetUserDataPath("cemuextend/mods");
		std::error_code error;
		fs::create_directories(root, error);
		if (error)
			return result;
		std::vector<fs::path> paths;
		for (fs::directory_iterator iterator(root, error), end; !error && iterator != end; ++iterator)
		{
			const auto status = iterator->symlink_status(error);
			if (error)
				break;
			if (fs::is_regular_file(status) && !fs::is_symlink(status) &&
				iterator->path().extension() == ".cemod")
				paths.push_back(iterator->path());
		}
		std::ranges::sort(paths);
		for (const auto& path : paths)
		{
			std::string packageError;
			auto package = CemodPackage::Inspect(path, packageError);
			if (!package)
			{
				result.push_back({path, {}, {}, 0, CemodExecutionMode::Isolated, false, {}, std::move(packageError)});
				continue;
			}
			result.push_back({path, package->manifest.modId, package->principal, package->manifest.requestedPermissions, package->manifest.executionMode, package->signedPackage, package->manifest.titleIds, {}, package->manifest.mem2ExpansionBytes, package->manifest.nativePermissions.mappedMemory});
		}
		return result;
	}

	std::vector<CemodPackageInfo> DiscoverCemods(std::uint64_t titleId)
	{
		std::vector<CemodPackageInfo> result;
		if (titleId == 0)
			return result;
		for (auto& package : DiscoverCemodCatalog())
		{
			if (!package.error.empty() || std::ranges::find(package.titleIds, titleId) != package.titleIds.end())
				result.push_back(std::move(package));
		}
		return result;
	}

	void ConfigureMemoryForTitle(std::uint64_t titleId)
	{
		std::uint32_t expansionBytes{};
		std::string requestingMod;
		for (const auto& package : DiscoverCemods(titleId))
		{
			if (!package.error.empty() ||
				package.executionMode != CemodExecutionMode::TrustedNative ||
				!package.mappedMemory || package.mem2ExpansionBytes == 0)
				continue;
			const auto exact = InspectConfiguredCemodPackage(titleId, package);
			constexpr auto kMappedMemory = 1ULL << static_cast<unsigned>(CemuExtend::CemodPermission::MappedMemory);
			if (!exact || !exact->approved ||
				(exact->grantedPermissions & kMappedMemory) == 0 ||
				package.mem2ExpansionBytes <= expansionBytes)
				continue;
			expansionBytes = package.mem2ExpansionBytes;
			requestingMod = package.modId;
		}

		constexpr std::uint32_t defaultMem2End =
			MEMORY_DATA_AREA_ADDR + MEMORY_DATA_AREA_SIZE;
		std::string error;
		if (!memory_requestMem2End(defaultMem2End + expansionBytes, error))
		{
			cemuLog_log(LogType::Force,
						"CemuExtend rejected the MEM2 request for title {:016x}: {}",
						titleId, error);
			return;
		}
		if (expansionBytes != 0)
			cemuLog_log(LogType::Force,
						"CemuExtend: '{}' requested {} MiB of additional MEM2 for title {:016x}",
						requestingMod, expansionBytes / (1024U * 1024U), titleId);
	}

	std::vector<CemodPermissionRequest> PendingCemodPermissionRequests(std::uint64_t titleId)
	{
		struct Aggregate
		{
			CemodPermissionRequest request;
			std::uint32_t missingPermissions{};
			bool needsApproval{};
		};
		std::map<std::string, Aggregate> grouped;
		for (const auto& package : DiscoverCemods(titleId))
		{
			// The legacy launch dialog only understands CEX2's six service bits.
			// Native/WUPS approvals are managed by the exact-digest manager.
			if (!package.error.empty() || package.executionMode == CemodExecutionMode::TrustedNative)
				continue;
			const auto exact = InspectConfiguredCemodPackage(titleId, package);
			const auto requested = package.requestedPermissions & kCemodPermissionMask;
			const auto exactGranted = exact && exact->approved ? static_cast<std::uint32_t>(exact->grantedPermissions) : 0U;
			const auto effectiveGranted = exactGranted & requested;
			auto found = grouped.try_emplace(package.principal,
											 Aggregate{CemodPermissionRequest{package.modId, package.principal, 0, 0,
																			  package.executionMode, package.signedPackage, exact && exact->headless}})
							 .first;
			auto& aggregate = found->second;
			aggregate.request.requestedPermissions |= requested;
			aggregate.missingPermissions |= requested & ~effectiveGranted;
			aggregate.needsApproval |= !exact || !exact->approved;
			aggregate.request.headless = aggregate.request.headless || (exact && exact->headless);
			aggregate.request.signedPackage = aggregate.request.signedPackage && package.signedPackage;
		}

		std::vector<CemodPermissionRequest> result;
		for (auto& [principal, aggregate] : grouped)
		{
			aggregate.request.grantedPermissions = aggregate.request.requestedPermissions &
												   ~aggregate.missingPermissions;
			// An exact approval may intentionally deny requested capabilities.  The
			// approval decision, rather than a legacy broad grant, is the launch gate.
			if (aggregate.needsApproval)
				result.push_back(std::move(aggregate.request));
		}
		std::ranges::sort(result, [](const auto& left, const auto& right) {
			if (left.modId != right.modId)
				return left.modId < right.modId;
			return left.principal < right.principal;
		});
		return result;
	}

	void LoadCemodsForTitle(std::uint64_t titleId)
	{
		if (titleId == 0)
			return;
		auto& runtime = GetCemodRuntime();
		std::string previousTitleError;
		if (!runtime.ReadyForNextTitle(previousTitleError))
		{
			cemuLog_log(LogType::Force,
						"CemuExtend refused to load a title before prior CEMod release: {}",
						previousTitleError);
			return;
		}
		const auto titleGrant = GetConfig().GetCemuExtendGrant(titleId).value_or(CemuExtendTitleGrant{
			kDefaultReadMask, kDefaultWriteMask, kDefaultInjectMask});
		const ModServicePermissions services{titleGrant.read_mask, titleGrant.write_mask,
											 titleGrant.inject_mask};
		struct ApprovedPackage
		{
			CemodPackage package;
			std::filesystem::path path;
			std::uint32_t permissions{};
		};
		std::vector<ApprovedPackage> approved;
		std::uint32_t trustedPermissions{};
		for (const auto& info : DiscoverCemods(titleId))
		{
			if (!info.error.empty())
			{
				cemuLog_log(LogType::Force, "CemuExtend rejected cemod '{}': {}",
							_pathToUtf8(info.path), info.error);
				continue;
			}
			// Inspect before loading, then inspect again after loading.  Approval is
			// accepted only when both observations and the loaded manifest agree, so
			// a package replacement cannot authorize different loaded bytes.
			const auto before = InspectConfiguredCemodPackage(titleId, info);
			if (before && !before->modIdentity.empty() &&
				!GetConfig().IsCemuExtendModEnabled(before->modIdentity))
			{
				cemuLog_log(LogType::Force, "CemuExtend skipped cemod '{}' switched off in the manager",
							_pathToUtf8(info.path));
				continue;
			}
			if (!before || !before->approved)
			{
				cemuLog_log(LogType::Force,
							"CemuExtend skipped cemod '{}' without an exact package approval",
							_pathToUtf8(info.path));
				continue;
			}
			// Service permissions are the user's to pick individually, so a declined
			// one denies that service rather than refusing to load the package. The
			// native permissions still have to be granted in full.
			if ((info.executionMode == CemodExecutionMode::TrustedNative || before->wups) &&
				(before->requestedPermissions & ~before->grantedPermissions &
				 ~kCemodServiceApprovalMask) != 0)
			{
				cemuLog_log(LogType::Force,
							"CemuExtend skipped native cemod '{}' with denied native permissions",
							_pathToUtf8(info.path));
				continue;
			}
			std::string error;
			auto package = CemodPackage::Load(info.path, titleId, error);
			if (!package)
				continue;
			const auto after = InspectConfiguredCemodPackage(titleId, info);
			if (!after || !after->approved || after->packageDigest != before->packageDigest ||
				after->modIdentity != before->modIdentity ||
				after->requestedPermissions != before->requestedPermissions ||
				package->manifest.modId != before->modId || package->principal != before->principal)
			{
				cemuLog_log(LogType::Force,
							"CemuExtend rejected cemod '{}' changed while it was being loaded",
							_pathToUtf8(info.path));
				continue;
			}
			// Exact grants are the sole runtime authority.  Only isolated CEX2
			// packages share the six-bit service namespace.  Native/WUPS permission
			// bits must never be truncated and reinterpreted as CEX2 permissions.
			const auto permissions = ExactRuntimeServicePermissions(before->grantedPermissions,
																	package->manifest.requestedPermissions,
																	info.executionMode == CemodExecutionMode::TrustedNative || before->wups);
			if (package->IsTrustedNative())
				trustedPermissions |= permissions;
			approved.push_back({std::move(*package), info.path, permissions});
		}
		std::ranges::sort(approved, [](const ApprovedPackage& left, const ApprovedPackage& right) {
			if (left.package.manifest.modId != right.package.manifest.modId)
				return left.package.manifest.modId < right.package.manifest.modId;
			return left.path < right.path;
		});
		const bool hasTrustedElf = std::ranges::any_of(approved, [](const ApprovedPackage& item) {
			return item.package.IsTrustedNative() &&
				   item.package.manifest.payload.format == CemodPayloadFormat::CemodElf;
		});
		if (hasTrustedElf)
		{
			std::string error;
			if (!runtime.BeginTrustedTitle(titleId, error))
			{
				cemuLog_log(LogType::Force,
							"CemuExtend refused trusted CEMods for the new title: {}", error);
				return;
			}
		}
		for (auto& item : approved)
		{
			if (runtime.Size() >= CemodRuntime::kMaximumModsPerTitle)
				break;
			std::string error;
			const auto effective = item.package.IsTrustedNative() ? trustedPermissions : item.permissions;
			if (!runtime.Load(std::move(item.package), effective, kCemodPermissionMask,
							  error, &services))
				cemuLog_log(LogType::Force, "CemuExtend failed to load cemod '{}': {}",
							_pathToUtf8(item.path), error);
		}
		runtime.EventAll(1); // title loaded
	}

	void NotifyApplicationStarts()
	{
		GetCemodRuntime().OnApplicationStarts();
	}

	void TickCemods()
	{
		GetCemodRuntime().TickAll();
	}

	void ReloadCemodPermissions(std::uint64_t titleId, std::string_view principal)
	{
		const auto titleGrant = GetConfig().GetCemuExtendGrant(titleId).value_or(CemuExtendTitleGrant{
			kDefaultReadMask, kDefaultWriteMask, kDefaultInjectMask});
		const ModServicePermissions services{titleGrant.read_mask, titleGrant.write_mask,
											 titleGrant.inject_mask};
		std::uint32_t permissions{};
		for (const auto& package : DiscoverCemods(titleId))
		{
			if (package.principal != principal || !package.error.empty())
				continue;
			const auto exact = InspectConfiguredCemodPackage(titleId, package);
			if (!exact || !exact->approved)
				continue;
			permissions |= ExactRuntimeServicePermissions(exact->grantedPermissions,
														  package.requestedPermissions,
														  package.executionMode == CemodExecutionMode::TrustedNative || exact->wups);
		}
		GetCemodRuntime().UpdatePermissions(principal, permissions, services);
		GetCemodRuntime().EventAll(2); // permissions changed
	}

	void ReloadCemodTitlePermissions(std::uint64_t titleId)
	{
		const auto titleGrant = GetConfig().GetCemuExtendGrant(titleId).value_or(CemuExtendTitleGrant{
			kDefaultReadMask, kDefaultWriteMask, kDefaultInjectMask});
		GetCemodRuntime().UpdateTitlePermissions({titleGrant.read_mask, titleGrant.write_mask,
												  titleGrant.inject_mask});
		GetCemodRuntime().EventAll(2); // permissions changed
	}

	bool ImportLegacyData(std::uint64_t titleId, std::string_view principal, std::string& error)
	{
		const auto status = Cex2Storage::ImportLegacy(titleId, principal);
		if (status == cemuextend::wire::Status::Ok)
		{
			error.clear();
			return true;
		}
		error = status == cemuextend::wire::Status::NotFound ? "No legacy title data was found." : status == cemuextend::wire::Status::Busy ? "The Mod already has ABI 2 data; nothing was overwritten."
																																			: "Legacy data failed validation or could not be copied.";
		return false;
	}
} // namespace cemuextend_hle
