#include "wxgui/CemuUpdateWindow.h"
#include "frontend/ArchiveInstallPolicy.h"

#include "Common/version.h"
#include "util/helpers/helpers.h"
#include "util/helpers/SystemException.h"
#include "host/contracts/HostContracts.h"
#include "Common/FileStream.h"
#include "wxCemuConfig.h"

#include <wx/sizer.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>

#ifndef BOOST_OS_WINDOWS
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <curl/curl.h>
#include <zip.h>
#include <boost/tokenizer.hpp>
#include <fstream>


wxDECLARE_EVENT(wxEVT_RESULT, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_RESULT, wxCommandEvent);

wxDECLARE_EVENT(wxEVT_PROGRESS, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_PROGRESS, wxCommandEvent);

namespace
{
	constexpr zip_uint64_t kMaximumUpdateEntries = 20000;
	constexpr zip_uint64_t kMaximumUpdateFileSize = 1024ULL * 1024ULL * 1024ULL;
	constexpr zip_uint64_t kMaximumUpdateTotalSize = 4ULL * 1024ULL * 1024ULL * 1024ULL;
	constexpr zip_uint64_t kMaximumCompressionRatio = 1000;
	constexpr curl_off_t kMaximumUpdateDownloadSize = 2LL * 1024LL * 1024LL * 1024LL;

	bool IsZipSymlink(zip_t* archive, zip_uint64_t index)
	{
		zip_uint8_t operatingSystem{};
		zip_uint32_t attributes{};
		if (zip_file_get_external_attributes(archive, index, 0, &operatingSystem, &attributes) != 0)
			return true;
		return operatingSystem == ZIP_OPSYS_UNIX && ((attributes >> 16U) & 0170000U) == 0120000U;
	}
}

CemuUpdateWindow::CemuUpdateWindow(wxWindow* parent,
	std::shared_ptr<Host::IPathProvider> pathProvider)
	: wxDialog(parent, wxID_ANY, _("Cemu update"), wxDefaultPosition, wxDefaultSize,
		wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_pathProvider(std::move(pathProvider))
{
	cemu_assert(static_cast<bool>(m_pathProvider));
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(500, 20), wxGA_HORIZONTAL);
	m_gauge->SetValue(0);
	sizer->Add(m_gauge, 0, wxALL | wxEXPAND, 5);

	auto* rows = new wxFlexGridSizer(0, 2, 0, 0);
	rows->AddGrowableCol(1);

	m_text = new wxStaticText(this, wxID_ANY, _("Checking for latest version..."));
	rows->Add(m_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	{
		auto* right_side = new wxBoxSizer(wxHORIZONTAL);

		m_updateButton = new wxButton(this, wxID_ANY, _("Update"));
		m_updateButton->Bind(wxEVT_BUTTON, &CemuUpdateWindow::OnUpdateButton, this);
		right_side->Add(m_updateButton, 0, wxALL, 5);

		m_cancelButton = new wxButton(this, wxID_ANY, _("Cancel"));
		m_cancelButton->Bind(wxEVT_BUTTON, &CemuUpdateWindow::OnCancelButton, this);
		right_side->Add(m_cancelButton, 0, wxALL, 5);

		rows->Add(right_side, 1, wxALIGN_RIGHT, 5);
	}

	m_changelog = new wxHyperlinkCtrl(this, wxID_ANY, _("Changelog"), wxEmptyString);
	rows->Add(m_changelog, 0, wxLEFT | wxBOTTOM | wxRIGHT | wxEXPAND, 5);

	sizer->Add(rows, 0, wxALL | wxEXPAND, 5);

	SetSizerAndFit(sizer);
	Centre(wxBOTH);

	Bind(wxEVT_CLOSE_WINDOW, &CemuUpdateWindow::OnClose, this);
	Bind(wxEVT_RESULT, &CemuUpdateWindow::OnResult, this);
	Bind(wxEVT_PROGRESS, &CemuUpdateWindow::OnGaugeUpdate, this);
	m_thread = std::thread(&CemuUpdateWindow::WorkerThread, this);

	m_updateButton->Hide();
	m_changelog->Hide();
}

CemuUpdateWindow::~CemuUpdateWindow()
{
	m_workerMailbox.RequestStop();
	if (m_thread.joinable())
		m_thread.join();
	DeletePendingEvents();
}

size_t CemuUpdateWindow::WriteStringCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	((std::string*)userdata)->append(ptr, size * nmemb);
	return size * nmemb;
};

std::string _curlUrlEscape(CURL* curl, const std::string& input)
{
	char* escapedStr = curl_easy_escape(curl, input.c_str(), input.size());
	std::string r(escapedStr);
	curl_free(escapedStr);
	return r;
}

std::string _curlUrlUnescape(CURL* curl, std::string_view input)
{
	int decodedLen = 0;
	const char* decoded = curl_easy_unescape(curl, input.data(), input.size(), &decodedLen);
	return std::string(decoded, decodedLen);
}

// returns true if update is available and sets output parameters
bool CemuUpdateWindow::QueryUpdateInfo(std::string& downloadUrlOut, std::string& changelogUrlOut)
{
	std::string buffer;
	std::string urlStr("https://cemu.info/api2/version.php?v=");
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
	if (!curl)
		return false;
	urlStr.append(_curlUrlEscape(curl.get(), BUILD_VERSION_STRING));
#if BOOST_OS_LINUX || BOOST_OS_BSD // dummy placeholder on BSD for now
	urlStr.append("&platform=linux_appimage");
#elif BOOST_OS_WINDOWS
	urlStr.append("&platform=windows");
#elif BOOST_OS_MACOS
	urlStr.append("&platform=macos_bundle");
#else
#error Name for current platform is missing
#endif
#if defined(__aarch64__)
	urlStr.append("_aarch64");
#elif defined(ARCH_X86_64)
	urlStr.append("_x86_64");
#else
	urlStr.append("_unknown");
#endif
#if BOOST_OS_BSD
	return false; // BSD users must update from source; no binary available
#endif

	const auto& config = GetWxGUIConfig();
	if(config.receive_untested_updates)
		urlStr.append("&allowNewUpdates=1");

	curl_easy_setopt(curl.get(), CURLOPT_URL, urlStr.c_str());
	curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteStringCallback);
	curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);

	bool result = false;
	CURLcode cr = curl_easy_perform(curl.get());
	if (cr == CURLE_OK)
	{
		long http_code = 0;
		curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code != 0 && http_code != 200)
		{
			cemuLog_log(LogType::Force, "Update check failed (http code: {})", http_code);
			cemu_assert_debug(false);
			return false;
		}

		std::vector<std::string> tokens;
		const boost::char_separator<char> sep{ "|" };
		for (const auto& token : boost::tokenizer(buffer, sep))
			tokens.emplace_back(token);

		if (tokens.size() >= 3 && tokens[0] == "UPDATE")
		{
			// first token: "UPDATE"
			// second token: Download URL
			// third token: Changelog URL
			// we allow more tokens in case we ever want to add extra information for future releases
			downloadUrlOut = _curlUrlUnescape(curl.get(), tokens[1]);
			changelogUrlOut = _curlUrlUnescape(curl.get(), tokens[2]);
			if (!downloadUrlOut.empty() && !changelogUrlOut.empty())
				result = true;
		}
	}
	else
	{
		cemuLog_log(LogType::Force, "Update check failed with CURL error {}", (int)cr);
		cemu_assert_debug(false);
	}

	return result;
}

std::future<bool> CemuUpdateWindow::IsUpdateAvailableAsync()
{
	return std::async(std::launch::async, CheckVersion);
}

bool CemuUpdateWindow::CheckVersion()
{
	std::string downloadUrl, changelogUrl;
	return QueryUpdateInfo(downloadUrl, changelogUrl);
}


int CemuUpdateWindow::ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
	curl_off_t ulnow)
{
	auto* thisptr = (CemuUpdateWindow*)clientp;
	if (thisptr->m_workerMailbox.StopRequested() ||
		dlnow > kMaximumUpdateDownloadSize || dltotal > kMaximumUpdateDownloadSize)
		return 1;
	auto* event = new wxCommandEvent(wxEVT_PROGRESS);
	event->SetInt((int)dlnow);
	wxQueueEvent(thisptr, event);
	return 0;
}

bool CemuUpdateWindow::DownloadCemuZip(const std::string& url, const fs::path& filename)
{
	std::unique_ptr<FileStream> fsUpdateFile(FileStream::createFile2(filename));
	if (!fsUpdateFile)
		return false;

	bool result = false;
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
	if (!curl)
		return false;
	curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, BUILD_VERSION_WITH_NAME_STRING);
	curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 30L);
	curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 15L);
	auto r = curl_easy_perform(curl.get());
	if (r == CURLE_OK)
	{
		long http_code = 0;
		curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code != 0 && http_code != 200)
		{
			cemuLog_log(LogType::Force, "Unable to download cemu update zip file from {} (http error: {})", url, http_code);
			return false;
		}

		curl_off_t update_size{};
		if (curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &update_size) == CURLE_OK &&
			update_size > 0 && update_size <= std::numeric_limits<int>::max())
			m_gaugeMaxValue.store((int)update_size, std::memory_order_release);


		auto _curlWriteData = +[](void* ptr, size_t size, size_t nmemb, void* ctx) -> size_t
		{
			FileStream* fs = (FileStream*)ctx;
			const size_t writeSize = size * nmemb;
			fs->writeData(ptr, writeSize);
			return writeSize;
		};

		curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 0L);
		curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, _curlWriteData);
		curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, fsUpdateFile.get());
		curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, ProgressCallback);
		curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, this);

		result = curl_easy_perform(curl.get()) == CURLE_OK;
	}
	else
	{
		cemuLog_log(LogType::Force, "Cemu zip download failed with error {}", r);
	}

	if (!result && fs::exists(filename))
	{
		try
		{
			fs::remove(filename);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "can't remove update.zip on error: {}", ex.what());
		}
	}
	return result;
}

bool CemuUpdateWindow::ExtractUpdate(const fs::path& zipname, const fs::path& targetpath, std::string& cemuFolderName)
{
	cemuFolderName.clear();
	// open downloaded zip
	int err;
	auto* rawArchive = zip_open(zipname.string().c_str(), ZIP_RDONLY, &err);
	if (rawArchive == nullptr)
	{
		cemuLog_log(LogType::Force, "Cannot open zip file: {}", zipname.string());
		return false;
	}
	std::unique_ptr<zip_t, decltype(&zip_discard)> archive(rawArchive, &zip_discard);

	const auto count = zip_get_num_entries(archive.get(), 0);
	if (count < 0 || static_cast<zip_uint64_t>(count) > kMaximumUpdateEntries)
		return false;

	struct PlannedEntry
	{
		zip_uint64_t index{};
		fs::path relativePath;
		zip_uint64_t size{};
		bool directory{};
	};
	std::vector<PlannedEntry> plan;
	plan.reserve(static_cast<std::size_t>(count));
	zip_uint64_t totalSize{};
	std::optional<fs::path> rootDirectory;
	for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(count); ++index)
	{
		zip_stat_t stat{};
		if (zip_stat_index(archive.get(), index, 0, &stat) != 0 || stat.name == nullptr ||
			IsZipSymlink(archive.get(), index))
			return false;
		const auto relativePath = Frontend::ArchiveInstallPolicy::NormalizeRelativePath(stat.name);
		if (!relativePath)
			return false;
		const bool directory = stat.name[std::strlen(stat.name) - 1] == '/' ||
			stat.name[std::strlen(stat.name) - 1] == '\\';
		if (!directory)
		{
			if (stat.size > kMaximumUpdateFileSize || totalSize > kMaximumUpdateTotalSize - stat.size)
				return false;
			if (stat.comp_size > 0 && stat.size / stat.comp_size > kMaximumCompressionRatio)
				return false;
			totalSize += stat.size;
		}
		const auto firstComponent = *relativePath->begin();
		if (!rootDirectory)
			rootDirectory = firstComponent;
		else if (*rootDirectory != firstComponent)
			return false;
		plan.push_back({index, *relativePath, stat.size, directory});
	}
	if (!rootDirectory)
		return false;
	cemuFolderName = _pathToUtf8(*rootDirectory);

	m_gaugeMaxValue.store((int)count, std::memory_order_release);
	for (std::size_t planIndex = 0; planIndex < plan.size(); ++planIndex)
	{
		if (m_workerMailbox.StopRequested())
			return false;
		const auto& entry = plan[planIndex];
		const fs::path destination = targetpath / entry.relativePath;
		std::error_code filesystemError;
		if (entry.directory)
		{
			fs::create_directories(destination, filesystemError);
			if (filesystemError)
				return false;
			continue;
		}
		fs::create_directories(destination.parent_path(), filesystemError);
		if (filesystemError)
			return false;
		std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(
			zip_fopen_index(archive.get(), entry.index, 0), &zip_fclose);
		if (!file)
			return false;
		std::ofstream output(destination, std::ios::binary | std::ios::trunc);
		if (!output)
			return false;
		std::array<char, 1024 * 1024> buffer{};
		zip_uint64_t remaining = entry.size;
		while (remaining > 0)
		{
			if (m_workerMailbox.StopRequested())
				return false;
			const auto request = static_cast<zip_uint64_t>(std::min<std::size_t>(buffer.size(), remaining));
			const auto read = zip_fread(file.get(), buffer.data(), request);
			if (read <= 0 || static_cast<zip_uint64_t>(read) > remaining)
				return false;
			output.write(buffer.data(), read);
			if (!output)
				return false;
			remaining -= static_cast<zip_uint64_t>(read);
		}
		output.close();
		if (!output)
			return false;
		if ((planIndex % 10) == 0)
		{
			auto* event = new wxCommandEvent(wxEVT_PROGRESS);
			event->SetInt(static_cast<int>(planIndex));
			wxQueueEvent(this, event);
		}
	}

	auto* event = new wxCommandEvent(wxEVT_PROGRESS);
	event->SetInt(m_gaugeMaxValue.load(std::memory_order_acquire));
	wxQueueEvent(this, event);

	return true;
}

void CemuUpdateWindow::WorkerThread()
{
	const auto tmppath = fs::temp_directory_path() / L"cemu_update";
	std::error_code ec;
	// clean leftovers
	if (exists(tmppath))
		remove_all(tmppath, ec);

	while (true)
	{
		const auto order = m_workerMailbox.Wait();
		if (!order)
			break;

		try
		{
			if (*order == CemuUpdateWorkerMailbox::Work::CheckVersion)
			{
				if (m_workerMailbox.StopRequested())
					break;
				auto* event = new wxCommandEvent(wxEVT_RESULT);
				if (QueryUpdateInfo(m_downloadUrl, m_changelogUrl))
					event->SetInt((int)Result::UpdateAvailable);
				else
					event->SetInt((int)Result::NoUpdateAvailable);

				wxQueueEvent(this, event);
			}
			else if (*order == CemuUpdateWorkerMailbox::Work::UpdateVersion)
			{
				// download update
				const std::string url = m_downloadUrl;
				if (!exists(tmppath))
					create_directory(tmppath);

#if BOOST_OS_WINDOWS
				const auto update_file = tmppath / L"update.zip";
#elif BOOST_OS_LINUX || BOOST_OS_BSD // dummy placeholder on BSD for now
				const auto update_file = tmppath / L"Cemu.AppImage";
#elif BOOST_OS_MACOS
				const auto update_file = tmppath / L"cemu.dmg";
#endif	
				if (DownloadCemuZip(url, update_file))
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::UpdateDownloaded);
					wxQueueEvent(this, event);
				}
				else
				{
					if (m_workerMailbox.StopRequested())
						break;
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::UpdateDownloadError);
					wxQueueEvent(this, event);
					continue;
				}
				if (m_workerMailbox.StopRequested())
					break;

				// extract
				std::string cemuFolderName;
#if BOOST_OS_WINDOWS
				if (!ExtractUpdate(update_file, tmppath, cemuFolderName))
				{
					cemuLog_log(LogType::Force, "Extracting Cemu zip failed");
					if (m_workerMailbox.StopRequested())
						break;
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::ExtractError);
					wxQueueEvent(this, event);
					continue;
				}
				if (cemuFolderName.empty())
				{
					cemuLog_log(LogType::Force, "Cemu folder not found in zip");
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::ExtractError);
					wxQueueEvent(this, event);
					continue;
				}
#endif
				const auto expected_path = tmppath / cemuFolderName;
				if (exists(expected_path) && (!BOOST_OS_WINDOWS || is_directory(expected_path)))
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::ExtractSuccess);
					wxQueueEvent(this, event);
				}
				else
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::ExtractError);
					wxQueueEvent(this, event);

					if (exists(tmppath))
					{
						try
						{
							fs::remove(tmppath);
						}
						catch (const std::exception& ex)
						{
							SystemException sys(ex);
							cemuLog_log(LogType::Force, "can't remove extracted tmp files: {}", sys.what());
						}
					}

					continue;
				}

				if (m_workerMailbox.StopRequested())
					break;

				// apply update
				fs::path exePath = m_pathProvider->GetExecutablePath();
#if BOOST_OS_WINDOWS
				std::wstring target_directory = exePath.parent_path().generic_wstring();
				if (target_directory[target_directory.size() - 1] == '/')
					target_directory = target_directory.substr(0, target_directory.size() - 1); // remove trailing /

				// get exe name
				const auto exec = m_pathProvider->GetExecutablePath();
				const auto target_exe = fs::path(exec).replace_extension("exe.backup");
				fs::rename(exec, target_exe);
				m_restartFile = exec;				
#elif BOOST_OS_LINUX
				const char* appimage_path = std::getenv("APPIMAGE");
				const auto target_exe = fs::path(appimage_path).replace_extension("AppImage.backup");
				const char* filePath = update_file.c_str();
				mode_t permissions = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
				fs::rename(appimage_path, target_exe);
				m_restartFile = appimage_path;
				chmod(filePath, permissions);
				wxString wxAppPath = wxString::FromUTF8(appimage_path);
				wxCopyFile (wxT("/tmp/cemu_update/Cemu.AppImage"), wxAppPath);
#endif
#if BOOST_OS_WINDOWS
				const auto index = expected_path.wstring().size();
				int counter = 0;
				for (const auto& it : fs::recursive_directory_iterator(expected_path))
				{
					const auto filename = it.path().wstring().substr(index);
					auto target_file = target_directory + filename;
					try
					{
						if (is_directory(it))
						{
							if (!fs::exists(target_file))
								fs::create_directory(target_file);
						}
						else
						{
							if (it.path().filename() == L"Cemu.exe")
								fs::rename(it.path(), fs::path(target_file).replace_filename(exec.filename()));
							else
								fs::rename(it.path(), target_file);
						}
					}
					catch (const std::exception& ex)
					{
						SystemException sys(ex);
						cemuLog_log(LogType::Force, "applying update error: {}", sys.what());
					}

					if ((counter++ % 10) == 0)
					{
						auto* event = new wxCommandEvent(wxEVT_PROGRESS);
						event->SetInt(counter);
						wxQueueEvent(this, event);
					}
				}
#endif
				auto* event = new wxCommandEvent(wxEVT_PROGRESS);
			event->SetInt(m_gaugeMaxValue.load(std::memory_order_acquire));
				wxQueueEvent(this, event);

				auto* result_event = new wxCommandEvent(wxEVT_RESULT);
				result_event->SetInt((int)Result::Success);
				wxQueueEvent(this, result_event);
			}
		}
		catch (const std::exception& ex)
		{
			SystemException sys(ex);
			cemuLog_log(LogType::Force, "update error: {}", sys.what());

			// clean leftovers
			if (exists(tmppath))
				remove_all(tmppath, ec);

			if (!m_workerMailbox.StopRequested())
			{
				auto* result_event = new wxCommandEvent(wxEVT_RESULT);
				result_event->SetInt((int)Result::Error);
				wxQueueEvent(this, result_event);
			}
		}

	}
}

void CemuUpdateWindow::OnClose(wxCloseEvent& event)
{
	event.Skip();

#if BOOST_OS_WINDOWS
	if (m_restartRequired && !m_restartFile.empty() && fs::exists(m_restartFile))
	{
		PROCESS_INFORMATION pi{};
		STARTUPINFOW si{};
		si.cb = sizeof(si);

		std::wstring cmdline = GetCommandLineW();
		const auto index = cmdline.find('"', 1);
		cemu_assert_debug(index != std::wstring::npos);
		cmdline = L"\"" + boost::nowide::widen(_pathToUtf8(m_restartFile)) + L"\"" + cmdline.substr(index + 1);

		HANDLE lock = CreateMutexW(nullptr, TRUE, L"Global\\cemu_update_lock");
		CreateProcessW(nullptr, (wchar_t*)cmdline.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);

		exit(0);
	}
#elif BOOST_OS_LINUX
	if (m_restartRequired && !m_restartFile.empty() && fs::exists(m_restartFile))
	{
		const char* appimage_path = std::getenv("APPIMAGE");
		execlp(appimage_path, appimage_path, (char *)NULL);

		exit(0);
	}
#elif BOOST_OS_MACOS
	if (m_restartRequired)
	{
	    const auto tmppath = fs::temp_directory_path() / L"cemu_update/Cemu.dmg";
	    fs::path exePath = m_pathProvider->GetExecutablePath().parent_path();
		const auto appResources = exePath.parent_path().parent_path() / L"Resources";
		const auto apppath = appResources / L"update.sh";
	    execlp("sh", "sh", apppath.c_str(), NULL);
        
        exit(0);
	}	
#endif
}


void CemuUpdateWindow::OnResult(wxCommandEvent& event)
{
	switch ((Result)event.GetInt())
	{
	case Result::NoUpdateAvailable:
		m_cancelButton->SetLabel(_("Exit"));
		m_text->SetLabel(_("No update available!"));
		m_gauge->SetValue(100);
		break;
	case Result::UpdateAvailable:
	{
		if (!m_changelogUrl.empty())
		{
			m_changelog->SetURL(m_changelogUrl);
			m_changelog->Show();
		}
		else
			m_changelog->Hide();

		m_updateButton->Show();

		m_text->SetLabel(_("Update available!"));
		m_cancelButton->SetLabel(_("Exit"));
		break;
	}
	case Result::UpdateDownloaded:
		m_text->SetLabel(_("Extracting update..."));
		m_gauge->SetValue(0);
		break;
	case Result::UpdateDownloadError:
		m_updateButton->Enable();
		m_text->SetLabel(_("Couldn't download the update!"));
		break;
	case Result::ExtractSuccess:
		m_text->SetLabel(_("Applying update..."));
		m_gauge->SetValue(0);
		m_cancelButton->Disable();
		break;
	case Result::ExtractError:
		m_updateButton->Enable();
		m_cancelButton->Enable();
		m_text->SetLabel(_("Extracting failed!"));
		break;
	case Result::Success:
		m_cancelButton->Enable();
		m_updateButton->Hide();

		m_text->SetLabel(_("Success"));
		m_cancelButton->SetLabel(_("Restart"));
		m_restartRequired = true;
		break;
	default:;
	}
}

void CemuUpdateWindow::OnGaugeUpdate(wxCommandEvent& event)
{
	const int gaugeMaxValue = m_gaugeMaxValue.load(std::memory_order_acquire);
	const int total_size = gaugeMaxValue > 0 ? gaugeMaxValue : 10000000;
	m_gauge->SetValue((event.GetInt() * 100) / total_size);
}

void CemuUpdateWindow::OnUpdateButton(const wxCommandEvent& event)
{
	if (!m_workerMailbox.Request(CemuUpdateWorkerMailbox::Work::UpdateVersion))
		return;

	m_updateButton->Disable();

	m_text->SetLabel(_("Downloading update..."));
}

void CemuUpdateWindow::OnCancelButton(const wxCommandEvent& event)
{
	Close();
}
