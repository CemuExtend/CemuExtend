#pragma once

#include "host/contracts/HostContracts.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class wxSize;
class wxWindow;

namespace WxRendererAdapters
{
	struct VulkanDevice
	{
		std::string name;
		std::array<std::uint8_t, 16> uuid{};
	};

	struct MetalDevice
	{
		std::string name;
		std::uint64_t uuid{};
	};

	using ScreenshotRequestId = std::uint64_t;
	using ScreenshotSaveCallback = std::function<std::optional<std::string>(
		const std::vector<std::uint8_t>&, int, int, bool)>;

	void InitializeOverlay();
	void InitializeVulkanLoader();
	[[nodiscard]] bool IsVulkanLoaderAvailable();
	[[nodiscard]] std::vector<VulkanDevice> EnumerateVulkanDevices(
		const Host::NativeWindowHandle& mainWindow);
	[[nodiscard]] std::vector<MetalDevice> EnumerateMetalDevices();
	[[nodiscard]] wxWindow* CreateRenderCanvas(wxWindow* parent,
											   const wxSize& size, bool isMainWindow,
											   std::shared_ptr<Host::IWindowMetrics> windowMetrics,
											   std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces,
											   std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher);

	void NotifyWindowPositionChanged();
	[[nodiscard]] std::optional<ScreenshotRequestId> RequestScreenshot(
		ScreenshotSaveCallback saveCallback);
	[[nodiscard]] bool IsFrameCaptureSupported();
	[[nodiscard]] bool RequestFrameCapture();
} // namespace WxRendererAdapters
