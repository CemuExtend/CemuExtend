#include "wxgui/canvas/RendererWindowAdapter.h"

#include <utility>

#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "config/ActiveSettings.h"

#ifdef ENABLE_OPENGL
#include "wxgui/canvas/OpenGLCanvas.h"
#endif

#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VsyncDriver.h"
#include "wxgui/canvas/VulkanCanvas.h"
#endif

#ifdef ENABLE_METAL
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "wxgui/canvas/MetalCanvas.h"
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

	bool IsVulkanLoaderAvailable()
	{
#ifdef ENABLE_VULKAN
		return g_vulkan_available;
#else
		return false;
#endif
	}

	std::vector<VulkanDevice> EnumerateVulkanDevices(const Host::NativeWindowHandle& mainWindow)
	{
#ifdef ENABLE_VULKAN
		const auto devices = VulkanRenderer::GetDevices(mainWindow);
		std::vector<VulkanDevice> result;
		result.reserve(devices.size());
		for (const auto& device : devices)
			result.push_back({device.name, device.uuid});
		return result;
#else
		return {};
#endif
	}

	std::vector<MetalDevice> EnumerateMetalDevices()
	{
#ifdef ENABLE_METAL
		const auto devices = MetalRenderer::GetDevices();
		std::vector<MetalDevice> result;
		result.reserve(devices.size());
		for (const auto& device : devices)
			result.push_back({device.name, device.uuid});
		return result;
#else
		return {};
#endif
	}

	wxWindow* CreateRenderCanvas(wxWindow* parent, const wxSize& size,
								 bool isMainWindow,
								 std::shared_ptr<Host::IWindowMetrics> windowMetrics,
								 std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces,
								 std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher)
	{
		switch (ActiveSettings::GetGraphicsAPI())
		{
#ifdef ENABLE_OPENGL
		case kOpenGL:
			return GLCanvas_Create(parent, size, isMainWindow,
								   std::move(windowMetrics), std::move(nativeSurfaces));
#endif
#ifdef ENABLE_VULKAN
		case kVulkan:
			return new VulkanCanvas(parent, size, isMainWindow,
									std::move(windowMetrics), std::move(nativeSurfaces),
									std::move(nativeSurfacePublisher));
#endif
#ifdef ENABLE_METAL
		case kMetal:
			return new MetalCanvas(parent, size, isMainWindow,
								   std::move(windowMetrics), std::move(nativeSurfaces),
								   std::move(nativeSurfacePublisher));
#endif
		default:
			return nullptr;
		}
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
} // namespace WxRendererAdapters
