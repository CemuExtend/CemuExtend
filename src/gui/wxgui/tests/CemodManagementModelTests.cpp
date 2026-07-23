#include "gui/wxgui/CemodManagementModel.h"

#include <cstdlib>
#include <iostream>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
		{
			std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
			std::abort();
		}
	}
#define CHECK(condition) Check((condition), #condition, __LINE__)
}

int main()
{
	const auto functionPatch = CemodGuiAdapter::PermissionBit(CemodGuiPermission::FunctionPatching);
	const auto notifications = CemodGuiAdapter::PermissionBit(CemodGuiPermission::Notifications);
	CHECK(functionPatch != 0);
	CHECK(CemodGuiAdapter::PermissionName(CemodGuiPermission::Modules) == "Aroma/WUMS modules");
	CHECK(CemodGuiAdapter::MakeApprovalKey("org.example.plugin", "abc") ==
		"org.example.plugin|sha256:abc");
	CHECK(CemodGuiAdapter::DefaultGrantedPermissions(functionPatch | notifications) == notifications);

	const auto missing = CemodGuiAdapter::EvaluateApproval(functionPatch, std::nullopt, false);
	CHECK(missing.result == CemodGuiApprovalResult::DeniedByDefault);
	CHECK(missing.granted == 0);
	const auto headless = CemodGuiAdapter::EvaluateApproval(functionPatch, std::nullopt, true);
	CHECK(headless.result == CemodGuiApprovalResult::DeniedHeadlessRequiresExplicitApproval);

	CemuExtendPermissionApproval approval{
		"abc", "org.example.plugin", functionPatch, functionPatch, true, false};
	const auto accepted = CemodGuiAdapter::EvaluateApproval(functionPatch, approval, false);
	CHECK(accepted.result == CemodGuiApprovalResult::Approved);
	const auto changed = CemodGuiAdapter::EvaluateApproval(functionPatch | notifications,
		approval, false);
	CHECK(changed.result == CemodGuiApprovalResult::NeedsReapproval);
	const auto config = CemodGuiAdapter::UnavailableConfig();
	CHECK(config.size() == 1 && !config.front().available);
	const auto notices = CemodGuiAdapter::UnavailableNotifications();
	CHECK(notices.size() == 1 && notices.front().message.find("Requires runtime integration") != std::string::npos);
	return 0;
}
