#include "Cafe/OS/libs/cemuextend/CemodPermission.h"

#include <cstdlib>
#include <iostream>

namespace
{
	[[noreturn]] void CheckFailed(const char* expression, int line)
	{
		std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
		std::abort();
	}

#define CHECK(condition)                       \
	do                                         \
	{                                          \
		if (!(condition))                      \
			CheckFailed(#condition, __LINE__); \
	}                                          \
	while (false)
} // namespace

int main()
{
	using cemuextend_hle::CemodTrustAnchorCoversRequest;
	using cemuextend_hle::ExactRuntimeServicePermissions;
	using cemuextend_hle::kCemodExactApprovalPermissionMask;
	using cemuextend_hle::kCemodWebUiApprovalPermission;
	using cemuextend_hle::NeedsCemodPermissionPrompt;

	CHECK((kCemodExactApprovalPermissionMask & kCemodWebUiApprovalPermission) != 0);
	// The approval namespace now runs past Web UI to the CEX2 services, and stops
	// there: anything above the last service bit is outside what a package may ask.
	CHECK((kCemodExactApprovalPermissionMask & cemuextend_hle::kCemodServiceMicrophoneApproval) != 0);
	CHECK((kCemodExactApprovalPermissionMask &
		   (cemuextend_hle::kCemodServiceMicrophoneApproval << 1U)) == 0);

	CHECK(!NeedsCemodPermissionPrompt(0x1fU, 0, 0, false));
	CHECK(!NeedsCemodPermissionPrompt(0, 0, 0, true));
	CHECK(!NeedsCemodPermissionPrompt(0x1fU, 0x1fU, 0x1fU, true));
	CHECK(NeedsCemodPermissionPrompt(0x1fU, 0x0fU, 0x1fU, true));
	CHECK(NeedsCemodPermissionPrompt(0x1fU, 0x1fU, 0x0fU, true));
	CHECK(NeedsCemodPermissionPrompt(0x20U, 0, 0, true));
	CHECK(!NeedsCemodPermissionPrompt(0x20U, 0x20U, 0x20U, true));
	CHECK(NeedsCemodPermissionPrompt(0x40U, 0, 0, true));
	CHECK(!NeedsCemodPermissionPrompt(0x40U, 0x40U, 0x40U, true));
	CHECK(NeedsCemodPermissionPrompt(0x80U, 0, 0, true));
	CHECK(!NeedsCemodPermissionPrompt(0x80U, 0x80U, 0x80U, true));

	CHECK(ExactRuntimeServicePermissions(
			  0, cemuextend_hle::kCemodMicrophonePermission, false) == 0);
	CHECK(ExactRuntimeServicePermissions(
			  cemuextend_hle::kCemodServiceMicrophoneApproval,
			  cemuextend_hle::kCemodMicrophonePermission,
			  false) == cemuextend_hle::kCemodMicrophonePermission);

	CHECK(CemodTrustAnchorCoversRequest(0x0fU, 0x1fU));
	CHECK(CemodTrustAnchorCoversRequest(0x1fU, 0x1fU));
	CHECK(!CemodTrustAnchorCoversRequest(0x1fU, 0x0fU));
	CHECK(!CemodTrustAnchorCoversRequest(0x02U, 0x01U));
	CHECK(!CemodTrustAnchorCoversRequest(0x20U, 0));
	CHECK(CemodTrustAnchorCoversRequest(0x20U, 0x20U));
	CHECK(!CemodTrustAnchorCoversRequest(0x40U, 0));
	CHECK(CemodTrustAnchorCoversRequest(0x40U, 0x40U));

	// Native/WUPS approval bits must never be reinterpreted as legacy CEX2 bits.
	CHECK(ExactRuntimeServicePermissions(0x7ff, 0x15, false) == 0x15);
	CHECK(ExactRuntimeServicePermissions(0x400, 0x3f, false) == 0);
	CHECK(ExactRuntimeServicePermissions(0x3f, 0x3f, true) == 0x20);
	CHECK(ExactRuntimeServicePermissions(0x7ff, 0x3f, true) == 0x20);
	// A native package reaches a service only when its manifest asked for it and the
	// user approved that service's own bit; native bits still grant nothing here.
	CHECK(ExactRuntimeServicePermissions(cemuextend_hle::kCemodServiceReadApproval,
										 0x01, true) == 0x01);
	CHECK(ExactRuntimeServicePermissions(cemuextend_hle::kCemodServiceReadApproval,
										 0x02, true) == 0);
	CHECK(ExactRuntimeServicePermissions(cemuextend_hle::kCemodServiceApprovalMask,
										 0x1f, true) == 0x1f);
	CHECK(ExactRuntimeServicePermissions(0x7ff, 0x1f, true) == 0);
	CHECK(ExactRuntimeServicePermissions(1ULL << 11U, 0x40, false) == 0x40);
	CHECK(ExactRuntimeServicePermissions(1ULL << 11U, 0x40, true) == 0x40);
	CHECK(ExactRuntimeServicePermissions(0x40, 0x40, false) == 0);
	return 0;
}
