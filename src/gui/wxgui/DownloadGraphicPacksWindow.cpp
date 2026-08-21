#include "wxgui/wxgui.h"
#include "wxgui/DownloadGraphicPacksWindow.h"
#include "frontend/ArchiveInstallPolicy.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <curl/curl.h>
#include <zip.h>
#include <rapidjson/document.h>
#include <boost/algorithm/string.hpp>

#include "Common/FileStream.h"

#include "application/EmulationController.h"
#include "host/contracts/HostContracts.h"

namespace
{
	constexpr std::size_t kMaximumGraphicPackDownloadSize = 512ULL * 1024ULL * 1024ULL;
	constexpr zip_uint64_t kMaximumGraphicPackEntries = 20000;
	constexpr zip_uint64_t kMaximumGraphicPackFileSize = 128ULL * 1024ULL * 1024ULL;
	constexpr zip_uint64_t kMaximumGraphicPackTotalSize = 2ULL * 1024ULL * 1024ULL * 1024ULL;
	constexpr zip_uint64_t kMaximumGraphicPackCompressionRatio = 1000;

	bool IsGraphicPackArchiveSymlink(zip_t* archive, zip_uint64_t index)
	{
		zip_uint8_t operatingSystem{};
		zip_uint32_t attributes{};
		if (zip_file_get_external_attributes(archive, index, 0, &operatingSystem, &attributes) != 0)
			return true;
		return operatingSystem == ZIP_OPSYS_UNIX && ((attributes >> 16U) & 0170000U) == 0120000U;
	}

	fs::path UniqueGraphicPackSibling(const fs::path& target, std::string_view suffix)
	{
		const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
		for (std::uint32_t attempt = 0; attempt < 1000; ++attempt)
		{
			auto candidate = target.parent_path() /
				fmt::format(".{}.{}.{}.{}", _pathToUtf8(target.filename()), suffix, seed, attempt);
			std::error_code error;
			if (!fs::exists(candidate, error) && !error)
				return candidate;
		}
		throw std::runtime_error("Unable to allocate graphic-pack transaction path");
	}
}

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
	if (writeSize > kMaximumGraphicPackDownloadSize - std::min(currentSize, kMaximumGraphicPackDownloadSize))
		return 0;
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
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");

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
		hasVersionFile = true;
		return boost::iequals(versionInFile, nameVersion);
	}
	return false;
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
	m_stage = StageDownloading;
	if (!curlDownloadFile(m_browserDownloadUrl.c_str(), m_downloadState.get()))
	{
		if (!m_downloadState->isCanceled)
		{
			m_notification = NotificationConnectionFailed;
			SetThreadResult(ThreadError);
		}
		return;
	}
	if (m_downloadState->isCanceled)
		return;

	m_extractionProgress = 0.0;
	m_stage = StageExtracting;

	zip_error_t zipError;
	zip_error_init(&zipError);
	zip_source_t* source = zip_source_buffer_create(m_downloadState->fileData.data(),
		m_downloadState->fileData.size(), 0, &zipError);
	if (!source)
	{
		zip_error_fini(&zipError);
		SetThreadResult(ThreadError);
		return;
	}
	zip_t* rawArchive = zip_open_from_source(source, ZIP_RDONLY, &zipError);
	if (!rawArchive)
	{
		zip_source_free(source);
		zip_error_fini(&zipError);
		SetThreadResult(ThreadError);
		return;
	}
	std::unique_ptr<zip_t, decltype(&zip_discard)> archive(rawArchive, &zip_discard);
	zip_error_fini(&zipError);

	struct PlannedEntry
	{
		zip_uint64_t index{};
		fs::path relativePath;
		zip_uint64_t size{};
		bool directory{};
	};
	std::vector<PlannedEntry> plan;
	std::set<fs::path> paths;
	zip_uint64_t totalSize{};
	const auto entryCount = zip_get_num_entries(archive.get(), 0);
	if (entryCount < 0 || static_cast<zip_uint64_t>(entryCount) > kMaximumGraphicPackEntries)
	{
		SetThreadResult(ThreadError);
		return;
	}
	plan.reserve(static_cast<std::size_t>(entryCount));
	for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entryCount); ++index)
	{
		zip_stat_t stat{};
		if (zip_stat_index(archive.get(), index, 0, &stat) != 0 || !stat.name ||
			IsGraphicPackArchiveSymlink(archive.get(), index))
		{
			SetThreadResult(ThreadError);
			return;
		}
		auto relativePath = Frontend::ArchiveInstallPolicy::NormalizeRelativePath(stat.name);
		if (!relativePath || !paths.emplace(*relativePath).second)
		{
			SetThreadResult(ThreadError);
			return;
		}
		const auto nameLength = std::strlen(stat.name);
		const bool directory = nameLength > 0 &&
			(stat.name[nameLength - 1] == '/' || stat.name[nameLength - 1] == '\\');
		if (!directory)
		{
			if (stat.size > kMaximumGraphicPackFileSize ||
				totalSize > kMaximumGraphicPackTotalSize - stat.size ||
				(stat.comp_size > 0 && stat.size / stat.comp_size > kMaximumGraphicPackCompressionRatio))
			{
				SetThreadResult(ThreadError);
				return;
			}
			totalSize += stat.size;
		}
		plan.push_back({index, std::move(*relativePath), stat.size, directory});
	}

	const auto target = m_pathProvider->GetUserDataPath("graphicPacks/downloadedGraphicPacks");
	const auto staging = UniqueGraphicPackSibling(target, "staging");
	const auto backup = UniqueGraphicPackSibling(target, "backup");
	std::error_code filesystemError;
	fs::create_directories(staging, filesystemError);
	if (filesystemError)
	{
		SetThreadResult(ThreadError);
		return;
	}
	auto cleanupStaging = [&] {
		std::error_code cleanupError;
		fs::remove_all(staging, cleanupError);
	};

	bool extractionFailed = false;
	std::array<char, 1024 * 1024> buffer{};
	for (std::size_t planIndex = 0; planIndex < plan.size(); ++planIndex)
	{
		if (m_downloadState->isCanceled)
		{
			extractionFailed = true;
			break;
		}
		m_extractionProgress = plan.empty() ? 1.0 :
			static_cast<double>(planIndex) / static_cast<double>(plan.size());
		const auto& entry = plan[planIndex];
		const auto destination = staging / entry.relativePath;
		if (entry.directory)
		{
			fs::create_directories(destination, filesystemError);
			if (filesystemError)
			{
				extractionFailed = true;
				break;
			}
			continue;
		}
		fs::create_directories(destination.parent_path(), filesystemError);
		if (filesystemError)
		{
			extractionFailed = true;
			break;
		}
		std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(
			zip_fopen_index(archive.get(), entry.index, 0), &zip_fclose);
		std::ofstream output(destination, std::ios::binary | std::ios::trunc);
		if (!file || !output)
		{
			extractionFailed = true;
			break;
		}
		zip_uint64_t remaining = entry.size;
		while (remaining > 0)
		{
			if (m_downloadState->isCanceled)
			{
				extractionFailed = true;
				break;
			}
			const auto request = static_cast<zip_uint64_t>(
				std::min<zip_uint64_t>(buffer.size(), remaining));
			const auto read = zip_fread(file.get(), buffer.data(), request);
			if (read <= 0 || static_cast<zip_uint64_t>(read) > remaining)
			{
				extractionFailed = true;
				break;
			}
			output.write(buffer.data(), static_cast<std::streamsize>(read));
			if (!output)
			{
				extractionFailed = true;
				break;
			}
			remaining -= static_cast<zip_uint64_t>(read);
		}
		if (extractionFailed)
			break;
	}
	if (extractionFailed || m_downloadState->isCanceled)
	{
		cleanupStaging();
		if (!m_downloadState->isCanceled)
			SetThreadResult(ThreadError);
		return;
	}

	{
		std::ofstream versionFile(staging / "version.txt", std::ios::binary | std::ios::trunc);
		versionFile << m_assetName;
		if (!versionFile)
		{
			cleanupStaging();
			SetThreadResult(ThreadError);
			return;
		}
	}
	if (m_downloadState->isCanceled)
	{
		cleanupStaging();
		return;
	}

	const auto commit = Frontend::ArchiveInstallPolicy::CommitStagedDirectory(
		staging, target, backup, true);
	if (!commit.committed)
	{
		cleanupStaging();
		if (commit.error == Frontend::ArchiveInstallPolicy::CommitError::RollbackFailed)
			cemuLog_log(LogType::Force, "Graphic-pack rollback failed: {}",
				commit.filesystemError.message());
		SetThreadResult(ThreadError);
		return;
	}
	if (commit.error == Frontend::ArchiveInstallPolicy::CommitError::BackupCleanupFailed)
	{
		cemuLog_log(LogType::Force, "Unable to remove old graphic-pack backup: {}",
			commit.filesystemError.message());
	}
	m_extractionProgress = 1.0;
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

void DownloadGraphicPacksWindow::StartWorker(void (DownloadGraphicPacksWindow::*worker)())
{
	m_thread = std::thread([this, worker] {
		try
		{
			(this->*worker)();
		}
		catch (const std::exception& error)
		{
			m_threadException = error.what();
			if (!m_downloadState->isCanceled)
				SetThreadResult(ThreadError);
		}
		catch (...)
		{
			m_threadException = "Unknown graphic-pack worker error";
			if (!m_downloadState->isCanceled)
				SetThreadResult(ThreadError);
		}
	});
}

int DownloadGraphicPacksWindow::ShowModal()
{
	if(m_emulationController.IsTitleRunning())
	{
		wxMessageBox(_("Graphic packs cannot be updated while a game is running."), _("Graphic packs"), 5, this);
		return wxID_CANCEL;
	}
	m_downloadState->isCanceled = false;
	StartWorker(&DownloadGraphicPacksWindow::UpdateThread);
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
			StartWorker(&DownloadGraphicPacksWindow::DownloadAndInstall);
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
		if (threadState == ThreadError)
		{
			const auto details = m_threadException.empty() ?
				_("The downloaded graphic-pack archive could not be installed safely.") :
				wxString::FromUTF8(m_threadException);
			wxMessageBox(details, _("Graphic pack update failed"),
				wxOK | wxCENTRE | wxICON_ERROR, this);
		}
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
