#pragma once

#include "application/EmulationController.h"

#include <wx/app.h>

class MainWindow;
class WxWindowState;
class WxMainWindowRegistry;
class wxTimer;
class wxTimerEvent;

class CemuApp : public wxApp
{
public:
	bool OnInit() override;
	int OnExit() override;

	void OnAssertFailure(const wxChar* file, int line, const wxChar* func, const wxChar* cond, const wxChar* msg) override;
	int FilterEvent(wxEvent& event) override;

	std::vector<const wxLanguageInfo*> GetLanguages() const;

	static bool CheckMLCPath(const fs::path& mlc);
	static bool CreateDefaultMLCFiles(const fs::path& mlc);
	static void CreateDefaultCemuFiles();

	static void InitializeNewMLCOrFail(fs::path mlc);
	static void InitializeExistingMLCOrFail(fs::path mlc);
private:
	void LocalizeUI(wxLanguage languageToUse);

	void DeterminePaths(std::set<fs::path>& failedWriteAccess);

	void ActivateApp(wxActivateEvent& event);
	static std::vector<const wxLanguageInfo*> GetAvailableTranslationLanguages(wxTranslations* translationsMgr);

	// Composition root: the application owns core lifecycle and publishes
	// use-case events to whichever frontend is currently attached.
	Application::EmulationController m_emulationController;
	MainWindow* m_mainFrame = nullptr;
	std::shared_ptr<WxWindowState> m_windowState;
	std::shared_ptr<WxMainWindowRegistry> m_mainWindowRegistry;
#if BOOST_OS_MACOS
	void OnSDLEventPumpTimer(wxTimerEvent& event);
	wxTimer* m_sdlEventPumpTimer = nullptr;
#endif

	wxLocale m_locale;
	std::vector<const wxLanguageInfo*> m_availableTranslations;
};

wxDECLARE_APP(CemuApp);
