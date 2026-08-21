#pragma once

#include "CemodManagementModel.h"
#include "application/EmulationController.h"

#include <wx/dialog.h>

#include <cstdint>
#include <optional>
#include <vector>

class wxCheckListBox;
class wxListBox;
class wxStaticText;
class wxTextCtrl;

class CemodPluginManagerDialog final : public wxDialog
{
  public:
	CemodPluginManagerDialog(wxWindow* parent, std::uint64_t titleId,
							 Application::EmulationController& emulationController);

  private:
	void RefreshPlugins();
	void ShowPlugin(std::size_t index);
	void SaveApproval();
	void TogglePlugin(std::size_t index, bool enabled);
	[[nodiscard]] std::optional<CemuExtend::CemodApproval> LoadApproval(
		const CemodGuiPackageInfo& info) const;

	std::uint64_t m_titleId{};
	Application::EmulationController& m_emulationController;
	wxListBox* m_plugins{};
	wxCheckListBox* m_permissions{};
	wxStaticText* m_status{};
	wxTextCtrl* m_details{};
	std::vector<CemodGuiPackageInfo> m_infos;
	std::vector<CemodPluginView> m_views;
	std::vector<std::uint64_t> m_permissionBits;
};
