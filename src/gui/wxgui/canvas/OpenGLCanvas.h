#pragma once
#include "host/contracts/HostContracts.h"
#include <wx/window.h>

wxWindow* GLCanvas_Create(wxWindow* parent, const wxSize& size, bool is_main_window,
	std::shared_ptr<Host::IWindowMetrics> windowMetrics,
	std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces);
