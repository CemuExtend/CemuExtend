#include "wxgui/wxgui.h"
#include "wxgui/DownloadGraphicPacksWindow.h"

#include <cctype>
#include <filesystem>
#include <curl/curl.h>
#include <zip.h>
#include <rapidjson/document.h>
#include <boost/algorithm/string.hpp>

#include "Common/FileStream.h"

#include "application/EmulationController.h"
#include "host/contracts/HostContracts.h"

struct DownloadGraphicPacksWindow::curlDownloadFileState_t
{
	std::vector<uint8> fileData;
	std::atomic<double> progress{0.0};
	std::atomic<bool> isCanceled{false};
};

size_t DownloadGraphicPacksWindow::curlDownloadFile_writeData(void *ptr, size_t size, size_t nmemb, curlDownloadFileState_t* downloadState)
{
	const size_t writeSize = size * nmemb;
	const size_t currentSize = downloadState->fileData.size();
	const size_t newSize = currentSize + writeSize;
	downloadState->fileData.resize(newSize);
	memcpy(downloadState->fileData.data() + currentSize, ptr, writeSize);
	return writeSize;
}

int DownloadGraphicPacksWindow::progress_callback(curlDownloadFileState_t* downloadState, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	if (downloadState->isCanceled)
		return 1;

	if (dltotal > 1.0)
		downloadState->progress = dlnow / dltotal;
	else
		downloadState->progress = 0.0;
	return 0;
}

bool DownloadGraphicPacksWindow::curlDownloadFile(const char *url, curlDownloadFileState_t* downloadState)
{
	CURL* curl = curl_easy_init();
	if (curl == nullptr)
		return false;
	
	downloadState->progress = 0.0;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlDownloadFile_writeData);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, downloadState);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
	curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, downloadState);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 30L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

	curl_easy_setopt(curl, CURLOPT_USERAGENT, BUILD_VERSION_WITH_NAME_STRING);
	downloadState->fileData.resize(0);
	const CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	return res == CURLE_OK;
}

// returns true if the version matches
bool checkGraphicPackDownloadedVersion(const Host::IPathProvider& pathProvider,
	const char* nameVersion, bool& hasVersionFile)
{
	hasVersionFile = false;
	const auto path = pathProvider.GetUserDataPath(
		"graphicPacks/downloadedGraphicPacks/version.txt");
	std::unique_ptr<FileStream> file(FileStream::openFile2(path));

	std::string versionInFile;
	if (file && file->readLine(versionInFile))
	{
		return boost::iequals(versionInFile, nameVersion);
	}
	return false;
}

void createGraphicPackDownloadedVersionFile(const Host::IPathProvider& pathProvider,
	const char* nameVersion)
{
	const auto path = pathProvider.GetUserDataPath(
		"graphicPacks/downloadedGraphicPacks/version.txt");
	std::unique_ptr<FileStream> file(FileStream::createFile2(path));
	if (file)
		file->writeString(nameVersion);
	else
	{
		cemuLog_log(LogType::Force, "Failed to write graphic pack version.txt");
	}
}

void deleteDownloadedGraphicPacks(const Host::IPathProvider& pathProvider)
{
	const auto path = pathProvider.GetUserDataPath(
		"graphicPacks/downloadedGraphicPacks");
	std::error_code er;
	if (!fs::exists(path, er))
		return;
	try
	{
		for (auto& p : fs::directory_iterator(path))
		{
			fs::remove_all(p.path(), er);
		}
	}
	catch (std::filesystem::filesystem_error& e)
	{
		cemuLog_log(LogType::Force, "Error in deleteDownloadedGraphicPacks():");
		cemuLog_log(LogType::Force, e.what());
	}
}

void DownloadGraphicPacksWindow::UpdateThread()
{
	// get github url
	std::string githubAPIUrl;
	auto& tempDownloadState = *m_downloadState;
	std::string queryUrl("https://cemu.info/api2/query_graphicpack_url.php?");
	char temp[64];
	sprintf(temp, "version=%d.%d.%d", EMULATOR_VERSION_MAJOR, EMULATOR_VERSION_MINOR, EMULATOR_VERSION_PATCH);
	queryUrl.append(temp);
	queryUrl.append("&");
	sprintf(temp, "t=%u", (uint32)std::chrono::seconds(std::time(NULL)).count()); // add a dynamic part to the url to bypass overly aggressive caching (like some proxies do)
	queryUrl.append(temp);
	const bool queryDownloaded = curlDownloadFile(queryUrl.c_str(), &tempDownloadState);
	if (tempDownloadState.isCanceled)
		return;
	if (queryDownloaded && tempDownloadState.fileData.size() >= 4 &&
		std::memcmp(tempDownloadState.fileData.data(), "http", 4) == 0)
	{
		// convert downloaded data to url string
		githubAPIUrl.assign(tempDownloadState.fileData.cbegin(), tempDownloadState.fileData.cend());
	}
	else
	{
		// cemu api request failed, use hardcoded github url
		cemuLog_log(LogType::Force, "Graphic pack update request failed or returned invalid URL. Using default repository URL instead");
		githubAPIUrl = "https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest";
	}
	// github API request
	if (curlDownloadFile(githubAPIUrl.c_str(), &tempDownloadState) == false)
	{
		if (tempDownloadState.isCanceled)
			return;
		m_notification = NotificationConnectionFailed;
		SetThreadResult(ThreadError);
		return;
	}
	if (tempDownloadState.isCanceled)
		return;
	// parse json result
	rapidjson::Document d;
	d.Parse((const char*)tempDownloadState.fileData.data(), tempDownloadState.fileData.size());
	if (d.HasParseError() || !d.IsObject())
	{
		SetThreadResult(ThreadError);
		return;
	}
	const auto jsonName = d.FindMember("name");
	if (jsonName == d.MemberEnd() || !jsonName->value.IsString())
	{
		SetThreadResult(ThreadError);
		return;
	}
	const char* assetName = jsonName->value.GetString(); // name includes version
	const auto jsonAssets = d.FindMember("assets");
	if (jsonAssets == d.MemberEnd() || !jsonAssets->value.IsArray() || jsonAssets->value.Empty())
	{
		SetThreadResult(ThreadError);
		return;
	}
	auto& jsonAsset0 = jsonAssets->value.GetArray()[0];
	if (jsonAsset0.IsObject() == false)
	{
		SetThreadResult(ThreadError);
		return;
	}
	const auto jsonDownloadUrl = jsonAsset0.FindMember("browser_download_url");
	if (jsonDownloadUrl == jsonAsset0.MemberEnd() || !jsonDownloadUrl->value.IsString())
	{
		SetThreadResult(ThreadError);
		return;
	}
	m_assetName = assetName;
	m_browserDownloadUrl = jsonDownloadUrl->value.GetString();
	// check version
	bool hasVersionFile = false;
	if (checkGraphicPackDownloadedVersion(*m_pathProvider, assetName, hasVersionFile))
	{
		// already up to date
		m_notification = NotificationNoUpdates;
		SetThreadResult(ThreadFinished);
		return;
	}
	if (hasVersionFile)
	{
		SetThreadResult(ThreadAwaitingConfirmation);
		return;
	}
	if (tempDownloadState.isCanceled)
		return;

	DownloadAndInstall();
}

void DownloadGraphicPacksWindow::DownloadAndInstall()
{
	// download zip
	m_stage = StageDownloading;
	if (curlDownloadFile(m_browserDownloadUrl.c_str(), m_downloadState.get()) == false)
	{
		if (m_downloadState->isCanceled)
			return;
		m_notification = NotificationConnectionFailed;
		SetThreadResult(ThreadError);
		return;
	}
	if (m_downloadState->isCanceled)
		return;

	m_extractionProgress = 0.0;
	m_stage = StageExtracting;

	zip_source_t *src;
	zip_t *za;
	zip_error_t error;

	// init zip source
	zip_error_init(&error);
	if ((src = zip_source_buffer_create(m_downloadState->fileData.data(), m_downloadState->fileData.size(), 0, &error)) == NULL)
	{
		zip_error_fini(&error);
		SetThreadResult(ThreadError);
		return;
	}

	// open zip from source
	if ((za = zip_open_from_source(src, 0, &error)) == NULL)
	{
		zip_source_free(src);
		zip_error_fini(&error);
		SetThreadResult(ThreadError);
		return;
	}

	const auto extractionRoot = m_pathProvider->GetUserDataPath(
		"graphicPacks/downloadedGraphicPacks");
	if (m_downloadState->isCanceled)
	{
		zip_discard(za);
		zip_error_fini(&error);
		return;
	}
	auto path = extractionRoot;
	std::error_code er;
	//fs::remove_all(path, er); -> Don't delete the whole folder and recreate it immediately afterwards because sometimes it just fails
	deleteDownloadedGraphicPacks(*m_pathProvider);
	if (m_downloadState->isCanceled)
	{
		zip_discard(za);
		zip_error_fini(&error);
		return;
	}
	fs::create_directories(path, er); // make sure downloadedGraphicPacks folder exists

	sint32 numEntries = zip_get_num_entries(za, 0);
	bool extractionFailed = false;
	if (numEntries < 0)
		extractionFailed = true;
	for (sint32 i = 0; i < numEntries; i++)
	{
		if (m_downloadState->isCanceled)
			break;

		m_extractionProgress = (double)i / (double)numEntries;
		zip_stat_t sb = { 0 };
		if (zip_stat_index(za, i, 0, &sb) != 0)
		{
			assert_dbg();
			extractionFailed = true;
			break;
		}

		if (sb.name == nullptr)
		{
			extractionFailed = true;
			break;
		}

		const std::string_view entryName(sb.name);
		const fs::path entryPath = _utf8ToPath(entryName);
		const bool hasDrivePrefix = entryName.size() >= 2 &&
			std::isalpha(static_cast<unsigned char>(entryName[0])) && entryName[1] == ':';
		bool hasParentTraversal = false;
		for (const auto& component : entryPath)
			hasParentTraversal |= component == "..";
		if (entryName.empty() || entryName.front() == '/' || entryName.front() == '\\' ||
			hasDrivePrefix || entryPath.is_absolute() || entryPath.has_root_name() ||
			entryPath.has_root_directory() || hasParentTraversal ||
			std::strstr(sb.name, "..\\") != nullptr)
		{
			extractionFailed = true;
			break;
		}

		path = extractionRoot / entryPath;

		size_t sbNameLen = strlen(sb.name);
		if(sbNameLen == 0)
			continue;
		if (sb.name[sbNameLen - 1] == '/')
		{
			fs::create_directories(path, er);
			continue;
		}
		if(sb.size == 0)
			continue;
		if (sb.size > (1024 * 1024 * 128))
			continue; // skip unusually huge files

		zip_file_t* zipFile = zip_fopen_index(za, i, 0);
		if (zipFile == nullptr)
			continue;

		uint8* fileBuffer = new uint8[sb.size];

		if (zip_fread(zipFile, fileBuffer, sb.size) == sb.size)
		{
			FileStream* fs = FileStream::createFile2(path);
			if (fs)
			{
				fs->writeData(fileBuffer, sb.size);
				delete fs;
			}
		}

		delete [] fileBuffer;
		zip_fclose(zipFile);
	}

	zip_discard(za);
	zip_error_fini(&error);
	if (m_downloadState->isCanceled)
		return;
	if (extractionFailed)
	{
		SetThreadResult(ThreadError);
		return;
	}
	createGraphicPackDownloadedVersionFile(*m_pathProvider, m_assetName.c_str());
	SetThreadResult(ThreadFinished);
}

void DownloadGraphicPacksWindow::SetThreadResult(ThreadState_t result)
{
	auto expected = ThreadRunning;
	m_threadState.compare_exchange_strong(expected, result);
}

DownloadGraphicPacksWindow::DownloadGraphicPacksWindow(wxWindow* parent,
	Application::EmulationController& emulationController,
	std::shared_ptr<Host::IPathProvider> pathProvider)
	: wxDialog(parent, wxID_ANY, _("Checking version..."), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	m_threadState(ThreadRunning), m_stage(StageCheckVersion), m_currentStage(StageCheckVersion),
	m_emulationController(emulationController),
	m_pathProvider(std::move(pathProvider))
{
	cemu_assert(m_pathProvider != nullptr);
	auto* sizer = new wxBoxSizer(wxVERTICAL);

	m_processBar = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(500, 20), wxGA_HORIZONTAL);
	m_processBar->SetValue(0);
	m_processBar->SetRange(100);
	sizer->Add(m_processBar, 0, wxALL | wxEXPAND, 5);

	auto* m_cancelButton = new wxButton(this, wxID_ANY, _("Cancel"));
	m_cancelButton->Bind(wxEVT_BUTTON, &DownloadGraphicPacksWindow::OnCancelButton, this);
	sizer->Add(m_cancelButton, 0, wxALIGN_RIGHT | wxALL, 5);

	this->SetSizer(sizer);
	this->Centre(wxBOTH);

	wxWindowBase::Layout();
	wxWindowBase::Fit();

	m_timer = new wxTimer(this);
	this->Bind(wxEVT_TIMER, &DownloadGraphicPacksWindow::OnUpdate, this);
	this->Bind(wxEVT_CLOSE_WINDOW, &DownloadGraphicPacksWindow::OnClose, this);
	m_timer->Start(250);


	m_downloadState = std::make_unique<curlDownloadFileState_t>();
}

DownloadGraphicPacksWindow::~DownloadGraphicPacksWindow()
{
	m_downloadState->isCanceled = true;
	m_timer->Stop();
	if (m_thread.joinable())
		m_thread.join();
}

const std::string& DownloadGraphicPacksWindow::GetException() const
{
	return m_threadException;
}

int DownloadGraphicPacksWindow::ShowModal()
{
	if(m_emulationController.IsTitleRunning())
	{
		wxMessageBox(_("Graphic packs cannot be updated while a game is running."), _("Graphic packs"), 5, this);
		return wxID_CANCEL;
	}
	m_downloadState->isCanceled = false;
	m_thread = std::thread(&DownloadGraphicPacksWindow::UpdateThread, this);
	wxDialog::ShowModal();
	return m_threadState == ThreadCanceled ? wxID_CANCEL : wxID_OK;
}

void DownloadGraphicPacksWindow::OnClose(wxCloseEvent& event)
{
	m_downloadState->isCanceled = true;
	if (m_threadState == ThreadRunning || m_threadState == ThreadAwaitingConfirmation || m_threadState == ThreadPrompting)
	{
		//wxMessageDialog dialog(this, _("Do you really want to cancel the update process?\n\nCanceling the process will delete the applied update."), _("Info"), wxCENTRE | wxYES_NO);
		//if (dialog.ShowModal() != wxID_YES)
		//	return;

		m_threadState = ThreadCanceled;
	}

	m_timer->Stop();
	if (m_thread.joinable())
		m_thread.join();

	event.Skip();
}

void DownloadGraphicPacksWindow::OnUpdate(const wxTimerEvent& event)
{
	const auto threadState = m_threadState.load();
	if (threadState == ThreadAwaitingConfirmation)
	{
		if (m_thread.joinable())
			m_thread.join();

		m_timer->Stop();
		m_threadState = ThreadPrompting;
		const int answer = wxMessageBox(_("Updated graphic packs are available. Do you want to download and install them?"), _("Graphic packs"), wxYES_NO, this);
		if (m_threadState != ThreadPrompting)
			return;
		if (answer == wxYES)
		{
			m_threadState = ThreadRunning;
			m_thread = std::thread(&DownloadGraphicPacksWindow::DownloadAndInstall, this);
			m_timer->Start(250);
		}
		else
		{
			m_threadState = ThreadFinished;
			Close();
		}
		return;
	}
	if (threadState == ThreadPrompting)
		return;
	if (threadState != ThreadRunning)
	{
		m_timer->Stop();
		switch (m_notification.exchange(NotificationNone))
		{
		case NotificationConnectionFailed:
			wxMessageBox(_("Error"), _(L"Failed to connect to server"), wxOK | wxCENTRE | wxICON_ERROR, this);
			break;
		case NotificationNoUpdates:
			wxMessageBox(_("No updates available."), _("Graphic packs"), wxOK | wxCENTRE, this);
			break;
		case NotificationNone:
			break;
		}
		Close();
		return;
	}
	if (m_currentStage != m_stage)
	{
		if (m_stage == StageDownloading)
		{
			this->SetTitle(_("Downloading graphic packs..."));
		}
		else if (m_stage == StageExtracting)
		{
			this->SetTitle(_("Extracting..."));
		}
		m_currentStage = m_stage;
	}

	if (m_currentStage == StageDownloading)
	{
		const sint32 processedSize = (sint32)(m_downloadState->progress.load() * 100.0f);
		if (m_processBar->GetValue() != processedSize)
			m_processBar->SetValue(processedSize);
	}
	else if (m_currentStage == StageExtracting)
	{
		const sint32 processedSize = (sint32)(m_extractionProgress * 100.0f);
		if (m_processBar->GetValue() != processedSize)
			m_processBar->SetValue(processedSize);
	}
}

void DownloadGraphicPacksWindow::OnCancelButton(const wxCommandEvent& event)
{
	Close();
}
