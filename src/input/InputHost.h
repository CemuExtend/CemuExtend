#pragma once

#include "input/api/ControllerState.h"

namespace Input
{
	struct ScreenImageArea
	{
		int x{};
		int y{};
		int width{};
		int height{};

		[[nodiscard]] bool IsValid() const
		{
			return width > 0 && height > 0;
		}
	};

	class IEmulationInputContext
	{
	  public:
		virtual ~IEmulationInputContext() = default;
		[[nodiscard]] virtual bool IsTitleRunning() const = 0;
		[[nodiscard]] virtual ScreenImageArea GetScreenImageArea(bool padView) const = 0;
	};

	class IControllerStateObserver
	{
	  public:
		virtual ~IControllerStateObserver() = default;
		virtual void OnControllerState(const ControllerState& current,
									   const ControllerState& previous) = 0;
	};
} // namespace Input
