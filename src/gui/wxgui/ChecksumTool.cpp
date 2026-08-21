#include "wxgui/ChecksumTool.h"

#include "application/EmulationController.h"
#include "host/contracts/HostContracts.h"
#include "wxgui/helpers/wxCustomEvents.h"
#include "util/helpers/helpers.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/wxHelper.h"

#include <zip.h>
#include <curl/curl.h>

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/schema.h>

#include <wx/frame.h>
#include <wx/translation.h>
#include <wx/xrc/xmlres.h>
#include <wx/gauge.h>
#include <wx/gdicmn.h>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/msgdlg.h>

const char kSchema[] = R"(
{
  "$schema": "http://json-schema.org/draft-04/schema#",
  "type": "object",
  "properties": {
    "title_id": {
      "type": "string"
    },
    "region": {
      "type": "integer"
    },
    "version": {
      "type": "integer"
    },
	"wud_hash": {
      "type": "string"
    },
    "files": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "file": {
            "type": "string"
          },
          "hash": {
            "type": "string"
          }
        },
        "required": [
          "file",
          "hash"
        ]
      }
    }
  },
  "required": [
    "title_id",
    "region",
    "version",
    "files"
  ]
})";

ChecksumTool::ChecksumTool(wxWindow* parent, Application::EmulationController& controller,
	Application::ManagedContentEntry entry,
	std::shared_ptr<Host::IPathProvider> pathProvider)
	: wxDialog(parent, wxID_ANY,
		formatWxString(_("Title checksum of {:08x}-{:08x}"),
			(uint32)(entry.titleId >> 32), (uint32)(entry.titleId & 0xFFFFFFFF)),
		wxDefaultPosition, wxDefaultSize, wxCAPTION | wxFRAME_TOOL_WINDOW |
			wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_controller(controller), m_entry(std::move(entry)),
	  m_pathProvider(std::move(pathProvider))
{
	cemu_assert(m_pathProvider != nullptr);

	auto* sizer = new wxBoxSizer(wxVERTICAL);
	{
		auto* box_sizer = new wxStaticBoxSizer(new wxStaticBox(this, wxID_ANY, _("Verifying integrity of game files...")), wxVERTICAL);
		auto* box = box_sizer->GetStaticBox();
		
		m_progress = new wxGauge(box, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize, wxGA_HORIZONTAL | wxGA_SMOOTH);
		m_progress->SetMinSize({ 400, -1 });
		m_progress->SetValue(0);
		box_sizer->Add(m_progress, 0, wxALL | wxEXPAND, 5);

		m_status = new wxStaticText(box, wxID_ANY, wxEmptyString);
		m_status->Wrap(-1);
		box_sizer->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

		sizer->Add(box_sizer, 0, wxEXPAND | wxALL, 5);
	}

	{
		auto* box_sizer = new wxStaticBoxSizer(new wxStaticBox(this, wxID_ANY, _("Control")), wxHORIZONTAL);
		auto* box = box_sizer->GetStaticBox();
		
		m_verify_online = new wxButton(box, wxID_ANY, _("Verify online"));
		m_verify_online->SetToolTip(_("Verifies the checksum online"));
		m_verify_online->Disable();
		m_verify_online->Bind(wxEVT_BUTTON, &ChecksumTool::OnVerifyOnline, this);
		m_verify_online->Bind(wxEVT_ENABLE, [this](wxCommandEvent&)
		{
			++m_enable_verify_button;
			if (m_enable_verify_button >= 2)
			{
				// only enable if we have a file for it
				const auto title_id_str = fmt::format("{:016x}", m_entry.titleId);
				const auto default_file = fmt::format("{}_v{}.json", title_id_str, m_entry.version);

				const auto checksum_path = m_pathProvider->GetUserDataPath(
					"resources/checksums") / _utf8ToPath(default_file);
				if (exists(checksum_path))
					m_verify_online->Enable();
			}
		});
		box_sizer->Add(m_verify_online, 0, wxALL | wxEXPAND, 5);
		
		m_verify_local = new wxButton(box, wxID_ANY, _("Verify with local file"));
		m_verify_online->SetToolTip(_("Verifies the checksum with a local JSON file you can select"));
		m_verify_local->Disable();
		m_verify_local->Bind(wxEVT_BUTTON, &ChecksumTool::OnVerifyLocal, this);
		box_sizer->Add(m_verify_local, 0, wxALL | wxEXPAND, 5);
		
		m_export_button = new wxButton(box, wxID_ANY, _("Export"));
		m_verify_online->SetToolTip(_("Export the title checksum data to a local JSON file"));
		m_export_button->Disable();
		m_export_button->Bind(wxEVT_BUTTON, &ChecksumTool::OnExportChecksums, this);
		box_sizer->Add(m_export_button, 0, wxALL | wxEXPAND, 5);

		sizer->Add(box_sizer, 0, wxEXPAND | wxALL, 5);
	}

	this->Bind(wxEVT_SET_GAUGE_VALUE, &ChecksumTool::OnSetGaugevalue, this);
	this->SetSizerAndFit(sizer);
	this->Centre(wxBOTH);

	// Start background work only after dialog construction can no longer throw.
	static std::atomic_bool s_updateStarted{};
	if (!s_updateStarted.exchange(true, std::memory_order_acq_rel))
		m_online_ready = std::async(std::launch::async, &ChecksumTool::LoadOnlineData, this);
	else
		m_enable_verify_button = 1;

	m_worker = std::thread([this] {
		try
		{
			DoWork();
		}
		catch (const std::exception& exception)
		{
			cemuLog_log(LogType::Force, "Checksum worker failed: {}", exception.what());
			wxQueueEvent(this, new wxSetGaugeValue(0, m_progress, m_status,
				wxString::FromUTF8(exception.what())));
		}
	});
}

ChecksumTool::~ChecksumTool()
{
	m_running = false;
	if (m_worker.joinable())
		m_worker.join();
	if (m_online_ready.valid())
	{
		try
		{
			m_online_ready.get();
		}
		catch (const std::exception& exception)
		{
			cemuLog_log(LogType::Force, "Checksum metadata worker failed: {}",
				exception.what());
		}
	}
	DeletePendingEvents();
}

std::size_t WriteCallback(const char* in, std::size_t size, std::size_t num, std::string* out)
{
	const std::size_t totalBytes(size * num);
	out->append(in, totalBytes);
	return totalBytes;
}

void ChecksumTool::LoadOnlineData() const
{
	try
	{
		bool updated_required = true;

		std::string latest_commit;

		const auto checksum_path = m_pathProvider->GetUserDataPath("resources/checksums");
		if (exists(checksum_path))
		{
			std::string current_commit;
			// check for current version
			std::ifstream file(checksum_path / "commit.txt");
			if (file.is_open())
			{
				std::getline(file, current_commit);
				file.close();
			}

			// check latest version
			/*
				https://api.github.com/repos/teamcemu/title-checksums/branches/master
				https://api.github.com/repos/teamcemu/title-checksums/commits?per_page=1
			*/
			std::string data;
			auto* curl = curl_easy_init();
			curl_easy_setopt(curl, CURLOPT_URL, "https://api.github.com/repos/teamcemu/title-checksums/commits/master");
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
			curl_easy_setopt(curl, CURLOPT_USERAGENT, BUILD_VERSION_WITH_NAME_STRING);

			curl_easy_perform(curl);

			long http_code = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
			curl_easy_cleanup(curl);
			if (http_code == 200 && !data.empty())
			{
				rapidjson::Document doc;
				doc.Parse(data.c_str(), data.size());
				if (!doc.HasParseError() && doc.HasMember("sha"))
				{
					latest_commit = doc["sha"].GetString();
					if (boost::iequals(current_commit, latest_commit))
					{
						updated_required = false;
					}
				}
			}
		}
		else
		{
			// create directory since not available yet
			fs::create_directories(checksum_path);
		}

		if (updated_required)
		{
			std::string data;
			auto* curl = curl_easy_init();
			curl_easy_setopt(curl, CURLOPT_URL, "https://github.com/TeamCemu/title-checksums/archive/master.zip");
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
			curl_easy_setopt(curl, CURLOPT_USERAGENT, BUILD_VERSION_WITH_NAME_STRING);

			curl_easy_perform(curl);

			long http_code = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
			curl_easy_cleanup(curl);
			if (http_code == 200 && !data.empty())
			{
				// init zip source
				zip_error_t error;
				zip_error_init(&error);
				zip_source_t* src;
				if ((src = zip_source_buffer_create(data.data(), data.size(), 1, &error)) == nullptr)
				{
					zip_error_fini(&error);
					return;
				}

				auto* za = zip_open_from_source(src, ZIP_RDONLY, &error);
				if (!za)
				{
					wxQueueEvent(m_verify_online, new wxCommandEvent(wxEVT_ENABLE));
					return;
				}

				const auto numEntries = zip_get_num_entries(za, 0);
				for (sint64 i = 0; i < numEntries; i++)
				{
					zip_stat_t sb = { 0 };
					if (zip_stat_index(za, i, 0, &sb) != 0)
						continue;

					if (std::strstr(sb.name, "../") != nullptr ||
						std::strstr(sb.name, "..\\") != nullptr)
						continue; // bad path

					if (boost::equals(sb.name, "title-checksums-master/"))
						continue;

					// title-checksums-master/
					const auto path = checksum_path / &sb.name[sizeof("title-checksums-master")];

					size_t sbNameLen = strlen(sb.name);
					if (sbNameLen == 0)
						continue;

					if (sb.name[sbNameLen - 1] == '/')
					{
						std::error_code ec;
						fs::create_directories(path, ec);
						continue;
					}

					if (sb.size == 0)
						continue;

					if (sb.size > (1024 * 1024 * 128))
						continue; // skip unusually huge files

					zip_file_t* zipFile = zip_fopen_index(za, i, 0);
					if (zipFile == nullptr)
						continue;

					std::vector<char> buffer(sb.size);
					if (zip_fread(zipFile, buffer.data(), sb.size) == sb.size)
					{
						std::ofstream file(path);
						if (file.is_open())
						{
							file.write(buffer.data(), sb.size);
							file.flush();
							file.close();
						}
					}

					zip_fclose(zipFile);
				}

				std::ofstream file(checksum_path / "commit.txt");
				if (file.is_open())
				{
					file << latest_commit;
					file.close();
				}
			}
		}
	}
	catch(const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "error on updating json checksum data: {}", ex.what());
	}
	
	wxQueueEvent(m_verify_online, new wxCommandEvent(wxEVT_ENABLE));
}

void ChecksumTool::OnSetGaugevalue(wxSetGaugeValue& event)
{
	event.GetGauge()->SetValue(event.GetValue());
	event.GetTextCtrl()->SetLabelText(event.GetText());

	// no error
	if(event.GetInt() == 0 && event.GetValue() == 100)
	{
		m_export_button->Enable();
		m_verify_local->Enable();
		wxPostEvent(m_verify_online, wxCommandEvent(wxEVT_ENABLE));
	}
}

void ChecksumTool::OnExportChecksums(wxCommandEvent& event)
{
	// TODO: merge if json already exists
	wxDirDialog dialog(this, _("Export checksum entry"), "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
	if (dialog.ShowModal() != wxID_OK || dialog.GetPath().IsEmpty())
		return;
	
	rapidjson::Document doc;
	doc.SetObject();
	auto& a = doc.GetAllocator();
	/*
		title_id
		region
		version
		wud_hash
		files:
		[
			{file, hash}
		]
	 */

	auto title_id_str = fmt::format("{:016x}", m_json_entry.title_id);
	doc.AddMember("title_id", rapidjson::StringRef(title_id_str.c_str(), title_id_str.size()), a);
	doc.AddMember("region", static_cast<int>(m_entry.region), a);
	doc.AddMember("version", m_entry.version, a);
	if (!m_json_entry.wud_hash.empty())
		doc.AddMember("wud_hash", rapidjson::StringRef(m_json_entry.wud_hash.c_str(), m_json_entry.wud_hash.size()), a);
	
	rapidjson::Value entry_array(rapidjson::kArrayType);

	rapidjson::Value file_array(rapidjson::kArrayType);
	for(const auto& file : m_json_entry.file_hashes)
	{
		rapidjson::Value file_entry;
		file_entry.SetObject();
		
		file_entry.AddMember("file", rapidjson::StringRef(file.first.c_str(), file.first.size()), a);
		file_entry.AddMember("hash", rapidjson::StringRef(file.second.c_str(), file.second.size()), a);

		file_array.PushBack(file_entry, a);
	}

	doc.AddMember("files", file_array, a);

	std::filesystem::path target_file{ dialog.GetPath().c_str().AsInternal() };
	target_file /= fmt::format("{}_v{}.json", title_id_str, m_entry.version);
	
	std::ofstream file(target_file);
	if(file.is_open())
	{
		rapidjson::OStreamWrapper osw(file);
		rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(osw);
		//rapidjson::GenericSchemaValidator<rapidjson::SchemaDocument, rapidjson::Writer<rapidjson::StringBuffer> > validator(schema, writer);
		doc.Accept(writer);
		wxMessageBox(_("Export successful"), wxMessageBoxCaptionStr, wxOK | wxCENTRE, this);
	}
	else
	{
		wxMessageBox(formatWxString(_("Can't write to file: {}"), target_file.string()), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
	}
}

void ChecksumTool::VerifyJsonEntry(const rapidjson::Document& doc)
{
	rapidjson::Document sdoc;
	sdoc.Parse(kSchema, std::size(kSchema));
	wxASSERT(!sdoc.HasParseError());
	rapidjson::SchemaDocument schema(sdoc);
	rapidjson::SchemaValidator validator(schema);
	if (!doc.Accept(validator))
	{
		//// validation error:
		//rapidjson::StringBuffer sb;
		//validator.GetInvalidSchemaPointer().StringifyUriFragment(sb);
		//printf("Invalid schema: %s\n", sb.GetString());
		//printf("Invalid keyword: %s\n", validator.GetInvalidSchemaKeyword());
		//sb.Clear();
		//validator.GetInvalidDocumentPointer().StringifyUriFragment(sb);
		//printf("Invalid document: %s\n", sb.GetString());
		///*
		//Invalid schema: #
		//Invalid keyword: required
		//Invalid document: #
		// */

		wxMessageBox(_("JSON file doesn't satisfy needed schema"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}

	try
	{
		JsonEntry test_entry{};
		test_entry.title_id = ConvertString<uint64>(doc["title_id"].GetString(), 16);
		test_entry.region = static_cast<std::uint32_t>(doc["region"].GetInt());
		test_entry.version = doc["version"].GetInt();
		if (doc.HasMember("wud_hash"))
			test_entry.wud_hash = doc["wud_hash"].GetString();

		for (const auto& v : doc["files"].GetArray())
		{
			std::filesystem::path genericFilePath(v["file"].GetString(), std::filesystem::path::generic_format); // convert path to generic form (forward slashes)
			test_entry.file_hashes[genericFilePath.generic_string()] = v["hash"].GetString();
		}

		if (m_json_entry.title_id != test_entry.title_id)
		{
			wxMessageBox(formatWxString(_("The file you are comparing with is for a different title.")), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
			return;
		}
		if (m_json_entry.version != test_entry.version)
		{
			wxMessageBox(formatWxString(_("Wrong version: {}"), test_entry.version), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
			return;
		}
		if (m_json_entry.region != test_entry.region)
		{
			wxMessageBox(formatWxString(_("Wrong region: {}"), test_entry.region), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
			return;
		}
		if (!m_json_entry.wud_hash.empty())
		{
			if (test_entry.wud_hash.empty())
			{
				wxMessageBox(_("The verification data doesn't include a WUD hash!"), _("Error"), wxOK | wxCENTRE | wxICON_WARNING, this);
				return;
			}
			if(!boost::iequals(test_entry.wud_hash, m_json_entry.wud_hash))
			{
				wxMessageBox(formatWxString(_("Your game image is invalid!\n\nYour hash:\n{}\n\nExpected hash:\n{}"), m_json_entry.wud_hash, test_entry.wud_hash), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
				return;
			}
		}
		else
		{
			std::map<std::string_view, std::pair<std::string, std::string>> invalid_hashes;
			std::vector<std::string_view> missing_files;
			const auto writeMismatchInfoToLog = [this, &missing_files, &invalid_hashes]()
			{
				wxFileDialog dialog(this, _("Select a file to export the errors"), wxEmptyString, wxEmptyString, "Error list (*.txt)|*.txt", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
				if (dialog.ShowModal() != wxID_OK || dialog.GetPath().IsEmpty())
					return;

				const std::string path = dialog.GetPath().utf8_string();
				std::ofstream file(path);
				if (file.is_open())
				{
					if (!missing_files.empty())
					{
						file << "The following files are missing:\n";
						for (const auto& f : missing_files)
							file << "\t" << f << "\n";

						file << "\n";
					}

					if (!invalid_hashes.empty())
					{
						file << "The following files have an invalid hash (name | current hash | expected hash):\n";
						for (const auto& f : invalid_hashes)
							file << "\t" << f.first << " | " << f.second.first << " | " << f.second.second << "\n";
					}
					file.flush();
					file.close();

					wxLaunchDefaultBrowser(formatWxString("file:{}", path));
				}
				else
					wxMessageBox(_("Can't open file to write!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
			};

			for (const auto& f : test_entry.file_hashes)
			{
				const auto it = m_json_entry.file_hashes.find(f.first);
				if (it == m_json_entry.file_hashes.cend())
				{
					missing_files.emplace_back(f.first);
				}
				else if (!boost::iequals(f.second, it->second))
				{
					invalid_hashes[f.first] = std::make_pair(it->second, f.second);
				}
			}

			// files are missing but rest is okay
			if ((!missing_files.empty() || !invalid_hashes.empty()) && (missing_files.size() + invalid_hashes.size()) < 30)
			{
				// the list of missing + invalid hashes is short enough that we can print it to the message box
				std::stringstream str;
				if (missing_files.size() > 0)
				{
					str << _("The following files are missing:").ToUTF8().data() << "\n";
					for (const auto& v : missing_files)
						str << v << "\n";
					if(invalid_hashes.size() > 0)
						str << "\n";
				}
				if (invalid_hashes.size() > 0)
				{
					str << _("The following files are damaged:").ToUTF8().data() << "\n";
					for (const auto& v : invalid_hashes)
						str << v.first << "\n";
				}

				wxMessageBox(str.str(), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
				return;
			}
			else if (missing_files.empty() && !invalid_hashes.empty())
			{
				const int result = wxMessageBox(formatWxString(
						_("{} files have an invalid hash!\nDo you want to export a list of them to a file?"),
						invalid_hashes.size()), _("Error"), wxYES_NO | wxCENTRE | wxICON_ERROR, this);
				if (result == wxYES)
				{
					writeMismatchInfoToLog();
				}
				return;
			}
			else if (!missing_files.empty() && !invalid_hashes.empty())
			{
				const int result = wxMessageBox(formatWxString(
						_("Multiple issues with your game files have been found!\nDo you want to export them to a file?"),
						invalid_hashes.size()), _("Error"), wxYES_NO | wxCENTRE | wxICON_ERROR, this);
				if (result == wxYES)
				{
					writeMismatchInfoToLog();
				}
				return;
			}	
		}
		wxMessageBox(_("Your game files are valid"), _("Success"), wxOK | wxCENTRE, this);
	}
	catch (const std::exception& ex)
	{
		wxMessageBox(formatWxString(_("JSON parse error: {}"), ex.what()), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
	}

}

void ChecksumTool::OnVerifyOnline(wxCommandEvent& event)
{
	const auto title_id_str = fmt::format("{:016x}", m_json_entry.title_id);
	const auto default_file = fmt::format("{}_v{}.json", title_id_str, m_entry.version);
	
	const auto checksum_path = m_pathProvider->GetUserDataPath(
		"resources/checksums") / _utf8ToPath(default_file);
	if(!exists(checksum_path))
		return;
	
	std::ifstream file(checksum_path);
	if (!file.is_open())
	{
		wxMessageBox(_("Can't open file!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}

	rapidjson::IStreamWrapper str(file);
	rapidjson::Document d;
	d.ParseStream(str);
	if (d.HasParseError())
	{
		wxMessageBox(_("Can't parse JSON file!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}

	VerifyJsonEntry(d);
}

void ChecksumTool::OnVerifyLocal(wxCommandEvent& event)
{
	const auto title_id_str = fmt::format("{:016x}", m_json_entry.title_id);
	const auto default_file = fmt::format("{}_v{}.json", title_id_str, m_entry.version);
	wxFileDialog file_dialog(this, _("Open checksum entry"), "", default_file.c_str(),"JSON files (*.json)|*.json", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (file_dialog.ShowModal() != wxID_OK || file_dialog.GetPath().IsEmpty())
		return;

	std::filesystem::path filename{ file_dialog.GetPath().c_str().AsInternal() };
	std::ifstream file(filename);
	if(!file.is_open())
	{
		wxMessageBox(_("Can't open file!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;	
	}
	
	rapidjson::IStreamWrapper str(file);
	rapidjson::Document d;
	d.ParseStream(str);
	if (d.HasParseError())
	{
		wxMessageBox(_("Can't parse JSON file!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}

	VerifyJsonEntry(d);
}

void ChecksumTool::RunChecksum()
{
	const auto result = m_controller.ComputeTitleChecksum(m_entry.locationUid,
		[this](const Application::ContentOperationProgress& progress) {
			int percent{};
			wxString status;
			if (progress.phase == Application::ContentOperationPhase::Collecting)
			{
				status = formatWxString(_("Collecting game files... ({})"),
					progress.filesTotal);
			}
			else if (progress.bytesTotal != 0)
			{
				percent = static_cast<int>(std::min<std::uint64_t>(99,
					progress.bytesCompleted * 100 / progress.bytesTotal));
				status = formatWxString(_("Reading game image: {0}/{1} kB"),
					progress.bytesCompleted / 1024, progress.bytesTotal / 1024);
			}
			else
			{
				percent = progress.filesTotal == 0 ? 0 : static_cast<int>(
					progress.filesCompleted * 100 / progress.filesTotal);
				status = formatWxString(_("Hashing game file: {}/{}"),
					progress.filesCompleted, progress.filesTotal);
			}
			wxQueueEvent(this, new wxSetGaugeValue(percent, m_progress, m_status, status));
		}, [this] { return !m_running.load(std::memory_order_acquire); });
	if (result.error == Application::ContentOperationError::Cancelled)
		return;
	if (!result)
		throw std::runtime_error(result.diagnostic);

	m_json_entry.title_id = result.checksum->titleId;
	m_json_entry.version = result.checksum->version;
	m_json_entry.region = result.checksum->region;
	m_json_entry.wud_hash = result.checksum->imageSha256;
	m_json_entry.file_hashes.clear();
	for (const auto& file : result.checksum->files)
		m_json_entry.file_hashes.emplace(file.path, file.sha256);
	wxQueueEvent(this, new wxSetGaugeValue(100, m_progress, m_status,
		m_json_entry.wud_hash.empty() ?
			formatWxString(_("Generated checksum of {} game files"),
				m_json_entry.file_hashes.size()) :
			_("Generated checksum of game image")));
}

void ChecksumTool::DoWork()
{
	RunChecksum();
}
