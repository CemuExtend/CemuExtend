#pragma once

#include <functional>

namespace MacRelativePointer
{
	using MotionHandler = std::function<void(double deltaX, double deltaY)>;
	using HoverHandler = std::function<void()>;

	void Install(MotionHandler motion, HoverHandler hover);
	void SetCaptured(bool captured);
	void WarpTo(int screenX, int screenY);
} // namespace MacRelativePointer
