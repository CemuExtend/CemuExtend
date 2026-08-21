#pragma once

#include <cstdint>

namespace cemuextend_hle
{
	// read | write | inject | clipboard | capture | network
	constexpr std::uint32_t kCemodNetworkPermission = 1U << 5U;
	constexpr std::uint32_t kCemodPermissionMask = 0x3fU;

	[[nodiscard]] constexpr std::uint32_t ExactRuntimeServicePermissions(
		std::uint64_t exactGranted, std::uint32_t serviceRequested, bool nativePackage)
	{
		return nativePackage ? 0U :
			static_cast<std::uint32_t>(exactGranted) & serviceRequested & kCemodPermissionMask;
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
}
