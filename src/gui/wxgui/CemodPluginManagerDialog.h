#pragma once

#include "application/EmulationController.h"

#include <wx/dialog.h>

#include <cstdint>
#include <string>
#include <vector>

class wxCheckBox;
class wxCheckListBox;
class wxListBox;
class wxStaticText;
class wxTextCtrl;

// Approval UI for one title. It reads and writes the same CemuMod manager facade
// the CEF frontend uses, so exact-package approvals, the all-permissions rule for
// native packages, update trust, and the enable/disable flag behave identically.
class CemodPluginManagerDialog final : public wxDialog
{
  public:
	CemodPluginManagerDialog(wxWindow* parent, std::uint64_t titleId,
							 Application::EmulationController& emulationController);

  private:
	void RefreshPlugins();
	void ShowPlugin(std::size_t index);
	void SaveApproval();
	void TogglePlugin(std::size_t index);
	void RefreshLabels();
	void SyncTrustAvailability();
	[[nodiscard]] std::uint64_t CheckedPermissionMask() const;
	[[nodiscard]] std::string MissingPermissions() const;
	[[nodiscard]] static std::string StatusText(const Application::CemodManagerPackage& package);

	std::uint64_t m_titleId{};
	Application::EmulationController& m_emulationController;
	wxListBox* m_plugins{};
	wxCheckListBox* m_permissions{};
	wxCheckBox* m_approve{};
	wxCheckBox* m_trustUpdates{};
	wxStaticText* m_status{};
	wxTextCtrl* m_details{};
	Application::CemodManagerSnapshot m_snapshot;
	std::vector<std::uint64_t> m_permissionBits;
	std::vector<std::string> m_permissionNames;
};
