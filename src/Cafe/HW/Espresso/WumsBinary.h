#pragma once

#include "Cafe/HW/Espresso/WupsBinary.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

enum class WumsHookType : std::uint32_t
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
	Init,
	ApplicationStarts,
	ApplicationEnds,
	RelocationsDone,
	ApplicationRequestsExit,
	Deinit,
	AllApplicationStartsDone,
	AllApplicationEndsDone,
	AllApplicationRequestsExitDone,
	GetCustomRplAllocator,
	ClearAllocatedRplMemory,
	InitWutThread,
	InitReentFunctions,
};

enum class WumsDependencyMatch : std::uint8_t
{
	Any,
	Exact,
	AtLeast,
};

struct WumsDependency
{
	std::string moduleName;
	bool optional{};
	WumsDependencyMatch match{WumsDependencyMatch::Any};
	std::optional<WupsVersion> version;
};

struct WumsMetadata
{
	std::string moduleName;
	WupsVersion abiVersion;
	std::string version;
	std::string author;
	std::string license;
	std::string description;
	std::string buildTimestamp;
	bool skipInitFini{};
	bool initBeforeRelocationsDone{};
	std::map<std::string, std::string> unknown;
};

struct WumsHookInspection
{
	WumsHookType type{};
	std::uint32_t target{};
};

struct WumsInspection
{
	WumsMetadata metadata;
	std::vector<WumsHookInspection> hooks;
	std::vector<WumsDependency> dependencies;
	std::vector<WupsSymbolInspection> imports;
	std::vector<WupsSymbolInspection> exports;
	std::vector<WupsRelocationInspection> relocations;
	std::vector<WupsSectionInspection> sections;
	bool usesTls{};
};

class WumsBinaryInspector
{
public:
	static constexpr std::uint64_t kMaximumExpandedBytes =
		64ULL * 1024ULL * 1024ULL;
	static constexpr std::uint32_t kMaximumSections = 512;
	static constexpr std::uint32_t kMaximumDescriptors = 4096;

	[[nodiscard]] static std::optional<WumsInspection> Inspect(
		std::span<const std::byte> image, std::string& error);
};
