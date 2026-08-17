#include "wxgui/canvas/RendererWindowAdapter.h"

#include <utility>

#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VsyncDriver.h"
#endif

#ifdef ENABLE_METAL
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#endif

namespace WxRendererAdapters
{
	void InitializeOverlay()
	{
		LatteOverlay_init();
	}

	void InitializeVulkanLoader()
	{
#ifdef ENABLE_VULKAN
		(void)InitializeGlobalVulkan();
#endif
	}

	void NotifyWindowPositionChanged()
	{
#ifdef ENABLE_VULKAN
		VsyncDriver_notifyWindowPosChanged();
#endif
	}

	std::optional<ScreenshotRequestId> RequestScreenshot(ScreenshotSaveCallback saveCallback)
	{
		if (!g_renderer)
			return std::nullopt;
		return g_renderer->RequestScreenshot(std::move(saveCallback));
	}

	bool IsFrameCaptureSupported()
	{
#ifdef ENABLE_METAL
		return g_renderer && g_renderer->GetType() == RendererAPI::Metal;
#else
		return false;
#endif
	}

	bool RequestFrameCapture()
	{
#ifdef ENABLE_METAL
		if (!IsFrameCaptureSupported())
			return false;
		static_cast<MetalRenderer*>(g_renderer.get())->CaptureFrame();
		return true;
#else
		return false;
#endif
	}
}
