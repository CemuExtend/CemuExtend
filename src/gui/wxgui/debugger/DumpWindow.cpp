#include "wxgui/wxgui.h"
#include "wxgui/debugger/DumpWindow.h"

enum
{
	// REGISTER
	REGISTER_ADDRESS_R0 = wxID_HIGHEST + 8200,
	REGISTER_LABEL_R0 = REGISTER_ADDRESS_R0 + 32,
	REGISTER_LABEL_FPR0_0 = REGISTER_LABEL_R0 + 32,
	REGISTER_LABEL_FPR1_0 = REGISTER_LABEL_R0 + 32,
};

DumpWindow::DumpWindow(wxFrame& parent, const wxPoint& main_position, const wxSize& main_size, bool pinToMain)
	: wxFrame(&parent, wxID_ANY, _("Memory Dump"), wxDefaultPosition, wxSize(600, 250), wxSYSTEM_MENU | wxCAPTION | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT)
{
	wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
	m_dump_ctrl = new DumpCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxScrolledWindowStyle);
	main_sizer->Add(m_dump_ctrl, 1, wxEXPAND);

	this->SetSizer(main_sizer);
	this->wxWindowBase::Layout();

	this->Centre(wxBOTH);

	if (pinToMain)
		OnMainMove(main_position, main_size);
}

void DumpWindow::OnMainMove(const wxPoint& main_position, const wxSize& main_size)
{
	wxSize size(600, 250);
	this->SetSize(size);

	wxPoint position = main_position;
	position.y += main_size.GetHeight();
	this->SetPosition(position);
}

void DumpWindow::OnGameLoaded()
{
	m_dump_ctrl->Init();
}
