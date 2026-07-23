#include "Cafe/HW/Espresso/WupsBinary.h"
#include "Cafe/tests/WupsTestImage.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

namespace
{
	[[noreturn]] void CheckFailed(const char* expression, int line)
	{
		std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
		std::abort();
	}
#define CHECK(condition) do { if (!(condition)) CheckFailed(#condition, __LINE__); } while (false)

	bool Rejected(const std::vector<std::byte>& image, std::string_view contains = {})
	{
		std::string error;
		const auto result = WupsBinaryInspector::Inspect(image, error);
		return !result && (contains.empty() || error.find(contains) != std::string::npos);
	}

	void TestValidInspection()
	{
		std::string error;
		auto result = WupsBinaryInspector::Inspect(BuildWupsTestImage(), error);
		CHECK(result.has_value());
		CHECK(result->metadata.name == "Test Plugin");
		CHECK((result->metadata.abiVersion == WupsVersion{0, 9, 1}));
		CHECK(result->hooks.size() == 1);
		CHECK(result->replacements.size() == 1);
		CHECK(result->replacements[0].mandatory);
		CHECK(result->imports.size() == 1);
		CHECK(result->imports[0].module == "coreinit");
		CHECK(result->imports[0].kind == WupsSymbolKind::Function);
		CHECK(result->relocations.size() == 1);
		CHECK(result->requiredModules == std::vector<std::string>{"coreinit"});
		CHECK(result->processTargets == std::vector<std::uint32_t>{16});
		CHECK(result->compatibilityWarnings.empty());

		WupsTestImageOptions tls;
		tls.tls = true;
		result = WupsBinaryInspector::Inspect(BuildWupsTestImage(tls), error);
		CHECK(result && result->usesTls);

		WupsTestImageOptions legacy;
		legacy.metadata = DefaultWupsTestMetadata();
		const auto position = legacy.metadata.find("wups=0.9.1");
		legacy.metadata.replace(position, 10, "wups=0.9.0");
		result = WupsBinaryInspector::Inspect(BuildWupsTestImage(legacy), error);
		CHECK((result && result->metadata.abiVersion == WupsVersion{0, 9, 0}));
		CHECK(result->compatibilityWarnings.size() == 1);
	}

	void TestStructuralRejections()
	{
		auto image = BuildWupsTestImage();
		image[9] = std::byte{'X'};
		CHECK(Rejected(image, "WPS RPL"));

		image = BuildWupsTestImage();
		WupsTestBe32(image, 0x40 + 40 + 16, image.size() - 1);
		CHECK(Rejected(image, "out of bounds"));

		WupsTestImageOptions compressed;
		compressed.compressText = true;
		image = BuildWupsTestImage(compressed);
		const auto textHeader = 0x40 + 40;
		const auto textOffset = WupsTestU32(image, textHeader + 16);
		const auto textSize = WupsTestU32(image, textHeader + 20);
		image[textOffset + textSize - 1] ^= std::byte{0xff};
		CHECK(Rejected(image, "compressed section"));

		image = BuildWupsTestImage();
		WupsTestBe32(image, 0x40 + 40, 0xffffffffU);
		CHECK(Rejected(image, "section name"));

		image = BuildWupsTestImage();
		constexpr std::size_t symbolIndex = 8;
		WupsTestBe32(image, 0x40 + symbolIndex * 40 + 36, 8);
		CHECK(Rejected(image, "symbol or string table"));

		image = BuildWupsTestImage();
		const auto crcIndex = static_cast<std::size_t>(12 - 1);
		const auto crcOffset = WupsTestU32(image, 0x40 + crcIndex * 40 + 16);
		image[crcOffset + 4] ^= std::byte{1};
		CHECK(Rejected(image, "CRC"));
	}

	void TestDescriptorRejections()
	{
		WupsTestImageOptions options;
		options.relocationType = 2;
		CHECK(Rejected(BuildWupsTestImage(options), "relocation"));

		options = {};
		constexpr char unterminated[] = "name=Test\0wups=0.9.1";
		options.metadata.assign(unterminated, sizeof(unterminated) - 1);
		CHECK(Rejected(BuildWupsTestImage(options), ".wups.meta"));

		constexpr char duplicate[] = "name=Test\0name=Again\0wups=0.9.1\0";
		options.metadata.assign(duplicate, sizeof(duplicate) - 1);
		CHECK(Rejected(BuildWupsTestImage(options), "duplicate key"));

		options = {};
		constexpr char future[] = "name=Future Plugin\0wups=9.9.9\0";
		options.metadata.assign(future, sizeof(future) - 1);
		CHECK(Rejected(BuildWupsTestImage(options), "Future Plugin"));

		options = {};
		options.hookType = 99;
		CHECK(Rejected(BuildWupsTestImage(options), ".wups.hooks"));

		options = {};
		options.processTarget = 13;
		CHECK(Rejected(BuildWupsTestImage(options), ".wups.load"));

		options = {};
		options.wrongImportKind = true;
		CHECK(Rejected(BuildWupsTestImage(options), "function/data"));
	}
}

int main()
{
	if (const auto* path = std::getenv("CEMUEXTEND_WPS_CONFORMANCE_IMAGE"))
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		CHECK(input.good());
		const auto size = input.tellg();
		CHECK(size > 0);
		std::vector<std::byte> image(static_cast<std::size_t>(size));
		input.seekg(0);
		CHECK(input.read(reinterpret_cast<char*>(image.data()), size).good());
		std::string error;
		const auto result = WupsBinaryInspector::Inspect(image, error);
		if (!result) std::cerr << error << '\n';
		CHECK(result.has_value());
		CHECK((result->metadata.abiVersion == WupsVersion{0, 9, 1}));
		return 0;
	}
	TestValidInspection();
	TestStructuralRejections();
	TestDescriptorRejections();
	return 0;
}
