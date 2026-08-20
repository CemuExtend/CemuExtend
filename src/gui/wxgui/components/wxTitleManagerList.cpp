#include "wxgui/components/wxTitleManagerList.h"
#include "wxgui/WxFrontendContext.h"
#include "application/EmulationController.h"
#include "wxgui/helpers/wxHelpers.h"
#include "util/helpers/SystemException.h"
#include "wxgui/components/wxGameList.h"
#include "wxgui/helpers/wxCustomEvents.h"
#include "wxgui/helpers/wxHelpers.h"

#include <wx/imaglist.h>
#include <wx/app.h>
#include <wx/wupdlock.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/filedlg.h>
#include "../wxHelper.h"

#include <functional>

#include "config/ActiveSettings.h"
#include "wxgui/ChecksumTool.h"

wxDEFINE_EVENT(wxEVT_TITLE_FOUND, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_TITLE_REMOVED, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_TITLE_SEARCH_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_REMOVE_ENTRY, wxCommandEvent);

wxTitleManagerList::wxTitleManagerList(wxWindow* parent,
	Application::EmulationController& emulationController,
	std::shared_ptr<IWxUiDispatcher> uiDispatcher,
	std::shared_ptr<Host::IPathProvider> pathProvider,
	std::function<void(fs::path)> requestLaunch, wxWindowID id)
	: wxListView(parent, id, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL),
	  m_emulationController(emulationController),
	  m_uiDispatcher(std::move(uiDispatcher)),
	  m_pathProvider(std::move(pathProvider)),
	  m_requestLaunch(std::move(requestLaunch)),
	  m_lifetime(std::make_shared<std::atomic_bool>(true))
{
	cemu_assert(m_uiDispatcher && m_pathProvider && static_cast<bool>(m_requestLaunch));
	AddColumns();

	// tooltip TODO: extract class mb wxPanelTooltip
	m_tooltip_window = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNO_BORDER);
	auto* tooltip_sizer = new wxBoxSizer(wxVERTICAL);
	m_tooltip_text = new wxStaticText(m_tooltip_window, wxID_ANY, wxEmptyString);
	tooltip_sizer->Add(m_tooltip_text , 0, wxALL, 5);
	m_tooltip_window->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_INFOBK));
	m_tooltip_window->SetSizerAndFit(tooltip_sizer);
	m_tooltip_window->Hide();
	m_tooltip_timer = new wxTimer(this);

	Bind(wxEVT_LIST_COL_CLICK, &wxTitleManagerList::OnColumnClick, this);
	Bind(wxEVT_CONTEXT_MENU, &wxTitleManagerList::OnContextMenu, this);
	Bind(wxEVT_LIST_ITEM_SELECTED, &wxTitleManagerList::OnItemSelected, this);
	Bind(wxEVT_TIMER, &wxTitleManagerList::OnTimer, this);
	Bind(wxEVT_REMOVE_ITEM, &wxTitleManagerList::OnRemoveItem, this);
	Bind(wxEVT_REMOVE_ENTRY, &wxTitleManagerList::OnRemoveEntry, this);
	Bind(wxEVT_TITLE_FOUND, &wxTitleManagerList::OnTitleDiscovered, this);
	Bind(wxEVT_TITLE_REMOVED, &wxTitleManagerList::OnTitleRemoved, this);
	Bind(wxEVT_CLOSE_WINDOW, &wxTitleManagerList::OnClose, this);

	auto lifetime = m_lifetime;
	m_titleSubscription = m_emulationController.SubscribeTitleCatalog(
		[this, lifetime = std::move(lifetime)](const Application::TitleCatalogEvent& event) {
			if (!lifetime->load(std::memory_order_acquire) || !wxTheApp)
				return;
			(void)m_uiDispatcher->Queue([this, lifetime, event] {
				if (lifetime->load(std::memory_order_acquire))
					HandleTitleCatalogEvent(event);
			});
		});

	ShowSortIndicator(ColumnTitleId);
}

wxTitleManagerList::~wxTitleManagerList()
{
	m_lifetime->store(false, std::memory_order_release);
	m_titleSubscription.Reset();
	m_conversionCancelled.store(true, std::memory_order_release);
	if (m_conversionWorker.valid())
	{
		try
		{
			(void)m_conversionWorker.get();
		}
		catch (...)
		{
		}
	}
}

boost::optional<const wxTitleManagerList::TitleEntry&> wxTitleManagerList::GetSelectedTitleEntry() const
{
	const auto selection = GetFirstSelected();
	if (selection != wxNOT_FOUND)
	{
		const auto tmp = GetTitleEntry(selection);
		if (tmp.has_value())
			return tmp.value();
	}

	return {};
}

boost::optional<wxTitleManagerList::TitleEntry&> wxTitleManagerList::GetSelectedTitleEntry()
{
	const auto selection = GetFirstSelected();
	if (selection != wxNOT_FOUND)
	{
		const auto tmp = GetTitleEntry(selection);
		if (tmp.has_value())
			return tmp.value();
	}

	return {};
}

//boost::optional<wxTitleManagerList::TitleEntry&> wxTitleManagerList::GetTitleEntry(EntryType type, uint64 titleid)
//{
//	for(const auto& v : m_data)
//	{
//		if (v->entry.title_id == titleid && v->entry.type == type)
//			return v->entry;
//	}
//
//	return {};
//}

boost::optional<wxTitleManagerList::TitleEntry&> wxTitleManagerList::GetTitleEntryByUID(uint64 uid)
{
	for (const auto& v : m_data)
	{
		if (v->entry.location_uid == uid)
			return v->entry;
	}
	return {};
}

void wxTitleManagerList::AddColumns()
{
	wxListItem col0;
	col0.SetId(ColumnTitleId);
	col0.SetText(_("Title ID"));
	col0.SetWidth(120);
	InsertColumn(ColumnTitleId, col0);

	wxListItem col1;
	col1.SetId(ColumnName);
	col1.SetText(_("Name"));
	col1.SetWidth(435);
	InsertColumn(ColumnName, col1);

	wxListItem col2;
	col2.SetId(ColumnType);
	col2.SetText(_("Type"));
	col2.SetWidth(65);
	InsertColumn(ColumnType, col2);

	wxListItem col3;
	col3.SetId(ColumnVersion);
	col3.SetText(_("Version"));
	col3.SetWidth(40);
	InsertColumn(ColumnVersion, col3);

	wxListItem col4;
	col4.SetId(ColumnRegion);
	col4.SetText(_("Region"));
	col4.SetWidth(60);
	InsertColumn(ColumnRegion, col4);

	wxListItem col5;
	col5.SetId(ColumnFormat);
	col5.SetText(_("Format"));
	col5.SetWidth(63);
	InsertColumn(ColumnFormat, col5);

	wxListItem col6;
	col6.SetId(ColumnLocation);
	col6.SetText(_("Location"));
	col6.SetWidth(63);
	InsertColumn(ColumnLocation, col6);
}

wxString wxTitleManagerList::OnGetItemText(long item, long column) const
{
	if (item >= GetItemCount())
		return wxEmptyString;

	const auto entry = GetTitleEntry(item);
	if (entry.has_value())
		return GetTitleEntryText(entry.value(), (ItemColumn)column);

	return wxEmptyString;
}

wxItemAttr* wxTitleManagerList::OnGetItemAttr(long item) const
{
	static wxColour bgColourPrimary = GetBackgroundColour();
	static wxColour bgColourSecondary = wxHelper::CalculateAccentColour(bgColourPrimary);
	static wxListItemAttr s_primary_attr(GetTextColour(), bgColourPrimary, GetFont());
	static wxListItemAttr s_secondary_attr(GetTextColour(), bgColourSecondary, GetFont());
	return item % 2 == 0 ? &s_primary_attr : &s_secondary_attr;
}

boost::optional<wxTitleManagerList::TitleEntry&> wxTitleManagerList::GetTitleEntry(long item)
{
	long counter = 0;
	for (const auto& data : m_sorted_data)
	{
		if (!data.get().visible)
			continue;

		if (item != counter++)
			continue;

		return data.get().entry;
	}
	
	return {};
}

boost::optional<const wxTitleManagerList::TitleEntry&> wxTitleManagerList::GetTitleEntry(const fs::path& path) const
{
	const auto tmp = _pathToUtf8(path);
	for (const auto& data : m_data)
	{
		if (boost::iequals(_pathToUtf8(data->entry.path), tmp))
			return data->entry;
	}

	return {};
}
boost::optional<wxTitleManagerList::TitleEntry&> wxTitleManagerList::GetTitleEntry(const fs::path& path)
{
	const auto tmp = _pathToUtf8(path);
	for (const auto& data : m_data)
	{
		if (boost::iequals(_pathToUtf8(data->entry.path), tmp))
			return data->entry;
	}

	return {};
}

boost::optional<const wxTitleManagerList::TitleEntry&> wxTitleManagerList::GetTitleEntry(long item) const
{
	long counter = 0;
	for (const auto& data : m_sorted_data)
	{
		if (!data.get().visible)
			continue;

		if (item != counter++)
			continue;

		return data.get().entry;
	}

	return {};
}

void wxTitleManagerList::RunWuaConversion(uint64 titleId, uint64 rightClickedUID)
{
	if (m_conversionActive.exchange(true, std::memory_order_acq_rel))
		return;
	struct ActiveReset
	{
		std::atomic_bool& active;
		~ActiveReset() { active.store(false, std::memory_order_release); }
	} activeReset{m_conversionActive};
	m_conversionCancelled.store(false, std::memory_order_release);

	const auto plan = m_emulationController.PlanWuaConversion(titleId, rightClickedUID);
	if (!plan || plan->items.empty())
	{
		wxMessageBox(_("No installed content was found for conversion."), _("Error"),
			wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}

	auto findItem = [&plan](Application::ContentRole role) -> const Application::WuaContentItem* {
		const auto found = std::ranges::find_if(plan->items,
			[role](const auto& item) { return item.role == role; });
		return found == plan->items.end() ? nullptr : &*found;
	};
	auto appendItem = [](wxString& message, const wxString& label,
		const Application::WuaContentItem* item) {
		message.append(formatWxString("{}:\n{}", label,
			item ? wxString::FromUTF8(item->displayPath) : _("Not installed")));
	};
	wxString message = _("The following content will be converted to a compressed Wii U archive file (.wua):");
	message.append("\n \n");
	appendItem(message, _("Base game"), findItem(Application::ContentRole::Base));
	message.append("\n\n");
	appendItem(message, _("Update"), findItem(Application::ContentRole::Update));
	message.append("\n\n");
	appendItem(message, _("DLC"), findItem(Application::ContentRole::Dlc));
	if (wxMessageBox(message, _("Confirmation"), wxOK | wxCANCEL | wxCENTRE |
		wxICON_QUESTION, this) != wxOK)
		return;

	std::string defaultDirectory;
	if (!GetConfig().game_paths.empty())
		defaultDirectory = GetConfig().game_paths.front();
	wxFileDialog saveDialog(this, _("Save Wii U game archive file"), defaultDirectory,
		wxString::FromUTF8(plan->suggestedFileName), "WUA files (*.wua)|*.wua",
		wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (saveDialog.ShowModal() == wxID_CANCEL || saveDialog.GetPath().IsEmpty())
		return;

	std::vector<std::uint64_t> locationUids;
	locationUids.reserve(plan->items.size());
	for (const auto& item : plan->items)
		locationUids.push_back(item.locationUid);

	struct SharedProgress
	{
		std::mutex mutex;
		Application::ContentOperationProgress value;
	} sharedProgress;
	const auto outputPath = wxHelper::MakeFSPath(saveDialog.GetPath());
	auto* controller = &m_emulationController;
	m_conversionWorker = std::async(std::launch::async,
		[controller, locationUids = std::move(locationUids), outputPath, &sharedProgress,
			cancelled = &m_conversionCancelled] {
			return controller->ConvertToWua(locationUids, outputPath,
				[&sharedProgress](const auto& progress) {
					std::scoped_lock lock(sharedProgress.mutex);
					sharedProgress.value = progress;
				}, [cancelled] { return cancelled->load(std::memory_order_acquire); });
		});

	wxGenericProgressDialog progressDialog(_("Converting to .wua"), _("Counting files..."),
		100, this, wxPD_CAN_ABORT);
	progressDialog.Show();
	while (!future_is_ready(m_conversionWorker))
	{
		Application::ContentOperationProgress progress;
		{
			std::scoped_lock lock(sharedProgress.mutex);
			progress = sharedProgress.value;
		}
		std::uint32_t percent{};
		wxString status;
		if (m_conversionCancelled.load(std::memory_order_acquire))
			status = _("Stopping...");
		else if (progress.phase == Application::ContentOperationPhase::Finalizing)
		{
			percent = 99;
			status = _("Finalizing...");
		}
		else if (progress.phase == Application::ContentOperationPhase::Converting)
		{
			if (progress.bytesTotal != 0)
				percent = static_cast<std::uint32_t>(std::min<std::uint64_t>(99,
					progress.bytesCompleted * 100 / progress.bytesTotal));
			status = formatWxString(_("Converting files... ({0}MiB/{1}MiB)"),
				progress.bytesCompleted / 1024 / 1024, progress.bytesTotal / 1024 / 1024);
		}
		else
		{
			status = formatWxString(_("Collecting list of files... ({})"), progress.filesTotal);
		}
		progressDialog.Update(percent, status);
		if (progressDialog.WasCancelled())
			m_conversionCancelled.store(true, std::memory_order_release);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	Application::ContentOperationResult result;
	try
	{
		result = m_conversionWorker.get();
	}
	catch (const std::exception& exception)
	{
		result = {Application::ContentOperationError::ReadFailure, exception.what()};
	}
	catch (...)
	{
		result = {Application::ContentOperationError::ReadFailure,
			"Unknown archive conversion failure"};
	}
	progressDialog.Hide();
	if (result.error == Application::ContentOperationError::Cancelled)
		return;
	if (!result)
	{
		wxMessageBox(wxString::FromUTF8(result.diagnostic), _("Conversion failed"),
			wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}
	wxMessageBox(_("Conversion finished\n"), _("Complete"),
		wxOK | wxCENTRE | wxICON_INFORMATION, this);
}

void wxTitleManagerList::OnConvertToCompressedFormat(uint64 titleId, uint64 rightClickedUID)
{
	RunWuaConversion(titleId, rightClickedUID);
}
void wxTitleManagerList::OnClose(wxCloseEvent& event)
{
	if (m_conversionActive.load(std::memory_order_acquire))
	{
		m_conversionCancelled.store(true, std::memory_order_release);
		if (event.CanVeto())
		{
			event.Veto();
			return;
		}
	}
	event.Skip();
	// wait until all tasks are complete
	if (m_context_worker.valid())
		m_context_worker.get();
}

void wxTitleManagerList::OnColumnClick(wxListEvent& event)
{
	const int column = event.GetColumn();
	SortEntries(column);
	event.Skip();
}

void wxTitleManagerList::RemoveItem(long item)
{
	const int item_count = GetItemCount();

	const ItemData* ref = nullptr;
	long counter = 0;
	for(auto it = m_sorted_data.begin(); it != m_sorted_data.end(); ++it)
	{
		if (!it->get().visible)
			continue;

		if (item != counter++)
			continue;

		ref = &(it->get());
		m_sorted_data.erase(it);
		break;
	}

	// shouldn't happen
	if (ref == nullptr)
		return;
	
	for(auto it = m_data.begin(); it != m_data.end(); ++it)
	{
		if (ref != (*it).get())
			continue;
		
		m_data.erase(it);
		break;
	}

	SetItemCount(std::max(0, item_count - 1));
	RefreshPage();
}

void wxTitleManagerList::RemoveItem(const TitleEntry& entry)
{
	const int item_count = GetItemCount();

	const TitleEntry* ref = &entry;
	for (auto it = m_sorted_data.begin(); it != m_sorted_data.end(); ++it)
	{
		if (ref != &it->get().entry)
			continue;

		m_sorted_data.erase(it);
		break;
	}

	for (auto it = m_data.begin(); it != m_data.end(); ++it)
	{
		if (ref != &(*it).get()->entry)
			continue;

		m_data.erase(it);
		break;
	}

	SetItemCount(std::max(0, item_count - 1));
	RefreshPage();
}

void wxTitleManagerList::OnItemSelected(wxListEvent& event)
{
	event.Skip();
	m_tooltip_timer->Stop();
	const auto selection = event.GetIndex();

	if (selection == wxNOT_FOUND)
	{
		m_tooltip_window->Hide();
		return;
	}

	const auto entry = GetTitleEntry(selection);
	if (!entry.has_value())
	{
		m_tooltip_window->Hide();
		return;
	}

	m_tooltip_window->Hide();
	return;

	//const auto mouse_position = wxGetMousePosition() - GetScreenPosition();
	//m_tooltip_window->SetPosition(wxPoint(mouse_position.x + 15, mouse_position.y + 15));

	//wxString msg;
	//switch(entry->error)
	//{
	//case TitleError::WrongBaseLocation:
	//	msg = _("This base game is installed at the wrong location.");
	//	break;
	//case TitleError::WrongUpdateLocation:
	//	msg = _("This update is installed at the wrong location.");
	//	break;
	//case TitleError::WrongDlcLocation:
	//	msg = _("This DLC is installed at the wrong location.");
	//	break;
	//default:
	//	return;;
	//}

	//m_tooltip_text->SetLabel(formatWxString("{}\n{}", msg, _("You can use the context menu to fix it.")));
	//m_tooltip_window->Fit();
	//m_tooltip_timer->StartOnce(250);
}

enum ContextMenuEntries
{
	kContextMenuOpenDirectory = wxID_HIGHEST + 1,
	kContextMenuDelete,
	kContextMenuLaunch,
	kContextMenuVerifyGameFiles,
	kContextMenuConvertToWUA,
};
void wxTitleManagerList::OnContextMenu(wxContextMenuEvent& event)
{
	// still doing work
	if (m_context_worker.valid() && !future_is_ready(m_context_worker))
		return;
	
	wxMenu menu;
	menu.Bind(wxEVT_COMMAND_MENU_SELECTED, &wxTitleManagerList::OnContextMenuSelected, this);

	const auto selection = GetFirstSelected();
	if (selection == wxNOT_FOUND)
		return;

	const auto entry = GetTitleEntry(selection);
	if (!entry.has_value())
		return;

	if(entry->type == EntryType::Base)
		menu.Append(kContextMenuLaunch, _("&Launch title"));
	
	menu.Append(kContextMenuOpenDirectory, _("&Open directory"));
	if (entry->type != EntryType::Save)
		menu.Append(kContextMenuVerifyGameFiles, _("&Verify integrity of game files"));

	menu.AppendSeparator();

	if (entry->type != EntryType::Save && entry->format != EntryFormat::WUA)
	{
		menu.Append(kContextMenuConvertToWUA, _("Convert to compressed Wii U archive (.wua)"));

		menu.AppendSeparator();
	}
	menu.Append(kContextMenuDelete, _("&Delete"));	

	PopupMenu(&menu);
	
	// TODO: fix tooltip position
}

bool wxTitleManagerList::DeleteEntry(long index, const TitleEntry& entry)
{
	wxDTorFunc reset_text(wxQueueEvent, this, new wxSetStatusBarTextEvent(wxEmptyString, 1));
	wxQueueEvent(this, new wxSetStatusBarTextEvent("Deleting entry...", 1));
	
	wxString msg;
	const bool is_directory = fs::is_directory(entry.path);
	if(is_directory)
		msg = formatWxString(_("Are you really sure that you want to delete the following folder:\n{}"), _pathToUtf8(entry.path));
	else
		msg = formatWxString(_("Are you really sure that you want to delete the following file:\n{}"), _pathToUtf8(entry.path));
	
	const auto result = wxMessageBox(msg, _("Warning"), wxYES_NO | wxCENTRE | wxICON_EXCLAMATION, this);
	if (result == wxNO)
		return false;
				
	std::error_code ec;
	if (is_directory)
	{
		if (entry.type != EntryType::Save)
		{
			// delete content, meta, code folders first
			const auto content = entry.path / "content";
			fs::remove_all(content, ec);

			const auto meta = entry.path / "meta";
			fs::remove_all(meta, ec);

			const auto code = entry.path / "code";
			fs::remove_all(code, ec);
		}
		else
		{
			// delete meta, user folders first
			const auto meta = entry.path / "meta";
			fs::remove_all(meta, ec);
			
			const auto user = entry.path / "user";
			fs::remove_all(user, ec);
		}
		

		// check if folder is empty
		if(fs::is_empty(entry.path, ec))
			fs::remove_all(entry.path, ec);
	}	
	else // simply remove file
		fs::remove(entry.path, ec);
	
	if(ec)
	{
		const auto error_msg = formatWxString(_("Error when trying to delete the entry:\n{}"), GetSystemErrorMessage(ec));
		wxMessageBox(error_msg, _("Error"), wxOK|wxCENTRE, this);
		return false;
	}

	// thread safe request to delete entry
	const auto evt = new wxCommandEvent(wxEVT_REMOVE_ITEM);
	evt->SetInt(index);
	wxQueueEvent(this, evt);
	return true;
}

void wxTitleManagerList::OnContextMenuSelected(wxCommandEvent& event)
{
	// still doing work
	if (m_context_worker.valid() && !future_is_ready(m_context_worker))
		return;

	const auto selection = GetFirstSelected();
	if (selection == wxNOT_FOUND)
		return;

	const auto entry = GetTitleEntry(selection);
	if (!entry.has_value())
		return;
	
	switch (event.GetId())
	{
	case kContextMenuOpenDirectory:
		{
			const auto path = fs::is_directory(entry->path) ? entry->path : entry->path.parent_path();
			wxLaunchDefaultApplication(wxHelper::FromPath(path));
		}
		break;
	case kContextMenuDelete:
		m_context_worker = std::async(std::launch::async, &wxTitleManagerList::DeleteEntry, this, selection, entry.value());
		break;
	case kContextMenuLaunch:
		{
			try
			{
				m_requestLaunch(entry->path);
				Close();
			}
			catch (const std::exception& ex)
			{
				cemuLog_log(LogType::Force, "wxTitleManagerList::OnContextMenuSelected: can't launch title: {}", ex.what());
			}
		}
		break;
	case kContextMenuVerifyGameFiles:
		(new ChecksumTool(this, m_emulationController, Application::ManagedContentEntry{
			.locationUid = entry->location_uid,
			.titleId = entry->title_id,
			.path = entry->path,
			.name = entry->name.ToStdString(),
			.version = entry->version,
			.region = entry->region,
			.regionName = entry->region_name.ToStdString(),
		}, m_pathProvider))->Show();
		break;
	case kContextMenuConvertToWUA:
		
		OnConvertToCompressedFormat(entry.value().title_id, entry.value().location_uid);
		break;
	}
}

void wxTitleManagerList::OnTimer(wxTimerEvent& event)
{
	if(event.GetTimer().GetId() != m_tooltip_timer->GetId())
	{
		event.Skip();
		return;
	}

	m_tooltip_window->Show();
}

void wxTitleManagerList::OnRemoveItem(wxCommandEvent& event)
{
	RemoveItem(event.GetInt());
}

void wxTitleManagerList::OnRemoveEntry(wxCommandEvent& event)
{
	wxASSERT(event.GetClientData() != nullptr);
	RemoveItem(*(TitleEntry*)event.GetClientData());
}

wxString wxTitleManagerList::GetTitleEntryText(const TitleEntry& entry, ItemColumn column)
{
	switch (column)
	{
	case ColumnTitleId:
		return formatWxString("{:08x}-{:08x}", (uint32) (entry.title_id >> 32), (uint32) (entry.title_id & 0xFFFFFFFF));
	case ColumnName:
		return entry.name;
	case ColumnType:
		return GetTranslatedTitleEntryType(entry.type);
	case ColumnVersion:
		return formatWxString("{}", entry.version);
	case ColumnRegion:
		return wxGetTranslation(entry.region_name);
	case ColumnFormat:
	{
		if (entry.type == EntryType::Save)
			return _("Save folder");
		switch (entry.format)
		{
		case wxTitleManagerList::EntryFormat::Folder:
			return _("Folder");
		case wxTitleManagerList::EntryFormat::WUD:
			return _("WUD");
		case wxTitleManagerList::EntryFormat::NUS:
			return _("NUS");
		case wxTitleManagerList::EntryFormat::WUA:
			return _("WUA");
		case wxTitleManagerList::EntryFormat::WUHB:
			return _("WUHB");
		}
		return "";
	}
	case ColumnLocation:
	{
		const auto relative_mlc_path = _pathToUtf8(entry.path.lexically_relative(ActiveSettings::GetMlcPath()));
		if (relative_mlc_path.starts_with("usr") || relative_mlc_path.starts_with("sys"))
			return _("MLC");
		else
			return _("Game Paths");
	}
	default:
		UNREACHABLE;
	}
	
	return wxEmptyString;
}

wxString wxTitleManagerList::GetTranslatedTitleEntryType(EntryType type)
{
	switch (type)
	{
		case EntryType::Base:
			return _("base");
		case EntryType::Update:
			return _("update");
		case EntryType::Dlc:
			return _("DLC");
		case EntryType::Save:
			return _("save");
		case EntryType::System:
			return _("system");
		default:
			return std::to_string(static_cast<std::underlying_type_t<EntryType>>(type));
	}
}

namespace
{
	wxTitleManagerList::EntryType ToWxEntryType(Application::ManagedContentType type)
	{
		switch (type)
		{
		case Application::ManagedContentType::Update:
			return wxTitleManagerList::EntryType::Update;
		case Application::ManagedContentType::Dlc:
			return wxTitleManagerList::EntryType::Dlc;
		case Application::ManagedContentType::Save:
			return wxTitleManagerList::EntryType::Save;
		case Application::ManagedContentType::System:
			return wxTitleManagerList::EntryType::System;
		case Application::ManagedContentType::Base:
		default:
			return wxTitleManagerList::EntryType::Base;
		}
	}

	wxTitleManagerList::EntryFormat ToWxEntryFormat(
		Application::ManagedContentFormat format)
	{
		switch (format)
		{
		case Application::ManagedContentFormat::Wud:
			return wxTitleManagerList::EntryFormat::WUD;
		case Application::ManagedContentFormat::Nus:
			return wxTitleManagerList::EntryFormat::NUS;
		case Application::ManagedContentFormat::Wua:
			return wxTitleManagerList::EntryFormat::WUA;
		case Application::ManagedContentFormat::Wuhb:
			return wxTitleManagerList::EntryFormat::WUHB;
		case Application::ManagedContentFormat::Folder:
		default:
			return wxTitleManagerList::EntryFormat::Folder;
		}
	}
}

void wxTitleManagerList::HandleTitleCatalogEvent(
	const Application::TitleCatalogEvent& event)
{
	if (event.type == Application::TitleCatalogEventType::ScanFinished)
	{
		wxCommandEvent complete(wxEVT_TITLE_SEARCH_COMPLETE);
		complete.SetEventObject(this);
		ProcessWindowEvent(complete);
		return;
	}
	if (event.type == Application::TitleCatalogEventType::SaveScanFinished ||
		!event.managedEntry)
		return;

	const auto& managed = *event.managedEntry;
	TitleEntry entry(ToWxEntryType(managed.type), ToWxEntryFormat(managed.format),
		managed.path);
	entry.location_uid = managed.locationUid;
	entry.title_id = managed.titleId;
	entry.name = wxString::FromUTF8(managed.name);
	entry.version = managed.version;
	entry.region = managed.region;
	entry.region_name = wxString::FromUTF8(managed.regionName);

	const bool removed = event.type == Application::TitleCatalogEventType::Removed ||
		event.type == Application::TitleCatalogEventType::SaveRemoved;
	wxCommandEvent command(removed ? wxEVT_TITLE_REMOVED : wxEVT_TITLE_FOUND);
	command.SetEventObject(this);
	command.SetClientObject(new wxCustomData(entry));
	ProcessWindowEvent(command);
}

void wxTitleManagerList::OnTitleDiscovered(wxCommandEvent& event)
{
	auto* obj = dynamic_cast<wxTitleManagerList::TitleEntryData_t*>(event.GetClientObject());
	wxASSERT(obj);
	AddTitle(obj);
}

void wxTitleManagerList::OnTitleRemoved(wxCommandEvent& event)
{
	auto* obj = dynamic_cast<wxTitleManagerList::TitleEntryData_t*>(event.GetClientObject());
	wxASSERT(obj);
	for (auto& itr : m_data)
	{
		if (itr.get()->entry.location_uid == obj->get().location_uid)
		{
			RemoveItem(itr.get()->entry);
			break;
		}
	}
}

void wxTitleManagerList::AddTitle(TitleEntryData_t* obj)
{
	const auto& data = obj->GetData();
	if (GetTitleEntryByUID(data.location_uid).has_value())
		return; // already in list
	m_data.emplace_back(std::make_unique<ItemData>(true, data));
	m_sorted_data.emplace_back(*m_data[m_data.size() - 1]);
	SetItemCount(m_data.size());
}

int wxTitleManagerList::AddImage(const wxImage& image) const
{
	return -1; // m_image_list->Add(image.Scale(kListIconWidth, kListIconWidth, wxIMAGE_QUALITY_BICUBIC));
}

// return <
bool wxTitleManagerList::SortFunc(int column, const Type_t& v1, const Type_t& v2)
{
	// last sort option
	if (column == -1)
		return v1.get().entry.path.compare(v2.get().entry.path) < 0;

	// visible have always priority
	if (!v1.get().visible && v2.get().visible)
		return false;
	else if (v1.get().visible && !v2.get().visible)
		return true;

	const auto& entry1 = v1.get().entry;
	const auto& entry2 = v2.get().entry;
	
	// check column: title id -> type -> path
	if (column == ColumnTitleId)
	{
		// ensure strong ordering -> use type since only one entry should be now (should be changed if every save for every user is displayed separately?)
		if (entry1.title_id == entry2.title_id)
			return SortFunc(ColumnType, v1, v2);
		
		return entry1.title_id < entry2.title_id;
	}
	else if (column == ColumnName)
	{
		const int tmp = entry1.name.CmpNoCase(entry2.name);
		if(tmp == 0)
			return SortFunc(ColumnTitleId, v1, v2);
			
		return tmp < 0;
	}
	else if (column == ColumnType)
	{
		if(entry1.type == entry2.type)
			return SortFunc(-1, v1, v2);
		
		return std::underlying_type_t<EntryType>(entry1.type) < std::underlying_type_t<EntryType>(entry2.type);
	}
	else if (column == ColumnVersion)
	{
		if(entry1.version == entry2.version)
			return SortFunc(ColumnTitleId, v1, v2);

		return entry1.version < entry2.version;
	}
	else if (column == ColumnRegion)
	{
		if(entry1.region == entry2.region)
			return SortFunc(ColumnTitleId, v1, v2);

		return entry1.region < entry2.region;
	}
	else if (column == ColumnFormat)
	{
		if(entry1.format == entry2.format)
			return SortFunc(ColumnType, v1, v2);

		return std::underlying_type_t<EntryFormat>(entry1.format) < std::underlying_type_t<EntryFormat>(entry2.format);
	}
		
	return false;
}
void wxTitleManagerList::SortEntries(int column)
{
	bool ascending;
	if (column == -1)
	{
		column = GetSortIndicator();
		if (column == -1)
			column = ColumnTitleId;
		ascending = IsAscendingSortIndicator();
	}
	else
		ascending = GetUpdatedAscendingSortIndicator(column);

	if (column != ColumnTitleId && column != ColumnName && column != ColumnType && column != ColumnVersion && column != ColumnRegion && column != ColumnFormat)
		return;

	std::sort(m_sorted_data.begin(), m_sorted_data.end(),
			  [this, column, ascending](const Type_t& v1, const Type_t& v2) -> bool {
				  return ascending ? SortFunc(column, v1, v2) : SortFunc(column, v2, v1);
			  });

	ShowSortIndicator(column, ascending);
	RefreshPage();
}

void wxTitleManagerList::RefreshPage()
{
	long item_count = GetItemCount();

	if (item_count > 0)
		RefreshItems(GetTopItem(), std::min(item_count - 1, GetTopItem() + GetCountPerPage() + 1));
}

int wxTitleManagerList::Filter(const wxString& filter, const wxString& prefix, ItemColumn column)
{
	if (prefix.empty())
		return -1;
	
	if (!filter.StartsWith(prefix))
		return -1;

	int counter = 0;
	const auto tmp_filter = filter.substr(prefix.size() - 1).Trim(false);
	for (auto&& data : m_data)
	{
		if (GetTitleEntryText(data->entry, column).Upper().Contains(tmp_filter))
		{
			data->visible = true;
			++counter;
		}
		else
			data->visible = false;
	}
	return counter;
}

void wxTitleManagerList::Filter(const wxString& filter)
{
	if(filter.empty())
	{
		std::for_each(m_data.begin(), m_data.end(), [](ItemDataPtr& data) { data->visible = true; });
		SetItemCount(m_data.size());
		RefreshPage();
		return;
	}

	const auto filter_upper = filter.Upper().Trim(false).Trim(true);
	int counter = 0;
	
	if (const auto result = Filter(filter_upper, "TITLEID:", ColumnTitleId) != -1)
		counter = result;
	else if (const auto result = Filter(filter_upper, "NAME:", ColumnName) != -1)
		counter = result;
	else if (const auto result = Filter(filter_upper, "TYPE:", ColumnType) != -1)
		counter = result;
	else if (const auto result = Filter(filter_upper, "REGION:", ColumnRegion) != -1)
		counter = result;
	else if (const auto result = Filter(filter_upper, "VERSION:", ColumnVersion) != -1)
		counter = result;
	else if (const auto result = Filter(filter_upper, "FORMAT:", ColumnFormat) != -1)
		counter = result;
	else if(filter_upper == "ERROR")
	{
		for (auto&& data : m_data)
		{
			bool visible = true;
			data->visible = visible;
			if (visible)
				++counter;
		}
	}
	else
	{
		for (auto&& data : m_data)
		{
			bool visible = false;
			if (data->entry.name.Upper().Contains(filter_upper))
				visible = true;
			else if (GetTitleEntryText(data->entry, ColumnTitleId).Upper().Contains(filter_upper))
				visible = true;
			else if (GetTitleEntryText(data->entry, ColumnType).Upper().Contains(filter_upper))
				visible = true;

			data->visible = visible;
			if (visible)
				++counter;
		}
	}
	
	SetItemCount(counter);
	RefreshPage();
}

size_t wxTitleManagerList::GetCountByType(EntryType type) const
{
	size_t result = 0;
	for(const auto& data : m_data)
	{
		if (data->entry.type == type)
			++result;
	}
	return result;
}

void wxTitleManagerList::ClearItems()
{
	m_sorted_data.clear();
	m_data.clear();
	SetItemCount(0);
	RefreshPage();
}

void wxTitleManagerList::AutosizeColumns()
{
	wxAutosizeColumns(this, ColumnTitleId, ColumnMAX - 1);
}
