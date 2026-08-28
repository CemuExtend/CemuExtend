#include "wxgui/components/wxGameList.h"
#include "wxgui/WxFrontendContext.h"

#include "wxgui/helpers/wxCustomData.h"
#include "wxCemuConfig.h"
#include "host/contracts/HostContracts.h"
#include "util/helpers/helpers.h"
#include "wxgui/GameProfileWindow.h"

#include <numeric>

#include <wx/listctrl.h>
#include <wx/app.h>
#include <wx/wupdlock.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/imaglist.h>
#include <wx/textdlg.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/wfstream.h>
#include <wx/imagpng.h>
#include <wx/string.h>
#include <wx/utils.h>
#include <wx/clipbrd.h>

#include <boost/algorithm/string.hpp>
#include <boost/tokenizer.hpp>

#include "wxgui/helpers/wxHelpers.h"

#include "../wxHelper.h"

#if BOOST_OS_WINDOWS
// for shortcut creation
#include <windows.h>
#include <winnls.h>
#include <shobjidl.h>
#include <objbase.h>
#include <objidl.h>
#include <shlguid.h>
#include <shlobj.h>
#include <wrl/client.h>
#endif

// public events
wxDEFINE_EVENT(wxEVT_OPEN_SETTINGS, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_GAMELIST_BEGIN_UPDATE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_GAMELIST_END_UPDATE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_OPEN_GRAPHIC_PACK, wxTitleIdEvent);

// internal events
wxDEFINE_EVENT(wxEVT_GAME_ENTRY_ADDED_OR_REMOVED, wxTitleIdEvent);

constexpr uint64 kDefaultEntryData = 0x1337;

void _stripPathFilename(fs::path& path)
{
	if (path.has_extension())
		path = path.parent_path();
}

std::vector<fs::path> _getCachesPaths(const Host::IPathProvider& pathProvider, uint64 titleId)
{
	std::vector<fs::path> cachePaths{
		pathProvider.GetCachePath(fmt::format("shaderCache/driver/vk/{:016x}.bin", titleId)),
		pathProvider.GetCachePath(fmt::format("shaderCache/precompiled/{:016x}_spirv.bin", titleId)),
		pathProvider.GetCachePath(fmt::format("shaderCache/precompiled/{:016x}_gl.bin", titleId)),
		pathProvider.GetCachePath(fmt::format("shaderCache/precompiled/{:016x}_air.bin", titleId)),
		pathProvider.GetCachePath(fmt::format("shaderCache/transferable/{:016x}_shaders.bin", titleId)),
		pathProvider.GetCachePath(fmt::format("shaderCache/transferable/{:016x}_mtlshaders.bin", titleId)),
		pathProvider.GetCachePath(fmt::format("shaderCache/transferable/{:016x}_vkpipeline.bin", titleId)),
		pathProvider.GetCachePath(fmt::format("shaderCache/transferable/{:016x}_mtlpipeline.bin", titleId))};

	cachePaths.erase(std::remove_if(cachePaths.begin(), cachePaths.end(),
									[](const fs::path& cachePath) {
										std::error_code ec;
										return !fs::exists(cachePath, ec);
									}),
					 cachePaths.end());

	return cachePaths;
}

// Convert PNG to Apple icon image format
bool writeICNS(const fs::path& pngPath, const fs::path& icnsPath)
{
	// Read PNG file
	std::ifstream pngFile(pngPath, std::ios::binary);
	if (!pngFile)
		return false;

	// Get PNG size
	pngFile.seekg(0, std::ios::end);
	uint32 pngSize = static_cast<uint32>(pngFile.tellg());
	pngFile.seekg(0, std::ios::beg);

	// Calculate total file size (header + size + type + data)
	uint32 totalSize = 8 + 8 + pngSize;

	// Create output file
	std::ofstream icnsFile(icnsPath, std::ios::binary);
	if (!icnsFile)
		return false;

	// Write ICNS header
	icnsFile.put(0x69); // 'i'
	icnsFile.put(0x63); // 'c'
	icnsFile.put(0x6e); // 'n'
	icnsFile.put(0x73); // 's'

	// Write total file size (big endian)
	icnsFile.put((totalSize >> 24) & 0xFF);
	icnsFile.put((totalSize >> 16) & 0xFF);
	icnsFile.put((totalSize >> 8) & 0xFF);
	icnsFile.put(totalSize & 0xFF);

	// Write icon type (ic07 = 128x128 PNG)
	icnsFile.put(0x69); // 'i'
	icnsFile.put(0x63); // 'c'
	icnsFile.put(0x30); // '0'
	icnsFile.put(0x37); // '7'

	// Write PNG size (big endian)
	icnsFile.put((pngSize >> 24) & 0xFF);
	icnsFile.put((pngSize >> 16) & 0xFF);
	icnsFile.put((pngSize >> 8) & 0xFF);
	icnsFile.put(pngSize & 0xFF);

	// Copy PNG data
	icnsFile << pngFile.rdbuf();

	return true;
}

wxGameList::wxGameList(wxWindow* parent,
					   Application::EmulationController& emulationController,
					   std::shared_ptr<IWxUiDispatcher> uiDispatcher,
					   std::shared_ptr<Host::IPathProvider> pathProvider,
					   std::function<void(fs::path)> requestLaunch, wxWindowID id)
	: wxListView(parent, id, wxDefaultPosition, wxDefaultSize, GetStyleFlags(Style::kList)),
	  m_style(Style::kList), m_emulationController(emulationController),
	  m_uiDispatcher(std::move(uiDispatcher)),
	  m_pathProvider(std::move(pathProvider)),
	  m_requestLaunch(std::move(requestLaunch))
{
	cemu_assert(m_uiDispatcher && m_pathProvider && static_cast<bool>(m_requestLaunch));
	const auto& config = GetWxGUIConfig();

	char transparent_bitmap[kIconWidth * kIconWidth * 4] = {};
	memset(transparent_bitmap, wxSystemSettings::GetAppearance().IsDark() ? 0xFF : 0x00, sizeof(transparent_bitmap));
	wxBitmap blank(transparent_bitmap, kIconWidth, kIconWidth);

	m_image_list_data.Add(blank);
	wxListCtrl::SetImageList(&m_image_list_data, wxIMAGE_LIST_NORMAL);

	wxBitmap::Rescale(blank, {kListIconWidth, kListIconWidth});
	m_image_list_small_data.Add(blank);
	wxListCtrl::SetImageList(&m_image_list_small_data, wxIMAGE_LIST_SMALL);

	InsertColumn(ColumnHiddenName, "", wxLIST_FORMAT_LEFT, 0);
	if (config.show_icon_column)
		InsertColumn(ColumnIcon, _("Icon"), wxLIST_FORMAT_LEFT, GetColumnDefaultWidth(ColumnIcon));
	else
		InsertColumn(ColumnIcon, _("Icon"), wxLIST_FORMAT_LEFT, 0);
	InsertColumn(ColumnName, _("Game"), wxLIST_FORMAT_LEFT, config.column_width.name);
	InsertColumn(ColumnVersion, _("Version"), wxLIST_FORMAT_LEFT, config.column_width.version);
	InsertColumn(ColumnDLC, _("DLC"), wxLIST_FORMAT_LEFT, config.column_width.dlc);
	InsertColumn(ColumnGameTime, _("You've played"), wxLIST_FORMAT_LEFT, config.column_width.game_time);
	InsertColumn(ColumnGameStarted, _("Last played"), wxLIST_FORMAT_LEFT, config.column_width.game_started);
	InsertColumn(ColumnRegion, _("Region"), wxLIST_FORMAT_LEFT, config.column_width.region);
	InsertColumn(ColumnTitleID, _("Title ID"), wxLIST_FORMAT_LEFT, config.column_width.title_id);

	m_tooltip_window = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNO_BORDER);
	auto* tooltip_sizer = new wxBoxSizer(wxVERTICAL);
	tooltip_sizer->Add(new wxStaticText(m_tooltip_window, wxID_ANY, _("This game entry seems to be either an update or the base game was merged with update data\nBroken game dumps cause various problems during emulation and may even stop working at all in future Cemu versions\nPlease make sure the base game is intact and install updates only with the File->Install Update/DLC option")), 0, wxALL, 5);
	m_tooltip_window->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_INFOBK));
	m_tooltip_window->SetSizerAndFit(tooltip_sizer);
	m_tooltip_window->Hide();

	m_tooltip_timer = new wxTimer(this);

	Bind(wxEVT_CLOSE_WINDOW, &wxGameList::OnClose, this);
	Bind(wxEVT_MOTION, &wxGameList::OnMouseMove, this);
	Bind(wxEVT_LIST_KEY_DOWN, &wxGameList::OnKeyDown, this);
	Bind(wxEVT_CONTEXT_MENU, &wxGameList::OnContextMenu, this);
	Bind(wxEVT_LIST_ITEM_ACTIVATED, &wxGameList::OnItemActivated, this);
	Bind(wxEVT_GAME_ENTRY_ADDED_OR_REMOVED, &wxGameList::OnGameEntryUpdatedByTitleId, this);
	Bind(wxEVT_TIMER, &wxGameList::OnTimer, this);
	Bind(wxEVT_LEAVE_WINDOW, &wxGameList::OnLeaveWindow, this);

	Bind(wxEVT_LIST_COL_CLICK, &wxGameList::OnColumnClick, this);
	Bind(wxEVT_LIST_COL_BEGIN_DRAG, &wxGameList::OnColumnBeginResize, this);
	Bind(wxEVT_LIST_COL_END_DRAG, &wxGameList::OnColumnResize, this);
	Bind(wxEVT_LIST_COL_RIGHT_CLICK, &wxGameList::OnColumnRightClick, this);
	Bind(wxEVT_SIZE, &wxGameList::OnGameListSize, this);
	m_bulkUpdateTimer.Bind(wxEVT_TIMER, &wxGameList::OnTimerBulkAddEntriesToGameList, this);

	m_titleSubscription = m_emulationController.SubscribeTitleCatalog(
		[this, lifetime = m_lifetime](const Application::TitleCatalogEvent& event) {
			if (!lifetime->load(std::memory_order_acquire) || !wxTheApp)
				return;
			(void)m_uiDispatcher->Queue([this, lifetime, event] {
				if (lifetime->load(std::memory_order_acquire))
					HandleTitleCatalogEvent(event);
			});
		});

	// start async worker (for icon loading)
	m_async_worker_active = true;
	m_async_worker_thread = std::thread(&wxGameList::AsyncWorkerThread, this);

	ShowSortIndicator(ColumnName);
}

wxGameList::~wxGameList()
{
	m_lifetime->store(false, std::memory_order_release);
	m_titleSubscription.Reset();

	m_tooltip_timer->Stop();

	// shut down async worker
	m_async_worker_active.store(false);
	m_async_task_count.increment();
	m_async_worker_thread.join();

	// clear image cache
	m_icon_cache_mtx.lock();
	m_icon_cache.clear();
	m_icon_cache_mtx.unlock();
}

void wxGameList::LoadConfig()
{
	const auto& config = GetWxGUIConfig();
	SetStyle((Style)config.game_list_style, false);

	if (!config.game_list_column_order.empty())
	{
		wxArrayInt order;
		order.reserve(ColumnCounts);

		const auto order_string = std::string_view(config.game_list_column_order).substr(1);

		const boost::char_separator<char> sep(",");
		boost::tokenizer tokens(order_string.begin(), order_string.end(), sep);
		for (const auto& token : tokens)
		{
			order.push_back(ConvertString<int>(token, 10));
		}

#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
		if (order.GetCount() == ColumnCounts)
			SetColumnsOrder(order);
#endif
	}
}

void wxGameList::OnGameListSize(wxSizeEvent& event)
{
	event.Skip();

	// when using a sizer-based layout, do not change the size of the wxComponent in its own wxSizeEvent handler to avoid some UI issues.
	int last_col_index = 0;
	for (int i = GetColumnCount() - 1; i > 0; i--)
	{
#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
		if (GetColumnWidth(GetColumnIndexFromOrder(i)) > 0)
		{
			last_col_index = GetColumnIndexFromOrder(i);
			break;
		}
#else
		if (GetColumnWidth(i) > 0)
		{
			last_col_index = i;
			break;
		}
#endif
	}
	wxListEvent column_resize_event(wxEVT_LIST_COL_END_DRAG);
	column_resize_event.SetColumn(last_col_index);
	wxPostEvent(this, column_resize_event);
}

void wxGameList::AdjustLastColumnWidth()
{
	wxWindowUpdateLocker windowlock(this);
	int last_col_index = 0;
	int last_col_width = GetClientSize().GetWidth();
	for (int i = 1; i < GetColumnCount(); i++)
	{
#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
		if (GetColumnWidth(GetColumnIndexFromOrder(i)) > 0)
		{
			last_col_index = GetColumnIndexFromOrder(i);
			last_col_width -= GetColumnWidth(last_col_index);
		}
#else
		if (GetColumnWidth(i) > 0)
		{
			last_col_index = i;
			last_col_width -= GetColumnWidth(i);
		}
#endif
	}
	last_col_width += GetColumnWidth(last_col_index);
	if (last_col_width < GetColumnDefaultWidth(last_col_index)) // keep a minimum width
		last_col_width = GetColumnDefaultWidth(last_col_index);
	SetColumnWidth(last_col_index, last_col_width);
}

// todo: scale all columns using a ratio instead of hardcoding exact widths
int wxGameList::GetColumnDefaultWidth(int column)
{
	switch (column)
	{
	case ColumnIcon:
		return kListIconWidth + 2;
	case ColumnName:
		return DefaultColumnSize::name;
	case ColumnVersion:
		return DefaultColumnSize::version;
	case ColumnDLC:
		return DefaultColumnSize::dlc;
	case ColumnGameTime:
		return DefaultColumnSize::game_time;
	case ColumnGameStarted:
		return DefaultColumnSize::game_started;
	case ColumnRegion:
		return DefaultColumnSize::region;
	case ColumnTitleID:
		return DefaultColumnSize::title_id;
	default:
		return 80;
	}
}

void wxGameList::SaveConfig(bool flush)
{
	auto& config = GetWxGUIConfig();

	config.game_list_style = (int)m_style;
#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
	config.game_list_column_order = fmt::format("{}", GetColumnsOrder());
#endif

	if (flush)
		GetConfigHandle().Save();
}

bool wxGameList::IsVisible(long item) const
{
	wxRect itemRect;
	GetItemRect(item, itemRect);
	const wxRect clientRect = GetClientRect();
	bool visible = clientRect.Intersects(itemRect);
	return visible;
}

void wxGameList::ReloadGameEntries()
{
	wxWindowUpdateLocker windowlock(this);
	DeleteAllItems();
	m_bulkTitlesToAdd.clear();
	for (const auto& game : m_emulationController.ListGames())
		m_bulkTitlesToAdd.push_back(game.titleId);
	if (!m_bulkTitlesToAdd.empty())
		m_bulkUpdateTimer.StartOnce(1);
	m_emulationController.RefreshTitles();
}

long wxGameList::FindListItemByTitleId(uint64 title_id) const
{
	for (int i = 0; i < GetItemCount(); ++i)
	{
		const auto id = (uint64)GetItemData(i);
		if (id == title_id)
			return i;
	}

	return wxNOT_FOUND;
}

// get title name with cache
std::string wxGameList::GetNameByTitleId(uint64 titleId)
{
	auto it = m_name_cache.find(titleId);
	if (it != m_name_cache.end())
		return it->second;
	const auto game = m_emulationController.GetGame(titleId);
	if (!game)
		return "Unknown title";
	std::string name;
	if (!GetConfig().GetGameListCustomName(titleId, name))
		name = game->name;
	m_name_cache.emplace(titleId, name);
	return name;
}

void wxGameList::SetStyle(Style style, bool save)
{
	if (m_style == style)
		return;

	wxWindowUpdateLocker updatelock(this);

	m_style = style;
	SetWindowStyleFlag(GetStyleFlags(m_style));

	uint64 selected_title_id = 0;
	auto selection = GetFirstSelected();
	if (selection != wxNOT_FOUND)
	{
		selected_title_id = (uint64)GetItemData(selection);
		selection = wxNOT_FOUND;
	}

	switch (style)
	{
	case Style::kIcons:
		wxListCtrl::SetImageList(&m_image_list_data, wxIMAGE_LIST_NORMAL);
		break;
	case Style::kSmallIcons:
		wxListCtrl::SetImageList(&m_image_list_small_data, wxIMAGE_LIST_NORMAL);
		break;
	case Style::kList:
		wxListCtrl::SetImageList(&m_image_list_small_data, wxIMAGE_LIST_SMALL);
		break;
	}

	ReloadGameEntries();
	SortEntries();
	UpdateItemColors();

	if (selection != wxNOT_FOUND)
	{
		Select(selection);
		Focus(selection);
	}

	if (save)
	{
		GetWxGUIConfig().game_list_style = (int)m_style;
		GetConfigHandle().Save();
	}

	if (style == Style::kList)
		ApplyGameListColumnWidths();
}

long wxGameList::GetStyleFlags(Style style) const
{
	switch (style)
	{
	case Style::kList:
		return (wxLC_SINGLE_SEL | wxLC_VRULES | wxLC_REPORT);
	case Style::kIcons:
		return (wxLC_SINGLE_SEL | wxLC_ICON);
	case Style::kSmallIcons:
		return (wxLC_SINGLE_SEL | wxLC_ICON);
	default:
		wxASSERT(false);
		return (wxLC_SINGLE_SEL | wxLC_REPORT);
	}
}

void wxGameList::UpdateItemColors(sint32 startIndex)
{
	wxWindowUpdateLocker lock(this);

	for (int i = startIndex; i < GetItemCount(); ++i)
	{
		const uint64 titleId = GetItemData(i);
		if (GetConfig().IsGameListFavorite(titleId))
		{
			SetItemBackgroundColour(i, kFavoriteColor);
			SetItemTextColour(i, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
		}
		else if ((i % 2) != 0)
		{
			SetItemBackgroundColour(i, kPrimaryColor);
			SetItemTextColour(i, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
		}
		else
		{
			SetItemBackgroundColour(i, kAlternateColor);
			SetItemTextColour(i, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
		}
	}
}

static inline int order_to_int(const std::weak_ordering& wo)
{
	// no easy conversion seems to exist in C++20
	if (wo == std::weak_ordering::less)
		return -1;
	else if (wo == std::weak_ordering::greater)
		return 1;
	return 0;
}

std::weak_ordering wxGameList::SortComparator(uint64 titleId1, uint64 titleId2, SortData* sortData)
{
	auto titleLastPlayed = [this](uint64_t id) {
		const auto game = m_emulationController.GetGame(id);
		if (!game)
			return std::tuple{std::uint32_t{}, std::uint32_t{}, std::uint32_t{}};
		return std::tuple{game->playStats.lastPlayedYear,
						  game->playStats.lastPlayedMonth, game->playStats.lastPlayedDay};
	};

	auto titlePlayMinutes = [this](uint64_t id) {
		const auto game = m_emulationController.GetGame(id);
		return game ? game->playStats.minutesPlayed : 0u;
	};

	auto titleRegion = [this](uint64_t id) {
		const auto game = m_emulationController.GetGame(id);
		return game ? game->region : 0u;
	};

	switch (sortData->column)
	{
	default:
	case ColumnName:
	{
		const auto isFavoriteA = GetConfig().IsGameListFavorite(titleId1);
		const auto isFavoriteB = GetConfig().IsGameListFavorite(titleId2);
		const auto nameA = GetNameByTitleId(titleId1);
		const auto nameB = GetNameByTitleId(titleId2);
		return std::tie(isFavoriteB, nameA) <=> std::tie(isFavoriteA, nameB);
	}
	case ColumnGameStarted:
		return titleLastPlayed(titleId1) <=> titleLastPlayed(titleId2);
	case ColumnGameTime:
		return titlePlayMinutes(titleId1) <=> titlePlayMinutes(titleId2);
	case ColumnRegion:
		return titleRegion(titleId1) <=> titleRegion(titleId2);
	case ColumnTitleID:
		return titleId1 <=> titleId2;
	}
	// unreachable
	cemu_assert_debug(false);
	return std::weak_ordering::less;
}

int wxGameList::SortFunction(wxIntPtr item1, wxIntPtr item2, wxIntPtr sortData)
{
	const auto sort_data = (SortData*)sortData;
	return sort_data->dir * order_to_int(sort_data->thisptr->SortComparator((uint64)item1, (uint64)item2, sort_data));
}

void wxGameList::SortEntries(int column)
{
	bool ascending;
	if (column == -1)
	{
		column = GetSortIndicator();
		if (column == -1)
			column = ColumnName;
		ascending = IsAscendingSortIndicator();
	}
	else
		ascending = GetUpdatedAscendingSortIndicator(column);

	switch (column)
	{
	case ColumnName:
	case ColumnGameTime:
	case ColumnGameStarted:
	case ColumnRegion:
	case ColumnTitleID:
	{
		SortData data{this, ItemColumns{column}, ascending ? 1 : -1};
		SortItems(SortFunction, (wxIntPtr)&data);
		ShowSortIndicator(column, ascending);
		break;
	}
	}
}

void wxGameList::OnKeyDown(wxListEvent& event)
{
	event.Skip();
	if (m_style != Style::kList)
		return;

	const auto keycode = event.GetKeyCode();
	if (keycode == WXK_LEFT)
	{
		const auto item_count = GetItemCount();
		if (item_count > 0)
		{
			auto selection = (int)GetFirstSelected();
			if (selection == wxNOT_FOUND)
				selection = 0;
			else
				selection = std::max(0, selection - GetCountPerPage());

			Select(selection);
			Focus(selection);
		}
	}
	else if (keycode == WXK_RIGHT)
	{
		const auto item_count = GetItemCount();
		if (item_count > 0)
		{
			auto selection = (int)GetFirstSelected();
			if (selection == wxNOT_FOUND)
				selection = 0;

			selection = std::min(item_count - 1, selection + GetCountPerPage());

			Select(selection);
			Focus(selection);
		}
	}
}

enum GameListContextMenuEntries
{
	kContextMenuRefreshGames = wxID_HIGHEST + 1,

	kContextMenuStart,
	kWikiPage,
	kContextMenuFavorite,
	kContextMenuEditName,

	kContextMenuGameFolder,
	kContextMenuSaveFolder,
	kContextMenuUpdateFolder,
	kContextMenuDLCFolder,
	kContextMenuEditGraphicPacks,
	kContextMenuEditGameProfile,

	kContextMenuRemoveCache,

	kContextMenuStyleList,
	kContextMenuStyleIcon,
	kContextMenuStyleIconSmall,

	kContextMenuCreateShortcut,

	kContextMenuCopyTitleName,
	kContextMenuCopyTitleId,
	kContextMenuCopyTitleImage
};

void wxGameList::OnContextMenu(wxContextMenuEvent& event)
{
	auto& config = GetConfig();

	wxMenu menu;
	menu.Bind(wxEVT_COMMAND_MENU_SELECTED, &wxGameList::OnContextMenuSelected, this);

	const auto selection = GetFirstSelected();
	if (selection != wxNOT_FOUND)
	{
		const auto title_id = (uint64)GetItemData(selection);
		const auto gameInfo = m_emulationController.GetGame(title_id);
		if (gameInfo)
		{
			menu.SetClientData((void*)title_id);

			menu.Append(kContextMenuStart, _("&Start"));

			bool isFavorite = GetConfig().IsGameListFavorite(title_id);
			std::error_code ec;

			menu.AppendSeparator();
			menu.AppendCheckItem(kContextMenuFavorite, _("&Favorite"))->Check(isFavorite);
			menu.Append(kContextMenuEditName, _("&Edit name"));

			menu.AppendSeparator();
			menu.Append(kWikiPage, _("&Wiki page"));
			menu.Append(kContextMenuGameFolder, _("&Game directory"));
			menu.Append(kContextMenuSaveFolder, _("&Save directory"))->Enable(fs::is_directory(gameInfo->savePath, ec));
			menu.Append(kContextMenuUpdateFolder, _("&Update directory"))->Enable(gameInfo->updatePath.has_value());
			menu.Append(kContextMenuDLCFolder, _("&DLC directory"))->Enable(gameInfo->aocPath.has_value());

			menu.AppendSeparator();
			menu.Append(kContextMenuRemoveCache, _("&Remove shader caches"))->Enable(!_getCachesPaths(*m_pathProvider, gameInfo->titleId).empty());

			menu.AppendSeparator();
			menu.Append(kContextMenuEditGraphicPacks, _("&Edit graphic packs"));
			menu.Append(kContextMenuEditGameProfile, _("&Edit game profile"));

			menu.AppendSeparator();
			menu.Append(kContextMenuCreateShortcut, _("&Create shortcut"));
			menu.AppendSeparator();
			menu.Append(kContextMenuCopyTitleName, _("&Copy Title Name"));
			menu.Append(kContextMenuCopyTitleId, _("&Copy Title ID"));
			menu.Append(kContextMenuCopyTitleImage, _("&Copy Title Image"));
			menu.AppendSeparator();
		}
	}

	menu.Append(kContextMenuRefreshGames, _("&Refresh game list"))->Enable(!m_emulationController.IsTitleScanning());
	menu.AppendSeparator();
	menu.AppendRadioItem(kContextMenuStyleList, _("Style: &List"))->Check(m_style == Style::kList);
	menu.AppendRadioItem(kContextMenuStyleIcon, _("Style: &Icons"))->Check(m_style == Style::kIcons);
	menu.AppendRadioItem(kContextMenuStyleIconSmall, _("Style: &Small Icons"))->Check(m_style == Style::kSmallIcons);
	PopupMenu(&menu);
}

void wxGameList::OnContextMenuSelected(wxCommandEvent& event)
{
	const auto title_id = (uint64)((wxMenu*)event.GetEventObject())->GetClientData();
	if (title_id)
	{
		const auto gameInfo = m_emulationController.GetGame(title_id);
		if (gameInfo)
		{
			switch (event.GetId())
			{
			case kContextMenuStart:
			{
				m_requestLaunch(gameInfo->basePath);
				break;
			}
			case kContextMenuFavorite:
				GetConfig().SetGameListFavorite(title_id, !GetConfig().IsGameListFavorite(title_id));
				SortEntries();
				UpdateItemColors();
				SaveConfig(true);
				break;
			case kContextMenuEditName:
			{
				std::string customName = "";
				if (!GetConfig().GetGameListCustomName(title_id, customName))
					customName.clear();
				wxTextEntryDialog dialog(this, wxEmptyString, _("Enter a custom game title"), wxString::FromUTF8(customName));
				if (dialog.ShowModal() == wxID_OK)
				{
					const auto custom_name = dialog.GetValue();
					GetConfig().SetGameListCustomName(title_id, custom_name.utf8_string());
					m_name_cache.clear();
					GetConfigHandle().Save();
					// update list entry
					for (int i = 0; i < GetItemCount(); ++i)
					{
						const auto id = (uint64)GetItemData(i);
						if (id == title_id)
						{
							if (m_style == Style::kList)
								SetItem(i, ColumnName, wxString::FromUTF8(GetNameByTitleId(title_id)));
							break;
						}
					}
					SortEntries();
					UpdateItemColors();
				}
				break;
			}
			case kContextMenuGameFolder:
			{
				fs::path path(gameInfo->basePath);
				_stripPathFilename(path);
				wxLaunchDefaultApplication(wxHelper::FromPath(path));
				break;
			}
			case kWikiPage:
			{
				// https://wiki.cemu.info/wiki/GameIDs
				// WUP-P-ALZP
				if (!gameInfo->productCode.empty() && gameInfo->companyCode.size() >= 2)
				{
					const auto tokens = TokenizeView(gameInfo->productCode, '-');
					wxASSERT(!tokens.empty());
					wxLaunchDefaultBrowser(formatWxString("https://wiki.cemu.info/wiki/{}{}",
														  *tokens.rbegin(), gameInfo->companyCode.substr(gameInfo->companyCode.size() - 2)));
				}
				break;
			}

			case kContextMenuSaveFolder:
			{
				wxLaunchDefaultApplication(wxHelper::FromPath(gameInfo->savePath));
				break;
			}
			case kContextMenuUpdateFolder:
			{
				fs::path path(*gameInfo->updatePath);
				_stripPathFilename(path);
				wxLaunchDefaultApplication(wxHelper::FromPath(path));
				break;
			}
			case kContextMenuDLCFolder:
			{
				fs::path path(*gameInfo->aocPath);
				_stripPathFilename(path);
				wxLaunchDefaultApplication(wxHelper::FromPath(path));
				break;
			}
			case kContextMenuRemoveCache:
			{
				RemoveCache(_getCachesPaths(*m_pathProvider, gameInfo->titleId), gameInfo->name);
				break;
			}
			case kContextMenuEditGraphicPacks:
			{
				wxTitleIdEvent open_event(wxEVT_OPEN_GRAPHIC_PACK, title_id);
				ProcessWindowEvent(open_event);
				break;
			}
			case kContextMenuEditGameProfile:
			{
				(new GameProfileWindow(GetParent(), m_emulationController, title_id))->Show();
				break;
			}
			case kContextMenuCreateShortcut:
			{
				CreateShortcut(*gameInfo);
				break;
			}
			case kContextMenuCopyTitleName:
			{
				if (wxClipboard::Get()->Open())
				{
					wxClipboard::Get()->SetData(new wxTextDataObject(wxString::FromUTF8(gameInfo->name)));
					wxClipboard::Get()->Close();
				}
				break;
			}
			case kContextMenuCopyTitleId:
			{
				if (wxClipboard::Get()->Open())
				{
					wxClipboard::Get()->SetData(new wxTextDataObject(fmt::format("{:016x}", gameInfo->titleId)));
					wxClipboard::Get()->Close();
				}
				break;
			}
			case kContextMenuCopyTitleImage:
			{
				if (wxClipboard::Get()->Open())
				{
					int icon_large;
					int icon_small;
					if (!QueryIconForTitle(title_id, icon_large, icon_small))
						break;
					auto icon = m_image_list_data.GetIcon(icon_large);
					auto newClipboardData = wxBitmapDataObject(icon);
					wxClipboard::Get()->SetData(&newClipboardData);
					wxClipboard::Get()->Close();
				}
				break;
			}
			}
		}
	}

	switch (event.GetId())
	{
	case kContextMenuRefreshGames:
		ReloadGameEntries();
		break;
	case kContextMenuStyleList:
		SetStyle(Style::kList);
		break;
	case kContextMenuStyleIcon:
		SetStyle(Style::kIcons);
		break;
	case kContextMenuStyleIconSmall:
		SetStyle(Style::kSmallIcons);
		break;
	}
}

void wxGameList::OnColumnClick(wxListEvent& event)
{
	const int column = event.GetColumn();
	SortEntries(column);
	UpdateItemColors();
	event.Skip();
}

void wxGameList::OnColumnRightClick(wxListEvent& event)
{
	enum ItemIds
	{
		ResetWidth = wxID_HIGHEST + 1,
		ResetOrder,

		ShowIcon,
		ShowName,
		ShowVersion,
		ShowDlc,
		ShowGameTime,
		ShowLastPlayed,
		ShowRegion,
		ShowTitleId
	};
	const int column = event.GetColumn();
	wxMenu menu;
	menu.SetClientObject(new wxCustomData(column));

	menu.Append(ResetWidth, _("Reset &width"));
	menu.Append(ResetOrder, _("Reset &order"));

	menu.AppendSeparator();
	menu.AppendCheckItem(ShowIcon, _("Show &icon"))->Check(GetColumnWidth(ColumnIcon) > 0);
	menu.AppendCheckItem(ShowName, _("Show &name"))->Check(GetColumnWidth(ColumnName) > 0);
	menu.AppendCheckItem(ShowVersion, _("Show &version"))->Check(GetColumnWidth(ColumnVersion) > 0);
	menu.AppendCheckItem(ShowDlc, _("Show &dlc"))->Check(GetColumnWidth(ColumnDLC) > 0);
	menu.AppendCheckItem(ShowGameTime, _("Show &game time"))->Check(GetColumnWidth(ColumnGameTime) > 0);
	menu.AppendCheckItem(ShowLastPlayed, _("Show &last played"))->Check(GetColumnWidth(ColumnGameStarted) > 0);
	menu.AppendCheckItem(ShowRegion, _("Show &region"))->Check(GetColumnWidth(ColumnRegion) > 0);
	menu.AppendCheckItem(ShowTitleId, _("Show &title ID"))->Check(GetColumnWidth(ColumnTitleID) > 0);

	menu.Bind(wxEVT_COMMAND_MENU_SELECTED,
			  [this](wxCommandEvent& event) {
				  event.Skip();

				  const auto menu = dynamic_cast<wxMenu*>(event.GetEventObject());
				  const int column = dynamic_cast<wxCustomData<int>*>(menu->GetClientObject())->GetData();
				  auto& config = GetWxGUIConfig();

				  switch (event.GetId())
				  {
				  case ShowIcon:
					  config.show_icon_column = menu->IsChecked(ShowIcon);
					  break;
				  case ShowName:
					  config.column_width.name = menu->IsChecked(ShowName) ? DefaultColumnSize::name : 0;
					  break;
				  case ShowVersion:
					  config.column_width.version = menu->IsChecked(ShowVersion) ? DefaultColumnSize::version : 0;
					  break;
				  case ShowDlc:
					  config.column_width.dlc = menu->IsChecked(ShowDlc) ? DefaultColumnSize::dlc : 0;
					  break;
				  case ShowGameTime:
					  config.column_width.game_time = menu->IsChecked(ShowGameTime) ? DefaultColumnSize::game_time : 0;
					  break;
				  case ShowLastPlayed:
					  config.column_width.game_started = menu->IsChecked(ShowLastPlayed) ? DefaultColumnSize::game_started : 0;
					  break;
				  case ShowRegion:
					  config.column_width.region = menu->IsChecked(ShowRegion) ? DefaultColumnSize::region : 0;
					  break;
				  case ShowTitleId:
					  config.column_width.title_id = menu->IsChecked(ShowTitleId) ? DefaultColumnSize::title_id : 0;
					  break;
				  case ResetWidth:
				  {
					  switch (column)
					  {
					  case ColumnIcon:
						  break;
					  case ColumnName:
						  config.column_width.name = DefaultColumnSize::name;
						  break;
					  case ColumnVersion:
						  config.column_width.version = DefaultColumnSize::version;
						  break;
					  case ColumnDLC:
						  config.column_width.dlc = DefaultColumnSize::dlc;
						  break;
					  case ColumnGameTime:
						  config.column_width.game_time = DefaultColumnSize::game_time;
						  break;
					  case ColumnGameStarted:
						  config.column_width.game_started = DefaultColumnSize::game_started;
						  break;
					  case ColumnRegion:
						  config.column_width.region = DefaultColumnSize::region;
						  break;
					  case ColumnTitleID:
						  config.column_width.title_id = DefaultColumnSize::title_id;
					  default:
						  return;
					  }

					  break;
				  }
				  case ResetOrder:
				  {
					  config.game_list_column_order.clear();
					  wxArrayInt order(ColumnCounts);
					  std::iota(order.begin(), order.end(), 0);
#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
					  SetColumnsOrder(order);
#endif
					  // ApplyGameListColumnWidths();
					  // Refresh();
					  // return;
				  }
				  }

				  GetConfigHandle().Save();
				  ApplyGameListColumnWidths();
			  });

	PopupMenu(&menu);
	event.Skip();
}

void wxGameList::ApplyGameListColumnWidths()
{
	const auto& config = GetWxGUIConfig();
	wxWindowUpdateLocker lock(this);
	if (config.show_icon_column)
		SetColumnWidth(ColumnIcon, kListIconWidth + 2);
	else
		SetColumnWidth(ColumnIcon, 0);
	SetColumnWidth(ColumnName, config.column_width.name);
	SetColumnWidth(ColumnVersion, config.column_width.version);
	SetColumnWidth(ColumnDLC, config.column_width.dlc);
	SetColumnWidth(ColumnGameTime, config.column_width.game_time);
	SetColumnWidth(ColumnGameStarted, config.column_width.game_started);
	SetColumnWidth(ColumnRegion, config.column_width.region);
	SetColumnWidth(ColumnTitleID, config.column_width.title_id);

	AdjustLastColumnWidth();
}

void wxGameList::OnColumnBeginResize(wxListEvent& event)
{
	const int column = event.GetColumn();
	const int width = GetColumnWidth(column);
	int last_col_index = 0;
	for (int i = GetColumnCount() - 1; i > 0; i--)
	{
#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
		if (GetColumnWidth(GetColumnIndexFromOrder(i)) > 0)
		{
			last_col_index = GetColumnIndexFromOrder(i);
			break;
		}
#else
		if (GetColumnWidth(i) > 0)
		{
			last_col_index = i;
			break;
		}
#endif
	}
	if (width == 0 || column == ColumnIcon || column == last_col_index) // dont resize hidden name, icon, and last column
		event.Veto();
	else
		event.Skip();
}

void wxGameList::OnColumnResize(wxListEvent& event)
{
	event.Skip();

	if (m_style != Style::kList)
		return;

	const int column = event.GetColumn();
	const int width = GetColumnWidth(column);

	auto& config = GetWxGUIConfig();
	switch (column)
	{
	case ColumnName:
		config.column_width.name = width;
		break;
	case ColumnVersion:
		config.column_width.version = width;
		break;
	case ColumnDLC:
		config.column_width.dlc = width;
		break;
	case ColumnGameTime:
		config.column_width.game_time = width;
		break;
	case ColumnGameStarted:
		config.column_width.game_started = width;
		break;
	case ColumnRegion:
		config.column_width.region = width;
		break;
	default:
		break;
	}

	GetConfigHandle().Save();
	AdjustLastColumnWidth();
}

void wxGameList::OnClose(wxCloseEvent& event)
{
	event.Skip();
	m_exit = true;
}

int wxGameList::FindInsertPosition(uint64 titleId, bool& entryAlreadyExists)
{
	entryAlreadyExists = false;
	SortData data{this, ItemColumns(GetSortIndicator()), IsAscendingSortIndicator()};
	const auto itemCount = GetItemCount();

	if (itemCount == 0)
		return 0;
	sint32 low = 0;
	sint32 high = itemCount;
	while (low < high)
	{
		sint32 mid = low + (high - low) / 2;
		auto cmp = SortComparator(titleId, (uint64)GetItemData(mid), &data);
		if (cmp <= 0)
		{
			if (cmp == 0)
			{
				entryAlreadyExists = true;
				return mid;
			}
			high = mid;
		}
		else
		{
			low = mid + 1;
		}
	}
	return low;
}

void wxGameList::OnTimerBulkAddEntriesToGameList(wxTimerEvent& event)
{
	std::vector<uint64> titleIdsToUpdate;
	std::swap(titleIdsToUpdate, m_bulkTitlesToAdd);

	wxWindowUpdateLocker lock(this);
	bool hasAnyNewEntry = false;
	for (auto& titleId : titleIdsToUpdate)
	{
		const auto gameInfo = m_emulationController.GetGame(titleId);
		if (!gameInfo || gameInfo->systemData)
		{
			// entry no longer exists or is not a valid game
			// we dont need to remove list entries here because all delete operations should trigger a full list refresh
			continue;
		}
		const uint64 baseTitleId = gameInfo->titleId;
		bool isNewEntry = false;

		int icon = -1;		 /* 0 is the default empty icon */
		int icon_small = -1; /* 0 is the default empty icon */
		QueryIconForTitle(baseTitleId, icon, icon_small);

		bool entryAlreadyExists = false;
		auto index = FindInsertPosition(baseTitleId, entryAlreadyExists);
		if (!entryAlreadyExists)
		{
			// entry doesn't exist
			index = InsertItem(index, wxString::FromUTF8(GetNameByTitleId(baseTitleId)));
			SetItemPtrData(index, baseTitleId);
			isNewEntry = true;
			hasAnyNewEntry = true;
		}

		if (m_style == Style::kList)
		{
			SetItemColumnImage(index, ColumnIcon, icon_small);

			SetItem(index, ColumnName, wxString::FromUTF8(GetNameByTitleId(baseTitleId)));

			SetItem(index, ColumnVersion, fmt::format("{}", gameInfo->version));

			if (gameInfo->aocPath)
				SetItem(index, ColumnDLC, fmt::format("{}", gameInfo->aocVersion));
			else
				SetItem(index, ColumnDLC, wxString());

			if (isNewEntry)
			{
				const auto& playTimeStat = gameInfo->playStats;
				if (playTimeStat.available)
				{
					// time played
					uint32 minutesPlayed = playTimeStat.minutesPlayed;
					if (minutesPlayed == 0)
						SetItem(index, ColumnGameTime, wxEmptyString);
					else if (minutesPlayed < 60)
						SetItem(index, ColumnGameTime, formatWxString(wxPLURAL("{} minute", "{} minutes", minutesPlayed), minutesPlayed));
					else
					{
						uint32 hours = minutesPlayed / 60;
						uint32 minutes = minutesPlayed % 60;
						wxString hoursText = formatWxString(wxPLURAL("{} hour", "{} hours", hours), hours);
						wxString minutesText = formatWxString(wxPLURAL("{} minute", "{} minutes", minutes), minutes);
						SetItem(index, ColumnGameTime, hoursText + " " + minutesText);
					}

					// last played
					if (playTimeStat.lastPlayedYear != 0)
					{
						const wxDateTime tmp((wxDateTime::wxDateTime_t)playTimeStat.lastPlayedDay,
											 (wxDateTime::Month)playTimeStat.lastPlayedMonth,
											 (wxDateTime::wxDateTime_t)playTimeStat.lastPlayedYear, 0, 0, 0, 0);
						SetItem(index, ColumnGameStarted, tmp.FormatDate());
					}
					else
						SetItem(index, ColumnGameStarted, _("never"));
				}
				else
				{
					SetItem(index, ColumnGameTime, wxEmptyString);
					SetItem(index, ColumnGameStarted, _("never"));
				}
			}
			SetItem(index, ColumnRegion, wxGetTranslation(gameInfo->regionName));
			SetItem(index, ColumnTitleID, fmt::format("{:016x}", baseTitleId));
		}
		else if (m_style == Style::kIcons)
		{
			SetItemImage(index, icon);
		}
		else if (m_style == Style::kSmallIcons)
		{
			SetItemImage(index, icon_small);
		}
	}
	if (hasAnyNewEntry)
		UpdateItemColors();
}

void wxGameList::OnGameEntryUpdatedByTitleId(wxTitleIdEvent& event)
{
	if (m_bulkTitlesToAdd.size() < 100)
		m_bulkUpdateTimer.StartOnce(100); // if timer is started already this will delay it
	const auto titleId = event.GetTitleId();
	m_bulkTitlesToAdd.emplace_back(titleId);
}

void wxGameList::OnItemActivated(wxListEvent& event)
{
	event.Skip();

	const auto selection = event.GetIndex();
	if (selection == wxNOT_FOUND)
		return;

	const auto item_data = (uint64)GetItemData(selection);
	if (item_data == kDefaultEntryData)
	{
		const wxCommandEvent open_settings_event(wxEVT_OPEN_SETTINGS);
		wxPostEvent(this, open_settings_event);
		return;
	}

	const auto game = m_emulationController.GetGame(item_data);
	if (!game)
		return;

	m_requestLaunch(game->basePath);
}

void wxGameList::OnTimer(wxTimerEvent& event)
{
	const auto& obj = event.GetTimer().GetId();
	if (obj == m_tooltip_timer->GetId())
	{
		m_tooltip_window->Hide();

		auto flag = wxLIST_HITTEST_ONITEM;
		const auto item = this->HitTest(m_mouse_position, flag);
		if (item != wxNOT_FOUND)
		{
			// const auto title_id = (uint64_t)GetItemData(item);
			// auto entry = GetGameEntry(title_id);
			// if (entry && entry->is_update)
			//{
			//	m_tooltip_window->SetPosition(wxPoint(m_mouse_position.x + 15, m_mouse_position.y + 15));
			//	m_tooltip_window->SendSizeEvent();
			//	m_tooltip_window->Show();
			// }
		}
	}
}

void wxGameList::OnMouseMove(wxMouseEvent& event)
{
	m_tooltip_timer->Stop();
	m_tooltip_timer->StartOnce(250);
	m_mouse_position = event.GetPosition();
}

void wxGameList::OnLeaveWindow(wxMouseEvent& event)
{
	m_tooltip_timer->Stop();
	m_tooltip_window->Hide();
}

void wxGameList::HandleTitleCatalogEvent(const Application::TitleCatalogEvent& event)
{
	if (event.type == Application::TitleCatalogEventType::SaveDiscovered ||
		event.type == Application::TitleCatalogEventType::SaveRemoved ||
		event.type == Application::TitleCatalogEventType::SaveScanFinished)
		return;

	if (event.type == Application::TitleCatalogEventType::ScanFinished)
		return;

	if (event.type == Application::TitleCatalogEventType::Removed &&
		!m_emulationController.GetGame(event.titleId))
	{
		const auto item = FindListItemByTitleId(event.titleId);
		if (item != wxNOT_FOUND)
			DeleteItem(item);
		m_name_cache.erase(event.titleId);
		UpdateItemColors();
		return;
	}

	if (m_bulkTitlesToAdd.size() < 100)
		m_bulkUpdateTimer.StartOnce(100);
	m_bulkTitlesToAdd.emplace_back(event.titleId);
}

void wxGameList::RemoveCache(const std::vector<fs::path>& cachePaths, const std::string& titleName)
{
	wxMessageDialog dialog(this, formatWxString(_("Remove the shader caches for {}?"), titleName), _("Remove shader caches"), wxCENTRE | wxYES_NO | wxICON_EXCLAMATION);
	dialog.SetYesNoLabels(_("Yes"), _("No"));

	const auto dialogResult = dialog.ShowModal();
	if (dialogResult != wxID_YES)
		return;
	std::vector<std::string> errs;
	for (const fs::path& cachePath : cachePaths)
	{
		if (std::error_code ec; !fs::remove(cachePath, ec))
			errs.emplace_back(fmt::format("{} : {}", cachePath.string(), ec.message()));
	}
	if (errs.empty())
		wxMessageDialog(this, _("The shader caches were removed!"), _("Shader caches removed"), wxCENTRE | wxOK | wxICON_INFORMATION).ShowModal();
	else
		wxMessageDialog(this, formatWxString(_("Failed to remove the shader caches:\n{}"), fmt::join(errs, "\n")), _("Error"), wxCENTRE | wxOK | wxICON_ERROR).ShowModal();
}

void wxGameList::AsyncWorkerThread()
{
	SetThreadName("GameListWorker");
	while (m_async_worker_active)
	{
		m_async_task_count.decrementWithWait();
		// get next titleId to load (if any)
		m_async_worker_mutex.lock();
		bool hasJob = !m_icon_load_queue.empty();
		uint64 titleId = 0;
		if (hasJob)
		{
			titleId = m_icon_load_queue.front();
			m_icon_load_queue.erase(m_icon_load_queue.begin());
		}
		m_async_worker_mutex.unlock();
		if (!hasJob)
			continue;
		if (m_icon_loaded.find(titleId) != m_icon_loaded.end())
			continue;
		m_icon_loaded.emplace(titleId);
		auto data = m_emulationController.LoadTitleIcon(titleId);
		if (!data || data->size() <= 16)
			cemuLog_log(LogType::Force, "Failed to load icon for title {:016x}", titleId);
		else if (wxTheApp)
		{
			auto lifetime = m_lifetime;
			(void)m_uiDispatcher->Queue([this, lifetime, titleId, data = std::move(*data)]() mutable {
				if (lifetime->load(std::memory_order_acquire))
					InstallLoadedIcon(titleId, std::move(data));
			});
		}
	}
}

void wxGameList::RequestLoadIconAsync(uint64 titleId)
{
	m_async_worker_mutex.lock();
	m_icon_load_queue.push_back(titleId);
	m_async_worker_mutex.unlock();
	m_async_task_count.increment();
}

// returns icons if cached, otherwise an async request to load them is made
bool wxGameList::QueryIconForTitle(uint64 titleId, int& icon, int& iconSmall)
{
	m_icon_cache_mtx.lock();
	auto it = m_icon_cache.find(titleId);
	if (it == m_icon_cache.end())
	{
		m_icon_cache_mtx.unlock();
		RequestLoadIconAsync(titleId);
		return false;
	}
	icon = it->second.first;
	iconSmall = it->second.second;
	m_icon_cache_mtx.unlock();
	return true;
}

void wxGameList::InstallLoadedIcon(uint64 titleId, std::vector<std::uint8_t> data)
{
	wxMemoryInputStream stream(data.data(), data.size());
	const wxImage image(stream);
	if (!image.IsOk())
		return;
	const int icon = m_image_list_data.Add(
		image.Scale(kIconWidth, kIconWidth, wxIMAGE_QUALITY_BICUBIC));
	const int iconSmall = m_image_list_small_data.Add(
		image.Scale(kListIconWidth, kListIconWidth, wxIMAGE_QUALITY_BICUBIC));
	{
		std::scoped_lock lock(m_icon_cache_mtx);
		m_icon_cache.try_emplace(titleId, icon, iconSmall);
	}
	wxTitleIdEvent updated(wxEVT_GAME_ENTRY_ADDED_OR_REMOVED, titleId);
	ProcessWindowEvent(updated);
}

void wxGameList::DeleteCachedStrings()
{
	m_name_cache.clear();
}

#if BOOST_OS_LINUX || BOOST_OS_BSD
void wxGameList::CreateShortcut(const Application::GameSummary& gameInfo)
{
	const auto titleId = gameInfo.titleId;
	const auto titleName = wxString::FromUTF8(gameInfo.name);
	auto exePath = m_pathProvider->GetExecutablePath();
	const char* flatpakId = getenv("FLATPAK_ID");

	const wxString desktopEntryName = wxString::Format("%s.desktop", titleName);
	wxFileDialog entryDialog(this, _("Choose desktop entry location"), "~/.local/share/applications", desktopEntryName,
							 "Desktop file (*.desktop)|*.desktop", wxFD_SAVE | wxFD_CHANGE_DIR | wxFD_OVERWRITE_PROMPT);
	const auto result = entryDialog.ShowModal();
	if (result == wxID_CANCEL)
		return;
	const auto output_path = entryDialog.GetPath();

	std::optional<fs::path> iconPath;
	// Obtain and convert icon
	[&]() {
		int iconIdx, smallIconIdx;

		if (!QueryIconForTitle(titleId, iconIdx, smallIconIdx))
		{
			cemuLog_log(LogType::Force, "Icon hasn't loaded");
			return;
		}
		const fs::path outIconDir = m_pathProvider->GetUserDataPath("icons");

		if (!fs::exists(outIconDir) && !fs::create_directories(outIconDir))
		{
			cemuLog_log(LogType::Force, "Failed to create icon directory");
			return;
		}

		iconPath = outIconDir / fmt::format("{:016x}.png", gameInfo.titleId);
		wxFileOutputStream pngFileStream(_pathToUtf8(iconPath.value()));

		const auto icon = m_image_list_data.GetIcon(iconIdx);
		wxBitmap bitmap{icon};
		wxImage image = bitmap.ConvertToImage();
		wxPNGHandler pngHandler;
		if (!pngHandler.SaveFile(&image, pngFileStream, false))
		{
			iconPath = std::nullopt;
			cemuLog_log(LogType::Force, "Icon failed to save");
		}
	}();

	std::string desktopExecEntry = flatpakId ? fmt::format("/usr/bin/flatpak run {0} --title-id {1:016x}", flatpakId, titleId)
											 : fmt::format("{0:?} --title-id {1:016x}", _pathToUtf8(exePath), titleId);

	// 'Icon' accepts spaces in file name, does not accept quoted file paths
	// 'Exec' does not accept non-escaped spaces, and can accept quoted file paths
	auto desktopEntryString = fmt::format(
		"[Desktop Entry]\n"
		"Name={0}\n"
		"Comment=Play {0} on Cemu\n"
		"Exec={1}\n"
		"Icon={2}\n"
		"Terminal=false\n"
		"Type=Application\n"
		"Categories=Game;\n",
		titleName.utf8_string(),
		desktopExecEntry,
		_pathToUtf8(iconPath.value_or("")));

	if (flatpakId)
		desktopEntryString += fmt::format("X-Flatpak={}\n", flatpakId);

	std::ofstream outputStream(output_path.utf8_string());
	if (!outputStream.good())
	{
		auto errorMsg = formatWxString(_("Failed to save desktop entry to {}"), output_path.utf8_string());
		wxMessageBox(errorMsg, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		return;
	}
	outputStream << desktopEntryString;
}
#elif BOOST_OS_MACOS
void wxGameList::CreateShortcut(const Application::GameSummary& gameInfo)
{
	const auto titleId = gameInfo.titleId;
	const auto titleName = wxString::FromUTF8(gameInfo.name);
	auto exePath = m_pathProvider->GetExecutablePath();

	const wxString appName = wxString::Format("%s.app", titleName);
	wxFileDialog entryDialog(this, _("Choose shortcut location"), "~/Applications", appName,
							 "Application (*.app)|*.app", wxFD_SAVE | wxFD_CHANGE_DIR | wxFD_OVERWRITE_PROMPT);
	const auto result = entryDialog.ShowModal();
	if (result == wxID_CANCEL)
		return;
	const auto output_path = entryDialog.GetPath();
	// Create .app folder
	const fs::path appPath = output_path.utf8_string();
	if (!fs::create_directories(appPath))
	{
		cemuLog_log(LogType::Force, "Failed to create app directory");
		return;
	}
	const fs::path infoPath = appPath / "Contents/Info.plist";
	const fs::path scriptPath = appPath / "Contents/MacOS/run.sh";
	const fs::path icnsPath = appPath / "Contents/Resources/shortcut.icns";
	if (!(fs::create_directories(scriptPath.parent_path()) && fs::create_directories(icnsPath.parent_path())))
	{
		cemuLog_log(LogType::Force, "Failed to create app shortcut directories");
		return;
	}

	std::optional<fs::path> iconPath;
	// Obtain and convert icon
	[&]() {
		int iconIdx, smallIconIdx;

		if (!QueryIconForTitle(titleId, iconIdx, smallIconIdx))
		{
			cemuLog_log(LogType::Force, "Icon hasn't loaded");
			return;
		}
		const fs::path outIconDir = fs::temp_directory_path();

		if (!fs::exists(outIconDir) && !fs::create_directories(outIconDir))
		{
			cemuLog_log(LogType::Force, "Failed to create icon directory");
			return;
		}

		iconPath = outIconDir / fmt::format("{:016x}.png", gameInfo.titleId);
		wxFileOutputStream pngFileStream(_pathToUtf8(iconPath.value()));

		const auto icon = m_image_list_data.GetIcon(iconIdx);
		wxBitmap bitmap{icon};
		wxImage image = bitmap.ConvertToImage();
		wxPNGHandler pngHandler;
		if (!pngHandler.SaveFile(&image, pngFileStream, false))
		{
			iconPath = std::nullopt;
			cemuLog_log(LogType::Force, "Icon failed to save");
		}
	}();

	std::string runCommand = fmt::format("#!/bin/zsh\n\n{0:?} --title-id {1:016x}", _pathToUtf8(exePath), titleId);
	const std::string infoPlist = fmt::format(
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
		"<plist version=\"1.0\">\n"
		"<dict>\n"
		"	<key>CFBundleDisplayName</key>\n"
		"	<string>{0}</string>\n"
		"	<key>CFBundleExecutable</key>\n"
		"	<string>run.sh</string>\n"
		"	<key>CFBundleIconFile</key>\n"
		"	<string>shortcut.icns</string>\n"
		"	<key>CFBundleName</key>\n"
		"	<string>{0}</string>\n"
		"	<key>CFBundlePackageType</key>\n"
		"	<string>APPL</string>\n"
		"	<key>CFBundleSignature</key>\n"
		"	<string>\?\?\?\?</string>\n"
		"	<key>LSApplicationCategoryType</key>\n"
		"	<string>public.app-category.games</string>\n"
		"	<key>CFBundleShortVersionString</key>\n"
		"	<string>{1}</string>\n"
		"	<key>CFBundleVersion</key>\n"
		"	<string>{1}</string>\n"
		"</dict>\n"
		"</plist>\n",
		gameInfo.name,
		std::to_string(gameInfo.version));
	// write Info.plist to infoPath
	std::ofstream infoStream(infoPath);
	std::ofstream scriptStream(scriptPath);
	if (!infoStream.good() || !scriptStream.good())
	{
		auto errorMsg = formatWxString(_("Failed to save app shortcut to {}"), output_path.utf8_string());
		wxMessageBox(errorMsg, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		return;
	}
	infoStream << infoPlist;
	scriptStream << runCommand;
	scriptStream.close();

	// Set execute permissions for script
	fs::permissions(
		scriptPath,
		fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
		fs::perm_options::add);

	// Return if iconPath is empty
	if (!iconPath)
	{
		cemuLog_log(LogType::Force, "Icon not found");
		return;
	}

	// Convert icon to icns, only works for 128x128 PNG
	// Alternatively, can run the command "sips -s format icns {iconPath} --out '{icnsPath}'"
	// using std::system() to handle images of any size
	if (!writeICNS(*iconPath, icnsPath))
	{
		cemuLog_log(LogType::Force, "Failed to convert icon to icns");
		return;
	}

	// Remove temp file
	fs::remove(*iconPath);
}
#elif BOOST_OS_WINDOWS
void wxGameList::CreateShortcut(const Application::GameSummary& gameInfo)
{
	const auto titleId = gameInfo.titleId;
	const auto titleName = wxString::FromUTF8(gameInfo.name);
	auto exePath = m_pathProvider->GetExecutablePath();

	// Get '%APPDATA%\Microsoft\Windows\Start Menu\Programs' path
	PWSTR userShortcutFolder;
	SHGetKnownFolderPath(FOLDERID_Programs, 0, NULL, &userShortcutFolder);
	const wxString shortcutName = wxString::Format("%s.lnk", titleName);
	wxFileDialog shortcutDialog(this, _("Choose shortcut location"), userShortcutFolder, shortcutName,
								"Shortcut (*.lnk)|*.lnk", wxFD_SAVE | wxFD_CHANGE_DIR | wxFD_OVERWRITE_PROMPT);

	CoTaskMemFree(userShortcutFolder);

	const auto result = shortcutDialog.ShowModal();
	if (result == wxID_CANCEL)
		return;
	const auto outputPath = shortcutDialog.GetPath();

	std::optional<fs::path> icon_path = std::nullopt;
	{
		int iconIdx;
		int smallIconIdx;
		if (!QueryIconForTitle(titleId, iconIdx, smallIconIdx))
		{
			cemuLog_log(LogType::Force, "Icon hasn't loaded");
			return;
		}
		const auto icon = m_image_list_data.GetIcon(iconIdx);
		const auto folder = m_pathProvider->GetUserDataPath("icons");
		if (!fs::exists(folder) && !fs::create_directories(folder))
		{
			cemuLog_log(LogType::Force, "Failed to create icon directory");
			return;
		}
		wxBitmap bitmap{icon};

		icon_path = folder / fmt::format("{:016x}.ico", titleId);
		auto stream = wxFileOutputStream(icon_path->wstring());
		auto image = bitmap.ConvertToImage();
		wxICOHandler icohandler{};
		if (!icohandler.SaveFile(&image, stream, false))
		{
			icon_path = std::nullopt;
			cemuLog_log(LogType::Force, "Icon failed to save");
		}
	}

	Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
	HRESULT hres = CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink));
	if (SUCCEEDED(hres))
	{
		const auto description = wxString::Format("Play %s on Cemu", titleName);
		const auto args = wxString::Format("-t %016llx", titleId);

		shellLink->SetPath(exePath.wstring().c_str());
		shellLink->SetDescription(description.wc_str());
		shellLink->SetArguments(args.wc_str());
		shellLink->SetWorkingDirectory(exePath.parent_path().wstring().c_str());

		if (icon_path)
			shellLink->SetIconLocation(icon_path->wstring().c_str(), 0);
		else
			shellLink->SetIconLocation(exePath.wstring().c_str(), 0);

		Microsoft::WRL::ComPtr<IPersistFile> shellLinkFile;
		// save the shortcut
		hres = shellLink.As(&shellLinkFile);
		if (SUCCEEDED(hres))
		{
			hres = shellLinkFile->Save(outputPath.wc_str(), TRUE);
		}
	}
	if (FAILED(hres))
	{
		auto errorMsg = formatWxString(_("Failed to save shortcut to {}"), outputPath);
		wxMessageBox(errorMsg, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
	}
}
#endif
