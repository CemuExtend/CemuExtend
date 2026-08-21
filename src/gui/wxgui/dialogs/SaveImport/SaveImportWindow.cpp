#include "SaveImportWindow.h"
#include "application/EmulationController.h"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/filepicker.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "util/helpers/helpers.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/wxHelper.h"

SaveImportWindow::SaveImportWindow(wxWindow* parent,
								   Application::EmulationController& emulationController, uint64 title_id)
	: wxDialog(parent, wxID_ANY, _("Import save entry"), wxDefaultPosition,
			   wxDefaultSize, wxCAPTION | wxFRAME_TOOL_WINDOW | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_title_id(title_id), m_emulationController(emulationController)
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	auto* row1 = new wxFlexGridSizer(0, 2, 0, 0);
	row1->AddGrowableCol(1);
	row1->Add(new wxStaticText(this, wxID_ANY, _("Source")), 0,
			  wxALIGN_CENTER_VERTICAL | wxALL, 5);
	m_source_selection = new wxFilePickerCtrl(this, wxID_ANY, wxEmptyString,
											  _("Select a zipped save file"),
											  formatWxString("{}|*.zip", _("Save entry (*.zip)")));
	m_source_selection->SetMinSize({270, -1});
	row1->Add(m_source_selection, 1, wxALL | wxEXPAND, 5);

	row1->Add(new wxStaticText(this, wxID_ANY, _("Target")), 0,
			  wxALIGN_CENTER_VERTICAL | wxALL, 5);
	m_target_selection = new wxComboBox(this, wxID_ANY);
	for (const auto& account : m_emulationController.ListAccounts())
	{
		m_target_selection->Append(fmt::format("{:x} ({})", account.persistentId,
											   boost::nowide::narrow(account.miiName)),
								   (void*)(uintptr_t)account.persistentId);
	}
	row1->Add(m_target_selection, 1, wxALL | wxEXPAND, 5);
	sizer->Add(row1, 0, wxEXPAND, 5);

	auto* row2 = new wxFlexGridSizer(0, 2, 0, 0);
	row2->AddGrowableCol(1);
	auto* okButton = new wxButton(this, wxID_ANY, _("OK"));
	okButton->Bind(wxEVT_BUTTON, &SaveImportWindow::OnImport, this);
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

void SaveImportWindow::EndModal(int retCode)
{
	wxDialog::EndModal(retCode);
	SetReturnCode(m_return_code);
}

void SaveImportWindow::OnImport(wxCommandEvent& event)
{
	const auto source = wxHelper::MakeFSPath(m_source_selection->GetPath());
	if (source.empty())
		return;

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
							 Application::kMinimumPersistentId),
						 _("Error"),
						 wxOK | wxCENTRE | wxICON_ERROR, this);
			return;
		}
	}

	const auto inspection = m_emulationController.InspectSaveImport(source,
																	m_title_id, targetId);
	if (!inspection)
	{
		wxMessageBox(formatWxString(_("The save archive could not be imported:\n{}"),
									inspection.diagnostic),
					 _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}
	if (inspection.sourceTitleId && *inspection.sourceTitleId != 0 &&
		*inspection.sourceTitleId != m_title_id)
	{
		const auto message = formatWxString(
			_("You are trying to import a savegame for a different title than your currently selected one: {:016x} vs {:016x}\nAre you sure that you want to continue?"),
			*inspection.sourceTitleId, m_title_id);
		if (wxMessageBox(message, _("Error"), wxYES_NO | wxCENTRE | wxICON_WARNING, this) == wxNO)
			return;
	}
	if (inspection.target.state == Application::SaveEntryState::NonDirectory)
	{
		wxMessageBox(formatWxString(_("There's already a file at the target directory:\n{}"),
									_pathToUtf8(inspection.target.path)),
					 _("Error"),
					 wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}

	bool overwrite{};
	if (inspection.target.state == Application::SaveEntryState::Directory)
	{
		const auto message = _("There's already a save game available for the target account, do you want to overwrite it?\nThis will delete the existing save files for the account and replace them.");
		if (wxMessageBox(message, _("Error"), wxYES_NO | wxCENTRE | wxICON_WARNING, this) == wxNO)
			return;
		overwrite = true;
	}

	const auto imported = m_emulationController.ImportSave(source, m_title_id,
														   targetId, overwrite);
	if (!imported)
	{
		wxMessageBox(formatWxString(_("The save archive could not be imported:\n{}"),
									imported.diagnostic),
					 _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}

	m_target_id = targetId;
	m_return_code = wxOK;
	Close();
}
