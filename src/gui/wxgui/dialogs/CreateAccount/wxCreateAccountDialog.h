#pragma once
#include <wx/dialog.h>

namespace Application
{
	class EmulationController;
}

class wxCreateAccountDialog : public wxDialog
{
  public:
	wxCreateAccountDialog(wxWindow* parent,
						  Application::EmulationController& emulationController);

	[[nodiscard]] uint32 GetPersistentId() const;
	[[nodiscard]] wxString GetMiiName() const;

  private:
	class wxTextCtrl* m_persistent_id;
	class wxTextCtrl* m_mii_name;
	class wxButton *m_ok_button, *m_cancel_buton;
	Application::EmulationController& m_emulationController;

	void OnOK(wxCommandEvent& event);
	void OnCancel(wxCommandEvent& event);
};
