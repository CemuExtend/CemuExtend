#include "wxgui/DownloadCustomGraphicPackWindow.h"
#include "frontend/ArchiveInstallPolicy.h"
#include "host/contracts/HostContracts.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <wx/event.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/gdicmn.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/timer.h>

#include <curl/curl.h>
#include <zip.h>

namespace
{
	constexpr std::size_t kMaximumCustomPackDownloadSize = 512ULL * 1024ULL * 1024ULL;
	constexpr zip_uint64_t kMaximumCustomPackEntries = 20000;
	constexpr zip_uint64_t kMaximumCustomPackFileSize = 128ULL * 1024ULL * 1024ULL;
	constexpr zip_uint64_t kMaximumCustomPackTotalSize = 2ULL * 1024ULL * 1024ULL * 1024ULL;
	constexpr zip_uint64_t kMaximumCustomPackCompressionRatio = 1000;

	bool IsCustomPackArchiveSymlink(zip_t* archive, zip_uint64_t index)
	{
		zip_uint8_t operatingSystem{};
		zip_uint32_t attributes{};
		if (zip_file_get_external_attributes(archive, index, 0, &operatingSystem, &attributes) != 0)
			return true;
		return operatingSystem == ZIP_OPSYS_UNIX && ((attributes >> 16U) & 0170000U) == 0120000U;
	}

	fs::path UniqueCustomPackSibling(const fs::path& target)
	{
		const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
		for (std::uint32_t attempt = 0; attempt < 1000; ++attempt)
		{
			const auto candidate = target.parent_path() /
								   fmt::format(".{}.staging.{}.{}", _pathToUtf8(target.filename()), seed, attempt);
			std::error_code error;
			if (!fs::exists(candidate, error) && !error)
				return candidate;
		}
		throw std::runtime_error("Unable to allocate custom graphic-pack transaction path");
	}
} // namespace

static size_t curlDownloadFile_writeData(void* ptr, size_t size, size_t nmemb, DownloadCustomGraphicPackWindow::curlDownloadFileState_t* downloadState)
{
	const size_t writeSize = size * nmemb;
	const size_t currentSize = downloadState->fileData.size();
	if (writeSize > kMaximumCustomPackDownloadSize - std::min(currentSize, kMaximumCustomPackDownloadSize))
		return 0;
	auto* bytePtr = static_cast<const uint8*>(ptr);
	downloadState->fileData.insert(downloadState->fileData.end(), bytePtr, bytePtr + writeSize);
	return writeSize;
}

static int progress_callback(DownloadCustomGraphicPackWindow::curlDownloadFileState_t* downloadState, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	if (downloadState->isCanceled)
		return 1;

	if (dltotal > 1.0)
		downloadState->progress = dlnow / dltotal;
	else
		downloadState->progress = 0.0;
	return 0;
}

static bool curlDownloadFile(const char* url, DownloadCustomGraphicPackWindow::curlDownloadFileState_t* downloadState)
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
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
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

DownloadCustomGraphicPackWindow::DownloadCustomGraphicPackWindow(wxWindow* parent,
																 std::shared_ptr<Host::IPathProvider> pathProvider)
	: wxDialog(parent, wxID_ANY, _("Download Graphic Pack from URL"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_stage(StageDone), m_currentStage(StageDone),
	  m_pathProvider(std::move(pathProvider))
{
	cemu_assert(m_pathProvider != nullptr);
	auto* sizer = new wxBoxSizer(wxVERTICAL);

	m_urlField = new wxTextCtrl(this, wxID_ANY, wxEmptyString);
	m_urlField->SetHint(_("Enter download URL..."));
	sizer->Add(m_urlField, 0, wxALL | wxEXPAND, 5);

	m_processBar = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(500, 20), wxGA_HORIZONTAL);
	m_processBar->SetValue(0);
	m_processBar->SetRange(100);
	sizer->Add(m_processBar, 0, wxALL | wxEXPAND, 5);

	auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);

	m_statusText = new wxStaticText(this, wxID_ANY, _("Ready..."));
	buttonSizer->Add(m_statusText, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	buttonSizer->AddStretchSpacer(1);

	auto* m_closeButton = new wxButton(this, wxID_ANY, _("Close"));
	m_closeButton->Bind(wxEVT_BUTTON, &DownloadCustomGraphicPackWindow::OnCancelButton, this);
	buttonSizer->Add(m_closeButton, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	m_downloadButton = new wxButton(this, wxID_ANY, _("Download"));
	m_downloadButton->Bind(wxEVT_BUTTON, &DownloadCustomGraphicPackWindow::OnDownloadButton, this);
	buttonSizer->Add(m_downloadButton, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	sizer->Add(buttonSizer, 0, wxEXPAND | wxALL, 5);

	this->SetSizer(sizer);
	this->Centre(wxBOTH);

	wxWindowBase::Layout();
	wxWindowBase::Fit();

	m_timer = new wxTimer(this);
	this->Bind(wxEVT_TIMER, &DownloadCustomGraphicPackWindow::OnUpdate, this);
	this->Bind(wxEVT_CLOSE_WINDOW, &DownloadCustomGraphicPackWindow::OnClose, this);
	m_timer->Start(100);

	m_downloadState = std::make_unique<curlDownloadFileState_t>();
}

DownloadCustomGraphicPackWindow::~DownloadCustomGraphicPackWindow()
{
	if (m_downloadState)
		m_downloadState->isCanceled = true;

	m_timer->Stop();
	if (m_thread.joinable())
		m_thread.join();
}

int DownloadCustomGraphicPackWindow::ShowModal()
{
	wxDialog::ShowModal();
	return wxID_OK;
}

void DownloadCustomGraphicPackWindow::OnClose(wxCloseEvent& event)
{
	if (m_downloadState)
	{
		m_downloadState->isCanceled = true;
	}

	m_timer->Stop();
	if (m_thread.joinable())
		m_thread.join();

	event.Skip();
}

void DownloadCustomGraphicPackWindow::OnUpdate(const wxTimerEvent& event)
{
	if (m_currentStage >= StageDone)
	{
		m_downloadButton->Enable();
		m_urlField->Enable();
	}
	else
	{
		m_downloadButton->Disable();
		m_urlField->Disable();
	}

	if (m_currentStage == StageDownloading)
	{
		const sint32 processedSize = (sint32)(m_downloadState->progress * 100.0f);
		if (m_processBar->GetValue() != processedSize)
			m_processBar->SetValue(processedSize);
	}
	else if (m_currentStage == StageExtracting)
	{
		const sint32 processedSize = (sint32)(m_extractionProgress * 100.0f);
		if (m_processBar->GetValue() != processedSize)
			m_processBar->SetValue(processedSize);
	}

	if (m_currentStage != m_stage)
	{
		wxString status = "...";
		const wxColour* colour = wxWHITE;

		switch (m_stage)
		{
		case StageDownloading:
			status = "Downloading...";
			colour = wxWHITE;
			break;
		case StageVerifying:
			status = "Verifying...";
			colour = wxWHITE;
			break;
		case StageExtracting:
			status = "Extracting...";
			colour = wxWHITE;
			break;
		case StageDone:
			status = "Done!";
			colour = wxGREEN;
			m_processBar->SetValue(100.0);
			break;
		case StageErrConnectFailed:
			if (m_urlField->GetValue().empty())
			{
				status = "Please enter a valid URL.";
				colour = wxWHITE;
			}
			else
			{
				status = "ERROR: Connection failed.";
				colour = wxRED;
			}
			m_processBar->SetValue(0.0);
			break;
		case StageErrInvalidPack:
			status = "ERROR: Invalid pack.";
			colour = wxRED;
			m_processBar->SetValue(0.0);
			break;
		case StageErrSourceFailed:
			status = "ERROR: Failed to create ZIP source.";
			colour = wxRED;
			m_processBar->SetValue(0.0);
			break;
		case StageErrOpenFailed:
			status = "ERROR: Failed to open downloaded ZIP.";
			colour = wxRED;
			m_processBar->SetValue(0.0);
			break;
		case StageErrConflict:
			status = "ERROR: File conflict. Pack already installed?";
			colour = wxRED;
			m_processBar->SetValue(0.0);
			break;
		}

		m_currentStage = m_stage;
		m_statusText->SetLabel(status);
		m_statusText->SetForegroundColour(*colour);
	}
}

void DownloadCustomGraphicPackWindow::OnCancelButton(const wxCommandEvent& event)
{
	Close();
}

void DownloadCustomGraphicPackWindow::OnDownloadButton(const wxCommandEvent& event)
{
	m_downloadButton->Disable();
	m_urlField->Disable();

	const wxString urlText = m_urlField->GetValue();
	const auto urlUtf8 = urlText.utf8_string();
	std::string downloadUrl(urlUtf8.data(), urlUtf8.length());
	std::string folderName = "NewCustomPack";
	const int lastSlash = urlText.Find('/', true);
	wxString fileNameBase = (lastSlash != wxNOT_FOUND) ? urlText.Mid(lastSlash + 1) : urlText;
	const int lastDot = fileNameBase.Find('.', true);
	if (lastDot != wxNOT_FOUND)
		fileNameBase = fileNameBase.Left(lastDot);
	fileNameBase.Trim(true).Trim(false);
	if (!fileNameBase.IsEmpty())
		folderName = fileNameBase.ToStdString();

	if (m_thread.joinable())
		m_thread.join();

	m_thread = std::thread([this, downloadUrl = std::move(downloadUrl),
							folderName = std::move(folderName)]() mutable {
		try
		{
			UpdateThread(std::move(downloadUrl), std::move(folderName));
		} catch (const std::exception& error)
		{
			cemuLog_log(LogType::Force, "Custom graphic-pack worker failed: {}", error.what());
			if (!m_downloadState->isCanceled)
				m_stage = StageErrInvalidPack;
		} catch (...)
		{
			cemuLog_log(LogType::Force, "Custom graphic-pack worker failed with an unknown error");
			if (!m_downloadState->isCanceled)
				m_stage = StageErrInvalidPack;
		}
	});
}

void DownloadCustomGraphicPackWindow::UpdateThread(std::string downloadUrl, std::string folderName)
{
	m_stage = StageDownloading;
	if (!curlDownloadFile(downloadUrl.c_str(), m_downloadState.get()))
	{
		if (!m_downloadState->isCanceled)
			m_stage = StageErrConnectFailed;
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
		m_stage = StageErrSourceFailed;
		return;
	}
	zip_t* rawArchive = zip_open_from_source(source, ZIP_RDONLY, &zipError);
	if (!rawArchive)
	{
		zip_source_free(source);
		zip_error_fini(&zipError);
		m_stage = StageErrOpenFailed;
		return;
	}
	std::unique_ptr<zip_t, decltype(&zip_discard)> archive(rawArchive, &zip_discard);
	zip_error_fini(&zipError);

	const auto normalizedFolder = Frontend::ArchiveInstallPolicy::NormalizeRelativePath(folderName);
	if (!normalizedFolder || normalizedFolder->has_parent_path())
	{
		m_stage = StageErrInvalidPack;
		return;
	}
	zip_stat_t rulesStat{};
	if (zip_stat(archive.get(), "rules.txt", 0, &rulesStat) != 0)
	{
		m_stage = StageErrInvalidPack;
		return;
	}

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
	if (entryCount < 0 || static_cast<zip_uint64_t>(entryCount) > kMaximumCustomPackEntries)
	{
		m_stage = StageErrInvalidPack;
		return;
	}
	plan.reserve(static_cast<std::size_t>(entryCount));
	for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entryCount); ++index)
	{
		zip_stat_t stat{};
		if (zip_stat_index(archive.get(), index, 0, &stat) != 0 || !stat.name ||
			IsCustomPackArchiveSymlink(archive.get(), index))
		{
			m_stage = StageErrInvalidPack;
			return;
		}
		auto relativePath = Frontend::ArchiveInstallPolicy::NormalizeRelativePath(stat.name);
		if (!relativePath || !paths.emplace(*relativePath).second)
		{
			m_stage = StageErrInvalidPack;
			return;
		}
		const auto nameLength = std::strlen(stat.name);
		const bool directory = nameLength > 0 &&
							   (stat.name[nameLength - 1] == '/' || stat.name[nameLength - 1] == '\\');
		if (!directory)
		{
			if (stat.size > kMaximumCustomPackFileSize ||
				totalSize > kMaximumCustomPackTotalSize - stat.size ||
				(stat.comp_size > 0 && stat.size / stat.comp_size > kMaximumCustomPackCompressionRatio))
			{
				m_stage = StageErrInvalidPack;
				return;
			}
			totalSize += stat.size;
		}
		plan.push_back({index, std::move(*relativePath), stat.size, directory});
	}

	const auto customRoot = m_pathProvider->GetUserDataPath("graphicPacks/customGraphicPacks");
	const auto target = customRoot / *normalizedFolder;
	std::error_code filesystemError;
	fs::create_directories(customRoot, filesystemError);
	if (filesystemError)
	{
		m_stage = StageErrInvalidPack;
		return;
	}
	if (fs::exists(target, filesystemError) || filesystemError)
	{
		m_stage = StageErrConflict;
		return;
	}
	const auto staging = UniqueCustomPackSibling(target);
	fs::create_directories(staging, filesystemError);
	if (filesystemError)
	{
		m_stage = StageErrInvalidPack;
		return;
	}
	auto cleanupStaging = [&] {
		std::error_code cleanupError;
		fs::remove_all(staging, cleanupError);
	};

	bool failed = false;
	std::array<char, 1024 * 1024> buffer{};
	for (std::size_t planIndex = 0; planIndex < plan.size(); ++planIndex)
	{
		if (m_downloadState->isCanceled)
		{
			failed = true;
			break;
		}
		m_extractionProgress = plan.empty() ? 1.0 : static_cast<double>(planIndex) / static_cast<double>(plan.size());
		const auto& entry = plan[planIndex];
		const auto destination = staging / entry.relativePath;
		if (entry.directory)
		{
			fs::create_directories(destination, filesystemError);
			if (filesystemError)
			{
				failed = true;
				break;
			}
			continue;
		}
		fs::create_directories(destination.parent_path(), filesystemError);
		if (filesystemError)
		{
			failed = true;
			break;
		}
		std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(
			zip_fopen_index(archive.get(), entry.index, 0), &zip_fclose);
		std::ofstream output(destination, std::ios::binary | std::ios::trunc);
		if (!file || !output)
		{
			failed = true;
			break;
		}
		zip_uint64_t remaining = entry.size;
		while (remaining > 0)
		{
			if (m_downloadState->isCanceled)
			{
				failed = true;
				break;
			}
			const auto request = static_cast<zip_uint64_t>(
				std::min<zip_uint64_t>(buffer.size(), remaining));
			const auto read = zip_fread(file.get(), buffer.data(), request);
			if (read <= 0 || static_cast<zip_uint64_t>(read) > remaining)
			{
				failed = true;
				break;
			}
			output.write(buffer.data(), static_cast<std::streamsize>(read));
			if (!output)
			{
				failed = true;
				break;
			}
			remaining -= static_cast<zip_uint64_t>(read);
		}
		if (failed)
			break;
	}
	if (failed || m_downloadState->isCanceled)
	{
		cleanupStaging();
		if (!m_downloadState->isCanceled)
			m_stage = StageErrInvalidPack;
		return;
	}
	const auto commit = Frontend::ArchiveInstallPolicy::CommitStagedDirectory(
		staging, target, {}, false);
	if (!commit.committed)
	{
		cleanupStaging();
		m_stage = commit.error == Frontend::ArchiveInstallPolicy::CommitError::TargetExists ? StageErrConflict : StageErrInvalidPack;
		return;
	}
	m_extractionProgress = 1.0;
	m_stage = StageDone;
}
