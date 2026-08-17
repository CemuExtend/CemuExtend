#include "helpers/wxHelpers.h"
#include "wxgui/wxgui.h"
#include "wxgui/GraphicPacksWindow2.h"
#include "wxgui/DownloadGraphicPacksWindow.h"
#include "wxgui/DownloadCustomGraphicPackWindow.h"
#include "util/helpers/helpers.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif

// main.cpp
class wxGraphicPackData : public wxTreeItemData
{
public:
	explicit wxGraphicPackData(std::string key)
	 : m_key(std::move(key)) {  }

	const std::string& GetKey() const { return m_key; }

private:
	std::string m_key;
};

void GraphicPacksWindow2::FillGraphicPackList() const
{
	wxWindowUpdateLocker lock(m_graphic_pack_tree);

	m_graphic_pack_tree->DeleteAllItems();
	const auto graphic_packs = m_emulationController.ListGraphicPacks();

	const auto root = m_graphic_pack_tree->AddRoot("Root");

	const bool has_filter = !m_filter.empty();

	for(const auto& p : graphic_packs)
	{
		if (!p.universal)
		{
			// filter graphic packs by given title id
			if (m_filter_installed_games && !m_installed_games.empty())
			{
				bool found = false;
				for (uint64 titleId : p.titleIds)
				{
					if (std::find(m_installed_games.cbegin(), m_installed_games.cend(), titleId) != m_installed_games.cend())
					{
						found = true;
						break;
					}
				}
	
				if (!found)
					continue;
			}
	
			// filter graphic packs by given title id
			if(has_filter)
			{
				bool found = false;
	
				if (boost::icontains(p.virtualPath, m_filter))
					found = true;
				else
				{
					for (uint64 titleId : p.titleIds)
					{
						if (boost::icontains(fmt::format("{:x}", titleId), m_filter))
						{
							found = true;
							break;
						}
					}
				}
	
				if (!found)
					continue;
			}
		}

		const auto& path = p.virtualPath;
		auto tokens = TokenizeView(path, '/');
		auto node = root;
		for(size_t i=0; i<tokens.size(); i++)
		{
			auto& token = tokens[i];
			const auto parent_node = node;
			if (i < (tokens.size() - 1))
			{
				node = FindTreeItem(parent_node, wxString::FromUTF8(token));
				if (!node.IsOk())
				{
					node = m_graphic_pack_tree->AppendItem(parent_node, wxString::FromUTF8(token));
				}
			}
			else
			{
				// last token
				// if a node with same name already exists, add a number suffix
				for (sint32 s = 0; s < 999; s++)
				{
					wxString nodeName = wxString::FromUTF8(token);
					if (s > 0)
						nodeName.append(formatWxString(" #{}", s + 1));

					node = FindTreeItem(parent_node, nodeName);
					if (!node.IsOk())
					{
						node = m_graphic_pack_tree->AppendItem(parent_node, nodeName);
						break;
					}
				}
			}
		}

		if(node.IsOk() && node != root)
		{
			m_graphic_pack_tree->SetItemData(node, new wxGraphicPackData(p.key));
			bool canEnable = true;

			if (p.version == 3)
			{
				auto tmp_text = m_graphic_pack_tree->GetItemText(node);
				m_graphic_pack_tree->SetItemText(node, tmp_text + " (may not be compatible with Vulkan)");
			}
			else if (!p.supportedVersion)
			{
				auto tmp_text = m_graphic_pack_tree->GetItemText(node);
				m_graphic_pack_tree->SetItemText(node, tmp_text + " (Unsupported version)");
				m_graphic_pack_tree->SetItemTextColour(node, m_incompatible_colour);
				canEnable = false;
			}
			else if (p.activated)
				m_graphic_pack_tree->SetItemTextColour(node, m_activated_colour);

			m_graphic_pack_tree->MakeCheckable(node, p.enabled);
			if (!canEnable)
				m_graphic_pack_tree->DisableCheckBox(node);
		}
	}

	m_graphic_pack_tree->Sort(root, true);

	if (!m_filter.empty())
	{
		size_t counter = 0;
		ExpandChildren({ root }, counter);
	}
}


void GraphicPacksWindow2::GetChildren(const wxTreeItemId& id, std::vector<wxTreeItemId>& children) const
{
	wxTreeItemIdValue cookie;
	wxTreeItemId child = m_graphic_pack_tree->GetFirstChild(id, cookie);
	while (child.IsOk())
	{
		children.emplace_back(child);
		child = m_graphic_pack_tree->GetNextChild(child, cookie);
	}
}

void GraphicPacksWindow2::ExpandChildren(const std::vector<wxTreeItemId>& ids, size_t& counter) const
{
	std::vector<wxTreeItemId> children;
	for (const auto& id : ids)
		GetChildren(id, children);

	counter += children.size();
	if (counter >= 30 || children.empty())
		return;

	for (const auto& id : ids)
	{
		if(id != m_graphic_pack_tree->GetRootItem() && m_graphic_pack_tree->HasChildren(id))
			m_graphic_pack_tree->Expand(id);
	}

	ExpandChildren(children, counter);
}

GraphicPacksWindow2::GraphicPacksWindow2(wxWindow* parent, uint64_t title_id_filter,
	Application::EmulationController& emulationController)
	: wxDialog(parent, wxID_ANY, _("Graphic packs"), wxDefaultPosition, wxSize(1000,670), wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION | wxRESIZE_BORDER),
		m_emulationController(emulationController)
{
	for (const auto& title : m_emulationController.ListTitles())
		m_installed_games.push_back(title.titleId);
	if (title_id_filter != 0)
		m_filter = fmt::format("{:x}", title_id_filter);

	m_filter_installed_games = !m_installed_games.empty();

	SetIcon(wxICON(X_BOX));
	SetMinSize(wxSize(500, 400));
	auto main_sizer = new wxBoxSizer(wxVERTICAL);

	m_splitter_window = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_3D);
	m_splitter_window->Bind(wxEVT_SIZE, &GraphicPacksWindow2::OnSizeChanged, this);
	m_splitter_window->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, &GraphicPacksWindow2::SashPositionChanged, this);
	
	// left side
	auto left_panel = new wxPanel(m_splitter_window);
	{
		auto sizer = new wxBoxSizer(wxVERTICAL);

		wxFlexGridSizer* filter_row = new wxFlexGridSizer(0, 3, 0, 0);
		filter_row->AddGrowableCol(1);
		filter_row->SetFlexibleDirection(wxBOTH);
		filter_row->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

		const auto text = new wxStaticText(left_panel, wxID_ANY, _("Filter"));
		text->Wrap(-1);
		filter_row->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

		m_filter_text = new wxTextCtrl(left_panel, wxID_ANY, wxString::FromUTF8(m_filter));
		filter_row->Add(m_filter_text, 0, wxALL | wxEXPAND, 5);
		m_filter_text->Bind(wxEVT_COMMAND_TEXT_UPDATED, &GraphicPacksWindow2::OnFilterUpdate, this);

		m_installed_games_only = new wxCheckBox(left_panel, wxID_ANY, _("Installed games"));
		m_installed_games_only->SetValue(m_filter_installed_games);
		filter_row->Add(m_installed_games_only, 0, wxALL | wxEXPAND, 5);
		m_installed_games_only->Bind(wxEVT_CHECKBOX, &GraphicPacksWindow2::OnInstalledGamesChanged, this);
		if (m_installed_games.empty())
			m_installed_games_only->Disable();

		sizer->Add(filter_row, 0, wxEXPAND, 5);

		m_graphic_pack_tree = new wxCheckTree(left_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT);
		m_graphic_pack_tree->Bind(wxEVT_TREE_SEL_CHANGED, &GraphicPacksWindow2::OnTreeSelectionChanged, this);
		m_graphic_pack_tree->Bind(wxEVT_CHECKTREE_CHOICE, &GraphicPacksWindow2::OnTreeChoiceChanged, this);
		//m_graphic_pack_tree->SetMinSize(wxSize(600, 400));
		sizer->Add(m_graphic_pack_tree, 1, wxEXPAND | wxALL, 5);

		left_panel->SetSizerAndFit(sizer);
	}

	// right side
	m_right_panel = new wxPanel(m_splitter_window, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxNO_BORDER | wxFULL_REPAINT_ON_RESIZE);
	{
		auto* sizer = new wxBoxSizer(wxVERTICAL);
		{
			m_gp_options = new wxScrolled<wxPanel>(m_right_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxNO_BORDER | wxVSCROLL);
			m_gp_options->SetScrollRate(-1, 10);

			auto* inner_sizer = new wxBoxSizer(wxVERTICAL);
			{
				auto* box = new wxStaticBox(m_gp_options, wxID_ANY, _("Graphic pack"));
				auto* box_sizer = new wxStaticBoxSizer(box, wxVERTICAL);

				m_graphic_pack_name = new wxStaticText(box, wxID_ANY, wxEmptyString);
				box_sizer->Add(m_graphic_pack_name, 1, wxEXPAND | wxALL, 5);

				inner_sizer->Add(box_sizer, 0, wxEXPAND | wxALL, 5);
			}

			{
				auto* box = new wxStaticBox(m_gp_options, wxID_ANY, _("Description"));
				auto* box_sizer = new wxStaticBoxSizer(box, wxVERTICAL);

				m_graphic_pack_description = new wxStaticText(box, wxID_ANY, wxEmptyString);
				box_sizer->Add(m_graphic_pack_description, 1, wxEXPAND | wxALL, 5);

				inner_sizer->Add(box_sizer, 0, wxEXPAND | wxALL, 5);
			}

			m_preset_sizer = new wxBoxSizer(wxVERTICAL);
			inner_sizer->Add(m_preset_sizer, 0, wxEXPAND, 0);

			{
				auto* box = new wxStaticBox(m_gp_options, wxID_ANY, _("Control"));
				auto* box_sizer = new wxStaticBoxSizer(box, wxHORIZONTAL);

				m_reload_shaders = new wxButton(box, wxID_ANY, _("Reload edited shaders"));
				m_reload_shaders->Bind(wxEVT_BUTTON, &GraphicPacksWindow2::OnReloadShaders, this);
				m_reload_shaders->Disable();
				box_sizer->Add(m_reload_shaders, 0, wxEXPAND | wxALL, 5);

				inner_sizer->Add(box_sizer, 0, wxEXPAND | wxALL, 5);
			}

			inner_sizer->Add(new wxStaticText(m_gp_options, wxID_ANY, wxEmptyString), 1, wxALL | wxEXPAND, 5);

			m_gp_options->SetSizerAndFit(inner_sizer);
			m_gp_options->Hide();

			sizer->Add(m_gp_options, 1, wxEXPAND | wxRESERVE_SPACE_EVEN_IF_HIDDEN);
		}

		
		sizer->Add(new wxStaticLine(m_right_panel, wxID_ANY), 0, wxLEFT|wxRIGHT | wxEXPAND, 3);

		auto* row = new wxBoxSizer(wxHORIZONTAL);
		
		m_update_graphicPacks = new wxButton(m_right_panel, wxID_ANY, _("Download latest community graphic packs"));
		m_update_graphicPacks->Bind(wxEVT_BUTTON, &GraphicPacksWindow2::OnCheckForUpdates, this);
		row->Add(m_update_graphicPacks, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

		m_download_from_url = new wxHyperlinkCtrl(m_right_panel, wxID_ANY, _("Or download pack from URL..."), _(""));
		m_download_from_url->Bind(wxEVT_HYPERLINK, &GraphicPacksWindow2::OnClickCustomDownload, this);
		row->Add(m_download_from_url, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

		sizer->Add(row, 0, wxALL | wxEXPAND, 5);

		m_right_panel->SetSizerAndFit(sizer);
	}

	m_splitter_window->SetMinimumPaneSize(50);
	m_splitter_window->SplitVertically(left_panel, m_right_panel, (sint32)(m_ratio * m_splitter_window->GetParent()->GetSize().GetWidth()));
	main_sizer->Add(m_splitter_window, 1, wxEXPAND, 5);


	m_info_bar = new wxInfoBar(this);
	m_info_bar->SetShowHideEffects(wxSHOW_EFFECT_BLEND, wxSHOW_EFFECT_BLEND);
	m_info_bar->SetEffectDuration(500);
	main_sizer->Add(m_info_bar, 0, wxALL | wxEXPAND, 5);

	SetSizer(main_sizer);

	UpdateTitleRunning(m_emulationController.IsTitleRunning());
	FillGraphicPackList();
}

void GraphicPacksWindow2::SaveStateToConfig()
{
	m_emulationController.SaveGraphicPackState();
}

GraphicPacksWindow2::~GraphicPacksWindow2()
{
	m_graphic_pack_tree->Unbind(wxEVT_CHECKTREE_CHOICE, &GraphicPacksWindow2::OnTreeSelectionChanged, this);
	m_graphic_pack_tree->Unbind(wxEVT_CHECKTREE_CHOICE, &GraphicPacksWindow2::OnTreeChoiceChanged, this);
	// m_active_preset->Unbind(wxEVT_CHOICE, &GraphicPacksWindow2::OnActivePresetChanged, this);
	m_reload_shaders->Unbind(wxEVT_BUTTON, &GraphicPacksWindow2::OnReloadShaders, this);

	SaveStateToConfig();
}

wxTreeItemId GraphicPacksWindow2::FindTreeItem(const wxTreeItemId& root, const wxString& text) const
{
	wxTreeItemIdValue cookie;
	for(auto item = m_graphic_pack_tree->GetFirstChild(root, cookie); item.IsOk(); item = m_graphic_pack_tree->GetNextSibling(item))
	{
		if (m_graphic_pack_tree->GetItemText(item) == text)
			return item;
	}

	return {};
}

std::optional<Application::GraphicPackInfo> GraphicPacksWindow2::FindGraphicPack(
	std::string_view key) const
{
	const auto packs = m_emulationController.ListGraphicPacks();
	const auto found = std::ranges::find_if(packs,
		[key](const auto& pack) { return pack.key == key; });
	return found == packs.end() ? std::nullopt : std::optional{*found};
}

void GraphicPacksWindow2::LoadPresetSelections(
	const Application::GraphicPackInfo& graphicPack)
{
	for(const auto& category : graphicPack.presetOrder)
	{
		std::vector<const Application::GraphicPackPreset*> entry;
		for (const auto& preset : graphicPack.presets)
			if (preset.category == category)
				entry.push_back(&preset);
		
		// test if any preset is visible and update its status
		if (std::none_of(entry.cbegin(), entry.cend(), [](const auto* preset)
		{
			return preset->visible;
		}))
		{
			continue;
		}

		wxString categoryWxStr = wxString::FromUTF8(category);
		wxString label(category.empty() ? _("Active preset") : categoryWxStr);
		auto* box = new wxStaticBox(m_preset_sizer->GetContainingWindow(), wxID_ANY, label);
		auto* box_sizer = new wxStaticBoxSizer(box, wxVERTICAL);

		auto* preset = new wxChoice(box, wxID_ANY);
		preset->SetClientObject(new wxStringClientData(categoryWxStr));
		preset->Bind(wxEVT_CHOICE, &GraphicPacksWindow2::OnActivePresetChanged, this);

		std::optional<std::string> active_preset;
		for (const auto* pentry : entry)
		{
			if (!pentry->visible)
				continue;

			preset->AppendString(wxString::FromUTF8(pentry->name));
			if (pentry->active)
				active_preset = pentry->name;
		}

		if (active_preset)
			preset->SetStringSelection(wxString::FromUTF8(active_preset.value()));
		else if (preset->GetCount() > 0)
			preset->SetSelection(0);
					
		box_sizer->Add(preset, 1, wxEXPAND | wxALL, 5);

		m_preset_sizer->Add(box_sizer, 0, wxEXPAND | wxALL, 5);
	}
}

void GraphicPacksWindow2::OnTreeSelectionChanged(wxTreeEvent& event)
{
	wxWindowUpdateLocker lock(this);
	
	bool item_deselected = m_graphic_pack_tree->GetSelection() == m_graphic_pack_tree->GetRootItem(); 
	if (item_deselected)
		return;

	const auto selection = m_graphic_pack_tree->GetSelection();
	if (selection.IsOk())
	{
		const auto data = dynamic_cast<wxGraphicPackData*>(m_graphic_pack_tree->GetItemData(selection));
		if (data)
		{
			if(!m_gp_options->IsShown())
				m_gp_options->Show();
			
			const auto graphicPack = FindGraphicPack(data->GetKey());
			if (!graphicPack)
				return;
			if (graphicPack->key != m_shown_graphic_pack_key)
			{
				m_preset_sizer->Clear(true);
				m_gp_name = graphicPack->name;
				m_graphic_pack_name->SetLabel(wxString::FromUTF8(m_gp_name));

				if (graphicPack->description.empty())
					m_gp_description = _("This graphic pack has no description").utf8_string();
				else
					m_gp_description = graphicPack->description;

				m_graphic_pack_description->SetLabel(wxString::FromUTF8(m_gp_description));

				LoadPresetSelections(*graphicPack);
				
				m_reload_shaders->Enable(graphicPack->hasShaders);

				m_shown_graphic_pack_key = graphicPack->key;

				m_graphic_pack_name->Wrap(m_graphic_pack_name->GetParent()->GetClientSize().GetWidth() - 20);
				m_graphic_pack_name->GetGrandParent()->Layout();

				m_graphic_pack_description->Wrap(m_graphic_pack_description->GetParent()->GetClientSize().GetWidth() - 20);
				m_graphic_pack_description->GetGrandParent()->Layout();

				m_right_panel->FitInside();
				m_right_panel->Layout();	
			}
			return;
		}
	}

	m_preset_sizer->Clear(true);
	m_graphic_pack_name->SetLabel(wxEmptyString);
	m_graphic_pack_description->SetLabel(wxEmptyString);

	m_reload_shaders->Disable();

	m_shown_graphic_pack_key.clear();

	m_gp_options->Hide();
	m_right_panel->FitInside();
	m_right_panel->Layout();
}

void GraphicPacksWindow2::OnTreeChoiceChanged(wxTreeEvent& event)
{
	auto item = event.GetItem();
	if (!item.IsOk())
		return;

	const bool state = event.GetExtraLong() != 0;

	const auto data = dynamic_cast<wxGraphicPackData*>(m_graphic_pack_tree->GetItemData(item));
	if (!data)
		return;

	const auto result = m_emulationController.SetGraphicPackEnabled(data->GetKey(), state);
	if (!result)
	{
		m_graphic_pack_tree->Check(item, !state);
		wxMessageBox(wxString::FromUTF8(result.diagnostic), _("Graphic packs"),
			wxOK | wxICON_ERROR, this);
		return;
	}
	if (result.titleRunning && !result.requiresRestart)
		m_graphic_pack_tree->SetItemTextColour(item,
			state ? m_activated_colour : m_default_colour);

	if (!m_info_bar->IsShown() && result.titleRunning && result.requiresRestart)
		m_info_bar->ShowMessage(_("Restart of Cemu required for changes to take effect"));

	// also change selection to activated gp
	m_graphic_pack_tree->SelectItem(item);
}

// In some environments with GTK (e.g. a flatpak app with org.freedesktop.Platform 22.08 runtime),
// destroying the event source inside the handler crashes the app.
// As a workaround to that, the wxWindow that needs to be destroyed is hidden and then 
// destroyed at a later time, outside the handler.
void GraphicPacksWindow2::ClearPresets()
{
	size_t item_count = m_preset_sizer->GetItemCount();
	std::vector<wxSizer*> sizers;
	sizers.reserve(item_count);
	for (size_t i = 0; i < item_count; i++)
		sizers.push_back(m_preset_sizer->GetItem(i)->GetSizer());

	for (auto&& sizer : sizers)
	{
		auto static_box_sizer = dynamic_cast<wxStaticBoxSizer*>(sizer);
		if (static_box_sizer)
		{
			wxStaticBox* parent_window = static_box_sizer->GetStaticBox();
			if (parent_window)
			{
				m_preset_sizer->Detach(sizer);
				parent_window->Hide();
				CallAfter([=]()
				{
					parent_window->DestroyChildren();
					delete static_box_sizer;
				});
			}
		}
	}
}

void GraphicPacksWindow2::OnActivePresetChanged(wxCommandEvent& event)
{
	if (m_shown_graphic_pack_key.empty())
		return;

	const auto obj = wxDynamicCast(event.GetEventObject(), wxChoice);
	wxASSERT(obj);
	const auto string_data = dynamic_cast<wxStringClientData*>(obj->GetClientObject());
	wxASSERT(string_data);
	const auto preset = obj->GetStringSelection().utf8_string();
	const auto result = m_emulationController.SetGraphicPackPreset(
		m_shown_graphic_pack_key, string_data->GetData().utf8_string(), preset);
	if (result.changed && result.info)
	{
		wxWindowUpdateLocker lock(this);
		ClearPresets();
		LoadPresetSelections(*result.info);
		//m_preset_sizer->GetContainingWindow()->Layout();
		//m_right_panel->FitInside();
		m_right_panel->FitInside();
		m_right_panel->Layout();
	}

	if (result.requiresRestart && !m_info_bar->IsShown())
		m_info_bar->ShowMessage(_("Restart of Cemu required for changes to take effect"));		
}

void GraphicPacksWindow2::OnReloadShaders(wxCommandEvent& event)
{
	if (!m_shown_graphic_pack_key.empty())
		(void)m_emulationController.ReloadGraphicPack(m_shown_graphic_pack_key);
}

void GraphicPacksWindow2::OnClickCustomDownload(wxCommandEvent& event)
{
	DownloadCustomGraphicPackWindow frame(this);
	if (frame.ShowModal() == wxID_OK)
	{
		if (m_emulationController.RefreshGraphicPacks())
			FillGraphicPackList();
	}
}

void GraphicPacksWindow2::OnCheckForUpdates(wxCommandEvent& event)
{
	DownloadGraphicPacksWindow frame(this, m_emulationController);
	SaveStateToConfig();
	const int updateResult = frame.ShowModal();
	if (updateResult == wxID_OK)
	{
		const auto refresh = m_emulationController.RefreshGraphicPacks();
		if (refresh)
		{
			FillGraphicPackList();
			if(!refresh.removedEnabledPaths.empty())
			{
				std::string lost_packs;
				for(const auto& path : refresh.removedEnabledPaths)
				{
					lost_packs.append(path);
					lost_packs.push_back('\n');
				}
				wxString message = _("This update removed or renamed the following graphic packs:");
				message << "\n \n" << wxString::FromUTF8(lost_packs) << " \n" << _("You may need to set them up again.");
				wxMessageBox(message, _("Warning"), wxOK | wxCENTRE | wxICON_INFORMATION, this);
			}
		}
		else if (!refresh.diagnostic.empty())
			wxMessageBox(wxString::FromUTF8(refresh.diagnostic), _("Graphic packs"),
				wxOK | wxICON_WARNING, this);
	}
}

void GraphicPacksWindow2::OnSizeChanged(wxSizeEvent& event)
{
	const auto obj = (wxSplitterWindow*)event.GetEventObject();
	wxASSERT(obj);

	const auto width = std::max(obj->GetMinimumPaneSize(), obj->GetParent()->GetClientSize().GetWidth());
	obj->SetSashPosition((sint32)(m_ratio * width));

	if (!m_gp_name.empty())
		m_graphic_pack_name->SetLabel(wxString::FromUTF8(m_gp_name));

	if (!m_gp_description.empty())
		m_graphic_pack_description->SetLabel(wxString::FromUTF8(m_gp_description));

	m_graphic_pack_name->Wrap(m_graphic_pack_name->GetParent()->GetClientSize().GetWidth() - 10);
	m_graphic_pack_description->Wrap(m_graphic_pack_description->GetParent()->GetClientSize().GetWidth() - 10);

	event.Skip();
}

void GraphicPacksWindow2::SashPositionChanged(wxEvent& event)
{
	const auto obj = (wxSplitterWindow*)event.GetEventObject();
	wxASSERT(obj);

	const auto width = std::max(obj->GetMinimumPaneSize(), obj->GetParent()->GetClientSize().GetWidth());
	m_ratio = (float)obj->GetSashPosition() / width;
	event.Skip();
}

void GraphicPacksWindow2::OnFilterUpdate(wxEvent& event)
{
	m_filter = m_filter_text->GetValue().utf8_string();
	FillGraphicPackList();
	event.Skip();
}

void GraphicPacksWindow2::OnInstalledGamesChanged(wxCommandEvent& event)
{
	m_filter_installed_games = m_installed_games_only->GetValue();
	FillGraphicPackList();
	event.Skip();
}

void GraphicPacksWindow2::UpdateTitleRunning(bool running)
{
	m_update_graphicPacks->Enable(!running);
	m_download_from_url->Enable(!running);
	if(running)
	{
		m_download_from_url->SetToolTip(_("Graphic packs cannot be updated while a game is running."));
		m_update_graphicPacks->SetToolTip(_("Graphic packs cannot be updated while a game is running."));
	}
	else
	{
		m_update_graphicPacks->SetToolTip(nullptr);
		m_download_from_url->SetToolTip(nullptr);
	}
}
