#pragma once

#include "application/TitleInstallFacade.h"

#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/timer.h>

#include <atomic>
#include <string>
#include <thread>

namespace Application
{
	class EmulationController;
}

class AbortException : public std::exception {};

class GameUpdateWindow : public wxDialog
{
public:
	GameUpdateWindow(wxWindow& parent,
		Application::EmulationController& emulationController,
		const fs::path& sourcePath);
	~GameUpdateWindow();

	[[nodiscard]] uint64 GetTitleId() const { return m_plan.titleId; }
	[[nodiscard]] bool HasException() const
	{
		return m_result.error != Application::TitleInstallError::None &&
			m_result.error != Application::TitleInstallError::Cancelled;
	}
	[[nodiscard]] const std::string& GetExceptionMessage() const
	{
		return HasException() ? m_result.diagnostic : m_emptyDiagnostic;
	}
	[[nodiscard]] const std::string& GetGameName() const { return m_plan.titleName; }
	[[nodiscard]] uint32 GetTargetVersion() const { return m_plan.version; }
	[[nodiscard]] fs::path GetTargetPath() const
	{
		return m_result.installedPath.empty() ? m_plan.targetPath : m_result.installedPath;
	}

	int ShowModal() override;
	void OnClose(wxCloseEvent& event);
	void OnUpdate(wxTimerEvent& event);
	void OnCancelButton(wxCommandEvent& event);

private:
	enum class ThreadState : std::uint8_t
	{
		Running,
		Finished,
	};

	Application::EmulationController& m_emulationController;
	Application::TitleInstallPlan m_plan;
	Application::TitleInstallDecision m_decision{
		Application::TitleInstallDecision::Proceed};
	Application::TitleInstallResult m_result{
		Application::TitleInstallError::Cancelled, "Title installation cancelled", {}};
	std::string m_emptyDiagnostic;
	std::atomic_bool m_cancelRequested{};
	std::atomic<ThreadState> m_threadState{ThreadState::Running};
	std::atomic<std::uint64_t> m_processedBytes{};
	std::atomic<std::uint64_t> m_totalBytes{};
	std::thread m_thread;

	wxGauge* m_processBar{};
	wxTimer* m_timer{};

	void ThreadWork();
};
