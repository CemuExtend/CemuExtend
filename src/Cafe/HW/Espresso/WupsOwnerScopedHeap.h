#pragma once

#include "Cafe/HW/Espresso/WupsGuestMemoryOwnership.h"
#include "Cafe/HW/Espresso/WupsServices.h"

#include <cstdint>
#include <map>
#include <mutex>

// Tracks guest heap activity performed by a WUPS owner so that heap pointers the
// plugin later hands to heap-taking ABIs can be attributed to it. Because a WUT
// plugin sub-allocates from its own guest-side allocator, Cemu cannot observe
// every malloc; instead the tracker records the heap *backings* the plugin
// obtains while executing inside a WupsGuestOwnerScope, plus the coarse HLE-level
// allocations it can see, and registers each as an owned range. Interior
// pointers inside a registered backing then carry provable owner provenance.
//
// All ranges are registered through the shared WupsGuestMemoryOwnershipRegistry,
// so validation, isolation and unload cleanup stay unified with RPL/TLS/mapped
// memory.

enum class WupsHeapAllocatorKind : std::uint8_t
{
	DefaultHeap,
	ExpHeap,
	UnitHeap,
	AllocatorInterface,
	OwnerCreatedHeap,
};

class WupsOwnerScopedHeapTracker
{
public:
	explicit WupsOwnerScopedHeapTracker(
		WupsGuestMemoryOwnershipRegistry& registry);

	// A single HLE-visible allocation succeeded for `owner`. `trackedSize` is the
	// allocator-managed block size when known, otherwise the requested size; only
	// the payload range is registered.
	void OnAllocation(WupsOwnerToken owner, WupsHeapAllocatorKind kind,
		std::uint32_t heapHandle, std::uint32_t address,
		std::uint32_t requestedSize, std::uint32_t trackedSize,
		std::uint32_t alignment);

	// The allocation at `address` was freed. Only an exact base match owned by
	// `owner` is honoured.
	void OnFree(WupsOwnerToken owner, std::uint32_t address);

	// The allocation at `address` was resized in place to `newSize`.
	void OnResize(WupsOwnerToken owner, std::uint32_t address,
		std::uint32_t newSize);

	// The plugin created its own heap on memory it owns; register the backing so
	// interior pointers are attributable without per-allocation tracking.
	void RegisterOwnedHeap(WupsOwnerToken owner, std::uint32_t heapHandle,
		std::uint32_t backingBase, std::uint32_t backingSize,
		WupsHeapAllocatorKind kind);

	void UnregisterOwnedHeap(WupsOwnerToken owner, std::uint32_t heapHandle);

	// Drops all bookkeeping for `owner`; the registry ranges themselves are
	// reclaimed by WupsGuestMemoryOwnershipRegistry::ReleaseOwner. Idempotent.
	void ReleaseOwner(WupsOwnerToken owner);

	[[nodiscard]] std::size_t OwnerAllocationCount(WupsOwnerToken owner) const;

private:
	struct AllocationKey
	{
		std::uint64_t owner{};
		std::uint32_t generation{};
		std::uint32_t address{};
		[[nodiscard]] friend bool operator<(const AllocationKey& a,
			const AllocationKey& b)
		{
			return std::tie(a.owner, a.generation, a.address) <
				std::tie(b.owner, b.generation, b.address);
		}
	};
	struct HeapKey
	{
		std::uint64_t owner{};
		std::uint32_t generation{};
		std::uint32_t heapHandle{};
		[[nodiscard]] friend bool operator<(const HeapKey& a, const HeapKey& b)
		{
			return std::tie(a.owner, a.generation, a.heapHandle) <
				std::tie(b.owner, b.generation, b.heapHandle);
		}
	};

	WupsGuestMemoryOwnershipRegistry& m_registry;
	mutable std::mutex m_mutex;
	std::map<AllocationKey, std::uint64_t> m_allocations; // -> rangeId
	std::map<HeapKey, std::uint64_t> m_ownedHeaps;        // -> rangeId
};
