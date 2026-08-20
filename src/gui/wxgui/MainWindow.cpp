#include "MainWindow.h"

// subwindows
#include "TitleManager.h"
#include "GeneralSettings2.h"
#include "CemodPermissionDialog.h"
#include "GameUpdateWindow.h"
#include "CemuUpdateWindow.h"
#include "GraphicPacksWindow2.h"
#include "AudioDebuggerWindow.h"
#include "input/InputSettings2.h"
#include "input/HotkeySettings.h"
#include "debugger/DebuggerWindowAdapter.h"
#include "debugger/MemorySearcherAdapter.h"
#include "EmulatedUSBDevices/EmulatedUSBDeviceAdapter.h"
#include "windows/PPCThreadsViewer/DebugPPCThreadsWindow.h"
#include "windows/TextureRelationViewer/TextureRelationWindow.h"

//wxgui + misc.
#include "wxgui.h"
#include "wxCemuConfig.h"
#include "wxgui/WxFrontendContext.h"
#include "wxHelper.h"
#include "helpers/wxHelpers.h"
#include "PadViewFrame.h"
#include "WxWindowState.h"

#if defined(__WXGTK__)
#include <gtk/gtk.h>
#endif

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif

// settings
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "config/ActiveSettings.h"

// External functionality headers
#include "Cemu/DiscordPresence/DiscordPresence.h"
#include "util/ScreenSaver/ScreenSaver.h"
#include "util/helpers/SystemException.h"
#if BOOST_OS_LINUX && defined(ENABLE_FERAL_GAMEMODE)
#include <gamemode_client.h>
#endif
#if ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND
#include "helpers/wxWayland.h"
#endif

// Renderer Canvasses
#ifdef ENABLE_OPENGL
#include "canvas/OpenGLCanvas.h"
#endif
#ifdef ENABLE_VULKAN
#include "canvas/VulkanCanvas.h"
#endif
#include "canvas/RendererWindowAdapter.h"
#ifdef ENABLE_METAL
#include "canvas/MetalCanvas.h"
#endif

//Cafe libs

#include <wx/app.h>
#include <wx/thread.h>

#include <vector>

#if BOOST_OS_WINDOWS
#include <windows.h>
#endif

enum
{
	// ui elements
	MAINFRAME_GAMELIST_ID = 20000, //wxID_HIGHEST + 1,
	// file
	MAINFRAME_MENU_ID_FILE_LOAD = 20100,
	MAINFRAME_MENU_ID_FILE_INSTALL_UPDATE,
	MAINFRAME_MENU_ID_FILE_OPEN_CEMU_FOLDER,
	MAINFRAME_MENU_ID_FILE_OPEN_MLC_FOLDER,
	MAINFRAME_MENU_ID_FILE_OPEN_SHADERCACHE_FOLDER,
	MAINFRAME_MENU_ID_FILE_CLEAR_SPOTPASS_CACHE,
	MAINFRAME_MENU_ID_FILE_EXIT,
	MAINFRAME_MENU_ID_FILE_END_EMULATION,
	MAINFRAME_MENU_ID_FILE_RECENT_0,
	MAINFRAME_MENU_ID_FILE_RECENT_LAST = MAINFRAME_MENU_ID_FILE_RECENT_0 + 15,
	// options
	MAINFRAME_MENU_ID_OPTIONS_FULLSCREEN = 20200,
	MAINFRAME_MENU_ID_OPTIONS_SECOND_WINDOW_PADVIEW,
	MAINFRAME_MENU_ID_OPTIONS_GRAPHIC,
	MAINFRAME_MENU_ID_OPTIONS_GRAPHIC_PACKS2,
	MAINFRAME_MENU_ID_OPTIONS_GENERAL,
	MAINFRAME_MENU_ID_OPTIONS_GENERAL2,
	MAINFRAME_MENU_ID_OPTIONS_AUDIO,
	MAINFRAME_MENU_ID_OPTIONS_INPUT,
	MAINFRAME_MENU_ID_OPTIONS_HOTKEY,
	MAINFRAME_MENU_ID_OPTIONS_MAC_SETTINGS,
	// options -> account
	MAINFRAME_MENU_ID_OPTIONS_ACCOUNT_1 = 20350,
	MAINFRAME_MENU_ID_OPTIONS_ACCOUNT_12 = 20350 + 11,

	// options -> system language
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_JAPANESE = 20500,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_ENGLISH,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_FRENCH,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_GERMAN,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_ITALIAN,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_SPANISH,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_CHINESE,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_KOREAN,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_DUTCH,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_PORTUGUESE,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_RUSSIAN,
	MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_TAIWANESE,
	// tools
	MAINFRAME_MENU_ID_TOOLS_MEMORY_SEARCHER = 20600,
	MAINFRAME_MENU_ID_TOOLS_TITLE_MANAGER,
	MAINFRAME_MENU_ID_TOOLS_DOWNLOAD_MANAGER,
	MAINFRAME_MENU_ID_TOOLS_EMULATED_USB_DEVICES,
	// cpu
	// cpu->timer speed
	MAINFRAME_MENU_ID_TIMER_SPEED_1X = 20700,
	MAINFRAME_MENU_ID_TIMER_SPEED_2X = 20701,
	MAINFRAME_MENU_ID_TIMER_SPEED_4X = 20702,
	MAINFRAME_MENU_ID_TIMER_SPEED_8X = 20703,
	MAINFRAME_MENU_ID_TIMER_SPEED_05X = 20704,
	MAINFRAME_MENU_ID_TIMER_SPEED_025X = 20705,
	MAINFRAME_MENU_ID_TIMER_SPEED_0125X = 20706,

	// nfc->Touch NFC file
	MAINFRAME_MENU_ID_NFC_TOUCH_NFC_FILE = 21000,
	MAINFRAME_MENU_ID_NFC_RECENT_0,
	MAINFRAME_MENU_ID_NFC_RECENT_LAST = MAINFRAME_MENU_ID_NFC_RECENT_0 + 15,
	// debug
	MAINFRAME_MENU_ID_DEBUG_RENDER_UPSIDE_DOWN = 21100,
	MAINFRAME_MENU_ID_DEBUG_VIEW_LOGGING_WINDOW,
	MAINFRAME_MENU_ID_DEBUG_TOGGLE_GDB_STUB,
	MAINFRAME_MENU_ID_DEBUG_VIEW_PPC_THREADS,
	MAINFRAME_MENU_ID_DEBUG_VIEW_PPC_DEBUGGER,
	MAINFRAME_MENU_ID_DEBUG_VIEW_AUDIO_DEBUGGER,
	MAINFRAME_MENU_ID_DEBUG_VIEW_TEXTURE_RELATIONS,
	MAINFRAME_MENU_ID_DEBUG_AUDIO_AUX_ONLY,
	MAINFRAME_MENU_ID_DEBUG_VK_ACCURATE_BARRIERS,
	MAINFRAME_MENU_ID_DEBUG_GPU_CAPTURE,

	// debug->logging
	MAINFRAME_MENU_ID_DEBUG_LOGGING_MESSAGE = 21499,
	MAINFRAME_MENU_ID_DEBUG_LOGGING0 = 21500,
	MAINFRAME_MENU_ID_DEBUG_ADVANCED_PPC_INFO = 21599,
	// debug->dump
	MAINFRAME_MENU_ID_DEBUG_DUMP_TEXTURES = 21600,
	MAINFRAME_MENU_ID_DEBUG_DUMP_SHADERS,
	MAINFRAME_MENU_ID_DEBUG_DUMP_RECOMPILER_FUNCTIONS,
	MAINFRAME_MENU_ID_DEBUG_DUMP_RAM,
	MAINFRAME_MENU_ID_DEBUG_DUMP_FST,
	MAINFRAME_MENU_ID_DEBUG_DUMP_CURL_REQUESTS,
	// help
	MAINFRAME_MENU_ID_HELP_ABOUT = 21700,
	MAINFRAME_MENU_ID_HELP_UPDATE,
	// custom
	MAINFRAME_ID_TIMER1 = 21800,
};

wxDEFINE_EVENT(wxEVT_SET_WINDOW_TITLE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_REQUEST_GAMELIST_REFRESH, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_LAUNCH_GAME, wxLaunchGameEvent);
wxDEFINE_EVENT(wxEVT_REQUEST_GAME_EXIT, wxCommandEvent);

wxBEGIN_EVENT_TABLE(MainWindow, wxFrame)
EVT_TIMER(MAINFRAME_ID_TIMER1, MainWindow::OnTimer)
EVT_CLOSE(MainWindow::OnClose)
EVT_SIZE(MainWindow::OnSizeEvent)
EVT_DPI_CHANGED(MainWindow::OnDPIChangedEvent)
EVT_MOVE(MainWindow::OnMove)
// file menu
EVT_MENU(MAINFRAME_MENU_ID_FILE_LOAD, MainWindow::OnFileMenu)
EVT_MENU(MAINFRAME_MENU_ID_FILE_INSTALL_UPDATE, MainWindow::OnInstallUpdate)
EVT_MENU(MAINFRAME_MENU_ID_FILE_OPEN_CEMU_FOLDER, MainWindow::OnOpenFolder)
EVT_MENU(MAINFRAME_MENU_ID_FILE_OPEN_MLC_FOLDER, MainWindow::OnOpenFolder)
EVT_MENU(MAINFRAME_MENU_ID_FILE_OPEN_SHADERCACHE_FOLDER, MainWindow::OnOpenFolder)
EVT_MENU(MAINFRAME_MENU_ID_FILE_CLEAR_SPOTPASS_CACHE, MainWindow::OnClearSpotPassCache)
EVT_MENU(MAINFRAME_MENU_ID_FILE_EXIT, MainWindow::OnFileExit)
EVT_MENU(MAINFRAME_MENU_ID_FILE_END_EMULATION, MainWindow::OnFileMenu)
EVT_MENU_RANGE(MAINFRAME_MENU_ID_FILE_RECENT_0 + 0, MAINFRAME_MENU_ID_FILE_RECENT_LAST, MainWindow::OnFileMenu)
// options -> region menu
EVT_MENU_RANGE(MAINFRAME_MENU_ID_OPTIONS_ACCOUNT_1, MAINFRAME_MENU_ID_OPTIONS_ACCOUNT_12, MainWindow::OnAccountSelect)
EVT_MENU_RANGE(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_JAPANESE, MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_TAIWANESE, MainWindow::OnConsoleLanguage)
// options menu
EVT_MENU(MAINFRAME_MENU_ID_OPTIONS_FULLSCREEN, MainWindow::OnOptionsInput)
EVT_MENU(MAINFRAME_MENU_ID_OPTIONS_SECOND_WINDOW_PADVIEW, MainWindow::OnOptionsInput)
EVT_MENU(MAINFRAME_MENU_ID_OPTIONS_GRAPHIC, MainWindow::OnOptionsInput)
EVT_MENU(MAINFRAME_MENU_ID_OPTIONS_GRAPHIC_PACKS2, MainWindow::OnOptionsInput)
EVT_MENU(MAINFRAME_MENU_ID_OPTIONS_GENERAL, MainWindow::OnOptionsInput)
EVT_MENU(MAINFRAME_MENU_ID_OPTIONS_GENERAL2, MainWindow::OnOptionsInput)
EVT_MENU(MAINFRAME_MENU_ID_OPTIONS_AUDIO, MainWindow::OnOptionsInput)
EVT_MENU(MAINFRAME_MENU_ID_OPTIONS_INPUT, MainWindow::OnOptionsInput)
EVT_MENU(MAINFRAME_MENU_ID_OPTIONS_HOTKEY, MainWindow::OnOptionsInput)
EVT_MENU(MAINFRAME_MENU_ID_OPTIONS_MAC_SETTINGS, MainWindow::OnOptionsInput)
// tools menu
EVT_MENU(MAINFRAME_MENU_ID_TOOLS_MEMORY_SEARCHER, MainWindow::OnToolsInput)
EVT_MENU(MAINFRAME_MENU_ID_TOOLS_TITLE_MANAGER, MainWindow::OnToolsInput)
EVT_MENU(MAINFRAME_MENU_ID_TOOLS_DOWNLOAD_MANAGER, MainWindow::OnToolsInput)
EVT_MENU(MAINFRAME_MENU_ID_TOOLS_EMULATED_USB_DEVICES, MainWindow::OnToolsInput)
// cpu menu
EVT_MENU(MAINFRAME_MENU_ID_TIMER_SPEED_8X, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_TIMER_SPEED_4X, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_TIMER_SPEED_2X, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_TIMER_SPEED_1X, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_TIMER_SPEED_05X, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_TIMER_SPEED_025X, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_TIMER_SPEED_0125X, MainWindow::OnDebugSetting)
// nfc menu
EVT_MENU(MAINFRAME_MENU_ID_NFC_TOUCH_NFC_FILE, MainWindow::OnNFCMenu)
EVT_MENU_RANGE(MAINFRAME_MENU_ID_NFC_RECENT_0 + 0, MAINFRAME_MENU_ID_NFC_RECENT_LAST, MainWindow::OnNFCMenu)
// debug -> logging menu
EVT_MENU_RANGE(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + 0, MAINFRAME_MENU_ID_DEBUG_LOGGING0 + 98, MainWindow::OnDebugLoggingToggleFlagGeneric)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_ADVANCED_PPC_INFO, MainWindow::OnPPCInfoToggle)
// debug -> dump menu
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_DUMP_TEXTURES, MainWindow::OnDebugDumpGeneric)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_DUMP_SHADERS, MainWindow::OnDebugDumpGeneric)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_DUMP_RECOMPILER_FUNCTIONS, MainWindow::OnDebugDumpGeneric)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_DUMP_CURL_REQUESTS, MainWindow::OnDebugSetting)
// debug -> Other options
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_RENDER_UPSIDE_DOWN, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_AUDIO_AUX_ONLY, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_VK_ACCURATE_BARRIERS, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_GPU_CAPTURE, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_DUMP_RAM, MainWindow::OnDebugSetting)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_DUMP_FST, MainWindow::OnDebugSetting)
// debug -> View ...
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_VIEW_LOGGING_WINDOW, MainWindow::OnLoggingWindow)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_TOGGLE_GDB_STUB, MainWindow::OnGDBStubToggle)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_VIEW_PPC_THREADS, MainWindow::OnDebugViewPPCThreads)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_VIEW_PPC_DEBUGGER, MainWindow::OnDebugViewPPCDebugger)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_VIEW_AUDIO_DEBUGGER, MainWindow::OnDebugViewAudioDebugger)
EVT_MENU(MAINFRAME_MENU_ID_DEBUG_VIEW_TEXTURE_RELATIONS, MainWindow::OnDebugViewTextureRelations)
// help menu
EVT_MENU(MAINFRAME_MENU_ID_HELP_ABOUT, MainWindow::OnHelpAbout)
EVT_MENU(MAINFRAME_MENU_ID_HELP_UPDATE, MainWindow::OnHelpUpdate)
// misc
EVT_COMMAND(wxID_ANY, wxEVT_REQUEST_GAMELIST_REFRESH, MainWindow::OnRequestGameListRefresh)

EVT_COMMAND(wxID_ANY, wxEVT_GAMELIST_BEGIN_UPDATE, MainWindow::OnGameListBeginUpdate)
EVT_COMMAND(wxID_ANY, wxEVT_GAMELIST_END_UPDATE, MainWindow::OnGameListEndUpdate)
EVT_COMMAND(wxID_ANY, wxEVT_ACCOUNTLIST_REFRESH, MainWindow::OnAccountListRefresh)
EVT_COMMAND(wxID_ANY, wxEVT_SET_WINDOW_TITLE, MainWindow::OnSetWindowTitle)

EVT_COMMAND(wxID_ANY, wxEVT_REQUEST_GAME_EXIT, MainWindow::OnRequestGameExit)

wxEND_EVENT_TABLE()

class wxGameDropTarget : public wxFileDropTarget
{
public:
	wxGameDropTarget(MainWindow* window) : m_window(window) {}
	bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString& filenames) override
	{
		if(!m_window->IsGameLaunched() && filenames.GetCount() == 1)
			return m_window->FileLoad(_utf8ToPath(filenames[0].utf8_string()), wxLaunchGameEvent::INITIATED_BY::DRAG_AND_DROP);

		return false;
	}

private:
	MainWindow* m_window;
};

class wxAmiiboDropTarget : public wxFileDropTarget
{
public:
	wxAmiiboDropTarget(MainWindow* window) : m_window(window) {}
	bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString& filenames) override
	{
		if (!m_window->IsGameLaunched() || filenames.GetCount() != 1)
			return false;
		return m_window->TouchNfcFile(filenames[0].utf8_string(), true);
	}

private:
	MainWindow* m_window;
};

namespace
{
	std::uint32_t CemuExtendMouseButtons(const wxMouseEvent& event)
	{
		using Frontend::CemuExtendMouseButton;
		return (event.LeftIsDown() ? static_cast<std::uint32_t>(CemuExtendMouseButton::Left) : 0U) |
			(event.RightIsDown() ? static_cast<std::uint32_t>(CemuExtendMouseButton::Right) : 0U) |
			(event.MiddleIsDown() ? static_cast<std::uint32_t>(CemuExtendMouseButton::Middle) : 0U) |
			(event.Aux1IsDown() ? static_cast<std::uint32_t>(CemuExtendMouseButton::X1) : 0U) |
			(event.Aux2IsDown() ? static_cast<std::uint32_t>(CemuExtendMouseButton::X2) : 0U);
	}

	wxStockCursor CemuExtendCursor(std::uint8_t cursor)
	{
		using Frontend::CemuExtendPointerCursor;
		switch (static_cast<CemuExtendPointerCursor>(cursor))
		{
		case CemuExtendPointerCursor::TextInput: return wxCURSOR_IBEAM;
		case CemuExtendPointerCursor::ResizeAll: return wxCURSOR_SIZING;
		case CemuExtendPointerCursor::ResizeNS: return wxCURSOR_SIZENS;
		case CemuExtendPointerCursor::ResizeEW: return wxCURSOR_SIZEWE;
		case CemuExtendPointerCursor::ResizeNESW: return wxCURSOR_SIZENESW;
		case CemuExtendPointerCursor::ResizeNWSE: return wxCURSOR_SIZENWSE;
		case CemuExtendPointerCursor::Hand: return wxCURSOR_HAND;
		case CemuExtendPointerCursor::NotAllowed: return wxCURSOR_NO_ENTRY;
		default: return wxCURSOR_ARROW;
		}
	}
}

MainWindow::MainWindow(Application::EmulationController& emulationController,
	std::shared_ptr<WxFrontendContext> frontendContext)
	: wxFrame(nullptr, wxID_ANY, GetInitialWindowTitle(), wxDefaultPosition, wxSize(1280, 720), wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxCLIP_CHILDREN | wxRESIZE_BORDER),
	  m_emulationController(emulationController),
	  m_frontendContext(std::move(frontendContext)),
	  m_windowMetrics(m_frontendContext->windowMetrics),
	  m_pathProvider(m_frontendContext->pathProvider),
	  m_nativeSurfaces(m_frontendContext->nativeSurfaces),
	  m_nativeSurfacePublisher(m_frontendContext->nativeSurfacePublisher),
	  m_keyboardState(m_frontendContext->keyboardState),
	  m_inputHostEvents(m_frontendContext->inputHostEvents),
	  m_windowState(m_frontendContext->windowState),
	  m_mainWindowRegistry(m_frontendContext->mainWindowRegistry),
	  m_uiDispatcher(m_frontendContext->uiDispatcher),
	  m_showErrorDialog(m_frontendContext->showErrorDialog),
	  m_updateWindowTitles(m_frontendContext->updateWindowTitles),
	  m_applicationEventLifetime(std::make_shared<std::atomic_bool>(true))
{
	cemu_assert(m_frontendContext && m_windowMetrics && m_pathProvider && m_nativeSurfaces && m_nativeSurfacePublisher &&
		m_keyboardState && m_inputHostEvents && m_windowState && m_mainWindowRegistry &&
		m_uiDispatcher &&
		m_showErrorDialog && m_updateWindowTitles);
#ifdef __WXMAC__
	// Not necessary to set wxApp::s_macExitMenuItemId as automatically handled
	wxApp::s_macAboutMenuItemId = MAINFRAME_MENU_ID_HELP_ABOUT;
	wxApp::s_macPreferencesMenuItemId = MAINFRAME_MENU_ID_OPTIONS_MAC_SETTINGS;
#endif
	m_mainWindowRegistry->Register(*this, m_applicationEventLifetime);
	m_nativeWindowHandle = initHandleContextFromWxWidgetsWindow(this);
	m_nativeWindowPublication =
		m_nativeSurfacePublisher->PublishMainWindow(m_nativeWindowHandle);
	const std::weak_ptr<std::atomic_bool> eventLifetime = m_applicationEventLifetime;
	const auto eventDispatcher = m_uiDispatcher;
	m_applicationEventSubscription = m_emulationController.Events().Subscribe(
		[this, eventLifetime, eventDispatcher](const Application::Event& event) {
			(void)eventDispatcher->Queue([this, eventLifetime, event] {
				const auto lifetime = eventLifetime.lock();
				if (!lifetime || !lifetime->load(std::memory_order_acquire))
					return;
				HandleApplicationEvent(event);
			});
		});
	RecreateMenu();
	SetClientSize(1280, 720);
	SetIcon(wxICON(M_WND_ICON128));

#if BOOST_OS_MACOS
	this->EnableFullScreenView(true);
#endif

#if BOOST_OS_WINDOWS
	HICON hWindowIcon = (HICON)LoadImageA(NULL, "M_WND_ICON16", IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
	SendMessage(this->GetHWND(), WM_SETICON, ICON_SMALL, (LPARAM)hWindowIcon);
#endif

	auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    auto load_file = LaunchSettings::GetLoadFile();
    auto load_title_id = LaunchSettings::GetLoadTitleID();
    bool quick_launch = false;

	if (load_file)
	{
		RequestLaunchGame(load_file.value(), wxLaunchGameEvent::INITIATED_BY::COMMAND_LINE);
		quick_launch = true;
	}
	else if (load_title_id)
	{
		if (const auto title = m_emulationController.ResolveBaseTitle(*load_title_id))
		{
			RequestLaunchGame(title->path, wxLaunchGameEvent::INITIATED_BY::COMMAND_LINE);
			quick_launch = true;
		}
		else
		{
			wxString errorMsg = fmt::format("Title ID {:016x} not found", load_title_id.value());
			wxMessageBox(errorMsg, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);

		}
	}
	SetSizer(main_sizer);
	if (!quick_launch)
	{
		CreateGameListAndStatusBar();
	}
	else
	{
		// launching game via -g or -t option. Don't set up or load game list
		m_game_list = nullptr;
		m_info_bar = nullptr;
	}
	SetSizer(main_sizer);

	m_last_mouse_move_time = std::chrono::steady_clock::now();

	m_timer = new wxTimer(this, MAINFRAME_ID_TIMER1);
	m_timer->Start(500);

	LoadSettings();

	#ifdef ENABLE_DISCORD_RPC
	if (GetWxGUIConfig().use_discord_presence)
			m_discord = std::make_unique<DiscordPresence>();
	#endif

	Bind(wxEVT_OPEN_GRAPHIC_PACK, &MainWindow::OnGraphicWindowOpen, this);
	Bind(wxEVT_LAUNCH_GAME, &MainWindow::OnLaunchFromFile, this);

	if (LaunchSettings::GDBStubEnabled())
	{
		WxDebuggerAdapters::EnsureGdbStub(GetConfig().gdb_port);
	}
}

MainWindow::~MainWindow()
{
	ClosePpcThreadsViewer();
	m_applicationEventLifetime->store(false, std::memory_order_release);
	m_mainWindowRegistry->Unregister(*this);
	m_applicationEventSubscription.Reset();
	m_uiDispatcher->BeginShutdown();
	UpdateCemuExtendPointerConfinement(false);
	// Publish invalid handles and wait for outstanding snapshots before wx destroys
	// their native objects.  NativeHandleLease is a publication/destruction barrier.
	(void)m_nativeSurfacePublisher->PublishCanvas(true, {});
	(void)m_nativeSurfacePublisher->PublishCanvas(false, {});
	(void)m_nativeSurfacePublisher->PublishPadWindow({});
	m_nativeSurfacePublisher->ClearMainWindow(m_nativeWindowPublication);
	if (m_padView)
	{
		m_padView->PrepareForDestroy();
		m_padView->Destroy();
		m_padView = nullptr;
	}

	m_timer->Stop();

}

void MainWindow::HandleApplicationEvent(const Application::Event& event)
{
	using Application::EventType;
	switch (event.type)
	{
	case EventType::LoadingStarted:
		m_updateWindowTitles(false, true, 0.0, std::nullopt);
		break;
	case EventType::GameLoaded:
		OnGameLoaded();
		UpdateSettingsAfterGameLaunch();
		break;
	case EventType::GameExited:
		RestoreSettingsAfterGameExited();
		break;
	case EventType::PpcProcessExited:
		HandlePpcProcessExit();
		break;
	case EventType::PerformanceUpdated:
		m_updateWindowTitles(false, false, event.framesPerSecond,
			m_emulationController.CurrentWindowTitlePresentation());
		break;
	case EventType::Diagnostic:
	{
		std::optional<WxFrontendErrorCategory> category;
		if (event.diagnosticCode == Application::DiagnosticCode::KeyFileCreateFailed ||
			event.diagnosticCode == Application::DiagnosticCode::KeyFileInvalidLine)
			category = WxFrontendErrorCategory::KeysFileCreation;
		else if (event.diagnosticCode == Application::DiagnosticCode::GraphicPackInvalid)
			category = WxFrontendErrorCategory::GraphicPacks;
		m_showErrorDialog(event.diagnostic, _tr("Error"), category);
		break;
	}
	case EventType::GameListRefreshRequested:
		RequestGameListRefresh();
		break;
	case EventType::TextInputWakeRequested:
		RefreshCemuExtendTextInput();
		break;
	}
}

void MainWindow::CreateGameListAndStatusBar()
{
    if(m_main_panel)
        return; // already displayed
    m_main_panel = new wxPanel(this);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    // game list
    m_game_list = new wxGameList(m_main_panel, m_emulationController,
		m_uiDispatcher,
		[this](fs::path path) {
			QueueEvent(new wxLaunchGameEvent(
				std::move(path), wxLaunchGameEvent::INITIATED_BY::GAME_LIST));
		}, MAINFRAME_GAMELIST_ID);
    m_game_list->Bind(wxEVT_OPEN_SETTINGS, [this](auto&) {OpenSettings(); });
    m_game_list->SetDropTarget(new wxGameDropTarget(this));
    sizer->Add(m_game_list, 1, wxEXPAND);

    // info, warning bar
    m_info_bar = new wxInfoBar(m_main_panel);
    m_info_bar->SetShowHideEffects(wxSHOW_EFFECT_BLEND, wxSHOW_EFFECT_BLEND);
    m_info_bar->SetEffectDuration(500);
    sizer->Add(m_info_bar, 0, wxALL | wxEXPAND, 5);

    m_main_panel->SetSizer(sizer);

    auto* main_sizer = this->GetSizer();
    main_sizer->Add(m_main_panel, 1, wxEXPAND, 0, nullptr);
}

void MainWindow::DestroyGameListAndStatusBar()
{
    if(!m_main_panel)
        return;
    m_main_panel->Destroy();
    m_main_panel = nullptr;
    m_game_list = nullptr;
    m_info_bar = nullptr;
}

wxString MainWindow::GetInitialWindowTitle()
{
	return BUILD_VERSION_WITH_NAME_STRING;
}

void MainWindow::OnClose(wxCloseEvent& event)
{
	if (!IsMaximized() && !m_windowState->is_fullscreen.load())
		m_restored_size = GetSize();

	SaveSettings();

	if (m_active_cemod_permission_dialog && m_active_cemod_permission_dialog->IsModal())
		m_active_cemod_permission_dialog->EndModal(wxID_CANCEL);
	ClosePpcThreadsViewer();
	CloseMemorySearcher();
	m_uiDispatcher->BeginShutdown();
	const auto shutdownResult = m_emulationController.ShutdownApplication();
	if (!shutdownResult.stopped)
	{
		cemuLog_log(LogType::Force, "Failed to shut down emulation while closing: {}",
			shutdownResult.diagnostic);
		m_uiDispatcher->ResumeAfterFailedShutdown();
		event.Veto();
		return;
	}
	if (m_debugger_window)
	{
		WxDebuggerAdapters::DestroyDebuggerWindow(*m_debugger_window);
		m_debugger_window = nullptr;
	}

	if(m_game_list)
		m_game_list->OnClose(event);

	m_timer->Stop();
	event.Skip();
	DestroyCanvas();
}

bool MainWindow::InstallUpdate(const fs::path& metaFilePath)
{
	try
	{
		GameUpdateWindow frame(*this, m_emulationController, metaFilePath);
		const int updateResult = frame.ShowModal();

		if (updateResult == wxID_OK)
		{
			wxMessageBox(_("Title installed!"), _("Success"));
			return true;
		}
		else
		{
			if (frame.GetExceptionMessage().empty())
				wxMessageBox(_("Title installation has been canceled!"));
			else
			{
				throw std::runtime_error(frame.GetExceptionMessage());
			}
		}
	}
	catch(const AbortException&)
	{
		// ignored
	}
	catch (const std::exception& ex)
	{
		wxMessageBox(ex.what(), _("Update error"));
	}
	return false;
}

bool MainWindow::FileLoad(const fs::path launchPath, wxLaunchGameEvent::INITIATED_BY initiatedBy)
{
	auto beforeStart = [this](const Application::LaunchResult& prepared) {
			wxWindowUpdateLocker lock(this);
			DestroyGameListAndStatusBar();
			m_game_launched = true;
			m_loadMenuItem->Enable(false);
			m_installUpdateMenuItem->Enable(false);
			m_memorySearcherMenuItem->Enable(true);
			m_launched_game_name = prepared.titleName;
	#ifdef ENABLE_DISCORD_RPC
			if (m_discord)
				m_discord->UpdatePresence(DiscordPresence::Playing, m_launched_game_name);
	#endif
			if (GetConfig().disable_screensaver)
				ScreenSaver::SetInhibit(true);
			if (FullscreenEnabled())
				SetFullScreen(true);

			// GameMode support
#if BOOST_OS_LINUX && defined(ENABLE_FERAL_GAMEMODE)
			if(GetWxGUIConfig().feral_gamemode)
			{
				if(gamemode_request_start() < 0)
					cemuLog_log(LogType::Force, "Could not start GameMode");
				else
					cemuLog_log(LogType::Force, "GameMode has been started.");
			}
#endif
			CreateCanvas();
		};
	Application::LaunchResult launchResult;
	for (;;)
	{
		launchResult = m_emulationController.Launch(
			{launchPath}, beforeStart, [this] { RollbackFailedLaunchUi(); });
		if (launchResult.error != Application::LaunchError::PermissionRequired)
			break;
		if (!ConfirmCemodPermissions(launchResult.titleId,
			std::move(launchResult.titleName), std::move(launchResult.permissionRequests)))
			return false;
	}

	if (!launchResult)
	{
		using Application::LaunchError;
		if (launchResult.error == LaunchError::PermissionDenied)
			return false;
		wxString message;
		if (launchResult.error == LaunchError::BaseTitleMissing)
			message = _("Unable to launch game because the base files were not found.");
		else if (launchResult.error == LaunchError::UnableToMount)
			message = _("Unable to mount title.\nMake sure the configured game paths are still valid and refresh the game list.\n\nFile which failed to load:\n");
		else if (launchResult.error == LaunchError::CemodRuntimeBusy)
			message = _("The CemuExtend mod runtime is still shutting down the previous title. Please try again.");
		else if (initiatedBy == wxLaunchGameEvent::INITIATED_BY::GAME_LIST &&
			(launchResult.error == LaunchError::InvalidTitle ||
			 launchResult.error == LaunchError::MissingDiscKey ||
			 launchResult.error == LaunchError::MissingTitleTicket))
			message = _("Unable to launch title.\nMake sure the configured game paths are still valid and refresh the game list.\n\nPath which failed to load:\n");
		else
		{
			message = _("Unable to launch game\nPath:\n");
			if (launchResult.error == LaunchError::MissingDiscKey)
				message.append(_("\n\nCould not decrypt title. Make sure that keys.txt contains the correct disc key for this title."));
			else if (launchResult.error == LaunchError::MissingTitleTicket)
				message.append(_("\n\nCould not decrypt title because title.tik is missing."));
		}
		message.append(_pathToUtf8(launchPath));
		wxMessageBox(message, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		return false;
	}
	GetWxGUIConfig().AddRecentlyLaunchedFile(_pathToUtf8(launchResult.recentPath));

	RecreateMenu();
	UpdateChildWindowTitleRunningState();

	return true;
}

void MainWindow::RollbackFailedLaunchUi()
{
	DestroyCanvas();
	m_game_launched = false;
	m_launched_game_name.clear();
	if (m_windowState->is_fullscreen.load())
	{
		m_windowState->is_fullscreen = false;
		ShowFullScreen(false);
		SetMenuVisible(true);
	}
	#ifdef ENABLE_DISCORD_RPC
	if (m_discord)
		m_discord->UpdatePresence(DiscordPresence::Idling, "");
	#endif
	if (GetConfig().disable_screensaver)
		ScreenSaver::SetInhibit(false);
	RecreateMenu();
	CreateGameListAndStatusBar();
	DoLayout();
	UpdateChildWindowTitleRunningState();
}

void MainWindow::OnLaunchFromFile(wxLaunchGameEvent& event)
{
	if (event.GetPath().empty())
		return;
	FileLoad(event.GetPath(), event.GetInitiatedBy());
}

void MainWindow::OnFileMenu(wxCommandEvent& event)
{
	const auto menuId = event.GetId();
	if (menuId == MAINFRAME_MENU_ID_FILE_LOAD)
	{
		const auto wildcard = formatWxString(
			"{}|*.wud;*.wux;*.wua;*.wuhb;*.iso;*.rpx;*.elf;title.tmd"
			"|{}|*.wud;*.wux;*.iso"
			"|{}|title.tmd"
			"|{}|*.wua"
			"|{}|*.wuhb"
			"|{}|*.rpx;*.elf"
			"|{}|*",
			_("All Wii U files (*.wud, *.wux, *.wua, *.wuhb, *.iso, *.rpx, *.elf)"),
			_("Wii U image (*.wud, *.wux, *.iso, *.wad)"),
			_("Wii U NUS content"),
			_("Wii U archive (*.wua)"),
			_("Wii U homebrew bundle (*.wuhb)"),
			_("Wii U executable (*.rpx, *.elf)"),
			_("All files (*.*)")
		);

		wxFileDialog openFileDialog(this, _("Open file to launch"), wxEmptyString, wxEmptyString, wildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST);

		if (openFileDialog.ShowModal() == wxID_CANCEL || openFileDialog.GetPath().IsEmpty())
			return;

		const wxString wxStrFilePath = openFileDialog.GetPath();
		FileLoad(_utf8ToPath(wxStrFilePath.utf8_string()), wxLaunchGameEvent::INITIATED_BY::MENU);
	}
	else if (menuId >= MAINFRAME_MENU_ID_FILE_RECENT_0 && menuId <= MAINFRAME_MENU_ID_FILE_RECENT_LAST)
	{
		const auto& config = GetWxGUIConfig();
		const size_t index = menuId - MAINFRAME_MENU_ID_FILE_RECENT_0;
		if (index < config.recent_launch_files.size())
		{
			fs::path path = _utf8ToPath(config.recent_launch_files[index]);
			if (!path.empty())
				FileLoad(path, wxLaunchGameEvent::INITIATED_BY::MENU);
		}
	}
	else if (menuId == MAINFRAME_MENU_ID_FILE_END_EMULATION)
	{
		EndEmulation();
	}
}

void MainWindow::OnOpenFolder(wxCommandEvent& event)
{
	const auto id = event.GetId();
	if(id == MAINFRAME_MENU_ID_FILE_OPEN_CEMU_FOLDER)
		wxLaunchDefaultApplication(wxHelper::FromPath(m_pathProvider->GetUserDataPath("")));
	else if(id == MAINFRAME_MENU_ID_FILE_OPEN_MLC_FOLDER)
		wxLaunchDefaultApplication(wxHelper::FromPath(m_pathProvider->GetMlcPath()));
	else if (id == MAINFRAME_MENU_ID_FILE_OPEN_SHADERCACHE_FOLDER)
		wxLaunchDefaultApplication(wxHelper::FromPath(m_pathProvider->GetCachePath("shaderCache")));
}

void MainWindow::OnClearSpotPassCache(wxCommandEvent& event)
{
	fs::path bossPath = m_pathProvider->GetMlcPath() / "usr/boss";
	fs::remove_all(bossPath);
	// recreate usr/boss/
	fs::create_directory(bossPath);
	wxMessageBox(_("SpotPass cache cleared"));
}

void MainWindow::OnInstallUpdate(wxCommandEvent& event)
{
	while (true)
	{
		wxDirDialog openDirDialog(nullptr, _("Select folder of title to install"), "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST, wxDefaultPosition, wxDefaultSize, _("Select the folder that stores your update, DLC or base game files"));
		int modalChoice = openDirDialog.ShowModal();
		if (modalChoice == wxID_CANCEL || openDirDialog.GetPath().IsEmpty())
			break;
		if (modalChoice == wxID_OK)
		{
			#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
			fs::path dirPath((const char*)(openDirDialog.GetPath().fn_str()));
			#else
			fs::path dirPath(openDirDialog.GetPath().fn_str());
			#endif

			if ((dirPath.filename() == "code" || dirPath.filename() == "content" || dirPath.filename() == "meta") && dirPath.has_parent_path())
			{
				if (!fs::exists(dirPath.parent_path() / "code") || !fs::exists(dirPath.parent_path() / "content") || !fs::exists(dirPath.parent_path() / "meta"))
				{
					wxMessageBox(formatWxString(_("The (parent) folder of the title you selected is missing at least one of the required subfolders (\"code\", \"content\" and \"meta\")\nMake sure that the files are complete."), dirPath.filename().string()));
					continue;
				}
				else
					dirPath = dirPath.parent_path();
			}

			if (!fs::exists(dirPath))
				wxMessageBox(_("The folder you have selected cannot be found on your system."));
			else if (!fs::exists(dirPath / "meta" / "meta.xml"))
				wxMessageBox(_("Unable to find the /meta/meta.xml file inside the selected folder."));
			else
			{
				InstallUpdate(dirPath);
				return;
			}
		}
	}
}

void MainWindow::OnNFCMenu(wxCommandEvent& event)
{
	if (event.GetId() == MAINFRAME_MENU_ID_NFC_TOUCH_NFC_FILE)
	{
		wxFileDialog
			openFileDialog(this, _("Open file to load"), "", "",
				"All NFC files (bin, dat, nfc)|*.bin;*.dat;*.nfc|All files (*.*)|*", wxFD_OPEN | wxFD_FILE_MUST_EXIST); // TRANSLATE
		if (openFileDialog.ShowModal() == wxID_CANCEL || openFileDialog.GetPath().IsEmpty())
			return;
		TouchNfcFile(openFileDialog.GetPath().utf8_string(), false);
	}
	else if (event.GetId() >= MAINFRAME_MENU_ID_NFC_RECENT_0 && event.GetId() <= MAINFRAME_MENU_ID_NFC_RECENT_LAST)
	{
		const size_t index = event.GetId() - MAINFRAME_MENU_ID_NFC_RECENT_0;
		auto& config = GetWxGUIConfig();
		if (index < config.recent_nfc_files.size())
		{
			const auto path = config.recent_nfc_files[index];
			if (!path.empty())
				TouchNfcFile(path, false);
		}
	}
}

bool MainWindow::TouchNfcFile(std::string path, bool dropTarget)
{
	const auto result = m_emulationController.TouchNfcTagFromFile(_utf8ToPath(path));
	if (result == Application::NfcTouchResult::Success)
	{
		GetWxGUIConfig().AddRecentNfcFile(path);
		UpdateNFCMenu();
		return true;
	}

	wxString message;
	if (result == Application::NfcTouchResult::NoAccess)
		message = _("Cannot open file");
	else if (result == Application::NfcTouchResult::InvalidFileFormat)
		message = _("Not a valid NFC file");
	if (!message.empty())
	{
		if (dropTarget)
			wxMessageBox(message, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		else
			wxMessageBox(message);
	}
	return false;
}

void MainWindow::OnFileExit(wxCommandEvent& event)
{
	Close();
}

void MainWindow::TogglePadView()
{
	const auto& config = GetWxGUIConfig();
	if (config.pad_open)
	{
		if (m_padView)
			return;

		m_padView = new PadViewFrame(
			this, m_emulationController, m_frontendContext);

		m_padView->Bind(wxEVT_CLOSE_WINDOW, &MainWindow::OnPadClose, this);

		m_padView->Show(true);

#if ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND
		if (wxWlIsWaylandWindow(m_padView))
			wxWlSetAppId(m_padView, "info.cemu.Cemu");
#endif

		m_padView->Initialize();
		if (m_game_launched)
			m_padView->InitializeRenderCanvas();
	}
	else if (m_padView)
	{
		m_padView->PrepareForDestroy();
		m_padView->Destroy();
		m_padView = nullptr;
	}
}

#if BOOST_OS_WINDOWS

#ifndef DBT_DEVNODES_CHANGED
#define DBT_DEVNODES_CHANGED (0x0007)
#endif
WXLRESULT MainWindow::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
	if (nMsg == WM_DEVICECHANGE)
	{
		if (wParam == DBT_DEVNODES_CHANGED)
		{
			m_inputHostEvents->NotifyDeviceChanged();
		}
	}
	else if (nMsg == WM_INPUT && m_cemuextend_bridge.RawMouseRequested() &&
		m_cemuextend_bridge.PointerMode() == static_cast<std::uint8_t>(
			Frontend::CemuExtendPointerMode::CapturedRelative))
	{
		UINT size{};
		if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr,
			&size, sizeof(RAWINPUTHEADER)) == 0 && size >= sizeof(RAWINPUTHEADER))
		{
			std::vector<std::byte> storage(size);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
				storage.data(), &size, sizeof(RAWINPUTHEADER)) == size)
			{
				const auto& raw = *reinterpret_cast<const RAWINPUT*>(storage.data());
				if (raw.header.dwType == RIM_TYPEMOUSE)
				{
					using Frontend::CemuExtendMouseButton;
					std::uint32_t changed{};
					const auto updateButton = [this, &changed](USHORT flags,
						USHORT downFlag, USHORT upFlag, CemuExtendMouseButton button)
					{
						const auto mask = static_cast<std::uint32_t>(button);
						if ((flags & downFlag) != 0 &&
							(m_cemuextend_bridge.MouseButtons() & mask) == 0)
						{
							m_cemuextend_bridge.UpdateButtons(
								Frontend::CemuExtendMouseTransition::Down, mask);
							changed |= mask;
						}
						if ((flags & upFlag) != 0 &&
							(m_cemuextend_bridge.MouseButtons() & mask) != 0)
						{
							m_cemuextend_bridge.UpdateButtons(
								Frontend::CemuExtendMouseTransition::Up, mask);
							changed |= mask;
						}
					};
					const auto flags = raw.data.mouse.usButtonFlags;
					updateButton(flags, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP,
						CemuExtendMouseButton::Left);
					updateButton(flags, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP,
						CemuExtendMouseButton::Right);
					updateButton(flags, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP,
						CemuExtendMouseButton::Middle);
					updateButton(flags, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP,
						CemuExtendMouseButton::X1);
					updateButton(flags, RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP,
						CemuExtendMouseButton::X2);

					if ((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
					{
						m_cemuextend_bridge.MarkRawMouseSeen();
						EmitCemuExtendRawMouseEvent(raw.data.mouse.lLastX,
							raw.data.mouse.lLastY, changed);
					}
				}
			}
		}
	}

	return wxFrame::MSWWindowProc(nMsg, wParam, lParam);
}
#endif

void MainWindow::OpenSettings()
{
	auto& config = GetWxGUIConfig();
	const auto language = config.language;

	GeneralSettings2 frame(this, m_game_launched, m_emulationController,
		m_nativeSurfaces);
	frame.ShowModal();
	const bool paths_modified = frame.ShouldReloadGamelist();
	const bool mlc_modified = frame.MLCModified();

	if (paths_modified)
		m_game_list->ReloadGameEntries();
	else
		SaveSettings();

	#ifdef ENABLE_DISCORD_RPC
	if (config.use_discord_presence)
	{
		if (!m_discord)
		{
			m_discord = std::make_unique<DiscordPresence>();
			if (!m_launched_game_name.empty())
				m_discord->UpdatePresence(DiscordPresence::Playing, m_launched_game_name);
		}
	}
	else
		m_discord.reset();
	#endif

	if(config.check_update && !m_game_launched)
		m_update_available = CemuUpdateWindow::IsUpdateAvailableAsync();

	if (mlc_modified)
		RecreateMenu();

	if (!config.fullscreen_menubar && IsFullScreen())
		SetMenuVisible(false);

	if (language != config.language)
		wxMessageBox(_("Cemu must be restarted to apply the selected UI language."), _("Information"), wxOK | wxCENTRE, this); // TODO: change language to newly selected one
}

void MainWindow::OnOptionsInput(wxCommandEvent& event)
{
	switch (event.GetId())
	{
	case MAINFRAME_MENU_ID_OPTIONS_FULLSCREEN:
	{
		const bool state = m_fullscreenMenuItem->IsChecked();
		SetFullScreen(state);
		break;
	}
	case MAINFRAME_MENU_ID_OPTIONS_SECOND_WINDOW_PADVIEW:
	{
		GetWxGUIConfig().pad_open = !GetWxGUIConfig().pad_open;
		g_wxConfig.Save();

		TogglePadView();
		break;
	}
	case MAINFRAME_MENU_ID_OPTIONS_GRAPHIC_PACKS2:
	{
		if (m_graphic_pack_window)
			return;

		uint64 titleId = 0;
		if (const auto runningTitleId = m_emulationController.RunningTitleId())
			titleId = *runningTitleId;

		m_graphic_pack_window = new GraphicPacksWindow2(
			this, titleId, m_emulationController, m_uiDispatcher, m_pathProvider);
		m_graphic_pack_window->Bind(wxEVT_CLOSE_WINDOW, &MainWindow::OnGraphicWindowClose, this);
		m_graphic_pack_window->Show(true);

		break;
	}

	case MAINFRAME_MENU_ID_OPTIONS_MAC_SETTINGS:
	case MAINFRAME_MENU_ID_OPTIONS_GENERAL2:
	{
		OpenSettings();
		break;
	}
	case MAINFRAME_MENU_ID_OPTIONS_INPUT:
	{
		auto* frame = new InputSettings2(this,
			[keyboardState = m_keyboardState] {
				return keyboardState->IsKeyDown(Host::Key::Escape);
			});
		frame->ShowModal();
		frame->Destroy();
		break;
	}

	case MAINFRAME_MENU_ID_OPTIONS_HOTKEY:
	{
		auto* frame = new HotkeySettings(this);
		frame->Show();
		break;
	}
	}
}

void MainWindow::OnAccountSelect(wxCommandEvent& event)
{
	const int index = event.GetId() - MAINFRAME_MENU_ID_OPTIONS_ACCOUNT_1;
	const auto accounts = m_emulationController.ListAccounts();
	wxASSERT(index >= 0 && index < (int)accounts.size());
	auto& config = GetConfig();
	config.account.m_persistent_id = accounts[index].persistentId;
	// config.account.online_enabled.value = false; // reset online for safety
	GetConfigHandle().Save();
}

void MainWindow::OnConsoleLanguage(wxCommandEvent& event)
{
	switch (event.GetId())
	{
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_JAPANESE:
		GetConfig().console_language = CafeConsoleLanguage::JA;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_ENGLISH:
		GetConfig().console_language = CafeConsoleLanguage::EN;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_FRENCH:
		GetConfig().console_language = CafeConsoleLanguage::FR;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_GERMAN:
		GetConfig().console_language = CafeConsoleLanguage::DE;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_ITALIAN:
		GetConfig().console_language = CafeConsoleLanguage::IT;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_SPANISH:
		GetConfig().console_language = CafeConsoleLanguage::ES;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_CHINESE:
		GetConfig().console_language = CafeConsoleLanguage::ZH;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_KOREAN:
		GetConfig().console_language = CafeConsoleLanguage::KO;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_DUTCH:
		GetConfig().console_language = CafeConsoleLanguage::NL;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_PORTUGUESE:
		GetConfig().console_language = CafeConsoleLanguage::PT;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_RUSSIAN:
		GetConfig().console_language = CafeConsoleLanguage::RU;
		break;
	case MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_TAIWANESE:
		GetConfig().console_language = CafeConsoleLanguage::TW;
		break;
	default:
		cemu_assert_debug(false);
	}
	if (m_game_list)
	{
		m_game_list->DeleteCachedStrings();
		m_game_list->ReloadGameEntries();
	}
	GetConfigHandle().Save();
}

//void MainWindow::OnCPUMode(wxCommandEvent& event)
//{
//	if (event.GetId() == MAINFRAME_MENU_ID_CPU_MODE_SINGLECORE_INTERPRETER)
//		GetConfig().cpu_mode = CPUMode::SinglecoreInterpreter;
//	else if (event.GetId() == MAINFRAME_MENU_ID_CPU_MODE_SINGLECORE_RECOMPILER)
//		GetConfig().cpu_mode = CPUMode::SinglecoreRecompiler;
//	else if (event.GetId() == MAINFRAME_MENU_ID_CPU_MODE_DUALCORE_RECOMPILER)
//		GetConfig().cpu_mode = CPUMode::DualcoreRecompiler;
//	else if (event.GetId() == MAINFRAME_MENU_ID_CPU_MODE_TRIPLECORE_RECOMPILER)
//		GetConfig().cpu_mode = CPUMode::TriplecoreRecompiler;
//	else
//		cemu_assert_debug(false);
//
//	GetConfigHandle().Save();
//}

void MainWindow::OnDebugSetting(wxCommandEvent& event)
{
	if (event.GetId() == MAINFRAME_MENU_ID_DEBUG_RENDER_UPSIDE_DOWN)
		GetConfig().render_upside_down = event.IsChecked();
	else if (event.GetId() == MAINFRAME_MENU_ID_DEBUG_VK_ACCURATE_BARRIERS)
	{
		GetConfig().vk_accurate_barriers = event.IsChecked();
		if(!GetConfig().vk_accurate_barriers)
			wxMessageBox(_("Warning: Disabling the accurate barriers option will lead to flickering graphics but may improve performance. It is highly recommended to leave it turned on."), _("Accurate barriers are off"), wxOK);
	}
#ifdef ENABLE_METAL
	else if (event.GetId() == MAINFRAME_MENU_ID_DEBUG_GPU_CAPTURE)
	{
		(void)WxRendererAdapters::RequestFrameCapture();
	}
#endif
	else if (event.GetId() == MAINFRAME_MENU_ID_DEBUG_AUDIO_AUX_ONLY)
		ActiveSettings::EnableAudioOnlyAux(event.IsChecked());
	else if (event.GetId() == MAINFRAME_MENU_ID_DEBUG_DUMP_RAM)
		memory_createDump();
	else if (event.GetId() == MAINFRAME_MENU_ID_DEBUG_DUMP_FST)
	{
		/*	int msgBoxAnswer = wxMessageBox(_("All files from the currently running game will be dumped to /dump/<gamefolder>. This process can take a few minutes."),
				_("Dump WUD"), wxOK | wxCANCEL | wxICON_WARNING);
			if (msgBoxAnswer == wxOK)
			{
				volumeFST_dump(bootGame_getMountedWUD());
				wxMessageBox(_("Dump complete"));
			}*/
	}
	else if (event.GetId() == MAINFRAME_MENU_ID_DEBUG_DUMP_CURL_REQUESTS)
	{
		// toggle debug -> dump -> curl requests
		const bool state = event.IsChecked();
		ActiveSettings::EnableDumpLibcurlRequests(state);
		if (state)
		{
			try
			{
				const fs::path path(m_pathProvider->GetUserDataPath(""));
				fs::create_directories(path / "dump" / "curl");
			}
			catch (const std::exception& ex)
			{
				SystemException sys(ex);
				cemuLog_log(LogType::Force, "error when creating dump curl folder: {}", sys.what());
				ActiveSettings::EnableDumpLibcurlRequests(false);
			}
		}
	}
	else if (event.GetId() == MAINFRAME_MENU_ID_TIMER_SPEED_8X)
		ActiveSettings::SetTimerShiftFactor(0);
	else if (event.GetId() == MAINFRAME_MENU_ID_TIMER_SPEED_4X)
		ActiveSettings::SetTimerShiftFactor(1);
	else if (event.GetId() == MAINFRAME_MENU_ID_TIMER_SPEED_2X)
		ActiveSettings::SetTimerShiftFactor(2);
	else if (event.GetId() == MAINFRAME_MENU_ID_TIMER_SPEED_1X)
		ActiveSettings::SetTimerShiftFactor(3);
	else if (event.GetId() == MAINFRAME_MENU_ID_TIMER_SPEED_05X)
		ActiveSettings::SetTimerShiftFactor(4);
	else if (event.GetId() == MAINFRAME_MENU_ID_TIMER_SPEED_025X)
		ActiveSettings::SetTimerShiftFactor(5);
	else if (event.GetId() == MAINFRAME_MENU_ID_TIMER_SPEED_0125X)
		ActiveSettings::SetTimerShiftFactor(6);
	else
		cemu_assert_debug(false);

	GetConfigHandle().Save();
}

void MainWindow::OnDebugLoggingToggleFlagGeneric(wxCommandEvent& event)
{
	sint32 loggingIdBase = MAINFRAME_MENU_ID_DEBUG_LOGGING0;

	sint32 id = event.GetId();
	if (id >= loggingIdBase && id < (MAINFRAME_MENU_ID_DEBUG_LOGGING0 + 64))
	{
		bool isEnable = event.IsChecked();
		LogType loggingType = static_cast<LogType>(id - loggingIdBase);
		if (isEnable)
			GetConfig().log_flag = GetConfig().log_flag.GetValue() | cemuLog_getFlag(loggingType);
		else
			GetConfig().log_flag = GetConfig().log_flag.GetValue() & ~cemuLog_getFlag(loggingType);
		cemuLog_setActiveLoggingFlags(GetConfig().log_flag.GetValue());
		GetConfigHandle().Save();
	}
}

void MainWindow::OnPPCInfoToggle(wxCommandEvent& event)
{
	GetConfig().advanced_ppc_logging = !GetConfig().advanced_ppc_logging.GetValue();
	GetConfigHandle().Save();
}

void MainWindow::OnDebugDumpGeneric(wxCommandEvent& event)
{
	std::string dumpSubpath;
	std::function<void(bool)> setDumpState;
	switch(event.GetId())
	{
	case MAINFRAME_MENU_ID_DEBUG_DUMP_TEXTURES:
		dumpSubpath = "dump/textures";
		setDumpState = ActiveSettings::EnableDumpTextures;
		break;
	case MAINFRAME_MENU_ID_DEBUG_DUMP_SHADERS:
		dumpSubpath = "dump/shaders";
		setDumpState = ActiveSettings::EnableDumpShaders;
		break;
	case MAINFRAME_MENU_ID_DEBUG_DUMP_RECOMPILER_FUNCTIONS:
		dumpSubpath = "dump/recompiler";
		setDumpState = ActiveSettings::EnableDumpRecompilerFunctions;
		break;
	default:
		UNREACHABLE;
	}
	const bool value = event.IsChecked();
	setDumpState(value);
	if (value)
	{
		try
		{
			fs::create_directories(m_pathProvider->GetUserDataPath(dumpSubpath));
		}
		catch (const std::exception & ex)
		{
			SystemException sys(ex);
			cemuLog_log(LogType::Force, "can't create folder {} in user data folder: {}", dumpSubpath, ex.what());
			setDumpState(false);
		}
	}
}

void MainWindow::OnLoggingWindow(wxCommandEvent& event)
{
	if(m_logging_window)
		return;

	m_logging_window = new LoggingWindow(this);
	m_logging_window->Bind(wxEVT_CLOSE_WINDOW,
		[this](wxCloseEvent& event) {
		m_logging_window = nullptr;
		event.Skip();
	});
	m_logging_window->Show(true);
}

void MainWindow::OnGDBStubToggle(wxCommandEvent& event)
{
	const auto& config = GetConfig();
	WxDebuggerAdapters::ToggleGdbStub(config.gdb_port);
}

void MainWindow::OnDebugViewPPCThreads(wxCommandEvent& event)
{
	if (m_ppc_threads_window)
	{
		m_ppc_threads_window->Raise();
		return;
	}

	m_ppc_threads_window = new DebugPPCThreadsWindow(*this);
	auto* createdWindow = m_ppc_threads_window;
	const std::weak_ptr<std::atomic_bool> lifetime = m_applicationEventLifetime;
	m_ppc_threads_window->Bind(wxEVT_DESTROY, [this, lifetime, createdWindow](wxWindowDestroyEvent& destroyEvent) {
		if (const auto alive = lifetime.lock();
			alive && alive->load(std::memory_order_acquire) && m_ppc_threads_window == createdWindow)
			m_ppc_threads_window = nullptr;
		destroyEvent.Skip();
	});
	m_ppc_threads_window->Show(true);
}

void MainWindow::ClosePpcThreadsViewer()
{
	if (!m_ppc_threads_window)
		return;
	auto* window = m_ppc_threads_window;
	m_ppc_threads_window = nullptr;
	window->Close();
}

void MainWindow::OnDebugViewPPCDebugger(wxCommandEvent& event)
{
	if (m_debugger_window && m_debugger_window->IsShown())
	{
		WxDebuggerAdapters::RequestCloseDebuggerWindow(*m_debugger_window);
		m_debugger_window = nullptr;
		return;
	}

	auto rect = GetDesktopRect();
	/*
	sint32 new_width = max(rect.GetWidth() * 0.70, rect.GetWidth() - 850);
	this->SetSize(new_width, 480);*/

	this->SetSize(800, 450 + 50);
	this->CenterOnScreen();

	auto pos = this->GetPosition();
	pos.y = std::min(pos.y + 200, rect.GetHeight() - 400);
	this->SetPosition(pos);

	m_debugger_window = WxDebuggerAdapters::CreateDebuggerWindow(*this, rect);
	if (!m_debugger_window)
		return;
	m_debugger_window->Bind(wxEVT_CLOSE_WINDOW, &MainWindow::OnDebuggerClose, this);
	m_debugger_window->Show(true);
}

void MainWindow::OnDebugViewAudioDebugger(wxCommandEvent& event)
{
	auto frame = new AudioDebuggerWindow(*this);
	frame->Show(true);
}

void MainWindow::OnDebugViewTextureRelations(wxCommandEvent& event)
{
	openTextureViewer(*this);
}

void MainWindow::ShowCursor(bool state)
{
	#if BOOST_OS_WINDOWS
	CURSORINFO info{};
	info.cbSize = sizeof(CURSORINFO);
	GetCursorInfo(&info);
	const bool visible = info.flags == CURSOR_SHOWING;

	if (state == visible)
		return;

	int counter = 0;
	if(state)
	{
		do
		{
			counter = ::ShowCursor(TRUE);
		} while (counter < 0);
	}
	else
	{
		do
		{
			counter = ::ShowCursor(FALSE);
		} while (counter >= 0);
	}
	#else
	if (state)
	{
		wxSetCursor(wxNullCursor); // restore system default cursor
	}
	else
	{
		wxSetCursor(wxCursor(wxCURSOR_BLANK));
	}
	#endif
}

uintptr_t MainWindow::GetRenderCanvasHWND()
{
	// deprecated. We can use the global cross-platform window info structs now
	#if BOOST_OS_WINDOWS
	if (!m_render_canvas)
		return 0;
	return (uintptr_t)m_render_canvas->GetHWND();
	#else
	return 0;
	#endif
}

wxRect MainWindow::GetDesktopRect()
{
	const auto pos = GetPosition();
	const auto middle = pos.x + GetSize().GetWidth() / 2;

	const auto displayCount = wxDisplay::GetCount();
	for (uint32 i = 0; i < displayCount; ++i)
	{
		wxDisplay display(i);
		if (!display.IsOk())
			continue;

		const auto geo = display.GetGeometry();
		if (geo.x <= middle && middle <= geo.x + geo.width)
			return geo;
	}
	return { 0,0,800,600 };
}

void MainWindow::LoadSettings()
{
	GetConfigHandle().Load();
	const auto& config = GetWxGUIConfig();

	if(config.check_update)
		m_update_available = CemuUpdateWindow::IsUpdateAvailableAsync();

	if (config.window_position != Vector2i{ -1,-1 })
		this->SetPosition({ config.window_position.x, config.window_position.y });

	if (config.window_size.x > 0 && config.window_size.y > 0)
	{
		this->SetSize({ config.window_size.x, config.window_size.y });

		if (config.window_maximized)
			this->Maximize();
	}

	if (config.pad_position != Vector2i{ -1,-1 })
	{
		m_windowState->restored_pad_x = config.pad_position.x;
		m_windowState->restored_pad_y = config.pad_position.y;
	}

	if (config.pad_size != Vector2i{ -1,-1 })
	{
		m_windowState->restored_pad_width = config.pad_size.x;
		m_windowState->restored_pad_height = config.pad_size.y;

		m_windowState->pad_maximized = config.pad_maximized;
	}

	this->TogglePadView();

	if(m_game_list)
		m_game_list->LoadConfig();
}

void MainWindow::SaveSettings()
{
	auto lock = GetConfigHandle().Lock();
	auto& config = GetWxGUIConfig();

	if (config.window_position != Vector2i{ -1,-1 })
	{
		config.window_position.x = m_restored_position.x;
		config.window_position.y = m_restored_position.y;
	}
	if (config.window_size != Vector2i{ -1,-1 })
	{
		config.window_size.x = m_restored_size.x;
		config.window_size.y = m_restored_size.y;
		config.window_maximized = IsMaximized();
	}
	else
	{
		config.window_maximized = false;
	}

	config.pad_open = m_padView != nullptr;

	if (config.pad_position != Vector2i{ -1,-1 } && m_windowState->restored_pad_x != -1)
	{
		config.pad_position.x = m_windowState->restored_pad_x;
		config.pad_position.y = m_windowState->restored_pad_y;
	}
	if (config.pad_size != Vector2i{ -1,-1 } && m_windowState->restored_pad_width != -1)
	{
		config.pad_size.x = m_windowState->restored_pad_width;
		config.pad_size.y = m_windowState->restored_pad_height;
		config.pad_maximized = m_windowState->pad_maximized;
	}
	else
	{
		config.pad_maximized = false;
	}

	if(m_game_list)
		m_game_list->SaveConfig();

	g_wxConfig.Save();
}

void MainWindow::EmitCemuExtendMouseEvent(wxMouseEvent& event, std::int32_t wheelX,
	std::int32_t wheelY, std::uint32_t changedButtons)
{
	if (!m_render_canvas)
		return;
	const auto logicalPosition = event.GetPosition();
	const auto physicalPosition = ToPhys(logicalPosition);
	const auto clientSize = m_render_canvas->GetClientSize();
	const auto physicalWidth = ToPhys(clientSize.GetWidth());
	const auto physicalHeight = ToPhys(clientSize.GetHeight());
	const bool inside = logicalPosition.x >= 0 && logicalPosition.y >= 0 &&
		logicalPosition.x < clientSize.GetWidth() && logicalPosition.y < clientSize.GetHeight();
	// Button callbacks are the authoritative transitions. In particular,
	// synthetic/private-desktop motion can report wx's aggregate state as
	// released even while a button is held. Preserve the confirmed mask for
	// motion/wheel events so a drag cannot turn into a spurious ButtonUp.
	// wxWidgets reports the second press of a rapid click pair as DCLICK
	// instead of another DOWN event. It is still a physical down transition.
	Frontend::CemuExtendMouseTransition transition =
		Frontend::CemuExtendMouseTransition::None;
	if (event.ButtonDown() || event.ButtonDClick())
		transition = Frontend::CemuExtendMouseTransition::Down;
	else if (event.ButtonUp())
		transition = Frontend::CemuExtendMouseTransition::Up;
	else if (changedButtons != 0)
		transition = Frontend::CemuExtendMouseTransition::Aggregate;
	const auto buttonUpdate = m_cemuextend_bridge.UpdateButtons(transition,
		changedButtons, CemuExtendMouseButtons(event));

	const wxPoint center{clientSize.GetWidth() / 2, clientSize.GetHeight() / 2};
	const bool rawMouseAvailable = m_cemuextend_bridge.PointerMode() ==
		static_cast<std::uint8_t>(Frontend::CemuExtendPointerMode::CapturedRelative) &&
		m_cemuextend_bridge.RawMouseRequested() &&
		EnsureCemuExtendRawMouse();
	const auto motion = m_cemuextend_bridge.UpdatePosition(
		{physicalPosition.x, physicalPosition.y},
		{ToPhys(center).x, ToPhys(center).y}, rawMouseAvailable);
	const auto flags = motion.rawRelative
		? static_cast<std::uint8_t>(Frontend::CemuExtendMouseEventFlag::RawRelative)
		: static_cast<std::uint8_t>(Frontend::CemuExtendMouseEventFlag::None);
	m_emulationController.SubmitMouse({
		.surface = Application::PointerSurface::Tv,
		.x = physicalPosition.x,
		.y = physicalPosition.y,
		.deltaX = motion.delta.x,
		.deltaY = motion.delta.y,
		.wheelX = wheelX,
		.wheelY = wheelY,
		.buttons = buttonUpdate.buttons,
		.changedButtons = buttonUpdate.changed,
		.contentWidth = physicalWidth,
		.contentHeight = physicalHeight,
		.insideContent = inside,
		.focused = m_windowState->app_active.load(),
		.flags = flags,
	});
}

void MainWindow::EmitCemuExtendRawMouseEvent(std::int32_t deltaX, std::int32_t deltaY,
	std::uint32_t changedButtons)
{
	if (!m_render_canvas)
		return;
	const auto logicalPosition = m_render_canvas->ScreenToClient(wxGetMousePosition());
	const auto physicalPosition = ToPhys(logicalPosition);
	const auto clientSize = m_render_canvas->GetClientSize();
	const auto physicalWidth = ToPhys(clientSize.GetWidth());
	const auto physicalHeight = ToPhys(clientSize.GetHeight());
	const bool inside = logicalPosition.x >= 0 && logicalPosition.y >= 0 &&
		logicalPosition.x < clientSize.GetWidth() && logicalPosition.y < clientSize.GetHeight();
	m_cemuextend_bridge.RecordRawPosition({physicalPosition.x, physicalPosition.y});
	m_emulationController.SubmitMouse({
		.surface = Application::PointerSurface::Tv,
		.x = physicalPosition.x,
		.y = physicalPosition.y,
		.deltaX = deltaX,
		.deltaY = deltaY,
		.buttons = m_cemuextend_bridge.MouseButtons(),
		.changedButtons = changedButtons,
		.contentWidth = physicalWidth,
		.contentHeight = physicalHeight,
		.insideContent = inside,
		.focused = m_windowState->app_active.load(),
		.flags = static_cast<std::uint8_t>(Frontend::CemuExtendMouseEventFlag::RawRelative),
	});
}

bool MainWindow::EnsureCemuExtendRawMouse()
{
#if BOOST_OS_WINDOWS
	if (m_cemuextend_raw_mouse_registered)
		return true;
	RAWINPUTDEVICE device{};
	device.usUsagePage = 0x01;
	device.usUsage = 0x02;
	device.dwFlags = RIDEV_INPUTSINK;
	device.hwndTarget = reinterpret_cast<HWND>(GetHandle());
	m_cemuextend_raw_mouse_registered =
		RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
	return m_cemuextend_raw_mouse_registered;
#else
	return false;
#endif
}

void MainWindow::UpdateCemuExtendPointerConfinement(bool confine)
{
#if BOOST_OS_WINDOWS
	if (!confine || !m_render_canvas)
	{
		if (m_cemuextend_pointer_confined)
			ClipCursor(nullptr);
		m_cemuextend_pointer_confined = false;
		return;
	}

	const auto window = reinterpret_cast<HWND>(m_render_canvas->GetHandle());
	RECT client{};
	POINT upperLeft{};
	POINT lowerRight{};
	if (!window || !::GetClientRect(window, &client) ||
		client.right <= client.left || client.bottom <= client.top)
	{
		UpdateCemuExtendPointerConfinement(false);
		return;
	}
	upperLeft.x = client.left;
	upperLeft.y = client.top;
	lowerRight.x = client.right;
	lowerRight.y = client.bottom;
	if (!::ClientToScreen(window, &upperLeft) ||
		!::ClientToScreen(window, &lowerRight))
	{
		UpdateCemuExtendPointerConfinement(false);
		return;
	}
	RECT screen{upperLeft.x, upperLeft.y, lowerRight.x, lowerRight.y};
	m_cemuextend_pointer_confined = ClipCursor(&screen) != FALSE;
#else
	(void)confine;
	m_cemuextend_pointer_confined = false;
#endif
}

bool MainWindow::ApplyCemuExtendPointerPolicy()
{
	const auto policy = m_emulationController.GetPointerPolicy();
	const auto decision = m_cemuextend_bridge.ApplyPointerPolicy(policy.mode,
		policy.cursor, policy.flags, m_windowState->app_active.load(),
		m_render_canvas != nullptr);
	UpdateCemuExtendPointerConfinement(decision.confine);

	if (decision.leavingPolicy)
	{
		ShowCursor(true);
		if (m_render_canvas)
			m_render_canvas->SetCursor(wxCursor(wxCURSOR_ARROW));
	}
	else if (decision.showCursor)
	{
		ShowCursor(true);
		if (m_render_canvas)
			m_render_canvas->SetCursor(wxCursor(CemuExtendCursor(decision.cursor)));
	}
	else if (decision.ownsPointer)
		ShowCursor(false);

	if (decision.enteringCapture && m_render_canvas)
	{
		if (decision.requestRawMouse)
			EnsureCemuExtendRawMouse();
		const auto size = m_render_canvas->GetClientSize();
		const wxPoint center{size.GetWidth() / 2, size.GetHeight() / 2};
		m_render_canvas->WarpPointer(center.x, center.y);
		const auto physicalCenter = ToPhys(center);
		m_cemuextend_bridge.RecordRawPosition({physicalCenter.x, physicalCenter.y});
	}
	return decision.ownsPointer;
}

void MainWindow::OnMouseMove(wxMouseEvent& event)
{
	event.Skip();

	m_last_mouse_move_time = std::chrono::steady_clock::now();
	m_mouse_position = wxGetMousePosition();
	const bool bridgeOwnsPointer = ApplyCemuExtendPointerPolicy();
	if (!bridgeOwnsPointer)
		ShowCursor(true);

	auto physPos = ToPhys(event.GetPosition());
	m_inputHostEvents->UpdateMousePosition(Host::PointerSurface::Main,
		{physPos.x, physPos.y});
	EmitCemuExtendMouseEvent(event);

	if (m_cemuextend_bridge.PointerMode() == static_cast<std::uint8_t>(
		Frontend::CemuExtendPointerMode::CapturedRelative) && m_render_canvas)
	{
		const auto size = m_render_canvas->GetClientSize();
		const wxPoint center{size.GetWidth() / 2, size.GetHeight() / 2};
		if (event.GetPosition() != center)
			m_render_canvas->WarpPointer(center.x, center.y);
	}

	if (bridgeOwnsPointer || !IsFullScreen())
		return;

	const auto& config = GetWxGUIConfig();
	// if mouse goes to upper screen then show our menu in fullscreen mode
	if (config.fullscreen_menubar)
		SetMenuVisible(event.GetPosition().y < 50);
}

void MainWindow::OnMouseLeft(wxMouseEvent& event)
{
	const bool pressed = event.ButtonDown(wxMOUSE_BTN_LEFT) ||
		event.ButtonDClick(wxMOUSE_BTN_LEFT);
	const auto physPos = ToPhys(event.GetPosition());
	m_inputHostEvents->UpdateMouseButton(Host::PointerSurface::Main,
		Host::PointerButton::Left, pressed, {physPos.x, physPos.y});
	EmitCemuExtendMouseEvent(event, 0, 0,
		static_cast<std::uint32_t>(Frontend::CemuExtendMouseButton::Left));

	event.Skip();
}

void MainWindow::OnMouseRight(wxMouseEvent& event)
{
	const bool pressed = event.ButtonDown(wxMOUSE_BTN_RIGHT) ||
		event.ButtonDClick(wxMOUSE_BTN_RIGHT);
	const auto physPos = ToPhys(event.GetPosition());
	m_inputHostEvents->UpdateMouseButton(Host::PointerSurface::Main,
		Host::PointerButton::Right, pressed, {physPos.x, physPos.y});
	EmitCemuExtendMouseEvent(event, 0, 0,
		static_cast<std::uint32_t>(Frontend::CemuExtendMouseButton::Right));

	event.Skip();
}

void MainWindow::OnMouseMiddle(wxMouseEvent& event)
{
	cemuLog_log(LogType::Force,
		"CEX2-PERSPECTIVE wx-middle down={} dclick={} up={} aggregateMiddle={} trackedButtons={}",
		event.ButtonDown(wxMOUSE_BTN_MIDDLE), event.ButtonDClick(wxMOUSE_BTN_MIDDLE),
		event.ButtonUp(wxMOUSE_BTN_MIDDLE), event.MiddleIsDown(),
		m_cemuextend_bridge.MouseButtons());
	EmitCemuExtendMouseEvent(event, 0, 0,
		static_cast<std::uint32_t>(Frontend::CemuExtendMouseButton::Middle));
	event.Skip();
}

void MainWindow::OnMouseAux(wxMouseEvent& event)
{
	std::uint32_t changed{};
	if (event.GetButton() == wxMOUSE_BTN_AUX1)
		changed = static_cast<std::uint32_t>(Frontend::CemuExtendMouseButton::X1);
	else if (event.GetButton() == wxMOUSE_BTN_AUX2)
		changed = static_cast<std::uint32_t>(Frontend::CemuExtendMouseButton::X2);
	EmitCemuExtendMouseEvent(event, 0, 0, changed);
	event.Skip();
}

void MainWindow::OnGameListBeginUpdate(wxCommandEvent& event)
{
	if (m_game_list->IsShown())
		m_info_bar->ShowMessage(_("Updating game list..."));
}

void MainWindow::OnGameListEndUpdate(wxCommandEvent& event)
{
	m_info_bar->Dismiss();
}

void MainWindow::OnAccountListRefresh(wxCommandEvent& event)
{
	RecreateMenu();
}

void MainWindow::OnRequestGameListRefresh(wxCommandEvent& event)
{
	m_game_list->ReloadGameEntries();
}

void MainWindow::OnSetWindowTitle(wxCommandEvent& event)
{
	this->SetTitle(event.GetString());
}

void MainWindow::OnKeyUp(wxKeyEvent& event)
{
	event.Skip();

	if (m_emulationController.SoftwareKeyboardActive())
		return;

	HotkeySettings::CaptureInput(event);
}

void MainWindow::OnKeyDown(wxKeyEvent& event)
{
#if defined(__APPLE__)
       // On macOS, allow Cmd+Q to quit the application
    if (event.CmdDown() && event.GetKeyCode() == 'Q')
    {
        Close(true);
    }
#else
     // On Windows/Linux, only Alt+F4 is allowed for quitting
    if (event.AltDown() && event.GetKeyCode() == WXK_F4)
    {
        Close(true);
    }
#endif
    else
    {
        event.Skip();
    }
}

void MainWindow::OnChar(wxKeyEvent& event)
{
	m_emulationController.SubmitSoftwareKeyboardKey(event.GetUnicodeKey());

	// event.Skip();
}

void MainWindow::OnToolsInput(wxCommandEvent& event)
{
	const auto id = event.GetId();
	switch (id)
	{
	case MAINFRAME_MENU_ID_TOOLS_MEMORY_SEARCHER:
	{
		if (m_toolWindow)
			m_toolWindow->SetFocus();
		else
		{
			m_toolWindow = WxDebuggerAdapters::CreateMemorySearcherWindow(*this);
			auto* const createdWindow = m_toolWindow;
			m_toolWindow->Bind(wxEVT_CLOSE_WINDOW, [this, createdWindow](wxCloseEvent& event)
				{
					if (m_toolWindow == createdWindow)
						m_toolWindow = nullptr;
					event.Skip();
				});
			m_toolWindow->Show(true);
		}
		break;
	}
	case MAINFRAME_MENU_ID_TOOLS_TITLE_MANAGER:
	case MAINFRAME_MENU_ID_TOOLS_DOWNLOAD_MANAGER:
	{
		const auto default_tab = id == MAINFRAME_MENU_ID_TOOLS_TITLE_MANAGER ? TitleManagerPage::TitleManager : TitleManagerPage::DownloadManager;

		if (m_title_manager)
			m_title_manager->SetFocusAndTab(default_tab);
		else
		{
			m_title_manager = new TitleManager(this, m_emulationController,
				m_uiDispatcher, m_pathProvider,
				[this](fs::path path) {
					QueueEvent(new wxLaunchGameEvent(std::move(path),
						wxLaunchGameEvent::INITIATED_BY::TITLE_MANAGER));
				},
				[this] { RequestGameListRefresh(); }, default_tab);
			m_title_manager->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event)
				{
					m_title_manager = nullptr;
					event.Skip();
				});
			m_title_manager->Show();
		}
		break;
	}
	case MAINFRAME_MENU_ID_TOOLS_EMULATED_USB_DEVICES:
	{
		if (m_usb_devices)
		{
			m_usb_devices->Show(true);
			m_usb_devices->Raise();
			m_usb_devices->SetFocus();
		}
		else
		{
			m_usb_devices = WxDeviceAdapters::CreateEmulatedUSBDeviceWindow(*this);
			auto* createdWindow = m_usb_devices;
			m_usb_devices->Bind(wxEVT_CLOSE_WINDOW, [this, createdWindow](wxCloseEvent& event)
				{
					if (event.CanVeto()) {
						createdWindow->Show(false);
						event.Veto();
						return;
					}
					if (m_usb_devices == createdWindow)
						m_usb_devices = nullptr;
					event.Skip();
				});
			m_usb_devices->Show(true);
		}
		break;
	}
	break;
	}
}

void MainWindow::OnGesturePan(wxPanGestureEvent& event)
{
	auto physPos = ToPhys(event.GetPosition());
	m_inputHostEvents->UpdateTouch(Host::PointerSurface::Main,
		{physPos.x, physPos.y}, event.IsGestureStart() || !event.IsGestureEnd());


	event.Skip();
}

void MainWindow::OnGameLoaded()
{
	if (m_debugger_window)
		WxDebuggerAdapters::NotifyGameLoaded(*m_debugger_window);
}

void MainWindow::AsyncSetTitle(std::string_view windowTitle)
{
	cemu_assert_debug(wxIsMainThread());
	SetTitle(wxString::FromUTF8(windowTitle));
}

bool MainWindow::IsCemuExtendTextInputEvent(const wxEvent& event) const
{
	return m_cemuextend_text_input != nullptr &&
		(HasCemuExtendTextInputNativeFocus() ||
			event.GetEventObject() == m_cemuextend_text_input);
}

bool MainWindow::CanSubmitCemuExtendTextInput() const
{
	// Enter must remain owned by the OS IME while a preedit/candidate is
	// active. Once it is committed, a subsequent Enter may be mirrored to the
	// guest as the single-line field's submit action.
	return m_cemuextend_text_input != nullptr &&
		m_cemuextend_bridge.CanSubmitText();
}

bool MainWindow::HasCemuExtendTextInputNativeFocus() const
{
	if (m_cemuextend_text_input == nullptr) return false;
#if defined(__WXGTK__)
	GtkWidget* entry =
		static_cast<GtkWidget*>(m_cemuextend_text_input->GetHandle());
	return GTK_IS_WIDGET(entry) && gtk_widget_has_focus(entry);
#else
	return m_cemuextend_text_input->HasFocus();
#endif
}

void MainWindow::EnsureCemuExtendTextInputFocus(std::uint64_t sequence)
{
	if (m_cemuextend_text_input == nullptr ||
		m_cemuextend_bridge.TextInputSequence() != sequence ||
		!m_cemuextend_text_input->IsShown())
		return;
	if (HasCemuExtendTextInputNativeFocus())
	{
		m_cemuextend_text_input_focus_retries = 0;
		if (!m_cemuextend_text_input_focus_logged)
		{
			m_cemuextend_text_input_focus_logged = true;
			cemuLog_log(LogType::Force,
				"CemuExtend text input: native IME focus acquired");
		}
		return;
	}

	m_cemuextend_text_input->Raise();
	m_cemuextend_text_input->SetFocus();
#if defined(__WXGTK__)
	GtkWidget* entry =
		static_cast<GtkWidget*>(m_cemuextend_text_input->GetHandle());
	if (GTK_IS_WIDGET(entry) && gtk_widget_get_mapped(entry))
		gtk_widget_grab_focus(entry);
#endif
	if (HasCemuExtendTextInputNativeFocus())
	{
		EnsureCemuExtendTextInputFocus(sequence);
		return;
	}
	if (m_cemuextend_text_input_focus_retries >= 4)
	{
		if (!m_cemuextend_text_input_focus_failure_logged)
		{
			m_cemuextend_text_input_focus_failure_logged = true;
			cemuLog_log(LogType::Force,
				"CemuExtend text input: failed to acquire native IME focus");
		}
		return;
	}
	++m_cemuextend_text_input_focus_retries;
	const std::weak_ptr<std::atomic_bool> lifetime = m_applicationEventLifetime;
	(void)m_uiDispatcher->Queue([this, lifetime, sequence] {
		const auto alive = lifetime.lock();
		if (alive && alive->load(std::memory_order_acquire))
			EnsureCemuExtendTextInputFocus(sequence);
	});
}

void MainWindow::PublishCemuExtendTextComposition()
{
	if (m_cemuextend_text_input_updating || m_cemuextend_text_input == nullptr)
		return;
	const std::string committed =
		m_cemuextend_text_input->GetValue().utf8_string();
	const long insertion = m_cemuextend_text_input->GetInsertionPoint();
	const std::string prefix = m_cemuextend_text_input
		->GetRange(0, insertion).utf8_string();
	const auto composition = m_cemuextend_bridge.ComposeText(committed,
		static_cast<std::uint32_t>(prefix.size()));
	m_emulationController.SubmitTextComposition(composition.committed, composition.preedit,
		composition.cursor, composition.selectionLength);
}

void MainWindow::OnCemuExtendTextChanged(wxCommandEvent& event)
{
	PublishCemuExtendTextComposition();
}

void MainWindow::OnCemuExtendTextPreedit(std::string_view preedit)
{
	m_cemuextend_bridge.SetPreedit(preedit);
	if (!preedit.empty() && !m_cemuextend_text_input_preedit_logged)
	{
		m_cemuextend_text_input_preedit_logged = true;
		cemuLog_log(LogType::Force,
			"CemuExtend text input: native IME preedit received");
	}
	PublishCemuExtendTextComposition();
}

void MainWindow::RefreshCemuExtendTextInput()
{
	if (m_cemuextend_text_input == nullptr || m_render_canvas == nullptr) return;
	const auto state = m_emulationController.GetTextInputState();
	if (!state.active)
	{
		if (m_cemuextend_text_input->IsShown())
		{
#if defined(__WXGTK__)
			GtkWidget* entry = static_cast<GtkWidget*>(
				m_cemuextend_text_input->GetHandle());
			if (GTK_IS_ENTRY(entry))
				gtk_entry_reset_im_context(GTK_ENTRY(entry));
#endif
			m_cemuextend_text_input->Hide();
			m_render_canvas->SetFocus();
		}
		m_cemuextend_bridge.EndTextInput();
		m_cemuextend_text_input_focus_retries = 0;
		m_cemuextend_text_input_focus_logged = false;
		m_cemuextend_text_input_focus_failure_logged = false;
		m_cemuextend_text_input_preedit_logged = false;
		return;
	}
	const wxSize canvas = m_render_canvas->GetClientSize();
	const int x = std::clamp(state.caretX * canvas.GetWidth() / 1280,
		0, std::max(0, canvas.GetWidth() - 1));
	const int y = std::clamp(state.caretY * canvas.GetHeight() / 720,
		0, std::max(0, canvas.GetHeight() - 1));
	// Moving the native widget while an IME owns a preedit can reset some IM
	// modules. Keep its anchor stable until that composition ends.
	if (!m_cemuextend_bridge.HasPreedit())
		m_cemuextend_text_input->SetPosition(
			m_render_canvas->GetPosition() + wxPoint{x, y});
#if defined(__WXGTK__)
	// GTK IM modules derive their candidate position and activation state from
	// the focused widget's allocation. A 1x1 allocation is rejected by some
	// IBus/Fcitx input modules. Keep a normal line height but make the widget
	// itself fully transparent; the IME candidate popup is a separate window
	// and remains visible at this anchor.
	const int nativeLineHeight = std::clamp(
		state.lineHeight * canvas.GetHeight() / 720, 20, 64);
	m_cemuextend_text_input->SetSize(wxSize{2, nativeLineHeight});
#else
	m_cemuextend_text_input->SetSize(wxSize{1, 1});
#endif
	const bool startingSession = m_cemuextend_bridge.BeginTextInput(state.sequence);
	if (startingSession)
	{
		m_cemuextend_text_input_updating = true;
		m_cemuextend_text_input_focus_retries = 0;
		m_cemuextend_text_input_focus_logged = false;
		m_cemuextend_text_input_focus_failure_logged = false;
		m_cemuextend_text_input_preedit_logged = false;
		m_cemuextend_text_input->ChangeValue(
			wxString::FromUTF8(state.initialText.data(), state.initialText.size()));
		m_cemuextend_text_input->SetMaxLength(state.maximumLength);
		m_cemuextend_text_input->SetInsertionPointEnd();
		m_cemuextend_text_input_updating = false;
		// Release any key held by the render canvas before the native control
		// starts owning key events. Otherwise its key-up would be intentionally
		// filtered and the guest could retain a stuck raw key.
		m_emulationController.KeyboardFocusLost();
		m_cemuextend_text_input->Show();
		// This native control exists only to own the OS IME session and its
		// candidate window. Its one-pixel surface is effectively invisible, but
		// it must remain above the render canvas or some native backends refuse
		// to give their IM context real keyboard focus.
	}
	// A render-canvas click can steal native focus without changing the guest's
	// request id. Verify ownership on every wake, using the platform's real
	// focus state rather than wxWindow's pending-focus cache.
	EnsureCemuExtendTextInputFocus(state.sequence);
}

void MainWindow::CreateCanvas()
{
    // create panel for canvas
    m_game_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxNO_BORDER | wxWANTS_CHARS);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // shouldn't be needed, but who knows
    m_game_panel->Bind(wxEVT_KEY_UP, &MainWindow::OnKeyUp, this);
    m_game_panel->Bind(wxEVT_CHAR, &MainWindow::OnChar, this);

    m_game_panel->SetSizer(sizer);
    this->GetSizer()->Add(m_game_panel, 1, wxEXPAND);

    // create canvas
	#ifdef ENABLE_OPENGL
	if (ActiveSettings::GetGraphicsAPI() == kOpenGL)
		m_render_canvas = GLCanvas_Create(m_game_panel, wxSize(1280, 720), true,
			m_windowMetrics, m_nativeSurfaces);
	#endif
	#ifdef ENABLE_VULKAN
	if (ActiveSettings::GetGraphicsAPI() == kVulkan)
		m_render_canvas = new VulkanCanvas(m_game_panel, wxSize(1280, 720), true,
			m_windowMetrics, m_nativeSurfaces, m_nativeSurfacePublisher);
	#endif
	#ifdef ENABLE_METAL
	if (ActiveSettings::GetGraphicsAPI() == kMetal)
		m_render_canvas = new MetalCanvas(m_game_panel, wxSize(1280, 720), true,
			m_windowMetrics, m_nativeSurfaces, m_nativeSurfacePublisher);
	#endif
	if (!m_render_canvas)
		cemu_assert(false && "Failed to create canvas or invalid graphics API selected");
	cemu_assert(m_render_canvas != nullptr);

	// mouse events
	m_render_canvas->Bind(wxEVT_MOTION, &MainWindow::OnMouseMove, this);
	m_render_canvas->Bind(wxEVT_MOUSEWHEEL, &MainWindow::OnMouseWheel, this);
	m_render_canvas->Bind(wxEVT_LEFT_DOWN, &MainWindow::OnMouseLeft, this);
	m_render_canvas->Bind(wxEVT_LEFT_UP, &MainWindow::OnMouseLeft, this);
	m_render_canvas->Bind(wxEVT_LEFT_DCLICK, &MainWindow::OnMouseLeft, this);
	m_render_canvas->Bind(wxEVT_RIGHT_DOWN, &MainWindow::OnMouseRight, this);
	m_render_canvas->Bind(wxEVT_RIGHT_UP, &MainWindow::OnMouseRight, this);
	m_render_canvas->Bind(wxEVT_RIGHT_DCLICK, &MainWindow::OnMouseRight, this);
	m_render_canvas->Bind(wxEVT_MIDDLE_DOWN, &MainWindow::OnMouseMiddle, this);
	m_render_canvas->Bind(wxEVT_MIDDLE_UP, &MainWindow::OnMouseMiddle, this);
	m_render_canvas->Bind(wxEVT_MIDDLE_DCLICK, &MainWindow::OnMouseMiddle, this);
	m_render_canvas->Bind(wxEVT_AUX1_DOWN, &MainWindow::OnMouseAux, this);
	m_render_canvas->Bind(wxEVT_AUX1_UP, &MainWindow::OnMouseAux, this);
	m_render_canvas->Bind(wxEVT_AUX2_DOWN, &MainWindow::OnMouseAux, this);
	m_render_canvas->Bind(wxEVT_AUX2_UP, &MainWindow::OnMouseAux, this);

	m_render_canvas->Bind(wxEVT_GESTURE_PAN, &MainWindow::OnGesturePan, this);

	// key events
	m_render_canvas->Bind(wxEVT_KEY_UP, &MainWindow::OnKeyUp, this);
	m_render_canvas->Bind(wxEVT_KEY_DOWN, &MainWindow::OnKeyDown, this);
	m_render_canvas->Bind(wxEVT_CHAR, &MainWindow::OnChar, this);

	m_render_canvas->SetDropTarget(new wxAmiiboDropTarget(this));
	m_game_panel->GetSizer()->Add(m_render_canvas, 1, wxEXPAND, 0, nullptr);
	m_cemuextend_text_input = new wxTextCtrl(m_game_panel, wxID_ANY, wxEmptyString,
		wxPoint(0, 0), wxSize(1, 1), wxBORDER_NONE | wxTE_PROCESS_ENTER);
	m_cemuextend_text_input->Hide();
	m_cemuextend_text_input->Bind(wxEVT_TEXT,
		&MainWindow::OnCemuExtendTextChanged, this);
#if defined(__WXGTK__)
	GtkWidget* entry = static_cast<GtkWidget*>(m_cemuextend_text_input->GetHandle());
	if (GTK_IS_ENTRY(entry))
	{
		GdkDisplay* display = gtk_widget_get_display(entry);
		const char* backend = display != nullptr
			? G_OBJECT_TYPE_NAME(display) : "unknown";
		const char* configuredModule = g_getenv("GTK_IM_MODULE");
		const char* selectedModule = configuredModule != nullptr
			? configuredModule : "default";
		// steam-run and similar game environments commonly force XIM for X11
		// games. wxGTK may nevertheless select its Wayland backend. XIM cannot
		// create an input context on a Wayland GdkDisplay, so override only this
		// incompatible pairing for the native text proxy. Preserve explicit
		// fcitx/ibus modules and the normal GTK default everywhere else.
		if (g_strrstr(backend, "Wayland") != nullptr &&
			configuredModule != nullptr &&
			g_strcmp0(configuredModule, "xim") == 0)
		{
			selectedModule = "wayland";
			g_object_set(entry, "im-module", selectedModule, nullptr);
		}
		else if (g_strrstr(backend, "X11") != nullptr &&
			configuredModule != nullptr &&
			g_strcmp0(configuredModule, "wayland") == 0)
		{
			selectedModule = "xim";
			g_object_set(entry, "im-module", selectedModule, nullptr);
		}
		cemuLog_log(LogType::Force,
			"CemuExtend text input: GTK backend {}, IM module {} (environment {})",
			backend, selectedModule,
			configuredModule != nullptr ? configuredModule : "unset");
		gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
		gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_FREE_FORM);
		gtk_entry_set_input_hints(GTK_ENTRY(entry), GTK_INPUT_HINT_NONE);
		// Opacity affects only the proxy widget. IBus/Fcitx candidate windows are
		// separate native windows and therefore remain visible.
		gtk_widget_set_opacity(entry, 0.0);
		g_signal_connect(entry, "preedit-changed",
			G_CALLBACK(+[](GtkEntry*, gchar* preedit, gpointer data) {
				static_cast<MainWindow*>(data)->OnCemuExtendTextPreedit(
					preedit != nullptr ? std::string_view{preedit} : std::string_view{});
			}), this);
	}
#endif

	GetSizer()->Layout();
	m_render_canvas->SetFocus();

	if (m_padView)
		m_padView->InitializeRenderCanvas();
}

void MainWindow::DestroyCanvas()
{
	if (m_cemuextend_text_input)
	{
		m_cemuextend_text_input->Destroy();
		m_cemuextend_text_input = nullptr;
		m_cemuextend_bridge.EndTextInput();
		m_cemuextend_text_input_focus_retries = 0;
		m_cemuextend_text_input_focus_logged = false;
		m_cemuextend_text_input_focus_failure_logged = false;
		m_cemuextend_text_input_preedit_logged = false;
	}
	UpdateCemuExtendPointerConfinement(false);
	if (m_padView)
	{
		m_padView->DestroyCanvas();
	}
	if (m_render_canvas)
	{
		if (auto* canvas = dynamic_cast<IRenderCanvas*>(m_render_canvas))
			canvas->PrepareForDestroy();
		(void)m_nativeSurfacePublisher->PublishCanvas(true, {});
		m_render_canvas->Destroy();
		m_render_canvas = nullptr;
	}
    if(m_game_panel)
    {
        m_game_panel->Destroy();
        m_game_panel = nullptr;
    }
}

void MainWindow::OnSizeEvent(wxSizeEvent& event)
{
	if (!IsMaximized() && !m_windowState->is_fullscreen.load())
		m_restored_size = GetSize();

	const wxSize client_size = GetClientSize();
	m_windowState->width = client_size.GetWidth();
	m_windowState->height = client_size.GetHeight();
	m_windowState->phys_width = ToPhys(client_size.GetWidth());
	m_windowState->phys_height = ToPhys(client_size.GetHeight());
	m_windowState->dpi_scale = GetDPIScaleFactor();

	if (m_debugger_window && m_debugger_window->IsShown())
		WxDebuggerAdapters::NotifyParentMove(*m_debugger_window, GetPosition(), event.GetSize());

	event.Skip();

	WxRendererAdapters::NotifyWindowPositionChanged();
}

void MainWindow::OnDPIChangedEvent(wxDPIChangedEvent& event)
{
	event.Skip();
	const wxSize client_size = GetClientSize();
	m_windowState->width = client_size.GetWidth();
	m_windowState->height = client_size.GetHeight();
	m_windowState->phys_width = ToPhys(client_size.GetWidth());
	m_windowState->phys_height = ToPhys(client_size.GetHeight());
	m_windowState->dpi_scale = GetDPIScaleFactor();
}

void MainWindow::OnMove(wxMoveEvent& event)
{
	if (!IsMaximized() && !m_windowState->is_fullscreen.load())
		m_restored_position = GetPosition();

	if (m_debugger_window && m_debugger_window->IsShown())
		WxDebuggerAdapters::NotifyParentMove(*m_debugger_window, GetPosition(), GetSize());
	WxRendererAdapters::NotifyWindowPositionChanged();
}

void MainWindow::OnDebuggerClose(wxCloseEvent& event)
{
	if (m_debugger_window == event.GetEventObject())
		m_debugger_window = nullptr;
	event.Skip();
}

void MainWindow::OnPadClose(wxCloseEvent& event)
{
	auto* const closingPad = dynamic_cast<PadViewFrame*>(event.GetEventObject());
	if (closingPad)
		closingPad->PrepareForDestroy();
	if (m_padView != closingPad)
	{
		event.Skip();
		return;
	}
	auto& config = GetWxGUIConfig();
	config.pad_open = false;
	if (config.pad_position != Vector2i{ -1,-1 })
		closingPad->GetPosition(&config.pad_position.x, &config.pad_position.y);

	if (config.pad_size != Vector2i{ -1,-1 })
		closingPad->GetSize(&config.pad_size.x, &config.pad_size.y);

	g_wxConfig.Save();

	// already deleted by wxwidget
	m_padView = nullptr;

	if (m_padViewMenuItem)
		m_padViewMenuItem->Check(false);

	event.Skip();
}

void MainWindow::CloseMemorySearcher()
{
	if (!m_toolWindow)
		return;

	auto* const window = m_toolWindow;
	m_toolWindow = nullptr;
	WxDebuggerAdapters::CloseMemorySearcherWindow(*window);
}

void MainWindow::OnMouseWheel(wxMouseEvent& event)
{
	const auto rotation = event.GetWheelRotation();
	const auto reportedDelta = event.GetWheelDelta();
	const auto wheelDelta = reportedDelta > 0 ? reportedDelta : 120;
	const bool horizontal = event.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL;
	const auto steps = m_cemuextend_bridge.NormalizeWheel(rotation, wheelDelta, horizontal);

	m_inputHostEvents->UpdateMouseWheel(
		static_cast<float>(rotation) / static_cast<float>(wheelDelta), steps);
	if (steps != 0)
	{
		if (horizontal)
			EmitCemuExtendMouseEvent(event, steps, 0);
		else
			EmitCemuExtendMouseEvent(event, 0, steps);
	}

	event.Skip();
}

void MainWindow::SetFullScreen(bool state)
{
	// only update config entry if we dont't have launch parameters
	if (!LaunchSettings::FullscreenEnabled().has_value())
	{
		GetWxGUIConfig().fullscreen = state;
		g_wxConfig.Save();
	}
	if (state && !m_game_launched)
		return;
	m_windowState->is_fullscreen = state;
	m_fullscreenMenuItem->Check(state);

	this->ShowFullScreen(state);

	if (state)
		m_menu_visible = false; // menu gets always disabled by wxFULLSCREEN_NOMENUBAR
	else
		SetMenuVisible(true);
}

void MainWindow::EndEmulation() // unfinished - memory leaks and crashes after repeated use (after 3x usually)
{
	ClosePpcThreadsViewer();
	CloseMemorySearcher();
	const auto stopResult = m_emulationController.Stop();
	if (!stopResult.stopped)
	{
		cemuLog_log(LogType::Force, "EmulationController stop request ignored: {}",
			stopResult.diagnostic);
		return;
	}
	DestroyCanvas();
	m_game_launched = false;
	m_launched_game_name.clear();
	#ifdef ENABLE_DISCORD_RPC
	if (m_discord)
		m_discord->UpdatePresence(DiscordPresence::Idling, "");
	#endif

	if (GetConfig().disable_screensaver)
		ScreenSaver::SetInhibit(false);

	if (m_memorySearcherMenuItem)
		m_memorySearcherMenuItem->Enable(false);

	RecreateMenu();
	CreateGameListAndStatusBar();
	DoLayout();
	UpdateChildWindowTitleRunningState();
}

void MainWindow::SetMenuVisible(bool state)
{
	if (m_menu_visible == state)
		return;

	SetMenuBar(state ? m_menuBar : nullptr);
	m_menu_visible = state;
}

void MainWindow::UpdateNFCMenu()
{
	if (m_nfcMenuSeparator0)
	{
		m_nfcMenu->Remove(m_nfcMenuSeparator0->GetId());
		m_nfcMenuSeparator0 = nullptr;
	}
	// remove recent files list
	for (sint32 i = 0; i < wxCemuConfig::kMaxRecentEntries; i++)
	{
		if (m_nfcMenu->FindChildItem(MAINFRAME_MENU_ID_NFC_RECENT_0 + i) == nullptr)
			continue;
		m_nfcMenu->Remove(MAINFRAME_MENU_ID_NFC_RECENT_0 + i);
	}
	// add entries
	const auto& config = GetWxGUIConfig();
	sint32 recentFileIndex = 0;
	for (size_t i = 0; i < config.recent_nfc_files.size(); i++)
	{
		const auto& entry = config.recent_nfc_files[i];
		if (entry.empty())
			continue;

		if (!fs::exists(_utf8ToPath(entry)))
			continue;

		if (recentFileIndex == 0)
			m_nfcMenuSeparator0 = m_nfcMenu->AppendSeparator();

		m_nfcMenu->Append(MAINFRAME_MENU_ID_NFC_RECENT_0 + i, formatWxString("{}. {}", recentFileIndex, entry));

		recentFileIndex++;
		if (recentFileIndex >= 12)
			break;
	}
}

bool MainWindow::IsMenuHidden() const
{
	return m_menu_visible;
}

void MainWindow::OnTimer(wxTimerEvent& event)
{
	if(m_update_available.valid() && future_is_ready(m_update_available))
	{
		if(m_update_available.get())
		{
			wxMessageDialog dialog(this, _("There's a new update available.\nDo you want to update?"), _("Update notification"), wxCENTRE | wxYES_NO);
			if(dialog.ShowModal() == wxID_YES)
			{
				CemuUpdateWindow update_window(this, m_pathProvider);
				update_window.ShowModal();
			}
		}

		m_update_available = {};
	}

	// A connected Mod owns visibility/capture while its pointer policy is active.
	// Returning to Default (focus loss, title stop, or session close) restores
	// Cemu's regular fullscreen auto-hide behavior.
	if (ApplyCemuExtendPointerPolicy())
		return;

	if (!IsFullScreen() || m_menu_visible)
		return;

	const auto mouse_position = wxGetMousePosition();
	if(m_mouse_position != mouse_position)
	{
		m_last_mouse_move_time = std::chrono::steady_clock::now();
		m_mouse_position = mouse_position;
		ShowCursor(true);
		return;
	}

	auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_last_mouse_move_time);
	if (diff.count() > 3000)
	{
		ShowCursor(false);
	}

}

#define BUILD_DATE __DATE__ " " __TIME__

class CemuAboutDialog : public wxDialog
{
public:
	CemuAboutDialog(wxWindow* parent = NULL)
		: wxDialog(NULL, wxID_ANY, _("About Cemu"), wxDefaultPosition, wxSize(500, 700))
	{
		Create(parent);
	}

	void Create(wxWindow* parent = NULL)
	{
		SetIcon(wxICON(M_WND_ICON128));

		wxScrolledWindow* scrolledWindow = new wxScrolledWindow(this);

		wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);

		m_scrolledSizer = new wxBoxSizer(wxVERTICAL);

		AddHeaderInfo(scrolledWindow, m_scrolledSizer);
		m_scrolledSizer->AddSpacer(5);
		AddLibInfo(scrolledWindow, m_scrolledSizer);
		m_scrolledSizer->AddSpacer(5);
		AddThanks(scrolledWindow, m_scrolledSizer);

		scrolledWindow->SetSizer(m_scrolledSizer);
		scrolledWindow->FitInside();
		scrolledWindow->SetScrollRate(25, 25);
		mainSizer->Add(scrolledWindow, wxSizerFlags(1).Expand().Border(wxLEFT, 10));

		SetSizer(mainSizer);
		CentreOnParent();
	}

	void AddHeaderInfo(wxWindow* parent, wxSizer* sizer)
	{
		auto versionString = formatWxString(_("Cemu\nVersion {0}\nCompiled on {1}\nOriginal authors: {2}"), BUILD_VERSION_STRING, BUILD_DATE, "Exzap, Petergov");

		sizer->Add(new wxStaticText(parent, wxID_ANY, versionString), wxSizerFlags().Border(wxALL, 3).Border(wxTOP, 10));
		sizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://cemu.info", "https://cemu.info", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)), wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, 3));

		sizer->AddSpacer(3);
		sizer->Add(new wxStaticLine(parent), wxSizerFlags().Expand().Border(wxRIGHT, 4));
		sizer->AddSpacer(5);

		wxString extraInfo(_("Cemu is a Wii U emulator.\n\nWii and Wii U are trademarks of Nintendo.\nCemu is not affiliated with Nintendo."));
		sizer->Add(new wxStaticText(parent, wxID_ANY, extraInfo), wxSizerFlags());
	}

	void AddLibInfo(wxWindow* parent, wxSizer* sizer)
	{
		sizer->AddSpacer(3);
		sizer->Add(new wxStaticLine(parent), wxSizerFlags().Expand().Border(wxRIGHT, 4));
		sizer->AddSpacer(3);

		sizer->Add(new wxStaticText(parent, wxID_ANY, _("Used libraries and utilities:")), wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, 2));
		// zLib
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "zLib ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://www.zlib.net", "https://www.zlib.net", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(lineSizer);
		}
		// wxWidgets
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "wxWidgets ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://www.wxwidgets.org/", "https://www.wxwidgets.org/", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(lineSizer);
		}
		// OpenSSL
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "OpenSSL ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://www.openssl.org/", "https://www.openssl.org/", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(lineSizer);
		}
		// libcurl
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "libcurl ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://curl.haxx.se/libcurl/", "https://curl.haxx.se/libcurl/", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(lineSizer);
		}
		// imgui
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "imgui ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://github.com/ocornut/imgui", "https://github.com/ocornut/imgui", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(lineSizer);
		}
		// fontawesome
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "fontawesome ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://github.com/FortAwesome/Font-Awesome", "https://github.com/FortAwesome/Font-Awesome", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(lineSizer);
		}
		// boost
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "boost ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://www.boost.org", "https://www.boost.org", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(lineSizer);
		}
		// libusb
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "libusb ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://libusb.info", "https://libusb.info", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(lineSizer);
		}
#if BOOST_OS_MACOS
		// MoltenVK
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, -1, "MoltenVK ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, -1, "https://github.com/KhronosGroup/MoltenVK", "https://github.com/KhronosGroup/MoltenVK", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, -1, ")"));
			sizer->Add(lineSizer);
		}
#endif
		// icons
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "icons from "));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://icons8.com", "https://icons8.com", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			sizer->Add(lineSizer);
		}
		// Lato font (are we still using it?)
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "\"Lato\" font by tyPoland Lukasz Dziedzic (OFL, V1.1)", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			sizer->Add(lineSizer);
		}
		// SDL
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "SDL ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "https://github.com/libsdl-org/SDL", "https://github.com/libsdl-org/SDL", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(lineSizer);
		}
		// IH264
		{
			wxSizer* lineSizer = new wxBoxSizer(wxHORIZONTAL);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, "Modified ih264 from Android project ("));
			lineSizer->Add(new wxHyperlinkCtrl(parent, wxID_ANY, "Source", "https://cemu.info/oss/ih264d.zip", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT)));
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, " "));
			wxHyperlinkCtrl* noticeLink = new wxHyperlinkCtrl(parent, wxID_ANY, "NOTICE", "", wxDefaultPosition, wxDefaultSize, (wxHL_CONTEXTMENU|wxNO_BORDER|wxHL_ALIGN_LEFT));
			noticeLink->Bind(wxEVT_LEFT_DOWN, [](wxMouseEvent& event)
				{
					fs::path tempPath = fs::temp_directory_path();
					tempPath.append("NOTICE_IH264.txt");
					FileStream* fs = FileStream::createFile2(tempPath);
					if (!fs)
						return;
					fs->writeString(
						"/******************************************************************************\r\n"
						" *\r\n"
						" * Copyright (C) 2015 The Android Open Source Project\r\n"
						" *\r\n"
						" * Licensed under the Apache License, Version 2.0 (the \"License\");\r\n"
						" * you may not use this file except in compliance with the License.\r\n"
						" * You may obtain a copy of the License at:"
						" *\r\n"
						" * http://www.apache.org/licenses/LICENSE-2.0\r\n"
						" *\r\n"
						" * Unless required by applicable law or agreed to in writing, software\r\n"
						" * distributed under the License is distributed on an \"AS IS\" BASIS,\r\n"
						" * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\r\n"
						" * See the License for the specific language governing permissions and\r\n"
						" * limitations under the License.\r\n"
						" *\r\n"
						" *****************************************************************************\r\n"
						" * Originally developed and contributed by Ittiam Systems Pvt. Ltd, Bangalore\r\n"
						"*/\r\n"
						"/*****************************************************************************/\r\n"
					);
					delete fs;
					wxLaunchDefaultBrowser(formatWxString("file:{}", _pathToUtf8(tempPath)));
				});
			lineSizer->Add(noticeLink);
			lineSizer->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(lineSizer);
		}
	}

	void AddThanks(wxWindow* parent, wxSizer* sizer)
	{
		sizer->AddSpacer(3);
		sizer->Add(new wxStaticLine(parent), wxSizerFlags().Expand().Border(wxRIGHT, 4));
		sizer->AddSpacer(3);

		wxGridSizer* gridSizer = new wxGridSizer(1, 2, 0, 0);

		sizer->AddSpacer(2);

		sizer->Add(new wxStaticText(parent, wxID_ANY, _("Thanks to our Patreon supporters:")), wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, 2));

		std::vector<const char*> patreonSupporterNames{ "Maufeat", "lvlv", "F34R", "John Godgames", "Jameel Lewis", "skooks", "Cheesy", "Barrowsx", "Mored1984", "madmat007"
			, "Kuhnnl", "Owen M", "lucianobugalu", "KimoMaka", "nick palma aka renaissance18", "TheGiantBros", "SpiGAndromeda"
			, "Chimech0", "Nicolás Pino", "Pezzatti", "Barry Wallace", "REGNR8 Productions", "Lagia", "Freestyler316", "Dentora"
			, "tactics", "Merola.C", "Ceigyx", "Mata", "BobSchneeder45", "fenixDG", "jjalapeno55", "FissionMetroid101", "Jetta88"
			, "nesxdie", "Mikah", "PornfoxVR.com", "Hunter4everosa", "Bbzx", "Salim Sanehi", "FalloutpunkX", "NashOH-CL", "RaheemWala"
			, "Faris Leonhart", "MahvZero", "PlaguedGuardian", "Stuffie", "CaptainLester", "Qtech", "Zaurexus", "Leonidas", "Artifesto"
			, "Alca259", "SirWestofAsh", "Loli Co.", "The Technical Revolutionary", "MegaYama", "mitori", "Seymordius", "Adrian Josh Cruz", "Manuel Hoenings", "Just A Jabb"
			, "pgantonio", "CannonXIII", "Lonewolf00708", "AlexsDesign.com", "NoskLo", "MrSirHaku", "xElite_V AKA William H. Johnson", "Zalnor", "Pig", "James \"SE4LS\"", "DairyOrange", "Horoko Lawrence", "bloodmc", "Officer Jenny", "Quasar", "Postposterous", "Jake Jackson", "Kaydax", "CthePredatorG"
			, "Hengi", "Pyrochaser", "luma.x3"};

		wxString nameListLeft, nameListRight;
		for (size_t i = 0; i < patreonSupporterNames.size(); i++)
		{
			const char* name = patreonSupporterNames[i];
			wxString& nameList = ((i % 2) == 0) ? nameListLeft : nameListRight;
			if (i >= 2)
				nameList.append("\n");
			nameList.append(wxString::FromUTF8(name));
		}

		gridSizer->Add(new wxStaticText(parent, wxID_ANY, nameListLeft), wxSizerFlags());
		gridSizer->Add(new wxStaticText(parent, wxID_ANY, nameListRight), wxSizerFlags());

		sizer->AddSpacer(4);

		sizer->Add(gridSizer, 1, wxEXPAND);

		sizer->AddSpacer(2);
		sizer->Add(new wxStaticText(parent, wxID_ANY, _("Special thanks:")), wxSizerFlags().Expand().Border(wxTOP, 2));
		sizer->Add(new wxStaticText(parent, wxID_ANY, "espes - Also try XQEMU!\nWaltzz92"), wxSizerFlags().Expand().Border(wxTOP, 1));
	}

protected:
	wxSizer* m_scrolledSizer;
};

void MainWindow::OnHelpAbout(wxCommandEvent& event)
{
	CemuAboutDialog dlgAbout(this);
	dlgAbout.ShowModal();
}

void MainWindow::OnHelpUpdate(wxCommandEvent& event)
{
	CemuUpdateWindow test(this, m_pathProvider);
	test.ShowModal();
}

void MainWindow::RecreateMenu()
{
	if (m_menuBar)
	{
		SetMenuBar(nullptr);
		m_menuBar->Destroy();
		m_menuBar = nullptr;
	}

	auto& guiConfig = GetWxGUIConfig();

	m_menuBar = new wxMenuBar();
	// file submenu
	m_fileMenu = new wxMenu();

	if (!m_game_launched)
	{
		m_loadMenuItem = m_fileMenu->Append(MAINFRAME_MENU_ID_FILE_LOAD, _("&Load..."));
		m_installUpdateMenuItem = m_fileMenu->Append(MAINFRAME_MENU_ID_FILE_INSTALL_UPDATE, _("&Install game title, update or DLC..."));

		wxMenu* recentMenu = new wxMenu();
		sint32 recentFileIndex = 1;
		m_fileMenuSeparator0 = nullptr;
		m_fileMenuSeparator1 = nullptr;
		for (size_t i = 0; i < guiConfig.recent_launch_files.size(); i++)
		{
			const std::string& pathStr = guiConfig.recent_launch_files[i];
			if (pathStr.empty())
				continue;
			recentMenu->Append(MAINFRAME_MENU_ID_FILE_RECENT_0 + i, formatWxString("{}. {}", recentFileIndex, pathStr));
			recentFileIndex++;

			if (recentFileIndex >= 10)
				break;
		}
		if (recentFileIndex == 0)
		{
			wxMenuItem* placeholder = recentMenu->Append(wxID_NONE, _("(No recent files)"));
			placeholder->Enable(false);
		}

		m_fileMenu->AppendSeparator();
		m_fileMenu->AppendSubMenu(recentMenu, _("Recent files"));
		m_fileMenu->AppendSeparator();
	}
	else
	{
#ifdef CEMU_DEBUG_ASSERT
		m_fileMenu->Append(MAINFRAME_MENU_ID_FILE_END_EMULATION, _("Close game"));
		m_fileMenuSeparator1 = m_fileMenu->AppendSeparator();
#endif
	}

	m_fileMenu->Append(MAINFRAME_MENU_ID_FILE_OPEN_CEMU_FOLDER, _("Open Cemu folder"));
	m_fileMenu->Append(MAINFRAME_MENU_ID_FILE_OPEN_MLC_FOLDER, _("Open MLC folder"));
	m_fileMenu->Append(MAINFRAME_MENU_ID_FILE_OPEN_SHADERCACHE_FOLDER, _("Open &shader cache folder"));
	m_fileMenu->AppendSeparator();
	m_fileMenu->Append(MAINFRAME_MENU_ID_FILE_CLEAR_SPOTPASS_CACHE, _("Clear Spot&Pass cache"));
	if (m_game_launched)
		m_fileMenu->Enable(MAINFRAME_MENU_ID_FILE_CLEAR_SPOTPASS_CACHE, false);
	m_fileMenu->AppendSeparator();
	m_exitMenuItem = m_fileMenu->Append(MAINFRAME_MENU_ID_FILE_EXIT, _("&Exit"));
	m_menuBar->Append(m_fileMenu, _("&File"));
	// options->account submenu
	m_optionsAccountMenu = new wxMenu();
	const auto account_id = ActiveSettings::GetPersistentId();
	int index = 0;
	for(const auto& account : m_emulationController.ListAccounts())
	{
		wxMenuItem* item = m_optionsAccountMenu->AppendRadioItem(
			MAINFRAME_MENU_ID_OPTIONS_ACCOUNT_1 + index,
			fmt::format(L"{} ({:x})", account.miiName, account.persistentId));
		item->Check(account_id == account.persistentId);
		if (m_game_launched || LaunchSettings::GetPersistentId().has_value())
			item->Enable(false);

		++index;
	}

	auto& config = GetConfig();
	auto& wxConfig = GetWxGUIConfig();
	// options->console language submenu
	wxMenu* optionsConsoleLanguageMenu = new wxMenu();
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_ENGLISH, _("&English"))->Check(config.console_language == CafeConsoleLanguage::EN);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_JAPANESE, _("&Japanese"))->Check(config.console_language == CafeConsoleLanguage::JA);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_FRENCH, _("&French"))->Check(config.console_language == CafeConsoleLanguage::FR);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_GERMAN, _("&German"))->Check(config.console_language == CafeConsoleLanguage::DE);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_ITALIAN, _("&Italian"))->Check(config.console_language == CafeConsoleLanguage::IT);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_SPANISH, _("&Spanish"))->Check(config.console_language == CafeConsoleLanguage::ES);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_CHINESE, _("&Chinese"))->Check(config.console_language == CafeConsoleLanguage::ZH);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_KOREAN, _("&Korean"))->Check(config.console_language == CafeConsoleLanguage::KO);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_DUTCH, _("&Dutch"))->Check(config.console_language == CafeConsoleLanguage::NL);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_PORTUGUESE, _("&Portuguese"))->Check(config.console_language == CafeConsoleLanguage::PT);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_RUSSIAN, _("&Russian"))->Check(config.console_language == CafeConsoleLanguage::RU);
	optionsConsoleLanguageMenu->AppendRadioItem(MAINFRAME_MENU_ID_OPTIONS_LANGUAGE_TAIWANESE, _("&Taiwanese"))->Check(config.console_language == CafeConsoleLanguage::TW);
	if(IsGameLaunched())
	{
		auto items = optionsConsoleLanguageMenu->GetMenuItems();
		for (auto& item : items)
		{
			item->Enable(false);
		}
	}

	// options submenu
	wxMenu* optionsMenu = new wxMenu();
	m_fullscreenMenuItem = optionsMenu->AppendCheckItem(MAINFRAME_MENU_ID_OPTIONS_FULLSCREEN, _("&Fullscreen"));
	m_fullscreenMenuItem->Check(FullscreenEnabled());

	optionsMenu->Append(MAINFRAME_MENU_ID_OPTIONS_GRAPHIC_PACKS2, _("&Graphic packs"));
	m_padViewMenuItem = optionsMenu->AppendCheckItem(MAINFRAME_MENU_ID_OPTIONS_SECOND_WINDOW_PADVIEW, _("&Separate GamePad view"));
	m_padViewMenuItem->Check(wxConfig.pad_open);
	optionsMenu->AppendSeparator();
	#if BOOST_OS_MACOS
	optionsMenu->Append(MAINFRAME_MENU_ID_OPTIONS_MAC_SETTINGS, _("&Settings..." "\tCtrl-,"));
	#endif
	optionsMenu->Append(MAINFRAME_MENU_ID_OPTIONS_GENERAL2, _("&General settings"));
	optionsMenu->Append(MAINFRAME_MENU_ID_OPTIONS_INPUT, _("&Input settings"));
	optionsMenu->Append(MAINFRAME_MENU_ID_OPTIONS_HOTKEY, _("&Hotkey settings"));

	optionsMenu->AppendSeparator();
	optionsMenu->AppendSubMenu(m_optionsAccountMenu, _("&Active account"));
	optionsMenu->AppendSubMenu(optionsConsoleLanguageMenu, _("&Console language"));
	m_menuBar->Append(optionsMenu, _("&Options"));

	// tools submenu
	wxMenu* toolsMenu = new wxMenu();
	m_memorySearcherMenuItem = toolsMenu->Append(MAINFRAME_MENU_ID_TOOLS_MEMORY_SEARCHER, _("&Memory searcher"));
	m_memorySearcherMenuItem->Enable(false);
	toolsMenu->Append(MAINFRAME_MENU_ID_TOOLS_TITLE_MANAGER, _("&Title Manager"));
	toolsMenu->Append(MAINFRAME_MENU_ID_TOOLS_DOWNLOAD_MANAGER, _("&Download Manager"));
	toolsMenu->Append(MAINFRAME_MENU_ID_TOOLS_EMULATED_USB_DEVICES, _("&Emulated USB Devices"));

	m_menuBar->Append(toolsMenu, _("&Tools"));

	// cpu timer speed menu
	wxMenu* timerSpeedMenu = new wxMenu();
	timerSpeedMenu->AppendRadioItem(MAINFRAME_MENU_ID_TIMER_SPEED_1X, _("&1x speed"))->Check(ActiveSettings::GetTimerShiftFactor() == 3);
	timerSpeedMenu->AppendRadioItem(MAINFRAME_MENU_ID_TIMER_SPEED_2X, _("&2x speed"))->Check(ActiveSettings::GetTimerShiftFactor() == 2);
	timerSpeedMenu->AppendRadioItem(MAINFRAME_MENU_ID_TIMER_SPEED_4X, _("&4x speed"))->Check(ActiveSettings::GetTimerShiftFactor() == 1);
	timerSpeedMenu->AppendRadioItem(MAINFRAME_MENU_ID_TIMER_SPEED_8X, _("&8x speed"))->Check(ActiveSettings::GetTimerShiftFactor() == 0);
	timerSpeedMenu->AppendRadioItem(MAINFRAME_MENU_ID_TIMER_SPEED_05X, _("&0.5x speed"))->Check(ActiveSettings::GetTimerShiftFactor() == 4);
	timerSpeedMenu->AppendRadioItem(MAINFRAME_MENU_ID_TIMER_SPEED_025X, _("&0.25x speed"))->Check(ActiveSettings::GetTimerShiftFactor() == 5);
	timerSpeedMenu->AppendRadioItem(MAINFRAME_MENU_ID_TIMER_SPEED_0125X, _("&0.125x speed"))->Check(ActiveSettings::GetTimerShiftFactor() == 6);

	// cpu submenu
	wxMenu* cpuMenu = new wxMenu();
	cpuMenu->AppendSubMenu(timerSpeedMenu, _("&Timer speed"));
	m_menuBar->Append(cpuMenu, _("&CPU"));

	// nfc submenu
	wxMenu* nfcMenu = new wxMenu();
	m_nfcMenu = nfcMenu;
	nfcMenu->Append(MAINFRAME_MENU_ID_NFC_TOUCH_NFC_FILE, _("&Scan NFC tag/amiibo from file"))->Enable(false);
	m_menuBar->Append(nfcMenu, _("&NFC"));
	m_nfcMenuSeparator0 = nullptr;
	// debug->logging submenu
	wxMenu* debugLoggingMenu = new wxMenu();

	debugLoggingMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::UnsupportedAPI), _("&Unsupported API calls"))->Check(cemuLog_isLoggingEnabled(LogType::UnsupportedAPI));
	debugLoggingMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::APIErrors), _("&Invalid API usage"))->Check(cemuLog_isLoggingEnabled(LogType::APIErrors));
	debugLoggingMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::CoreinitLogging), _("&Coreinit Logging (OSReport/OSConsole)"))->Check(cemuLog_isLoggingEnabled(LogType::CoreinitLogging));
	debugLoggingMenu->AppendSeparator();

	wxMenu* logCosModulesMenu = new wxMenu();
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING_MESSAGE, _("&Options below are for experts. Leave off if unsure"))->Enable(false);
	logCosModulesMenu->AppendSeparator();
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::CoreinitFile), _("coreinit File-Access API"))->Check(cemuLog_isLoggingEnabled(LogType::CoreinitFile));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::CoreinitThreadSync), _("coreinit Thread-Synchronization API"))->Check(cemuLog_isLoggingEnabled(LogType::CoreinitThreadSync));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::CoreinitMem), _("coreinit Memory API"))->Check(cemuLog_isLoggingEnabled(LogType::CoreinitMem));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::CoreinitMP), _("coreinit MP API"))->Check(cemuLog_isLoggingEnabled(LogType::CoreinitMP));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::CoreinitThread), _("coreinit Thread API"))->Check(cemuLog_isLoggingEnabled(LogType::CoreinitThread));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::Save), _("nn_save API"))->Check(cemuLog_isLoggingEnabled(LogType::Save));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::NN_NFP), _("nn_nfp API"))->Check(cemuLog_isLoggingEnabled(LogType::NN_NFP));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::NN_FP), _("nn_fp API"))->Check(cemuLog_isLoggingEnabled(LogType::NN_FP));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::PRUDP), _("nn_fp PRUDP"))->Check(cemuLog_isLoggingEnabled(LogType::PRUDP));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::NN_BOSS), _("nn_boss API"))->Check(cemuLog_isLoggingEnabled(LogType::NN_BOSS));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::NFC), _("nfc API"))->Check(cemuLog_isLoggingEnabled(LogType::NFC));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::NTAG), _("ntag API"))->Check(cemuLog_isLoggingEnabled(LogType::NTAG));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::Socket), _("nsysnet API"))->Check(cemuLog_isLoggingEnabled(LogType::Socket));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::H264), _("h264 API"))->Check(cemuLog_isLoggingEnabled(LogType::H264));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::GX2), _("gx2 API"))->Check(cemuLog_isLoggingEnabled(LogType::GX2));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::SWKBD), _("swkbd API"))->Check(cemuLog_isLoggingEnabled(LogType::SWKBD));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::SoundAPI), _("Audio API"))->Check(cemuLog_isLoggingEnabled(LogType::SoundAPI));
	logCosModulesMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::InputAPI), _("Input API"))->Check(cemuLog_isLoggingEnabled(LogType::InputAPI));

	debugLoggingMenu->AppendSubMenu(logCosModulesMenu, _("&CafeOS modules logging"));
	debugLoggingMenu->AppendSeparator();
	debugLoggingMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::Patches), _("&Graphic pack patches"))->Check(cemuLog_isLoggingEnabled(LogType::Patches));
	debugLoggingMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::TextureCache), _("&Texture cache warnings"))->Check(cemuLog_isLoggingEnabled(LogType::TextureCache));
	debugLoggingMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::TextureReadback), _("&Texture readback"))->Check(cemuLog_isLoggingEnabled(LogType::TextureReadback));
	debugLoggingMenu->AppendSeparator();
	debugLoggingMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::OpenGLLogging), _("&OpenGL debug output"))->Check(cemuLog_isLoggingEnabled(LogType::OpenGLLogging));
	debugLoggingMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::VulkanValidation), _("&Vulkan validation layer (slow)"))->Check(cemuLog_isLoggingEnabled(LogType::VulkanValidation));
	debugLoggingMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_ADVANCED_PPC_INFO, _("&Log PPC context for API"))->Check(cemuLog_advancedPPCLoggingEnabled());
	m_loggingSubmenu = debugLoggingMenu;
	// debug->dump submenu
	wxMenu* debugDumpMenu = new wxMenu;
	debugDumpMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_DUMP_TEXTURES, _("&Textures"))->Check(ActiveSettings::DumpTexturesEnabled());
	debugDumpMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_DUMP_SHADERS, _("&Shaders"))->Check(ActiveSettings::DumpShadersEnabled());
	debugDumpMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_DUMP_RECOMPILER_FUNCTIONS, _("&Recompiled functions"))->Check(ActiveSettings::DumpRecompilerFunctionsEnabled());
	debugDumpMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_DUMP_CURL_REQUESTS, _("&nlibcurl HTTP/HTTPS requests"));
	// debug submenu
	wxMenu* debugMenu = new wxMenu();
	m_debugMenu = debugMenu;
	debugMenu->AppendSubMenu(debugLoggingMenu, _("&Logging"));
	debugMenu->AppendSubMenu(debugDumpMenu, _("&Dump"));
	debugMenu->AppendSeparator();

	auto upsidedownItem = debugMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_RENDER_UPSIDE_DOWN, _("&Render upside-down"));
	upsidedownItem->Check(ActiveSettings::RenderUpsideDownEnabled());
	if(LaunchSettings::RenderUpsideDownEnabled().has_value())
		upsidedownItem->Enable(false);

	auto accurateBarriers = debugMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_VK_ACCURATE_BARRIERS, _("&Accurate barriers (Vulkan)"));
	accurateBarriers->Check(GetConfig().vk_accurate_barriers);

#ifdef ENABLE_METAL
	auto gpuCapture = debugMenu->Append(MAINFRAME_MENU_ID_DEBUG_GPU_CAPTURE, _("&GPU capture (Metal)"));
	gpuCapture->Enable(m_game_launched && WxRendererAdapters::IsFrameCaptureSupported());
#endif

	debugMenu->AppendSeparator();

#ifdef CEMU_DEBUG_ASSERT
	auto audioAuxOnly = debugMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_AUDIO_AUX_ONLY, _("&Audio AUX only"));
	audioAuxOnly->Check(ActiveSettings::AudioOutputOnlyAux());
#endif

	debugMenu->Append(MAINFRAME_MENU_ID_DEBUG_VIEW_LOGGING_WINDOW, _("&Open logging window"));
	m_gdbstub_toggle = debugMenu->AppendCheckItem(MAINFRAME_MENU_ID_DEBUG_TOGGLE_GDB_STUB, _("&Launch with GDB stub"));
	m_gdbstub_toggle->Check(WxDebuggerAdapters::IsGdbStubEnabled());
	m_gdbstub_toggle->Enable(!m_game_launched);

	debugMenu->Append(MAINFRAME_MENU_ID_DEBUG_VIEW_PPC_THREADS, _("&View PPC threads"));
	debugMenu->Append(MAINFRAME_MENU_ID_DEBUG_VIEW_PPC_DEBUGGER, _("&View PPC debugger"));
	debugMenu->Append(MAINFRAME_MENU_ID_DEBUG_VIEW_AUDIO_DEBUGGER, _("&View audio debugger"));
	debugMenu->Append(MAINFRAME_MENU_ID_DEBUG_VIEW_TEXTURE_RELATIONS, _("&View texture cache info"));
	debugMenu->Append(MAINFRAME_MENU_ID_DEBUG_DUMP_RAM, _("&Dump current RAM"));
	// debugMenu->Append(MAINFRAME_MENU_ID_DEBUG_DUMP_FST, _("&Dump WUD filesystem"))->Enable(false);

	m_menuBar->Append(debugMenu, _("&Debug"));
	// help menu
	wxMenu* helpMenu = new wxMenu();
	m_check_update_menu = helpMenu->Append(MAINFRAME_MENU_ID_HELP_UPDATE, _("&Check for updates"));
#if BOOST_OS_LINUX
	if (!std::getenv("APPIMAGE")) {
		m_check_update_menu->Enable(false);
	}
#elif BOOST_OS_BSD // BSD users must update from source so disable update checks
	m_check_update_menu->Enable(false);
#endif
	helpMenu->AppendSeparator();
	helpMenu->Append(MAINFRAME_MENU_ID_HELP_ABOUT, _("&About Cemu"));

	m_menuBar->Append(helpMenu, _("&Help"));

	SetMenuBar(m_menuBar);
	m_menu_visible = true;

	if (m_game_launched)
	{
		if (m_check_update_menu)
			m_check_update_menu->Enable(false);

		m_memorySearcherMenuItem->Enable(true);
		m_nfcMenu->Enable(MAINFRAME_MENU_ID_NFC_TOUCH_NFC_FILE, true);

		// these options cant be toggled after the renderer backend is initialized:
		m_loggingSubmenu->Enable(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::OpenGLLogging), false);
		m_loggingSubmenu->Enable(MAINFRAME_MENU_ID_DEBUG_LOGGING0 + stdx::to_underlying(LogType::VulkanValidation), false);

		UpdateNFCMenu();
	}

	// hide new menu in fullscreen
	if (IsFullScreen())
		SetMenuVisible(false);
}

void MainWindow::UpdateChildWindowTitleRunningState()
{
	const bool running = m_emulationController.IsTitleRunning();

	if(m_graphic_pack_window)
		m_graphic_pack_window->UpdateTitleRunning(running);
}

void MainWindow::RestoreSettingsAfterGameExited()
{
	RecreateMenu();
}

void MainWindow::UpdateSettingsAfterGameLaunch()
{
	m_update_available = {};
	RecreateMenu();
}

void MainWindow::OnGraphicWindowClose(wxCloseEvent& event)
{
	m_graphic_pack_window->Destroy();
	m_graphic_pack_window = nullptr;
}

void MainWindow::OnGraphicWindowOpen(wxTitleIdEvent& event)
{
	if (m_graphic_pack_window)
		return;
	m_graphic_pack_window = new GraphicPacksWindow2(
		this, event.GetTitleId(), m_emulationController, m_uiDispatcher, m_pathProvider);
	m_graphic_pack_window->Bind(wxEVT_CLOSE_WINDOW, &MainWindow::OnGraphicWindowClose, this);
	m_graphic_pack_window->Show(true);
}

void MainWindow::RequestGameListRefresh()
{
	cemu_assert_debug(wxIsMainThread());
	QueueEvent(new wxCommandEvent(wxEVT_REQUEST_GAMELIST_REFRESH));
}

void MainWindow::RequestLaunchGame(fs::path filePath, wxLaunchGameEvent::INITIATED_BY initiatedBy)
{
	cemu_assert_debug(wxIsMainThread());
	QueueEvent(new wxLaunchGameEvent(std::move(filePath), initiatedBy));
}

void MainWindow::RecreateCanvasForHost()
{
	cemu_assert_debug(wxIsMainThread());
	DestroyCanvas();
	CreateCanvas();
}

void MainWindow::HandlePpcProcessExit()
{
	// this is called from the emulated PPC thread, so queue an event instead of handling it directly
	const std::weak_ptr<std::atomic_bool> lifetime = m_applicationEventLifetime;
	(void)m_uiDispatcher->Queue([this, lifetime] {
		const auto alive = lifetime.lock();
		if (alive && alive->load(std::memory_order_acquire))
			QueueEvent(new wxCommandEvent(wxEVT_REQUEST_GAME_EXIT));
	});
}

bool MainWindow::ConfirmCemodPermissions(std::uint64_t titleId, std::string gameName,
	std::vector<Application::CemodPermissionRequest> requests)
{
	cemu_assert_debug(wxIsMainThread());
	if (requests.empty()) return true;
	if (gameName.empty()) gameName = fmt::format("Title {:016x}", titleId);

	CemodPermissionDialog dialog(this, wxString::FromUTF8(gameName), std::move(requests));
	m_active_cemod_permission_dialog = &dialog;
	const auto modalResult = dialog.ShowModal();
	m_active_cemod_permission_dialog = nullptr;
	if (modalResult != wxID_OK) return false;
	std::vector<Application::CemodPermissionDecision> decisions;
	for (const auto& selection : dialog.GetSelections())
		decisions.push_back({selection.principal, selection.requestedPermissions,
			selection.grantedPermissions});
	m_emulationController.SaveCemodPermissionDecisions(titleId, decisions);
	return true;
}

void MainWindow::OnRequestGameExit(wxCommandEvent& event)
{
	// if the title was launched via the command line we close Cemu and use the foreground title's exit status as Cemu's process return code (via CemuApp:OnExit)
	// this is useful for homebrew testing setups that use Cemu via CLI
	// otherwise the title was launched from the game list, so we just stop it and return to the game list instead of closing Cemu
	if (LaunchSettings::GetLoadFile() || LaunchSettings::GetLoadTitleID())
	{
		Close();
	}
	else
	{
		EndEmulation();
	}
}

bool MainWindow::FullscreenEnabled() const
{
	return LaunchSettings::FullscreenEnabled().value_or(GetWxGUIConfig().fullscreen);
}
