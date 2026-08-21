#include "webview/UpdatePlanRegistry.h"

#include <cassert>

int main()
{
	WebFrontend::UpdatePlanRegistry registry;
	Application::TitleInstallPlan plan;
	plan.titleId = 0x5000012345678ULL;
	plan.titleName = "Example";
	const auto token = registry.Issue(7, 3, plan);
	assert(token != 0 && registry.Size() == 1);
	assert(!registry.Take(token, 8, 3));
	assert(!registry.Take(token, 7, 4));
	const auto owned = registry.Take(token, 7, 3);
	assert(owned && owned->titleName == "Example" && registry.Size() == 0);

	const auto stale = registry.Issue(7, 3, plan);
	(void)registry.Issue(7, 3, plan);
	assert(!registry.Take(stale, 7, 3));
	registry.RevokeOwner(7, 3);
	assert(registry.Size() == 0);
}
