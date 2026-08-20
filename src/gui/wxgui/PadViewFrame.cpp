#include "wxgui/wxgui.h"
#include "wxgui/PadViewFrame.h"
#include "wxgui/WxWindowState.h"
#include "wxgui/canvas/IRenderCanvas.h"

#include <wx/display.h>

#include "config/ActiveSettings.h"
#ifdef ENABLE_OPENGL
#include "wxgui/canvas/OpenGLCanvas.h"
#endif
#ifdef ENABLE_VULKAN
#include "wxgui/canvas/VulkanCanvas.h"
#endif
#ifdef ENABLE_METAL
#include "wxgui/canvas/MetalCanvas.h"
#endif
#include "config/CemuConfig.h"
#include "wxgui/MainWindow.h"
#include "wxgui/helpers/wxHelpers.h"
#include "input/InputManager.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif
#include "wxHelper.h"

#define PAD_MIN_WIDTH  320
#define PAD_MIN_HEIGHT 180

PadViewFrame::PadViewFrame(wxFrame* parent,
	Application::EmulationController& emulationController,
	std::shared_ptr<Host::IWindowMetrics> windowMetrics,
	std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces,
	std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher,
	std::shared_ptr<WxWindowState> windowState)
	: wxFrame(nullptr, wxID_ANY, _("GamePad View"), wxDefaultPosition, wxDefaultSize, wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxSYSTEM_MENU | wxCAPTION | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxCLOSE_BOX | wxWANTS_CHARS),
	  m_emulationController(emulationController),
	  m_windowMetrics(std::move(windowMetrics)), m_nativeSurfaces(std::move(nativeSurfaces)),
	  m_nativeSurfacePublisher(std::move(nativeSurfacePublisher)),
	  m_windowState(std::move(windowState))
{
	cemu_assert(m_windowState != nullptr);
	m_nativeWindowHandle = initHandleContextFromWxWidgetsWindow(this);
	m_nativeWindowPublication =
		m_nativeSurfacePublisher->PublishPadWindow(m_nativeWindowHandle);

	SetIcon(wxICON(M_WND_ICON128));
	wxWindow::EnableTouchEvents(wxTOUCH_PAN_GESTURES);

	SetMinClientSize({ PAD_MIN_WIDTH, PAD_MIN_HEIGHT });

	SetPosition({ m_windowState->restored_pad_x, m_windowState->restored_pad_y });
	if (m_windowState->restored_pad_width >= PAD_MIN_WIDTH &&
		m_windowState->restored_pad_height >= PAD_MIN_HEIGHT)
		SetClientSize({ m_windowState->restored_pad_width,
			m_windowState->restored_pad_height });
	else
		SetClientSize(wxSize(854, 480));

	if (m_windowState->pad_maximized)
		Maximize();

	Bind(wxEVT_SIZE, &PadViewFrame::OnSizeEvent, this);
	Bind(wxEVT_DPI_CHANGED, &PadViewFrame::OnDPIChangedEvent, this);
	Bind(wxEVT_MOVE, &PadViewFrame::OnMoveEvent, this);
	Bind(wxEVT_MOTION, &PadViewFrame::OnMouseMove, this);

	Bind(wxEVT_SET_WINDOW_TITLE, &PadViewFrame::OnSetWindowTitle, this);

	m_windowState->pad_open = true;
}

PadViewFrame::~PadViewFrame()
{
	PrepareForDestroy();
	m_windowState->pad_open = false;
}

void PadViewFrame::PrepareForDestroy()
{
	if (std::exchange(m_preparedForDestroy, true))
		return;
	m_windowState->pad_open = false;
	if (m_render_canvas)
	{
		if (auto* canvas = dynamic_cast<IRenderCanvas*>(m_render_canvas))
			canvas->PrepareForDestroy();
	}
	m_nativeSurfacePublisher->ClearPadWindow(m_nativeWindowPublication);
}

bool PadViewFrame::Initialize()
{
	const wxSize client_size = GetClientSize();
	m_windowState->pad_width = client_size.GetWidth();
	m_windowState->pad_height = client_size.GetHeight();
	m_windowState->phys_pad_width = ToPhys(client_size.GetWidth());
	m_windowState->phys_pad_height = ToPhys(client_size.GetHeight());

	return true;
}

void PadViewFrame::InitializeRenderCanvas()
{
	auto sizer = new wxBoxSizer(wxVERTICAL);
	{
		#ifdef ENABLE_VULKAN
		if (ActiveSettings::GetGraphicsAPI() == kVulkan)
			m_render_canvas = new VulkanCanvas(this, wxSize(854, 480), false,
				m_windowMetrics, m_nativeSurfaces, m_nativeSurfacePublisher);
		#endif
		#ifdef ENABLE_OPENGL
		if (ActiveSettings::GetGraphicsAPI() == kOpenGL)
			m_render_canvas = GLCanvas_Create(this, wxSize(854, 480), false,
				m_windowMetrics, m_nativeSurfaces);
		#endif
		#ifdef ENABLE_METAL
		if (ActiveSettings::GetGraphicsAPI() == kMetal)
			m_render_canvas = new MetalCanvas(this, wxSize(854, 480), false,
				m_windowMetrics, m_nativeSurfaces, m_nativeSurfacePublisher);
		#endif
		sizer->Add(m_render_canvas, 1, wxEXPAND, 0, nullptr);
	}
	cemu_assert(m_render_canvas != nullptr);
	SetSizer(sizer);
	Layout();

	m_render_canvas->Bind(wxEVT_KEY_UP, &PadViewFrame::OnKeyUp, this);
	m_render_canvas->Bind(wxEVT_CHAR, &PadViewFrame::OnChar, this);

	m_render_canvas->Bind(wxEVT_MOTION, &PadViewFrame::OnMouseMove, this);
	m_render_canvas->Bind(wxEVT_LEFT_DOWN, &PadViewFrame::OnMouseLeft, this);
	m_render_canvas->Bind(wxEVT_LEFT_UP, &PadViewFrame::OnMouseLeft, this);
	m_render_canvas->Bind(wxEVT_LEFT_DCLICK, &PadViewFrame::OnMouseLeft, this);
	m_render_canvas->Bind(wxEVT_RIGHT_DOWN, &PadViewFrame::OnMouseRight, this);
	m_render_canvas->Bind(wxEVT_RIGHT_UP, &PadViewFrame::OnMouseRight, this);
	m_render_canvas->Bind(wxEVT_RIGHT_DCLICK, &PadViewFrame::OnMouseRight, this);

	m_render_canvas->Bind(wxEVT_GESTURE_PAN, &PadViewFrame::OnGesturePan, this);

	m_render_canvas->SetFocus();
	SendSizeEvent();
}

void PadViewFrame::DestroyCanvas()
{
	if(!m_render_canvas)
		return;
	if (auto* canvas = dynamic_cast<IRenderCanvas*>(m_render_canvas))
		canvas->PrepareForDestroy();
	(void)m_nativeSurfacePublisher->PublishCanvas(false, {});
	m_render_canvas->Destroy();
	m_render_canvas = nullptr;
}

void PadViewFrame::OnSizeEvent(wxSizeEvent& event)
{
	if (!IsMaximized() && !IsFullScreen())
	{
		m_windowState->restored_pad_width = GetSize().x;
		m_windowState->restored_pad_height = GetSize().y;
	}
	m_windowState->pad_maximized = IsMaximized() && !IsFullScreen();

	const wxSize client_size = GetClientSize();
	m_windowState->pad_width = client_size.GetWidth();
	m_windowState->pad_height = client_size.GetHeight();
	m_windowState->phys_pad_width = ToPhys(client_size.GetWidth());
	m_windowState->phys_pad_height = ToPhys(client_size.GetHeight());
	m_windowState->pad_dpi_scale = GetDPIScaleFactor();

	event.Skip();
}

void PadViewFrame::OnDPIChangedEvent(wxDPIChangedEvent& event)
{
	event.Skip();
	const wxSize client_size = GetClientSize();
	m_windowState->pad_width = client_size.GetWidth();
	m_windowState->pad_height = client_size.GetHeight();
	m_windowState->phys_pad_width = ToPhys(client_size.GetWidth());
	m_windowState->phys_pad_height = ToPhys(client_size.GetHeight());
	m_windowState->pad_dpi_scale = GetDPIScaleFactor();
}

void PadViewFrame::OnMoveEvent(wxMoveEvent& event)
{
	if (!IsMaximized() && !IsFullScreen())
	{
		m_windowState->restored_pad_x = GetPosition().x;
		m_windowState->restored_pad_y = GetPosition().y;
	}
}

void PadViewFrame::OnKeyUp(wxKeyEvent& event)
{
	event.Skip();

	if (m_emulationController.SoftwareKeyboardActive())
		return;

	const auto code = event.GetKeyCode();
	if (code == WXK_ESCAPE)
		ShowFullScreen(false);
	else if (code == WXK_RETURN && event.AltDown() || code == WXK_F11)
		ShowFullScreen(!IsFullScreen());
}

void PadViewFrame::OnGesturePan(wxPanGestureEvent& event)
{
	auto& instance = InputManager::instance();

	std::scoped_lock lock(instance.m_pad_touch.m_mutex);
	auto physPos = ToPhys(event.GetPosition());
	instance.m_pad_touch.position = { physPos.x, physPos.y };
	instance.m_pad_touch.left_down = event.IsGestureStart() || !event.IsGestureEnd();
	if (event.IsGestureStart() || !event.IsGestureEnd())
		instance.m_pad_touch.left_down_toggle = true;
}

void PadViewFrame::OnChar(wxKeyEvent& event)
{
	m_emulationController.SubmitSoftwareKeyboardKey(event.GetUnicodeKey());

	event.Skip();
}

void PadViewFrame::EmitCemuExtendMouseEvent(wxMouseEvent& event, std::uint32_t changedButtons)
{
	if (!m_render_canvas)
		return;
	using Frontend::CemuExtendMouseButton;
	const auto logicalPosition = event.GetPosition();
	const auto physicalPosition = ToPhys(logicalPosition);
	const auto size = m_render_canvas->GetClientSize();
	const auto aggregateButtons =
		(event.LeftIsDown() ? static_cast<std::uint32_t>(CemuExtendMouseButton::Left) : 0U) |
		(event.RightIsDown() ? static_cast<std::uint32_t>(CemuExtendMouseButton::Right) : 0U) |
		(event.MiddleIsDown() ? static_cast<std::uint32_t>(CemuExtendMouseButton::Middle) : 0U);
	Frontend::CemuExtendMouseTransition transition =
		Frontend::CemuExtendMouseTransition::None;
	if (event.ButtonDown() || event.ButtonDClick())
		transition = Frontend::CemuExtendMouseTransition::Down;
	else if (event.ButtonUp())
		transition = Frontend::CemuExtendMouseTransition::Up;
	else if (changedButtons != 0)
		transition = Frontend::CemuExtendMouseTransition::Aggregate;
	const auto buttonUpdate = m_cemuextend_bridge.UpdateButtons(transition,
		changedButtons, aggregateButtons);
	const auto motion = m_cemuextend_bridge.UpdatePosition(
		{physicalPosition.x, physicalPosition.y}, {}, false);
	const bool inside = logicalPosition.x >= 0 && logicalPosition.y >= 0 &&
		logicalPosition.x < size.GetWidth() && logicalPosition.y < size.GetHeight();
	m_emulationController.SubmitMouse({
		.surface = Application::PointerSurface::Drc,
		.x = physicalPosition.x,
		.y = physicalPosition.y,
		.deltaX = motion.delta.x,
		.deltaY = motion.delta.y,
		.buttons = buttonUpdate.buttons,
		.changedButtons = buttonUpdate.changed,
		.contentWidth = ToPhys(size.GetWidth()),
		.contentHeight = ToPhys(size.GetHeight()),
		.insideContent = inside,
		.focused = m_windowState->app_active.load(),
	});
}

void PadViewFrame::OnMouseMove(wxMouseEvent& event)
{
	auto& instance = InputManager::instance();

	std::scoped_lock lock(instance.m_pad_touch.m_mutex);
	auto physPos = ToPhys(event.GetPosition());
	instance.m_pad_mouse.position = { physPos.x, physPos.y };
	EmitCemuExtendMouseEvent(event);

	event.Skip();
}

void PadViewFrame::OnMouseLeft(wxMouseEvent& event)
{
	auto& instance = InputManager::instance();
	const bool pressed = event.ButtonDown(wxMOUSE_BTN_LEFT) ||
		event.ButtonDClick(wxMOUSE_BTN_LEFT);

	std::scoped_lock lock(instance.m_pad_mouse.m_mutex);
	instance.m_pad_mouse.left_down = pressed;
	auto physPos = ToPhys(event.GetPosition());
	instance.m_pad_mouse.position = { physPos.x, physPos.y };
	if (pressed)
		instance.m_pad_mouse.left_down_toggle = true;
	EmitCemuExtendMouseEvent(event, static_cast<std::uint32_t>(
		Frontend::CemuExtendMouseButton::Left));

}

void PadViewFrame::OnMouseRight(wxMouseEvent& event)
{
	auto& instance = InputManager::instance();
	const bool pressed = event.ButtonDown(wxMOUSE_BTN_RIGHT) ||
		event.ButtonDClick(wxMOUSE_BTN_RIGHT);

	std::scoped_lock lock(instance.m_pad_mouse.m_mutex);
	instance.m_pad_mouse.right_down = pressed;
	auto physPos = ToPhys(event.GetPosition());
	instance.m_pad_mouse.position = { physPos.x, physPos.y };
	if (pressed)
		instance.m_pad_mouse.right_down_toggle = true;
	EmitCemuExtendMouseEvent(event, static_cast<std::uint32_t>(
		Frontend::CemuExtendMouseButton::Right));
}

void PadViewFrame::OnSetWindowTitle(wxCommandEvent& event)
{
	this->SetTitle(event.GetString());
}

void PadViewFrame::AsyncSetTitle(std::string_view windowTitle)
{
	cemu_assert_debug(wxIsMainThread());
	SetTitle(wxString::FromUTF8(windowTitle));
}
