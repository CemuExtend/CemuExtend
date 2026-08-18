#pragma once

#include <cstdint>

#include <wx/frame.h>

namespace WxDebuggerAdapters
{
	wxWindow* CreateDebuggerWindow(wxFrame& parent, const wxRect& displaySize);
	void RequestCloseDebuggerWindow(wxWindow& window);
	void DestroyDebuggerWindow(wxWindow& window);
	void NotifyGameLoaded(wxWindow& window);
	void NotifyParentMove(wxWindow& window, const wxPoint& position, const wxSize& size);
	bool HasDebuggerWindow();
	bool IsDebuggerWindowOrChild(const wxWindow* window);
	void EnsureGdbStub(std::uint16_t port);
	void ToggleGdbStub(std::uint16_t port);
	bool IsGdbStubEnabled();
}
