#pragma once

#include <wx/dialog.h>

namespace Application { class EmulationController; }

class SaveImportWindow : public wxDialog
{
public:
	SaveImportWindow(wxWindow* parent, Application::EmulationController& emulationController,
		uint64 title_id);

	void EndModal(int retCode) override;

	uint32 GetTargetPersistentId() const { return m_target_id; }
private:
	void OnImport(wxCommandEvent& event);

	class wxFilePickerCtrl* m_source_selection;
	class wxComboBox* m_target_selection;

	uint32 m_target_id = 0;
	const uint64 m_title_id;
	Application::EmulationController& m_emulationController;
	int m_return_code = wxCANCEL;
};
