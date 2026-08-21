#pragma once

#include <wx/frame.h>

class wxListView;

class ModuleWindow : public wxFrame
{
public:
	ModuleWindow(wxFrame& parent, const wxPoint& main_position, const wxSize& main_size, bool pinToMain);

	void OnMainMove(const wxPoint& position, const wxSize& main_size);
	void OnGameLoaded();

private:
	void OnLeftDClick(wxMouseEvent& event);

	wxListView* m_modules;
};
