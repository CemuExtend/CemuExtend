#pragma once

#include "Cafe/HW/Espresso/CemodPackage.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class ModuleExportRegistry;
class WupsFunctionPatchManager;
class IWupsPatchPlatform;

struct WupsOwnerToken
{
	std::uint64_t owner{};
	std::uint32_t generation{};

	[[nodiscard]] friend bool operator==(const WupsOwnerToken&,
		const WupsOwnerToken&) = default;
};

class WupsGuestOwnerScope
{
public:
	explicit WupsGuestOwnerScope(WupsOwnerToken owner) : m_previous(s_current)
	{
		s_current = owner;
	}
	~WupsGuestOwnerScope() { s_current = m_previous; }
	WupsGuestOwnerScope(const WupsGuestOwnerScope&) = delete;
	WupsGuestOwnerScope& operator=(const WupsGuestOwnerScope&) = delete;

	[[nodiscard]] static std::optional<WupsOwnerToken> Current()
	{
		return s_current;
	}

private:
	std::optional<WupsOwnerToken> m_previous;
	inline static thread_local std::optional<WupsOwnerToken> s_current;
};

enum class WupsGuestAccess : std::uint8_t
{
	Read,
	Write,
	Execute,
};

enum class WupsServiceStatus : std::int32_t
{
	Success = 0,
	InvalidArgument = -1,
	NotFound = -2,
	AlreadyExists = -3,
	BufferTooSmall = -4,
	PermissionDenied = -5,
	OwnerMismatch = -6,
	StaleGeneration = -7,
	LimitExceeded = -8,
	Conflict = -9,
	CorruptData = -10,
	IoError = -11,
	Unsupported = -12,
	Busy = -13,
	InternalError = -14,
	UnsupportedVersion = -15,
};

enum class WupsStorageValueType : std::uint32_t
{
	Signed32 = 0,
	Signed64 = 1,
	Unsigned32 = 2,
	Unsigned64 = 3,
	String = 4,
	Binary = 5,
	Boolean = 6,
	Float = 7,
	Double = 8,
};

struct WupsStorageValue
{
	WupsStorageValueType type{WupsStorageValueType::Binary};
	std::vector<std::byte> bytes;
};

enum class WupsConfigItemKind : std::uint8_t
{
	Boolean,
	IntegerRange,
	MultipleValues,
	ButtonCombo,
	Custom,
};

struct WupsConfigChoice
{
	std::int32_t value{};
	std::string label;
};

struct WupsConfigCallbacks
{
	std::uint32_t context{};
	std::uint32_t valueDisplay{};
	std::uint32_t selectedValueDisplay{};
	std::uint32_t selected{};
	std::uint32_t restoreDefault{};
	std::uint32_t movementAllowed{};
	std::uint32_t close{};
	std::uint32_t input{};
	std::uint32_t inputEx{};
	std::uint32_t destroy{};
};

struct WupsConfigItemModel
{
	std::uint32_t handle{};
	std::string displayName;
	WupsConfigItemKind kind{WupsConfigItemKind::Custom};
	std::string currentValue;
	std::int32_t minimum{};
	std::int32_t maximum{};
	std::vector<WupsConfigChoice> choices;
	WupsConfigCallbacks callbacks;
};

struct WupsConfigCategoryModel
{
	std::uint32_t handle{};
	std::string name;
	std::vector<WupsConfigCategoryModel> categories;
	std::vector<WupsConfigItemModel> items;
};

struct WupsConfigModel
{
	std::string pluginName;
	std::string pluginVersion;
	std::string abiVersion;
	std::string compatibilityWarning;
	bool menuOpen{};
	std::optional<WupsConfigCategoryModel> root;
};

enum class WupsButtonComboType : std::uint32_t
{
	Invalid = 0,
	Hold = 1,
	HoldObserver = 2,
	PressDown = 3,
	PressDownObserver = 4,
	PressRelease = 5,
	PressReleaseObserver = 6,
};

enum class WupsButtonComboStatus : std::uint32_t
{
	Invalid = 0,
	Valid = 1,
	Conflict = 2,
};

struct WupsButtonComboDefinition
{
	std::string label;
	std::uint32_t controllerMask{};
	std::uint32_t buttons{};
	WupsButtonComboType type{WupsButtonComboType::PressDown};
	std::uint32_t holdDurationMilliseconds{};
	std::uint32_t callback{};
	std::uint32_t context{};
};

struct WupsButtonSample
{
	std::uint32_t controllerMask{};
	std::uint32_t pressed{};
	std::uint32_t released{};
	std::uint32_t held{};
	std::chrono::milliseconds heldFor{};
};

enum class WupsMappedMemoryPurpose : std::uint8_t
{
	Cpu,
	Gx2,
};

struct WupsMappedMemoryInfo
{
	std::uint32_t address{};
	std::uint32_t physicalAddress{};
	std::uint32_t size{};
	std::uint32_t alignment{};
	bool writable{};
	WupsMappedMemoryPurpose purpose{WupsMappedMemoryPurpose::Cpu};
};

enum class WupsNotificationSeverity : std::uint8_t
{
	Info,
	Warning,
	Error,
};

struct WupsNotificationModel
{
	std::uint32_t handle{};
	std::string text;
	WupsNotificationSeverity severity{WupsNotificationSeverity::Info};
	std::chrono::milliseconds duration{};
	bool dynamic{};
	bool keepUntilShown{};
	bool finished{};
	std::uint32_t callback{};
	std::uint32_t callbackContext{};
};

enum class WupsLogLevel : std::uint8_t
{
	Error,
	Warning,
	Info,
	Verbose,
};

struct WupsContentRedirectRule
{
	std::uint32_t handle{};
	std::string virtualPath;
	std::filesystem::path sourcePath;
	std::int32_t priority{};
	bool writable{};
	bool active{true};
};

using WupsHostExportHandler = std::function<std::int32_t(
	std::span<const std::uint32_t> arguments, std::string& error)>;
using WupsGuestInvoker = std::function<bool(std::uint32_t target,
	std::span<const std::uint32_t> arguments, std::uint32_t& result,
	std::string& error)>;

class IWupsPlatform
{
public:
	virtual ~IWupsPlatform() = default;

	[[nodiscard]] virtual bool ValidateGuestRange(std::uint32_t address,
		std::uint32_t size, WupsGuestAccess access) const = 0;
	[[nodiscard]] virtual bool ValidateGuestRangeForOwner(WupsOwnerToken,
		std::uint32_t address, std::uint32_t size,
		WupsGuestAccess access) const
	{
		return ValidateGuestRange(address, size, access);
	}
	[[nodiscard]] virtual bool ReadGuest(std::uint32_t address,
		std::span<std::byte> output) const = 0;
	[[nodiscard]] virtual bool WriteGuest(std::uint32_t address,
		std::span<const std::byte> input) = 0;
	[[nodiscard]] virtual std::optional<std::uint32_t> AllocateGuestData(
		WupsOwnerToken owner, std::uint32_t size, std::uint32_t alignment,
		std::string& error) = 0;
	virtual void FreeGuestData(WupsOwnerToken owner, std::uint32_t address) = 0;

	[[nodiscard]] virtual std::optional<std::uint32_t> RegisterFunction(
		WupsOwnerToken owner, std::string_view moduleName,
		std::string_view symbolName, WupsHostExportHandler handler,
		std::string& error) = 0;
	virtual void ReleaseOwnerExports(WupsOwnerToken owner) = 0;

	[[nodiscard]] virtual std::uint64_t CurrentGuestThreadId() const = 0;
	[[nodiscard]] virtual bool QueueCpuTask(WupsOwnerToken owner,
		std::function<void()> task, std::string& error) = 0;
	// Must synchronously destroy every queued (not currently executing) task
	// owned by this token before returning. Tasks already executing may finish.
	virtual void CancelCpuTasks(WupsOwnerToken owner) = 0;
	// A platform which cannot map host-owned memory into Cafe's effective and
	// physical address spaces must say so. Import resolution then fails instead
	// of publishing a callable API which can only return null.
	[[nodiscard]] virtual bool SupportsMappedMemory() const { return true; }
	// False unless arbitrary guest heap pointers can be attributed to the
	// currently executing owner generation. Heap-taking ABIs are withheld when
	// this provenance is unavailable.
	[[nodiscard]] virtual bool SupportsOwnerScopedHeapPointers() const
	{
		return true;
	}

	// A platform-owned heap that WUPS plugin libc allocations are redirected
	// into (see the "coreinit"."MEMAllocFromDefaultHeap"(Ex)/
	// "MEMFreeToDefaultHeap" data-import handling in WupsServices.cpp), kept
	// structurally separate from the game's own default heap. False unless the
	// platform actually has one; the redirection is then withheld and those
	// data imports fail to resolve rather than silently falling back to the
	// game's heap.
	[[nodiscard]] virtual bool SupportsPluginHeap() const { return false; }
	// Allocates `size` bytes from the platform's plugin heap on behalf of
	// `owner`. `alignment` mirrors MEMAllocFromExpHeapEx's own signed
	// alignment ABI (negative requests a tail allocation). Returns 0 on any
	// failure (no plugin heap, not yet initialised, OOM, bad arguments).
	[[nodiscard]] virtual std::uint32_t AllocatePluginHeapMemory(
		WupsOwnerToken owner, std::uint32_t size, std::int32_t alignment)
	{
		(void)owner;
		(void)size;
		(void)alignment;
		return 0;
	}
	// Frees `address` on behalf of `owner`. Must be a no-op (aside from
	// logging) for any address the plugin heap did not itself hand out.
	virtual void FreePluginHeapMemory(WupsOwnerToken owner,
		std::uint32_t address)
	{
		(void)owner;
		(void)address;
	}

	[[nodiscard]] virtual std::optional<WupsMappedMemoryInfo> AllocateMappedMemory(
		WupsOwnerToken owner, std::uint32_t size, std::uint32_t alignment,
		bool writable, WupsMappedMemoryPurpose purpose, std::string& error) = 0;
	[[nodiscard]] virtual bool FreeMappedMemory(WupsOwnerToken owner,
		const WupsMappedMemoryInfo& allocation, std::string& error) = 0;

	virtual void ShowNotification(WupsOwnerToken owner,
		const WupsNotificationModel& notification) = 0;
	virtual void Log(WupsOwnerToken owner, WupsLogLevel level,
		std::string_view moduleName, std::string_view source,
		std::string_view message) = 0;
};

class IWupsFunctionPatcherFacade
{
public:
	virtual ~IWupsFunctionPatcherFacade() = default;

	[[nodiscard]] virtual std::uint32_t ApiVersion() const = 0;
	[[nodiscard]] virtual WupsServiceStatus AddPatch(WupsOwnerToken owner,
		std::uint32_t descriptorAddress, bool allowPhysicalAddress,
		std::uint32_t& handle,
		bool& applied, std::string& error) = 0;
	[[nodiscard]] virtual WupsServiceStatus RemovePatch(WupsOwnerToken owner,
		std::uint32_t handle, std::string& error) = 0;
	[[nodiscard]] virtual WupsServiceStatus IsPatchApplied(WupsOwnerToken owner,
		std::uint32_t handle, bool& applied, std::string& error) const = 0;
	virtual void ReleaseOwner(WupsOwnerToken owner) = 0;
};

struct AromaRuntimeOptions
{
	std::filesystem::path storageRoot;
	std::vector<std::filesystem::path> contentRoots;
	std::shared_ptr<IWupsPlatform> platform;
	std::shared_ptr<IWupsFunctionPatcherFacade> functionPatcher;
	std::shared_ptr<ModuleExportRegistry> exportRegistry;
	std::shared_ptr<WupsFunctionPatchManager> patchManager;
	std::shared_ptr<IWupsPatchPlatform> patchPlatform;
	std::size_t maximumStorageBytes{1024U * 1024U};
	std::size_t maximumStorageItems{1024};
	std::size_t maximumMappedBytes{64U * 1024U * 1024U};
	std::size_t maximumNotifications{64};
};

struct WupsOwnerResourceCounts
{
	std::size_t storageItems{};
	std::size_t configCategories{};
	std::size_t configItems{};
	std::size_t buttonCombos{};
	std::size_t reentContexts{};
	std::size_t mappedAllocations{};
	std::size_t notifications{};
	std::size_t contentRedirects{};
	std::size_t pendingCallbacks{};
};

// The Cemu guest-memory adapter is supplied by the production composition
// root. Keeping the service layer dependent only on this interface makes the
// Task 3 patcher/platform implementation link-safe in either build order.
[[nodiscard]] std::shared_ptr<IWupsFunctionPatcherFacade>
CreateUnsupportedFunctionPatcherFacade();

// Production Cemu adapters. The platform publishes direct owner-scoped HLE
// thunks and never adds guest-selected names to Cemu's global osLib registry.
[[nodiscard]] std::shared_ptr<IWupsPlatform> CreateCemuWupsPlatform();
[[nodiscard]] std::shared_ptr<IWupsFunctionPatcherFacade>
CreateWupsFunctionPatcherFacade(std::shared_ptr<IWupsPlatform> platform,
	std::shared_ptr<WupsFunctionPatchManager> manager);
