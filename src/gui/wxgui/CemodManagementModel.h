#pragma once

#include "config/CemuConfig.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

enum class CemodGuiExecutionMode : std::uint8_t
{
	Isolated,
	TrustedNative,
};

// GUI-owned copy of the catalog data. Runtime discovery types must not leak
// into this header because the wxGUI model is also compiled without Cemu's PCH.
struct CemodGuiPackageInfo
{
	std::filesystem::path path;
	std::string modId;
	std::string principal;
	std::uint32_t requestedPermissions{};
	CemodGuiExecutionMode executionMode{CemodGuiExecutionMode::Isolated};
	bool signedPackage{};
	std::vector<std::uint64_t> titleIds;
	std::string error;
};

enum class CemodGuiPermission : std::uint8_t
{
	NativeMemory,
	FunctionPatching,
	PhysicalAddressPatching,
	FilesystemRead,
	FilesystemWrite,
	Network,
	MappedMemory,
	Notifications,
	ContentRedirection,
	Modules,
	PluginManagement,
};

struct CemodGuiPermissionItem
{
	CemodGuiPermission permission{};
	std::string label;
	std::uint64_t bit{};
	bool requested{};
	bool granted{};
	bool dangerous{};
	bool manifestMismatch{};
};

enum class CemodGuiRuntimeAvailability : std::uint8_t
{
	Available,
	UnavailableRequiresRuntimeIntegration,
};

enum class CemodGuiLoadStatus : std::uint8_t
{
	Ready,
	Disabled,
	Unavailable,
	Rejected,
};

struct CemodGuiConfigChoice
{
	std::vector<std::string> values;
};

struct CemodGuiButtonComboValue
{
	std::uint32_t controllerMask{};
	std::uint32_t holdMilliseconds{};
};

using CemodGuiConfigValue = std::variant<bool, std::int64_t, std::string,
	CemodGuiConfigChoice, CemodGuiButtonComboValue>;

struct CemodGuiConfigItem
{
	std::string id;
	std::string label;
	CemodGuiConfigValue value{false};
	std::optional<std::int64_t> minimum;
	std::optional<std::int64_t> maximum;
	std::vector<std::string> options;
	bool available{};
};

struct CemodGuiConfigCategory
{
	std::string id;
	std::string label;
	std::vector<CemodGuiConfigItem> items;
	bool available{};
};

enum class CemodGuiNotificationSeverity : std::uint8_t
{
	Info,
	Warning,
	Error,
};

struct CemodGuiNotification
{
	std::string owner;
	std::string title;
	std::string message;
	CemodGuiNotificationSeverity severity{CemodGuiNotificationSeverity::Info};
	std::uint32_t durationMilliseconds{};
	bool keepUntilShown{};
};

// Runtime-facing contract for a future WUPS/WUMS adapter. wxGUI only consumes
// owned value objects and never calls guest code through this interface.
class ICemodRuntimeGuiAdapter
{
public:
	virtual ~ICemodRuntimeGuiAdapter() = default;
	[[nodiscard]] virtual CemodGuiRuntimeAvailability Availability() const = 0;
	[[nodiscard]] virtual std::vector<CemodGuiConfigCategory> ConfigCategories(
		std::string_view pluginIdentity) const = 0;
	virtual bool SetConfigValue(std::string_view pluginIdentity, std::string_view itemId,
		const CemodGuiConfigValue& value, std::string& error) = 0;
	[[nodiscard]] virtual std::vector<CemodGuiNotification> Notifications(
		std::string_view pluginIdentity) const = 0;
};

enum class CemodGuiApprovalResult : std::uint8_t
{
	Approved,
	NeedsReapproval,
	DeniedByDefault,
	DeniedHeadlessRequiresExplicitApproval,
};

struct CemodGuiApprovalState
{
	CemodGuiApprovalResult result{CemodGuiApprovalResult::DeniedByDefault};
	std::uint64_t requested{};
	std::uint64_t granted{};
	std::string reason;
};

struct CemodPluginView
{
	std::filesystem::path path;
	std::string modId;
	std::string principal;
	std::string modIdentity;
	std::string packageDigest;
	std::string payloadFormat;
	std::string scope;
	std::string pluginName;
	std::string author;
	std::string pluginVersion;
	std::string license;
	std::string description;
	std::string wupsAbiVersion;
	std::string buildTimestamp;
	std::string storageId;
	std::vector<std::string> requiredModules;
	std::vector<std::string> processTargets;
	std::vector<std::string> compatibilityWarnings;
	std::vector<std::string> permissionMismatches;
	std::vector<CemodGuiPermissionItem> permissions;
	std::vector<CemodGuiConfigCategory> configCategories;
	std::vector<CemodGuiNotification> notifications;
	std::string lastError;
	std::string abiWarning;
	std::string statusText;
	CemodGuiRuntimeAvailability runtimeAvailability{CemodGuiRuntimeAvailability::Available};
	CemodGuiLoadStatus loadStatus{CemodGuiLoadStatus::Rejected};
	CemodGuiApprovalState approval;
	bool isWups{};
	bool signedPackage{};
	bool enabled{};
	bool restartRequired{};
	bool reloadRequired{};
	bool usesTls{};
	bool usesFixedAddressPatches{};
};

class CemodGuiAdapter final
{
public:
	[[nodiscard]] static std::string MakeApprovalKey(std::string_view modIdentity,
		std::string_view packageDigest);
	[[nodiscard]] static std::string CalculatePackageDigest(const std::filesystem::path& path,
		std::string& error);
	[[nodiscard]] static std::uint64_t PermissionBit(CemodGuiPermission permission);
	[[nodiscard]] static std::string PermissionName(CemodGuiPermission permission);
	[[nodiscard]] static std::uint64_t DefaultGrantedPermissions(std::uint64_t requested);
	[[nodiscard]] static CemodGuiApprovalState EvaluateApproval(std::uint64_t requested,
		const std::optional<CemuExtendPermissionApproval>& approval, bool headless);
	[[nodiscard]] static CemodPluginView InspectPlugin(const CemodGuiPackageInfo& info,
		std::uint64_t titleId, const std::optional<CemuExtendPermissionApproval>& approval,
		bool headless = false);

	// These methods intentionally do not manufacture runtime state. A later runtime adapter
	// can replace the unavailable model without changing the wxGUI data contract.
	[[nodiscard]] static std::vector<CemodGuiConfigCategory> UnavailableConfig();
	[[nodiscard]] static std::vector<CemodGuiNotification> UnavailableNotifications();
};
