#include "Cafe/HW/Espresso/WupsOwnerScopedHeap.h"

#include <string>
#include <utility>

namespace
{
	[[nodiscard]] WupsOwnedRangeKind RangeKindFor(WupsHeapAllocatorKind kind)
	{
		switch (kind)
		{
		case WupsHeapAllocatorKind::DefaultHeap:
			return WupsOwnedRangeKind::DefaultHeapAllocation;
		case WupsHeapAllocatorKind::ExpHeap:
			return WupsOwnedRangeKind::ExpHeapAllocation;
		case WupsHeapAllocatorKind::UnitHeap:
			return WupsOwnedRangeKind::UnitHeapAllocation;
		case WupsHeapAllocatorKind::AllocatorInterface:
			return WupsOwnedRangeKind::AllocatorAllocation;
		case WupsHeapAllocatorKind::OwnerCreatedHeap:
			return WupsOwnedRangeKind::OwnedHeapBacking;
		}
		return WupsOwnedRangeKind::AllocatorAllocation;
	}

	[[nodiscard]] WupsOwnedGuestRange MakeHeapRange(WupsOwnerToken owner,
													WupsOwnedRangeKind kind, std::uint32_t base, std::uint32_t size)
	{
		WupsOwnedGuestRange range{};
		range.owner = owner;
		range.base = base;
		range.size = size;
		range.kind = kind;
		range.readable = true;
		range.writable = true;
		range.executable = false;
		range.heapEligible = true;
		range.acceptsInteriorPointers = true;
		range.activelyReleasedOnUnload = true;
		return range;
	}
} // namespace

WupsOwnerScopedHeapTracker::WupsOwnerScopedHeapTracker(
	WupsGuestMemoryOwnershipRegistry& registry)
	: m_registry(registry)
{
}

void WupsOwnerScopedHeapTracker::OnAllocation(WupsOwnerToken owner,
											  WupsHeapAllocatorKind kind, std::uint32_t heapHandle, std::uint32_t address,
											  std::uint32_t requestedSize, std::uint32_t trackedSize,
											  std::uint32_t alignment)
{
	(void)heapHandle;
	(void)alignment;
	if (address == 0)
		return;
	// Only the payload the guest may legitimately use is registered; never expose
	// allocator metadata or alignment padding.
	std::uint32_t size = trackedSize != 0 ? trackedSize : requestedSize;
	if (size == 0)
		return;

	const AllocationKey key{owner.owner, owner.generation, address};
	std::lock_guard lock(m_mutex);
	// A stale record at this address (missed free) must not leak a range.
	if (const auto existing = m_allocations.find(key);
		existing != m_allocations.end())
	{
		std::string ignore;
		(void)m_registry.UnregisterRange(existing->second, ignore);
		m_allocations.erase(existing);
	}
	std::string error;
	auto range = MakeHeapRange(owner, RangeKindFor(kind), address, size);
	const auto rangeId = m_registry.RegisterRange(std::move(range), error);
	if (!rangeId)
		return;
	if (!m_registry.CommitRange(*rangeId, error))
	{
		(void)m_registry.UnregisterRange(*rangeId, error);
		return;
	}
	m_allocations.emplace(key, *rangeId);
}

void WupsOwnerScopedHeapTracker::OnFree(WupsOwnerToken owner,
										std::uint32_t address)
{
	if (address == 0)
		return;
	const AllocationKey key{owner.owner, owner.generation, address};
	std::lock_guard lock(m_mutex);
	const auto it = m_allocations.find(key);
	if (it == m_allocations.end())
		return; // untracked / already freed / different owner or generation
	std::string error;
	(void)m_registry.UnregisterRange(it->second, error);
	m_allocations.erase(it);
}

void WupsOwnerScopedHeapTracker::OnResize(WupsOwnerToken owner,
										  std::uint32_t address, std::uint32_t newSize)
{
	if (address == 0 || newSize == 0)
		return;
	const AllocationKey key{owner.owner, owner.generation, address};
	std::lock_guard lock(m_mutex);
	const auto it = m_allocations.find(key);
	if (it == m_allocations.end())
		return;
	const auto current = m_registry.GetRange(it->second);
	if (!current)
		return;
	// Registry ranges are immutable in size; re-register at the same base. The
	// old range stays live until the replacement commits so provenance is never
	// lost mid-resize.
	std::string error;
	auto replacement = MakeHeapRange(owner, current->kind, address, newSize);
	const auto newId = m_registry.RegisterRange(std::move(replacement), error);
	if (!newId)
		return; // keep the previous range on failure
	if (!m_registry.CommitRange(*newId, error))
	{
		(void)m_registry.UnregisterRange(*newId, error);
		return;
	}
	(void)m_registry.UnregisterRange(it->second, error);
	it->second = *newId;
}

void WupsOwnerScopedHeapTracker::RegisterOwnedHeap(WupsOwnerToken owner,
												   std::uint32_t heapHandle, std::uint32_t backingBase,
												   std::uint32_t backingSize, WupsHeapAllocatorKind kind)
{
	(void)kind;
	if (backingBase == 0 || backingSize == 0)
		return;
	const HeapKey key{owner.owner, owner.generation, heapHandle};
	std::lock_guard lock(m_mutex);
	if (m_ownedHeaps.contains(key))
		return;
	std::string error;
	auto range = MakeHeapRange(owner, WupsOwnedRangeKind::OwnedHeapBacking,
							   backingBase, backingSize);
	const auto rangeId = m_registry.RegisterRange(std::move(range), error);
	if (!rangeId)
		return;
	if (!m_registry.CommitRange(*rangeId, error))
	{
		(void)m_registry.UnregisterRange(*rangeId, error);
		return;
	}
	m_ownedHeaps.emplace(key, *rangeId);
}

void WupsOwnerScopedHeapTracker::UnregisterOwnedHeap(WupsOwnerToken owner,
													 std::uint32_t heapHandle)
{
	const HeapKey key{owner.owner, owner.generation, heapHandle};
	std::lock_guard lock(m_mutex);
	const auto it = m_ownedHeaps.find(key);
	if (it == m_ownedHeaps.end())
		return;
	std::string error;
	(void)m_registry.UnregisterRange(it->second, error);
	m_ownedHeaps.erase(it);
}

void WupsOwnerScopedHeapTracker::ReleaseOwner(WupsOwnerToken owner)
{
	std::lock_guard lock(m_mutex);
	// The registry reclaims the ranges themselves via ReleaseOwner(); here we only
	// drop this owner+generation's bookkeeping so a reused owner id starts clean.
	std::erase_if(m_allocations, [&](const auto& entry) {
		return entry.first.owner == owner.owner &&
			   entry.first.generation == owner.generation;
	});
	std::erase_if(m_ownedHeaps, [&](const auto& entry) {
		return entry.first.owner == owner.owner &&
			   entry.first.generation == owner.generation;
	});
}

std::size_t WupsOwnerScopedHeapTracker::OwnerAllocationCount(
	WupsOwnerToken owner) const
{
	std::lock_guard lock(m_mutex);
	std::size_t count = 0;
	for (const auto& [key, rangeId] : m_allocations)
		if (key.owner == owner.owner && key.generation == owner.generation)
			++count;
	for (const auto& [key, rangeId] : m_ownedHeaps)
		if (key.owner == owner.owner && key.generation == owner.generation)
			++count;
	return count;
}
