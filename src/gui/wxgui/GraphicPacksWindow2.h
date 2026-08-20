#pragma once

#include <wx/dialog.h>
#include <wx/scrolwin.h>
#include <wx/infobar.h>
#include <wx/hyperlink.h>

#include "wxcomponents/checktree.h"
#include "application/EmulationController.h"

#include <memory>

class wxSplitterWindow;
class wxPanel;
class wxButton;
class wxChoice;
namespace Host { class IPathProvider; }

class GraphicPacksWindow2 : public wxDialog
{
public:
	GraphicPacksWindow2(wxWindow* parent, uint64_t title_id_filter,
		Application::EmulationController& emulationController,
		std::shared_ptr<class IWxUiDispatcher> uiDispatcher,
		std::shared_ptr<Host::IPathProvider> pathProvider);
	~GraphicPacksWindow2();

	void UpdateTitleRunning(bool running);

private:
	std::string m_filter;
	bool m_filter_installed_games;
	std::vector<uint64_t> m_installed_games;
	Application::EmulationController& m_emulationController;
	std::shared_ptr<IWxUiDispatcher> m_uiDispatcher;
	std::shared_ptr<Host::IPathProvider> m_pathProvider;

	void ClearPresets();
	void FillGraphicPackList() const;
	void GetChildren(const wxTreeItemId& id, std::vector<wxTreeItemId>& children) const;
	void ExpandChildren(const std::vector<wxTreeItemId>& ids, size_t& counter) const;
	
	wxSplitterWindow * m_splitter_window;

	wxPanel* m_right_panel;
	wxScrolled<wxPanel>* m_gp_options;
	
	wxCheckTree * m_graphic_pack_tree;
	wxTextCtrl* m_filter_text;
	wxCheckBox* m_installed_games_only;

	wxStaticText* m_graphic_pack_name, *m_graphic_pack_description;
	wxBoxSizer* m_preset_sizer;
	std::vector<wxChoice*> m_active_preset;
	wxButton* m_reload_shaders;
	wxHyperlinkCtrl* m_download_from_url;
	wxButton* m_update_graphicPacks;
	wxInfoBar* m_info_bar;

	std::string m_shown_graphic_pack_key;
	std::string m_gp_name, m_gp_description;

	float m_ratio = 0.55f;
	wxColour m_default_colour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	wxColour m_activated_colour = wxSystemSettings::SelectLightDark(wxColour(0x00, 0xCC, 0x00), wxColour(0x42, 0xB3, 0x42));
	wxColour m_incompatible_colour = wxSystemSettings::SelectLightDark(wxColour(0xCC, 0x00, 0x00), wxColour(0xDE, 0x49, 0x49));

	wxTreeItemId FindTreeItem(const wxTreeItemId& root, const wxString& text) const;
	void LoadPresetSelections(const Application::GraphicPackInfo& graphicPack);
	[[nodiscard]] std::optional<Application::GraphicPackInfo> FindGraphicPack(
		std::string_view key) const;

	void OnTreeSelectionChanged(wxTreeEvent& event);
	void OnTreeChoiceChanged(wxTreeEvent& event);
	void OnActivePresetChanged(wxCommandEvent& event);
	void OnReloadShaders(wxCommandEvent& event);
	void OnClickCustomDownload(wxCommandEvent& event);
	void OnCheckForUpdates(wxCommandEvent& event);
	void OnSizeChanged(wxSizeEvent& event);
	void SashPositionChanged(wxEvent& event);
	void OnFilterUpdate(wxEvent& event);
	void OnInstalledGamesChanged(wxCommandEvent& event);

	void SaveStateToConfig();

};
