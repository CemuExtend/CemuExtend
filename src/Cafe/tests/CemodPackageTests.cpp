#include "Cafe/HW/Espresso/CemodPackage.h"
#include "Cafe/tests/WupsTestImage.h"

#include <zip.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{

	[[noreturn]] void CheckFailed(const char* expression, int line)
	{
		std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
		std::abort();
	}
#define CHECK(condition)                       \
	do                                         \
	{                                          \
		if (!(condition))                      \
			CheckFailed(#condition, __LINE__); \
	}                                          \
	while (false)

	void Be16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value)
	{
		bytes[offset] = static_cast<std::byte>(value >> 8);
		bytes[offset + 1] = static_cast<std::byte>(value);
	}

	void Be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
	{
		bytes[offset] = static_cast<std::byte>(value >> 24);
		bytes[offset + 1] = static_cast<std::byte>(value >> 16);
		bytes[offset + 2] = static_cast<std::byte>(value >> 8);
		bytes[offset + 3] = static_cast<std::byte>(value);
	}

	std::vector<std::byte> Elf()
	{
		std::vector<std::byte> elf(88);
		elf[0] = std::byte{0x7f};
		elf[1] = std::byte{'E'};
		elf[2] = std::byte{'L'};
		elf[3] = std::byte{'F'};
		elf[4] = std::byte{1};
		elf[5] = std::byte{2};
		elf[6] = std::byte{1};
		Be16(elf, 16, 2);
		Be16(elf, 18, 20);
		Be32(elf, 20, 1);
		Be32(elf, 24, 0x10000000);
		Be32(elf, 28, 52);
		Be16(elf, 40, 52);
		Be16(elf, 42, 32);
		Be16(elf, 44, 1);
		Be32(elf, 52, 1);
		Be32(elf, 56, 84);
		Be32(elf, 60, 0x10000000);
		Be32(elf, 64, 0x10000000);
		Be32(elf, 68, 4);
		Be32(elf, 72, 4);
		Be32(elf, 76, 5);
		Be32(elf, 80, 4096);
		Be32(elf, 84, 0x4e800020); // blr
		return elf;
	}

	std::vector<std::byte> TrustedElf(bool writableExecutable = false, bool missingBootstrap = false,
									  std::optional<std::uint8_t> relocationType = std::nullopt)
	{
		constexpr std::uint32_t namesOffset = 84;
		constexpr std::string_view names{"\0.shstrtab\0.cemod.bootstrap\0.dynsym\0.rela.dyn\0", 47};
		constexpr std::uint32_t bootstrapOffset = 132;
		constexpr std::uint32_t symbolOffset = 168;
		constexpr std::uint32_t relocationOffset = 184;
		constexpr std::uint32_t sectionsOffset = 196;
		const std::uint16_t sectionCount = relocationType ? 5 : 3;
		std::vector<std::byte> elf(sectionsOffset + sectionCount * 40);
		elf[0] = std::byte{0x7f};
		elf[1] = std::byte{'E'};
		elf[2] = std::byte{'L'};
		elf[3] = std::byte{'F'};
		elf[4] = std::byte{1};
		elf[5] = std::byte{2};
		elf[6] = std::byte{1};
		Be16(elf, 16, 3);
		Be16(elf, 18, 20);
		Be32(elf, 20, 1);
		Be32(elf, 28, 52);
		Be32(elf, 32, sectionsOffset);
		Be16(elf, 40, 52);
		Be16(elf, 42, 32);
		Be16(elf, 44, 1);
		Be16(elf, 46, 40);
		Be16(elf, 48, sectionCount);
		Be16(elf, 50, 1);
		Be32(elf, 52, 1);
		Be32(elf, 56, 0);
		Be32(elf, 60, 0);
		Be32(elf, 64, 0);
		Be32(elf, 68, elf.size());
		Be32(elf, 72, elf.size());
		Be32(elf, 76, writableExecutable ? 7 : 5);
		Be32(elf, 80, 16);
		std::memcpy(elf.data() + namesOffset, names.data(), names.size());
		Be32(elf, bootstrapOffset, 0x434d4231);
		Be16(elf, bootstrapOffset + 4, 1);
		Be16(elf, bootstrapOffset + 6, 24);
		Be32(elf, bootstrapOffset + 8, 1);
		Be32(elf, bootstrapOffset + 12, 0x867317de);
		Be32(elf, bootstrapOffset + 16, 0x02f37154);
		Be32(elf, bootstrapOffset + 20, 0x4e800421);
		Be32(elf, bootstrapOffset + 24, 0xffffffff);
		Be32(elf, bootstrapOffset + 28, 0);
		Be32(elf, bootstrapOffset + 32, 0);
		const auto namesSection = sectionsOffset + 40;
		Be32(elf, namesSection + 4, 3);
		Be32(elf, namesSection + 16, namesOffset);
		Be32(elf, namesSection + 20, names.size());
		Be32(elf, namesSection + 32, 1);
		const auto bootstrapSection = sectionsOffset + 80;
		Be32(elf, bootstrapSection, missingBootstrap ? 1 : 11);
		Be32(elf, bootstrapSection + 4, 1);
		Be32(elf, bootstrapSection + 8, 2);
		Be32(elf, bootstrapSection + 12, bootstrapOffset);
		Be32(elf, bootstrapSection + 16, bootstrapOffset);
		Be32(elf, bootstrapSection + 20, 36);
		Be32(elf, bootstrapSection + 32, 4);
		if (relocationType)
		{
			const auto symbolSection = sectionsOffset + 120;
			Be32(elf, symbolSection, 28);
			Be32(elf, symbolSection + 4, 11);
			Be32(elf, symbolSection + 8, 2);
			Be32(elf, symbolSection + 16, symbolOffset);
			Be32(elf, symbolSection + 20, 16);
			Be32(elf, symbolSection + 24, 1);
			Be32(elf, symbolSection + 32, 4);
			Be32(elf, symbolSection + 36, 16);
			const auto relocationSection = sectionsOffset + 160;
			Be32(elf, relocationSection, 36);
			Be32(elf, relocationSection + 4, 4);
			Be32(elf, relocationSection + 8, 2);
			Be32(elf, relocationSection + 16, relocationOffset);
			Be32(elf, relocationSection + 20, 12);
			Be32(elf, relocationSection + 24, 3);
			Be32(elf, relocationSection + 32, 4);
			Be32(elf, relocationSection + 36, 12);
			Be32(elf, relocationOffset, bootstrapOffset + 28);
			Be32(elf, relocationOffset + 4, *relocationType);
		}
		return elf;
	}

	zip_int64_t Add(zip_t* archive, const char* name, const void* data, std::size_t size)
	{
		auto* source = zip_source_buffer(archive, data, size, 0);
		CHECK(source != nullptr);
		const auto index = zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8);
		CHECK(index >= 0);
		CHECK(zip_set_file_compression(archive, index, ZIP_CM_DEFLATE, 9) == 0);
		return index;
	}

	using Entry = std::pair<std::string, std::vector<std::byte>>;

	std::vector<std::byte> Bytes(std::string_view value)
	{
		std::vector<std::byte> result(value.size());
		std::memcpy(result.data(), value.data(), value.size());
		return result;
	}

	void WriteEntries(const std::filesystem::path& path, const std::vector<Entry>& entries)
	{
		std::filesystem::remove(path);
		int error{};
		auto* archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_EXCL, &error);
		CHECK(archive != nullptr);
		for (const auto& [name, data] : entries)
			Add(archive, name.c_str(), data.data(), data.size());
		CHECK(zip_close(archive) == 0);
	}

	void WriteAttributedUiPackage(const std::filesystem::path& path, std::string_view manifest,
								  zip_uint8_t operatingSystem, zip_uint32_t attributes)
	{
		std::filesystem::remove(path);
		int error{};
		auto* archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_EXCL, &error);
		CHECK(archive != nullptr);
		Add(archive, "manifest.json", manifest.data(), manifest.size());
		const auto wups = BuildWupsTestImage();
		Add(archive, "plugin.wps", wups.data(), wups.size());
		const auto index = Add(archive, "ui/main/index.html", "target", 6);
		Add(archive, "ui/overlay/index.html", "html", 4);
		CHECK(zip_file_set_external_attributes(archive, index, 0, operatingSystem, attributes) == 0);
		CHECK(zip_close(archive) == 0);
	}

	std::filesystem::path PackagePath(std::string_view suffix)
	{
		return std::filesystem::temp_directory_path() /
			   ("cemuextend-package-test-" + std::to_string(static_cast<unsigned long long>(std::hash<std::string_view>{}(suffix))) + ".cemod");
	}

	constexpr std::string_view kIsolatedManifest = R"({
 "package_version":1,
 "api_version":2,
 "execution_mode":"isolated",
 "mod_id":"org.example.safe",
 "title_ids":["0005000012345678"],
 "requested_permissions":["read"],
 "memory":{"code_bytes":4096,"private_bytes":4096,"stack_bytes":4096},
 "cpu":{"instructions_per_frame":100000,"time_us_per_frame":500},
 "entrypoint":"cemod_init"
})";

	constexpr std::string_view kTrustedManifest = R"({
 "package_version":1,
 "api_version":2,
 "execution_mode":"trusted_native",
 "mod_id":"org.example.native",
 "title_ids":["0005000012345678"],
 "requested_permissions":["read","write"]
})";

	constexpr std::string_view kWupsManifest = R"({
 "package_version":2,
 "api_version":2,
 "execution_mode":"trusted_native",
 "payload":{"format":"wups","path":"plugin.wps"},
 "scope":{"type":"process","targets":["game","wii_u_menu"]},
 "permissions":{
   "native_memory":true,
   "function_patching":true,
   "physical_address_patching":false,
   "filesystem":{"read":true,"write":false},
   "network":false,
   "mapped_memory":true,
   "notifications":true,
   "content_redirection":false,
   "modules":["homebrew_functionpatcher","homebrew_notifications"]
 },
 "mod_id":"org.example.wups",
 "title_ids":["0005000012345678"],
 "requested_permissions":[]
})";

	constexpr std::string_view kLateReleaseWupsManifest = R"({
 "package_version":3,
 "api_version":2,
 "execution_mode":"trusted_native",
 "payload":{"format":"wups","path":"plugin.wps"},
 "scope":{"type":"aroma_native"},
 "lifecycle":{"unload":"after_title_threads_stop"},
 "mod_id":"org.example.wups.late",
 "title_ids":["0005000012345678"],
 "requested_permissions":[]
})";

	constexpr std::string_view kElfV2Manifest = R"({
 "package_version":2,
 "api_version":2,
 "execution_mode":"trusted_native",
 "payload":{"format":"cemod_elf","path":"mod.elf"},
 "mod_id":"org.example.native.v2",
 "title_ids":["0005000012345678"],
 "requested_permissions":["read"]
})";

	constexpr std::string_view kWebUiManifest = R"({
 "package_version":4,
 "api_version":2,
 "execution_mode":"trusted_native",
 "payload":{"format":"wups","path":"plugin.wps"},
 "scope":{"type":"aroma_native"},
 "mod_id":"org.example.web-ui",
 "title_ids":["0005000012345678"],
 "requested_permissions":["ui","network"],
 "web_ui":{
   "bridge_version":1,
   "views":{
     "main":{
       "entry":"ui/main/index.html",
       "single_instance":true,
       "modes":["window"],
       "window":{"title":"AquaU Web UI","width":960,"height":540,"min_width":480,"min_height":270,"resizable":true}
     },
     "overlay":{
       "entry":"ui/overlay/index.html",
       "modes":["overlay"],
       "overlay":{"surfaces":["tv","drc"],"z_order":"above_builtin","transparent":true,"interactive":false}
     }
   },
   "network":{
     "connect":["https://API.example.com","wss://stream.example.com:443"],
     "resources":["https://cdn.example.com"],
     "credentials":false,
     "persistent_storage":false,
     "allow_private_network":false
   }
 }
})";

	void WritePackage(const std::filesystem::path& path, bool unsafe,
					  std::string_view manifest = kIsolatedManifest, std::vector<std::byte> elf = Elf(),
					  bool invalidSignature = false)
	{
		std::filesystem::remove(path);
		int error{};
		auto* archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_EXCL, &error);
		CHECK(archive != nullptr);
		Add(archive, "manifest.json", manifest.data(), manifest.size());
		Add(archive, "mod.elf", elf.data(), elf.size());
		if (unsafe)
			Add(archive, "../escape", manifest.data(), 1);
		if (invalidSignature)
		{
			static constexpr std::byte value{1};
			Add(archive, "public_key.ed25519", &value, 1);
			Add(archive, "signature.ed25519", &value, 1);
		}
		CHECK(zip_close(archive) == 0);
	}

	void TestUnsignedPrincipalAndValidation()
	{
		const auto path = PackagePath("valid");
		WritePackage(path, false);
		std::string error;
		auto package = CemodPackage::Load(path, 0x0005000012345678ULL, error);
		CHECK(package.has_value());
		CHECK(package->manifest.modId == "org.example.safe");
		CHECK(package->principal.starts_with("sha256:"));
		CHECK(!package->signedPackage);
		auto inspected = CemodPackage::Inspect(path, error);
		CHECK(inspected.has_value());
		CHECK(inspected->targetTitleId == 0);
		CHECK(inspected->manifest.titleIds == std::vector<std::uint64_t>{0x0005000012345678ULL});
		CHECK(!CemodPackage::Load(path, 0x0005000099999999ULL, error).has_value());
		CHECK(error == "package does not target the active title");
		std::filesystem::remove(path);
	}

	void TestUnsafeEntryRejected()
	{
		const auto path = PackagePath("unsafe");
		WritePackage(path, true);
		std::string error;
		CHECK(!CemodPackage::Load(path, 0x0005000012345678ULL, error).has_value());
		CHECK(error == "package contains an unsafe entry name");
		std::filesystem::remove(path);
	}

	void TestLegacyManifestRejected()
	{
		const auto path = PackagePath("legacy-manifest");
		constexpr std::string_view legacy = R"({"api_version":2,"mod_id":"old","title_ids":["0005000012345678"],"requested_permissions":[]})";
		WritePackage(path, false, legacy);
		std::string error;
		CHECK(!CemodPackage::Load(path, 0x0005000012345678ULL, error));
		CHECK(error == "manifest.json does not match the CEX2 schema");
		std::filesystem::remove(path);
	}

	void TestTrustedValidation()
	{
		std::string error;
		auto path = PackagePath("trusted-valid");
		WritePackage(path, false, kTrustedManifest, TrustedElf());
		auto package = CemodPackage::Load(path, 0x0005000012345678ULL, error);
		CHECK(package && package->IsTrustedNative());
		std::filesystem::remove(path);

		path = PackagePath("trusted-none-relocation");
		WritePackage(path, false, kTrustedManifest, TrustedElf(false, false, 0));
		CHECK(CemodPackage::Load(path, 0x0005000012345678ULL, error));
		std::filesystem::remove(path);

		path = PackagePath("trusted-wx");
		WritePackage(path, false, kTrustedManifest, TrustedElf(true));
		CHECK(!CemodPackage::Load(path, 0x0005000012345678ULL, error));
		CHECK(error == "PPC ELF contains an invalid or writable-executable segment");
		std::filesystem::remove(path);

		path = PackagePath("trusted-bootstrap");
		WritePackage(path, false, kTrustedManifest, TrustedElf(false, true));
		CHECK(!CemodPackage::Load(path, 0x0005000012345678ULL, error));
		CHECK(error == "trusted ELF is missing .cemod.bootstrap");
		std::filesystem::remove(path);

		path = PackagePath("trusted-relocation");
		WritePackage(path, false, kTrustedManifest, TrustedElf(false, false, 2));
		CHECK(!CemodPackage::Load(path, 0x0005000012345678ULL, error));
		CHECK(error == "trusted ELF contains an unsupported relocation");
		std::filesystem::remove(path);

		path = PackagePath("bad-signature");
		WritePackage(path, false, kTrustedManifest, TrustedElf(), true);
		CHECK(!CemodPackage::Load(path, 0x0005000012345678ULL, error));
		CHECK(error == "Ed25519 signature material has an invalid size");
		std::filesystem::remove(path);
	}

	void TestV2PayloadAndManifest()
	{
		std::string error;
		auto path = PackagePath("wups-v2");
		WriteEntries(path, {{"plugin.wps", BuildWupsTestImage()}, {"manifest.json", Bytes(kWupsManifest)}});
		auto package = CemodPackage::Load(path, 0x0005000012345678ULL, error);
		CHECK(package.has_value());
		CHECK(package->manifest.packageVersion == 2);
		CHECK(package->manifest.payload.format == CemodPayloadFormat::Wups);
		CHECK(package->manifest.payload.path == "plugin.wps");
		CHECK(package->manifest.scope.type == CemodScopeType::Process);
		CHECK(package->manifest.scope.targets == std::vector<std::string>({"game", "wii_u_menu"}));
		CHECK(package->manifest.nativePermissions.functionPatching);
		CHECK(package->manifest.nativePermissions.filesystemRead);
		CHECK(!package->manifest.nativePermissions.filesystemWrite);
		CHECK(package->manifest.nativePermissions.modules.size() == 2);
		CHECK(package->payload.size() == BuildWupsTestImage().size());
		CHECK(package->elf.empty());
		CHECK(package->wups && package->wups->metadata.name == "Test Plugin");
		std::filesystem::remove(path);

		path = PackagePath("elf-v2");
		WriteEntries(path, {{"manifest.json", Bytes(kElfV2Manifest)}, {"mod.elf", TrustedElf()}});
		package = CemodPackage::Load(path, 0x0005000012345678ULL, error);
		CHECK(package && package->manifest.payload.format == CemodPayloadFormat::CemodElf);
		CHECK(!package->wups);
		CHECK(package->elf == package->payload);
		std::filesystem::remove(path);
	}

	void TestV3PluginManagementPermission()
	{
		std::string v3(kWupsManifest);
		v3.replace(v3.find("\"package_version\":2"),
				   std::string_view("\"package_version\":2").size(),
				   "\"package_version\":3");
		const auto permissionsEnd = v3.find("\n },", v3.find("\"permissions\""));
		CHECK(permissionsEnd != std::string::npos);
		v3.insert(permissionsEnd, ",\n   \"plugin_management\":true");
		std::string error;
		auto path = PackagePath("wups-v3-management");
		WriteEntries(path, {{"plugin.wps", BuildWupsTestImage()},
							{"manifest.json", Bytes(v3)}});
		auto package = CemodPackage::Load(path, 0x0005000012345678ULL, error);
		CHECK(package.has_value());
		CHECK(package->manifest.packageVersion == 3);
		CHECK(package->manifest.nativePermissions.pluginManagement);
		std::filesystem::remove(path);

		std::string v2(kWupsManifest);
		const auto v2PermissionsEnd = v2.find("\n },", v2.find("\"permissions\""));
		CHECK(v2PermissionsEnd != std::string::npos);
		v2.insert(v2PermissionsEnd, ",\n   \"plugin_management\":false");
		path = PackagePath("wups-v2-management-rejected");
		WriteEntries(path, {{"plugin.wps", BuildWupsTestImage()},
							{"manifest.json", Bytes(v2)}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("package_version 3") != std::string::npos);
		std::filesystem::remove(path);
	}

	void TestV3Mem2ExpansionRequest()
	{
		std::string v3(kWupsManifest);
		v3.replace(v3.find("\"package_version\":2"),
				   std::string_view("\"package_version\":2").size(),
				   "\"package_version\":3");
		const auto payload = v3.find("\n \"payload\"");
		CHECK(payload != std::string::npos);
		v3.insert(payload, "\n \"memory\":{\"mem2_expansion_bytes\":268435456},");
		std::string error;
		auto path = PackagePath("wups-v3-mem2");
		WriteEntries(path, {{"plugin.wps", BuildWupsTestImage()},
							{"manifest.json", Bytes(v3)}});
		auto package = CemodPackage::Load(path, 0x0005000012345678ULL, error);
		CHECK(package.has_value());
		CHECK(package->manifest.mem2ExpansionBytes == 256U * 1024U * 1024U);
		std::filesystem::remove(path);

		std::string v2(kWupsManifest);
		const auto v2Payload = v2.find("\n \"payload\"");
		CHECK(v2Payload != std::string::npos);
		v2.insert(v2Payload, "\n \"memory\":{\"mem2_expansion_bytes\":4096},");
		path = PackagePath("wups-v2-mem2-rejected");
		WriteEntries(path, {{"plugin.wps", BuildWupsTestImage()},
							{"manifest.json", Bytes(v2)}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("package_version 3") != std::string::npos);
		std::filesystem::remove(path);
	}

	void TestV3LateWupsReleasePolicy()
	{
		std::string error;
		auto path = PackagePath("wups-v3-late-release");
		WriteEntries(path, {{"plugin.wps", BuildWupsTestImage()},
							{"manifest.json", Bytes(kLateReleaseWupsManifest)}});
		auto package = CemodPackage::Load(path, 0x0005000012345678ULL, error);
		CHECK(package.has_value());
		CHECK(package->manifest.unloadPolicy ==
			  CemodUnloadPolicy::AfterTitleThreadsStop);
		std::filesystem::remove(path);

		std::string v2(kLateReleaseWupsManifest);
		v2.replace(v2.find("\"package_version\":3"),
				   std::string_view("\"package_version\":3").size(),
				   "\"package_version\":2");
		path = PackagePath("wups-v2-late-release-rejected");
		WriteEntries(path, {{"plugin.wps", BuildWupsTestImage()},
							{"manifest.json", Bytes(v2)}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("package_version 3") != std::string::npos);
		std::filesystem::remove(path);

		std::string elf(kLateReleaseWupsManifest);
		elf.replace(elf.find("\"wups\""), 6, "\"cemod_elf\"");
		elf.replace(elf.find("\"plugin.wps\""), 12, "\"mod.elf\"");
		path = PackagePath("elf-late-release-rejected");
		WriteEntries(path, {{"mod.elf", TrustedElf()}, {"manifest.json", Bytes(elf)}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("only for trusted_native WUPS") != std::string::npos);
		std::filesystem::remove(path);
	}

	void TestPayloadAndZipRejections()
	{
		std::string error;
		auto path = PackagePath("payload-none");
		WriteEntries(path, {{"manifest.json", Bytes(kWupsManifest)}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("exactly one") != std::string::npos);
		std::filesystem::remove(path);

		path = PackagePath("payload-multiple");
		WriteEntries(path, {{"manifest.json", Bytes(kWupsManifest)}, {"plugin.wps", BuildWupsTestImage()}, {"mod.elf", TrustedElf()}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("exactly one") != std::string::npos);
		std::filesystem::remove(path);

		path = PackagePath("descriptor-mismatch");
		WriteEntries(path, {{"manifest.json", Bytes(kElfV2Manifest)}, {"plugin.wps", BuildWupsTestImage()}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("descriptor") != std::string::npos);
		std::filesystem::remove(path);

		path = PackagePath("normalized-duplicate");
		WriteEntries(path, {{"manifest.json", Bytes(kWupsManifest)}, {"./manifest.json", Bytes(kWupsManifest)}, {"plugin.wps", BuildWupsTestImage()}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("normalized") != std::string::npos);
		std::filesystem::remove(path);

		path = PackagePath("absolute");
		WriteEntries(path, {{"manifest.json", Bytes(kWupsManifest)}, {"/plugin.wps", BuildWupsTestImage()}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("unsafe") != std::string::npos);
		std::filesystem::remove(path);

		path = PackagePath("unknown-entry");
		WriteEntries(path, {{"manifest.json", Bytes(kWupsManifest)}, {"plugin.wps", BuildWupsTestImage()}, {"required.future", Bytes("x")}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("unknown mandatory") != std::string::npos);
		std::filesystem::remove(path);

		path = PackagePath("compression-bomb");
		WriteEntries(path, {{"manifest.json", Bytes(kWupsManifest)},
							{"plugin.wps", std::vector<std::byte>(2U * 1024U * 1024U)}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("expansion limit") != std::string::npos);
		std::filesystem::remove(path);
	}

	void TestWebUiV4Package()
	{
		std::string error;
		auto path = PackagePath("web-ui-v4");
		WriteEntries(path, {{"manifest.json", Bytes(kWebUiManifest)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui/main/index.html", Bytes("<!doctype html><title>AquaU</title>")},
							{"ui/main/assets/app.js", Bytes("window.ready = true;")},
							{"ui/overlay/index.html", Bytes("<!doctype html><canvas></canvas>")}});
		auto package = CemodPackage::Inspect(path, error);
		CHECK(package.has_value());
		CHECK(package->manifest.packageVersion == 4);
		CHECK((package->manifest.requestedPermissions & 64U) != 0);
		CHECK(package->manifest.webUi.has_value());
		CHECK(package->manifest.webUi->bridgeVersion == 1);
		CHECK(package->manifest.webUi->views.size() == 2);
		CHECK(package->manifest.webUi->views.at("main").singleInstance);
		CHECK(package->manifest.webUi->views.at("main").window->width == 960);
		CHECK(package->manifest.webUi->views.at("overlay").overlay->surfaces.size() == 2);
		CHECK(package->manifest.webUi->views.at("overlay").overlay->order ==
			  CemodWebUiOverlayOrder::AboveBuiltin);
		CHECK(package->manifest.webUi->network.connect ==
			  std::vector<std::string>({"https://api.example.com:443", "wss://stream.example.com:443"}));
		CHECK(package->uiAssets.size() == 3);
		CHECK(package->uiAssets.contains("ui/main/index.html"));
		CHECK(package->uiAssets.contains("ui/main/assets/app.js"));
		std::filesystem::remove(path);

		std::string belowBuiltin(kWebUiManifest);
		belowBuiltin.replace(belowBuiltin.find("above_builtin"),
							 std::string_view("above_builtin").size(), "below_builtin");
		path = PackagePath("web-ui-below-builtin");
		WriteEntries(path, {{"manifest.json", Bytes(belowBuiltin)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui/main/index.html", Bytes("html")},
							{"ui/overlay/index.html", Bytes("html")}});
		package = CemodPackage::Inspect(path, error);
		CHECK(package && package->manifest.webUi->views.at("overlay").overlay->order ==
							 CemodWebUiOverlayOrder::BelowBuiltin);
		std::filesystem::remove(path);

		std::string missingOrder(kWebUiManifest);
		const auto orderField = missingOrder.find(",\"z_order\":\"above_builtin\"");
		CHECK(orderField != std::string::npos);
		missingOrder.erase(orderField, std::string_view(",\"z_order\":\"above_builtin\"").size());
		path = PackagePath("web-ui-missing-z-order");
		WriteEntries(path, {{"manifest.json", Bytes(missingOrder)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui/main/index.html", Bytes("html")},
							{"ui/overlay/index.html", Bytes("html")}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("overlay descriptor") != std::string::npos);
		std::filesystem::remove(path);

		std::string invalidOrder(kWebUiManifest);
		invalidOrder.replace(invalidOrder.find("above_builtin"),
							 std::string_view("above_builtin").size(), "middle");
		path = PackagePath("web-ui-invalid-z-order");
		WriteEntries(path, {{"manifest.json", Bytes(invalidOrder)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui/main/index.html", Bytes("html")},
							{"ui/overlay/index.html", Bytes("html")}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("z_order is invalid") != std::string::npos);
		std::filesystem::remove(path);

		path = PackagePath("web-ui-missing-entry");
		WriteEntries(path, {{"manifest.json", Bytes(kWebUiManifest)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui/main/index.html", Bytes("html")}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("entry is missing") != std::string::npos);
		std::filesystem::remove(path);

		std::string missingPermission(kWebUiManifest);
		const auto permission = missingPermission.find("\"ui\",");
		CHECK(permission != std::string::npos);
		missingPermission.erase(permission, std::string_view("\"ui\",").size());
		path = PackagePath("web-ui-missing-permission");
		WriteEntries(path, {{"manifest.json", Bytes(missingPermission)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui/main/index.html", Bytes("html")},
							{"ui/overlay/index.html", Bytes("html")}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("requires ui permission") != std::string::npos);
		std::filesystem::remove(path);

		std::string oldVersion(kWebUiManifest);
		oldVersion.replace(oldVersion.find("\"package_version\":4"),
						   std::string_view("\"package_version\":4").size(), "\"package_version\":3");
		path = PackagePath("web-ui-old-version");
		WriteEntries(path, {{"manifest.json", Bytes(oldVersion)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui/main/index.html", Bytes("html")},
							{"ui/overlay/index.html", Bytes("html")}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("package_version 4") != std::string::npos);
		std::filesystem::remove(path);

		std::string invalidOrigin(kWebUiManifest);
		invalidOrigin.replace(invalidOrigin.find("https://API.example.com"),
							  std::string_view("https://API.example.com").size(), "http://api.example.com");
		path = PackagePath("web-ui-invalid-origin");
		WriteEntries(path, {{"manifest.json", Bytes(invalidOrigin)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui/main/index.html", Bytes("html")},
							{"ui/overlay/index.html", Bytes("html")}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("invalid origin") != std::string::npos);
		std::filesystem::remove(path);

		std::string emptyPort(kWebUiManifest);
		emptyPort.replace(emptyPort.find("https://API.example.com"),
						  std::string_view("https://API.example.com").size(), "https://api.example.com:");
		path = PackagePath("web-ui-empty-port");
		WriteEntries(path, {{"manifest.json", Bytes(emptyPort)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui/main/index.html", Bytes("html")},
							{"ui/overlay/index.html", Bytes("html")}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("invalid origin") != std::string::npos);
		std::filesystem::remove(path);

		path = PackagePath("web-ui-noncanonical-entry");
		WriteEntries(path, {{"manifest.json", Bytes(kWebUiManifest)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui//main/index.html", Bytes("html")},
							{"ui/overlay/index.html", Bytes("html")}});
		CHECK(!CemodPackage::Inspect(path, error));
		std::filesystem::remove(path);

		path = PackagePath("web-ui-unicode-entry");
		WriteEntries(path, {{"manifest.json", Bytes(kWebUiManifest)},
							{"plugin.wps", BuildWupsTestImage()},
							{"ui/main/index.html", Bytes("html")},
							{"ui/overlay/index.html", Bytes("html")},
							{"ui/caf\xc3\xa9.css", Bytes("css")}});
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("unsafe entry name") != std::string::npos);
		std::filesystem::remove(path);

		path = PackagePath("web-ui-non-unix-symlink");
		WriteAttributedUiPackage(path, kWebUiManifest, ZIP_OPSYS_DOS, 0120777U << 16U);
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("non-regular entry") != std::string::npos);
		std::filesystem::remove(path);

		path = PackagePath("web-ui-device-entry");
		WriteAttributedUiPackage(path, kWebUiManifest, ZIP_OPSYS_UNIX, 0020666U << 16U);
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("non-regular entry") != std::string::npos);
		std::filesystem::remove(path);

		std::vector<Entry> tooMany{{"manifest.json", Bytes(kWebUiManifest)},
								   {"plugin.wps", BuildWupsTestImage()},
								   {"ui/main/index.html", Bytes("html")},
								   {"ui/overlay/index.html", Bytes("html")}};
		for (std::size_t index = 0; index < CemodPackage::kMaximumUiFiles - 1; ++index)
			tooMany.emplace_back("ui/assets/" + std::to_string(index) + ".css", Bytes(""));
		path = PackagePath("web-ui-too-many-files");
		WriteEntries(path, tooMany);
		CHECK(!CemodPackage::Inspect(path, error));
		CHECK(error.find("count limit") != std::string::npos);
		std::filesystem::remove(path);
	}

} // namespace

int main()
{
	if (const auto* path = std::getenv("CEMUEXTEND_CEMOD_CONFORMANCE_PACKAGE"))
	{
		std::string error;
		const auto package = CemodPackage::Inspect(path, error);
		if (!package)
			std::cerr << error << '\n';
		CHECK(package.has_value());
		CHECK(package->signedPackage);
		CHECK(package->manifest.payload.format == CemodPayloadFormat::Wups);
		CHECK(package->wups.has_value());
		return 0;
	}
	TestUnsignedPrincipalAndValidation();
	TestUnsafeEntryRejected();
	TestLegacyManifestRejected();
	TestTrustedValidation();
	TestV2PayloadAndManifest();
	TestV3PluginManagementPermission();
	TestV3Mem2ExpansionRequest();
	TestV3LateWupsReleasePolicy();
	TestPayloadAndZipRejections();
	TestWebUiV4Package();
	return 0;
}
