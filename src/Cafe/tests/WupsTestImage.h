#pragma once

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

inline std::string DefaultWupsTestMetadata()
{
	constexpr char value[] =
		"name=Test Plugin\0author=Test Author\0version=1.0\0license=MIT\0"
		"description=Parser fixture\0wups=0.9.1\0buildtimestamp=Jul 23 2026\0storage_id=test.plugin\0";
	return {value, sizeof(value) - 1};
}

struct WupsTestImageOptions
{
	std::string metadata{DefaultWupsTestMetadata()};
	std::string dependencies;
	std::uint32_t hookType{17};
	std::uint32_t processTarget{16};
	std::uint32_t relocationType{1};
	bool includeLoad{true};
	bool includeImport{true};
	bool includeRelocation{true};
	bool wrongImportKind{};
	bool compressText{};
	bool tls{};
	bool wums{};
};

inline void WupsTestBe16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value)
{
	bytes[offset] = static_cast<std::byte>(value >> 8);
	bytes[offset + 1] = static_cast<std::byte>(value);
}

inline void WupsTestBe32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
	bytes[offset] = static_cast<std::byte>(value >> 24);
	bytes[offset + 1] = static_cast<std::byte>(value >> 16);
	bytes[offset + 2] = static_cast<std::byte>(value >> 8);
	bytes[offset + 3] = static_cast<std::byte>(value);
}

inline std::uint32_t WupsTestU32(const std::vector<std::byte>& bytes, std::size_t offset)
{
	return (std::to_integer<std::uint32_t>(bytes[offset]) << 24) |
		   (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16) |
		   (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8) |
		   std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

inline std::vector<std::byte> WupsTestBytes(std::string_view value)
{
	std::vector<std::byte> result(value.size());
	std::memcpy(result.data(), value.data(), value.size());
	return result;
}

inline std::vector<std::byte> BuildWupsTestImage(const WupsTestImageOptions& options = {})
{
	constexpr std::uint32_t progbits = 1;
	constexpr std::uint32_t symtab = 2;
	constexpr std::uint32_t strtab = 3;
	constexpr std::uint32_t rela = 4;
	constexpr std::uint32_t imports = 0x80000002U;
	constexpr std::uint32_t crcs = 0x80000003U;
	constexpr std::uint32_t fileinfo = 0x80000004U;
	constexpr std::uint32_t write = 1;
	constexpr std::uint32_t alloc = 2;
	constexpr std::uint32_t execute = 4;
	constexpr std::uint32_t tls = 0x400;
	constexpr std::uint32_t compressed = 0x08000000U;
	struct FixtureSection
	{
		std::string name;
		std::uint32_t type{};
		std::uint32_t flags{};
		std::uint32_t address{};
		std::uint32_t link{};
		std::uint32_t info{};
		std::uint32_t alignment{};
		std::uint32_t entrySize{};
		std::vector<std::byte> data;
		bool compress{};
	};
	std::vector<FixtureSection> sections;
	sections.push_back({});
	sections.push_back({".text", progbits, alloc | execute, 0x02000000, 0, 0, 4, 0,
						std::vector<std::byte>{std::byte{0x4e}, std::byte{0x80}, std::byte{0x00}, std::byte{0x20}}, options.compressText});
	std::vector<std::byte> data(64);
	const std::string_view functionName = "OSReport";
	const std::string_view replacementName = "my_OSReport";
	std::memcpy(data.data(), functionName.data(), functionName.size());
	std::memcpy(data.data() + 16, replacementName.data(), replacementName.size());
	sections.push_back({".data", progbits, alloc | write | (options.tls ? tls : 0), 0x10000000,
						0, 0, 4, 0, std::move(data)});
	sections.push_back({options.wums ? ".wums.meta" : ".wups.meta",
						progbits, alloc | write, 0x10000100,
						0, 0, 4, 0, WupsTestBytes(options.metadata)});
	std::vector<std::byte> hooks(8);
	WupsTestBe32(hooks, 0, options.hookType);
	WupsTestBe32(hooks, 4, 0x02000000);
	sections.push_back({options.wums ? ".wums.hooks" : ".wups.hooks",
						progbits, alloc | write, 0x10000200,
						0, 0, 4, 0, std::move(hooks)});
	if (options.includeLoad)
	{
		if (options.wums)
		{
			std::vector<std::byte> exports(12);
			WupsTestBe32(exports, 0, 0);
			WupsTestBe32(exports, 4, 0x10000000);
			WupsTestBe32(exports, 8, 0x02000000);
			sections.push_back({".wums.exports", progbits, alloc | write,
								0x10000300, 0, 0, 4, 0, std::move(exports)});
		}
		else
		{
			std::vector<std::byte> load(36);
			WupsTestBe32(load, 0, 1);
			WupsTestBe32(load, 12, 0x10000000);
			WupsTestBe32(load, 16, 2);
			WupsTestBe32(load, 20, 0x10000010);
			WupsTestBe32(load, 24, 0x02000000);
			WupsTestBe32(load, 28, 0x10000030);
			WupsTestBe32(load, 32, options.processTarget);
			sections.push_back({".wups.load", progbits, alloc | write,
								0x10000300, 0, 0, 4, 0, std::move(load)});
		}
	}
	if (options.wums && !options.dependencies.empty())
		sections.push_back({".wums.dependencies", progbits, alloc | write,
							0x10000400, 0, 0, 4, 0,
							WupsTestBytes(options.dependencies)});
	std::size_t importIndex{};
	if (options.includeImport)
	{
		importIndex = sections.size();
		sections.push_back({options.wrongImportKind ? ".dimport_coreinit" : ".fimport_coreinit",
							imports, alloc | (options.wrongImportKind ? 0 : execute), 0xc0000000,
							0, 0, 16, 0, std::vector<std::byte>(16)});
	}
	std::size_t relocationIndex{};
	if (options.includeRelocation)
	{
		relocationIndex = sections.size();
		sections.push_back({".rela.text", rela, 0, 0, 0, 1, 4, 12, std::vector<std::byte>(12)});
	}
	const auto symbolIndex = sections.size();
	std::vector<std::byte> symbols(options.includeImport ? 32 : 16);
	if (options.includeImport)
	{
		WupsTestBe32(symbols, 16, 1);
		WupsTestBe32(symbols, 20, 0xc0000000);
		symbols[28] = std::byte{0x12};
		WupsTestBe16(symbols, 30, static_cast<std::uint16_t>(importIndex));
	}
	sections.push_back({".symtab", symtab, 0, 0, 0, options.includeImport ? 1U : 0U, 4, 16, std::move(symbols)});
	const auto stringIndex = sections.size();
	sections.push_back({".strtab", strtab, 0, 0, 0, 0, 1, 0,
						WupsTestBytes(std::string_view{"\0OSReport\0", 10})});
	sections[symbolIndex].link = static_cast<std::uint32_t>(stringIndex);
	if (options.includeRelocation)
	{
		sections[relocationIndex].link = static_cast<std::uint32_t>(symbolIndex);
		WupsTestBe32(sections[relocationIndex].data, 0, 0x02000000);
		WupsTestBe32(sections[relocationIndex].data, 4,
					 (static_cast<std::uint32_t>(options.includeImport ? 1 : 0) << 8) | options.relocationType);
	}
	std::string names(1, '\0');
	std::vector<std::uint32_t> nameOffsets;
	for (const auto& section : sections)
	{
		nameOffsets.push_back(static_cast<std::uint32_t>(names.size()));
		names.append(section.name);
		names.push_back('\0');
	}
	const auto nameIndex = sections.size();
	nameOffsets.push_back(static_cast<std::uint32_t>(names.size()));
	names.append(".shstrtab\0", 10);
	sections.push_back({".shstrtab", strtab, 0, 0, 0, 0, 1, 0, WupsTestBytes(names)});
	const auto crcIndex = sections.size();
	nameOffsets.push_back(0);
	sections.push_back({{}, crcs, 0, 0, 0, 0, 4, 4, {}});
	const auto fileInfoIndex = sections.size();
	nameOffsets.push_back(0);
	std::vector<std::byte> fileInfoData(0x60);
	WupsTestBe32(fileInfoData, 0, 0xcafe0402);
	WupsTestBe32(fileInfoData, 4, 0x1000);
	WupsTestBe32(fileInfoData, 8, 0x20);
	WupsTestBe32(fileInfoData, 12, 0x1000);
	WupsTestBe32(fileInfoData, 16, 0x1000);
	WupsTestBe32(fileInfoData, 20, 0x1000);
	sections.push_back({{}, fileinfo, 0, 0, 0, 0, 4, 0, std::move(fileInfoData)});
	sections[crcIndex].data.resize(sections.size() * 4);

	const std::uint32_t sectionTableOffset = 0x40;
	const std::uint32_t sectionTableSize = static_cast<std::uint32_t>(sections.size() * 40);
	std::vector<std::vector<std::byte>> stored(sections.size());
	std::uint32_t cursor = (sectionTableOffset + sectionTableSize + 3) & ~3U;
	std::vector<std::uint32_t> fileOffsets(sections.size());
	for (std::size_t index = 1; index < sections.size(); ++index)
	{
		if (sections[index].compress)
		{
			uLongf bound = compressBound(sections[index].data.size());
			stored[index].resize(4 + bound);
			WupsTestBe32(stored[index], 0, sections[index].data.size());
			if (compress2(reinterpret_cast<Bytef*>(stored[index].data() + 4), &bound,
						  reinterpret_cast<const Bytef*>(sections[index].data.data()), sections[index].data.size(), 9) != Z_OK)
				std::abort();
			stored[index].resize(4 + bound);
			sections[index].flags |= compressed;
		}
		else
			stored[index] = sections[index].data;
		fileOffsets[index] = cursor;
		cursor = (cursor + static_cast<std::uint32_t>(stored[index].size()) + 3) & ~3U;
	}
	std::vector<std::byte> image(cursor);
	WupsTestBe32(image, 0, 0x7f454c46);
	image[4] = std::byte{1};
	image[5] = std::byte{2};
	image[6] = std::byte{1};
	image[7] = std::byte{0xca};
	image[8] = std::byte{0xfe};
	image[9] = options.wums ? std::byte{0xaf} : std::byte{'P'};
	image[10] = options.wums ? std::byte{0xfe} : std::byte{'L'};
	WupsTestBe16(image, 16, 0xfe01);
	WupsTestBe16(image, 18, 20);
	WupsTestBe32(image, 20, 1);
	WupsTestBe32(image, 24, 0x02000000);
	WupsTestBe32(image, 32, sectionTableOffset);
	WupsTestBe16(image, 40, 52);
	WupsTestBe16(image, 46, 40);
	WupsTestBe16(image, 48, sections.size());
	WupsTestBe16(image, 50, nameIndex);
	for (std::size_t index = 0; index < sections.size(); ++index)
	{
		const auto offset = sectionTableOffset + index * 40;
		WupsTestBe32(image, offset, nameOffsets[index]);
		WupsTestBe32(image, offset + 4, sections[index].type);
		WupsTestBe32(image, offset + 8, sections[index].flags);
		WupsTestBe32(image, offset + 12, sections[index].address);
		WupsTestBe32(image, offset + 16, fileOffsets[index]);
		WupsTestBe32(image, offset + 20, stored[index].size());
		WupsTestBe32(image, offset + 24, sections[index].link);
		WupsTestBe32(image, offset + 28, sections[index].info);
		WupsTestBe32(image, offset + 32, sections[index].alignment);
		WupsTestBe32(image, offset + 36, sections[index].entrySize);
		if (!stored[index].empty())
			std::memcpy(image.data() + fileOffsets[index], stored[index].data(), stored[index].size());
	}
	for (std::size_t index = 0; index < sections.size(); ++index)
	{
		if (index == crcIndex)
			continue;
		const auto checksum = static_cast<std::uint32_t>(crc32(0,
															   reinterpret_cast<const Bytef*>(sections[index].data.data()), sections[index].data.size()));
		WupsTestBe32(sections[crcIndex].data, index * 4, checksum);
		WupsTestBe32(image, fileOffsets[crcIndex] + index * 4, checksum);
	}
	return image;
}
