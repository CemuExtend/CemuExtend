#include "Common/precompiled.h"

#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// These are deliberately the production loader primitives.  This test-only
// oracle observes link state only; it never starts a C++ CPU and makes no CPU
// execution-parity claim.  Production fatal/assert paths cannot be converted
// into recoverable errors, so exact bounded fixture validation precedes them.
bool RPLLoader_ProcessHeaders(std::string_view moduleName, uint8* rplData,
							  uint32 rplSize, RPLModule** rplLoaderContextOut);
bool RPLLoader_LoadSections(sint32 procId, RPLModule* rplLoaderContext);
void RPLLoader_UpdateEntrypoint(RPLModule* rpl);
void RPLLoader_LinkSingleModule(RPLModule* rplLoaderContext, bool resolveOnlyExports);
void RPLLoader_DiscardPartiallyLoadedModule(RPLModule* rpl);

// Adapter-local registry projection: the ordinary production import scan reads
// these identities.  No production source is changed for this oracle.
extern RPLModule* rplModuleList[256];
extern sint32 rplModuleCount;

static_assert(sizeof(std::uint32_t) == 4);
static_assert(sizeof(rplRelocNew_t) == 0x0c);
static_assert(offsetof(rplSectionEntryNew_t, relocTargetSectionIndex) == 0x1c);
static_assert(offsetof(RPLFileInfoData, dataRegionSize) == 0x0c);

namespace
{
	constexpr std::size_t kMaximumFixtureBytes = 64U * 1024U * 1024U;
	constexpr std::size_t kReadChunkBytes = 8U * 1024U;
	constexpr std::size_t kPageBytes = 4096;
	constexpr std::size_t kSectionTableOffset = 0x40;
	constexpr std::size_t kSectionHeaderBytes = 0x28;
	constexpr std::size_t kFileInfoBytes = 0x60;
	constexpr std::size_t kMainImageBytes = 0x304;
	constexpr std::size_t kProviderImageBytes = 0x2cc;
	constexpr std::uint16_t kMainSectionCount = 10;
	constexpr std::uint16_t kProviderSectionCount = 9;

	constexpr std::uint32_t kExpectedMainTextBase = 0x02000000;
	constexpr std::uint32_t kExpectedProviderTextBase = 0x02002000;
	constexpr std::uint32_t kExpectedMainDataBase = 0x10000000;
	constexpr std::uint32_t kExpectedMainLoaderBase = 0x10001000;
	constexpr std::uint32_t kExpectedProviderLoaderBase = 0x10002000;
	constexpr std::uint8_t kPermissionRead = 1;
	constexpr std::uint8_t kPermissionWrite = 2;
	constexpr std::uint8_t kPermissionExecute = 4;

	constexpr std::size_t kMainTextSection = 1;
	constexpr std::size_t kMainDataSection = 2;
	constexpr std::size_t kMainImportSection = 3;
	constexpr std::size_t kMainSymtabSection = 4;
	constexpr std::size_t kMainStrtabSection = 5;
	constexpr std::size_t kMainRelaSection = 6;
	constexpr std::size_t kMainNameSection = 7;
	constexpr std::size_t kMainCrcSection = 8;
	constexpr std::size_t kMainFileInfoSection = 9;
	constexpr std::size_t kProviderTextSection = 1;
	constexpr std::size_t kProviderExportSection = 2;
	constexpr std::size_t kProviderSymtabSection = 3;
	constexpr std::size_t kProviderStrtabSection = 4;
	constexpr std::size_t kProviderRelaSection = 5;
	constexpr std::size_t kProviderNameSection = 6;
	constexpr std::size_t kProviderCrcSection = 7;
	constexpr std::size_t kProviderFileInfoSection = 8;

	constexpr std::string_view kUsageError =
		"cpp_rpl_call_oracle_trace: invalid arguments\n";
	constexpr std::string_view kInputError =
		"cpp_rpl_call_oracle_trace: failed to read fixtures\n";
	constexpr std::string_view kContractError =
		"cpp_rpl_call_oracle_trace: fixtures do not satisfy call contract\n";
	constexpr std::string_view kHarnessError =
		"cpp_rpl_call_oracle_trace: failed to produce canonical trace\n";

	struct SectionSpec
	{
		std::uint32_t name{};
		std::uint32_t type{};
		std::uint32_t flags{};
		std::uint32_t address{};
		std::uint32_t offset{};
		std::uint32_t size{};
		std::uint32_t link{};
		std::uint32_t info{};
		std::uint32_t alignment{};
		std::uint32_t entrySize{};
	};

	struct ModuleContract
	{
		std::uint16_t importCount{};
		std::uint16_t exportCount{};
		std::uint16_t relocationCount{};
	};

	constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

	void WriteBe16(std::vector<uint8>& bytes, std::size_t offset, std::uint16_t value)
	{
		bytes[offset] = static_cast<uint8>(value >> 8);
		bytes[offset + 1] = static_cast<uint8>(value);
	}

	void WriteBe32(std::vector<uint8>& bytes, std::size_t offset, std::uint32_t value)
	{
		bytes[offset] = static_cast<uint8>(value >> 24);
		bytes[offset + 1] = static_cast<uint8>(value >> 16);
		bytes[offset + 2] = static_cast<uint8>(value >> 8);
		bytes[offset + 3] = static_cast<uint8>(value);
	}

	std::uint16_t ReadBe16(std::span<const uint8> bytes, std::size_t offset)
	{
		return static_cast<std::uint16_t>((std::uint16_t{bytes[offset]} << 8) |
										  std::uint16_t{bytes[offset + 1]});
	}

	std::uint32_t ReadBe32(std::span<const uint8> bytes, std::size_t offset)
	{
		return (std::uint32_t{bytes[offset]} << 24) |
			   (std::uint32_t{bytes[offset + 1]} << 16) |
			   (std::uint32_t{bytes[offset + 2]} << 8) |
			   std::uint32_t{bytes[offset + 3]};
	}

	void AppendBe32(std::vector<uint8>& bytes, std::uint32_t value)
	{
		bytes.push_back(static_cast<uint8>(value >> 24));
		bytes.push_back(static_cast<uint8>(value >> 16));
		bytes.push_back(static_cast<uint8>(value >> 8));
		bytes.push_back(static_cast<uint8>(value));
	}

	std::uint32_t Crc32(std::span<const uint8> bytes)
	{
		std::uint32_t crc = 0xffffffffU;
		for (const uint8 byte : bytes)
		{
			crc ^= byte;
			for (unsigned int bit = 0; bit < 8; ++bit)
				crc = (crc & 1U) == 0 ? crc >> 1 : (crc >> 1) ^ 0xedb88320U;
		}
		return ~crc;
	}

	void WriteElfHeader(std::vector<uint8>& image, std::uint16_t sectionCount,
						std::uint16_t nameSection, std::uint32_t entrypoint)
	{
		constexpr std::array<uint8, 9> identity{
			0x7f, 'E', 'L', 'F', 1, 2, 1, 0xca, 0xfe};
		std::copy(identity.begin(), identity.end(), image.begin());
		WriteBe16(image, 16, 0xfe01);
		WriteBe16(image, 18, 20);
		WriteBe32(image, 20, 1);
		WriteBe32(image, 24, entrypoint);
		WriteBe32(image, 32, static_cast<std::uint32_t>(kSectionTableOffset));
		WriteBe16(image, 40, 0x34);
		WriteBe16(image, 46, static_cast<std::uint16_t>(kSectionHeaderBytes));
		WriteBe16(image, 48, sectionCount);
		WriteBe16(image, 50, nameSection);
	}

	template <std::size_t Count>
	void WriteSections(std::vector<uint8>& image,
					   const std::array<SectionSpec, Count>& sections)
	{
		for (std::size_t index = 0; index < sections.size(); ++index)
		{
			const SectionSpec& section = sections[index];
			const std::size_t offset = kSectionTableOffset + index * kSectionHeaderBytes;
			WriteBe32(image, offset, section.name);
			WriteBe32(image, offset + 4, section.type);
			WriteBe32(image, offset + 8, section.flags);
			WriteBe32(image, offset + 12, section.address);
			WriteBe32(image, offset + 16, section.offset);
			WriteBe32(image, offset + 20, section.size);
			WriteBe32(image, offset + 24, section.link);
			WriteBe32(image, offset + 28, section.info);
			WriteBe32(image, offset + 32, section.alignment);
			WriteBe32(image, offset + 36, section.entrySize);
		}
	}

	void WriteSymbol(std::vector<uint8>& image, std::size_t offset,
				 std::uint32_t value, std::uint16_t sectionIndex)
	{
		WriteBe32(image, offset, 1);
		WriteBe32(image, offset + 4, value);
		image[offset + 12] = 0x12;
		WriteBe16(image, offset + 14, sectionIndex);
	}

	void WriteRelocation(std::vector<uint8>& image, std::size_t offset,
					 std::uint32_t target, std::uint32_t kind)
	{
		WriteBe32(image, offset, target);
		WriteBe32(image, offset + 4, (1U << 8) | kind);
		WriteBe32(image, offset + 8, 0);
	}

	void WriteFileInfo(std::vector<uint8>& image, std::size_t offset,
				   std::uint32_t flags, std::uint32_t dataRegionSize)
	{
		WriteBe32(image, offset, 0xcafe0402);
		WriteBe32(image, offset + 4, 0x1000);
		WriteBe32(image, offset + 8, 0x1000);
		WriteBe32(image, offset + 12, dataRegionSize);
		WriteBe32(image, offset + 16, 0x1000);
		WriteBe32(image, offset + 20, 0x1000);
		WriteBe32(image, offset + 0x34, flags);
		WriteBe16(image, offset + 0x58, 0xffff);
	}

	template <std::size_t Count>
	void WriteCrcTable(std::vector<uint8>& image,
					   const std::array<SectionSpec, Count>& sections,
					   std::size_t crcOffset)
	{
		for (std::size_t index = 0; index < sections.size(); ++index)
		{
			const SectionSpec& section = sections[index];
			const std::uint32_t crc =
				(section.type == 0 || section.type == SHT_RPL_CRCS)
					? 0
					: Crc32(std::span<const uint8>{image}.subspan(section.offset,
																 section.size));
			WriteBe32(image, crcOffset + index * sizeof(std::uint32_t), crc);
		}
	}

	std::vector<uint8> ExpectedMainImage()
	{
		constexpr char sectionNameBytes[] =
			"\0.text\0.data\0.fimport_linkmod\0.symtab\0.strtab\0.rela.text\0.shstrtab\0.crcs\0.fileinfo\0";
		constexpr std::size_t sectionNameSize = sizeof(sectionNameBytes) - 1;
		constexpr std::size_t text = kSectionTableOffset +
			kMainSectionCount * kSectionHeaderBytes;
		constexpr std::size_t data = text + 12;
		constexpr std::size_t imports = data + 4;
		constexpr std::size_t symbols = imports + 20;
		constexpr std::size_t strings = symbols + 32;
		constexpr std::size_t rela = AlignUp(strings + 8, 4);
		constexpr std::size_t names = rela + 12;
		constexpr std::size_t crc = AlignUp(names + sectionNameSize, 4);
		constexpr std::size_t fileInfo = crc + kMainSectionCount * 4;
		static_assert(fileInfo + kFileInfoBytes == kMainImageBytes);

		const std::array<SectionSpec, kMainSectionCount> sections{{
			{},
			{1, SHT_PROGBITS, 6, 0x02000000, text, 12, 0, 0, 4, 0},
			{7, SHT_PROGBITS, 3, 0x10000000, data, 4, 0, 0, 4, 0},
			{13, SHT_RPL_IMPORTS, 6, 0xc0000000, imports, 20, 0, 0, 4, 0},
			{30, SHT_SYMTAB, 0, 0, symbols, 32, 5, 1, 4, 16},
			{38, SHT_STRTAB, 0, 0, strings, 8, 0, 0, 1, 0},
			{46, SHT_RELA, 0, 0, rela, 12, 4, 1, 4, 12},
			{57, SHT_STRTAB, 0, 0, names, sectionNameSize, 0, 0, 1, 0},
			{67, SHT_RPL_CRCS, 0, 0, crc, kMainSectionCount * 4, 0, 0, 4, 4},
			{73, SHT_RPL_FILEINFO, 0, 0, fileInfo, kFileInfoBytes, 0, 0, 4, 0},
		}};
		std::vector<uint8> image(kMainImageBytes, 0);
		WriteElfHeader(image, kMainSectionCount, 7, 0x02000000);
		WriteSections(image, sections);
		constexpr std::array<uint8, 12> textBytes{
			0x48, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
		constexpr std::array<uint8, 20> importBytes{
			0, 0, 0, 0, 0, 0, 0, 0, 'l', 'i', 'n', 'k', 'm', 'o', 'd', '.', 'r', 'p', 'l', 0};
		constexpr std::array<uint8, 8> symbolNames{0, 'a', 'n', 's', 'w', 'e', 'r', 0};
		std::copy(textBytes.begin(), textBytes.end(), image.begin() + text);
		std::copy(importBytes.begin(), importBytes.end(), image.begin() + imports);
		WriteSymbol(image, symbols + 16, 0xc0000008, 3);
		std::copy(symbolNames.begin(), symbolNames.end(), image.begin() + strings);
		WriteRelocation(image, rela, 0x02000000, RPL_RELOC_REL24);
		std::copy_n(reinterpret_cast<const uint8*>(sectionNameBytes), sectionNameSize,
					image.begin() + names);
		WriteFileInfo(image, fileInfo, 2, 0x1000);
		WriteCrcTable(image, sections, crc);
		return image;
	}

	std::vector<uint8> ExpectedProviderImage()
	{
		constexpr char sectionNameBytes[] =
			"\0.text\0.fexports\0.symtab\0.strtab\0.rela.fexports\0.shstrtab\0.crcs\0.fileinfo\0";
		constexpr std::size_t sectionNameSize = sizeof(sectionNameBytes) - 1;
		constexpr std::size_t text = kSectionTableOffset +
			kProviderSectionCount * kSectionHeaderBytes;
		constexpr std::size_t exports = text + 8;
		constexpr std::size_t symbols = AlignUp(exports + 23, 4);
		constexpr std::size_t strings = symbols + 32;
		constexpr std::size_t rela = AlignUp(strings + 8, 4);
		constexpr std::size_t names = rela + 12;
		constexpr std::size_t crc = AlignUp(names + sectionNameSize, 4);
		constexpr std::size_t fileInfo = crc + kProviderSectionCount * 4;
		static_assert(fileInfo + kFileInfoBytes == kProviderImageBytes);

		const std::array<SectionSpec, kProviderSectionCount> sections{{
			{},
			{1, SHT_PROGBITS, 6, 0x02000000, text, 8, 0, 0, 4, 0},
			{7, SHT_RPL_EXPORTS, 6, 0xc0000000, exports, 23, 0, 0, 4, 0},
			{17, SHT_SYMTAB, 0, 0, symbols, 32, 4, 1, 4, 16},
			{25, SHT_STRTAB, 0, 0, strings, 8, 0, 0, 1, 0},
			{33, SHT_RELA, 0, 0, rela, 12, 3, 2, 4, 12},
			{49, SHT_STRTAB, 0, 0, names, sectionNameSize, 0, 0, 1, 0},
			{59, SHT_RPL_CRCS, 0, 0, crc, kProviderSectionCount * 4, 0, 0, 4, 4},
			{65, SHT_RPL_FILEINFO, 0, 0, fileInfo, kFileInfoBytes, 0, 0, 4, 0},
		}};
		std::vector<uint8> image(kProviderImageBytes, 0);
		WriteElfHeader(image, kProviderSectionCount, 6, 0);
		WriteSections(image, sections);
		constexpr std::array<uint8, 8> textBytes{
			0x38, 0x60, 0x00, 0x2a, 0x4e, 0x80, 0x00, 0x20};
		constexpr std::array<uint8, 8> symbolNames{0, 'a', 'n', 's', 'w', 'e', 'r', 0};
		std::copy(textBytes.begin(), textBytes.end(), image.begin() + text);
		WriteBe32(image, exports, 1);
		WriteBe32(image, exports + 12, 16);
		std::copy_n(reinterpret_cast<const uint8*>("answer\0"), 7,
					image.begin() + exports + 16);
		WriteSymbol(image, symbols + 16, 0x02000000, 1);
		std::copy(symbolNames.begin(), symbolNames.end(), image.begin() + strings);
		WriteRelocation(image, rela, 0xc0000008, RPL_RELOC_ADDR32);
		std::copy_n(reinterpret_cast<const uint8*>(sectionNameBytes), sectionNameSize,
					image.begin() + names);
		WriteFileInfo(image, fileInfo, 0, 0);
		WriteCrcTable(image, sections, crc);
		return image;
	}

	bool ValidateFixtures(std::span<const uint8> mainImage,
					  std::span<const uint8> providerImage,
					  ModuleContract& mainContract,
					  ModuleContract& providerContract)
	{
		// Full-image equality makes every header byte, section descriptor, payload,
		// padding byte, CRC, FILEINFO field, symbol, import/export descriptor, and
		// relocation part of the preflight contract.
		const std::vector<uint8> expectedMain = ExpectedMainImage();
		const std::vector<uint8> expectedProvider = ExpectedProviderImage();
		if (mainImage.size() != expectedMain.size() ||
			providerImage.size() != expectedProvider.size() ||
			!std::equal(mainImage.begin(), mainImage.end(), expectedMain.begin()) ||
			!std::equal(providerImage.begin(), providerImage.end(), expectedProvider.begin()))
			return false;
		mainContract = {1, 0, 1};
		providerContract = {0, 1, 1};
		return true;
	}

	bool ReadFixture(const char* path, std::vector<uint8>& image)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
			return false;
		std::array<char, kReadChunkBytes> chunk{};
		for (;;)
		{
			const std::size_t remaining = kMaximumFixtureBytes - image.size();
			const std::size_t request = std::min(chunk.size(), remaining + 1);
			input.read(chunk.data(), static_cast<std::streamsize>(request));
			const std::streamsize count = input.gcount();
			if (count < 0 || static_cast<std::size_t>(count) > remaining)
				return false;
			image.insert(image.end(), reinterpret_cast<const uint8*>(chunk.data()),
						 reinterpret_cast<const uint8*>(chunk.data()) + count);
			if (input.bad())
				return false;
			if (input.eof())
				return true;
			if (input.fail() || count == 0)
				return false;
		}
	}

	std::string Hex32(std::uint32_t value)
	{
		constexpr char digits[] = "0123456789abcdef";
		std::string result(10, '0');
		result[1] = 'x';
		for (unsigned int index = 0; index < 8; ++index)
			result[2 + index] = digits[(value >> (28 - index * 4)) & 0xf];
		return result;
	}

	template <typename Integer>
	std::string Decimal(Integer value)
	{
		std::array<char, 32> buffer{};
		const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
		if (result.ec != std::errc{})
			throw std::runtime_error("decimal conversion failed");
		return std::string(buffer.data(), result.ptr);
	}

	std::string Sha256(std::span<const uint8> bytes)
	{
		std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
		if (!SHA256(bytes.data(), bytes.size(), digest.data()))
			throw std::runtime_error("SHA-256 failed");
		constexpr char digits[] = "0123456789abcdef";
		std::string result(SHA256_DIGEST_LENGTH * 2, '0');
		for (std::size_t index = 0; index < digest.size(); ++index)
		{
			result[index * 2] = digits[digest[index] >> 4];
			result[index * 2 + 1] = digits[digest[index] & 0xf];
		}
		return result;
	}

	std::uint32_t SectionField(std::span<const uint8> image, std::size_t index,
						   std::size_t fieldOffset)
	{
		return ReadBe32(image, kSectionTableOffset + index * kSectionHeaderBytes +
								 fieldOffset);
	}

	bool IsGuestRangeAccessible(std::uint32_t address, std::uint64_t size)
	{
		return size != 0 && size <= std::numeric_limits<std::uint32_t>::max() &&
			   std::uint64_t{address} + size <=
				   std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1 &&
			   memory_isAddressRangeAccessible(address, static_cast<std::uint32_t>(size));
	}

	bool IsHostSubrange(const void* allocation, std::size_t allocationSize,
					const void* pointer, std::size_t size)
	{
		if (!allocation || !pointer || size == 0 || size > allocationSize)
			return false;
		const std::uintptr_t allocationAddress =
			reinterpret_cast<std::uintptr_t>(allocation);
		const std::uintptr_t pointerAddress = reinterpret_cast<std::uintptr_t>(pointer);
		if (pointerAddress < allocationAddress)
			return false;
		const std::uintptr_t offset = pointerAddress - allocationAddress;
		return offset <= allocationSize && size <= allocationSize - offset;
	}

	std::int32_t Signed32(std::uint32_t raw)
	{
		if (raw <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
			return static_cast<std::int32_t>(raw);
		return -1 - static_cast<std::int32_t>(~raw);
	}

	bool ValidateModuleCommon(const RPLModule* module,
						  std::span<const uint8> image,
						  std::uint16_t expectedSectionCount,
						  std::string_view expectedName,
						  std::size_t crcIndex, std::size_t fileInfoIndex)
	{
		const std::size_t sectionTableSize =
			static_cast<std::size_t>(expectedSectionCount) * kSectionHeaderBytes;
		if (crcIndex >= expectedSectionCount || fileInfoIndex >= expectedSectionCount ||
			image.size() < kSectionTableOffset + sectionTableSize || !module ||
			module->rplHeader.sectionTableEntryCount != expectedSectionCount ||
			module->sectionTablePtr == nullptr ||
			module->sectionAddressTable2.size() != expectedSectionCount ||
			module->debugSectionLoadMask.size() != expectedSectionCount ||
			module->crcTable.size() != expectedSectionCount ||
			module->sectionData_fileInfo.size() != kFileInfoBytes ||
			!module->sectionData_crc.empty() || !module->ownedRPLRawData.empty() ||
			module->RPLRawData.data() != image.data() ||
			module->RPLRawData.size() != image.size() || module->moduleName != expectedName ||
			module->hasError || module->entrypoint != 0 || module->isLinked ||
			module->entrypointCalled || module->externalModule ||
			module->externalCallEntrypoint || module->externalRegisterDependency ||
			module->externalUseApplicationAllocator || module->externalUnloading ||
			module->externalEventInFlight || module->externalAccessCount != 0 ||
			std::memcmp(&module->rplHeader, image.data(), sizeof(rplHeaderNew_t)) != 0)
			return false;

		if (std::memcmp(module->sectionTablePtr,
						image.data() + kSectionTableOffset, sectionTableSize) != 0 ||
			module->sectionAddressTable2[0].ptr != nullptr ||
			module->sectionAddressTable2[crcIndex].ptr != nullptr ||
			module->sectionAddressTable2[fileInfoIndex].ptr !=
				module->sectionData_fileInfo.data())
			return false;

		const std::uint32_t fileInfoOffset = SectionField(image, fileInfoIndex, 16);
		const std::uint32_t fileInfoSize = SectionField(image, fileInfoIndex, 20);
		const std::uint32_t crcOffset = SectionField(image, crcIndex, 16);
		const std::uint32_t crcSize = SectionField(image, crcIndex, 20);
		if (fileInfoSize != kFileInfoBytes ||
			std::uint64_t{fileInfoOffset} + fileInfoSize > image.size() ||
			crcSize != static_cast<std::uint32_t>(expectedSectionCount) * 4 ||
			std::uint64_t{crcOffset} + crcSize > image.size() ||
			std::memcmp(module->sectionData_fileInfo.data(),
						image.data() + fileInfoOffset, fileInfoSize) != 0)
			return false;
		for (std::size_t index = 0; index < expectedSectionCount; ++index)
			if (module->crcTable[index] != ReadBe32(image, crcOffset + index * 4))
				return false;
		return true;
	}

	bool ValidateGuestSection(const RPLModule& module,
						 std::span<const uint8> image, std::size_t index,
						 std::uint32_t expectedGuestAddress)
	{
		if (index >= module.sectionAddressTable2.size())
			return false;
		const std::uint32_t fileOffset = SectionField(image, index, 16);
		const std::uint32_t size = SectionField(image, index, 20);
		if (size == 0 || std::uint64_t{fileOffset} + size > image.size() ||
			!IsGuestRangeAccessible(expectedGuestAddress, size))
			return false;
		const uint8* expectedPointer =
			memory_getPointerFromVirtualOffset(expectedGuestAddress);
		const void* loadedPointer = module.sectionAddressTable2[index].ptr;
		return expectedPointer != nullptr && loadedPointer == expectedPointer &&
			   std::memcmp(loadedPointer, image.data() + fileOffset, size) == 0;
	}

	template <std::size_t Count>
	bool ValidateTempSections(const RPLModule& module,
						  std::span<const uint8> image,
						  const std::array<std::size_t, Count>& indices)
	{
		if (indices.empty() || !module.tempRegionPtr)
			return false;
		for (const std::size_t index : indices)
			if (index >= module.sectionAddressTable2.size())
				return false;
		const std::uint32_t tempBegin = SectionField(image, indices.front(), 16);
		const std::uint32_t lastOffset = SectionField(image, indices.back(), 16);
		const std::uint32_t lastSize = SectionField(image, indices.back(), 20);
		const std::uint64_t tempEnd = std::uint64_t{lastOffset} + lastSize;
		if (tempEnd < tempBegin || tempEnd > image.size())
			return false;
		const std::size_t tempSize = static_cast<std::size_t>(tempEnd - tempBegin);
		if (module.tempRegionAllocSize != AlignUp(tempSize, 32))
			return false;

		for (const std::size_t index : indices)
		{
			const std::uint32_t fileOffset = SectionField(image, index, 16);
			const std::uint32_t size = SectionField(image, index, 20);
			const std::uint64_t sectionEnd = std::uint64_t{fileOffset} + size;
			if (size == 0 || fileOffset < tempBegin || sectionEnd > tempEnd ||
				sectionEnd > image.size())
				return false;
			const std::size_t relativeOffset = fileOffset - tempBegin;
			const void* loadedPointer = module.sectionAddressTable2[index].ptr;
			const std::uintptr_t allocationAddress =
				reinterpret_cast<std::uintptr_t>(module.tempRegionPtr);
			if (relativeOffset >
					std::numeric_limits<std::uintptr_t>::max() - allocationAddress ||
				reinterpret_cast<std::uintptr_t>(loadedPointer) !=
					allocationAddress + relativeOffset ||
				!IsHostSubrange(module.tempRegionPtr, module.tempRegionAllocSize,
							loadedPointer, size) ||
				std::memcmp(loadedPointer, image.data() + fileOffset, size) != 0)
				return false;
		}
		return true;
	}

	struct LoadedRelocation
	{
		std::uint32_t site{};
		std::uint32_t kind{};
		std::int32_t addend{};
	};

	bool ValidateRelocation(const RPLModule& module,
						std::span<const uint8> image,
						std::size_t relaIndex, std::size_t expectedSymtabIndex,
						std::size_t expectedTargetIndex,
						std::uint32_t targetGuestAddress,
						std::uint32_t expectedKind, LoadedRelocation& loaded)
	{
		const std::size_t sectionCount = module.sectionAddressTable2.size();
		if (relaIndex >= sectionCount || expectedSymtabIndex >= sectionCount ||
			expectedTargetIndex >= sectionCount ||
			SectionField(image, relaIndex, 4) != SHT_RELA ||
			SectionField(image, relaIndex, 20) != sizeof(rplRelocNew_t) ||
			SectionField(image, relaIndex, 24) != expectedSymtabIndex ||
			SectionField(image, relaIndex, 28) != expectedTargetIndex ||
			SectionField(image, relaIndex, 36) != sizeof(rplRelocNew_t))
			return false;

		const void* relocationPointer = module.sectionAddressTable2[relaIndex].ptr;
		if (!IsHostSubrange(module.tempRegionPtr, module.tempRegionAllocSize,
							relocationPointer, sizeof(rplRelocNew_t)))
			return false;
		std::array<uint8, sizeof(rplRelocNew_t)> relocationBytes{};
		std::memcpy(relocationBytes.data(), relocationPointer, relocationBytes.size());
		const std::uint32_t relocationOffset = ReadBe32(relocationBytes, 0);
		const std::uint32_t symbolAndType = ReadBe32(relocationBytes, 4);
		const std::uint32_t symbolIndex = symbolAndType >> 8;
		const std::uint32_t kind = symbolAndType & 0xffU;

		const std::uint32_t targetVirtualAddress =
			SectionField(image, expectedTargetIndex, 12);
		const std::uint32_t targetSize = SectionField(image, expectedTargetIndex, 20);
		if (kind != expectedKind || symbolIndex != 1 || targetSize < 4 ||
			relocationOffset < targetVirtualAddress ||
			relocationOffset - targetVirtualAddress > targetSize - 4)
			return false;
		const std::uint32_t targetDelta = relocationOffset - targetVirtualAddress;
		const std::uint64_t siteWide = std::uint64_t{targetGuestAddress} + targetDelta;
		if (siteWide > std::numeric_limits<std::uint32_t>::max())
			return false;
		const std::uint32_t site = static_cast<std::uint32_t>(siteWide);
		if (!IsGuestRangeAccessible(targetGuestAddress, targetSize) ||
			!IsGuestRangeAccessible(site, 4) ||
			module.sectionAddressTable2[expectedTargetIndex].ptr !=
				memory_getPointerFromVirtualOffset(targetGuestAddress))
			return false;

		if (SectionField(image, expectedSymtabIndex, 4) != SHT_SYMTAB ||
			SectionField(image, expectedSymtabIndex, 20) != 32 ||
			SectionField(image, expectedSymtabIndex, 36) != 16)
			return false;
		const std::uint32_t strtabIndex = SectionField(image, expectedSymtabIndex, 24);
		if (strtabIndex >= sectionCount || SectionField(image, strtabIndex, 4) != SHT_STRTAB)
			return false;
		const auto* symtabPointer = static_cast<const uint8*>(
			module.sectionAddressTable2[expectedSymtabIndex].ptr);
		const std::uint32_t symtabSize = SectionField(image, expectedSymtabIndex, 20);
		const std::uint32_t symbolOffset = symbolIndex * 16;
		if (symbolOffset > symtabSize || 16 > symtabSize - symbolOffset ||
			!IsHostSubrange(module.tempRegionPtr, module.tempRegionAllocSize,
							symtabPointer, symtabSize))
			return false;
		const std::span<const uint8> symbol{symtabPointer + symbolOffset, 16};
		const std::uint32_t nameOffset = ReadBe32(symbol, 0);
		const std::uint16_t symbolSectionIndex = ReadBe16(symbol, 14);
		const auto* strtabPointer = static_cast<const uint8*>(
			module.sectionAddressTable2[strtabIndex].ptr);
		const std::uint32_t strtabSize = SectionField(image, strtabIndex, 20);
		if (symbolSectionIndex >= sectionCount || nameOffset >= strtabSize ||
			!IsHostSubrange(module.tempRegionPtr, module.tempRegionAllocSize,
							strtabPointer, strtabSize) ||
			std::memchr(strtabPointer + nameOffset, 0, strtabSize - nameOffset) == nullptr)
			return false;

		loaded = {site, kind, Signed32(ReadBe32(relocationBytes, 8))};
		return true;
	}

	struct LoadedLinkState
	{
		LoadedRelocation local;
		LoadedRelocation imported;
		std::uint32_t resolvedSymbol{};
	};

	bool ValidateLoadedSession(const RPLModule* main, const RPLModule* provider,
						   std::span<const uint8> mainImage,
						   std::span<const uint8> providerImage,
						   LoadedLinkState& loaded)
	{
		if (!ValidateModuleCommon(main, mainImage, kMainSectionCount, "main",
							  kMainCrcSection, kMainFileInfoSection) ||
			!ValidateModuleCommon(provider, providerImage, kProviderSectionCount,
							  "linkmod", kProviderCrcSection,
							  kProviderFileInfoSection))
			return false;
		for (std::size_t index = 0; index < kMainSectionCount; ++index)
		{
			const bool expectedLoaded = index == kMainTextSection ||
				index == kMainDataSection || index == kMainImportSection;
			if (main->debugSectionLoadMask[index] != expectedLoaded)
				return false;
		}
		for (std::size_t index = 0; index < kProviderSectionCount; ++index)
		{
			const bool expectedLoaded = index == kProviderTextSection ||
				index == kProviderExportSection;
			if (provider->debugSectionLoadMask[index] != expectedLoaded)
				return false;
		}

		if (main->regionMappingBase_text.GetMPTR() != kExpectedMainTextBase ||
			main->regionMappingBase_data != kExpectedMainDataBase ||
			main->regionMappingBase_loaderInfo != kExpectedMainLoaderBase ||
			main->regionSize_text != kPageBytes || main->regionSize_data != kPageBytes ||
			main->regionSize_loaderInfo != kPageBytes ||
			main->regionOrigAddr_text != 0x02000000 ||
			main->regionOrigAddr_data != 0x10000000 ||
			main->fileInfo.textRegionSize != kPageBytes ||
			main->fileInfo.dataRegionSize != kPageBytes ||
			main->fileInfo.baseAlign != kPageBytes || main->fileInfo.ukn14 != kPageBytes ||
			main->fileInfo.trampolineAdjustment != 0 || main->fileInfo.ukn4C != 0 ||
			main->fileInfo.sdataBase1 != 0 || main->fileInfo.sdataBase2 != 0 ||
			main->fileInfo.tlsModuleIndex != -1 ||
			main->fileInfo.flags != 2 || main->exportDCount != 0 ||
			main->exportFCount != 0 || main->exportDDataPtr != nullptr ||
			main->exportFDataPtr != nullptr ||
			!IsGuestRangeAccessible(kExpectedMainTextBase, kPageBytes) ||
			!IsGuestRangeAccessible(kExpectedMainDataBase, kPageBytes) ||
			!IsGuestRangeAccessible(kExpectedMainLoaderBase, kPageBytes))
			return false;

		if (provider->regionMappingBase_text.GetMPTR() != kExpectedProviderTextBase ||
			provider->regionMappingBase_data != kExpectedProviderLoaderBase ||
			provider->regionMappingBase_loaderInfo != kExpectedProviderLoaderBase ||
			provider->regionSize_text != kPageBytes || provider->regionSize_data != 0 ||
			provider->regionSize_loaderInfo != kPageBytes ||
			provider->regionOrigAddr_text != 0x02000000 ||
			provider->regionOrigAddr_data != 0 ||
			provider->fileInfo.textRegionSize != kPageBytes ||
			provider->fileInfo.dataRegionSize != 0 ||
			provider->fileInfo.baseAlign != kPageBytes ||
			provider->fileInfo.ukn14 != kPageBytes ||
			provider->fileInfo.trampolineAdjustment != 0 ||
			provider->fileInfo.ukn4C != 0 || provider->fileInfo.sdataBase1 != 0 ||
			provider->fileInfo.sdataBase2 != 0 ||
			provider->fileInfo.tlsModuleIndex != -1 || provider->fileInfo.flags != 0 ||
			provider->exportDCount != 0 || provider->exportDDataPtr != nullptr ||
			provider->exportFCount != 1 ||
			!IsGuestRangeAccessible(kExpectedProviderTextBase, kPageBytes) ||
			!IsGuestRangeAccessible(kExpectedProviderLoaderBase, kPageBytes))
			return false;

		constexpr std::array<std::size_t, 4> mainTempSections{
			kMainSymtabSection, kMainStrtabSection, kMainRelaSection, kMainNameSection};
		constexpr std::array<std::size_t, 4> providerTempSections{
			kProviderSymtabSection, kProviderStrtabSection,
			kProviderRelaSection, kProviderNameSection};
		if (!ValidateGuestSection(*main, mainImage, kMainTextSection,
							  kExpectedMainTextBase) ||
			!ValidateGuestSection(*main, mainImage, kMainDataSection,
							  kExpectedMainDataBase) ||
			!ValidateGuestSection(*main, mainImage, kMainImportSection,
							  kExpectedMainLoaderBase) ||
			!ValidateTempSections(*main, mainImage, mainTempSections) ||
			!ValidateGuestSection(*provider, providerImage, kProviderTextSection,
							  kExpectedProviderTextBase) ||
			!ValidateGuestSection(*provider, providerImage, kProviderExportSection,
							  kExpectedProviderLoaderBase) ||
			!ValidateTempSections(*provider, providerImage, providerTempSections) ||
			!ValidateRelocation(*provider, providerImage, kProviderRelaSection,
							 kProviderSymtabSection, kProviderExportSection,
							 provider->regionMappingBase_loaderInfo, RPL_RELOC_ADDR32,
							 loaded.local) ||
			!ValidateRelocation(*main, mainImage, kMainRelaSection,
							 kMainSymtabSection, kMainTextSection,
							 main->regionMappingBase_text.GetMPTR(), RPL_RELOC_REL24,
							 loaded.imported))
			return false;
		const uint8* providerExportPointer =
			memory_getPointerFromVirtualOffset(kExpectedProviderLoaderBase);
		if (!providerExportPointer ||
			static_cast<const void*>(provider->exportFDataPtr) !=
				static_cast<const void*>(providerExportPointer + 8))
			return false;
		loaded.resolvedSymbol = provider->regionMappingBase_text.GetMPTR();
		return true;
	}

	bool ReadGuestBe32(std::uint32_t address, std::uint32_t& value)
	{
		if (!IsGuestRangeAccessible(address, 4))
			return false;
		const uint8* pointer = memory_getPointerFromVirtualOffset(address);
		if (!pointer)
			return false;
		value = ReadBe32(std::span<const uint8>{pointer, 4}, 0);
		return true;
	}

	class TestRegistryProjection
	{
	  public:
		struct Removal
		{
			bool exact{};
			bool detached{};
		};

		bool Publish(RPLModule* main, RPLModule* provider)
		{
			if (!main || !provider || rplModuleCount != 0)
				return false;
			rplModuleList[0] = main;
			rplModuleList[1] = provider;
			rplModuleCount = 2;
			m_main = main;
			m_provider = provider;
			m_published = true;
			return true;
		}

		Removal Remove()
		{
			if (!m_published)
				return {rplModuleCount == 0, true};
			if (rplModuleCount < 0 || rplModuleCount > 256)
			{
				m_main = nullptr;
				m_provider = nullptr;
				m_published = false;
				return {};
			}

			const bool exact = rplModuleCount == 2 && rplModuleList[0] == m_main &&
							   rplModuleList[1] == m_provider;
			sint32 writeIndex = 0;
			unsigned int mainMatches = 0;
			unsigned int providerMatches = 0;
			const sint32 previousCount = rplModuleCount;
			for (sint32 readIndex = 0; readIndex < previousCount; ++readIndex)
			{
				RPLModule* candidate = rplModuleList[readIndex];
				if (candidate == m_main)
				{
					++mainMatches;
					continue;
				}
				if (candidate == m_provider)
				{
					++providerMatches;
					continue;
				}
				rplModuleList[writeIndex++] = candidate;
			}
			for (sint32 index = writeIndex; index < previousCount; ++index)
				rplModuleList[index] = nullptr;
			rplModuleCount = writeIndex;
			m_main = nullptr;
			m_provider = nullptr;
			m_published = false;
			return {exact, mainMatches != 0 && providerMatches != 0};
		}

	  private:
		RPLModule* m_main{};
		RPLModule* m_provider{};
		bool m_published{};
	};

	class LoaderSession
	{
	  public:
		bool Initialize()
		{
			memory_init();
			memory_mapForCurrentTitle();
			m_mapped = true;
			RPLLoader_InitState();
			return rplModuleCount == 0;
		}

		~LoaderSession()
		{
			if (!m_tornDown)
				(void)Teardown();
		}

		bool Teardown()
		{
			const auto removal = m_registry.Remove();
			bool clean = removal.exact && removal.detached;
			if (clean && m_loaderOutputValidated)
			{
				if (m_provider)
					RPLLoader_DiscardPartiallyLoadedModule(m_provider);
				if (m_main)
					RPLLoader_DiscardPartiallyLoadedModule(m_main);
				clean = RPLLoader_UnloadAll();
			}
			else
			{
				clean = false;
			}
			// On identity drift, leaking process-local objects is safer than
			// dereferencing an uncertain owner or destroying foreign registry state.
			// The same rule applies until the complete post-load validator succeeds.
			m_provider = nullptr;
			m_main = nullptr;
			if (m_mapped)
				memory_unmapForCurrentTitle();
			m_mapped = false;
			m_tornDown = true;
			return clean && rplModuleCount == 0;
		}

		RPLModule*& Main()
		{
			return m_main;
		}

		RPLModule*& Provider()
		{
			return m_provider;
		}

		TestRegistryProjection& Registry()
		{
			return m_registry;
		}

		void MarkLoaderOutputValidated()
		{
			m_loaderOutputValidated = true;
		}

	  private:
		bool m_mapped{};
		bool m_tornDown{};
		bool m_loaderOutputValidated{};
		RPLModule* m_main{};
		RPLModule* m_provider{};
		TestRegistryProjection m_registry;
	};

	struct LogicalPage
	{
		std::uint32_t address{};
		std::uint8_t permissions{};
		std::array<uint8, kPageBytes> bytes{};
	};

	struct MemoryProof
	{
		std::uint32_t pageCount{};
		std::uint64_t byteCount{};
		std::string digest;
	};

	bool AddPage(std::vector<LogicalPage>& pages, std::uint32_t address,
			 std::uint8_t permissions)
	{
		if ((address & (kPageBytes - 1)) != 0 ||
			!IsGuestRangeAccessible(address, kPageBytes))
			return false;
		const uint8* pointer = memory_getPointerFromVirtualOffset(address);
		if (!pointer)
			return false;
		LogicalPage page{address, permissions};
		std::copy_n(pointer, kPageBytes, page.bytes.begin());
		pages.push_back(std::move(page));
		return true;
	}

	bool BuildMemoryProof(MemoryProof& proof)
	{
		std::vector<LogicalPage> pages;
		pages.reserve(5);
		// Permissions are the guest-memory-v1 projection shared with Rust:
		// text=R|X, data=R|W, loader metadata=R.
		if (!AddPage(pages, kExpectedMainTextBase,
					 kPermissionRead | kPermissionExecute) ||
			!AddPage(pages, kExpectedProviderTextBase,
					 kPermissionRead | kPermissionExecute) ||
			!AddPage(pages, kExpectedMainDataBase,
					 kPermissionRead | kPermissionWrite) ||
			!AddPage(pages, kExpectedMainLoaderBase, kPermissionRead) ||
			!AddPage(pages, kExpectedProviderLoaderBase, kPermissionRead))
			return false;

		std::sort(pages.begin(), pages.end(), [](const auto& left, const auto& right) {
			return left.address < right.address;
		});
		for (std::size_t index = 1; index < pages.size(); ++index)
			if (pages[index - 1].address == pages[index].address)
				return false;

		std::vector<uint8> canonical;
		canonical.reserve(64 + pages.size() * (5 + kPageBytes));
		constexpr std::string_view domain{"CemuExtend guest memory v1\0", 27};
		canonical.insert(canonical.end(), domain.begin(), domain.end());
		for (std::size_t first = 0; first < pages.size();)
		{
			std::size_t end = first + 1;
			while (end < pages.size() &&
				   pages[end].permissions == pages[first].permissions &&
				   pages[end].address == pages[end - 1].address + kPageBytes)
				++end;
			canonical.push_back('M');
			AppendBe32(canonical, pages[first].address / kPageBytes);
			AppendBe32(canonical, pages[end - 1].address / kPageBytes + 1);
			canonical.push_back(pages[first].permissions);
			first = end;
		}
		for (const LogicalPage& page : pages)
		{
			if (std::all_of(page.bytes.begin(), page.bytes.end(),
							[](uint8 byte) { return byte == 0; }))
				continue;
			canonical.push_back('P');
			AppendBe32(canonical, page.address / kPageBytes);
			canonical.insert(canonical.end(), page.bytes.begin(), page.bytes.end());
		}
		proof = {static_cast<std::uint32_t>(pages.size()),
				 static_cast<std::uint64_t>(pages.size()) * kPageBytes,
				 Sha256(canonical)};
		return true;
	}

	void AppendEnvelopePrefix(std::string& output, std::uint32_t sequence,
						  std::string_view source, std::string_view category)
	{
		output += "{\"schema_version\":1,\"guest_cycle\":\"0\",\"sequence\":";
		output += Decimal(sequence);
		output += ",\"source\":\"";
		output += source;
		output += "\",\"core\":null,\"category\":\"";
		output += category;
		output += "\",\"event\":";
	}

	void AppendTypedField(std::string& output, bool& first, std::string_view name,
					  std::string_view type, std::string_view value)
	{
		if (!first)
			output.push_back(',');
		first = false;
		output += "\"";
		output += name;
		output += "\":{\"type\":\"";
		output += type;
		output += "\",\"value\":\"";
		output += value;
		output += "\"}";
	}

	std::string BuildTrace(const ModuleContract& mainContract,
					   const ModuleContract& providerContract,
					   std::span<const uint8> mainImage,
					   std::span<const uint8> providerImage,
					   std::uint32_t localSite, std::uint32_t localBefore,
					   std::uint32_t localAfter, std::uint32_t resolvedSymbol,
					   std::int32_t localAddend, std::uint32_t importSite,
					   std::uint32_t importBefore, std::uint32_t importAfter,
					   std::int32_t importAddend, std::int32_t displacement,
					   std::uint32_t mainEntry, const MemoryProof& proof)
	{
		std::string output;
		output.reserve(2800);
		AppendEnvelopePrefix(output, 0, "cafe", "system");
		output += "{\"kind\":\"event\",\"name\":\"rpx-rpl-rel24-call-validated\",\"fields\":{";
		bool first = true;
		AppendTypedField(output, first, "main_image_sha256", "sha256", Sha256(mainImage));
		AppendTypedField(output, first, "main_import_count", "unsigned",
					 Decimal(mainContract.importCount));
		AppendTypedField(output, first, "main_relocation_count", "unsigned",
					 Decimal(mainContract.relocationCount));
		AppendTypedField(output, first, "main_relocation_kind", "text", "rel24");
		AppendTypedField(output, first, "provider_export_count", "unsigned",
					 Decimal(providerContract.exportCount));
		AppendTypedField(output, first, "provider_image_sha256", "sha256",
					 Sha256(providerImage));
		AppendTypedField(output, first, "provider_relocation_count", "unsigned",
					 Decimal(providerContract.relocationCount));
		output += "}}}\n";

		AppendEnvelopePrefix(output, 1, "cafe", "system");
		output += "{\"kind\":\"event\",\"name\":\"rpx-rpl-rel24-local-relocation\",\"fields\":{";
		first = true;
		AppendTypedField(output, first, "addend", "signed", Decimal(localAddend));
		AppendTypedField(output, first, "kind", "text", "addr32");
		AppendTypedField(output, first, "patch_after", "hex32", Hex32(localAfter));
		AppendTypedField(output, first, "patch_before", "hex32", Hex32(localBefore));
		AppendTypedField(output, first, "patch_site", "hex32", Hex32(localSite));
		AppendTypedField(output, first, "resolved_symbol", "hex32", Hex32(resolvedSymbol));
		output += "}}}\n";

		AppendEnvelopePrefix(output, 2, "cafe", "system");
		output += "{\"kind\":\"event\",\"name\":\"rpx-rpl-rel24-import-relocation\",\"fields\":{";
		first = true;
		AppendTypedField(output, first, "addend", "signed", Decimal(importAddend));
		AppendTypedField(output, first, "displacement", "signed", Decimal(displacement));
		AppendTypedField(output, first, "kind", "text", "rel24");
		AppendTypedField(output, first, "patch_after", "hex32", Hex32(importAfter));
		AppendTypedField(output, first, "patch_before", "hex32", Hex32(importBefore));
		AppendTypedField(output, first, "patch_site", "hex32", Hex32(importSite));
		AppendTypedField(output, first, "resolved_symbol", "hex32", Hex32(resolvedSymbol));
		output += "}}}\n";

		AppendEnvelopePrefix(output, 3, "memory", "memory");
		output += "{\"kind\":\"memory_hash\",\"range\":\"rpx-rpl-rel24-linked-map-v1\","
				  "\"guest_address\":\"";
		output += Hex32(mainEntry);
		output += "\",\"byte_length\":\"";
		output += Decimal(proof.byteCount);
		output += "\",\"algorithm\":\"sha256-v1\",\"digest\":\"";
		output += proof.digest;
		output += "\"}}\n";

		AppendEnvelopePrefix(output, 4, "test_harness", "terminal");
		output += "{\"kind\":\"terminal\",\"reason\":\"test_completed\","
				  "\"detail_code\":\"rpx-rpl-rel24-link-state-v1\"}}\n";
		return output;
	}

	int Fail(std::string_view message, int exitCode)
	{
		std::cerr.write(message.data(), static_cast<std::streamsize>(message.size()));
		return exitCode;
	}
}

int main(int argc, char** argv)
{
	if (argc != 3)
		return Fail(kUsageError, 2);

	try
	{
		std::vector<uint8> mainImage;
		std::vector<uint8> providerImage;
		if (!ReadFixture(argv[1], mainImage) || !ReadFixture(argv[2], providerImage))
			return Fail(kInputError, 2);

		ModuleContract mainContract{};
		ModuleContract providerContract{};
		if (!ValidateFixtures(mainImage, providerImage, mainContract, providerContract))
			return Fail(kContractError, 1);

		std::string trace;
		{
			LoaderSession session;
			if (!session.Initialize() ||
				!RPLLoader_ProcessHeaders("main", mainImage.data(),
								  static_cast<uint32>(mainImage.size()), &session.Main()) ||
				session.Main() == nullptr || !RPLLoader_LoadSections(0, session.Main()) ||
				!RPLLoader_ProcessHeaders("linkmod", providerImage.data(),
								  static_cast<uint32>(providerImage.size()), &session.Provider()) ||
				session.Provider() == nullptr ||
				!RPLLoader_LoadSections(0, session.Provider()))
				return Fail(kHarnessError, 2);

			LoadedLinkState loaded{};
			if (!ValidateLoadedSession(session.Main(), session.Provider(), mainImage,
								   providerImage, loaded))
				return Fail(kContractError, 1);
			session.MarkLoaderOutputValidated();

			// Only the RPX entrypoint is meaningful.  Updating a zero-entry RPL would
			// drive the production assertion path, so the provider remains untouched.
			RPLLoader_UpdateEntrypoint(session.Main());
			if (session.Provider()->rplHeader.entrypoint != 0 ||
				session.Provider()->entrypoint != 0 ||
				!session.Registry().Publish(session.Main(), session.Provider()))
				return Fail(kContractError, 1);

			const std::uint32_t localSite = loaded.local.site;
			const std::uint32_t importSite = loaded.imported.site;
			const std::int32_t localAddend = loaded.local.addend;
			const std::int32_t importAddend = loaded.imported.addend;
			std::uint32_t localBefore{};
			if (!ReadGuestBe32(localSite, localBefore))
				return Fail(kContractError, 1);

			// Production order: finish every local relocation, including the
			// provider export descriptor, before main import resolution consumes it.
			RPLLoader_LinkSingleModule(session.Main(), false);
			RPLLoader_LinkSingleModule(session.Provider(), false);
			std::uint32_t localAfter{};
			std::uint32_t importBefore{};
			if (!ReadGuestBe32(localSite, localAfter) ||
				!ReadGuestBe32(importSite, importBefore))
				return Fail(kContractError, 1);
			RPLLoader_LinkSingleModule(session.Main(), true);
			RPLLoader_LinkSingleModule(session.Provider(), true);
			std::uint32_t importAfter{};
			if (!ReadGuestBe32(importSite, importAfter))
				return Fail(kContractError, 1);

			const std::uint32_t resolvedSymbol = loaded.resolvedSymbol;
			const std::int64_t displacementWide =
				static_cast<std::int64_t>(resolvedSymbol) + importAddend - importSite;
			if (displacementWide < std::numeric_limits<std::int32_t>::min() ||
				displacementWide > std::numeric_limits<std::int32_t>::max())
				return Fail(kContractError, 1);
			const std::int32_t displacement = static_cast<std::int32_t>(displacementWide);
			constexpr std::uint32_t rel24Mask = 0x03fffffc;
			const std::uint32_t expectedImport =
				(importBefore & ~rel24Mask) |
				(static_cast<std::uint32_t>(displacement) & rel24Mask);
			const std::uint32_t expectedLocal =
				resolvedSymbol + static_cast<std::uint32_t>(localAddend);
			const std::uint32_t mainEntry = session.Main()->entrypoint;
			MemoryProof proof{};
			if (!BuildMemoryProof(proof))
				return Fail(kContractError, 1);

			if (proof.pageCount != 5 || proof.byteCount != 0x5000 ||
				loaded.local.kind != RPL_RELOC_ADDR32 ||
				loaded.imported.kind != RPL_RELOC_REL24 ||
				localAddend != 0 || importAddend != 0 || localBefore != 0 ||
				localSite != kExpectedProviderLoaderBase + 8 ||
				localAfter != expectedLocal || resolvedSymbol != kExpectedProviderTextBase ||
				importSite != kExpectedMainTextBase || importBefore != 0x48000001 ||
				(importBefore >> 26) != 18 || (importBefore & 2U) != 0 ||
				displacement != 8192 || importAfter != expectedImport ||
				(importAfter & ~rel24Mask) != (importBefore & ~rel24Mask) ||
				importAfter != 0x48002001 || mainEntry != kExpectedMainTextBase)
				return Fail(kContractError, 1);

			trace = BuildTrace(mainContract, providerContract, mainImage, providerImage,
							   localSite, localBefore, localAfter, resolvedSymbol,
							   localAddend, importSite, importBefore, importAfter,
							   importAddend, displacement, mainEntry, proof);
			if (!session.Teardown())
				return Fail(kHarnessError, 2);
		}

		// Canonical stdout is withheld until exact identity-safe teardown succeeds.
		std::cout.write(trace.data(), static_cast<std::streamsize>(trace.size()));
		std::cout.flush();
		if (!std::cout)
			return Fail(kHarnessError, 2);
		return 0;
	}
	catch (...)
	{
		return Fail(kHarnessError, 2);
	}
}
