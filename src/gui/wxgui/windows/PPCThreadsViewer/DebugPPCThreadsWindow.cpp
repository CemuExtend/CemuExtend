#include "wxgui/wxgui.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "DebugPPCThreadsWindow.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"

#include <atomic>
#include <cinttypes>
#include <helpers/wxHelpers.h>
#include <wx/listctrl.h>
#include <wx/progdlg.h>

struct ThreadProfileState
{
	std::atomic_bool cancel{};
	std::atomic_bool finished{};
	std::atomic<std::uint32_t> sampleCount{};
};

enum
{
	// options
	REFRESH_ID = wxID_HIGHEST + 1,
	AUTO_REFRESH_ID,
	CLOSE_ID,
	GPLIST_ID,

	// list context menu options
	THREADLIST_MENU_BOOST_PRIO_1,
	THREADLIST_MENU_BOOST_PRIO_5,
	THREADLIST_MENU_DECREASE_PRIO_1,
	THREADLIST_MENU_DECREASE_PRIO_5,
	THREADLIST_MENU_SUSPEND,
	THREADLIST_MENU_RESUME,
	THREADLIST_MENU_DUMP_STACK_TRACE,
	THREADLIST_MENU_PROFILE_THREAD,
};

wxBEGIN_EVENT_TABLE(DebugPPCThreadsWindow, wxFrame)
	EVT_BUTTON(CLOSE_ID, DebugPPCThreadsWindow::OnCloseButton)
		EVT_BUTTON(REFRESH_ID, DebugPPCThreadsWindow::OnRefreshButton)
			EVT_CLOSE(DebugPPCThreadsWindow::OnClose)
				wxEND_EVENT_TABLE()

					DebugPPCThreadsWindow::DebugPPCThreadsWindow(wxFrame& parent)
	: wxFrame(&parent, wxID_ANY, _("PPC threads"), wxDefaultPosition, wxSize(930, 280),
			  wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION | wxRESIZE_BORDER)
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	m_thread_list = new wxListView(this, GPLIST_ID, wxPoint(0, 0), wxSize(930, 240), wxLC_REPORT);

	m_thread_list->SetFont(wxFont(8, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Courier New")); // wxSystemSettings::GetFont(wxSYS_OEM_FIXED_FONT));

	// add columns
	wxListItem col0;
	col0.SetId(0);
	col0.SetText("Address");
	col0.SetWidth(75);
	m_thread_list->InsertColumn(0, col0);
	wxListItem col1;
	col1.SetId(1);
	col1.SetText("Entry");
	col1.SetWidth(75);
	m_thread_list->InsertColumn(1, col1);
	wxListItem col2;
	col2.SetId(2);
	col2.SetText("Stack");
	col2.SetWidth(145);
	m_thread_list->InsertColumn(2, col2);
	wxListItem col3;
	col3.SetId(3);
	col3.SetText("PC");
	col3.SetWidth(120);
	m_thread_list->InsertColumn(3, col3);
	wxListItem colLR;
	colLR.SetId(4);
	colLR.SetText("LR");
	colLR.SetWidth(75);
	m_thread_list->InsertColumn(4, colLR);
	wxListItem col4;
	col4.SetId(5);
	col4.SetText("State");
	col4.SetWidth(90);
	m_thread_list->InsertColumn(5, col4);
	wxListItem col5;
	col5.SetId(6);
	col5.SetText("Affinity");
	col5.SetWidth(70);
	m_thread_list->InsertColumn(6, col5);
	wxListItem colPriority;
	colPriority.SetId(7);
	colPriority.SetText("Priority");
	colPriority.SetWidth(80);
	m_thread_list->InsertColumn(7, colPriority);
	wxListItem col6;
	col6.SetId(8);
	col6.SetText("SliceStart");
	col6.SetWidth(110);
	m_thread_list->InsertColumn(8, col6);
	wxListItem col7;
	col7.SetId(9);
	col7.SetText("SumWakeTime");
	col7.SetWidth(110);
	m_thread_list->InsertColumn(9, col7);
	wxListItem col8;
	col8.SetId(10);
	col8.SetText("ThreadName");
	col8.SetWidth(180);
	m_thread_list->InsertColumn(10, col8);
	wxListItem col9;
	col9.SetId(11);
	col9.SetText("GPR");
	col9.SetWidth(180);
	m_thread_list->InsertColumn(11, col9);
	wxListItem col10;
	col10.SetId(12);
	col10.SetText("Extra info");
	col10.SetWidth(180);
	m_thread_list->InsertColumn(12, col10);

	sizer->Add(m_thread_list, 1, wxEXPAND | wxALL, 5);

	auto* row = new wxBoxSizer(wxHORIZONTAL);
	wxButton* button = new wxButton(this, REFRESH_ID, _("Refresh"), wxPoint(0, 0), wxSize(80, 26));
	row->Add(button, 0, wxALL, 5);

	m_auto_refresh = new wxCheckBox(this, AUTO_REFRESH_ID, _("Auto refresh"));
	m_auto_refresh->SetValue(true);
	row->Add(m_auto_refresh, 0, wxEXPAND | wxALL, 5);

	sizer->Add(row, 0, wxEXPAND | wxALL, 5);

	m_thread_list->Bind(wxEVT_RIGHT_DOWN, &DebugPPCThreadsWindow::OnThreadListRightClick, this);

	SetSizer(sizer);

	RefreshThreadList();

	m_timer = new wxTimer(this);
	this->Bind(wxEVT_TIMER, &DebugPPCThreadsWindow::OnTimer, this);
	m_timer->Start(250);
}

DebugPPCThreadsWindow::~DebugPPCThreadsWindow()
{
	m_timer->Stop();
	StopProfile();
}

void DebugPPCThreadsWindow::OnCloseButton(wxCommandEvent& event)
{
	Close();
}

void DebugPPCThreadsWindow::OnRefreshButton(wxCommandEvent& event)
{
	RefreshThreadList();
}

void DebugPPCThreadsWindow::OnClose(wxCloseEvent& event)
{
	Close();
}

void DebugPPCThreadsWindow::OnTimer(wxTimerEvent& event)
{
	UpdateProfileProgress();
	if (m_auto_refresh->IsChecked())
		RefreshThreadList();
}

#define _r(__idx) _swapEndianU32(cafeThread->context.gpr[__idx])

void DebugPPCThreadsWindow::RefreshThreadList()
{
	wxWindowUpdateLocker lock(m_thread_list);

	long selected_thread = 0;
	const int selection = m_thread_list->GetFirstSelected();
	if (selection != wxNOT_FOUND)
		selected_thread = m_thread_list->GetItemData(selection);

	const int scrollPos = m_thread_list->GetScrollPos(0);
	m_thread_list->DeleteAllItems();

	if (activeThreadCount > 0)
	{
		__OSLockScheduler();
		srwlock_activeThreadList.LockWrite();
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			MPTR threadItrMPTR = activeThread[i];
			OSThread_t* cafeThread = (OSThread_t*)memory_getPointerFromVirtualOffset(threadItrMPTR);

			wxListItem item;
			item.SetId(i);
			item.SetText(wxString::Format("%08X", threadItrMPTR));
			m_thread_list->InsertItem(item);
			m_thread_list->SetItemData(item, (long)threadItrMPTR);
			// entry point
			m_thread_list->SetItem(i, 1, wxString::Format("%08X", cafeThread->entrypoint.GetMPTR()));
			// stack base (low)
			m_thread_list->SetItem(i, 2, wxString::Format("%08X - %08X", cafeThread->stackEnd.GetMPTR(), cafeThread->stackBase.GetMPTR()));
			// pc
			RPLStoredSymbol* symbol = rplSymbolStorage_getByAddress(cafeThread->context.srr0);
			wxString pcLabel;
			if (symbol)
				pcLabel = wxString::Format("%s (0x%08x)", (const char*)symbol->symbolName, cafeThread->context.srr0);
			else
				pcLabel = wxString::Format("%08X", cafeThread->context.srr0);
			m_thread_list->SetItem(i, 3, pcLabel);
			// lr
			m_thread_list->SetItem(i, 4, wxString::Format("%08X", _swapEndianU32(cafeThread->context.lr)));
			// state
			OSThread_t::THREAD_STATE threadState = cafeThread->state;
			wxString threadStateStr = "UNDEFINED";
			if (cafeThread->suspendCounter != 0)
				threadStateStr = "SUSPENDED";
			else if (threadState == OSThread_t::THREAD_STATE::STATE_NONE)
				threadStateStr = "NONE";
			else if (threadState == OSThread_t::THREAD_STATE::STATE_READY)
				threadStateStr = "READY";
			else if (threadState == OSThread_t::THREAD_STATE::STATE_RUNNING)
				threadStateStr = "RUNNING";
			else if (threadState == OSThread_t::THREAD_STATE::STATE_WAITING)
				threadStateStr = "WAITING";
			else if (threadState == OSThread_t::THREAD_STATE::STATE_MORIBUND)
				threadStateStr = "MORIBUND";
			m_thread_list->SetItem(i, 5, threadStateStr);
			// affinity
			uint8 affinity = cafeThread->attr & 7;
			uint8 affinityReal = cafeThread->context.affinity;
			wxString affinityLabel;
			if (affinity != affinityReal)
				affinityLabel = wxString::Format("(!) %d%d%d real: %d%d%d", (affinity >> 0) & 1, (affinity >> 1) & 1, (affinity >> 2) & 1, (affinityReal >> 0) & 1, (affinityReal >> 1) & 1, (affinityReal >> 2) & 1);
			else
				affinityLabel = wxString::Format("%d%d%d", (affinity >> 0) & 1, (affinity >> 1) & 1, (affinity >> 2) & 1);
			m_thread_list->SetItem(i, 6, affinityLabel);
			// priority
			sint32 effectivePriority = cafeThread->effectivePriority;
			m_thread_list->SetItem(i, 7, wxString::Format("%d", effectivePriority));
			// last awake in cycles
			uint64 lastWakeUpTime = cafeThread->wakeUpTime;
			m_thread_list->SetItem(i, 8, wxString::Format("%" PRIu64, lastWakeUpTime));
			// awake time in cycles
			uint64 awakeTime = cafeThread->totalCycles;
			m_thread_list->SetItem(i, 9, wxString::Format("%" PRIu64, awakeTime));
			// thread name
			const char* threadName = "NULL";
			if (!cafeThread->threadName.IsNull())
				threadName = cafeThread->threadName.GetPtr();
			m_thread_list->SetItem(i, 10, threadName);
			// GPR
			m_thread_list->SetItem(i, 11, wxString::Format("r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x", _r(3), _r(4), _r(5), _r(6), _r(7)));
			// waiting condition / extra info
			coreinit::OSMutex* mutex = cafeThread->waitingForMutex;
			wxString extraInfoLabel;
			if (mutex)
				extraInfoLabel = wxString::Format("Mutex 0x%08x (Held by thread 0x%08X Lock-Count: %d)", memory_getVirtualOffsetFromPointer(mutex), mutex->owner.GetMPTR(), (uint32)mutex->lockCount);

			// OSSetThreadCancelState
			if (cafeThread->requestFlags & OSThread_t::REQUEST_FLAG_CANCEL)
				extraInfoLabel += "[Cancel requested]";

			m_thread_list->SetItem(i, 12, extraInfoLabel);

			if (selected_thread != 0 && selected_thread == (long)threadItrMPTR)
			{
				m_thread_list->Select(i);
				m_thread_list->Focus(i);
			}
		}
		srwlock_activeThreadList.UnlockWrite();
		__OSUnlockScheduler();
	}

	m_thread_list->SetScrollPos(0, scrollPos, true);
}

void DebugPPCThreadsWindow::DumpStackTrace(std::uint32_t threadAddress)
{
	auto* thread = reinterpret_cast<OSThread_t*>(memory_getPointerFromVirtualOffset(threadAddress));
	cemuLog_log(LogType::Force, "Dumping stack trace for thread {0:08x} LR: {1:08x}", memory_getVirtualOffsetFromPointer(thread), _swapEndianU32(thread->context.lr));
	DebugLogStackTrace(thread, _swapEndianU32(thread->context.gpr[1]));
}

namespace
{
	void PresentProfileResults(std::uint32_t threadAddress, const std::unordered_map<std::uint32_t, std::uint32_t>& samples)
	{
		std::vector<std::pair<std::uint32_t, std::uint32_t>> sortedSamples;
		std::uint32_t totalSampleCount = 0;
		for (const auto& sample : samples)
			totalSampleCount += sample.second;
		cemuLog_log(LogType::Force, "--- Thread {:08x} profile results with {:} samples captured ---",
					threadAddress, totalSampleCount);
		if (totalSampleCount == 0)
			return;

		cemuLog_log(LogType::Force, "Exclusive time, grouped by function:");
		for (const auto& sample : samples)
		{
			RPLStoredSymbol* symbol = rplSymbolStorage_getByClosestAddress(sample.first);
			std::uint32_t sampleAddr = sample.first;
			if (symbol)
				sampleAddr = symbol->address;
			auto it = std::find_if(sortedSamples.begin(), sortedSamples.end(),
								   [sampleAddr](const std::pair<std::uint32_t, std::uint32_t>& entry) { return entry.first == sampleAddr; });
			if (it != sortedSamples.end())
				it->second += sample.second;
			else
				sortedSamples.emplace_back(sampleAddr, sample.second);
		}
		std::sort(sortedSamples.begin(), sortedSamples.end(),
				  [](const auto& left, const auto& right) { return left.second > right.second; });
		for (const auto& sample : sortedSamples)
		{
			if (sample.second < 3)
				continue;
			RPLStoredSymbol* symbol = rplSymbolStorage_getByClosestAddress(sample.first);
			std::string symbolName = "Unknown";
			if (symbol)
			{
				symbolName = fmt::format("{}.{}+0x{:x}", (const char*)symbol->libName,
										 (const char*)symbol->symbolName, sample.first - symbol->address);
			}
			cemuLog_log(LogType::Force, "[{:08x}] {:8.2f}% (Samples: {:5}) Symbol: {}", sample.first,
						(double)(sample.second * 100) / (double)totalSampleCount, sample.second, symbolName);
		}
	}

	bool SampleThreadInstructionPointer(const std::shared_ptr<ThreadProfileState>& state,
										std::uint32_t threadAddress, std::uint32_t& instructionPointer)
	{
		auto* thread = reinterpret_cast<OSThread_t*>(memory_getPointerFromVirtualOffset(threadAddress));
		__OSLockScheduler();
		if (!coreinit::__OSIsThreadActive(thread) ||
			thread->state == OSThread_t::THREAD_STATE::STATE_NONE ||
			thread->state == OSThread_t::THREAD_STATE::STATE_MORIBUND)
		{
			__OSUnlockScheduler();
			return false;
		}

		coreinit::__OSSuspendThreadNolock(thread);
		while (coreinit::OSIsThreadRunningNoLock(thread))
		{
			__OSUnlockScheduler();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			__OSLockScheduler();
			if (!coreinit::__OSIsThreadActive(thread))
			{
				// Forced teardown can remove a thread while the profiler's suspend
				// request is still counted. Restore that increment before exiting.
				if (thread->suspendCounter > 0)
					thread->suspendCounter--;
				__OSUnlockScheduler();
				return false;
			}
			if (state->cancel.load(std::memory_order_acquire) && coreinit::OSIsThreadRunningNoLock(thread))
			{
				// Undo exactly the suspend requested by the profiler.  A running
				// thread is not on a ready queue, so decrement directly instead of
				// adding it to one through __OSResumeThreadInternal().
				cemu_assert_debug(thread->suspendCounter > 0);
				thread->suspendCounter--;
				__OSUnlockScheduler();
				return false;
			}
		}
		instructionPointer = thread->context.srr0;
		coreinit::__OSResumeThreadInternal(thread, 1);
		__OSUnlockScheduler();
		return true;
	}

	void ProfileThreadWorker(const std::shared_ptr<ThreadProfileState>& state, std::uint32_t threadAddress)
	{
		try
		{
			std::unordered_map<std::uint32_t, std::uint32_t> samples;
			while (!state->cancel.load(std::memory_order_acquire))
			{
				std::uint32_t instructionPointer{};
				if (!SampleThreadInstructionPointer(state, threadAddress, instructionPointer))
					break;
				samples[instructionPointer]++;
				state->sampleCount.fetch_add(1, std::memory_order_relaxed);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (!samples.empty())
				PresentProfileResults(threadAddress, samples);
		} catch (const std::exception& exception)
		{
			cemuLog_log(LogType::Force, "PPC thread profiler failed: {}", exception.what());
		} catch (...)
		{
			cemuLog_log(LogType::Force, "PPC thread profiler failed with an unknown error");
		}
		state->finished.store(true, std::memory_order_release);
	}
} // namespace

void DebugPPCThreadsWindow::ProfileThread(std::uint32_t threadAddress)
{
	if (m_profile_state)
		return;

	m_profile_state = std::make_shared<ThreadProfileState>();
	m_profile_dialog = new wxGenericProgressDialog(_("Profiling thread"), _("Capturing samples..."),
												   1000, this, wxPD_CAN_SKIP);
	const auto state = m_profile_state;
	m_profile_thread = std::jthread([state, threadAddress] {
		ProfileThreadWorker(state, threadAddress);
	});
}

void DebugPPCThreadsWindow::UpdateProfileProgress()
{
	if (!m_profile_state)
		return;

	const auto sampleCount = m_profile_state->sampleCount.load(std::memory_order_relaxed);
	if (m_profile_dialog && !m_profile_state->finished.load(std::memory_order_acquire))
	{
		wxString message = formatWxString(_("Capturing samples... ({:})\nResults will be written to log.txt\n"), sampleCount);
		if (sampleCount < 30000)
			message.Append(_("Click Skip button for early results with lower accuracy"));
		else
			message.Append(_("Click Skip button to finish"));
		bool skipped = false;
		const auto progress = std::min<std::uint32_t>(sampleCount * 1000 / 30000, 999);
		if (!m_profile_dialog->Update(progress, message, &skipped) || skipped)
			m_profile_state->cancel.store(true, std::memory_order_release);
	}

	if (m_profile_state->finished.load(std::memory_order_acquire))
		StopProfile();
}

void DebugPPCThreadsWindow::StopProfile()
{
	if (m_profile_state)
		m_profile_state->cancel.store(true, std::memory_order_release);
	if (m_profile_thread.joinable())
	{
		m_profile_thread.request_stop();
		m_profile_thread.join();
	}
	if (m_profile_dialog)
	{
		m_profile_dialog->Destroy();
		m_profile_dialog = nullptr;
	}
	m_profile_state.reset();
}

void DebugPPCThreadsWindow::OnThreadListPopupClick(wxCommandEvent& evt)
{
	MPTR threadMPTR = (MPTR)(size_t)static_cast<wxMenu*>(evt.GetEventObject())->GetClientData();
	auto* osThread = reinterpret_cast<OSThread_t*>(memory_getPointerFromVirtualOffset(threadMPTR));
	__OSLockScheduler();
	if (!coreinit::__OSIsThreadActive(osThread))
	{
		__OSUnlockScheduler();
		return;
	}

	bool priorityChanged = false;
	switch (evt.GetId())
	{
	case THREADLIST_MENU_BOOST_PRIO_5:
		osThread->basePriority = osThread->basePriority - 5;
		priorityChanged = true;
		break;
	case THREADLIST_MENU_BOOST_PRIO_1:
		osThread->basePriority = osThread->basePriority - 1;
		priorityChanged = true;
		break;
	case THREADLIST_MENU_DECREASE_PRIO_5:
		osThread->basePriority = osThread->basePriority + 5;
		priorityChanged = true;
		break;
	case THREADLIST_MENU_DECREASE_PRIO_1:
		osThread->basePriority = osThread->basePriority + 1;
		priorityChanged = true;
		break;
	case THREADLIST_MENU_SUSPEND:
		coreinit::__OSSuspendThreadNolock(osThread);
		break;
	case THREADLIST_MENU_RESUME:
		coreinit::__OSResumeThreadInternal(osThread, 1);
		break;
	case THREADLIST_MENU_DUMP_STACK_TRACE:
		DumpStackTrace(threadMPTR);
		break;
	case THREADLIST_MENU_PROFILE_THREAD:
		__OSUnlockScheduler();
		ProfileThread(threadMPTR);
		RefreshThreadList();
		return;
	}
	if (priorityChanged)
		coreinit::__OSUpdateThreadEffectivePriority(osThread);
	__OSUnlockScheduler();
	// update thread list
	RefreshThreadList();
}

void DebugPPCThreadsWindow::OnThreadListRightClick(wxMouseEvent& event)
{
	// Get the item index
	int hitTestFlag;
	int itemIndex = m_thread_list->HitTest(event.GetPosition(), hitTestFlag);
	if (itemIndex == wxNOT_FOUND)
		return;
	// select item
	m_thread_list->Focus(itemIndex);
	long sel = m_thread_list->GetFirstSelected();
	if (sel != wxNOT_FOUND)
		m_thread_list->Select(sel, false);
	m_thread_list->Select(itemIndex);
	// check if thread is still on the list of active threads
	MPTR threadMPTR = (MPTR)m_thread_list->GetItemData(itemIndex);
	__OSLockScheduler();
	if (!coreinit::__OSIsThreadActive(MEMPTR<OSThread_t>(threadMPTR)))
	{
		__OSUnlockScheduler();
		return;
	}
	__OSUnlockScheduler();
	// create menu entry
	wxMenu menu;
	menu.SetClientData((void*)(size_t)threadMPTR);
	menu.Append(THREADLIST_MENU_BOOST_PRIO_5, _("Boost priority (-5)"));
	menu.Append(THREADLIST_MENU_BOOST_PRIO_1, _("Boost priority (-1)"));
	menu.AppendSeparator();
	menu.Append(THREADLIST_MENU_DECREASE_PRIO_5, _("Decrease priority (+5)"));
	menu.Append(THREADLIST_MENU_DECREASE_PRIO_1, _("Decrease priority (+1)"));
	menu.AppendSeparator();
	menu.Append(THREADLIST_MENU_RESUME, _("Resume"));
	menu.Append(THREADLIST_MENU_SUSPEND, _("Suspend"));
	menu.AppendSeparator();
	menu.Append(THREADLIST_MENU_DUMP_STACK_TRACE, _("Write stack trace to log"));
	menu.Append(THREADLIST_MENU_PROFILE_THREAD, _("Profile thread"));
	menu.Bind(wxEVT_COMMAND_MENU_SELECTED, &DebugPPCThreadsWindow::OnThreadListPopupClick, this);
	PopupMenu(&menu);
}

void DebugPPCThreadsWindow::Close()
{
	StopProfile();
	this->Destroy();
}
