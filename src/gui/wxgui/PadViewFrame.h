#pragma once

#include "frontend/CemuExtendFrontendBridge.h"
#include "host/contracts/HostContracts.h"
#include "application/EmulationController.h"

#include <wx/frame.h>

#include <cstdint>

class WxWindowState;
struct WxFrontendContext;

#define WM_CREATE_PAD (WM_USER + 1)
#define WM_DESTROY_PAD (WM_USER + 2)

wxDECLARE_EVENT(EVT_PAD_CLOSE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SET_WINDOW_TITLE, wxCommandEvent);

class PadViewFrame : public wxFrame
{
  public:
	PadViewFrame(wxFrame* parent, Application::EmulationController& emulationController,
				 std::shared_ptr<WxFrontendContext> frontendContext);
	~PadViewFrame();

	bool Initialize();
	void InitializeRenderCanvas();
	void DestroyCanvas();
	void PrepareForDestroy();

	void OnKeyUp(wxKeyEvent& event);
	void OnChar(wxKeyEvent& event);

	void AsyncSetTitle(std::string_view windowTitle);

  private:
	void OnMouseMove(wxMouseEvent& event);
	void OnMouseLeft(wxMouseEvent& event);
	void OnMouseRight(wxMouseEvent& event);
	void OnSizeEvent(wxSizeEvent& event);
	void OnDPIChangedEvent(wxDPIChangedEvent& event);
	void OnMoveEvent(wxMoveEvent& event);
	void OnGesturePan(wxPanGestureEvent& event);
	void OnSetWindowTitle(wxCommandEvent& event);
	void EmitCemuExtendMouseEvent(wxMouseEvent& event, std::uint32_t changedButtons = 0);

	wxWindow* m_render_canvas = nullptr;
	Application::EmulationController& m_emulationController;
	std::shared_ptr<Host::IWindowMetrics> m_windowMetrics;
	std::shared_ptr<Host::INativeSurfaceProvider> m_nativeSurfaces;
	std::shared_ptr<Host::INativeSurfacePublisher> m_nativeSurfacePublisher;
	std::shared_ptr<Host::IInputHostEvents> m_inputHostEvents;
	std::shared_ptr<WxWindowState> m_windowState;
	Host::NativeWindowHandle m_nativeWindowHandle;
	Host::NativeSurfacePublication m_nativeWindowPublication{};
	bool m_preparedForDestroy{};
	Frontend::CemuExtendFrontendBridge m_cemuextend_bridge;
};
