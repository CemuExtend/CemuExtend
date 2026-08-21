#pragma once

#include <wx/dialog.h>

class wxComboBox;
namespace Application
{
	class EmulationController;
}

class SaveTransfer : public wxDialog
{
  public:
	SaveTransfer(wxWindow* parent, Application::EmulationController& emulationController,
				 uint64 title_id, const wxString& source_account, uint32 source_id);

	void EndModal(int retCode) override;

	uint32 GetTargetPersistentId() const
	{
		return m_target_id;
	}

  private:
	void OnTransfer(wxCommandEvent& event);

	wxComboBox* m_target_selection;

	uint32 m_target_id = 0;
	const uint64 m_title_id;
	const uint32 m_source_id;
	Application::EmulationController& m_emulationController;
	int m_return_code = wxCANCEL;
};
