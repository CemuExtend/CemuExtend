#pragma once

#include <atomic>
#include <curl/system.h>
#include <thread>
#include <string>
#include <memory>

#include <wx/dialog.h>
#include <wx/timer.h>
#include <wx/gauge.h>

namespace Application { class EmulationController; }
namespace Host { class IPathProvider; }

class DownloadGraphicPacksWindow : public wxDialog
{
public:
	DownloadGraphicPacksWindow(wxWindow* parent,
		Application::EmulationController& emulationController,
		std::shared_ptr<Host::IPathProvider> pathProvider);
	~DownloadGraphicPacksWindow();

	const std::string& GetException() const;

	int ShowModal() override;
	void OnClose(wxCloseEvent& event);

	void OnUpdate(const wxTimerEvent& event);
	void OnCancelButton(const wxCommandEvent& event);

private:
	void UpdateThread();
	void DownloadAndInstall();

	enum ThreadState_t
	{
		ThreadRunning,
		ThreadAwaitingConfirmation,
		ThreadPrompting,
		ThreadCanceled,
		ThreadError,
		ThreadFinished,
	};

	enum Notification_t
	{
		NotificationNone,
		NotificationConnectionFailed,
		NotificationNoUpdates,
	};

	enum DownloadStage_t
	{
		StageCheckVersion,
		StageDownloading,
		StageExtracting
	};

	void SetThreadResult(ThreadState_t result);

	std::atomic<ThreadState_t> m_threadState;
	std::atomic<DownloadStage_t> m_stage;
	std::atomic<double> m_extractionProgress;
	std::atomic<Notification_t> m_notification{NotificationNone};
	std::string m_threadException;
	std::string m_assetName;
	std::string m_browserDownloadUrl;
	std::thread m_thread;

	DownloadStage_t m_currentStage;
	wxGauge* m_processBar;
	wxTimer* m_timer;

	struct curlDownloadFileState_t;
	std::unique_ptr<curlDownloadFileState_t> m_downloadState;
	Application::EmulationController& m_emulationController;
	std::shared_ptr<Host::IPathProvider> m_pathProvider;

	static size_t curlDownloadFile_writeData(void* ptr, size_t size, size_t nmemb, curlDownloadFileState_t* downloadState);
	static int progress_callback(curlDownloadFileState_t* downloadState, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
	static bool curlDownloadFile(const char* url, curlDownloadFileState_t* downloadState);
};
