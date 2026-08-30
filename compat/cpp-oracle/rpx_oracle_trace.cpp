#include "Common/precompiled.h"

#include "Cafe/OS/RPL/rpl.h"
#include "Cemu/CemuExtend/Formats/ExternalModuleMappingPolicy.h"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

bool RPLLoader_ValidateExternalImage(std::span<const uint8> image,
									 const RPLLoadOptions& options, RPLExternalMarker& marker,
									 std::string& error);

namespace
{
	constexpr std::size_t kMaximumFixtureBytes = 64U * 1024U * 1024U;
	constexpr std::size_t kReadChunkBytes = 8U * 1024U;
	constexpr std::size_t kFixtureBytes = 0x19c;
	constexpr std::size_t kSectionTableOffset = 0x40;
	constexpr std::size_t kSectionHeaderBytes = 0x28;
	constexpr std::uint16_t kSectionCount = 5;
	constexpr std::size_t kTextSectionIndex = 1;
	constexpr std::uint32_t kTextSectionType = 1;
	constexpr std::uint32_t kTextSectionFlags = 0x6;
	constexpr std::uint32_t kTextSectionAddress = 0x02000000;
	constexpr std::uint32_t kTextSectionBytes = 12;
	constexpr std::size_t kTextOffset = 0x108;
	constexpr std::size_t kSectionNamesOffset = 0x114;
	constexpr std::size_t kCrcOffset = 0x128;
	constexpr std::size_t kFileInfoOffset = 0x13c;

	constexpr std::string_view kUsageError =
		"cpp_rpx_oracle_trace: invalid arguments\n";
	constexpr std::string_view kInputError =
		"cpp_rpx_oracle_trace: failed to read fixture\n";
	constexpr std::string_view kContractError =
		"cpp_rpx_oracle_trace: fixture does not satisfy synthetic RPX contract\n";
	constexpr std::string_view kHarnessError =
		"cpp_rpx_oracle_trace: failed to produce canonical trace\n";

	void WriteBe16(std::span<uint8> bytes, std::size_t offset, std::uint16_t value)
	{
		bytes[offset] = static_cast<uint8>(value >> 8);
		bytes[offset + 1] = static_cast<uint8>(value);
	}

	void WriteBe32(std::span<uint8> bytes, std::size_t offset, std::uint32_t value)
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

	void WriteSectionHeader(std::span<uint8> image, std::size_t index,
							std::uint32_t name, std::uint32_t type, std::uint32_t flags,
							std::uint32_t address, std::uint32_t fileOffset,
							std::uint32_t size, std::uint32_t alignment,
							std::uint32_t entrySize = 0)
	{
		const std::size_t offset = kSectionTableOffset + index * kSectionHeaderBytes;
		WriteBe32(image, offset, name);
		WriteBe32(image, offset + 4, type);
		WriteBe32(image, offset + 8, flags);
		WriteBe32(image, offset + 12, address);
		WriteBe32(image, offset + 16, fileOffset);
		WriteBe32(image, offset + 20, size);
		WriteBe32(image, offset + 32, alignment);
		WriteBe32(image, offset + 36, entrySize);
	}

	std::vector<uint8> BuiltinFixture()
	{
		std::vector<uint8> image(kFixtureBytes);
		const std::array<uint8, 16> identification{
			0x7f, 'E', 'L', 'F', 1, 2, 1, 0xca, 0xfe, 0, 0, 0, 0, 0, 0, 0};
		std::copy(identification.begin(), identification.end(), image.begin());
		WriteBe16(image, 16, 0xfe01);
		WriteBe16(image, 18, 20);
		WriteBe32(image, 20, 1);
		WriteBe32(image, 24, kTextSectionAddress);
		WriteBe32(image, 32, kSectionTableOffset);
		WriteBe16(image, 40, 0x34);
		WriteBe16(image, 46, kSectionHeaderBytes);
		WriteBe16(image, 48, kSectionCount);
		WriteBe16(image, 50, 2);

		WriteSectionHeader(image, 1, 1, kTextSectionType, kTextSectionFlags,
						   kTextSectionAddress, kTextOffset, kTextSectionBytes, 4);
		WriteSectionHeader(image, 2, 7, 3, 0, 0, kSectionNamesOffset, 17, 1);
		WriteSectionHeader(image, 3, 0, 0x80000003U, 0, 0, kCrcOffset, 20, 4, 4);
		WriteSectionHeader(image, 4, 0, 0x80000004U, 0, 0, kFileInfoOffset, 0x60, 4);

		constexpr std::array<uint8, kTextSectionBytes> text{
			0x38, 0x60, 0x00, 0x28, 0x38, 0x63, 0x00, 0x02, 0, 0, 0, 0};
		std::copy(text.begin(), text.end(), image.begin() + kTextOffset);
		constexpr std::array<uint8, 17> sectionNames{
			0, '.', 't', 'e', 'x', 't', 0, '.', 's', 'h', 's', 't', 'r', 't', 'a', 'b', 0};
		std::copy(sectionNames.begin(), sectionNames.end(), image.begin() + kSectionNamesOffset);

		std::array<uint8, 0x60> fileInfo{};
		WriteBe32(fileInfo, 0x00, 0xcafe0402U);
		WriteBe32(fileInfo, 0x04, 0x1000);
		WriteBe32(fileInfo, 0x08, 0x20);
		WriteBe32(fileInfo, 0x0c, 0);
		WriteBe32(fileInfo, 0x10, 0x1000);
		WriteBe32(fileInfo, 0x14, 0);
		WriteBe32(fileInfo, 0x20, 0);
		WriteBe32(fileInfo, 0x24, 0x8000);
		WriteBe32(fileInfo, 0x28, 0x8000);
		WriteBe32(fileInfo, 0x34, 2);
		WriteBe32(fileInfo, 0x4c, 0);
		WriteBe16(fileInfo, 0x58, 0xffff);
		std::copy(fileInfo.begin(), fileInfo.end(), image.begin() + kFileInfoOffset);

		WriteBe32(image, kCrcOffset + 4, Crc32(text));
		WriteBe32(image, kCrcOffset + 8, Crc32(sectionNames));
		WriteBe32(image, kCrcOffset + 16, Crc32(fileInfo));
		return image;
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

	void AppendJsonString(std::string& output, std::string_view value)
	{
		constexpr char digits[] = "0123456789abcdef";
		output.push_back('"');
		for (const unsigned char byte : value)
		{
			switch (byte)
			{
			case '"':
				output += "\\\"";
				break;
			case '\\':
				output += "\\\\";
				break;
			case '\b':
				output += "\\b";
				break;
			case '\t':
				output += "\\t";
				break;
			case '\n':
				output += "\\n";
				break;
			case '\f':
				output += "\\f";
				break;
			case '\r':
				output += "\\r";
				break;
			default:
				if (byte < 0x20 || byte >= 0x7f)
				{
					output += "\\u00";
					output.push_back(digits[byte >> 4]);
					output.push_back(digits[byte & 0xf]);
				}
				else
					output.push_back(static_cast<char>(byte));
				break;
			}
		}
		output.push_back('"');
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

	std::string Hex32(std::uint32_t value)
	{
		constexpr char digits[] = "0123456789abcdef";
		std::string result(10, '0');
		result[0] = '0';
		result[1] = 'x';
		for (unsigned int index = 0; index < 8; ++index)
			result[2 + index] = digits[(value >> (28 - index * 4)) & 0xf];
		return result;
	}

	std::string Sha256(std::span<const uint8> image)
	{
		std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
		if (!SHA256(image.data(), image.size(), digest.data()))
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

	void AppendEnvelopePrefix(std::string& output, std::uint32_t sequence,
						  std::string_view source, std::string_view category)
	{
		output += "{\"schema_version\":1,\"guest_cycle\":\"0\",\"sequence\":";
		output += Decimal(sequence);
		output += ",\"source\":";
		AppendJsonString(output, source);
		output += ",\"core\":null,\"category\":";
		AppendJsonString(output, category);
		output += ",\"event\":";
	}

	void AppendTypedStringField(std::string& output, bool& first, std::string_view name,
								std::string_view type, std::string_view value)
	{
		if (!first)
			output.push_back(',');
		first = false;
		AppendJsonString(output, name);
		output += ":{\"type\":";
		AppendJsonString(output, type);
		output += ",\"value\":";
		AppendJsonString(output, value);
		output.push_back('}');
	}

	std::string BuildTrace(std::span<const uint8> image, std::uint32_t entryPoint,
						   std::uint32_t fileInfoFlags, std::uint16_t sectionCount,
						   const RPLLoaderInternal::ExternalSectionMapping& textSection)
	{
		std::string output;
		output.reserve(2048);

		AppendEnvelopePrefix(output, 0, "cafe", "system");
		output += "{\"kind\":\"event\",\"name\":\"rpx-contract-validated\",\"fields\":{";
		bool first = true;
		AppendTypedStringField(output, first, "entry_point", "hex32", Hex32(entryPoint));
		AppendTypedStringField(output, first, "file_info_flags", "hex32", Hex32(fileInfoFlags));
		AppendTypedStringField(output, first, "fixture", "text", "synthetic-rpx-v1");
		AppendTypedStringField(output, first, "image_bytes", "unsigned", Decimal(image.size()));
		AppendTypedStringField(output, first, "program_sha256", "sha256", Sha256(image));
		AppendTypedStringField(output, first, "section_count", "unsigned", Decimal(sectionCount));
		output += "}}}\n";

		AppendEnvelopePrefix(output, 1, "cafe", "system");
		output += "{\"kind\":\"event\",\"name\":\"rpx-section-mapping\",\"fields\":{";
		first = true;
		AppendTypedStringField(output, first, "byte_length", "unsigned",
							   Decimal(textSection.expandedSize));
		AppendTypedStringField(output, first, "flags", "hex32", Hex32(textSection.flags));
		AppendTypedStringField(output, first, "guest_address", "hex32",
							   Hex32(textSection.virtualAddress));
		AppendTypedStringField(output, first, "region", "text", "text");
		AppendTypedStringField(output, first, "section_index", "unsigned",
							   Decimal(kTextSectionIndex));
		AppendTypedStringField(output, first, "section_type", "hex32",
							   Hex32(textSection.type));
		output += "}}}\n";

		AppendEnvelopePrefix(output, 2, "test_harness", "terminal");
		output += "{\"kind\":\"terminal\",\"reason\":\"test_completed\","
				  "\"detail_code\":\"rpx-parse-map-contract-v1\"}}\n";
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
	if (argc != 2)
		return Fail(kUsageError, 2);

	try
	{
		std::vector<uint8> image;
		if (!ReadFixture(argv[1], image))
			return Fail(kInputError, 2);

		RPLLoadOptions options{};
		RPLExternalMarker marker{};
		std::string validatorError;
		if (!RPLLoader_ValidateExternalImage(image, options, marker, validatorError) ||
			marker != RPLExternalMarker::None || image != BuiltinFixture())
			return Fail(kContractError, 1);

		const std::span<const uint8> bytes{image};
		const std::uint32_t entryPoint = ReadBe32(bytes, 24);
		const std::uint32_t sectionOffset = ReadBe32(bytes, 32);
		const std::uint16_t sectionCount = ReadBe16(bytes, 48);
		const std::size_t textHeader = sectionOffset + kTextSectionIndex * kSectionHeaderBytes;
		const RPLLoaderInternal::ExternalSectionMapping textSection{
			ReadBe32(bytes, textHeader + 4),
			ReadBe32(bytes, textHeader + 8),
			ReadBe32(bytes, textHeader + 12),
			ReadBe32(bytes, textHeader + 20),
		};
		const std::size_t fileInfoHeader =
			sectionOffset + (sectionCount - 1U) * kSectionHeaderBytes;
		const std::size_t fileInfoOffset = ReadBe32(bytes, fileInfoHeader + 16);
		const std::uint32_t fileInfoFlags = ReadBe32(bytes, fileInfoOffset + 0x34);

		if (entryPoint != kTextSectionAddress || sectionCount != kSectionCount ||
			fileInfoFlags != 2 || textSection.type != kTextSectionType ||
			textSection.flags != kTextSectionFlags ||
			textSection.virtualAddress != kTextSectionAddress ||
			textSection.expandedSize != kTextSectionBytes ||
			RPLLoaderInternal::ClassifyExternalSectionMapping(textSection) !=
				RPLLoaderInternal::ExternalMappingRegion::Text)
			return Fail(kContractError, 1);

		const std::string trace =
			BuildTrace(bytes, entryPoint, fileInfoFlags, sectionCount, textSection);
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
