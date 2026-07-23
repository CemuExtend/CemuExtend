#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsServices.h"
#include "Cafe/HW/Espresso/WupsFunctionPatcher.h"

#include <limits>
#include <map>
#include <mutex>

namespace
{
	constexpr std::uint32_t kDescriptorV2Size = 32;
	constexpr std::uint32_t kDescriptorV3RplSize = 36;
	constexpr std::size_t kMaximumFunctionName = 100;
	constexpr std::size_t kDynamicDescriptorBase =
		std::numeric_limits<std::size_t>::max() / 2;

	[[nodiscard]] std::uint32_t ReadBe32(std::span<const std::byte> bytes,
		std::size_t offset)
	{
		return (std::to_integer<std::uint32_t>(bytes[offset]) << 24) |
			(std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16) |
			(std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8) |
			std::to_integer<std::uint32_t>(bytes[offset + 3]);
	}

	class FunctionPatcherFacade final : public IWupsFunctionPatcherFacade
	{
	public:
		FunctionPatcherFacade(std::shared_ptr<IWupsPlatform> platform,
			std::shared_ptr<WupsFunctionPatchManager> manager) :
			m_platform(std::move(platform)), m_manager(std::move(manager))
		{
		}

		std::uint32_t ApiVersion() const override { return 2; }

		WupsServiceStatus AddPatch(WupsOwnerToken owner,
			std::uint32_t descriptorAddress, bool allowPhysicalAddress,
			std::uint32_t& handle, bool& applied,
			std::string& error) override
		{
			handle = 0;
			applied = false;
			error.clear();
			if (!m_platform || !m_manager || owner.owner == 0 ||
				owner.generation == 0 || descriptorAddress == 0)
			{
				error = "FunctionPatcher is uninitialized or received a null argument";
				return WupsServiceStatus::InvalidArgument;
			}

			WupsPatchRequest request;
			const auto parsed = Parse(owner, descriptorAddress,
				allowPhysicalAddress, request, error);
			if (parsed != WupsServiceStatus::Success) return parsed;

			std::lock_guard lock(m_mutex);
			const auto latest = m_latestGeneration.find(owner.owner);
			if (latest != m_latestGeneration.end() &&
				latest->second != owner.generation)
			{
				if (owner.generation < latest->second)
				{
					error = "FunctionPatcher rejected a stale owner generation";
					return WupsServiceStatus::StaleGeneration;
				}
				if (std::ranges::any_of(m_records, [&](const auto& entry) {
					return entry.second.owner.owner == owner.owner;
				}))
				{
					error = "FunctionPatcher owner generation changed with live handles";
					return WupsServiceStatus::Busy;
				}
			}
			m_latestGeneration[owner.owner] = owner.generation;
			handle = NextHandle();
			request.descriptorIndex = kDynamicDescriptorBase + handle;
			if (!m_manager->Add(request, error))
			{
				handle = 0;
				return WupsServiceStatus::Conflict;
			}
			applied = std::ranges::any_of(m_manager->Applied(),
				[&](const auto& patch) {
					return patch.owner == request.owner &&
						patch.descriptorIndex == request.descriptorIndex;
				});
			m_records.emplace(handle, Record{owner, request.descriptorIndex});
			return WupsServiceStatus::Success;
		}

		WupsServiceStatus RemovePatch(WupsOwnerToken owner,
			std::uint32_t handle, std::string& error) override
		{
			error.clear();
			std::lock_guard lock(m_mutex);
			const auto found = m_records.find(handle);
			if (found == m_records.end())
				return WupsServiceStatus::NotFound;
			if (found->second.owner != owner)
			{
				error = "FunctionPatcher handle belongs to another owner generation";
				return found->second.owner.owner == owner.owner ?
					WupsServiceStatus::StaleGeneration :
					WupsServiceStatus::OwnerMismatch;
			}
			if (!m_manager->Remove({owner.owner, owner.generation},
				found->second.descriptorIndex, error))
			{
				// Deactivation may already have removed the manager record. The
				// public handle is still retired exactly once by this facade.
				if (error.find("was not found") == std::string::npos)
					return WupsServiceStatus::InternalError;
			}
			m_records.erase(found);
			return WupsServiceStatus::Success;
		}

		WupsServiceStatus IsPatchApplied(WupsOwnerToken owner,
			std::uint32_t handle, bool& applied,
			std::string& error) const override
		{
			applied = false;
			error.clear();
			std::lock_guard lock(m_mutex);
			const auto found = m_records.find(handle);
			if (found == m_records.end()) return WupsServiceStatus::NotFound;
			if (found->second.owner != owner)
			{
				error = "FunctionPatcher handle belongs to another owner generation";
				return found->second.owner.owner == owner.owner ?
					WupsServiceStatus::StaleGeneration :
					WupsServiceStatus::OwnerMismatch;
			}
			applied = std::ranges::any_of(m_manager->Applied(),
				[&](const auto& patch) {
					return patch.owner == WupsPatchOwner{
						owner.owner, owner.generation} &&
						patch.descriptorIndex == found->second.descriptorIndex;
				});
			return WupsServiceStatus::Success;
		}

		void ReleaseOwner(WupsOwnerToken owner) override
		{
			std::lock_guard lock(m_mutex);
			for (auto iterator = m_records.begin(); iterator != m_records.end();)
			{
				if (iterator->second.owner != owner)
				{
					++iterator;
					continue;
				}
				std::string ignored;
				(void)m_manager->Remove({owner.owner, owner.generation},
					iterator->second.descriptorIndex, ignored);
				iterator = m_records.erase(iterator);
			}
		}

	private:
		struct Record
		{
			WupsOwnerToken owner;
			std::size_t descriptorIndex{};
		};

		WupsServiceStatus Parse(WupsOwnerToken owner, std::uint32_t address,
			bool allowPhysicalAddress, WupsPatchRequest& request,
			std::string& error) const
		{
			std::array<std::byte, kDescriptorV3RplSize> bytes{};
			if (!m_platform->ValidateGuestRangeForOwner(
				owner, address, 4, WupsGuestAccess::Read) ||
				!m_platform->ReadGuest(address, std::span(bytes).first(4)))
			{
				error = "FunctionPatcher descriptor version is not readable";
				return WupsServiceStatus::InvalidArgument;
			}
			const auto version = ReadBe32(bytes, 0);
			const auto size = version == 2 ? kDescriptorV2Size :
				version == 3 ? kDescriptorV3RplSize : 0;
			if (size == 0)
			{
				error = fmt::format(
					"FunctionPatcher descriptor version {} is unsupported (supported: 2..3)",
					version);
				return WupsServiceStatus::UnsupportedVersion;
			}
			if ((address & 3U) != 0 ||
				!m_platform->ValidateGuestRangeForOwner(owner, address, size,
					WupsGuestAccess::Read) ||
				!m_platform->ReadGuest(address, std::span(bytes).first(size)))
			{
				error = "FunctionPatcher descriptor is misaligned or truncated";
				return WupsServiceStatus::InvalidArgument;
			}

			std::uint32_t type{};
			std::uint32_t physical{};
			std::uint32_t effective{};
			std::uint32_t replacement{};
			std::uint32_t callThrough{};
			std::uint32_t process{};
			std::uint32_t nameAddress{};
			std::uint32_t library{};
			if (version == 2)
			{
				physical = ReadBe32(bytes, 4);
				effective = ReadBe32(bytes, 8);
				replacement = ReadBe32(bytes, 12);
				callThrough = ReadBe32(bytes, 16);
				library = ReadBe32(bytes, 20);
				nameAddress = ReadBe32(bytes, 24);
				process = ReadBe32(bytes, 28);
			}
			else
			{
				type = ReadBe32(bytes, 4);
				if (type != 0)
				{
					error = fmt::format(
						"FunctionPatcher v3 executable descriptor type {} is unsupported by Cemu",
						type);
					return WupsServiceStatus::Unsupported;
				}
				physical = ReadBe32(bytes, 8);
				effective = ReadBe32(bytes, 12);
				replacement = ReadBe32(bytes, 16);
				callThrough = ReadBe32(bytes, 20);
				process = ReadBe32(bytes, 24);
				nameAddress = ReadBe32(bytes, 28);
				library = ReadBe32(bytes, 32);
			}
			if ((replacement & 3U) != 0 || (callThrough & 3U) != 0 ||
				!m_platform->ValidateGuestRangeForOwner(owner, replacement, 4,
					WupsGuestAccess::Execute) ||
				!m_platform->ValidateGuestRangeForOwner(owner, callThrough, 4,
					WupsGuestAccess::Write))
			{
				error = "FunctionPatcher replacement/call-through pointer has invalid ownership, alignment, or permission";
				return WupsServiceStatus::InvalidArgument;
			}
			if (process != static_cast<std::uint32_t>(WupsPatchProcess::All) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::RootRpx) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::WiiUMenu) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::Tvii) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::EManual) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::HomeMenu) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::ErrorDisplay) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::MiniMiiverse) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::Browser) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::Miiverse) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::Eshop) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::DownloadManager) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::Game) &&
				 process != static_cast<std::uint32_t>(WupsPatchProcess::GameAndMenu))
			{
				error = "FunctionPatcher descriptor has an invalid process target";
				return WupsServiceStatus::InvalidArgument;
			}

			request = {};
			request.owner = {owner.owner, owner.generation};
			request.mandatory = false;
			request.replacementAddress = replacement;
			request.callThroughStorage = callThrough;
			request.process = static_cast<WupsPatchProcess>(process);
			const auto libraryName = WupsPatchLibraryName(library);
			if (libraryName)
			{
				std::string name;
				if (!ReadString(owner, nameAddress, name, error))
					return WupsServiceStatus::InvalidArgument;
				request.targetKind = WupsPatchTargetKind::NamedFunction;
				request.moduleName = *libraryName;
				request.functionName = std::move(name);
				return WupsServiceStatus::Success;
			}
			// LIBRARY_OTHER immediately follows the 66 named public libraries.
			if (library != 66U)
			{
				error = fmt::format("FunctionPatcher library {} is invalid", library);
				return WupsServiceStatus::InvalidArgument;
			}
			if (physical != 0)
			{
				if (!allowPhysicalAddress)
				{
					error = "physical_address_patching permission is required";
					return WupsServiceStatus::PermissionDenied;
				}
				request.targetKind = WupsPatchTargetKind::PhysicalAddress;
				request.physicalAddress = physical;
				request.virtualAddress = effective;
			}
			else if (effective != 0)
			{
				request.targetKind = WupsPatchTargetKind::VirtualAddress;
				request.virtualAddress = effective;
			}
			else
			{
				error = "FunctionPatcher LIBRARY_OTHER descriptor has no address";
				return WupsServiceStatus::InvalidArgument;
			}
			return WupsServiceStatus::Success;
		}

		bool ReadString(WupsOwnerToken owner, std::uint32_t address,
			std::string& value, std::string& error) const
		{
			value.clear();
			if (address == 0)
			{
				error = "FunctionPatcher function name pointer is null";
				return false;
			}
			for (std::size_t index = 0; index <= kMaximumFunctionName; ++index)
			{
				if (address > std::numeric_limits<std::uint32_t>::max() - index ||
					!m_platform->ValidateGuestRangeForOwner(owner,
						address + static_cast<std::uint32_t>(index), 1,
						WupsGuestAccess::Read))
				{
					error = "FunctionPatcher function name leaves readable guest memory";
					return false;
				}
				std::byte byte{};
				if (!m_platform->ReadGuest(
					address + static_cast<std::uint32_t>(index),
					std::span{&byte, 1}))
					return false;
				if (byte == std::byte{}) return !value.empty();
				value.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
			}
			error = "FunctionPatcher function name is not NUL-terminated";
			return false;
		}

		std::uint32_t NextHandle()
		{
			do
			{
				++m_nextHandle;
			} while (m_nextHandle == 0 || m_records.contains(m_nextHandle));
			return m_nextHandle;
		}

		std::shared_ptr<IWupsPlatform> m_platform;
		std::shared_ptr<WupsFunctionPatchManager> m_manager;
		mutable std::mutex m_mutex;
		std::map<std::uint32_t, Record> m_records;
		std::unordered_map<std::uint64_t, std::uint32_t> m_latestGeneration;
		std::uint32_t m_nextHandle{};
	};
}

std::shared_ptr<IWupsFunctionPatcherFacade>
CreateWupsFunctionPatcherFacade(std::shared_ptr<IWupsPlatform> platform,
	std::shared_ptr<WupsFunctionPatchManager> manager)
{
	if (!platform || !manager) return {};
	return std::make_shared<FunctionPatcherFacade>(
		std::move(platform), std::move(manager));
}
