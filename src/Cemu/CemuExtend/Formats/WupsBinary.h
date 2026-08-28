#pragma once

#include <cstddef>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

enum class WupsSymbolKind : std::uint8_t
{
	Function,
	Data,
};

enum class WupsHookType : std::uint32_t
{
	InitWutMalloc,
	FiniWutMalloc,
	InitWutNewlib,
	FiniWutNewlib,
	InitWutStdcpp,
	FiniWutStdcpp,
	InitWutDevoptab,
	FiniWutDevoptab,
	InitWutSockets,
	FiniWutSockets,
	InitWrapper,
	FiniWrapper,
	GetConfigDeprecated,
	ConfigClosedDeprecated,
	InitStorageDeprecated,
	InitPlugin,
	DeinitPlugin,
	ApplicationStarts,
	ReleaseForeground,
	AcquiredForeground,
	ApplicationRequestsExit,
	ApplicationEnds,
	InitStorage,
	InitConfig,
	InitButtonCombo,
	InitWutThread,
	InitReentFunctions,
};

enum class WupsLoadEntryType : std::uint8_t
{
	OptionalFunction,
	MandatoryFunction,
	LegacyExport,
};

struct WupsVersion
{
	std::uint16_t major{};
	std::uint16_t minor{};
	std::uint16_t patch{};

	[[nodiscard]] std::string ToString() const;
	[[nodiscard]] auto operator<=>(const WupsVersion&) const = default;
};

struct WupsMetadata
{
	std::string name;
	std::string author;
	std::string version;
	std::string license;
	std::string description;
	WupsVersion abiVersion;
	std::string buildTimestamp;
	std::string storageId;
	bool trackHeap{};
	bool collectHeapStackTraces{};
	std::map<std::string, std::string> unknown;
};

struct WupsSectionInspection
{
	std::string name;
	std::uint32_t type{};
	std::uint32_t flags{};
	std::uint32_t address{};
	std::uint32_t storedSize{};
	std::uint32_t expandedSize{};
	std::uint32_t alignment{};
	bool compressed{};
	bool tls{};
};

struct WupsHookInspection
{
	WupsHookType type{};
	std::uint32_t target{};
};

struct WupsReplacementInspection
{
	WupsLoadEntryType entryType{WupsLoadEntryType::OptionalFunction};
	bool mandatory{};
	std::uint32_t physicalAddress{};
	std::uint32_t virtualAddress{};
	std::string name;
	std::uint32_t library{};
	std::string replacementName;
	std::uint32_t target{};
	std::uint32_t callThroughStorage{};
	std::uint32_t processTarget{};
};

struct WupsSymbolInspection
{
	std::string module;
	std::string name;
	WupsSymbolKind kind{WupsSymbolKind::Function};
	std::uint32_t address{};
	bool mandatory{true};
};

struct WupsRelocationInspection
{
	std::string section;
	std::uint32_t offset{};
	std::uint32_t type{};
	std::string symbol;
	std::optional<std::string> importModule;
	WupsSymbolKind symbolKind{WupsSymbolKind::Function};
};

struct WupsInspection
{
	WupsMetadata metadata;
	std::vector<WupsSectionInspection> sections;
	std::vector<WupsHookInspection> hooks;
	std::vector<WupsReplacementInspection> replacements;
	std::vector<WupsSymbolInspection> imports;
	std::vector<WupsSymbolInspection> exports;
	std::vector<WupsRelocationInspection> relocations;
	std::vector<std::string> requiredModules;
	std::vector<std::uint32_t> processTargets;
	std::vector<std::uint32_t> relocationTypes;
	std::vector<std::string> compatibilityWarnings;
	bool usesTls{};
	bool usesFixedAddressPatches{};
};

class WupsBinaryInspector
{
  public:
	static constexpr std::uint64_t kMaximumExpandedBytes = 64ULL * 1024ULL * 1024ULL;
	static constexpr std::uint32_t kMaximumSections = 512;

	[[nodiscard]] static std::optional<WupsInspection> Inspect(
		std::span<const std::byte> image, std::string& error);
};
