#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsFunctionPatcher.h"

#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/RPL/rpl.h"

#include <atomic>
#include <map>
#include <mutex>

namespace
{
	class CemuWupsPatchPlatform final : public IWupsPatchPlatform
	{
	public:
		~CemuWupsPatchPlatform() override
		{
			std::map<std::uint32_t, std::uint32_t> allocations;
			{
				std::lock_guard lock(m_mutex);
				allocations.swap(m_allocations);
			}
			for (const auto& [address, size] : allocations)
				RPLLoader_ReleaseCodeCaveMem(MEMPTR<void>{address});
		}

		WupsPatchProcess CurrentProcess() const override
		{
			return m_process.load();
		}

		void SetCurrentProcess(WupsPatchProcess process) override
		{
			m_process.store(process);
		}

		std::optional<WupsResolvedPatchTarget> ResolveFunction(
			std::string_view moduleName, std::string_view functionName,
			std::string& error) override
		{
			error.clear();
			RPLResolvedExport resolved;
			if (!RPLLoader_FindLoadedExport(
				moduleName, functionName, false, resolved))
				return std::nullopt;
			return WupsResolvedPatchTarget{
				resolved.address, std::move(resolved.moduleName),
				resolved.lifetimeId};
		}

		std::optional<std::uint32_t> VirtualForPhysical(
			std::uint32_t physicalAddress, std::string& error) override
		{
			error.clear();
			const auto address = memory_physicalToVirtual(physicalAddress);
			if (!memory_isAddressRangeAccessible(address, 4))
			{
				error = fmt::format(
					"physical patch address 0x{:08x} is not mapped",
					physicalAddress);
				return std::nullopt;
			}
			return address;
		}

		bool IsExecutable(std::uint32_t address,
			std::uint32_t size) const override
		{
			RPLMappedAddressInfo info;
			if (RPLLoader_QueryMappedAddress(address, size, info))
				return (info.sectionFlags & 4U) != 0;
			const auto inRange = [address, size](
				std::uint32_t base, std::uint32_t rangeSize) {
				return address >= base && address - base <= rangeSize &&
					size <= rangeSize - (address - base);
			};
			return inRange(MEMORY_CODE_TRAMPOLINE_AREA_ADDR,
				MEMORY_CODE_TRAMPOLINE_AREA_SIZE) ||
				inRange(MEMORY_CODECAVEAREA_ADDR,
					MEMORY_CODECAVEAREA_SIZE);
		}

		bool IsWritable(std::uint32_t address,
			std::uint32_t size) const override
		{
			RPLMappedAddressInfo info;
			return RPLLoader_QueryMappedAddress(address, size, info) &&
				(info.sectionFlags & 1U) != 0;
		}

		bool ReadWords(std::uint32_t address,
			std::span<std::uint32_t> words, std::string& error) override
		{
			const auto bytes = static_cast<std::uint64_t>(words.size()) * 4;
			if (bytes > std::numeric_limits<std::uint32_t>::max() ||
				!memory_isAddressRangeAccessible(
					address, static_cast<std::uint32_t>(bytes)))
			{
				error = fmt::format(
					"guest read [0x{:08x}, +0x{:x}) is inaccessible",
					address, bytes);
				return false;
			}
			for (std::size_t index = 0; index < words.size(); ++index)
				words[index] = memory_readU32(
					address + static_cast<std::uint32_t>(index * 4));
			return true;
		}

		bool WriteWords(std::uint32_t address,
			std::span<const std::uint32_t> words, std::string& error) override
		{
			const auto bytes = static_cast<std::uint64_t>(words.size()) * 4;
			if (bytes > std::numeric_limits<std::uint32_t>::max() ||
				!memory_isAddressRangeAccessible(
					address, static_cast<std::uint32_t>(bytes)))
			{
				error = fmt::format(
					"guest write [0x{:08x}, +0x{:x}) is inaccessible",
					address, bytes);
				return false;
			}
			for (std::size_t index = 0; index < words.size(); ++index)
				memory_writeU32(
					address + static_cast<std::uint32_t>(index * 4),
					words[index]);
			return true;
		}

		bool AllocateExecutableNear(std::uint32_t,
			std::uint32_t size, std::uint32_t alignment,
			std::uint32_t& address, std::string& error) override
		{
			error.clear();
			address = 0;
			if (size == 0 || alignment == 0 ||
				(alignment & (alignment - 1)) != 0 ||
				alignment > 256)
			{
				error = "invalid executable trampoline allocation";
				return false;
			}
			// Patch preflight intentionally runs without the manager mutex so RPL
			// events can re-enter it. Serialize the underlying code-cave heap here;
			// RPLLoader_AllocateCodeCaveMem itself has no loader lock.
			std::lock_guard lock(m_mutex);
			const auto allocation =
				RPLLoader_AllocateCodeCaveMem(alignment, size);
			if (!allocation)
			{
				error = "Cemu code-cave area is exhausted";
				return false;
			}
			address = allocation.GetMPTR();
			if (!m_allocations.emplace(address, size).second)
			{
				RPLLoader_ReleaseCodeCaveMem(allocation);
				address = 0;
				error = "Cemu returned a duplicate trampoline allocation";
				return false;
			}
			return true;
		}

		void FreeExecutable(std::uint32_t address,
			std::uint32_t size) override
		{
			std::lock_guard lock(m_mutex);
			const auto found = m_allocations.find(address);
			if (found != m_allocations.end() && found->second == size)
			{
				m_allocations.erase(found);
				RPLLoader_ReleaseCodeCaveMem(MEMPTR<void>{address});
			}
		}

		void InvalidateCode(std::uint32_t address,
			std::uint32_t size) override
		{
			if (size != 0 && address <=
				std::numeric_limits<std::uint32_t>::max() - size)
				PPCRecompiler_invalidateRange(address, address + size);
		}

	private:
		std::atomic<WupsPatchProcess> m_process{WupsPatchProcess::Game};
		std::mutex m_mutex;
		std::map<std::uint32_t, std::uint32_t> m_allocations;
	};
}

std::shared_ptr<IWupsPatchPlatform> CreateCemuWupsPatchPlatform()
{
	return std::make_shared<CemuWupsPatchPlatform>();
}
