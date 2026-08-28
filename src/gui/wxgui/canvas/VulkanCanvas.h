#pragma once

#include "wxgui/canvas/IRenderCanvas.h"
#include "host/contracts/HostContracts.h"

#include <wx/frame.h>

#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include <set>

class VulkanCanvas : public IRenderCanvas, public wxWindow
{
#if (BOOST_OS_LINUX || BOOST_OS_BSD) && HAS_WAYLAND
	std::unique_ptr<class wxWlSubsurface> m_subsurface;
#endif
  public:
	VulkanCanvas(wxWindow* parent, const wxSize& size, bool is_main_window,
				 std::shared_ptr<Host::IWindowMetrics> windowMetrics,
				 std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces,
				 std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher);
	~VulkanCanvas();
	void PrepareForDestroy() override;

  private:
	std::shared_ptr<Host::INativeSurfacePublisher> m_nativeSurfacePublisher;
	Host::NativeWindowHandle m_nativeWindowHandle;
	Host::NativeSurfacePublication m_nativeSurfacePublication{};
	bool m_preparedForDestroy{};

	void OnPaint(wxPaintEvent& event);
	void OnResize(wxSizeEvent& event);
};
