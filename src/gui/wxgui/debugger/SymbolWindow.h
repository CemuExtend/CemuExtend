#pragma once

#include "wxgui/debugger/SymbolCtrl.h"
#include <wx/frame.h>

class SymbolWindow : public wxFrame
{
  public:
	SymbolWindow(wxFrame& parent, const wxPoint& main_position, const wxSize& main_size, bool pinToMain);

	void OnMainMove(const wxPoint& position, const wxSize& main_size);
	void OnGameLoaded();

	void OnLeftDClick(wxListEvent& event);

  private:
	wxTextCtrl* m_filter;
	SymbolListCtrl* m_symbol_ctrl;

	void OnFilterChanged(wxCommandEvent& event);
};
