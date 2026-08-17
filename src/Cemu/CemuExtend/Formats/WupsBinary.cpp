#include "Common/precompiled.h"

#include "Cemu/CemuExtend/Formats/WupsBinary.h"
#include "Cemu/CemuExtend/Formats/ExternalModuleMappingPolicy.h"

#include <zlib.h>

#include <array>
#include <charconv>
#include <limits>
#include <set>

namespace
{
	constexpr std::uint32_t kShtNull = 0;
	constexpr std::uint32_t kShtProgbits = 1;
	constexpr std::uint32_t kShtSymtab = 2;
	constexpr std::uint32_t kShtStrtab = 3;
	constexpr std::uint32_t kShtRela = 4;
	constexpr std::uint32_t kShtNobits = 8;
	constexpr std::uint32_t kShtRel = 9;
	constexpr std::uint32_t kShtDynsym = 11;
	constexpr std::uint32_t kShtRplExports = 0x80000001U;
	constexpr std::uint32_t kShtRplImports = 0x80000002U;
	constexpr std::uint32_t kShtRplCrcs = 0x80000003U;
	constexpr std::uint32_t kShtRplFileInfo = 0x80000004U;
	constexpr std::uint32_t kShfWrite = 1;
	constexpr std::uint32_t kShfAlloc = 2;
	constexpr std::uint32_t kShfExecute = 4;
	constexpr std::uint32_t kShfTls = 0x400;
	constexpr std::uint32_t kShfRplCompressed = 0x08000000U;
	constexpr std::uint32_t kWupsLibraryOther = 66;
	constexpr std::uint32_t kMaximumDescriptors = 4096;

	struct Section
	{
		std::uint32_t nameOffset{};
		std::uint32_t type{};
		std::uint32_t flags{};
		std::uint32_t address{};
		std::uint32_t fileOffset{};
		std::uint32_t storedSize{};
		std::uint32_t link{};
		std::uint32_t info{};
		std::uint32_t alignment{};
		std::uint32_t entrySize{};
		std::uint32_t expandedSize{};
		std::string name;
		std::vector<std::byte> data;
	};

	struct Symbol
	{
		std::string name;
		std::uint32_t value{};
		std::uint32_t size{};
		std::uint8_t info{};
		std::uint16_t section{};
		std::optional<std::string> importModule;
		WupsSymbolKind kind{WupsSymbolKind::Function};
	};

	std::uint16_t U16(std::span<const std::byte> bytes, std::size_t offset)
	{
		return (std::to_integer<std::uint16_t>(bytes[offset]) << 8) |
			std::to_integer<std::uint16_t>(bytes[offset + 1]);
	}

	std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset)
	{
		return (std::to_integer<std::uint32_t>(bytes[offset]) << 24) |
			(std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16) |
			(std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8) |
			std::to_integer<std::uint32_t>(bytes[offset + 3]);
	}

	bool IsPowerOfTwo(std::uint32_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	bool Range(std::uint64_t offset, std::uint64_t size, std::uint64_t limit)
	{
		return offset <= limit && size <= limit - offset;
	}

	bool Overlap(std::uint64_t leftOffset, std::uint64_t leftSize,
		std::uint64_t rightOffset, std::uint64_t rightSize)
	{
		return leftSize != 0 && rightSize != 0 && leftOffset < rightOffset + rightSize &&
			rightOffset < leftOffset + leftSize;
	}

	bool Inflate(std::span<const std::byte> input, std::uint32_t expected,
		std::vector<std::byte>& output)
	{
		if (input.empty() || expected == 0)
			return false;
		output.resize(expected);
		z_stream stream{};
		stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
		stream.avail_in = static_cast<uInt>(input.size());
		stream.next_out = reinterpret_cast<Bytef*>(output.data());
		stream.avail_out = expected;
		if (inflateInit(&stream) != Z_OK)
			return false;
		const auto result = inflate(&stream, Z_FINISH);
		const bool valid = result == Z_STREAM_END && stream.avail_in == 0 &&
			stream.avail_out == 0 && stream.total_out == expected;
		inflateEnd(&stream);
		return valid;
	}

	std::optional<std::string> StringAt(std::span<const std::byte> bytes,
		std::uint32_t offset, std::size_t maximum = 4096)
	{
		if (offset >= bytes.size())
			return std::nullopt;
		const auto available = std::min<std::size_t>(bytes.size() - offset, maximum + 1);
		const auto* value = reinterpret_cast<const char*>(bytes.data() + offset);
		const auto length = strnlen(value, available);
		if (length == available || length > maximum)
			return std::nullopt;
		return std::string(value, length);
	}

	bool SafeIdentifier(std::string_view value, std::size_t maximum)
	{
		return !value.empty() && value.size() <= maximum &&
			std::ranges::all_of(value, [](unsigned char c) {
				return std::isalnum(c) || c == '_' || c == '.' || c == '-';
			});
	}

	bool SafeText(std::string_view value, std::size_t maximum)
	{
		return !value.empty() && value.size() <= maximum &&
			std::ranges::all_of(value, [](unsigned char c) {
				return c >= 0x20 || c == '\t' || c == '\n' || c == '\r';
			});
	}

	std::optional<WupsVersion> ParseVersion(std::string_view value)
	{
		std::array<std::uint16_t, 3> parts{};
		for (std::size_t index = 0; index < parts.size(); ++index)
		{
			const auto end = index + 1 == parts.size() ? value.size() : value.find('.');
			if (end == std::string_view::npos || end == 0)
				return std::nullopt;
			unsigned int parsed{};
			const auto result = std::from_chars(value.data(), value.data() + end, parsed);
			if (result.ec != std::errc{} || result.ptr != value.data() + end ||
				parsed > std::numeric_limits<std::uint16_t>::max())
				return std::nullopt;
			parts[index] = static_cast<std::uint16_t>(parsed);
			value.remove_prefix(end + (index + 1 == parts.size() ? 0 : 1));
		}
		if (!value.empty())
			return std::nullopt;
		return WupsVersion{parts[0], parts[1], parts[2]};
	}

	std::optional<std::string> GuestString(const std::vector<Section>& sections,
		std::uint32_t address, std::size_t maximum = 255)
	{
		for (const auto& section : sections)
		{
			if ((section.flags & kShfAlloc) == 0 || section.data.empty() ||
				address < section.address || address - section.address >= section.expandedSize)
				continue;
			return StringAt(section.data, address - section.address, maximum);
		}
		return std::nullopt;
	}

	bool GuestRange(const std::vector<Section>& sections, std::uint32_t address,
		std::uint32_t size, std::uint32_t requiredFlags)
	{
		for (const auto& section : sections)
		{
			if ((section.flags & requiredFlags) != requiredFlags || address < section.address)
				continue;
			const auto offset = address - section.address;
			if (offset <= section.expandedSize && size <= section.expandedSize - offset)
				return true;
		}
		return false;
	}

	bool SupportedProcess(std::uint32_t value)
	{
		return value == 0xff || (value >= 1 && value <= 16 && value != 11 && value != 13 && value != 14);
	}

	std::uint32_t RelocationWidth(std::uint32_t type)
	{
		switch (type)
		{
		case 0: return 0;
		case 4:
		case 5:
		case 6:
		case 11:
		case 251:
		case 252:
		case 253: return 2;
		case 1:
		case 10:
		case 68:
		case 78: return 4;
		default: return std::numeric_limits<std::uint32_t>::max();
		}
	}
}

std::string WupsVersion::ToString() const
{
	return fmt::format("{}.{}.{}", major, minor, patch);
}

std::optional<WupsInspection> WupsBinaryInspector::Inspect(
	std::span<const std::byte> image, std::string& error)
{
	error.clear();
	if (image.size() < 52 || image.size() > kMaximumExpandedBytes)
	{
		error = "WPS image has an invalid size";
		return std::nullopt;
	}
	if (U32(image, 0) != 0x7f454c46U || std::to_integer<unsigned char>(image[4]) != 1 ||
		std::to_integer<unsigned char>(image[5]) != 2 || std::to_integer<unsigned char>(image[6]) != 1 ||
		std::to_integer<unsigned char>(image[7]) != 0xca || std::to_integer<unsigned char>(image[8]) != 0xfe ||
		std::to_integer<unsigned char>(image[9]) != 'P' || std::to_integer<unsigned char>(image[10]) != 'L' ||
		U16(image, 16) != 0xfe01 || U16(image, 18) != 20 || U32(image, 20) != 1 || U16(image, 40) != 52)
	{
		error = "payload is not a Wii U WPS RPL (ELF32 big-endian PPC with PL marker)";
		return std::nullopt;
	}
	if (U32(image, 28) != 0 || U16(image, 44) != 0)
	{
		error = "WPS RPL must not contain a program-header table";
		return std::nullopt;
	}
	const auto sectionOffset = U32(image, 32);
	const auto sectionEntrySize = U16(image, 46);
	const auto sectionCount = U16(image, 48);
	const auto nameSectionIndex = U16(image, 50);
	if (sectionEntrySize != 40 || sectionCount < 5 || sectionCount > kMaximumSections ||
		nameSectionIndex >= sectionCount || !Range(sectionOffset,
			static_cast<std::uint64_t>(sectionEntrySize) * sectionCount, image.size()))
	{
		error = "WPS RPL section table is invalid";
		return std::nullopt;
	}

	std::vector<Section> sections;
	sections.reserve(sectionCount);
	std::uint64_t expandedTotal{};
	std::vector<std::pair<std::uint32_t, std::uint32_t>> fileRanges;
	for (std::uint32_t index = 0; index < sectionCount; ++index)
	{
		const auto offset = sectionOffset + index * sectionEntrySize;
		Section section{U32(image, offset), U32(image, offset + 4), U32(image, offset + 8),
			U32(image, offset + 12), U32(image, offset + 16), U32(image, offset + 20),
			U32(image, offset + 24), U32(image, offset + 28), U32(image, offset + 32),
			U32(image, offset + 36)};
		if (index == 0 && (section.type != kShtNull || section.storedSize != 0))
		{
			error = "WPS RPL null section is invalid";
			return std::nullopt;
		}
		if (section.alignment != 0 && (!IsPowerOfTwo(section.alignment) || section.alignment > 0x10000))
		{
			error = fmt::format("WPS RPL section {} has invalid alignment", index);
			return std::nullopt;
		}
		if ((section.flags & (kShfWrite | kShfExecute)) == (kShfWrite | kShfExecute))
		{
			error = fmt::format("WPS RPL section {} is writable and executable", index);
			return std::nullopt;
		}
		if ((section.flags & kShfAlloc) != 0 && section.alignment != 0 &&
			(section.address & (section.alignment - 1)) != 0)
		{
			error = fmt::format("WPS RPL section {} address is misaligned", index);
			return std::nullopt;
		}
		if (section.type == kShtNobits)
		{
			if ((section.flags & kShfRplCompressed) != 0)
			{
				error = fmt::format("WPS RPL NOBITS section {} cannot be compressed", index);
				return std::nullopt;
			}
			section.expandedSize = section.storedSize;
		}
		else if (section.storedSize != 0)
		{
			if (!Range(section.fileOffset, section.storedSize, image.size()))
			{
				error = fmt::format("WPS RPL section {} data is out of bounds", index);
				return std::nullopt;
			}
			if (Overlap(section.fileOffset, section.storedSize, 0, 52) ||
				Overlap(section.fileOffset, section.storedSize, sectionOffset,
					static_cast<std::uint64_t>(sectionEntrySize) * sectionCount))
			{
				error = fmt::format("WPS RPL section {} overlaps structural data", index);
				return std::nullopt;
			}
			for (const auto& [existingOffset, existingSize] : fileRanges)
				if (Overlap(section.fileOffset, section.storedSize, existingOffset, existingSize))
				{
					error = "WPS RPL section file ranges overlap";
					return std::nullopt;
				}
			fileRanges.emplace_back(section.fileOffset, section.storedSize);
			const auto stored = image.subspan(section.fileOffset, section.storedSize);
			if ((section.flags & kShfRplCompressed) != 0)
			{
				if (stored.size() < 5)
				{
					error = fmt::format("WPS RPL compressed section {} is truncated", index);
					return std::nullopt;
				}
				section.expandedSize = U32(stored, 0);
				if (section.expandedSize == 0 || section.expandedSize > kMaximumExpandedBytes ||
					!Inflate(stored.subspan(4), section.expandedSize, section.data))
				{
					error = fmt::format("WPS RPL compressed section {} is invalid", index);
					return std::nullopt;
				}
			}
			else
			{
				section.expandedSize = section.storedSize;
				section.data.assign(stored.begin(), stored.end());
			}
		}
		if (section.expandedSize > kMaximumExpandedBytes - expandedTotal)
		{
			error = "WPS RPL expanded sections exceed the size limit";
			return std::nullopt;
		}
		expandedTotal += section.expandedSize;
		sections.push_back(std::move(section));
	}

	const auto& nameSection = sections[nameSectionIndex];
	if (nameSection.type != kShtStrtab || nameSection.data.empty() || nameSection.data.back() != std::byte{})
	{
		error = "WPS RPL section-name string table is invalid";
		return std::nullopt;
	}
	std::set<std::string> sectionNames;
	for (auto& section : sections)
	{
		const auto name = StringAt(nameSection.data, section.nameOffset, 255);
		if (!name)
		{
			error = "WPS RPL contains an unterminated section name";
			return std::nullopt;
		}
		section.name = *name;
		if (!section.name.empty() && !sectionNames.insert(section.name).second)
		{
			error = "WPS RPL contains duplicate section names";
			return std::nullopt;
		}
	}

	std::vector<std::pair<std::uint32_t, std::uint32_t>> virtualRanges;
	for (std::size_t index = 0; index < sections.size(); ++index)
	{
		const auto& section = sections[index];
		if ((section.flags & kShfAlloc) == 0 || section.expandedSize == 0)
			continue;
		if (section.address > std::numeric_limits<std::uint32_t>::max() - section.expandedSize)
		{
			error = fmt::format("WPS RPL section '{}' address wraps", section.name);
			return std::nullopt;
		}
		if (section.type == kShtRplImports)
		{
			if (section.address < 0xc0000000U)
			{
				error = fmt::format("WPS import section '{}' is outside loader memory", section.name);
				return std::nullopt;
			}
		}
		else if ((section.flags & kShfExecute) != 0)
		{
			if (section.address < 0x02000000U || section.address >= 0x10000000U)
			{
				error = fmt::format("WPS text section '{}' is outside text memory", section.name);
				return std::nullopt;
			}
		}
		else if (section.address >= 0xc0000000U)
		{
			const bool loaderSection = section.name == ".wut_load_bounds" || section.type == kShtSymtab ||
				section.type == kShtDynsym || section.type == kShtStrtab;
			if (!loaderSection)
			{
				error = fmt::format("WPS section '{}' is unexpectedly placed in loader memory", section.name);
				return std::nullopt;
			}
		}
		else if (section.address < 0x10000000U)
		{
			error = fmt::format("WPS data section '{}' is outside data memory", section.name);
			return std::nullopt;
		}
		for (const auto& [address, size] : virtualRanges)
			if (Overlap(section.address, section.expandedSize, address, size))
			{
				error = "WPS RPL allocated section ranges overlap";
				return std::nullopt;
			}
		virtualRanges.emplace_back(section.address, section.expandedSize);
	}

	if (sections.size() < 2 || sections[sections.size() - 2].type != kShtRplCrcs ||
		sections.back().type != kShtRplFileInfo)
	{
		error = "WPS RPL must end with CRC and FILEINFO sections";
		return std::nullopt;
	}
	const auto& crcSection = sections[sections.size() - 2];
	const auto& fileInfo = sections.back();
	if (crcSection.data.size() != sections.size() * sizeof(std::uint32_t))
	{
		error = "WPS RPL CRC table has an invalid size";
		return std::nullopt;
	}
	if (fileInfo.data.size() < 0x60 || fileInfo.data.size() > 4096 || U32(fileInfo.data, 0) != 0xcafe0402U)
	{
		error = "WPS RPL FILEINFO section is invalid";
		return std::nullopt;
	}
	const auto textRegionSize = U32(fileInfo.data, 4);
	const auto textAlignment = U32(fileInfo.data, 8);
	const auto dataRegionSize = U32(fileInfo.data, 12);
	const auto dataAlignment = U32(fileInfo.data, 16);
	const auto loaderRegionSize = U32(fileInfo.data, 20);
	const auto trampolineAdjustment = U32(fileInfo.data, 32);
	const auto loaderAdjustment = U32(fileInfo.data, 76);
	if (textRegionSize == 0 || textRegionSize > kMaximumExpandedBytes ||
		dataRegionSize > kMaximumExpandedBytes || loaderRegionSize > kMaximumExpandedBytes ||
		!IsPowerOfTwo(textAlignment) || textAlignment > 0x10000 ||
		!IsPowerOfTwo(dataAlignment) || dataAlignment > 0x10000 ||
		trampolineAdjustment > textRegionSize || loaderAdjustment > loaderRegionSize)
	{
		error = "WPS RPL FILEINFO region sizes or alignments are invalid";
		return std::nullopt;
	}
	std::vector<RPLLoaderInternal::ExternalSectionMapping> sectionMappings;
	sectionMappings.reserve(sections.size());
	for (const auto& section : sections)
	{
		sectionMappings.push_back({
			section.type, section.flags, section.address, section.expandedSize});
	}
	const RPLLoaderInternal::ExternalFileInfoMapping fileInfoMapping{
		textRegionSize,
		dataRegionSize,
		loaderRegionSize,
		trampolineAdjustment,
		loaderAdjustment,
	};
	if (const auto violation = RPLLoaderInternal::FindExternalMappingViolation(
		sectionMappings, fileInfoMapping))
	{
		if (violation->reason ==
			RPLLoaderInternal::ExternalMappingViolation::Reason::RegionAddressOverflow)
			error = fmt::format(
				"WPS RPL FILEINFO mapping region for section '{}' overflows the "
				"32-bit guest address space",
				sections[violation->sectionIndex].name);
		else
			error = fmt::format(
				"WPS RPL section '{}' expanded range [0x{:08x}, 0x{:08x}) "
				"exceeds its FILEINFO mapping region [0x{:08x}, 0x{:08x})",
				sections[violation->sectionIndex].name, violation->sectionBegin,
				violation->sectionEnd, violation->regionBegin, violation->regionEnd);
		return std::nullopt;
	}
	for (std::size_t index = 0; index < sections.size(); ++index)
	{
		if (sections[index].type == kShtNobits || sections[index].type == kShtRplCrcs)
			continue;
		const auto expected = U32(crcSection.data, index * 4);
		const auto actual = static_cast<std::uint32_t>(crc32(0,
			reinterpret_cast<const Bytef*>(sections[index].data.data()), sections[index].data.size()));
		if (expected != actual)
		{
			error = fmt::format("WPS RPL section '{}' CRC does not match", sections[index].name);
			return std::nullopt;
		}
	}

	WupsInspection inspection;
	std::set<std::tuple<std::string, WupsSymbolKind>> uniqueExports;
	for (const auto& section : sections)
	{
		inspection.sections.push_back({section.name, section.type, section.flags, section.address,
			section.storedSize, section.expandedSize, section.alignment,
			(section.flags & kShfRplCompressed) != 0, (section.flags & kShfTls) != 0});
		inspection.usesTls = inspection.usesTls || (section.flags & kShfTls) != 0;
	}

	std::optional<std::size_t> symbolSectionIndex;
	for (std::size_t index = 0; index < sections.size(); ++index)
		if (sections[index].type == kShtSymtab || sections[index].type == kShtDynsym)
		{
			if (symbolSectionIndex)
			{
				error = "WPS RPL contains multiple symbol tables";
				return std::nullopt;
			}
			symbolSectionIndex = index;
		}
	if (!symbolSectionIndex)
	{
		error = "WPS RPL is missing a symbol table";
		return std::nullopt;
	}
	const auto& symbolSection = sections[*symbolSectionIndex];
	if (symbolSection.entrySize != 16 || symbolSection.data.size() % 16 != 0 ||
		symbolSection.link >= sections.size() || sections[symbolSection.link].type != kShtStrtab ||
		sections[symbolSection.link].data.empty() || sections[symbolSection.link].data.back() != std::byte{})
	{
		error = "WPS RPL symbol or string table is invalid";
		return std::nullopt;
	}
	const auto& strings = sections[symbolSection.link].data;
	std::vector<Symbol> symbols;
	std::set<std::tuple<std::string, std::string, WupsSymbolKind>> uniqueImports;
	for (std::size_t offset = 0; offset < symbolSection.data.size(); offset += 16)
	{
		const auto nameOffset = U32(symbolSection.data, offset);
		const auto name = StringAt(strings, nameOffset, 1024);
		if (!name)
		{
			error = "WPS RPL symbol table contains an invalid name";
			return std::nullopt;
		}
		Symbol symbol{*name, U32(symbolSection.data, offset + 4), U32(symbolSection.data, offset + 8),
			std::to_integer<std::uint8_t>(symbolSection.data[offset + 12]), U16(symbolSection.data, offset + 14)};
		if (symbol.section < sections.size() && sections[symbol.section].type == kShtRplImports)
		{
			const auto& importSection = sections[symbol.section];
			const bool function = importSection.name.starts_with(".fimport_");
			const bool data = importSection.name.starts_with(".dimport_");
			if ((!function && !data) || importSection.name.size() <= 9 ||
				!SafeIdentifier(std::string_view(importSection.name).substr(9), 64) ||
				symbol.name.empty() || symbol.name.size() > 1024 ||
				(function && (symbol.info & 0xf) != 2) || (data && (symbol.info & 0xf) != 1) ||
				symbol.value < importSection.address ||
				symbol.value - importSection.address >= importSection.expandedSize)
			{
				error = fmt::format("WPS import symbol '{}' has an invalid function/data declaration", symbol.name);
				return std::nullopt;
			}
			symbol.importModule = importSection.name.substr(9);
			symbol.kind = function ? WupsSymbolKind::Function : WupsSymbolKind::Data;
			if (!uniqueImports.emplace(*symbol.importModule, symbol.name, symbol.kind).second)
			{
				error = "WPS RPL contains a duplicate import symbol";
				return std::nullopt;
			}
			inspection.imports.push_back({*symbol.importModule, symbol.name, symbol.kind, symbol.value, true});
		}
		symbols.push_back(std::move(symbol));
	}
	std::set<std::string> requiredModules;
	for (const auto& value : inspection.imports)
		requiredModules.insert(value.module);
	inspection.requiredModules.assign(requiredModules.begin(), requiredModules.end());

	std::set<std::uint32_t> relocationTypes;
	for (const auto& relocationSection : sections)
	{
		if (relocationSection.type != kShtRela && relocationSection.type != kShtRel)
			continue;
		const auto expectedSize = relocationSection.type == kShtRela ? 12U : 8U;
		if (relocationSection.entrySize != expectedSize || relocationSection.data.size() % expectedSize != 0 ||
			relocationSection.link != *symbolSectionIndex || relocationSection.info >= sections.size())
		{
			error = fmt::format("WPS relocation section '{}' is invalid", relocationSection.name);
			return std::nullopt;
		}
		const auto& target = sections[relocationSection.info];
		for (std::size_t offset = 0; offset < relocationSection.data.size(); offset += expectedSize)
		{
			const auto targetAddress = U32(relocationSection.data, offset);
			const auto info = U32(relocationSection.data, offset + 4);
			const auto symbolIndex = info >> 8;
			const auto type = info & 0xff;
			const auto width = RelocationWidth(type);
			if (symbolIndex >= symbols.size() || width == std::numeric_limits<std::uint32_t>::max() ||
				targetAddress < target.address || targetAddress - target.address > target.expandedSize ||
				width > target.expandedSize - (targetAddress - target.address))
			{
				error = fmt::format("WPS relocation in '{}' is invalid or unsupported", relocationSection.name);
				return std::nullopt;
			}
			const auto& symbol = symbols[symbolIndex];
			inspection.relocations.push_back({target.name, targetAddress, type, symbol.name,
				symbol.importModule, symbol.kind});
			relocationTypes.insert(type);
		}
	}
	inspection.relocationTypes.assign(relocationTypes.begin(), relocationTypes.end());

	for (const auto& section : sections)
	{
		if (section.type != kShtRplExports)
			continue;
		if (section.data.size() < 8)
		{
			error = "WPS RPL export table is truncated";
			return std::nullopt;
		}
		const auto count = U32(section.data, 0);
		if (count > kMaximumDescriptors || !Range(8, static_cast<std::uint64_t>(count) * 8, section.data.size()))
		{
			error = "WPS RPL export descriptor count is invalid";
			return std::nullopt;
		}
		const auto kind = (section.flags & kShfExecute) != 0 ? WupsSymbolKind::Function : WupsSymbolKind::Data;
		for (std::uint32_t index = 0; index < count; ++index)
		{
			const auto address = U32(section.data, 8 + index * 8);
			const auto name = StringAt(section.data, U32(section.data, 12 + index * 8), 1024);
			const auto requiredFlags = kind == WupsSymbolKind::Function ?
				(kShfAlloc | kShfExecute) : kShfAlloc;
			if (!name || name->empty() || !uniqueExports.emplace(*name, kind).second ||
				(kind == WupsSymbolKind::Function && (address & 3U) != 0) ||
				!GuestRange(sections, address, kind == WupsSymbolKind::Function ? 4 : 1, requiredFlags))
			{
				error = "WPS RPL export table contains an invalid, duplicate, or out-of-range export";
				return std::nullopt;
			}
			inspection.exports.push_back({{}, *name, kind, address, true});
		}
	}

	const auto metadataSection = std::ranges::find_if(sections, [](const Section& value) {
		return value.name == ".wups.meta";
	});
	if (metadataSection == sections.end() || metadataSection->type != kShtProgbits ||
		(metadataSection->flags & kShfAlloc) == 0 || metadataSection->data.empty() ||
		metadataSection->data.back() != std::byte{})
	{
		error = "WPS image is missing a valid .wups.meta section";
		return std::nullopt;
	}
	std::map<std::string, std::string> metadata;
	for (std::size_t offset = 0; offset < metadataSection->data.size();)
	{
		const auto entry = StringAt(metadataSection->data, static_cast<std::uint32_t>(offset), 4096);
		if (!entry)
		{
			error = ".wups.meta contains an unterminated value";
			return std::nullopt;
		}
		offset += entry->size() + 1;
		if (entry->empty())
			continue;
		const auto separator = entry->find('=');
		if (separator == std::string::npos)
			continue;
		const auto key = entry->substr(0, separator);
		const auto value = entry->substr(separator + 1);
		if (!SafeIdentifier(key, 64) || value.size() > 4096)
		{
			error = ".wups.meta contains an invalid key or oversized value";
			return std::nullopt;
		}
		if (!metadata.emplace(key, value).second)
		{
			error = fmt::format(".wups.meta contains duplicate key '{}'", key);
			return std::nullopt;
		}
	}
	if (!metadata.contains("name") || !metadata.contains("wups") ||
		!SafeText(metadata["name"], 128))
	{
		error = ".wups.meta is missing required name or wups metadata";
		return std::nullopt;
	}
	const auto parsedVersion = ParseVersion(metadata["wups"]);
	if (!parsedVersion)
	{
		error = fmt::format("plugin '{}' has malformed WUPS ABI version '{}'", metadata["name"], metadata["wups"]);
		return std::nullopt;
	}
	static constexpr std::array<WupsVersion, 5> supported{{
		{0, 7, 1}, {0, 8, 1}, {0, 8, 2}, {0, 9, 0}, {0, 9, 1},
	}};
	if (std::ranges::find(supported, *parsedVersion) == supported.end())
	{
		error = fmt::format("plugin '{}' uses unsupported WUPS ABI {}; supported versions are 0.7.1, 0.8.1, 0.8.2, 0.9.0, and 0.9.1",
			metadata["name"], parsedVersion->ToString());
		return std::nullopt;
	}
	inspection.metadata.name = metadata["name"];
	inspection.metadata.abiVersion = *parsedVersion;
	auto take = [&](std::string_view key, std::string& destination) {
		if (const auto found = metadata.find(std::string(key)); found != metadata.end()) destination = found->second;
	};
	take("author", inspection.metadata.author);
	take("version", inspection.metadata.version);
	take("license", inspection.metadata.license);
	take("description", inspection.metadata.description);
	take("buildtimestamp", inspection.metadata.buildTimestamp);
	take("storage_id", inspection.metadata.storageId);
	if (!inspection.metadata.storageId.empty() && !SafeIdentifier(inspection.metadata.storageId, 128))
	{
		error = ".wups.meta storage_id is invalid";
		return std::nullopt;
	}
	if (const auto found = metadata.find("debug"); found != metadata.end())
	{
		if (found->second == "track_heap")
			inspection.metadata.trackHeap = true;
		else if (found->second == "track_heap_with_stack_trace")
			inspection.metadata.trackHeap = inspection.metadata.collectHeapStackTraces = true;
		else
		{
			error = fmt::format(".wups.meta contains invalid debug flag '{}'", found->second);
			return std::nullopt;
		}
	}
	static const std::set<std::string> knownMetadata{
		"name", "author", "version", "license", "description", "wups", "buildtimestamp", "storage_id", "debug"};
	for (const auto& [key, value] : metadata)
		if (!knownMetadata.contains(key)) inspection.metadata.unknown.emplace(key, value);
	if (*parsedVersion != WupsVersion{0, 9, 1})
		inspection.compatibilityWarnings.push_back(fmt::format(
			"plugin uses legacy WUPS ABI {}; runtime compatibility handling is required", parsedVersion->ToString()));

	const auto hooksSection = std::ranges::find_if(sections, [](const Section& value) {
		return value.name == ".wups.hooks";
	});
	if (hooksSection == sections.end() || hooksSection->type != kShtProgbits ||
		(hooksSection->flags & kShfAlloc) == 0 || hooksSection->data.empty() ||
		hooksSection->data.size() % 8 != 0 || hooksSection->data.size() / 8 > 64)
	{
		error = "WPS image is missing a valid .wups.hooks section";
		return std::nullopt;
	}
	std::set<std::uint32_t> hookTypes;
	for (std::size_t offset = 0; offset < hooksSection->data.size(); offset += 8)
	{
		const auto type = U32(hooksSection->data, offset);
		const auto target = U32(hooksSection->data, offset + 4);
		if (type > static_cast<std::uint32_t>(WupsHookType::InitReentFunctions) ||
			!hookTypes.insert(type).second || (target & 3U) != 0 ||
			!GuestRange(sections, target, 4, kShfAlloc | kShfExecute))
		{
			error = ".wups.hooks contains an invalid, duplicate, or non-executable descriptor";
			return std::nullopt;
		}
		inspection.hooks.push_back({static_cast<WupsHookType>(type), target});
	}

	const auto loadSection = std::ranges::find_if(sections, [](const Section& value) {
		return value.name == ".wups.load";
	});
	if (loadSection != sections.end())
	{
		if (loadSection->type != kShtProgbits || (loadSection->flags & kShfAlloc) == 0 ||
			loadSection->data.size() % 36 != 0 || loadSection->data.size() / 36 > kMaximumDescriptors)
		{
			error = ".wups.load has an invalid descriptor size";
			return std::nullopt;
		}
		std::set<std::uint32_t> processTargets;
		for (std::size_t offset = 0; offset < loadSection->data.size(); offset += 36)
		{
			const auto type = U32(loadSection->data, offset);
			const auto physical = U32(loadSection->data, offset + 4);
			const auto virtualAddress = U32(loadSection->data, offset + 8);
			const auto name = GuestString(sections, U32(loadSection->data, offset + 12));
			const auto library = U32(loadSection->data, offset + 16);
			const auto replacementName = GuestString(sections, U32(loadSection->data, offset + 20));
			const auto target = U32(loadSection->data, offset + 24);
			const auto callThrough = U32(loadSection->data, offset + 28);
			const auto process = U32(loadSection->data, offset + 32);
			const bool fixedAddress = physical != 0 || virtualAddress != 0;
			if (type > static_cast<std::uint32_t>(WupsLoadEntryType::LegacyExport) ||
				!name || name->empty() || !replacementName || replacementName->empty() ||
				library > kWupsLibraryOther || !SupportedProcess(process) || (target & 3U) != 0 ||
				(callThrough & 3U) != 0 || !GuestRange(sections, target, 4, kShfAlloc | kShfExecute) ||
				!GuestRange(sections, callThrough, 4, kShfAlloc | kShfWrite) ||
				(fixedAddress && library != kWupsLibraryOther) || (!fixedAddress && library == kWupsLibraryOther))
			{
				error = ".wups.load contains an invalid replacement descriptor";
				return std::nullopt;
			}
			inspection.replacements.push_back({static_cast<WupsLoadEntryType>(type), type == 1,
				physical, virtualAddress, *name, library,
				*replacementName, target, callThrough, process});
			inspection.usesFixedAddressPatches = inspection.usesFixedAddressPatches || fixedAddress;
			processTargets.insert(process);
			if (type == static_cast<std::uint32_t>(WupsLoadEntryType::LegacyExport))
				inspection.compatibilityWarnings.push_back(fmt::format(
					".wups.load legacy export '{}' requires explicit runtime compatibility", *name));
		}
		inspection.processTargets.assign(processTargets.begin(), processTargets.end());
	}

	return inspection;
}
