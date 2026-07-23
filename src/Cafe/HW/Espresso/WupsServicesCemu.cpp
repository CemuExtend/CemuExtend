#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsServices.h"

#include "Cafe/HW/Espresso/ModExecutionContext.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cemu/Logging/CemuLogging.h"

#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>

namespace
{
	constexpr std::uint32_t kMaximumDataAllocation = 1024U * 1024U;
	constexpr std::uint32_t kMaximumAlignment = 256;
	constexpr std::size_t kMaximumAbiWords = 32;
	constexpr std::uint32_t kCallableStubSize = 4;

	[[nodiscard]] bool IsPowerOfTwo(std::uint32_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	[[nodiscard]] bool Contains(std::uint32_t base, std::uint32_t rangeSize,
		std::uint32_t address, std::uint32_t size)
	{
		return size != 0 && address >= base &&
			address - base <= rangeSize &&
			size <= rangeSize - (address - base);
	}

	struct CallableRecord
	{
		WupsOwnerToken owner;
		WupsHostExportHandler handler;
		std::mutex mutex;
		std::condition_variable idle;
		std::size_t executing{};
		bool revoked{};
	};

	std::mutex s_callableMutex;
	std::unordered_map<std::uint32_t, std::shared_ptr<CallableRecord>>
		s_callableRecords;
	std::once_flag s_dispatcherOnce;
	std::uint16_t s_dispatcherHle{};
	thread_local std::optional<WupsOwnerToken> s_dispatchOwner;

	void CemuWupsExportDispatcher(PPCInterpreter_t* cpu)
	{
		std::shared_ptr<CallableRecord> record;
		{
			std::lock_guard lock(s_callableMutex);
			const auto found = s_callableRecords.find(cpu->instructionPointer);
			if (found != s_callableRecords.end())
				record = found->second;
		}
		if (!record)
		{
			cemuLog_log(LogType::Force,
				fmt::format("WUPS: rejected unknown/revoked host export stub 0x{:08x}",
					cpu->instructionPointer));
			osLib_returnFromFunction(cpu,
				static_cast<std::uint32_t>(static_cast<std::int32_t>(
					WupsServiceStatus::StaleGeneration)));
			return;
		}

		{
			std::lock_guard lock(record->mutex);
			if (record->revoked)
			{
				osLib_returnFromFunction(cpu,
					static_cast<std::uint32_t>(static_cast<std::int32_t>(
						WupsServiceStatus::StaleGeneration)));
				return;
			}
			if (cpu->modExecutionContext &&
				(cpu->modExecutionContext->AddressSpaceId() != record->owner.owner ||
					cpu->modExecutionContext->Generation() != record->owner.generation))
			{
				osLib_returnFromFunction(cpu,
					static_cast<std::uint32_t>(static_cast<std::int32_t>(
						WupsServiceStatus::OwnerMismatch)));
				return;
			}
			++record->executing;
		}

		std::array<std::uint32_t, kMaximumAbiWords> arguments{};
		for (std::size_t index = 0; index < 8; ++index)
			arguments[index] = cpu->gpr[3 + index];
		const auto stack = cpu->gpr[1];
		std::size_t argumentCount = 8;
		if ((stack & 0xfU) == 0 &&
			stack <= std::numeric_limits<std::uint32_t>::max() -
				static_cast<std::uint32_t>(kMaximumAbiWords * 4) &&
			memory_isAddressRangeAccessible(stack + 8,
				static_cast<std::uint32_t>((kMaximumAbiWords - 8) * 4)))
		{
			argumentCount = arguments.size();
			for (std::size_t index = 8; index < arguments.size(); ++index)
				arguments[index] = memory_readU32(
					stack + 8 + static_cast<std::uint32_t>((index - 8) * 4));
		}

		std::string error;
		std::int32_t result = static_cast<std::int32_t>(
			WupsServiceStatus::InvalidArgument);
		const auto previousOwner = s_dispatchOwner;
		s_dispatchOwner = record->owner;
		try
		{
			result = record->handler(
				std::span<const std::uint32_t>(arguments).first(argumentCount), error);
		}
		catch (...)
		{
			error = "WUPS host export threw an unexpected host exception";
			result = static_cast<std::int32_t>(WupsServiceStatus::InternalError);
		}
		s_dispatchOwner = previousOwner;
		if (!error.empty())
			cemuLog_log(LogType::Force,
				fmt::format("WUPS owner {} generation {} export failed: {}",
					record->owner.owner, record->owner.generation, error));

		{
			std::lock_guard lock(record->mutex);
			cemu_assert_debug(record->executing != 0);
			if (record->executing != 0)
				--record->executing;
			record->idle.notify_all();
		}
		osLib_returnFromFunction(cpu, static_cast<std::uint32_t>(result));
	}

	[[nodiscard]] std::uint16_t DispatcherHle()
	{
		std::call_once(s_dispatcherOnce, [] {
			const auto id = PPCInterpreter_registerHLECall(
				CemuWupsExportDispatcher, "CemuWupsOwnerExportDispatcher");
			cemu_assert(id >= 0 && id <= std::numeric_limits<std::uint16_t>::max());
			s_dispatcherHle = static_cast<std::uint16_t>(id);
		});
		return s_dispatcherHle;
	}

	class CemuWupsPlatform final : public IWupsPlatform
	{
	public:
		~CemuWupsPlatform() override
		{
			std::vector<WupsOwnerToken> owners;
			{
				std::lock_guard lock(m_mutex);
				for (const auto& [address, allocation] : m_allocations)
					if (allocation.kind == AllocationKind::Callable &&
						std::ranges::find(owners, allocation.owner) == owners.end())
						owners.push_back(allocation.owner);
			}
			for (const auto owner : owners)
				ReleaseOwnerExports(owner);
			std::vector<std::uint32_t> allocations;
			{
				std::lock_guard lock(m_mutex);
				for (const auto& [address, allocation] : m_allocations)
					if (allocation.kind == AllocationKind::Data)
						allocations.push_back(address);
			}
			for (const auto address : allocations)
			{
				std::optional<WupsOwnerToken> owner;
				{
					std::lock_guard lock(m_mutex);
					const auto found = m_allocations.find(address);
					if (found != m_allocations.end()) owner = found->second.owner;
				}
				if (owner) FreeGuestData(*owner, address);
			}
		}

		bool ValidateGuestRange(std::uint32_t address, std::uint32_t size,
			WupsGuestAccess access) const override
		{
			return ValidateGuestRangeForOwner(
				s_dispatchOwner.value_or(WupsOwnerToken{}), address, size, access);
		}

		bool ValidateGuestRangeForOwner(WupsOwnerToken scopedOwner,
			std::uint32_t address, std::uint32_t size,
			WupsGuestAccess access) const override
		{
			const auto hasScopedOwner = scopedOwner.owner != 0 &&
				scopedOwner.generation != 0;
			if (address == 0 || size == 0 ||
				address > std::numeric_limits<std::uint32_t>::max() - (size - 1) ||
				(access != WupsGuestAccess::Execute &&
					!memory_isAddressRangeAccessible(address, size)))
				return false;
			{
				std::lock_guard lock(m_mutex);
				for (const auto& [base, allocation] : m_allocations)
				{
					if (!Contains(base, allocation.size, address, size)) continue;
					if (hasScopedOwner && allocation.owner != scopedOwner)
						return false;
					return access == WupsGuestAccess::Execute ?
						allocation.kind == AllocationKind::Callable :
						allocation.kind == AllocationKind::Data;
				}
			}
			if (hasScopedOwner && access != WupsGuestAccess::Execute)
			{
				if (const auto* thread = coreinit::OSGetCurrentThread())
				{
					const auto lower = thread->stackEnd.GetMPTR();
					const auto upper = thread->stackBase.GetMPTR();
					if (upper > lower && Contains(
						lower, upper - lower, address, size))
						return true;
				}
			}
			RPLMappedAddressInfo info;
			if (!RPLLoader_QueryMappedAddress(address, size, info)) return false;
			if (hasScopedOwner && info.external &&
				(info.owner != scopedOwner.owner ||
					info.generation != scopedOwner.generation))
				return false;
			if (access == WupsGuestAccess::Execute)
				return (info.sectionFlags & 4U) != 0;
			if (access == WupsGuestAccess::Write)
				return (info.sectionFlags & 1U) != 0;
			return (info.sectionFlags & 2U) != 0;
		}

		bool ReadGuest(std::uint32_t address,
			std::span<std::byte> output) const override
		{
			if (output.empty() || output.size() >
				std::numeric_limits<std::uint32_t>::max() ||
				!ValidateGuestRange(address, static_cast<std::uint32_t>(output.size()),
					WupsGuestAccess::Read))
				return false;
			std::memcpy(output.data(), memory_getPointerFromVirtualOffset(address),
				output.size());
			return true;
		}

		bool WriteGuest(std::uint32_t address,
			std::span<const std::byte> input) override
		{
			if (input.empty() || input.size() >
				std::numeric_limits<std::uint32_t>::max() ||
				!ValidateGuestRange(address, static_cast<std::uint32_t>(input.size()),
					WupsGuestAccess::Write))
				return false;
			std::memcpy(memory_getPointerFromVirtualOffset(address), input.data(),
				input.size());
			return true;
		}

		std::optional<std::uint32_t> AllocateGuestData(WupsOwnerToken owner,
			std::uint32_t size, std::uint32_t alignment,
			std::string& error) override
		{
			error.clear();
			if (!PPCInterpreter_getCurrentInstance())
			{
				error = "guest data allocation requires an emulated CPU thread";
				return std::nullopt;
			}
			if (!ValidOwner(owner, error) || size == 0 ||
				size > kMaximumDataAllocation || !IsPowerOfTwo(alignment) ||
				alignment > kMaximumAlignment)
			{
				if (error.empty()) error = "invalid WUPS guest data allocation";
				return std::nullopt;
			}
			const auto memory = RPLLoader_AllocateCodeCaveMem(alignment, size);
			if (!memory)
			{
				error = "Cemu guest allocation area is exhausted";
				return std::nullopt;
			}
			const auto address = memory.GetMPTR();
			{
				std::lock_guard lock(m_mutex);
				if (!m_allocations.emplace(address,
					Allocation{owner, size, AllocationKind::Data}).second)
				{
					RPLLoader_ReleaseCodeCaveMem(memory);
					error = "Cemu returned a duplicate guest allocation";
					return std::nullopt;
				}
			}
			std::memset(memory_getPointerFromVirtualOffset(address), 0, size);
			return address;
		}

		void FreeGuestData(WupsOwnerToken owner, std::uint32_t address) override
		{
			bool release{};
			{
				std::lock_guard lock(m_mutex);
				const auto found = m_allocations.find(address);
				if (found != m_allocations.end() && found->second.owner == owner &&
					found->second.kind == AllocationKind::Data)
				{
					m_allocations.erase(found);
					release = true;
				}
			}
			if (release) RPLLoader_ReleaseCodeCaveMem(MEMPTR<void>{address});
		}

		std::optional<std::uint32_t> RegisterFunction(WupsOwnerToken owner,
			std::string_view moduleName, std::string_view symbolName,
			WupsHostExportHandler handler, std::string& error) override
		{
			error.clear();
			if (!PPCInterpreter_getCurrentInstance())
			{
				error = "WUPS export registration requires an emulated CPU thread";
				return std::nullopt;
			}
			if (!ValidOwner(owner, error) || moduleName.empty() || symbolName.empty() ||
				!handler)
			{
				if (error.empty()) error = "invalid WUPS host export registration";
				return std::nullopt;
			}
			const auto memory = RPLLoader_AllocateCodeCaveMem(4, kCallableStubSize);
			if (!memory)
			{
				error = "Cemu code-cave area is exhausted";
				return std::nullopt;
			}
			const auto address = memory.GetMPTR();
			const auto hle = DispatcherHle();
			memory_writeU32(address, (1U << 26) | hle);
			PPCRecompiler_invalidateRange(address, address + kCallableStubSize);
			auto record = std::make_shared<CallableRecord>();
			record->owner = owner;
			record->handler = std::move(handler);
			{
				std::scoped_lock lock(m_mutex, s_callableMutex);
				if (!m_allocations.emplace(address,
					Allocation{owner, kCallableStubSize,
						AllocationKind::Callable}).second ||
					!s_callableRecords.emplace(address, record).second)
				{
					m_allocations.erase(address);
					s_callableRecords.erase(address);
					RPLLoader_ReleaseCodeCaveMem(memory);
					error = "Cemu returned a duplicate WUPS callable address";
					return std::nullopt;
				}
			}
			if (auto* cpu = PPCInterpreter_getCurrentInstance();
				cpu->modExecutionContext &&
				cpu->modExecutionContext->AddressSpaceId() == owner.owner &&
				cpu->modExecutionContext->Generation() == owner.generation)
				cpu->modExecutionContext->AllowHle(hle);
			return address;
		}

		void ReleaseOwnerExports(WupsOwnerToken owner) override
		{
			std::vector<std::pair<std::uint32_t,
				std::shared_ptr<CallableRecord>>> records;
			{
				std::scoped_lock lock(m_mutex, s_callableMutex);
				for (auto iterator = m_allocations.begin();
					iterator != m_allocations.end();)
				{
					if (iterator->second.owner != owner ||
						iterator->second.kind != AllocationKind::Callable)
					{
						++iterator;
						continue;
					}
					const auto found = s_callableRecords.find(iterator->first);
					if (found != s_callableRecords.end())
					{
						records.emplace_back(iterator->first, found->second);
						s_callableRecords.erase(found);
					}
					iterator = m_allocations.erase(iterator);
				}
			}
			for (auto& [address, record] : records)
			{
				std::unique_lock lock(record->mutex);
				record->revoked = true;
				record->idle.wait(lock, [&] { return record->executing == 0; });
				lock.unlock();
				PPCRecompiler_invalidateRange(address,
					address + kCallableStubSize);
				RPLLoader_ReleaseCodeCaveMem(MEMPTR<void>{address});
			}
		}

		std::uint64_t CurrentGuestThreadId() const override
		{
			const auto* cpu = PPCInterpreter_getCurrentInstance();
			if (!cpu) return 0;
			auto* thread = coreinit::OSGetCurrentThread();
			return thread ? static_cast<std::uint64_t>(
				memory_getVirtualOffsetFromPointer(thread)) : 0;
		}

		bool QueueCpuTask(WupsOwnerToken owner, std::function<void()> task,
			std::string& error) override
		{
			error.clear();
			if (!ValidOwner(owner, error)) return false;
			if (!PPCInterpreter_getCurrentInstance())
			{
				error = "Cemu has no cross-thread WUPS CPU task queue; callback rejected";
				return false;
			}
			if (!task)
			{
				error = "WUPS CPU task is empty";
				return false;
			}
			task();
			return true;
		}

		void CancelCpuTasks(WupsOwnerToken) override
		{
			// QueueCpuTask is deliberately synchronous, so there can be no queued
			// callable or captured RAII state when this method returns.
		}

		bool SupportsMappedMemory() const override { return false; }

		std::optional<WupsMappedMemoryInfo> AllocateMappedMemory(
			WupsOwnerToken, std::uint32_t, std::uint32_t, bool,
			WupsMappedMemoryPurpose, std::string& error) override
		{
			error = "Cemu mapped-memory effective/physical allocator is unavailable";
			return std::nullopt;
		}

		bool FreeMappedMemory(WupsOwnerToken,
			const WupsMappedMemoryInfo&, std::string& error) override
		{
			error = "Cemu mapped-memory allocator is unavailable";
			return false;
		}

		void ShowNotification(WupsOwnerToken owner,
			const WupsNotificationModel& notification) override
		{
			cemuLog_log(LogType::Force,
				fmt::format("WUPS notification owner {} generation {}: {}",
					owner.owner, owner.generation, notification.text));
		}

		void Log(WupsOwnerToken owner, WupsLogLevel level,
			std::string_view moduleName, std::string_view source,
			std::string_view message) override
		{
			static constexpr std::array levels{"error", "warning", "info", "verbose"};
			const auto index = std::min<std::size_t>(static_cast<std::size_t>(level),
				levels.size() - 1);
			cemuLog_log(LogType::Force,
				fmt::format("WUPS [{}] owner {}:{} {} {}: {}", levels[index],
					owner.owner, owner.generation, moduleName, source, message));
		}

	private:
		enum class AllocationKind : std::uint8_t { Data, Callable };
		struct Allocation
		{
			WupsOwnerToken owner;
			std::uint32_t size{};
			AllocationKind kind{};
		};

		bool ValidOwner(WupsOwnerToken owner, std::string& error) const
		{
			if (owner.owner == 0 || owner.generation == 0)
			{
				error = "WUPS owner token is invalid";
				return false;
			}
			std::lock_guard lock(m_mutex);
			auto [found, inserted] = m_latestGeneration.emplace(
				owner.owner, owner.generation);
			if (!inserted && found->second != owner.generation)
			{
				if (owner.generation > found->second)
				{
					const bool hasOldAllocations = std::ranges::any_of(
						m_allocations, [&](const auto& entry) {
							return entry.second.owner.owner == owner.owner;
						});
					if (hasOldAllocations)
					{
						error = "WUPS owner has live resources from an older generation";
						return false;
					}
					found->second = owner.generation;
				}
				else
				{
					error = "WUPS owner generation is stale";
					return false;
				}
			}
			return true;
		}

		mutable std::mutex m_mutex;
		mutable std::unordered_map<std::uint64_t, std::uint32_t>
			m_latestGeneration;
		std::map<std::uint32_t, Allocation> m_allocations;
	};
}

std::shared_ptr<IWupsPlatform> CreateCemuWupsPlatform()
{
	return std::make_shared<CemuWupsPlatform>();
}
