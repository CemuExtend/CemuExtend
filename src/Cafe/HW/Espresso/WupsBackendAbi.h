#pragma once

#include "Cafe/HW/Espresso/WupsBinary.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

enum class WupsBackendApiError : std::uint32_t
{
	None = 0,
	InvalidSize = 0xffffffffU,
	InvalidArgument = 0xfffffffeU,
	FailedAllocation = 0xfffffffdU,
	FileNotFound = 0xfffffffcU,
	InvalidHandle = 0xfffffffbU,
	ModuleNotFound = 0xfffffffaU,
	ModuleMissingExport = 0xfffffff9U,
	UnsupportedVersion = 0xfffffff8U,
	LibraryUninitialized = 0xfffffff7U,
	UnsupportedCommand = 0xfffffff6U,
};

enum class WupsBackendParseError : std::int32_t
{
	None = 0,
	Unknown = -1,
	IncompatibleVersion = -2,
};

enum class WupsBackendInputType : std::uint32_t
{
	Path = 0,
	Buffer = 1,
};

inline constexpr std::uint32_t kWupsBackendApiVersion = 3;
inline constexpr std::uint32_t kWupsBackendPluginInformationVersion = 2;
inline constexpr std::uint32_t kWupsBackendSectionInformationVersion = 1;
inline constexpr std::uint32_t kWupsBackendPluginInformationSize = 1800;
inline constexpr std::uint32_t kWupsBackendSectionInformationSize = 44;

// Wii U ABI wire layouts. All integral fields, including pointers and size_t,
// are serialized explicitly as 32-bit big-endian values.
struct GuestWupsPluginInformationV2
{
	std::array<std::byte, 4> informationVersion;
	std::array<std::byte, 256> name;
	std::array<std::byte, 256> author;
	std::array<std::byte, 256> buildTimestamp;
	std::array<std::byte, 256> description;
	std::array<std::byte, 256> license;
	std::array<std::byte, 256> version;
	std::array<std::byte, 256> storageId;
	std::array<std::byte, 4> size;
};

struct GuestWupsPluginSectionInfoV1
{
	std::array<std::byte, 4> informationVersion;
	std::array<std::byte, 32> name;
	std::array<std::byte, 4> address;
	std::array<std::byte, 4> size;
};

static_assert(sizeof(GuestWupsPluginInformationV2) ==
	kWupsBackendPluginInformationSize);
static_assert(sizeof(GuestWupsPluginSectionInfoV1) ==
	kWupsBackendSectionInformationSize);

enum class WupsBackendExportId : std::uint8_t
{
	LoadPluginAsDataByPath,
	LoadPluginAsDataByBuffer,
	LoadPluginAsData,
	LoadAndLinkByDataHandle,
	DeletePluginData,
	GetPluginMetaInformation,
	GetPluginMetaInformationByPath,
	GetPluginMetaInformationByBuffer,
	GetMetaInformation,
	GetLoadedPlugins,
	GetPluginDataForContainerHandles,
	GetApiVersion,
	GetNumberOfLoadedPlugins,
	GetSectionInformationForPlugin,
	WillReloadPluginsOnNextLaunch,
	GetSectionMemoryAddresses,
	GetPluginMetaInformationByPathEx,
	GetPluginMetaInformationByBufferEx,
};

struct WupsBackendExportDescriptor
{
	std::string_view name;
	WupsBackendExportId id;
	WupsSymbolKind kind;
};

[[nodiscard]] std::span<const WupsBackendExportDescriptor>
WupsBackendExportDescriptors();
[[nodiscard]] const WupsBackendExportDescriptor* FindWupsBackendExport(
	std::string_view name, WupsSymbolKind kind = WupsSymbolKind::Function);

