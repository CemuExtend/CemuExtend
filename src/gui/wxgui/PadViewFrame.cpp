#include "interface/WindowSystem.h"
#include "wxgui/wxgui.h"
#include "wxgui/PadViewFrame.h"

#include <wx/display.h>

#include "config/ActiveSettings.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "Cafe/OS/libs/cemuextend/BridgeHost.h"
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

extern WindowSystem::WindowInfo g_window_info;

#define PAD_MIN_WIDTH  320
#define PAD_MIN_HEIGHT 180

PadViewFrame::PadViewFrame(wxFrame* parent)
	: wxFrame(nullptr, wxID_ANY, _("GamePad View"), wxDefaultPosition, wxDefaultSize, wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxSYSTEM_MENU | wxCAPTION | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxCLOSE_BOX | wxWANTS_CHARS)
{
	g_window_info.window_pad = initHandleContextFromWxWidgetsWindow(this);

	SetIcon(wxICON(M_WND_ICON128));
	wxWindow::EnableTouchEvents(wxTOUCH_PAN_GESTURES);

	SetMinClientSize({ PAD_MIN_WIDTH, PAD_MIN_HEIGHT });

	SetPosition({ g_window_info.restored_pad_x, g_window_info.restored_pad_y });
	if (g_window_info.restored_pad_width >= PAD_MIN_WIDTH && g_window_info.restored_pad_height >= PAD_MIN_HEIGHT)
		SetClientSize({ g_window_info.restored_pad_width, g_window_info.restored_pad_height });
	else
		SetClientSize(wxSize(854, 480));

	if (g_window_info.pad_maximized)
		Maximize();

	Bind(wxEVT_SIZE, &PadViewFrame::OnSizeEvent, this);
	Bind(wxEVT_DPI_CHANGED, &PadViewFrame::OnDPIChangedEvent, this);
	Bind(wxEVT_MOVE, &PadViewFrame::OnMoveEvent, this);
	Bind(wxEVT_MOTION, &PadViewFrame::OnMouseMove, this);

	Bind(wxEVT_SET_WINDOW_TITLE, &PadViewFrame::OnSetWindowTitle, this);

	g_window_info.pad_open = true;
}

PadViewFrame::~PadViewFrame()
{
	g_window_info.pad_open = false;
}

bool PadViewFrame::Initialize()
{
	const wxSize client_size = GetClientSize();
	g_window_info.pad_width = client_size.GetWidth();
	g_window_info.pad_height = client_size.GetHeight();
	g_window_info.phys_pad_width = ToPhys(client_size.GetWidth());
	g_window_info.phys_pad_height = ToPhys(client_size.GetHeight());

	return true;
}

void PadViewFrame::InitializeRenderCanvas()
{
	auto sizer = new wxBoxSizer(wxVERTICAL);
	{
		#ifdef ENABLE_VULKAN
		if (ActiveSettings::GetGraphicsAPI() == kVulkan)
			m_render_canvas = new VulkanCanvas(this, wxSize(854, 480), false);
		#endif
		#ifdef ENABLE_OPENGL
		if (ActiveSettings::GetGraphicsAPI() == kOpenGL)
			m_render_canvas = GLCanvas_Create(this, wxSize(854, 480), false);
		#endif
		#ifdef ENABLE_METAL
		if (ActiveSettings::GetGraphicsAPI() == kMetal)
			m_render_canvas = new MetalCanvas(this, wxSize(854, 480), false);
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
	m_render_canvas->Destroy();
	m_render_canvas = nullptr;
}

void PadViewFrame::OnSizeEvent(wxSizeEvent& event)
{
	if (!IsMaximized() && !IsFullScreen())
	{
		g_window_info.restored_pad_width = GetSize().x;
		g_window_info.restored_pad_height = GetSize().y;
	}
	g_window_info.pad_maximized = IsMaximized() && !IsFullScreen();

	const wxSize client_size = GetClientSize();
	g_window_info.pad_width = client_size.GetWidth();
	g_window_info.pad_height = client_size.GetHeight();
	g_window_info.phys_pad_width = ToPhys(client_size.GetWidth());
	g_window_info.phys_pad_height = ToPhys(client_size.GetHeight());
	g_window_info.pad_dpi_scale = GetDPIScaleFactor();

	event.Skip();
}

void PadViewFrame::OnDPIChangedEvent(wxDPIChangedEvent& event)
{
	event.Skip();
	const wxSize client_size = GetClientSize();
	g_window_info.pad_width = client_size.GetWidth();
	g_window_info.pad_height = client_size.GetHeight();
	g_window_info.phys_pad_width = ToPhys(client_size.GetWidth());
	g_window_info.phys_pad_height = ToPhys(client_size.GetHeight());
	g_window_info.pad_dpi_scale = GetDPIScaleFactor();
}

void PadViewFrame::OnMoveEvent(wxMoveEvent& event)
{
	if (!IsMaximized() && !IsFullScreen())
	{
		g_window_info.restored_pad_x = GetPosition().x;
		g_window_info.restored_pad_y = GetPosition().y;
	}
}

void PadViewFrame::OnKeyUp(wxKeyEvent& event)
{
	event.Skip();

	if (swkbd_hasKeyboardInputHook())
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
	if (swkbd_hasKeyboardInputHook())
		swkbd_keyInput(event.GetUnicodeKey());

	event.Skip();
}

void PadViewFrame::EmitCemuExtendMouseEvent(wxMouseEvent& event, std::uint32_t changedButtons)
{
	if (!m_render_canvas)
		return;
	using cemuextend::wire::MouseButton;
	const auto logicalPosition = event.GetPosition();
	const auto physicalPosition = ToPhys(logicalPosition);
	const auto size = m_render_canvas->GetClientSize();
	auto buttons =
		(event.LeftIsDown() ? static_cast<std::uint32_t>(MouseButton::Left) : 0U) |
		(event.RightIsDown() ? static_cast<std::uint32_t>(MouseButton::Right) : 0U) |
		(event.MiddleIsDown() ? static_cast<std::uint32_t>(MouseButton::Middle) : 0U);
	if (event.ButtonDown() || event.ButtonDClick())
		buttons |= changedButtons;
	else if (event.ButtonUp())
		buttons &= ~changedButtons;
	wxPoint delta{};
	if (m_cemuextend_mouse_position_valid)
		delta = physicalPosition - m_cemuextend_last_mouse_position;
	m_cemuextend_last_mouse_position = physicalPosition;
	m_cemuextend_mouse_position_valid = true;
	const bool inside = logicalPosition.x >= 0 && logicalPosition.y >= 0 &&
		logicalPosition.x < size.GetWidth() && logicalPosition.y < size.GetHeight();
	cemuextend_hle::MouseEvent(cemuextend::wire::PointerSurface::Drc,
		physicalPosition.x, physicalPosition.y, delta.x, delta.y, 0, 0,
		buttons, changedButtons, ToPhys(size.GetWidth()), ToPhys(size.GetHeight()),
		inside, g_window_info.app_active.load());
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
		cemuextend::wire::MouseButton::Left));

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
		cemuextend::wire::MouseButton::Right));
}

void PadViewFrame::OnSetWindowTitle(wxCommandEvent& event)
{
	this->SetTitle(event.GetString());
}

void PadViewFrame::AsyncSetTitle(std::string_view windowTitle)
{
	wxCommandEvent set_title_event(wxEVT_SET_WINDOW_TITLE);
	set_title_event.SetString(wxString::FromUTF8(windowTitle));
	QueueEvent(set_title_event.Clone());
}
