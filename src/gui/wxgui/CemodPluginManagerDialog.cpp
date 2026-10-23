#include "CemodPluginManagerDialog.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <sstream>

namespace
{
	std::string ApprovalLabel(const Application::CemodManagerPackage& package)
	{
		std::string label = package.pluginName.empty() ? package.modId : package.pluginName;
		if (label.empty())
			label = package.packageKey;
		if (!package.valid)
			label += " — rejected";
		else if (!package.enabled)
			label += " — disabled";
		else
			label += package.approved ? " — approved" : " — approval required";
		return label;
	}

	std::string TitleIdsText(const std::vector<std::uint64_t>& titleIds)
	{
		std::ostringstream result;
		for (std::size_t index = 0; index < titleIds.size(); ++index)
		{
			if (index != 0)
				result << ", ";
			result << std::hex << titleIds[index];
		}
		return result.str();
	}
} // namespace

CemodPluginManagerDialog::CemodPluginManagerDialog(wxWindow* parent, std::uint64_t titleId,
												   Application::EmulationController& emulationController)
	: wxDialog(parent, wxID_ANY, _("Aroma / WUPS package manager"), wxDefaultPosition,
			   wxSize(1000, 780), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
	  m_titleId(titleId),
	  m_emulationController(emulationController)
{
	auto* root = new wxBoxSizer(wxVERTICAL);
	root->Add(new wxStaticText(this, wxID_ANY,
							   _("WUPS is a payload format, not an execution sandbox. Approvals are bound to the exact package digest: a rebuilt .cemod needs a new approval unless update trust covers it.")),
			  0, wxEXPAND | wxALL, 10);

	auto* body = new wxBoxSizer(wxHORIZONTAL);
	m_plugins = new wxListBox(this, wxID_ANY);
	body->Add(m_plugins, 0, wxEXPAND | wxLEFT | wxBOTTOM, 10);

	auto* right = new wxBoxSizer(wxVERTICAL);
	m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
	right->Add(m_status, 0, wxEXPAND | wxRIGHT | wxBOTTOM, 8);
	m_details = new wxTextCtrl(this, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
							   wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
	right->Add(m_details, 1, wxEXPAND | wxRIGHT | wxBOTTOM, 8);
	right->Add(new wxStaticText(this, wxID_ANY, _("Requested permissions (the runtime skips the package while any is denied):")),
			   0, wxEXPAND | wxRIGHT | wxBOTTOM, 6);
	m_permissions = new wxCheckListBox(this, wxID_ANY);
	m_permissions->SetMinSize(wxSize(-1, 330));
	right->Add(m_permissions, 0, wxEXPAND | wxRIGHT | wxBOTTOM, 8);
	m_approve = new wxCheckBox(this, wxID_ANY,
							   _("Mark this exact package as approved"));
	right->Add(m_approve, 0, wxEXPAND | wxRIGHT | wxBOTTOM, 8);
	m_trustUpdates = new wxCheckBox(this, wxID_ANY,
									_("Trust future updates to this Mod (keep this approval when the .cemod is rebuilt, unless it asks for more)"));
	right->Add(m_trustUpdates, 0, wxEXPAND | wxRIGHT | wxBOTTOM, 8);
	auto* actions = new wxBoxSizer(wxHORIZONTAL);
	auto* grantAll = new wxButton(this, wxID_ANY, _("Grant all requested permissions"));
	actions->Add(grantAll, 0, wxRIGHT, 8);
	auto* save = new wxButton(this, wxID_ANY, _("Save approval"));
	actions->Add(save, 0, 0);
	right->Add(actions, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, 8);
	body->Add(right, 1, wxEXPAND | wxRIGHT | wxBOTTOM, 10);
	root->Add(body, 1, wxEXPAND);

	auto* close = new wxButton(this, wxID_CANCEL, _("Close"));
	root->Add(close, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, 10);

	m_plugins->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& event) {
		ShowPlugin(static_cast<std::size_t>(event.GetInt()));
	});
	m_plugins->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent& event) {
		TogglePlugin(static_cast<std::size_t>(event.GetInt()));
	});
	save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveApproval(); });
	grantAll->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		for (std::size_t index = 0; index < m_permissionBits.size(); ++index)
			m_permissions->Check(static_cast<unsigned int>(index), true);
	});
	m_approve->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
		SyncTrustAvailability();
		if (!m_approve->IsChecked())
			return;
		const auto missing = MissingPermissions();
		m_status->SetLabel(wxString::FromUTF8(missing.empty()
												  ? "Ready to save: every requested permission is granted."
												  : "Still denied: " + missing + "."));
	});
	m_permissions->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent&) {
		if (!m_approve->IsChecked())
			return;
		const auto missing = MissingPermissions();
		m_status->SetLabel(wxString::FromUTF8(missing.empty()
												  ? "Ready to save: every requested permission is granted."
												  : "Still denied: " + missing + "."));
	});
	SetSizer(root);
	SetMinSize(wxSize(760, 500));
	CentreOnParent();
	RefreshPlugins();
}

void CemodPluginManagerDialog::RefreshPlugins()
{
	const int previous = m_plugins->GetSelection();
	m_snapshot = m_emulationController.GetCemodManagerSnapshot(m_titleId);
	m_plugins->Clear();
	for (const auto& package : m_snapshot.packages)
		m_plugins->Append(wxString::FromUTF8(ApprovalLabel(package)));
	if (m_snapshot.packages.empty())
	{
		m_status->SetLabel(_("No packages are installed for this title."));
		m_details->Clear();
		m_permissions->Clear();
		m_permissionBits.clear();
		m_permissionNames.clear();
		m_approve->SetValue(false);
		m_approve->Enable(false);
		m_trustUpdates->SetValue(false);
		m_trustUpdates->Enable(false);
		return;
	}
	const auto selection = previous == wxNOT_FOUND
							   ? 0
							   : std::min<int>(previous, static_cast<int>(m_snapshot.packages.size()) - 1);
	m_plugins->SetSelection(selection);
	ShowPlugin(static_cast<std::size_t>(selection));
}

std::string CemodPluginManagerDialog::StatusText(const Application::CemodManagerPackage& package)
{
	if (!package.valid)
		return package.warnings.empty() ? "Rejected: the package could not be inspected."
										: "Rejected: " + package.warnings.front();
	if (!package.enabled)
		return "Disabled in the manager: it is not loaded and is not asked about at launch.";
	if (package.headless)
		return "Approved, but this package targets a headless runtime and has no interactive UI.";
	if (package.approved)
		return "Approved for this exact package digest.";
	return "Approval required: the runtime skips this package until it is approved.";
}

void CemodPluginManagerDialog::ShowPlugin(std::size_t index)
{
	if (index >= m_snapshot.packages.size())
		return;
	const auto& package = m_snapshot.packages[index];
	m_permissionBits.clear();
	m_permissionNames.clear();
	m_permissions->Clear();
	for (const auto& permission : package.permissions)
		if (permission.requested)
		{
			m_permissionBits.push_back(permission.bit);
			m_permissionNames.push_back(permission.name);
			m_permissions->Append(wxString::FromUTF8(permission.name +
													 (permission.dangerous ? " (dangerous)" : "")));
			m_permissions->Check(static_cast<unsigned int>(m_permissionBits.size()) - 1, permission.granted);
		}
	m_permissions->Enable(package.valid);
	m_approve->SetValue(package.approved);
	m_approve->Enable(package.valid);
	m_trustUpdates->SetValue(package.trustUpdates);
	SyncTrustAvailability();

	std::ostringstream details;
	const std::string name = package.pluginName.empty() ? package.modId : package.pluginName;
	details << "Package: " << (name.empty() ? package.packageKey : name) << "\n"
			<< "Mod identity: " << package.modIdentity << "\n"
			<< "Package digest: sha256:" << package.packageDigest << "\n"
			<< "Scope: " << package.scope << "\n"
			<< "Signed: " << (package.signedPackage ? "yes" : "no") << "\n"
			<< "Trusted native: " << (package.trustedNative ? "yes" : "no") << "\n"
			<< "WUPS payload: " << (package.wups ? "yes" : "no") << "\n"
			<< "Enabled: " << (package.enabled ? "yes" : "no") << "\n"
			<< "Target titles: " << TitleIdsText(package.titleIds) << "\n";
	if (!package.pluginName.empty())
		details << "Plugin: " << package.pluginName << "\n"
				<< "Author: " << package.author << "\n"
				<< "Version: " << package.version << "\n";
	if (!package.description.empty())
		details << "Description: " << package.description << "\n";
	for (const auto& warning : package.warnings)
		details << "Warning: " << warning << "\n";
	details << "Status: " << StatusText(package) << "\n"
			<< "Approval: " << package.approvalReason;
	m_details->SetValue(wxString::FromUTF8(details.str()));
	m_status->SetLabel(wxString::FromUTF8(StatusText(package)));
}

std::uint64_t CemodPluginManagerDialog::CheckedPermissionMask() const
{
	std::uint64_t granted{};
	for (std::size_t index = 0; index < m_permissionBits.size(); ++index)
		if (m_permissions->IsChecked(index))
			granted |= m_permissionBits[index];
	return granted;
}

void CemodPluginManagerDialog::RefreshLabels()
{
	for (std::size_t index = 0; index < m_snapshot.packages.size(); ++index)
		m_plugins->SetString(index, wxString::FromUTF8(ApprovalLabel(m_snapshot.packages[index])));
}

void CemodPluginManagerDialog::SyncTrustAvailability()
{
	const bool approved = m_approve->IsChecked() && m_approve->IsEnabled();
	m_trustUpdates->Enable(approved);
	if (!approved)
		m_trustUpdates->SetValue(false);
}

std::string CemodPluginManagerDialog::MissingPermissions() const
{
	const auto selection = m_plugins->GetSelection();
	if (selection == wxNOT_FOUND || static_cast<std::size_t>(selection) >= m_snapshot.packages.size())
		return {};
	const auto& package = m_snapshot.packages[static_cast<std::size_t>(selection)];
	std::string missing;
	for (std::size_t index = 0; index < m_permissionBits.size(); ++index)
	{
		if ((package.requestedPermissions & m_permissionBits[index]) == 0 || m_permissions->IsChecked(index))
			continue;
		if (!missing.empty())
			missing += ", ";
		missing += index < m_permissionNames.size() ? m_permissionNames[index] : "unknown permission";
	}
	return missing;
}

void CemodPluginManagerDialog::TogglePlugin(std::size_t index)
{
	if (index >= m_snapshot.packages.size())
		return;
	// Keep permission ticks the user has not saved yet: enabling a package is an
	// independent decision and must not discard edits to this form.
	const auto grantedBefore = CheckedPermissionMask();
	Application::CemodEnableUpdate update{};
	update.packageKey = m_snapshot.packages[index].packageKey;
	update.enabled = !m_snapshot.packages[index].enabled;
	auto result = m_emulationController.SetCemodEnabled(update);
	if (!result)
	{
		m_status->SetLabel(wxString::FromUTF8(result.diagnostic.empty() ? "The package state could not be saved."
																	   : result.diagnostic));
		return;
	}
	m_snapshot = std::move(result.snapshot);
	if (index >= m_snapshot.packages.size())
		return;
	RefreshLabels();
	ShowPlugin(index);
	for (std::size_t permission = 0; permission < m_permissionBits.size(); ++permission)
		m_permissions->Check(static_cast<unsigned int>(permission), (grantedBefore & m_permissionBits[permission]) != 0);
	m_status->SetLabel(wxString::FromUTF8(m_snapshot.packages[index].enabled
											  ? "Package enabled."
											  : "Package disabled: it is not loaded and is not asked about at launch."));
}

void CemodPluginManagerDialog::SaveApproval()
{
	const auto selection = m_plugins->GetSelection();
	if (selection == wxNOT_FOUND || static_cast<std::size_t>(selection) >= m_snapshot.packages.size())
		return;
	const auto& package = m_snapshot.packages[static_cast<std::size_t>(selection)];
	if (!package.valid)
	{
		wxMessageBox(_("The package failed inspection; its approval cannot be saved."),
					 _("CemuExtend"), wxOK | wxICON_ERROR, this);
		return;
	}
	const auto granted = CheckedPermissionMask();
	const bool approved = m_approve->IsChecked();
	if (approved)
	{
		const auto missing = MissingPermissions();
		if (!missing.empty())
		{
			wxMessageBox(wxString::FromUTF8("Native and WUPS packages require every requested permission. Still denied: " +
											missing + "."),
						 _("CemuExtend"), wxOK | wxICON_ERROR, this);
			return;
		}
	}
	Application::CemodApprovalUpdate update{};
	update.generation = m_snapshot.generation;
	update.titleId = m_titleId;
	update.packageKey = package.packageKey;
	update.grantedPermissions = granted;
	update.approved = approved;
	update.trustUpdates = m_trustUpdates->IsChecked();
	auto result = m_emulationController.SaveCemodApproval(update);
	if (!result)
	{
		m_status->SetLabel(wxString::FromUTF8(result.diagnostic.empty() ? "The approval could not be saved."
																	   : result.diagnostic));
		if (result.snapshot.generation != 0)
		{
			m_snapshot = std::move(result.snapshot);
			RefreshLabels();
			if (static_cast<std::size_t>(selection) < m_snapshot.packages.size())
				ShowPlugin(static_cast<std::size_t>(selection));
		}
		return;
	}
	m_snapshot = std::move(result.snapshot);
	RefreshLabels();
	if (static_cast<std::size_t>(selection) < m_snapshot.packages.size())
		ShowPlugin(static_cast<std::size_t>(selection));
	m_status->SetLabel(wxString::FromUTF8(approved ? "Approval saved for this exact package digest."
												   : "The package is now marked as not approved."));
}
