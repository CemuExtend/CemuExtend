#pragma once

#include <cstdint>

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

	[[nodiscard]] constexpr std::uint32_t ExactRuntimeServicePermissions(
		std::uint64_t exactGranted, std::uint32_t serviceRequested, bool nativePackage)
	{
		std::uint32_t result{};
		if (!nativePackage)
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
