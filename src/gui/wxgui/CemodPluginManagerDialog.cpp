#include "CemodPluginManagerDialog.h"

#include "Cafe/OS/libs/cemuextend/cemuextend.h"
#include "config/CemuConfig.h"

#include <wx/button.h>
#include <wx/checklst.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <sstream>

namespace
{
	std::string Join(const std::vector<std::string>& values, std::string_view separator = ", ")
	{
		std::ostringstream result;
		for (std::size_t index = 0; index < values.size(); ++index)
		{
			if (index != 0) result << separator;
			result << values[index];
		}
		return result.str();
	}

	std::string ApprovalLabel(const CemodPluginView& view)
	{
		std::string label = view.pluginName.empty() ? view.modId : view.pluginName;
		if (label.empty()) label = view.path.filename().string();
		label += " — ";
		label += view.enabled ? "enabled" : "disabled";
		return label;
	}

	CemodGuiPackageInfo ToGuiPackageInfo(const CemodPackageInfo& info)
	{
		return {info.path, info.modId, info.principal, info.requestedPermissions,
			info.executionMode == CemodExecutionMode::TrustedNative ?
				CemodGuiExecutionMode::TrustedNative : CemodGuiExecutionMode::Isolated,
			info.signedPackage, info.titleIds, info.error};
	}
}

CemodPluginManagerDialog::CemodPluginManagerDialog(wxWindow* parent, std::uint64_t titleId)
	: wxDialog(parent, wxID_ANY, _("Aroma / WUPS package manager"), wxDefaultPosition,
		wxSize(980, 680), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), m_titleId(titleId)
{
	auto* root = new wxBoxSizer(wxVERTICAL);
	root->Add(new wxStaticText(this, wxID_ANY,
		_("WUPS is a payload format, not an execution sandbox. Runtime-backed operations are shown as unavailable until the runtime adapter is connected.")),
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
	right->Add(new wxStaticText(this, wxID_ANY, _("Requested permissions (dangerous permissions start denied):")),
		0, wxEXPAND | wxRIGHT | wxBOTTOM, 6);
	m_permissions = new wxCheckListBox(this, wxID_ANY);
	right->Add(m_permissions, 0, wxEXPAND | wxRIGHT | wxBOTTOM, 8);
	auto* save = new wxButton(this, wxID_ANY, _("Save approval"));
	right->Add(save, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, 8);
	body->Add(right, 1, wxEXPAND | wxRIGHT | wxBOTTOM, 10);
	root->Add(body, 1, wxEXPAND);

	auto* close = new wxButton(this, wxID_CANCEL, _("Close"));
	root->Add(close, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, 10);

	m_plugins->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& event) {
		ShowPlugin(static_cast<std::size_t>(event.GetInt()));
	});
	m_plugins->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent& event) {
		TogglePlugin(static_cast<std::size_t>(event.GetInt()),
			m_views[static_cast<std::size_t>(event.GetInt())].enabled);
	});
	save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveApproval(); });
	SetSizer(root);
	SetMinSize(wxSize(760, 500));
	CentreOnParent();
	RefreshPlugins();
}

std::optional<CemuExtendPermissionApproval> CemodPluginManagerDialog::LoadApproval(
	const CemodGuiPackageInfo& info) const
{
	std::string digestError;
	const auto digest = CemodGuiAdapter::CalculatePackageDigest(info.path, digestError);
	if (digest.empty()) return std::nullopt;
	const auto identity = info.modId.empty() ? info.principal : info.modId;
	return GetConfig().GetCemuExtendPermissionApproval(m_titleId,
		CemodGuiAdapter::MakeApprovalKey(identity, digest));
}

void CemodPluginManagerDialog::RefreshPlugins()
{
	m_infos.clear();
	for (const auto& info : cemuextend_hle::DiscoverCemods(m_titleId))
		m_infos.push_back(ToGuiPackageInfo(info));
	m_views.clear();
	m_plugins->Clear();
	for (const auto& info : m_infos)
	{
		const auto view = CemodGuiAdapter::InspectPlugin(info, m_titleId, LoadApproval(info));
		m_views.push_back(view);
		m_plugins->Append(wxString::FromUTF8(ApprovalLabel(view)));
	}
	if (m_views.empty())
	{
		m_status->SetLabel(_("No packages are installed for this title."));
		m_details->Clear();
		m_permissions->Clear();
		return;
	}
	m_plugins->SetSelection(0);
	ShowPlugin(0);
}

void CemodPluginManagerDialog::ShowPlugin(std::size_t index)
{
	if (index >= m_views.size()) return;
	const auto& view = m_views[index];
	m_permissionBits.clear();
	m_permissions->Clear();
	for (const auto& permission : view.permissions)
		if (permission.requested)
		{
			m_permissionBits.push_back(permission.bit);
			m_permissions->Append(wxString::FromUTF8(permission.label +
				(permission.dangerous ? " (dangerous)" : "")));
			m_permissions->Check(m_permissionBits.size() - 1, permission.granted);
		}

	std::ostringstream details;
	details << "Package: " << view.path.filename().string() << "\n"
		<< "Payload: " << view.payloadFormat << "\n"
		<< "Mod identity: " << view.modIdentity << "\n"
		<< "Package digest: sha256:" << view.packageDigest << "\n"
		<< "Scope: " << view.scope << "\n"
		<< "Signed: " << (view.signedPackage ? "yes" : "no") << "\n";
	if (view.isWups)
	{
		details << "Plugin: " << view.pluginName << "\n"
			<< "Author: " << view.author << "\n"
			<< "Version: " << view.pluginVersion << "\n"
			<< "WUPS ABI: " << view.wupsAbiVersion << "\n"
			<< "License: " << view.license << "\n"
			<< "Build timestamp: " << view.buildTimestamp << "\n"
			<< "Storage ID: " << view.storageId << "\n"
			<< "Required modules: " << Join(view.requiredModules) << "\n"
			<< "Process targets: " << Join(view.processTargets) << "\n"
			<< "TLS: " << (view.usesTls ? "yes" : "no") << "\n"
			<< "Fixed-address patches: " << (view.usesFixedAddressPatches ? "yes" : "no") << "\n"
			<< "Config: Unavailable — Requires runtime integration\n"
			<< "Notifications: Unavailable — Requires runtime integration\n"
			<< "Restart/reload: " << (view.restartRequired ? "required" : "not required") << "/"
			<< (view.reloadRequired ? "required" : "not required") << "\n";
	}
	if (!view.compatibilityWarnings.empty()) details << "Compatibility warnings: " << Join(view.compatibilityWarnings) << "\n";
	if (!view.permissionMismatches.empty()) details << "Permission mismatch: " << Join(view.permissionMismatches) << "\n";
	if (!view.abiWarning.empty()) details << "ABI warning: " << view.abiWarning << "\n";
	if (!view.lastError.empty()) details << "Last error: " << view.lastError << "\n";
	details << "Status: " << view.statusText << "\n"
		<< "Approval: " << view.approval.reason;
	m_details->SetValue(wxString::FromUTF8(details.str()));
	m_status->SetLabel(wxString::FromUTF8(view.statusText));
}

void CemodPluginManagerDialog::TogglePlugin(std::size_t index, bool enabled)
{
	if (index >= m_views.size()) return;
	// Double-click is only a convenience for changing the check state in the
	// list. It never claims that the runtime loaded or unloaded a plugin.
	m_views[index].enabled = !enabled;
	m_plugins->SetString(index, wxString::FromUTF8(ApprovalLabel(m_views[index])));
	m_plugins->SetSelection(static_cast<int>(index));
	ShowPlugin(index);
}

void CemodPluginManagerDialog::SaveApproval()
{
	const auto selection = m_plugins->GetSelection();
	if (selection == wxNOT_FOUND || static_cast<std::size_t>(selection) >= m_views.size()) return;
	auto& view = m_views[static_cast<std::size_t>(selection)];
	if (view.packageDigest.empty() || view.modIdentity.empty())
	{
		wxMessageBox(_("The package digest or mod identity is unavailable; approval was not saved."),
			_("CemuExtend"), wxOK | wxICON_ERROR, this);
		return;
	}
	std::uint64_t granted{};
	for (std::size_t index = 0; index < m_permissionBits.size(); ++index)
		if (m_permissions->IsChecked(index)) granted |= m_permissionBits[index];
	const auto requested = view.approval.requested;
	const auto key = CemodGuiAdapter::MakeApprovalKey(view.modIdentity, view.packageDigest);
	GetConfig().SetCemuExtendPermissionApproval(m_titleId, key,
		{view.packageDigest, view.modIdentity, requested, granted, view.enabled, false});
	GetConfigHandle().Save();
	view.approval = CemodGuiAdapter::EvaluateApproval(requested,
		GetConfig().GetCemuExtendPermissionApproval(m_titleId, key), false);
	view.enabled = view.approval.result == CemodGuiApprovalResult::Approved;
	view.loadStatus = view.runtimeAvailability == CemodGuiRuntimeAvailability::UnavailableRequiresRuntimeIntegration ?
		CemodGuiLoadStatus::Unavailable :
		(view.enabled ? CemodGuiLoadStatus::Ready : CemodGuiLoadStatus::Disabled);
	m_plugins->SetString(selection, wxString::FromUTF8(ApprovalLabel(view)));
	ShowPlugin(static_cast<std::size_t>(selection));
	m_status->SetLabel(_("Approval saved. Runtime load/reload remains unavailable until runtime integration is connected."));
}
