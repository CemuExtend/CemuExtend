#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Cafe/HW/Espresso/WupsBinary.h"

enum class CemodExecutionMode : std::uint8_t
{
	Isolated,
	TrustedNative,
};

enum class CemodPayloadFormat : std::uint8_t
{
	CemodElf,
	Wups,
};

enum class CemodScopeType : std::uint8_t
{
	Title,
	Process,
	AromaNative,
};

struct CemodPayloadDescriptor
{
	CemodPayloadFormat format{CemodPayloadFormat::CemodElf};
	std::string path{"mod.elf"};
};

struct CemodScope
{
	CemodScopeType type{CemodScopeType::Title};
	std::vector<std::string> targets;
};

struct CemodNativePermissions
{
	bool nativeMemory{};
	bool functionPatching{};
	bool physicalAddressPatching{};
	bool filesystemRead{};
	bool filesystemWrite{};
	bool network{};
	bool mappedMemory{};
	bool notifications{};
	bool contentRedirection{};
	bool pluginManagement{};
	std::vector<std::string> modules;
};

struct CemodManifest
{
	std::uint32_t packageVersion{};
	std::uint32_t apiVersion{};
	CemodExecutionMode executionMode{CemodExecutionMode::Isolated};
	CemodPayloadDescriptor payload;
	CemodScope scope;
	CemodNativePermissions nativePermissions;
	std::string modId;
	std::vector<std::uint64_t> titleIds;
	std::uint32_t requestedPermissions{};
	std::uint32_t codeBytes{};
	std::uint32_t privateBytes{};
	std::uint32_t stackBytes{};
	std::uint32_t instructionsPerFrame{};
	std::uint32_t timeMicrosecondsPerFrame{};
	std::string entrypoint;
};

struct CemodPackage
{
	static constexpr std::uint64_t kMaximumExpandedBytes = 64ULL * 1024ULL * 1024ULL;
	static constexpr std::uint64_t kMaximumPayloadBytes = 64ULL * 1024ULL * 1024ULL;
	static constexpr std::uint64_t kMaximumCompressionRatio = 200;

	CemodManifest manifest;
	std::vector<std::byte> payload;
	// Source-compatibility adapter for callers that construct or inspect legacy packages.
	// Loaded ELF packages mirror payload here; new code must use PayloadBytes().
	std::vector<std::byte> elf;
	std::optional<WupsInspection> wups;
	std::string principal;
	std::uint64_t targetTitleId{};
	bool signedPackage{};
	// Runtime-only grants populated after package approval.
	std::uint32_t grantedPermissions{};
	std::uint32_t serviceReadMask{};
	std::uint32_t serviceWriteMask{};
	std::uint32_t serviceInjectMask{};

	[[nodiscard]] bool IsTrustedNative() const
	{
		return manifest.executionMode == CemodExecutionMode::TrustedNative;
	}

	[[nodiscard]] std::span<const std::byte> PayloadBytes() const
	{
		return payload.empty() ? std::span<const std::byte>(elf) : std::span<const std::byte>(payload);
	}

	[[nodiscard]] std::span<std::byte> PayloadBytes()
	{
		return payload.empty() ? std::span<std::byte>(elf) : std::span<std::byte>(payload);
	}

	[[nodiscard]] static std::optional<CemodPackage> Load(const std::filesystem::path& path,
		std::uint64_t titleId, std::string& error);
	[[nodiscard]] static std::optional<CemodPackage> Inspect(const std::filesystem::path& path,
		std::string& error);
};
