#include "wxgui/wxgui.h"
#include "wxgui/GameUpdateWindow.h"

#include "application/EmulationController.h"
#include "wxgui/helpers/wxHelpers.h"

namespace
{
	wxString GetTitleKindString(Application::TitleInstallKind kind)
	{
		switch (kind)
		{
		case Application::TitleInstallKind::Dlc: return _("DLC");
		case Application::TitleInstallKind::Base: return _("Base game");
		case Application::TitleInstallKind::Demo: return _("Demo");
		case Application::TitleInstallKind::SystemTitle: return _("System title");
		case Application::TitleInstallKind::SystemData: return _("System data title");
		case Application::TitleInstallKind::Update: return _("Update");
		default: return _("Unknown");
		}
	}
}

GameUpdateWindow::GameUpdateWindow(wxWindow& parent,
	Application::EmulationController& emulationController, const fs::path& sourcePath)
	: wxDialog(&parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
		wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_emulationController(emulationController)
{
	const auto planned = m_emulationController.PlanTitleInstall(sourcePath);
	if (!planned)
		throw std::runtime_error(planned.diagnostic);
	m_plan = *planned.plan;

	if (m_plan.conflict != Application::TitleInstallConflict::None)
	{
		wxString message;
		switch (m_plan.conflict)
		{
		case Application::TitleInstallConflict::DifferentType:
			message = formatWxString(_("It seems that there is already a title installed at the target location but it has a different type.\nCurrently installed: '{}' Installing: '{}'\n\nThis can happen for titles which were installed with very old Cemu versions.\nDo you still want to continue with the installation? It will replace the currently installed title."),
				GetTitleKindString(m_plan.installed.kind), GetTitleKindString(m_plan.kind));
			break;
		case Application::TitleInstallConflict::SameVersion:
			message = _("It seems that the selected title is already installed, do you want to reinstall it?");
			break;
		case Application::TitleInstallConflict::NewerVersionInstalled:
			message = _("It seems that a newer version is already installed, do you still want to install the older version?");
			break;
		default:
			break;
		}
		wxMessageDialog dialog(this, message, _("Warning"),
			wxCENTRE | wxYES_NO | wxICON_EXCLAMATION);
		if (dialog.ShowModal() != wxID_YES)
			throw AbortException();
		m_decision = Application::TitleInstallDecision::AcceptConflict;
	}

	switch (m_plan.kind)
	{
	case Application::TitleInstallKind::Dlc: SetTitle(_("Installing DLC...")); break;
	case Application::TitleInstallKind::Update: SetTitle(_("Installing update...")); break;
	case Application::TitleInstallKind::SystemTitle:
	case Application::TitleInstallKind::SystemData:
		SetTitle(_("Installing system title...")); break;
	default: SetTitle(_("Installing title...")); break;
	}

	auto* sizer = new wxBoxSizer(wxVERTICAL);
	m_processBar = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition,
		wxSize(500, 20), wxGA_HORIZONTAL);
	m_processBar->SetValue(0);
	sizer->Add(m_processBar, 0, wxALL | wxEXPAND, 5);

	auto* cancelButton = new wxButton(this, wxID_ANY, _("Cancel"));
	cancelButton->Bind(wxEVT_BUTTON, &GameUpdateWindow::OnCancelButton, this);
	sizer->Add(cancelButton, 0, wxALIGN_RIGHT | wxALL, 5);

	SetSizerAndFit(sizer);
	Centre(wxBOTH);

	m_timer = new wxTimer(this);
	Bind(wxEVT_TIMER, &GameUpdateWindow::OnUpdate, this);
	Bind(wxEVT_CLOSE_WINDOW, &GameUpdateWindow::OnClose, this);
	m_timer->Start(250);

	m_totalBytes = m_plan.requiredBytes;
	m_thread = std::thread(&GameUpdateWindow::ThreadWork, this);
}

GameUpdateWindow::~GameUpdateWindow()
{
	if (m_timer)
		m_timer->Stop();
	m_cancelRequested = true;
	if (m_thread.joinable())
		m_thread.join();
}

void GameUpdateWindow::ThreadWork()
{
	try
	{
		m_result = m_emulationController.InstallTitle(m_plan, m_decision,
			[this](const Application::TitleInstallProgress& progress) {
				m_processedBytes.store(progress.bytesCompleted, std::memory_order_relaxed);
				m_totalBytes.store(progress.bytesTotal, std::memory_order_relaxed);
			}, [this] { return m_cancelRequested.load(std::memory_order_relaxed); });
	}
	catch (const std::exception& exception)
	{
		m_result = {Application::TitleInstallError::CopyFailure, exception.what(), {}};
	}
	catch (...)
	{
		m_result = {Application::TitleInstallError::CopyFailure,
			"Unknown title installation failure", {}};
	}
	if (m_result && !m_result.diagnostic.empty())
		cemuLog_log(LogType::Force, "Title installation warning: {}", m_result.diagnostic);
	m_threadState.store(ThreadState::Finished, std::memory_order_release);
}

int GameUpdateWindow::ShowModal()
{
	wxDialog::ShowModal();
	if (m_thread.joinable())
		m_thread.join();
	return m_result ? wxID_OK : wxID_CANCEL;
}

void GameUpdateWindow::OnClose(wxCloseEvent& event)
{
	if (m_threadState.load(std::memory_order_acquire) == ThreadState::Running)
	{
		wxMessageDialog dialog(this,
			_("Do you really want to cancel the installation process?\n\nCanceling the process will delete the staged files."),
			_("Info"), wxCENTRE | wxYES_NO);
		if (dialog.ShowModal() != wxID_YES)
			return;
		m_cancelRequested = true;
	}

	if (m_timer)
		m_timer->Stop();
	if (m_thread.joinable())
		m_thread.join();
	event.Skip();
}

void GameUpdateWindow::OnUpdate(wxTimerEvent& event)
{
	if (m_threadState.load(std::memory_order_acquire) == ThreadState::Finished)
	{
		Close();
		return;
	}

	const auto completed = m_processedBytes.load(std::memory_order_relaxed);
	const auto total = m_totalBytes.load(std::memory_order_relaxed);
	const auto percent = total == 0 ? 0 : static_cast<int>(std::min<long double>(100,
		static_cast<long double>(completed) * 100 / static_cast<long double>(total)));
	if (m_processBar->GetValue() != percent)
		m_processBar->SetValue(percent);
	event.Skip();
}

void GameUpdateWindow::OnCancelButton(wxCommandEvent& event)
{
	Close();
	event.Skip();
}
