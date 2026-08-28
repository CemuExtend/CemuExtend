#include "CemodPermissionDialog.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

CemodPermissionDialog::CemodPermissionDialog(wxWindow* parent, const wxString& gameName,
											 std::vector<Application::CemodPermissionRequest> requests)
	: CemodPermissionDialog(parent, gameName, [&requests] {
		  std::vector<CemodPermissionDialogEntry> entries;
		  for (auto& request : requests)
		  {
			  CemodPermissionDialogEntry entry{};
			  entry.modId = request.modId;
			  entry.principal = request.principal;
			  entry.approvalKey = request.principal;
			  entry.modIdentity = request.modId;
			  entry.executionMode = request.executionMode == Application::CemodExecutionMode::TrustedNative ? CemodGuiExecutionMode::TrustedNative : CemodGuiExecutionMode::Isolated;
			  entry.signedPackage = request.signedPackage;
			  static constexpr std::array labels{
				  "Read host state", "Write Mod storage/logging", "Input injection",
				  "Clipboard", "Capture", "Network"};
			  for (std::size_t index = 0; index < labels.size(); ++index)
				  if ((request.requestedPermissions & (1U << index)) != 0)
					  entry.permissions.push_back({static_cast<CemodGuiPermission>(index), labels[index],
												   1ULL << index, true, (request.grantedPermissions & (1U << index)) != 0,
												   index > 0, false});
			  entries.push_back(std::move(entry));
		  }
		  return entries;
	  }())
{
}

CemodPermissionDialog::CemodPermissionDialog(wxWindow* parent, const wxString& gameName,
											 std::vector<CemodPermissionDialogEntry> entries)
	: wxDialog(parent, wxID_ANY, _("Mod permissions required"), wxDefaultPosition,
			   wxSize(720, 600), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	auto* root = new wxBoxSizer(wxVERTICAL);
	root->Add(new wxStaticText(this, wxID_ANY,
							   wxString::Format(_("Enabled Mods require permission settings before %s can start."),
												gameName.c_str())),
			  0, wxEXPAND | wxALL, 12);
	root->Add(new wxStaticText(this, wxID_ANY,
							   _("Unchecked permissions remain denied. The request will be shown again on the next launch while permissions are missing.")),
			  0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

	auto* scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
										wxVSCROLL | wxBORDER_NONE);
	scroll->SetScrollRate(0, FromDIP(10));
	auto* mods = new wxBoxSizer(wxVERTICAL);

	for (auto& entry : entries)
	{
		const auto mode = entry.executionMode == CemodGuiExecutionMode::TrustedNative ? _("trusted native") : _("isolated");
		const auto signature = entry.signedPackage ? _("signed") : _("unsigned");
		const auto packageLabel = wxString::FromUTF8(entry.modId) + " — " + mode + " / " + signature;
		auto* box = new wxStaticBoxSizer(wxVERTICAL, scroll, packageLabel);
		auto* boxParent = box->GetStaticBox();

		if (entry.executionMode == CemodGuiExecutionMode::TrustedNative)
		{
			auto* warning = new wxStaticText(boxParent, wxID_ANY,
											 _("Warning: this trusted native Mod executes in the game address space and can access all game memory and Cafe/GX2 APIs."));
			warning->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT));
			box->Add(warning, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		}

		ModRow row{std::move(entry)};
		if (!row.entry.payloadDetails.empty())
			box->Add(new wxStaticText(boxParent, wxID_ANY,
									  wxString::FromUTF8(row.entry.payloadDetails)),
					 0,
					 wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		for (const auto* details : {&row.entry.metadataDetails, &row.entry.scopeDetails,
									&row.entry.moduleDetails})
			if (!details->empty())
				box->Add(new wxStaticText(boxParent, wxID_ANY, wxString::FromUTF8(*details)), 0,
						 wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		for (const auto& warningText : row.entry.warnings)
		{
			auto* warning = new wxStaticText(boxParent, wxID_ANY, wxString::FromUTF8(warningText));
			warning->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT));
			box->Add(warning, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		}
		wxString missing;
		for (const auto& permission : row.entry.permissions)
		{
			auto label = wxString::FromUTF8(permission.label);
			if (permission.dangerous)
				label += _(" (dangerous; denied by default)");
			auto* checkbox = new wxCheckBox(boxParent, wxID_ANY, label);
			checkbox->SetValue(permission.granted && !row.entry.headless);
			row.permissions.push_back(checkbox);
			box->Add(checkbox, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
			if (!permission.granted)
			{
				if (!missing.empty())
					missing += ", ";
				missing += label;
			}
		}
		if (row.entry.headless)
		{
			auto* denied = new wxStaticText(boxParent, wxID_ANY,
											_("Headless contract: explicit approval is required in the GUI; this request is denied."));
			denied->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT));
			box->Add(denied, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		}
		if (!missing.empty())
		{
			auto* missingText = new wxStaticText(boxParent, wxID_ANY,
												 wxString::Format(_("Currently not allowed: %s"), missing.c_str()));
			missingText->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT));
			box->Add(missingText, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		}
		mods->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		m_rows.push_back(std::move(row));
	}
	scroll->SetSizer(mods);
	root->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

	auto* buttons = new wxBoxSizer(wxHORIZONTAL);
	auto* allowAll = new wxButton(this, wxID_ANY, _("Allow all requested"));
	auto* cancel = new wxButton(this, wxID_CANCEL, _("Cancel"));
	auto* start = new wxButton(this, wxID_OK, _("Save permissions and start game"));
	buttons->Add(allowAll, 0, wxRIGHT, 8);
	buttons->AddStretchSpacer();
	buttons->Add(cancel, 0, wxRIGHT, 8);
	buttons->Add(start, 0);
	root->Add(buttons, 0, wxEXPAND | wxALL, 12);

	allowAll->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AllowAll(); });
	start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveAndClose(); });
	start->SetDefault();
	SetSizer(root);
	SetMinSize(wxSize(620, 440));
	CentreOnParent();
}

void CemodPermissionDialog::AllowAll()
{
	for (auto& row : m_rows)
		for (auto* checkbox : row.permissions)
			if (checkbox)
				checkbox->SetValue(true);
}

void CemodPermissionDialog::SaveAndClose()
{
	m_selections.clear();
	for (const auto& row : m_rows)
	{
		std::uint32_t granted{};
		std::uint64_t grantedNative{};
		for (std::size_t index = 0; index < row.permissions.size(); ++index)
			if (row.permissions[index] && row.permissions[index]->IsChecked())
			{
				const auto bit = row.entry.permissions[index].bit;
				grantedNative |= bit;
				if (bit <= 0x20)
					granted |= static_cast<std::uint32_t>(bit);
			}
		std::uint32_t requestedLegacy{};
		for (const auto& permission : row.entry.permissions)
			if (permission.bit <= 0x20)
				requestedLegacy |= static_cast<std::uint32_t>(permission.bit);
		m_selections.push_back({row.entry.principal, requestedLegacy, granted,
								row.entry.approvalKey, row.entry.packageDigest, row.entry.modIdentity,
								requestedLegacy == 0 ? 0 : static_cast<std::uint64_t>(requestedLegacy),
								grantedNative, row.entry.headless});
	}
	EndModal(wxID_OK);
}
