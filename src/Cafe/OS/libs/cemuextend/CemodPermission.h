#pragma once

#include <cstdint>
#include <utility>

namespace cemuextend_hle
{
	// Runtime CEX2 permission namespace: read | write | inject | clipboard |
	// capture | network | ui.
	constexpr std::uint32_t kCemodNetworkPermission = 1U << 5U;
	constexpr std::uint32_t kCemodUiPermission = 1U << 6U;
	constexpr std::uint32_t kCemodLegacyPermissionMask = 0x3fU;
	constexpr std::uint32_t kCemodPermissionMask = 0x7fU;
	// Exact approval namespace. Native/WUPS permissions occupy bits 0..10;
	// Web UI deliberately lives outside both that range and runtime bit 6.
	constexpr std::uint64_t kCemodWebUiApprovalPermission = 1ULL << 11U;
	// Approval bits for the CEX2 services, matching CemodPermission::Service* in
	// CemodInspectionService.h. A static_assert in cemuextend.cpp pins them together.
	constexpr std::uint64_t kCemodServiceReadApproval = 1ULL << 12U;
	constexpr std::uint64_t kCemodServiceWriteApproval = 1ULL << 13U;
	constexpr std::uint64_t kCemodServiceInjectApproval = 1ULL << 14U;
	constexpr std::uint64_t kCemodServiceClipboardApproval = 1ULL << 15U;
	constexpr std::uint64_t kCemodServiceCaptureApproval = 1ULL << 16U;
	constexpr std::uint64_t kCemodServiceApprovalMask =
		kCemodServiceReadApproval | kCemodServiceWriteApproval | kCemodServiceInjectApproval |
		kCemodServiceClipboardApproval | kCemodServiceCaptureApproval;
	// Everything an exact approval can express: the native permissions, Web UI, and
	// the services above. A package asking for anything outside this is rejected.
	constexpr std::uint64_t kCemodExactApprovalPermissionMask =
		(kCemodServiceCaptureApproval << 1U) - 1U;

	[[nodiscard]] constexpr std::uint32_t ExactRuntimeServicePermissions(
		std::uint64_t exactGranted, std::uint32_t serviceRequested, bool nativePackage)
	{
		std::uint32_t result{};
		if (nativePackage)
		{
			// A native package's approval speaks the native namespace, so its service
			// permissions are approved through their own bits rather than by
			// reinterpreting native ones. Each service is granted only when the
			// manifest asked for it and the user approved that exact bit.
			constexpr std::pair<std::uint64_t, std::uint32_t> services[]{
				{kCemodServiceReadApproval, 1U << 0U},
				{kCemodServiceWriteApproval, 1U << 1U},
				{kCemodServiceInjectApproval, 1U << 2U},
				{kCemodServiceClipboardApproval, 1U << 3U},
				{kCemodServiceCaptureApproval, 1U << 4U},
			};
			for (const auto& [approvalBit, serviceBit] : services)
				if ((serviceRequested & serviceBit) != 0 && (exactGranted & approvalBit) != 0)
					result |= serviceBit;
		}
		else
			result |= static_cast<std::uint32_t>(exactGranted) & serviceRequested &
					  kCemodLegacyPermissionMask;
		if ((serviceRequested & kCemodNetworkPermission) != 0 &&
			(exactGranted & kCemodNetworkPermission) != 0)
			result |= kCemodNetworkPermission;
		if ((serviceRequested & kCemodUiPermission) != 0 &&
			(exactGranted & kCemodWebUiApprovalPermission) != 0)
			result |= kCemodUiPermission;
		return result;
	}

	[[nodiscard]] constexpr bool NeedsCemodPermissionPrompt(std::uint32_t requested,
															std::uint32_t granted, std::uint32_t approvedRequests, bool enabled)
	{
		requested &= kCemodPermissionMask;
		return enabled && ((requested & ~granted) != 0 ||
						   (requested & ~approvedRequests) != 0);
	}

	[[nodiscard]] constexpr bool CemodTrustAnchorCoversRequest(std::uint32_t requested,
															   std::uint32_t anchorApprovedRequests)
	{
		requested &= kCemodPermissionMask;
		return (requested & ~anchorApprovedRequests) == 0;
	}
} // namespace cemuextend_hle
