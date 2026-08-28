#pragma once

#include "Cafe/HW/Espresso/WupsOwnerScopedHeap.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

// A dedicated guest ExpHeap that every WUPS plugin's redirected libc/coreinit
// default-heap allocation (see the "coreinit"."MEMAllocFromDefaultHeap"(Ex)/
// "MEMFreeToDefaultHeap" data-import redirection in WupsServices.cpp) is
// serviced from, instead of the game's own default heap.
//
// Background: a WUT plugin loaded as an external RPL imports these coreinit
// data exports the same way any Cafe RPL does. Left alone, that resolves to
// the *game*'s gCoreinitData->MEMAllocFromDefaultHeap* function-pointer
// slots (see coreinit.cpp's osLib_addVirtualPointer registrations) - i.e.
// plugin libc allocations end up inside the game's own dlmalloc/newlib heap.
// On real hardware an Aroma plugin never shares the game's heap at all,
// because it isn't looked up through the game's own coreinit data exports in
// the first place; some titles' own heap locking (taken around their private
// allocator) can then be re-entered by an unrelated plugin allocation via
// Cemu's coreinit HLE plumbing, which is a hazard that structurally cannot
// occur on console.
//
// The fix is to give WUPS plugins their own backing memory - carved out of
// the *game* heap exactly once per title, lazily, from a clean PPC thread
// context with no guest lock held (see CemodRuntime::OnApplicationStarts /
// WupsPayloadRuntime::OnApplicationStarts) - and a separate, host-driven
// ExpHeap over that backing. Allocation and free never call back into guest
// code, so there is no reentrancy hazard through this path.
class WupsPluginHeap
{
  public:
	static constexpr std::uint32_t kBackingSize = 32U * 1024U * 1024U; // 32 MiB
	static constexpr std::uint32_t kBackingAlignment = 0x40;
	static constexpr std::int32_t kDefaultAlignment = 0x40;

	explicit WupsPluginHeap(
		std::shared_ptr<WupsOwnerScopedHeapTracker> heapTracker);
	~WupsPluginHeap();

	WupsPluginHeap(const WupsPluginHeap&) = delete;
	WupsPluginHeap& operator=(const WupsPluginHeap&) = delete;

	// Carves the backing out of the game's default heap via
	// coreinit::_weak_MEMAllocFromDefaultHeapEx and builds a host-managed
	// ExpHeap over it. Must be called once per title from a clean emulated PPC
	// thread context (no guest allocator lock held) - CemodRuntime calls this
	// at the very start of OnApplicationStarts, before any plugin init hook
	// runs. Idempotent: only the first call has an effect; subsequent calls
	// return the cached result. On failure the heap stays permanently
	// disabled for the rest of the title and every Alloc/AllocEx call fails.
	bool EnsureInitialized(std::string& error);

	[[nodiscard]] bool IsUsable() const;

	// True once EnsureInitialized() has run for the current title, whether it
	// succeeded or failed. Reset() clears it again. Used to tell "this title
	// never needed the plugin heap" apart from "the reservation was tried".
	[[nodiscard]] bool WasInitializationAttempted() const;

	// Allocates from the plugin ExpHeap on behalf of `owner`. Never calls back
	// into guest code. Returns 0 on failure (heap not usable, OOM, or bad
	// arguments).
	[[nodiscard]] std::uint32_t Alloc(WupsOwnerToken owner, std::uint32_t size);
	// `alignment` mirrors MEMAllocFromExpHeapEx's own signed alignment ABI
	// (negative requests a tail allocation).
	[[nodiscard]] std::uint32_t AllocEx(WupsOwnerToken owner, std::uint32_t size,
										std::int32_t alignment);
	// Frees `address` on behalf of `owner`. If `address` does not fall inside
	// this heap's backing range the call is rejected and logged instead of
	// touching the game's own heap - a plugin (or a bug in this redirection)
	// must never be able to corrupt memory it doesn't own.
	void Free(WupsOwnerToken owner, std::uint32_t address);

	// Drops the current backing/heap bookkeeping without touching the game
	// heap (the title that owned that backing is gone by the time this runs,
	// so there is nothing safe to free it back to). Called from title
	// teardown; the next title lazily re-creates everything via
	// EnsureInitialized.
	void Reset();

  private:
	bool CreateBackingLocked(std::string& error);

	mutable std::mutex m_mutex;
	std::shared_ptr<WupsOwnerScopedHeapTracker> m_heapTracker;
	std::uint32_t m_backingBase{};
	std::uint32_t m_backingSize{};
	void* m_heapHandle{}; // coreinit::MEMHeapHandle (host pointer into guest memory)
	bool m_attempted{};
	bool m_usable{};
};

// Thin decoupling bridge, mirroring WupsHeapInterception: the production
// CemuWupsPlatform installs the per-title plugin heap instance here so that
// WupsServices.cpp's coreinit data-import redirection and CemodRuntime's
// OnApplicationStarts hook can reach it without a direct dependency on the
// Cemu adapter's internals.
namespace cafe::wups
{
	void SetActivePluginHeap(std::shared_ptr<WupsPluginHeap> heap);
	std::shared_ptr<WupsPluginHeap> ActivePluginHeap();
} // namespace cafe::wups
