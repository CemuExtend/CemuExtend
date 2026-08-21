#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WumsBinary.h"
#include "Cafe/OS/RPL/RPLExternalModulePolicy.h"

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
	constexpr std::uint32_t kShtRplImports = 0x80000002U;
	constexpr std::uint32_t kShtRplCrcs = 0x80000003U;
	constexpr std::uint32_t kShtRplFileInfo = 0x80000004U;
	constexpr std::uint32_t kShfWrite = 1;
	constexpr std::uint32_t kShfAlloc = 2;
	constexpr std::uint32_t kShfExecute = 4;
	constexpr std::uint32_t kShfTls = 0x400;
	constexpr std::uint32_t kShfRplCompressed = 0x08000000U;

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

	bool Range(std::uint64_t offset, std::uint64_t size, std::uint64_t limit)
	{
		return offset <= limit && size <= limit - offset;
	}

	bool Overlap(std::uint64_t leftOffset, std::uint64_t leftSize,
				 std::uint64_t rightOffset, std::uint64_t rightSize)
	{
		return leftSize != 0 && rightSize != 0 &&
			   leftOffset < rightOffset + rightSize &&
			   rightOffset < leftOffset + leftSize;
	}

	bool IsPowerOfTwo(std::uint32_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	bool SafeIdentifier(std::string_view value, std::size_t maximum)
	{
		return !value.empty() && value.size() <= maximum &&
			   std::ranges::all_of(value, [](unsigned char character) {
				   return std::isalnum(character) || character == '_' ||
						  character == '.' || character == '-';
			   });
	}

	bool SafeText(std::string_view value, std::size_t maximum)
	{
		return !value.empty() && value.size() <= maximum &&
			   std::ranges::all_of(value, [](unsigned char character) {
				   return character >= 0x20 || character == '\t' ||
						  character == '\n' || character == '\r';
			   });
	}

	std::optional<std::string> StringAt(std::span<const std::byte> bytes,
										std::uint32_t offset, std::size_t maximum)
	{
		if (offset >= bytes.size())
			return std::nullopt;
		const auto available = std::min<std::size_t>(
			bytes.size() - offset, maximum + 1);
		const auto* value = reinterpret_cast<const char*>(bytes.data() + offset);
		const auto size = strnlen(value, available);
		if (size == available || size > maximum)
			return std::nullopt;
		return std::string(value, size);
	}

	bool Inflate(std::span<const std::byte> input, std::uint32_t expected,
				 std::vector<std::byte>& output)
	{
		if (input.empty() || expected == 0)
			return false;
		output.resize(expected);
		z_stream stream{};
		stream.next_in =
			reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
		stream.avail_in = static_cast<uInt>(input.size());
		stream.next_out = reinterpret_cast<Bytef*>(output.data());
		stream.avail_out = expected;
		if (inflateInit(&stream) != Z_OK)
			return false;
		const auto status = inflate(&stream, Z_FINISH);
		const bool valid = status == Z_STREAM_END && stream.avail_in == 0 &&
						   stream.avail_out == 0 && stream.total_out == expected;
		inflateEnd(&stream);
		return valid;
	}

	std::optional<WupsVersion> ParseVersion(std::string_view value)
	{
		std::array<std::uint16_t, 3> parts{};
		for (std::size_t index = 0; index < parts.size(); ++index)
		{
			const auto separator = index + 1 == parts.size() ? value.size() : value.find('.');
			if (separator == std::string_view::npos || separator == 0)
				return std::nullopt;
			unsigned parsed{};
			const auto result = std::from_chars(
				value.data(), value.data() + separator, parsed);
			if (result.ec != std::errc{} ||
				result.ptr != value.data() + separator ||
				parsed > std::numeric_limits<std::uint16_t>::max())
				return std::nullopt;
			parts[index] = static_cast<std::uint16_t>(parsed);
			value.remove_prefix(separator +
								(index + 1 == parts.size() ? 0 : 1));
		}
		if (!value.empty())
			return std::nullopt;
		return WupsVersion{parts[0], parts[1], parts[2]};
	}

	bool GuestRange(const std::vector<Section>& sections, std::uint32_t address,
					std::uint32_t size, std::uint32_t flags)
	{
		for (const auto& section : sections)
		{
			if ((section.flags & flags) != flags || address < section.address)
				continue;
			const auto offset = address - section.address;
			if (offset <= section.expandedSize &&
				size <= section.expandedSize - offset)
				return true;
		}
		return false;
	}

	std::optional<std::string> GuestString(
		const std::vector<Section>& sections, std::uint32_t address)
	{
		for (const auto& section : sections)
		{
			if ((section.flags & kShfAlloc) == 0 || section.data.empty() ||
				address < section.address)
				continue;
			const auto offset = address - section.address;
			if (offset < section.expandedSize)
				return StringAt(section.data, offset, 1024);
		}
		return std::nullopt;
	}

	std::uint32_t RelocationWidth(std::uint32_t type)
	{
		switch (type)
		{
		case 0:
			return 0;
		case 4:
		case 5:
		case 6:
		case 11:
		case 251:
		case 252:
		case 253:
			return 2;
		case 1:
		case 10:
		case 68:
		case 78:
			return 4;
		default:
			return std::numeric_limits<std::uint32_t>::max();
		}
	}

	std::optional<WumsDependency> ParseDependency(
		std::string_view raw, std::string& error)
	{
		WumsDependency dependency;
		if (raw.starts_with('?'))
		{
			dependency.optional = true;
			raw.remove_prefix(1);
		}
		const auto versionSeparator = raw.find('@');
		const auto moduleName = raw.substr(0, versionSeparator);
		if (!SafeIdentifier(moduleName, 128))
		{
			error = fmt::format("invalid WUMS dependency '{}'", raw);
			return std::nullopt;
		}
		dependency.moduleName = moduleName;
		if (versionSeparator == std::string_view::npos)
			return dependency;
		auto constraint = raw.substr(versionSeparator + 1);
		if (constraint.starts_with(">="))
		{
			dependency.match = WumsDependencyMatch::AtLeast;
			constraint.remove_prefix(2);
		}
		else if (constraint.starts_with('='))
		{
			dependency.match = WumsDependencyMatch::Exact;
			constraint.remove_prefix(1);
		}
		else
		{
			error = fmt::format(
				"WUMS dependency '{}' has an unsupported version constraint", raw);
			return std::nullopt;
		}
		dependency.version = ParseVersion(constraint);
		if (!dependency.version)
		{
			error = fmt::format(
				"WUMS dependency '{}' has a malformed semantic version", raw);
			return std::nullopt;
		}
		return dependency;
	}
} // namespace

std::optional<WumsInspection> WumsBinaryInspector::Inspect(
	std::span<const std::byte> image, std::string& error)
{
	error.clear();
	if (image.size() < 52 || image.size() > kMaximumExpandedBytes)
	{
		error = "WMS image has an invalid size";
		return std::nullopt;
	}
	if (U32(image, 0) != 0x7f454c46U ||
		std::to_integer<std::uint8_t>(image[4]) != 1 ||
		std::to_integer<std::uint8_t>(image[5]) != 2 ||
		std::to_integer<std::uint8_t>(image[6]) != 1 ||
		std::to_integer<std::uint8_t>(image[7]) != 0xca ||
		std::to_integer<std::uint8_t>(image[8]) != 0xfe ||
		std::to_integer<std::uint8_t>(image[9]) != 0xaf ||
		std::to_integer<std::uint8_t>(image[10]) != 0xfe ||
		U16(image, 16) != 0xfe01 || U16(image, 18) != 20 ||
		U32(image, 20) != 1 || U16(image, 40) != 52)
	{
		error = "payload is not a Wii U WUMS RPL";
		return std::nullopt;
	}
	const auto sectionOffset = U32(image, 32);
	const auto sectionEntrySize = U16(image, 46);
	const auto sectionCount = U16(image, 48);
	const auto nameSectionIndex = U16(image, 50);
	if (U32(image, 28) != 0 || U16(image, 44) != 0 ||
		sectionEntrySize != 40 || sectionCount < 5 ||
		sectionCount > kMaximumSections || nameSectionIndex >= sectionCount ||
		!Range(sectionOffset,
			   static_cast<std::uint64_t>(sectionEntrySize) * sectionCount,
			   image.size()))
	{
		error = "WMS RPL section table is invalid";
		return std::nullopt;
	}

	std::vector<Section> sections;
	sections.reserve(sectionCount);
	std::vector<std::pair<std::uint32_t, std::uint32_t>> fileRanges;
	std::uint64_t expandedTotal{};
	for (std::uint32_t index = 0; index < sectionCount; ++index)
	{
		const auto offset = sectionOffset + index * sectionEntrySize;
		Section section{
			U32(image, offset), U32(image, offset + 4),
			U32(image, offset + 8), U32(image, offset + 12),
			U32(image, offset + 16), U32(image, offset + 20),
			U32(image, offset + 24), U32(image, offset + 28),
			U32(image, offset + 32), U32(image, offset + 36)};
		if (index == 0 &&
			(section.type != kShtNull || section.storedSize != 0))
		{
			error = "WMS RPL null section is invalid";
			return std::nullopt;
		}
		if (section.alignment != 0 &&
			(!IsPowerOfTwo(section.alignment) ||
			 section.alignment > 0x10000U))
		{
			error = fmt::format(
				"WMS RPL section {} has invalid alignment", index);
			return std::nullopt;
		}
		if ((section.flags & (kShfWrite | kShfExecute)) ==
			(kShfWrite | kShfExecute))
		{
			error = fmt::format(
				"WMS RPL section {} violates write-xor-execute", index);
			return std::nullopt;
		}
		if ((section.flags & kShfAlloc) != 0 && section.alignment != 0 &&
			(section.address & (section.alignment - 1)) != 0)
		{
			error = fmt::format(
				"WMS RPL section {} address is misaligned", index);
			return std::nullopt;
		}
		if (section.type == kShtNobits)
		{
			if ((section.flags & kShfRplCompressed) != 0)
			{
				error = "WMS RPL NOBITS section cannot be compressed";
				return std::nullopt;
			}
			section.expandedSize = section.storedSize;
		}
		else if (section.storedSize != 0)
		{
			if (!Range(section.fileOffset, section.storedSize, image.size()) ||
				Overlap(section.fileOffset, section.storedSize, 0, 52) ||
				Overlap(section.fileOffset, section.storedSize, sectionOffset,
						static_cast<std::uint64_t>(sectionEntrySize) * sectionCount))
			{
				error = fmt::format(
					"WMS RPL section {} data is out of bounds", index);
				return std::nullopt;
			}
			for (const auto& [existingOffset, existingSize] : fileRanges)
				if (Overlap(section.fileOffset, section.storedSize,
							existingOffset, existingSize))
				{
					error = "WMS RPL section file ranges overlap";
					return std::nullopt;
				}
			fileRanges.emplace_back(section.fileOffset, section.storedSize);
			const auto stored =
				image.subspan(section.fileOffset, section.storedSize);
			if ((section.flags & kShfRplCompressed) != 0)
			{
				if (stored.size() < 5)
				{
					error = "WMS RPL compressed section is truncated";
					return std::nullopt;
				}
				section.expandedSize = U32(stored, 0);
				if (section.expandedSize == 0 ||
					section.expandedSize > kMaximumExpandedBytes ||
					!Inflate(stored.subspan(4),
							 section.expandedSize, section.data))
				{
					error = "WMS RPL compressed section is invalid";
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
			error = "WMS RPL expanded sections exceed the size limit";
			return std::nullopt;
		}
		expandedTotal += section.expandedSize;
		sections.push_back(std::move(section));
	}

	const auto& nameSection = sections[nameSectionIndex];
	if (nameSection.type != kShtStrtab || nameSection.data.empty() ||
		nameSection.data.back() != std::byte{})
	{
		error = "WMS RPL section-name string table is invalid";
		return std::nullopt;
	}
	std::set<std::string> sectionNames;
	for (auto& section : sections)
	{
		const auto name = StringAt(nameSection.data, section.nameOffset, 255);
		if (!name)
		{
			error = "WMS RPL contains an unterminated section name";
			return std::nullopt;
		}
		section.name = *name;
		if (!section.name.empty() &&
			!sectionNames.insert(section.name).second)
		{
			error = "WMS RPL contains duplicate section names";
			return std::nullopt;
		}
	}

	std::vector<std::pair<std::uint32_t, std::uint32_t>> virtualRanges;
	for (const auto& section : sections)
	{
		if ((section.flags & kShfAlloc) == 0 || section.expandedSize == 0)
			continue;
		if (section.address >
			std::numeric_limits<std::uint32_t>::max() - section.expandedSize)
		{
			error = fmt::format(
				"WMS RPL section '{}' address wraps", section.name);
			return std::nullopt;
		}
		if (section.type == kShtRplImports)
		{
			if (section.address < 0xc0000000U)
			{
				error = "WMS RPL import section is outside loader memory";
				return std::nullopt;
			}
		}
		else if ((section.flags & kShfExecute) != 0)
		{
			if (section.address < 0x02000000U ||
				section.address >= 0x10000000U)
			{
				error = "WMS RPL executable section is outside text memory";
				return std::nullopt;
			}
		}
		else if (section.address < 0x10000000U ||
				 (section.address >= 0xc0000000U &&
				  section.type != kShtSymtab &&
				  section.type != kShtDynsym &&
				  section.type != kShtStrtab &&
				  section.name != ".wut_load_bounds"))
		{
			error = fmt::format(
				"WMS RPL data section '{}' is outside an allowed region",
				section.name);
			return std::nullopt;
		}
		for (const auto& [address, size] : virtualRanges)
			if (Overlap(section.address, section.expandedSize, address, size))
			{
				error = "WMS RPL allocated section ranges overlap";
				return std::nullopt;
			}
		virtualRanges.emplace_back(section.address, section.expandedSize);
	}

	if (sections.size() < 2 ||
		sections[sections.size() - 2].type != kShtRplCrcs ||
		sections.back().type != kShtRplFileInfo)
	{
		error = "WMS RPL must end with CRC and FILEINFO sections";
		return std::nullopt;
	}
	const auto& crcSection = sections[sections.size() - 2];
	const auto& fileInfo = sections.back();
	if (crcSection.data.size() != sections.size() * 4 ||
		fileInfo.data.size() < 0x60 || fileInfo.data.size() > 4096 ||
		U32(fileInfo.data, 0) != 0xcafe0402U)
	{
		error = "WMS RPL CRC or FILEINFO section is invalid";
		return std::nullopt;
	}
	const RPLLoaderInternal::ExternalFileInfoMapping fileInfoMapping{
		U32(fileInfo.data, 4), U32(fileInfo.data, 12),
		U32(fileInfo.data, 20), U32(fileInfo.data, 32),
		U32(fileInfo.data, 76)};
	if (fileInfoMapping.textRegionSize == 0 ||
		fileInfoMapping.textRegionSize > kMaximumExpandedBytes ||
		fileInfoMapping.dataRegionSize > kMaximumExpandedBytes ||
		fileInfoMapping.loaderRegionSize > kMaximumExpandedBytes ||
		fileInfoMapping.trampolineAdjustment >
			fileInfoMapping.textRegionSize ||
		fileInfoMapping.loaderAdjustment >
			fileInfoMapping.loaderRegionSize ||
		!IsPowerOfTwo(U32(fileInfo.data, 8)) ||
		!IsPowerOfTwo(U32(fileInfo.data, 16)))
	{
		error = "WMS RPL FILEINFO region values are invalid";
		return std::nullopt;
	}
	std::vector<RPLLoaderInternal::ExternalSectionMapping> mappings;
	for (const auto& section : sections)
		mappings.push_back({section.type, section.flags, section.address, section.expandedSize});
	if (RPLLoaderInternal::FindExternalMappingViolation(
			mappings, fileInfoMapping))
	{
		error = "WMS RPL section exceeds its FILEINFO mapping region";
		return std::nullopt;
	}
	for (std::size_t index = 0; index < sections.size(); ++index)
	{
		if (sections[index].type == kShtNobits ||
			sections[index].type == kShtRplCrcs)
			continue;
		const auto actual = static_cast<std::uint32_t>(crc32(0,
															 reinterpret_cast<const Bytef*>(sections[index].data.data()),
															 sections[index].data.size()));
		if (U32(crcSection.data, index * 4) != actual)
		{
			error = fmt::format(
				"WMS RPL section '{}' CRC does not match",
				sections[index].name);
			return std::nullopt;
		}
	}

	WumsInspection inspection;
	for (const auto& section : sections)
	{
		inspection.sections.push_back({section.name, section.type, section.flags, section.address,
									   section.storedSize, section.expandedSize, section.alignment,
									   (section.flags & kShfRplCompressed) != 0,
									   (section.flags & kShfTls) != 0});
		inspection.usesTls =
			inspection.usesTls || (section.flags & kShfTls) != 0;
	}

	std::optional<std::size_t> symbolSectionIndex;
	for (std::size_t index = 0; index < sections.size(); ++index)
		if (sections[index].type == kShtSymtab ||
			sections[index].type == kShtDynsym)
		{
			if (symbolSectionIndex)
			{
				error = "WMS RPL contains multiple symbol tables";
				return std::nullopt;
			}
			symbolSectionIndex = index;
		}
	if (!symbolSectionIndex)
	{
		error = "WMS RPL is missing a symbol table";
		return std::nullopt;
	}
	const auto& symbolSection = sections[*symbolSectionIndex];
	if (symbolSection.entrySize != 16 ||
		symbolSection.data.size() % 16 != 0 ||
		symbolSection.link >= sections.size() ||
		sections[symbolSection.link].type != kShtStrtab ||
		sections[symbolSection.link].data.empty() ||
		sections[symbolSection.link].data.back() != std::byte{})
	{
		error = "WMS RPL symbol or string table is invalid";
		return std::nullopt;
	}
	const auto& strings = sections[symbolSection.link].data;
	std::vector<Symbol> symbols;
	std::set<std::tuple<std::string, std::string, WupsSymbolKind>> imports;
	for (std::size_t offset = 0;
		 offset < symbolSection.data.size(); offset += 16)
	{
		const auto name = StringAt(
			strings, U32(symbolSection.data, offset), 1024);
		if (!name)
		{
			error = "WMS RPL symbol table has an invalid name";
			return std::nullopt;
		}
		Symbol symbol{
			*name, U32(symbolSection.data, offset + 4),
			std::to_integer<std::uint8_t>(
				symbolSection.data[offset + 12]),
			U16(symbolSection.data, offset + 14)};
		if (symbol.section < sections.size() &&
			sections[symbol.section].type == kShtRplImports)
		{
			const auto& importSection = sections[symbol.section];
			const bool function =
				importSection.name.starts_with(".fimport_");
			const bool data = importSection.name.starts_with(".dimport_");
			const auto module = importSection.name.size() > 9 ? std::string_view(importSection.name).substr(9) : std::string_view{};
			if ((!function && !data) || !SafeIdentifier(module, 128) ||
				symbol.name.empty() ||
				(function && (symbol.info & 0xf) != 2) ||
				(data && (symbol.info & 0xf) != 1) ||
				symbol.value < importSection.address ||
				symbol.value - importSection.address >=
					importSection.expandedSize)
			{
				error = fmt::format(
					"WMS import '{}' has an invalid function/data declaration",
					symbol.name);
				return std::nullopt;
			}
			symbol.importModule = std::string(module);
			symbol.kind = function ? WupsSymbolKind::Function : WupsSymbolKind::Data;
			if (!imports.emplace(
							*symbol.importModule, symbol.name, symbol.kind)
					 .second)
			{
				error = "WMS RPL contains duplicate imports";
				return std::nullopt;
			}
			inspection.imports.push_back({*symbol.importModule, symbol.name, symbol.kind,
										  symbol.value, true});
		}
		symbols.push_back(std::move(symbol));
	}

	for (const auto& relocationSection : sections)
	{
		if (relocationSection.type != kShtRela &&
			relocationSection.type != kShtRel)
			continue;
		const auto entrySize =
			relocationSection.type == kShtRela ? 12U : 8U;
		if (relocationSection.entrySize != entrySize ||
			relocationSection.data.size() % entrySize != 0 ||
			relocationSection.link != *symbolSectionIndex ||
			relocationSection.info >= sections.size())
		{
			error = fmt::format(
				"WMS relocation section '{}' is invalid",
				relocationSection.name);
			return std::nullopt;
		}
		const auto& target = sections[relocationSection.info];
		for (std::size_t offset = 0;
			 offset < relocationSection.data.size(); offset += entrySize)
		{
			const auto targetAddress =
				U32(relocationSection.data, offset);
			const auto info = U32(relocationSection.data, offset + 4);
			const auto symbolIndex = info >> 8;
			const auto type = info & 0xff;
			const auto width = RelocationWidth(type);
			if (symbolIndex >= symbols.size() ||
				width == std::numeric_limits<std::uint32_t>::max() ||
				targetAddress < target.address ||
				targetAddress - target.address > target.expandedSize ||
				width > target.expandedSize -
							(targetAddress - target.address))
			{
				error = fmt::format(
					"WMS relocation in '{}' is invalid or unsupported",
					relocationSection.name);
				return std::nullopt;
			}
			const auto& symbol = symbols[symbolIndex];
			inspection.relocations.push_back({target.name, targetAddress, type, symbol.name,
											  symbol.importModule, symbol.kind});
		}
	}

	const auto metadataSection = std::ranges::find_if(
		sections, [](const Section& section) {
			return section.name == ".wums.meta";
		});
	if (metadataSection == sections.end() ||
		metadataSection->type != kShtProgbits ||
		(metadataSection->flags & kShfAlloc) == 0 ||
		metadataSection->data.empty() ||
		metadataSection->data.back() != std::byte{})
	{
		error = "WMS image is missing a valid .wums.meta section";
		return std::nullopt;
	}
	std::map<std::string, std::string> metadata;
	for (std::size_t offset = 0; offset < metadataSection->data.size();)
	{
		const auto entry = StringAt(
			metadataSection->data, static_cast<std::uint32_t>(offset), 4096);
		if (!entry)
		{
			error = ".wums.meta contains an unterminated value";
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
		if (!SafeIdentifier(key, 64) || value.size() > 4096 ||
			!metadata.emplace(key, value).second)
		{
			error = fmt::format(
				".wums.meta contains invalid or duplicate key '{}'", key);
			return std::nullopt;
		}
	}
	const auto versionEntry = metadata.contains("wums") ? metadata.find("wums") : metadata.find("wum");
	if (!metadata.contains("export_name") ||
		versionEntry == metadata.end() ||
		!SafeIdentifier(metadata["export_name"], 128))
	{
		error = ".wums.meta is missing export_name or WUMS ABI version";
		return std::nullopt;
	}
	const auto abiVersion = ParseVersion(versionEntry->second);
	if (!abiVersion || abiVersion->major != 0 ||
		abiVersion->minor != 3 || abiVersion->patch < 1 ||
		abiVersion->patch > 6)
	{
		error = fmt::format(
			"module '{}' uses unsupported WUMS ABI {}; supported versions are 0.3.1 through 0.3.6",
			metadata["export_name"], versionEntry->second);
		return std::nullopt;
	}
	inspection.metadata.moduleName = metadata["export_name"];
	inspection.metadata.abiVersion = *abiVersion;
	auto take = [&](std::string_view key, std::string& destination) {
		if (const auto found = metadata.find(std::string(key));
			found != metadata.end())
			destination = found->second;
	};
	take("version", inspection.metadata.version);
	take("author", inspection.metadata.author);
	take("license", inspection.metadata.license);
	take("description", inspection.metadata.description);
	take("buildtimestamp", inspection.metadata.buildTimestamp);
	for (const auto* text : {
			 &inspection.metadata.version, &inspection.metadata.author,
			 &inspection.metadata.license, &inspection.metadata.description,
			 &inspection.metadata.buildTimestamp})
		if (!text->empty() && !SafeText(*text, 4096))
		{
			error = ".wums.meta contains invalid text";
			return std::nullopt;
		}
	auto flag = [&](std::string_view key, bool& destination) {
		const auto found = metadata.find(std::string(key));
		if (found == metadata.end())
			return true;
		if (found->second != "true" && found->second != "false")
			return false;
		destination = found->second == "true";
		return true;
	};
	if (!flag("skipInitFini", inspection.metadata.skipInitFini) ||
		!flag("initBeforeRelocationDoneHook",
			  inspection.metadata.initBeforeRelocationsDone))
	{
		error = ".wums.meta contains an invalid boolean flag";
		return std::nullopt;
	}
	static const std::set<std::string> knownMetadata{
		"export_name", "wums", "wum", "version", "author", "license",
		"description", "buildtimestamp", "skipInitFini",
		"initBeforeRelocationDoneHook"};
	for (const auto& [key, value] : metadata)
		if (!knownMetadata.contains(key))
			inspection.metadata.unknown.emplace(key, value);

	const auto dependencySection = std::ranges::find_if(
		sections, [](const Section& section) {
			return section.name == ".wums.dependencies";
		});
	std::set<std::string> dependencyNames;
	if (dependencySection != sections.end())
	{
		if (dependencySection->type != kShtProgbits ||
			(dependencySection->flags & kShfAlloc) == 0 ||
			dependencySection->data.empty() ||
			dependencySection->data.back() != std::byte{})
		{
			error = ".wums.dependencies is invalid";
			return std::nullopt;
		}
		for (std::size_t offset = 0;
			 offset < dependencySection->data.size();)
		{
			const auto raw = StringAt(dependencySection->data,
									  static_cast<std::uint32_t>(offset), 256);
			if (!raw)
			{
				error = ".wums.dependencies contains an unterminated entry";
				return std::nullopt;
			}
			offset += raw->size() + 1;
			if (raw->empty())
				continue;
			auto dependency = ParseDependency(*raw, error);
			if (!dependency ||
				!dependencyNames.insert(dependency->moduleName).second ||
				dependency->moduleName == inspection.metadata.moduleName)
			{
				if (error.empty())
					error = fmt::format(
						"module '{}' has a duplicate or self dependency '{}'",
						inspection.metadata.moduleName, *raw);
				return std::nullopt;
			}
			inspection.dependencies.push_back(std::move(*dependency));
		}
	}

	const auto hooksSection = std::ranges::find_if(
		sections, [](const Section& section) {
			return section.name == ".wums.hooks";
		});
	if (hooksSection == sections.end() ||
		hooksSection->type != kShtProgbits ||
		(hooksSection->flags & kShfAlloc) == 0 ||
		hooksSection->data.empty() || hooksSection->data.size() % 8 != 0 ||
		hooksSection->data.size() / 8 > 64)
	{
		error = "WMS image is missing a valid .wums.hooks section";
		return std::nullopt;
	}
	std::set<std::uint32_t> hookTypes;
	for (std::size_t offset = 0;
		 offset < hooksSection->data.size(); offset += 8)
	{
		const auto type = U32(hooksSection->data, offset);
		const auto target = U32(hooksSection->data, offset + 4);
		if (type > static_cast<std::uint32_t>(
					   WumsHookType::InitReentFunctions) ||
			!hookTypes.insert(type).second || (target & 3U) != 0 ||
			!GuestRange(sections, target, 4,
						kShfAlloc | kShfExecute))
		{
			error = ".wums.hooks contains an invalid descriptor";
			return std::nullopt;
		}
		if ((type == static_cast<std::uint32_t>(WumsHookType::Deinit) &&
			 *abiVersion < WupsVersion{0, 3, 2}) ||
			(type >= static_cast<std::uint32_t>(
						 WumsHookType::AllApplicationStartsDone) &&
			 type <= static_cast<std::uint32_t>(
						 WumsHookType::AllApplicationRequestsExitDone) &&
			 *abiVersion < WupsVersion{0, 3, 3}) ||
			(type >= static_cast<std::uint32_t>(
						 WumsHookType::GetCustomRplAllocator) &&
			 type <= static_cast<std::uint32_t>(
						 WumsHookType::ClearAllocatedRplMemory) &&
			 *abiVersion < WupsVersion{0, 3, 4}) ||
			(type == static_cast<std::uint32_t>(
						 WumsHookType::InitWutThread) &&
			 *abiVersion < WupsVersion{0, 3, 5}) ||
			(type == static_cast<std::uint32_t>(
						 WumsHookType::InitReentFunctions) &&
			 *abiVersion < WupsVersion{0, 3, 6}))
		{
			error = ".wums.hooks uses a hook newer than the declared WUMS ABI";
			return std::nullopt;
		}
		inspection.hooks.push_back({static_cast<WumsHookType>(type), target});
	}

	const auto exportsSection = std::ranges::find_if(
		sections, [](const Section& section) {
			return section.name == ".wums.exports";
		});
	if (exportsSection != sections.end())
	{
		if (exportsSection->type != kShtProgbits ||
			(exportsSection->flags & (kShfAlloc | kShfWrite)) !=
				(kShfAlloc | kShfWrite) ||
			exportsSection->data.size() % 12 != 0 ||
			exportsSection->data.size() / 12 > kMaximumDescriptors)
		{
			error = ".wums.exports has an invalid descriptor layout";
			return std::nullopt;
		}
		std::set<std::pair<std::string, WupsSymbolKind>> exportNames;
		for (std::size_t offset = 0;
			 offset < exportsSection->data.size(); offset += 12)
		{
			const auto rawKind = U32(exportsSection->data, offset);
			const auto name =
				GuestString(sections, U32(exportsSection->data, offset + 4));
			const auto address = U32(exportsSection->data, offset + 8);
			if (rawKind > 1 || !name || !SafeIdentifier(*name, 1024))
			{
				error = ".wums.exports contains an invalid type or name";
				return std::nullopt;
			}
			const auto kind = rawKind == 0 ? WupsSymbolKind::Function : WupsSymbolKind::Data;
			const auto requiredFlags = kind == WupsSymbolKind::Function ? kShfAlloc | kShfExecute : kShfAlloc;
			if (!exportNames.emplace(*name, kind).second ||
				(kind == WupsSymbolKind::Function &&
				 (address & 3U) != 0) ||
				!GuestRange(sections, address,
							kind == WupsSymbolKind::Function ? 4 : 1,
							requiredFlags))
			{
				error = ".wums.exports contains a duplicate or out-of-range export";
				return std::nullopt;
			}
			inspection.exports.push_back({inspection.metadata.moduleName, *name, kind, address, true});
		}
	}
	return inspection;
}
