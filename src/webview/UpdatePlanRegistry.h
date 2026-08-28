#pragma once

#include "application/TitleInstallFacade.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace WebFrontend
{
	class UpdatePlanRegistry final
	{
	  public:
		struct OwnedPlan
		{
			std::uint64_t token{};
			std::uint64_t ownerWindow{};
			std::uint64_t ownerGeneration{};
			Application::TitleInstallPlan plan;
		};

		[[nodiscard]] std::uint64_t Issue(std::uint64_t ownerWindow,
										  std::uint64_t ownerGeneration, Application::TitleInstallPlan plan);
		[[nodiscard]] std::optional<Application::TitleInstallPlan> Take(
			std::uint64_t token, std::uint64_t ownerWindow,
			std::uint64_t ownerGeneration);
		void RevokeOwner(std::uint64_t ownerWindow, std::uint64_t ownerGeneration);
		[[nodiscard]] std::size_t Size() const
		{
			return m_plans.size();
		}

	  private:
		std::unordered_map<std::uint64_t, OwnedPlan> m_plans;
		std::uint64_t m_nextToken{};
	};
} // namespace WebFrontend
