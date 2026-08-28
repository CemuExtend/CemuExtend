#include "Cemu/CemuExtend/CemodInspectionService.h"

#include <zip.h>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

namespace
{
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

	std::vector<std::byte> MinimalElf()
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
		Be32(elf, 84, 0x4e800020);
		return elf;
	}

	void Add(zip_t* archive, const char* name, const void* data, std::size_t size)
	{
		auto* source = zip_source_buffer(archive, data, size, 0);
		assert(source);
		assert(zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8) >= 0);
	}

	std::filesystem::path WritePackage()
	{
		const auto path = std::filesystem::temp_directory_path() /
						  "cemuextend-inspection-service-test.cemod";
		std::filesystem::remove(path);
		int error{};
		auto* archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_EXCL, &error);
		assert(archive);
		constexpr std::string_view manifest = R"({
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
		const auto elf = MinimalElf();
		Add(archive, "manifest.json", manifest.data(), manifest.size());
		Add(archive, "mod.elf", elf.data(), elf.size());
		assert(zip_close(archive) == 0);
		return path;
	}
} // namespace

int main()
{
	using namespace CemuExtend;
	const auto patching = CemodInspectionService::PermissionBit(
		CemodPermission::FunctionPatching);
	const auto notifications = CemodInspectionService::PermissionBit(
		CemodPermission::Notifications);
	assert(CemodInspectionService::DefaultGrantedPermissions(patching | notifications) ==
		   notifications);
	assert(CemodInspectionService::EvaluateApproval(patching, std::nullopt, false).result ==
		   CemodApprovalResult::DeniedByDefault);
	assert(CemodInspectionService::EvaluateApproval(patching, std::nullopt, true).result ==
		   CemodApprovalResult::DeniedHeadlessRequiresExplicitApproval);

	CemodApproval approval{"digest", "identity", patching, patching, true, false};
	assert(CemodInspectionService::EvaluateApproval(patching, approval, false).result ==
		   CemodApprovalResult::Approved);
	assert(CemodInspectionService::EvaluateApproval(patching | notifications, approval, false).result ==
		   CemodApprovalResult::NeedsReapproval);

	const auto path = WritePackage();
	CemodPackageDescriptor descriptor{path, "org.example.safe", "unsigned-principal", patching, CemodExecutionMode::Isolated, false, {0x0005000012345678ULL}, {}};
	auto inspected = CemodInspectionService::Inspect(descriptor, std::nullopt);
	assert(inspected.Valid());
	assert(inspected.modIdentity == "org.example.safe");
	assert(!inspected.signedPackage);

	approval = {inspected.packageDigest, inspected.modIdentity, patching, patching, true, false};
	inspected = CemodInspectionService::Inspect(descriptor, approval);
	assert(inspected.approval.result == CemodApprovalResult::Approved);
	approval.packageDigest = "changed";
	inspected = CemodInspectionService::Inspect(descriptor, approval);
	assert(inspected.approval.result == CemodApprovalResult::NeedsReapproval);
	inspected = CemodInspectionService::Inspect(descriptor, approval, true);
	assert(inspected.approval.result ==
		   CemodApprovalResult::DeniedHeadlessRequiresExplicitApproval);
	descriptor.signedPackage = true;
	inspected = CemodInspectionService::Inspect(descriptor, std::nullopt);
	assert(inspected.signedPackage);

	descriptor.discoveryError = "malformed discovery entry";
	inspected = CemodInspectionService::Inspect(descriptor, std::nullopt);
	assert(!inspected.Valid() && inspected.error == descriptor.discoveryError);
	std::filesystem::remove(path);
	return 0;
}
