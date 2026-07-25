#include "Cafe/HW/Espresso/WupsGuestMemoryOwnership.h"
#include "Cafe/HW/Espresso/WupsOwnerScopedHeap.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace
{
[[noreturn]] void CheckFailed(const char* expression, int line)
{
	std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
	std::abort();
}
#define CHECK(condition) \
	do { if (!(condition)) CheckFailed(#condition, __LINE__); } while (false)

constexpr WupsOwnerToken kOwnerA{1, 1};
constexpr WupsOwnerToken kOwnerB{2, 1};

WupsOwnedGuestRange MakeRange(WupsOwnerToken owner, std::uint32_t base,
	std::uint32_t size, WupsOwnedRangeKind kind, bool writable, bool executable,
	bool heapEligible, bool interior)
{
	WupsOwnedGuestRange range{};
	range.owner = owner;
	range.base = base;
	range.size = size;
	range.kind = kind;
	range.readable = true;
	range.writable = writable;
	range.executable = executable;
	range.heapEligible = heapEligible;
	range.acceptsInteriorPointers = interior;
	range.activelyReleasedOnUnload = true;
	return range;
}

std::uint64_t Commit(WupsGuestMemoryOwnershipRegistry& reg,
	WupsOwnedGuestRange range)
{
	std::string error;
	const auto id = reg.RegisterRange(range, error);
	CHECK(id.has_value());
	CHECK(reg.CommitRange(*id, error));
	return *id;
}

void TestRegisterCommitAndProvisionalHiding()
{
	WupsGuestMemoryOwnershipRegistry reg;
	std::string error;
	auto range = MakeRange(kOwnerA, 0x10000000, 0x1000,
		WupsOwnedRangeKind::RplWritableData, true, false, false, true);
	const auto id = reg.RegisterRange(range, error);
	CHECK(id.has_value());
	// Provisional ranges are never returned to lookups.
	CHECK(!reg.BelongsTo(kOwnerA, 0x10000000, 4, WupsGuestAccess::Read,
		WupsGuestPointerPolicy::AnyOwnedMemory));
	CHECK(reg.CommitRange(*id, error));
	CHECK(reg.BelongsTo(kOwnerA, 0x10000000, 4, WupsGuestAccess::Read,
		WupsGuestPointerPolicy::AnyOwnedMemory));
}

void TestCrossOwnerOverlapRejectedNestingAllowed()
{
	WupsGuestMemoryOwnershipRegistry reg;
	std::string error;
	(void)Commit(reg, MakeRange(kOwnerA, 0x20000000, 0x2000,
		WupsOwnedRangeKind::OwnedHeapBacking, true, false, true, true));
	// Different owner overlapping the same span is rejected.
	CHECK(!reg.RegisterRange(MakeRange(kOwnerB, 0x20001000, 0x1000,
		WupsOwnedRangeKind::DefaultHeapAllocation, true, false, true, true),
		error).has_value());
	// Same owner nested allocation inside its backing is allowed.
	CHECK(reg.RegisterRange(MakeRange(kOwnerA, 0x20001000, 0x100,
		WupsOwnedRangeKind::ExpHeapAllocation, true, false, true, true), error)
		.has_value());
}

void TestMostSpecificSelection()
{
	WupsGuestMemoryOwnershipRegistry reg;
	(void)Commit(reg, MakeRange(kOwnerA, 0x30000000, 0x10000,
		WupsOwnedRangeKind::OwnedHeapBacking, true, false, true, true));
	(void)Commit(reg, MakeRange(kOwnerA, 0x30000000, 0x100,
		WupsOwnedRangeKind::ExpHeapAllocation, true, false, true, true));
	std::string error;
	auto lease = reg.PinRange(kOwnerA, 0x30000000, 4, WupsGuestAccess::Read,
		WupsGuestPointerPolicy::HeapBackedMemory, error);
	CHECK(lease.has_value());
	// The narrower 0x100 allocation wins over the 0x10000 backing.
	CHECK(lease->Size() == 0x100);
}

void TestPolicyGating()
{
	WupsGuestMemoryOwnershipRegistry reg;
	(void)Commit(reg, MakeRange(kOwnerA, 0x40000000, 0x1000,
		WupsOwnedRangeKind::RplWritableData, true, false, false, true));
	(void)Commit(reg, MakeRange(kOwnerA, 0x41000000, 0x1000,
		WupsOwnedRangeKind::RplText, false, true, false, false));
	(void)Commit(reg, MakeRange(kOwnerA, 0x80000000, 0x1000,
		WupsOwnedRangeKind::MappedCpu, true, false, true, true));

	// RPL writable data is writable but not heap-eligible.
	CHECK(reg.BelongsTo(kOwnerA, 0x40000000, 4, WupsGuestAccess::Write,
		WupsGuestPointerPolicy::WritableOwnedMemory));
	CHECK(!reg.BelongsTo(kOwnerA, 0x40000000, 4, WupsGuestAccess::Write,
		WupsGuestPointerPolicy::HeapBackedMemory));
	// RPL text is an executable callback, not writable/heap.
	CHECK(reg.BelongsTo(kOwnerA, 0x41000000, 4, WupsGuestAccess::Execute,
		WupsGuestPointerPolicy::ExecutableCallback));
	CHECK(!reg.BelongsTo(kOwnerA, 0x41000000, 4, WupsGuestAccess::Write,
		WupsGuestPointerPolicy::WritableOwnedMemory));
	// Mapped memory satisfies MappedMemory and HeapBackedMemory but not callback.
	CHECK(reg.BelongsTo(kOwnerA, 0x80000000, 4, WupsGuestAccess::Write,
		WupsGuestPointerPolicy::MappedMemory));
	CHECK(!reg.BelongsTo(kOwnerA, 0x80000000, 4, WupsGuestAccess::Execute,
		WupsGuestPointerPolicy::ExecutableCallback));
}

void TestInteriorPointerRules()
{
	WupsGuestMemoryOwnershipRegistry reg;
	// Callable thunk: interior rejected, base only.
	(void)Commit(reg, MakeRange(kOwnerA, 0x50000000, 0x40,
		WupsOwnedRangeKind::BackendCallable, false, true, false, false));
	CHECK(reg.BelongsTo(kOwnerA, 0x50000000, 4, WupsGuestAccess::Execute,
		WupsGuestPointerPolicy::ExecutableCallback));
	CHECK(!reg.BelongsTo(kOwnerA, 0x50000004, 4, WupsGuestAccess::Execute,
		WupsGuestPointerPolicy::ExecutableCallback));
	// Heap allocation: interior allowed.
	(void)Commit(reg, MakeRange(kOwnerA, 0x51000000, 0x100,
		WupsOwnedRangeKind::DefaultHeapAllocation, true, false, true, true));
	CHECK(reg.BelongsTo(kOwnerA, 0x51000040, 4, WupsGuestAccess::Read,
		WupsGuestPointerPolicy::HeapBackedMemory));
}

void TestOwnerAndGenerationIsolation()
{
	WupsGuestMemoryOwnershipRegistry reg;
	(void)Commit(reg, MakeRange(kOwnerA, 0x60000000, 0x1000,
		WupsOwnedRangeKind::DefaultHeapAllocation, true, false, true, true));
	// Different owner cannot see it.
	CHECK(!reg.BelongsTo(kOwnerB, 0x60000000, 4, WupsGuestAccess::Read,
		WupsGuestPointerPolicy::HeapBackedMemory));
	// Same owner id, newer generation cannot see the old generation's range.
	CHECK(!reg.BelongsTo(WupsOwnerToken{1, 2}, 0x60000000, 4,
		WupsGuestAccess::Read, WupsGuestPointerPolicy::HeapBackedMemory));
}

void TestPinBlocksRemovalUntilReleased()
{
	WupsGuestMemoryOwnershipRegistry reg;
	const auto id = Commit(reg, MakeRange(kOwnerA, 0x70000000, 0x1000,
		WupsOwnedRangeKind::DefaultHeapAllocation, true, false, true, true));
	std::string error;
	auto lease = reg.PinRange(kOwnerA, 0x70000000, 4, WupsGuestAccess::Write,
		WupsGuestPointerPolicy::HeapBackedMemory, error);
	CHECK(lease.has_value());
	CHECK(reg.UnregisterRange(id, error)); // pinned -> Retiring
	// Hidden from new lookups immediately, but still present until unpinned.
	CHECK(!reg.BelongsTo(kOwnerA, 0x70000000, 4, WupsGuestAccess::Write,
		WupsGuestPointerPolicy::HeapBackedMemory));
	CHECK(reg.OwnerRangeCount(kOwnerA) == 1);
	lease->Release();
	CHECK(reg.OwnerRangeCount(kOwnerA) == 0);
}

void TestFreeSemanticsBaseOnly()
{
	WupsGuestMemoryOwnershipRegistry reg;
	(void)Commit(reg, MakeRange(kOwnerA, 0x71000000, 0x1000,
		WupsOwnedRangeKind::DefaultHeapAllocation, true, false, true, true));
	// Interior pointer is not a valid free target.
	CHECK(!reg.FindOwnedBase(kOwnerA, 0x71000040).has_value());
	// Base owned by another owner is not found for this owner.
	CHECK(!reg.FindOwnedBase(kOwnerB, 0x71000000).has_value());
	CHECK(reg.FindOwnedBase(kOwnerA, 0x71000000).has_value());
}

void TestReleaseOwnerAndReset()
{
	WupsGuestMemoryOwnershipRegistry reg;
	reg.BeginOwner(kOwnerA, 100);
	(void)Commit(reg, MakeRange(kOwnerA, 0x72000000, 0x1000,
		WupsOwnedRangeKind::DefaultHeapAllocation, true, false, true, true));
	(void)Commit(reg, MakeRange(kOwnerB, 0x73000000, 0x1000,
		WupsOwnedRangeKind::DefaultHeapAllocation, true, false, true, true));
	reg.ReleaseOwner(kOwnerA);
	CHECK(reg.OwnerRangeCount(kOwnerA) == 0);
	CHECK(reg.OwnerRangeCount(kOwnerB) == 1);
	// A new title epoch drops everything from the previous lifetime.
	reg.ResetTitle(200);
	CHECK(reg.LiveRangeCount() == 0);
}

void TestValidationErrors()
{
	WupsGuestMemoryOwnershipRegistry reg;
	std::string error;
	CHECK(!reg.RegisterRange(MakeRange(kOwnerA, 0x10000000, 0,
		WupsOwnedRangeKind::DefaultHeapAllocation, true, false, true, true),
		error).has_value());
	// End overflow: base near the top with a size that crosses 2^32.
	CHECK(!reg.RegisterRange(MakeRange(kOwnerA, 0xFFFFF000, 0x2000,
		WupsOwnedRangeKind::DefaultHeapAllocation, true, false, true, true),
		error).has_value());
	// Exact top of the address space is valid.
	CHECK(reg.RegisterRange(MakeRange(kOwnerA, 0xFFFFF000, 0x1000,
		WupsOwnedRangeKind::DefaultHeapAllocation, true, false, true, true),
		error).has_value());
}

void TestHeapTrackerAllocationLifecycle()
{
	WupsGuestMemoryOwnershipRegistry reg;
	WupsOwnerScopedHeapTracker tracker(reg);
	tracker.OnAllocation(kOwnerA, WupsHeapAllocatorKind::DefaultHeap, 0,
		0x90000000, 0x80, 0x80, 8);
	CHECK(tracker.OwnerAllocationCount(kOwnerA) == 1);
	CHECK(reg.BelongsTo(kOwnerA, 0x90000040, 4, WupsGuestAccess::Write,
		WupsGuestPointerPolicy::HeapBackedMemory));
	// Different owner cannot use the allocation.
	CHECK(!reg.BelongsTo(kOwnerB, 0x90000040, 4, WupsGuestAccess::Read,
		WupsGuestPointerPolicy::HeapBackedMemory));
	// Free by a different owner is ignored.
	tracker.OnFree(kOwnerB, 0x90000000);
	CHECK(reg.BelongsTo(kOwnerA, 0x90000000, 4, WupsGuestAccess::Read,
		WupsGuestPointerPolicy::HeapBackedMemory));
	tracker.OnFree(kOwnerA, 0x90000000);
	CHECK(!reg.BelongsTo(kOwnerA, 0x90000000, 4, WupsGuestAccess::Read,
		WupsGuestPointerPolicy::HeapBackedMemory));
	// Double free is a no-op.
	tracker.OnFree(kOwnerA, 0x90000000);
	CHECK(tracker.OwnerAllocationCount(kOwnerA) == 0);
}

void TestHeapTrackerOwnedHeapBacking()
{
	WupsGuestMemoryOwnershipRegistry reg;
	WupsOwnerScopedHeapTracker tracker(reg);
	tracker.RegisterOwnedHeap(kOwnerA, 42, 0x91000000, 0x10000,
		WupsHeapAllocatorKind::OwnerCreatedHeap);
	// Any interior pointer inside the backing carries owner provenance.
	CHECK(reg.BelongsTo(kOwnerA, 0x91008000, 16, WupsGuestAccess::Write,
		WupsGuestPointerPolicy::HeapBackedMemory));
	tracker.ReleaseOwner(kOwnerA);
	reg.ReleaseOwner(kOwnerA);
	CHECK(!reg.BelongsTo(kOwnerA, 0x91008000, 16, WupsGuestAccess::Write,
		WupsGuestPointerPolicy::HeapBackedMemory));
	CHECK(tracker.OwnerAllocationCount(kOwnerA) == 0);
}
} // namespace

int main()
{
	TestRegisterCommitAndProvisionalHiding();
	TestCrossOwnerOverlapRejectedNestingAllowed();
	TestMostSpecificSelection();
	TestPolicyGating();
	TestInteriorPointerRules();
	TestOwnerAndGenerationIsolation();
	TestPinBlocksRemovalUntilReleased();
	TestFreeSemanticsBaseOnly();
	TestReleaseOwnerAndReset();
	TestValidationErrors();
	TestHeapTrackerAllocationLifecycle();
	TestHeapTrackerOwnedHeapBacking();
	std::cout << "WUPS guest memory ownership tests passed\n";
	return 0;
}
