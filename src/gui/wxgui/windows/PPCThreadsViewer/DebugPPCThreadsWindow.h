#pragma once

#include <cstdint>
#include <memory>
#include <thread>

#include <wx/wx.h>

struct ThreadProfileState;
class wxGenericProgressDialog;
class wxListView;

class DebugPPCThreadsWindow: public wxFrame
{
public:
	DebugPPCThreadsWindow(wxFrame& parent);
	~DebugPPCThreadsWindow();

	void OnCloseButton(wxCommandEvent& event);
	void OnRefreshButton(wxCommandEvent& event);
	void OnClose(wxCloseEvent& event);
	void RefreshThreadList();
	void OnThreadListPopupClick(wxCommandEvent &evt);
	void OnThreadListRightClick(wxMouseEvent& event);

	void Close();

private:
	void ProfileThread(std::uint32_t threadAddress);
	void StopProfile();
	void UpdateProfileProgress();
	void DumpStackTrace(std::uint32_t threadAddress);

	wxListView* m_thread_list;
	wxCheckBox* m_auto_refresh;
	wxTimer* m_timer;
	wxGenericProgressDialog* m_profile_dialog{};
	std::shared_ptr<ThreadProfileState> m_profile_state;
	std::jthread m_profile_thread;

	void OnTimer(wxTimerEvent& event);

	wxDECLARE_EVENT_TABLE();


};
