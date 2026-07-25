#include "Cafe/HW/Espresso/WupsHeapInterception.h"

#include "Cafe/HW/Espresso/WupsServices.h"

#include <mutex>

namespace cafe::wups
{
namespace
{
std::mutex g_mutex;
std::shared_ptr<WupsOwnerScopedHeapTracker> g_tracker;

[[nodiscard]] std::shared_ptr<WupsOwnerScopedHeapTracker> ActiveTrackerForScope(
	WupsOwnerToken& owner)
{
	const auto scoped = WupsGuestOwnerScope::Current();
	if (!scoped || scoped->owner == 0 || scoped->generation == 0)
		return nullptr;
	std::lock_guard lock(g_mutex);
	if (!g_tracker)
		return nullptr;
	owner = *scoped;
	return g_tracker;
}
} // namespace

void SetActiveHeapTracker(std::shared_ptr<WupsOwnerScopedHeapTracker> tracker)
{
	std::lock_guard lock(g_mutex);
	g_tracker = std::move(tracker);
}

void NotifyHeapAllocation(WupsHeapAllocatorKind kind, std::uint32_t heapHandle,
	std::uint32_t address, std::uint32_t requestedSize,
	std::uint32_t trackedSize, std::uint32_t alignment)
{
	WupsOwnerToken owner{};
	if (auto tracker = ActiveTrackerForScope(owner))
		tracker->OnAllocation(owner, kind, heapHandle, address, requestedSize,
			trackedSize, alignment);
}

void NotifyHeapFree(std::uint32_t address)
{
	WupsOwnerToken owner{};
	if (auto tracker = ActiveTrackerForScope(owner))
		tracker->OnFree(owner, address);
}

void NotifyHeapResize(std::uint32_t address, std::uint32_t newSize)
{
	WupsOwnerToken owner{};
	if (auto tracker = ActiveTrackerForScope(owner))
		tracker->OnResize(owner, address, newSize);
}
} // namespace cafe::wups
