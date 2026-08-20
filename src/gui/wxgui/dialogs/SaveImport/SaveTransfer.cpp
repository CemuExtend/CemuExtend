#include "SaveTransfer.h"
#include "application/EmulationController.h"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "util/helpers/helpers.h"
#include "wxgui/helpers/wxHelpers.h"

SaveTransfer::SaveTransfer(wxWindow* parent,
	Application::EmulationController& emulationController, uint64 title_id,
	const wxString& source_account, uint32 source_id)
	: wxDialog(parent, wxID_ANY, _("Save transfer"), wxDefaultPosition,
		wxDefaultSize, wxCAPTION | wxFRAME_TOOL_WINDOW | wxSYSTEM_MENU |
		wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_title_id(title_id), m_source_id(source_id),
	  m_emulationController(emulationController)
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	auto* row1 = new wxFlexGridSizer(0, 2, 0, 0);
	row1->AddGrowableCol(1);
	row1->Add(new wxStaticText(this, wxID_ANY, _("Source")), 0,
		wxALIGN_CENTER_VERTICAL | wxALL, 5);
	auto* sourceChoice = new wxChoice(this, wxID_ANY);
	sourceChoice->SetMinSize({170, -1});
	sourceChoice->Append(source_account);
	sourceChoice->SetSelection(0);
	row1->Add(sourceChoice, 1, wxALL | wxEXPAND, 5);

	row1->Add(new wxStaticText(this, wxID_ANY, _("Target")), 0,
		wxALIGN_CENTER_VERTICAL | wxALL, 5);
	m_target_selection = new wxComboBox(this, wxID_ANY);
	for (const auto& account : m_emulationController.ListAccounts())
	{
		if (account.persistentId == m_source_id)
			continue;
		m_target_selection->Append(fmt::format("{:x} ({})", account.persistentId,
			boost::nowide::narrow(account.miiName)),
			(void*)(uintptr_t)account.persistentId);
	}
	row1->Add(m_target_selection, 1, wxALL | wxEXPAND, 5);
	sizer->Add(row1, 0, wxEXPAND, 5);

	auto* row2 = new wxFlexGridSizer(0, 2, 0, 0);
	row2->AddGrowableCol(1);
	auto* okButton = new wxButton(this, wxID_ANY, _("OK"));
	okButton->Bind(wxEVT_BUTTON, &SaveTransfer::OnTransfer, this);
	row2->Add(okButton, 0, wxALL, 5);
	auto* cancelButton = new wxButton(this, wxID_ANY, _("Cancel"));
	cancelButton->Bind(wxEVT_BUTTON, [this](auto&) {
		m_return_code = wxCANCEL;
		Close();
	});
	row2->Add(cancelButton, 0, wxALIGN_RIGHT | wxALL, 5);
	sizer->Add(row2, 1, wxEXPAND, 5);

	SetSizerAndFit(sizer);
	Centre(wxBOTH);
}

void SaveTransfer::EndModal(int retCode)
{
	wxDialog::EndModal(retCode);
	SetReturnCode(m_return_code);
}

void SaveTransfer::OnTransfer(wxCommandEvent& event)
{
	uint32 targetId{};
	const auto selection = m_target_selection->GetCurrentSelection();
	if (selection != wxNOT_FOUND)
		targetId = (uint32)(uintptr_t)m_target_selection->GetClientData(selection);
	if (targetId == 0)
	{
		targetId = ConvertString<uint32>(m_target_selection->GetValue().ToStdString(), 16);
		if (targetId < Application::kMinimumPersistentId)
		{
			wxMessageBox(formatWxString(
				_("The given account id is not valid!\nIt must be a hex number bigger or equal than {:08x}"),
				Application::kMinimumPersistentId), _("Error"),
				wxOK | wxCENTRE | wxICON_ERROR, this);
			return;
		}
	}

	const auto target = m_emulationController.InspectSaveEntry(m_title_id, targetId);
	if (target.state == Application::SaveEntryState::NonDirectory)
	{
		wxMessageBox(formatWxString(_("There's already a file at the target directory:\n{}"),
			_pathToUtf8(target.path)), _("Error"), wxOK | wxCENTRE |
			wxICON_ERROR, this);
		return;
	}
	bool overwrite{};
	if (target.state == Application::SaveEntryState::Directory)
	{
		const auto message = _("There's already a save game available for the target account, do you want to overwrite it?\nThis will delete the existing save files for the account and replace them.");
		if (wxMessageBox(message, _("Error"), wxYES_NO | wxCENTRE |
			wxICON_WARNING, this) == wxNO)
			return;
		overwrite = true;
	}

	const auto transferred = m_emulationController.TransferSave(m_title_id,
		m_source_id, targetId, overwrite);
	if (!transferred)
	{
		wxMessageBox(formatWxString(_("Error when trying to move the save game:\n{}"),
			transferred.diagnostic), _("Error"), wxOK | wxCENTRE, this);
		return;
	}

	m_target_id = targetId;
	m_return_code = wxOK;
	Close();
}
