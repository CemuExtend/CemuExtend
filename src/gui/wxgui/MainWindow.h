#pragma once

#include <wx/wx.h>
#include <wx/dataview.h>
#include <wx/infobar.h>

#include "wxgui/PadViewFrame.h"

#include "config/XMLConfig.h"

#include "wxgui/LoggingWindow.h"
#include "wxgui/components/wxGameList.h"

#include <future>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include "application/EmulationController.h"
#include "frontend/CemuExtendFrontendBridge.h"
#include "host/contracts/HostContracts.h"

class DebugPPCThreadsWindow;
struct GameEntry;
class DiscordPresence;
class TitleManager;
class GraphicPacksWindow2;
class wxLaunchGameEvent;
class CemodPermissionDialog;
class WxWindowState;
class WxMainWindowRegistry;
class IWxUiDispatcher;
struct WxFrontendContext;
enum class WxFrontendErrorCategory;

wxDECLARE_EVENT(wxEVT_LAUNCH_GAME, wxLaunchGameEvent);
wxDECLARE_EVENT(wxEVT_SET_WINDOW_TITLE, wxCommandEvent);

class wxLaunchGameEvent : public wxCommandEvent
{
  public:
	enum class INITIATED_BY
	{
		MENU, // via file menu
		DRAG_AND_DROP,
		GAME_LIST,
		TITLE_MANAGER,
		COMMAND_LINE, // -g parameter
	};

	wxLaunchGameEvent(fs::path path, INITIATED_BY initiatedBy)
		: wxCommandEvent(wxEVT_LAUNCH_GAME), m_launchPath(path), m_initiatedBy(initiatedBy) {}

	[[nodiscard]] fs::path GetPath() const
	{
		return m_launchPath;
	}
	[[nodiscard]] INITIATED_BY GetInitiatedBy() const
	{
		return m_initiatedBy;
	}

	wxEvent* Clone() const
	{
		return new wxLaunchGameEvent(*this);
	}

  private:
	fs::path m_launchPath;
	INITIATED_BY m_initiatedBy;
};

class MainWindow : public wxFrame
{
	friend class CemuApp;

  public:
	explicit MainWindow(Application::EmulationController& emulationController,
						std::shared_ptr<WxFrontendContext> frontendContext);
	~MainWindow();

	void CreateGameListAndStatusBar();
	void DestroyGameListAndStatusBar();

	void UpdateSettingsAfterGameLaunch();
	void RestoreSettingsAfterGameExited();

	bool FileLoad(const fs::path launchPath, wxLaunchGameEvent::INITIATED_BY initiatedBy);

	[[nodiscard]] bool IsGameLaunched() const
	{
		return m_game_launched;
	}

	void SetFullScreen(bool state);
	void EndEmulation();
	void SetMenuVisible(bool state);
	void UpdateNFCMenu();
	bool TouchNfcFile(std::string path, bool dropTarget);
	bool IsMenuHidden() const;
	void TogglePadView();

#if BOOST_OS_WINDOWS
	WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif
	void OpenSettings();

	PadViewFrame* GetPadView() const
	{
		return m_padView;
	}

	void OnSizeEvent(wxSizeEvent& event);
	void OnDPIChangedEvent(wxDPIChangedEvent& event);
	void OnMove(wxMoveEvent& event);

	void OnDebuggerClose(wxCloseEvent& event);
	void OnPadClose(wxCloseEvent& event);
	void CloseMemorySearcher();
	void OnMouseWheel(wxMouseEvent& event);
	void OnClose(wxCloseEvent& event);
	void OnFileMenu(wxCommandEvent& event);
	void OnOpenFolder(wxCommandEvent& event);
	void OnClearSpotPassCache(wxCommandEvent& event);
	void OnLaunchFromFile(wxLaunchGameEvent& event);
	void OnInstallUpdate(wxCommandEvent& event);
	void OnFileExit(wxCommandEvent& event);
	void OnNFCMenu(wxCommandEvent& event);
	void OnOptionsInput(wxCommandEvent& event);
	void OnAccountSelect(wxCommandEvent& event);
	void OnConsoleLanguage(wxCommandEvent& event);
	void OnHelpAbout(wxCommandEvent& event);
	void OnHelpUpdate(wxCommandEvent& event);
	void OnDebugSetting(wxCommandEvent& event);
	void OnDebugLoggingToggleFlagGeneric(wxCommandEvent& event);
	void OnPPCInfoToggle(wxCommandEvent& event);
	void OnDebugDumpGeneric(wxCommandEvent& event);
	void OnLoggingWindow(wxCommandEvent& event);
	void OnGDBStubToggle(wxCommandEvent& event);
	void OnDebugViewPPCThreads(wxCommandEvent& event);
	void OnDebugViewPPCDebugger(wxCommandEvent& event);
	void OnDebugViewAudioDebugger(wxCommandEvent& event);
	void OnDebugViewTextureRelations(wxCommandEvent& event);
	void OnMouseMove(wxMouseEvent& event);
	void OnMouseLeft(wxMouseEvent& event);
	void OnMouseRight(wxMouseEvent& event);
	void OnMouseMiddle(wxMouseEvent& event);
	void OnMouseAux(wxMouseEvent& event);
	void OnGameListBeginUpdate(wxCommandEvent& event);
	void OnGameListEndUpdate(wxCommandEvent& event);
	void OnAccountListRefresh(wxCommandEvent& event);
	void OnRequestGameListRefresh(wxCommandEvent& event);
	void OnSetWindowTitle(wxCommandEvent& event);

	void OnKeyUp(wxKeyEvent& event);
	void OnKeyDown(wxKeyEvent& event);
	void OnChar(wxKeyEvent& event);

	void OnToolsInput(wxCommandEvent& event);
	void OnGesturePan(wxPanGestureEvent& event);

	void OnGameLoaded();

	void AsyncSetTitle(std::string_view windowTitle);
	void RefreshCemuExtendTextInput();
	bool IsCemuExtendTextInputEvent(const wxEvent& event) const;
	bool CanSubmitCemuExtendTextInput() const;

	void CreateCanvas();
	void DestroyCanvas();

	static void ShowCursor(bool state);

	uintptr_t GetRenderCanvasHWND();
	void RequestGameListRefresh();
	void RequestLaunchGame(fs::path filePath,
						   wxLaunchGameEvent::INITIATED_BY initiatedBy);

	// Frontend host entry point for renderer canvas recreation.
	void RecreateCanvasForHost();
	void HandlePpcProcessExit();

  private:
	bool FullscreenEnabled() const;
	void RecreateMenu();
	void UpdateChildWindowTitleRunningState();
	static wxString GetInitialWindowTitle();

	bool InstallUpdate(const fs::path& metaFilePath);
	bool ConfirmCemodPermissions(std::uint64_t titleId, std::string gameName,
								 std::vector<Application::CemodPermissionRequest> requests);
	void RollbackFailedLaunchUi();
	void ClosePpcThreadsViewer();

	void OnTimer(wxTimerEvent& event);
	void OnCemuExtendTextChanged(wxCommandEvent& event);
	void OnCemuExtendTextPreedit(std::string_view preedit);
	void PublishCemuExtendTextComposition();
	void EnsureCemuExtendTextInputFocus(std::uint64_t sequence);
	bool HasCemuExtendTextInputNativeFocus() const;
	void EmitCemuExtendMouseEvent(wxMouseEvent& event, std::int32_t wheelX = 0,
								  std::int32_t wheelY = 0, std::uint32_t changedButtons = 0);
	void EmitCemuExtendRawMouseEvent(std::int32_t deltaX, std::int32_t deltaY,
									 std::uint32_t changedButtons = 0);
	bool EnsureCemuExtendRawMouse();
	void UpdateCemuExtendPointerConfinement(bool confine);
	bool ApplyCemuExtendPointerPolicy();

	void OnRequestGameExit(wxCommandEvent& event);
	void HandleApplicationEvent(const Application::Event& event);

	wxRect GetDesktopRect();

	wxWindow* m_toolWindow = nullptr;
	TitleManager* m_title_manager = nullptr;
	wxWindow* m_usb_devices = nullptr;
	PadViewFrame* m_padView = nullptr;
	GraphicPacksWindow2* m_graphic_pack_window = nullptr;
	CemodPermissionDialog* m_active_cemod_permission_dialog{};

	wxTimer* m_timer;
	Application::EmulationController& m_emulationController;
	std::shared_ptr<WxFrontendContext> m_frontendContext;
	std::shared_ptr<Host::IWindowMetrics> m_windowMetrics;
	std::shared_ptr<Host::IPathProvider> m_pathProvider;
	std::shared_ptr<Host::INativeSurfaceProvider> m_nativeSurfaces;
	std::shared_ptr<Host::INativeSurfacePublisher> m_nativeSurfacePublisher;
	std::shared_ptr<Host::IKeyboardState> m_keyboardState;
	std::shared_ptr<Host::IInputHostEvents> m_inputHostEvents;
	std::shared_ptr<WxWindowState> m_windowState;
	std::shared_ptr<WxMainWindowRegistry> m_mainWindowRegistry;
	std::shared_ptr<IWxUiDispatcher> m_uiDispatcher;
	std::function<void(std::string_view, std::string_view,
					   std::optional<WxFrontendErrorCategory>)>
		m_showErrorDialog;
	std::function<void(bool, bool, double,
					   std::optional<Application::WindowTitlePresentation>)>
		m_updateWindowTitles;
	Host::NativeWindowHandle m_nativeWindowHandle;
	Host::NativeSurfacePublication m_nativeWindowPublication{};
	Application::EventSubscription m_applicationEventSubscription;
	std::shared_ptr<std::atomic_bool> m_applicationEventLifetime;
	wxPoint m_mouse_position{};
	std::chrono::steady_clock::time_point m_last_mouse_move_time;
	Frontend::CemuExtendFrontendBridge m_cemuextend_bridge;
	bool m_cemuextend_raw_mouse_registered{};
	bool m_cemuextend_pointer_confined{};
	wxSize m_restored_size;
	wxPoint m_restored_position;

	bool m_menu_visible = false;
	bool m_game_launched = false;

#ifdef ENABLE_DISCORD_RPC
	std::unique_ptr<DiscordPresence> m_discord;
#endif

	std::string m_launched_game_name;

	wxMenuItem* m_gdbstub_toggle{};
	wxWindow* m_debugger_window = nullptr;
	DebugPPCThreadsWindow* m_ppc_threads_window = nullptr;
	LoggingWindow* m_logging_window = nullptr;

	std::future<bool> m_update_available;

	wxMenuItem* m_check_update_menu{};
	void LoadSettings();
	void SaveSettings();

	void OnGraphicWindowClose(wxCloseEvent& event);
	void OnGraphicWindowOpen(wxTitleIdEvent& event);

	// panels
	wxPanel *m_main_panel{}, *m_game_panel{};
	wxTextCtrl* m_cemuextend_text_input{};
	bool m_cemuextend_text_input_updating{};
	unsigned m_cemuextend_text_input_focus_retries{};
	bool m_cemuextend_text_input_focus_logged{};
	bool m_cemuextend_text_input_focus_failure_logged{};
	bool m_cemuextend_text_input_preedit_logged{};

	// rendering
	wxWindow* m_render_canvas{};

	// gamelist
	wxGameList* m_game_list{};
	wxInfoBar* m_info_bar{};

	// menu
	wxMenuBar* m_menuBar{};

	// file
	wxMenu* m_fileMenu{};
	wxMenuItem* m_fileMenuSeparator0{};
	wxMenuItem* m_fileMenuSeparator1{};
	wxMenuItem* m_loadMenuItem{};
	wxMenuItem* m_installUpdateMenuItem{};
	wxMenuItem* m_exitMenuItem{};

	// options
	wxMenu* m_optionsAccountMenu{};

	wxMenuItem* m_fullscreenMenuItem{};
	wxMenuItem* m_padViewMenuItem{};

	// tools
	wxMenuItem* m_memorySearcherMenuItem{};

	// cpu
	wxMenu* m_cpuTimerSubmenu{};

	// nfc
	wxMenu* m_nfcMenu{};
	wxMenuItem* m_nfcMenuSeparator0{};

	// debug
	wxMenu* m_debugMenu{};
	wxMenu* m_loggingSubmenu{};
	wxMenuItem* m_asyncCompile{};

	wxDECLARE_EVENT_TABLE();
};
