#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace WxRendererAdapters
{
	using ScreenshotRequestId = std::uint64_t;
	using ScreenshotSaveCallback = std::function<std::optional<std::string>(
		const std::vector<std::uint8_t>&, int, int, bool)>;

	void InitializeOverlay();
	void InitializeVulkanLoader();

	void NotifyWindowPositionChanged();
	[[nodiscard]] std::optional<ScreenshotRequestId> RequestScreenshot(
		ScreenshotSaveCallback saveCallback);
	[[nodiscard]] bool IsFrameCaptureSupported();
	[[nodiscard]] bool RequestFrameCapture();
}
