#pragma once

#include <wx/wx.h>
#include "wxCemuConfig.h"
#include "input/api/Controller.h"

#include <memory>

class HotkeyEntry;
class IWxUiDispatcher;
class WxMainWindowRegistry;
namespace Host { class IPathProvider; }

class HotkeySettings : public wxFrame
{
public:
	static void Init(std::shared_ptr<IWxUiDispatcher> uiDispatcher,
		std::shared_ptr<WxMainWindowRegistry> mainWindowRegistry,
		std::shared_ptr<Host::IPathProvider> pathProvider);

	static void CaptureInput(wxKeyEvent& event);
	static void CaptureInput(const ControllerState& currentState, const ControllerState& lastState);

	HotkeySettings(wxWindow* parent);
	~HotkeySettings();

private:
	inline static std::weak_ptr<IWxUiDispatcher> s_uiDispatcher;
	inline static std::weak_ptr<WxMainWindowRegistry> s_mainWindowRegistry;
	inline static std::weak_ptr<Host::IPathProvider> s_pathProvider;
	static void RunOnUi(std::function<void(class MainWindow&)> action);
	static std::unordered_map<sHotkeyCfg*, std::function<void(void)>> s_cfgHotkeyToFuncMap;
	inline static std::unordered_map<uint16, std::function<void(void)>> s_keyboardHotkeyToFuncMap{};
	inline static std::unordered_map<uint16, std::function<void(void)>> s_controllerHotkeyToFuncMap{};
	inline static auto& s_cfgHotkeys = GetWxGUIConfig().hotkeys;

	wxPanel* m_panel;
	wxFlexGridSizer* m_sizer;
	wxButton* m_activeInputButton{ nullptr };
	wxTimer* m_controllerTimer{ nullptr };
	const wxSize m_minButtonSize{ 250, 45 };
	const wxString m_disabledHotkeyText{ _("----") };
	const wxString m_editModeHotkeyText{ "" };

	std::vector<HotkeyEntry> m_hotkeys;
	std::weak_ptr<ControllerBase> m_activeController{};
	bool m_needToSave = false;

	/* helpers */
	void CreateColumnHeaders(void);
	void CreateHotkeyRow(const wxString& label, sHotkeyCfg& cfgHotkey);
	wxString To_wxString(uKeyboardHotkey hotkey);
	wxString To_wxString(ControllerHotkey_t hotkey);
	bool IsValidKeycodeUp(int keycode);
	bool SetActiveController(void);

	template<typename T>
	void FinalizeInput(wxButton* inputButton);

	template <typename T>
	void RestoreInputButton(void);

	/* events */
	void OnKeyboardHotkeyInputLeftClick(wxCommandEvent& event);
	void OnControllerHotkeyInputLeftClick(wxCommandEvent& event);
	void OnKeyboardHotkeyInputRightClick(wxMouseEvent& event);
	void OnControllerHotkeyInputRightClick(wxMouseEvent& event);
	void OnKeyUp(wxKeyEvent& event);
	void OnControllerTimer(wxTimerEvent& event);
};
