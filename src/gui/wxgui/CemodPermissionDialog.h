#pragma once

#include "application/EmulationController.h"
#include "CemodManagementModel.h"

#include <wx/dialog.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class wxCheckBox;

struct CemodPermissionSelection
{
	std::string principal;
	std::uint32_t requestedPermissions{};
	std::uint32_t grantedPermissions{};
	std::string approvalKey;
	std::string packageDigest;
	std::string modIdentity;
	std::uint64_t requestedNativePermissions{};
	std::uint64_t grantedNativePermissions{};
	bool explicitHeadlessDenial{};
};

struct CemodPermissionDialogEntry
{
	std::string modId;
	std::string principal;
	std::string approvalKey;
	std::string packageDigest;
	std::string modIdentity;
	CemodGuiExecutionMode executionMode{CemodGuiExecutionMode::Isolated};
	bool signedPackage{};
	bool headless{};
	std::vector<CemodGuiPermissionItem> permissions;
	std::string payloadDetails;
	std::string metadataDetails;
	std::string scopeDetails;
	std::string moduleDetails;
	std::vector<std::string> warnings;
};

class CemodPermissionDialog final : public wxDialog
{
public:
	CemodPermissionDialog(wxWindow* parent, const wxString& gameName,
		std::vector<Application::CemodPermissionRequest> requests);
	CemodPermissionDialog(wxWindow* parent, const wxString& gameName,
		std::vector<CemodPermissionDialogEntry> entries);

	[[nodiscard]] const std::vector<CemodPermissionSelection>& GetSelections() const
	{
		return m_selections;
	}

private:
	struct ModRow
	{
		CemodPermissionDialogEntry entry;
		std::vector<wxCheckBox*> permissions;
	};

	void AllowAll();
	void SaveAndClose();

	std::vector<ModRow> m_rows;
	std::vector<CemodPermissionSelection> m_selections;
};
