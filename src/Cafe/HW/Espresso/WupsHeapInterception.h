#pragma once

#include "Cafe/HW/Espresso/WupsOwnerScopedHeap.h"

#include <cstdint>
#include <memory>

// Thin decoupling bridge between Cemu's coreinit heap HLE chokepoints and the
// active title's WUPS owner-scoped heap tracker. coreinit_MEM calls the Notify*
// hooks unconditionally; they are no-ops unless the calling guest thread is
// currently executing inside a WupsGuestOwnerScope (i.e. plugin code invoked by
// the runtime) and a production platform has installed a tracker. This keeps
// coreinit ignorant of the WUPS runtime while still attributing the heap
// backings a plugin obtains during its initialisation hooks.
namespace cafe::wups
{
	// Installed by the production CemuWupsPlatform for the lifetime of a title; pass
	// nullptr on teardown.
	void SetActiveHeapTracker(std::shared_ptr<WupsOwnerScopedHeapTracker> tracker);

	void NotifyHeapAllocation(WupsHeapAllocatorKind kind, std::uint32_t heapHandle,
							  std::uint32_t address, std::uint32_t requestedSize,
							  std::uint32_t trackedSize, std::uint32_t alignment);
	void NotifyHeapFree(std::uint32_t address);
	void NotifyHeapResize(std::uint32_t address, std::uint32_t newSize);
} // namespace cafe::wups
