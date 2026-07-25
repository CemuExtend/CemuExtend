#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsPluginHeap.h"

#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.h"
#include "Cemu/Logging/CemuLogging.h"

#include <utility>

namespace
{
// Arbitrary stable key used only for the heap tracker's own bookkeeping maps;
// this heap never registers its backing as a single owned range (see the
// comment in WupsPluginHeap::CreateBackingLocked), so the value only needs to
// be distinguishable from the handles other allocator kinds use.
constexpr std::uint32_t kTrackerHeapHandle = 0;
} // namespace

WupsPluginHeap::WupsPluginHeap(
	std::shared_ptr<WupsOwnerScopedHeapTracker> heapTracker)
	: m_heapTracker(std::move(heapTracker))
{
}

WupsPluginHeap::~WupsPluginHeap() = default;

bool WupsPluginHeap::CreateBackingLocked(std::string& error)
{
	if (!PPCInterpreter_getCurrentInstance())
	{
		error = "WUPS plugin heap backing allocation requires an emulated CPU thread";
		return false;
	}
	// This is the one and only guest reentry this component performs: a
	// single, title-lifetime reservation from the game's own default heap,
	// executed from a clean call site (CemodRuntime::OnApplicationStarts)
	// where no guest allocator lock can possibly be held. Every allocation
	// serviced from the resulting backing is handled entirely on the host
	// side afterwards and never touches guest code again.
	void* backing = coreinit::_weak_MEMAllocFromDefaultHeapEx(
		kBackingSize, static_cast<std::int32_t>(kBackingAlignment));
	if (!backing)
	{
		error = "failed to reserve the WUPS plugin heap backing from the "
			"game's default heap";
		return false;
	}
	const auto backingAddress = memory_getVirtualOffsetFromPointer(backing);
	auto* heapHandle = coreinit::MEMCreateExpHeapEx(backing, kBackingSize, 0);
	if (!heapHandle)
	{
		// The 32 MiB backing is leaked from the game heap's point of view in
		// this (expected to be exceedingly rare) failure case. Freeing it back
		// would require re-entering the game's allocator a second time, which
		// is exactly the hazard this component exists to avoid, so leaking it
		// is the safer choice.
		error = "failed to create the WUPS plugin ExpHeap over its backing";
		return false;
	}
	m_backingBase = backingAddress;
	m_backingSize = kBackingSize;
	m_heapHandle = heapHandle;
	m_usable = true;
	cemuLog_log(LogType::Force,
		"WUPS: plugin heap backing allocated at 0x{:08x}, size 0x{:x}",
		backingAddress, kBackingSize);
	return true;
}

bool WupsPluginHeap::EnsureInitialized(std::string& error)
{
	std::lock_guard lock(m_mutex);
	if (m_usable)
	{
		error.clear();
		return true;
	}
	if (m_attempted)
	{
		error = "WUPS plugin heap failed to initialize earlier and stays "
			"disabled for this title";
		return false;
	}
	m_attempted = true;
	return CreateBackingLocked(error);
}

bool WupsPluginHeap::IsUsable() const
{
	std::lock_guard lock(m_mutex);
	return m_usable;
}

std::uint32_t WupsPluginHeap::Alloc(WupsOwnerToken owner, std::uint32_t size)
{
	return AllocEx(owner, size, kDefaultAlignment);
}

std::uint32_t WupsPluginHeap::AllocEx(WupsOwnerToken owner, std::uint32_t size,
	std::int32_t alignment)
{
	std::lock_guard lock(m_mutex);
	if (!m_usable || size == 0)
		return 0;
	auto* handle = static_cast<coreinit::MEMHeapHandle>(m_heapHandle);
	void* mem = coreinit::MEMAllocFromExpHeapEx(handle, size, alignment);
	if (!mem)
		return 0;
	const auto address = memory_getVirtualOffsetFromPointer(mem);
	if (m_heapTracker)
	{
		const auto trackedAlignment = static_cast<std::uint32_t>(
			alignment < 0 ? -alignment : alignment);
		m_heapTracker->OnAllocation(owner, WupsHeapAllocatorKind::ExpHeap,
			kTrackerHeapHandle, address, size, size, trackedAlignment);
	}
	return address;
}

void WupsPluginHeap::Free(WupsOwnerToken owner, std::uint32_t address)
{
	std::lock_guard lock(m_mutex);
	if (!m_usable || address == 0)
		return;
	if (address < m_backingBase ||
		address - m_backingBase >= m_backingSize)
	{
		// A plugin ABI may legitimately be called with a pointer this
		// redirection never handed out (for example a pointer obtained
		// through a different, non-redirected allocator). Silently ignoring
		// it is the only safe option: freeing it here would either corrupt
		// this heap's own metadata or reach outside the backing entirely.
		cemuLog_log(LogType::Force,
			"WUPS: plugin heap free rejected out-of-range pointer 0x{:08x} "
			"(backing 0x{:08x}+0x{:x})",
			address, m_backingBase, m_backingSize);
		return;
	}
	auto* handle = static_cast<coreinit::MEMHeapHandle>(m_heapHandle);
	coreinit::MEMFreeToExpHeap(handle, memory_getPointerFromVirtualOffset(address));
	if (m_heapTracker)
		m_heapTracker->OnFree(owner, address);
}

void WupsPluginHeap::Reset()
{
	std::lock_guard lock(m_mutex);
	m_backingBase = 0;
	m_backingSize = 0;
	m_heapHandle = nullptr;
	m_attempted = false;
	m_usable = false;
}

namespace cafe::wups
{
namespace
{
std::mutex g_pluginHeapMutex;
std::shared_ptr<WupsPluginHeap> g_pluginHeap;
} // namespace

void SetActivePluginHeap(std::shared_ptr<WupsPluginHeap> heap)
{
	std::lock_guard lock(g_pluginHeapMutex);
	g_pluginHeap = std::move(heap);
}

std::shared_ptr<WupsPluginHeap> ActivePluginHeap()
{
	std::lock_guard lock(g_pluginHeapMutex);
	return g_pluginHeap;
}
} // namespace cafe::wups
