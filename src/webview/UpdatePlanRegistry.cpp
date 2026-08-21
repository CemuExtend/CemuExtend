#include "webview/UpdatePlanRegistry.h"

#include <algorithm>
#include <stdexcept>

namespace WebFrontend
{
	std::uint64_t UpdatePlanRegistry::Issue(std::uint64_t ownerWindow,
		std::uint64_t ownerGeneration, Application::TitleInstallPlan plan)
	{
		if (ownerWindow == 0 || ownerGeneration == 0)
			throw std::invalid_argument("update plans require a live tool-window owner");
		RevokeOwner(ownerWindow, ownerGeneration);
		if (m_plans.size() >= 16)
			throw std::runtime_error("too many pending update plans");
		const auto token = ++m_nextToken;
		m_plans.emplace(token, OwnedPlan{token, ownerWindow, ownerGeneration,
			std::move(plan)});
		return token;
	}

	std::optional<Application::TitleInstallPlan> UpdatePlanRegistry::Take(
		std::uint64_t token, std::uint64_t ownerWindow, std::uint64_t ownerGeneration)
	{
		const auto found = m_plans.find(token);
		if (found == m_plans.end() || found->second.ownerWindow != ownerWindow ||
			found->second.ownerGeneration != ownerGeneration)
			return std::nullopt;
		auto plan = std::move(found->second.plan);
		m_plans.erase(found);
		return plan;
	}

	void UpdatePlanRegistry::RevokeOwner(std::uint64_t ownerWindow,
		std::uint64_t ownerGeneration)
	{
		std::erase_if(m_plans, [=](const auto& entry) {
			return entry.second.ownerWindow == ownerWindow &&
				entry.second.ownerGeneration == ownerGeneration;
		});
	}
}
