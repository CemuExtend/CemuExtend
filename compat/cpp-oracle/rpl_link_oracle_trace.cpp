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
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// These loader primitives intentionally remain production implementation calls.
// The executable is test-only, but production cemu_assert/assert/fatal paths cannot
// be converted into recoverable harness errors by a C++ exception boundary.  The
// bounded contract validation below therefore runs before any such call.
bool RPLLoader_ProcessHeaders(std::string_view moduleName, uint8* rplData,
							  uint32 rplSize, RPLModule** rplLoaderContextOut);
bool RPLLoader_LoadSections(sint32 procId, RPLModule* rplLoaderContext);
void RPLLoader_UpdateEntrypoint(RPLModule* rpl);
void RPLLoader_LinkSingleModule(RPLModule* rplLoaderContext, bool resolveOnlyExports);
void RPLLoader_DiscardPartiallyLoadedModule(RPLModule* rpl);

// This adapter-local projection is the only test-only registry seam.  Production
// loader code is unchanged; the ordinary module scan consumes these globals.
extern RPLModule* rplModuleList[256];
extern sint32 rplModuleCount;

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
		"cpp_rpl_link_oracle_trace: invalid arguments\n";
	constexpr std::string_view kInputError =
		"cpp_rpl_link_oracle_trace: failed to read fixtures\n";
	constexpr std::string_view kContractError =
		"cpp_rpl_link_oracle_trace: fixtures do not satisfy link contract\n";
	constexpr std::string_view kHarnessError =
		"cpp_rpl_link_oracle_trace: failed to produce canonical trace\n";

	struct SectionRecord
	{
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

	bool HasRange(std::span<const uint8> bytes, std::uint64_t offset,
				  std::uint64_t size)
	{
		return offset <= bytes.size() && size <= bytes.size() - offset;
	}

	bool IsZero(std::span<const uint8> bytes)
	{
		return std::all_of(bytes.begin(), bytes.end(), [](uint8 byte) { return byte == 0; });
	}

	SectionRecord ReadSection(std::span<const uint8> bytes, std::size_t index)
	{
		const std::size_t offset = kSectionTableOffset + index * kSectionHeaderBytes;
		return {
			ReadBe32(bytes, offset + 4), ReadBe32(bytes, offset + 8),
			ReadBe32(bytes, offset + 12), ReadBe32(bytes, offset + 16),
			ReadBe32(bytes, offset + 20), ReadBe32(bytes, offset + 24),
			ReadBe32(bytes, offset + 28), ReadBe32(bytes, offset + 32),
			ReadBe32(bytes, offset + 36),
		};
	}

	bool IsSection(const SectionRecord& section, std::uint32_t type,
				   std::uint32_t flags, std::uint32_t address,
				   std::uint32_t size, std::uint32_t link,
				   std::uint32_t info, std::uint32_t alignment,
				   std::uint32_t entrySize)
	{
		return section.type == type && section.flags == flags &&
			   section.address == address && section.size == size &&
			   section.link == link && section.info == info &&
			   section.alignment == alignment && section.entrySize == entrySize;
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

	bool ValidateCommon(std::span<const uint8> image, std::size_t expectedBytes,
					std::uint16_t expectedSections, std::uint16_t expectedNameSection,
					std::size_t crcIndex, std::size_t fileInfoIndex)
	{
		if (image.size() != expectedBytes || image.size() < 0x34 ||
			ReadBe32(image, 0) != 0x7f454c46 || image[4] != 1 || image[5] != 2 ||
			image[6] != 1 || image[7] != 0xca || image[8] != 0xfe ||
			ReadBe16(image, 16) != 0xfe01 || ReadBe16(image, 18) != 20 ||
			ReadBe32(image, 20) != 1 || ReadBe32(image, 32) != kSectionTableOffset ||
			ReadBe16(image, 40) != 0x34 || ReadBe16(image, 46) != kSectionHeaderBytes ||
			ReadBe16(image, 48) != expectedSections ||
			ReadBe16(image, 50) != expectedNameSection ||
			!HasRange(image, kSectionTableOffset,
					  std::uint64_t{expectedSections} * kSectionHeaderBytes))
			return false;

		for (std::size_t index = 0; index < expectedSections; ++index)
		{
			const SectionRecord section = ReadSection(image, index);
			if (section.type != SHT_NOBITS &&
				!HasRange(image, section.offset, section.size))
				return false;
		}

		const SectionRecord crc = ReadSection(image, crcIndex);
		const SectionRecord fileInfo = ReadSection(image, fileInfoIndex);
		if (!IsSection(crc, SHT_RPL_CRCS, 0, 0,
					   expectedSections * sizeof(std::uint32_t), 0, 0, 4, 4) ||
			!IsSection(fileInfo, SHT_RPL_FILEINFO, 0, 0, kFileInfoBytes,
					   0, 0, 4, 0))
			return false;

		for (std::size_t index = 0; index < expectedSections; ++index)
		{
			const SectionRecord section = ReadSection(image, index);
			const std::uint32_t expected =
				(section.type == 0 || section.type == SHT_RPL_CRCS ||
				 section.type == SHT_NOBITS)
					? 0
					: Crc32(image.subspan(section.offset, section.size));
			if (ReadBe32(image, crc.offset + index * sizeof(std::uint32_t)) != expected)
				return false;
		}
		return ReadBe32(image, fileInfo.offset) == 0xcafe0402 &&
			   ReadBe32(image, fileInfo.offset + 4) == 0x1000 &&
			   ReadBe32(image, fileInfo.offset + 16) == 0x1000 &&
			   ReadBe32(image, fileInfo.offset + 20) == 0x1000 &&
			   ReadBe32(image, fileInfo.offset + 32) == 0 &&
			   ReadBe32(image, fileInfo.offset + 76) == 0 &&
			   ReadBe16(image, fileInfo.offset + 88) == 0xffff;
	}

	bool ValidateMain(std::span<const uint8> image, ModuleContract& contract)
	{
		if (!ValidateCommon(image, kMainImageBytes, kMainSectionCount, 7,
						kMainCrcSection, kMainFileInfoSection) ||
			ReadBe32(image, 24) != 0x02000000)
			return false;
		const SectionRecord text = ReadSection(image, kMainTextSection);
		const SectionRecord data = ReadSection(image, kMainDataSection);
		const SectionRecord imports = ReadSection(image, kMainImportSection);
		const SectionRecord symbols = ReadSection(image, kMainSymtabSection);
		const SectionRecord strings = ReadSection(image, kMainStrtabSection);
		const SectionRecord rela = ReadSection(image, kMainRelaSection);
		const SectionRecord names = ReadSection(image, kMainNameSection);
		const SectionRecord fileInfo = ReadSection(image, kMainFileInfoSection);
		constexpr std::array<uint8, 12> textBytes{
			0x38, 0x60, 0x00, 0x28, 0x38, 0x63, 0x00, 0x02, 0, 0, 0, 0};
		constexpr std::array<uint8, 20> importBytes{
			0, 0, 0, 0, 0, 0, 0, 0, 'l', 'i', 'n', 'k', 'm', 'o', 'd', '.', 'r', 'p', 'l', 0};
		constexpr std::array<uint8, 8> symbolName{0, 'a', 'n', 's', 'w', 'e', 'r', 0};
		constexpr char sectionNameBytes[] =
			"\0.text\0.data\0.fimport_linkmod\0.symtab\0.strtab\0.rela.data\0.shstrtab\0.crcs\0.fileinfo\0";
		const std::string_view sectionNames{sectionNameBytes,
									 sizeof(sectionNameBytes) - 1};
		if (!IsSection(ReadSection(image, 0), 0, 0, 0, 0, 0, 0, 0, 0) ||
			!IsSection(text, SHT_PROGBITS, 6, 0x02000000, 12, 0, 0, 4, 0) ||
			!IsSection(data, SHT_PROGBITS, 3, 0x10000000, 4, 0, 0, 4, 0) ||
			!IsSection(imports, SHT_RPL_IMPORTS, 6, 0xc0000000, 20, 0, 0, 4, 0) ||
			!IsSection(symbols, SHT_SYMTAB, 0, 0, 32, 5, 1, 4, 16) ||
			!IsSection(strings, SHT_STRTAB, 0, 0, 8, 0, 0, 1, 0) ||
			!IsSection(rela, SHT_RELA, 0, 0, 12, 4, 2, 4, 12) ||
			!IsSection(names, SHT_STRTAB, 0, 0, sectionNames.size(), 0, 0, 1, 0) ||
			!std::equal(textBytes.begin(), textBytes.end(), image.begin() + text.offset) ||
			ReadBe32(image, data.offset) != 0 ||
			!std::equal(importBytes.begin(), importBytes.end(), image.begin() + imports.offset) ||
			!std::equal(symbolName.begin(), symbolName.end(), image.begin() + strings.offset) ||
			!std::equal(sectionNames.begin(), sectionNames.end(), image.begin() + names.offset) ||
			!IsZero(image.subspan(symbols.offset, 16)) ||
			ReadBe32(image, symbols.offset + 16) != 1 ||
			ReadBe32(image, symbols.offset + 20) != 0xc0000008 ||
			ReadBe32(image, symbols.offset + 24) != 0 || image[symbols.offset + 28] != 0x12 ||
			image[symbols.offset + 29] != 0 || ReadBe16(image, symbols.offset + 30) != 3 ||
			ReadBe32(image, rela.offset) != 0x10000000 ||
			ReadBe32(image, rela.offset + 4) != 0x101 ||
			ReadBe32(image, rela.offset + 8) != 0 ||
			ReadBe32(image, fileInfo.offset + 8) != 0x1000 ||
			ReadBe32(image, fileInfo.offset + 12) != 0x1000 ||
			ReadBe32(image, fileInfo.offset + 52) != 2)
			return false;
		contract = {1, 0, 1};
		return true;
	}

	bool ValidateProvider(std::span<const uint8> image, ModuleContract& contract)
	{
		if (!ValidateCommon(image, kProviderImageBytes, kProviderSectionCount, 6,
						kProviderCrcSection, kProviderFileInfoSection) ||
			ReadBe32(image, 24) != 0)
			return false;
		const SectionRecord text = ReadSection(image, kProviderTextSection);
		const SectionRecord exports = ReadSection(image, kProviderExportSection);
		const SectionRecord symbols = ReadSection(image, kProviderSymtabSection);
		const SectionRecord strings = ReadSection(image, kProviderStrtabSection);
		const SectionRecord rela = ReadSection(image, kProviderRelaSection);
		const SectionRecord names = ReadSection(image, kProviderNameSection);
		const SectionRecord fileInfo = ReadSection(image, kProviderFileInfoSection);
		constexpr std::array<uint8, 8> textBytes{
			0x38, 0x60, 0x00, 0x2a, 0x4e, 0x80, 0x00, 0x20};
		constexpr std::array<uint8, 8> symbolName{0, 'a', 'n', 's', 'w', 'e', 'r', 0};
		constexpr char sectionNameBytes[] =
			"\0.text\0.fexports\0.symtab\0.strtab\0.rela.fexports\0.shstrtab\0.crcs\0.fileinfo\0";
		const std::string_view sectionNames{sectionNameBytes,
									 sizeof(sectionNameBytes) - 1};
		if (!IsSection(ReadSection(image, 0), 0, 0, 0, 0, 0, 0, 0, 0) ||
			!IsSection(text, SHT_PROGBITS, 6, 0x02000000, 8, 0, 0, 4, 0) ||
			!IsSection(exports, SHT_RPL_EXPORTS, 6, 0xc0000000, 23, 0, 0, 4, 0) ||
			!IsSection(symbols, SHT_SYMTAB, 0, 0, 32, 4, 1, 4, 16) ||
			!IsSection(strings, SHT_STRTAB, 0, 0, 8, 0, 0, 1, 0) ||
			!IsSection(rela, SHT_RELA, 0, 0, 12, 3, 2, 4, 12) ||
			!IsSection(names, SHT_STRTAB, 0, 0, sectionNames.size(), 0, 0, 1, 0) ||
			!std::equal(textBytes.begin(), textBytes.end(), image.begin() + text.offset) ||
			!std::equal(symbolName.begin(), symbolName.end(), image.begin() + strings.offset) ||
			!std::equal(sectionNames.begin(), sectionNames.end(), image.begin() + names.offset) ||
			!IsZero(image.subspan(symbols.offset, 16)) ||
			ReadBe32(image, exports.offset) != 1 || ReadBe32(image, exports.offset + 8) != 0 ||
			ReadBe32(image, exports.offset + 12) != 16 ||
			std::memcmp(image.data() + exports.offset + 16, "answer\0", 7) != 0 ||
			ReadBe32(image, symbols.offset + 16) != 1 ||
			ReadBe32(image, symbols.offset + 20) != 0x02000000 ||
			ReadBe32(image, symbols.offset + 24) != 0 || image[symbols.offset + 28] != 0x12 ||
			image[symbols.offset + 29] != 0 || ReadBe16(image, symbols.offset + 30) != 1 ||
			ReadBe32(image, rela.offset) != 0xc0000008 ||
			ReadBe32(image, rela.offset + 4) != 0x101 ||
			ReadBe32(image, rela.offset + 8) != 0 ||
			ReadBe32(image, fileInfo.offset + 8) != 0x1000 ||
			ReadBe32(image, fileInfo.offset + 12) != 0 ||
			ReadBe32(image, fileInfo.offset + 52) != 0)
			return false;
		contract = {0, 1, 1};
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

	std::uint32_t GuestAddress(const void* pointer)
	{
		return memory_getVirtualOffsetFromPointer(const_cast<void*>(pointer));
	}

	std::uint32_t RelocationSite(const RPLModule& module, std::size_t relaIndex)
	{
		const rplSectionEntryNew_t& rela = module.sectionTablePtr[relaIndex];
		const std::uint32_t targetIndex = rela.relocTargetSectionIndex;
		const rplSectionEntryNew_t& target = module.sectionTablePtr[targetIndex];
		const auto* record = static_cast<const rplRelocNew_t*>(
			module.sectionAddressTable2[relaIndex].ptr);
		const std::uint32_t targetBase =
			GuestAddress(module.sectionAddressTable2[targetIndex].ptr);
		return targetBase + (static_cast<std::uint32_t>(record->relocOffset) -
						 static_cast<std::uint32_t>(target.virtualAddress));
	}

	std::uint32_t ReadGuestBe32(std::uint32_t address)
	{
		return ReadBe32(std::span<const uint8>{memory_getPointerFromVirtualOffset(address), 4}, 0);
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
			if (clean)
			{
				if (m_provider)
					RPLLoader_DiscardPartiallyLoadedModule(m_provider);
				if (m_main)
					RPLLoader_DiscardPartiallyLoadedModule(m_main);
				clean = RPLLoader_UnloadAll();
			}
			else
			{
				// Drift can mean a missing identity, a foreign live module, or both.
				// Leaking on this process-fatal harness path is safer than dereferencing
				// an uncertain owner or asking UnloadAll to destroy foreign state.
				clean = false;
			}
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

	  private:
		bool m_mapped{};
		bool m_tornDown{};
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
		if ((address & (kPageBytes - 1)) != 0)
			return false;
		LogicalPage page{address, permissions};
		std::copy_n(memory_getPointerFromVirtualOffset(address), kPageBytes,
					page.bytes.begin());
		pages.push_back(std::move(page));
		return true;
	}

	MemoryProof BuildMemoryProof(const RPLModule& main, const RPLModule& provider)
	{
		std::vector<LogicalPage> pages;
		pages.reserve(5);
		const std::uint32_t mainText = GuestAddress(main.sectionAddressTable2[kMainTextSection].ptr);
		const std::uint32_t providerText =
			GuestAddress(provider.sectionAddressTable2[kProviderTextSection].ptr);
		const std::uint32_t mainData = GuestAddress(main.sectionAddressTable2[kMainDataSection].ptr);
		const std::uint32_t mainLoader = main.regionMappingBase_loaderInfo;
		const std::uint32_t providerLoader = provider.regionMappingBase_loaderInfo;
		// Permissions are a harness-defined projection shared with cex-memory:
		// text=R|X, data=R|W, and loader metadata=R.
		if (mainText != kExpectedMainTextBase || providerText != kExpectedProviderTextBase ||
			mainData != kExpectedMainDataBase || mainLoader != kExpectedMainLoaderBase ||
			providerLoader != kExpectedProviderLoaderBase ||
			!AddPage(pages, mainText, kPermissionRead | kPermissionExecute) ||
			!AddPage(pages, providerText, kPermissionRead | kPermissionExecute) ||
			!AddPage(pages, mainData, kPermissionRead | kPermissionWrite) ||
			!AddPage(pages, mainLoader, kPermissionRead) ||
			!AddPage(pages, providerLoader, kPermissionRead))
			throw std::runtime_error("placement contract mismatch");

		std::sort(pages.begin(), pages.end(), [](const auto& left, const auto& right) {
			return left.address < right.address;
		});
		for (std::size_t index = 1; index < pages.size(); ++index)
			if (pages[index - 1].address == pages[index].address)
				throw std::runtime_error("duplicate logical page");

		std::vector<uint8> canonical;
		canonical.reserve(64 + pages.size() * (5 + kPageBytes));
		constexpr std::string_view domain{"CemuExtend guest memory v1\0", 27};
		canonical.insert(canonical.end(), domain.begin(), domain.end());
		for (std::size_t first = 0; first < pages.size();)
		{
			std::size_t end = first + 1;
			while (end < pages.size() && pages[end].permissions == pages[first].permissions &&
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
			if (std::all_of(page.bytes.begin(), page.bytes.end(), [](uint8 byte) { return byte == 0; }))
				continue;
			canonical.push_back('P');
			AppendBe32(canonical, page.address / kPageBytes);
			canonical.insert(canonical.end(), page.bytes.begin(), page.bytes.end());
		}
		return {static_cast<std::uint32_t>(pages.size()),
				static_cast<std::uint64_t>(pages.size()) * kPageBytes, Sha256(canonical)};
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
					   std::uint32_t localSite, std::uint32_t localValue,
					   std::uint32_t importBefore, std::uint32_t importSite,
					   std::uint32_t importValue, std::uint32_t mainEntry,
					   const MemoryProof& proof)
	{
		std::string output;
		output.reserve(2400);
		AppendEnvelopePrefix(output, 0, "cafe", "system");
		output += "{\"kind\":\"event\",\"name\":\"rpx-rpl-link-validated\",\"fields\":{";
		bool first = true;
		AppendTypedField(output, first, "main_image_sha256", "sha256", Sha256(mainImage));
		AppendTypedField(output, first, "main_import_count", "unsigned",
					 Decimal(mainContract.importCount));
		AppendTypedField(output, first, "main_relocation_count", "unsigned",
					 Decimal(mainContract.relocationCount));
		AppendTypedField(output, first, "provider_export_count", "unsigned",
					 Decimal(providerContract.exportCount));
		AppendTypedField(output, first, "provider_image_sha256", "sha256", Sha256(providerImage));
		AppendTypedField(output, first, "provider_relocation_count", "unsigned",
					 Decimal(providerContract.relocationCount));
		output += "}}}\n";

		AppendEnvelopePrefix(output, 1, "cafe", "system");
		output += "{\"kind\":\"event\",\"name\":\"rpx-rpl-local-relocation\",\"fields\":{";
		first = true;
		AppendTypedField(output, first, "patch_site", "hex32", Hex32(localSite));
		AppendTypedField(output, first, "patch_value", "hex32", Hex32(localValue));
		output += "}}}\n";

		AppendEnvelopePrefix(output, 2, "cafe", "system");
		output += "{\"kind\":\"event\",\"name\":\"rpx-rpl-import-relocation\",\"fields\":{";
		first = true;
		AppendTypedField(output, first, "patch_before", "hex32", Hex32(importBefore));
		AppendTypedField(output, first, "patch_site", "hex32", Hex32(importSite));
		AppendTypedField(output, first, "patch_value", "hex32", Hex32(importValue));
		AppendTypedField(output, first, "resolved_export", "hex32", Hex32(localValue));
		output += "}}}\n";

		AppendEnvelopePrefix(output, 3, "memory", "memory");
		output += "{\"kind\":\"memory_hash\",\"range\":\"rpx-rpl-linked-map-v1\","
				  "\"guest_address\":\"";
		output += Hex32(mainEntry);
		output += "\",\"byte_length\":\"";
		output += Decimal(proof.byteCount);
		output += "\",\"algorithm\":\"sha256-v1\",\"digest\":\"";
		output += proof.digest;
		output += "\"}}\n";

		AppendEnvelopePrefix(output, 4, "test_harness", "terminal");
		output += "{\"kind\":\"terminal\",\"reason\":\"test_completed\","
				  "\"detail_code\":\"rpx-rpl-link-state-v1\"}}\n";
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
		if (!ValidateMain(mainImage, mainContract) ||
			!ValidateProvider(providerImage, providerContract))
			return Fail(kContractError, 1);

		std::string trace;
		{
			LoaderSession session;
			if (!session.Initialize() ||
				!RPLLoader_ProcessHeaders("main", mainImage.data(),
								  static_cast<uint32>(mainImage.size()), &session.Main()) ||
				!RPLLoader_LoadSections(0, session.Main()) ||
				!RPLLoader_ProcessHeaders("linkmod", providerImage.data(),
								  static_cast<uint32>(providerImage.size()), &session.Provider()) ||
				!RPLLoader_LoadSections(0, session.Provider()))
				return Fail(kHarnessError, 2);

			RPLLoader_UpdateEntrypoint(session.Main());
			if (session.Provider()->rplHeader.entrypoint != 0 ||
				session.Provider()->entrypoint != 0 ||
				!session.Registry().Publish(session.Main(), session.Provider()))
				return Fail(kContractError, 1);

			const std::uint32_t localSite =
				RelocationSite(*session.Provider(), kProviderRelaSection);
			const std::uint32_t importSite =
				RelocationSite(*session.Main(), kMainRelaSection);
			RPLLoader_LinkSingleModule(session.Main(), false);
			RPLLoader_LinkSingleModule(session.Provider(), false);
			const std::uint32_t localValue = ReadGuestBe32(localSite);
			const std::uint32_t importBefore = ReadGuestBe32(importSite);
			RPLLoader_LinkSingleModule(session.Main(), true);
			RPLLoader_LinkSingleModule(session.Provider(), true);
			const std::uint32_t importValue = ReadGuestBe32(importSite);
			const std::uint32_t mainEntry = session.Main()->entrypoint;
			const MemoryProof proof = BuildMemoryProof(*session.Main(), *session.Provider());

			if (proof.pageCount != 5 || importBefore != 0 ||
				localSite != kExpectedProviderLoaderBase + 8 ||
				importSite != kExpectedMainDataBase ||
				localValue != kExpectedProviderTextBase || importValue != localValue ||
				mainEntry != kExpectedMainTextBase)
				return Fail(kContractError, 1);
			trace = BuildTrace(mainContract, providerContract, mainImage, providerImage,
							   localSite, localValue, importBefore, importSite, importValue,
							   mainEntry, proof);
			if (!session.Teardown())
				return Fail(kHarnessError, 2);
		}

		// Canonical output is emitted only after the complete teardown succeeds as
		// far as the recoverable adapter contract can observe.
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
