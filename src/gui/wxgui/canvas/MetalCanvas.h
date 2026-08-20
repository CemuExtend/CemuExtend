#pragma once

#include "wxgui/canvas/IRenderCanvas.h"
#include "host/contracts/HostContracts.h"

#include <wx/frame.h>

#include <set>

class MetalCanvas : public IRenderCanvas, public wxWindow
{
public:
	MetalCanvas(wxWindow* parent, const wxSize& size, bool is_main_window,
		std::shared_ptr<Host::IWindowMetrics> windowMetrics,
		std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces,
		std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher);
	~MetalCanvas();
	void PrepareForDestroy() override;

private:
	std::shared_ptr<Host::INativeSurfacePublisher> m_nativeSurfacePublisher;
	Host::NativeWindowHandle m_nativeWindowHandle;
	Host::NativeSurfacePublication m_nativeSurfacePublication{};
	bool m_preparedForDestroy{};

	void OnPaint(wxPaintEvent& event);
	void OnResize(wxSizeEvent& event);
};
