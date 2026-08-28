#pragma once

#include <wx/frame.h>

class wxListEvent;
class wxListView;

class BreakpointWindow : public wxFrame
{
  public:
	BreakpointWindow(wxFrame& parent, const wxPoint& main_position, const wxSize& main_size, bool pinToMain);
	virtual ~BreakpointWindow();

	void OnMainMove(const wxPoint& position, const wxSize& main_size);
	void OnUpdateView();
	void OnGameLoaded();

  private:
	void OnBreakpointToggled(wxListEvent& event);
	void OnLeftDClick(wxMouseEvent& event);
	void OnRightDown(wxMouseEvent& event);

	void OnContextMenuClick(wxCommandEvent& evt);
	void OnContextMenuClickSelected(wxCommandEvent& evt);

	wxListView* m_breakpoints;
};
