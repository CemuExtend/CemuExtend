#pragma once

#include "wxgui/debugger/DumpCtrl.h"

class DumpWindow : public wxFrame
{
public:
	DumpWindow(wxFrame& parent, const wxPoint& main_position, const wxSize& main_size, bool pinToMain);

	void OnMainMove(const wxPoint& position, const wxSize& main_size);
	void OnGameLoaded();

private:
	wxScrolledWindow* m_scrolled_window;
	DumpCtrl* m_dump_ctrl;
};
