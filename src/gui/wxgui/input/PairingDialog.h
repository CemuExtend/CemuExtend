#pragma once

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/stattext.h>

#include <atomic>
#include <thread>

class PairingDialog : public wxDialog
{
  public:
	PairingDialog(wxWindow* parent);
	~PairingDialog();

  private:
	enum class PairingState
	{
		Pairing,
		SearchBlocking,
		SearchResumed,
		Finished,
		NoBluetoothAvailable,
		SearchFailed,
		PairingFailed,
		BluetoothUnusable
	};

	void OnClose(wxCloseEvent& event);
	void OnCancelButton(const wxCommandEvent& event);
	void OnGaugeUpdate(wxCommandEvent& event);

	void WorkerThread();
	void UpdateCallback(PairingState state);
	void StopWorker();

	wxStaticText* m_text;
	wxGauge* m_gauge;
	wxButton* m_cancelButton;

	std::thread m_thread;
	std::atomic_bool m_threadShouldQuit{};
};
