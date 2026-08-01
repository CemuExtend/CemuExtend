#pragma once

#include "Cafe/HW/Espresso/WupsServices.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// Unified guest-memory ownership foundation shared by the owner-scoped heap
// tracker and the mapped-memory manager. Every owner-scoped guest range (RPL
// section, TLS block, thread stack, tracked heap allocation, owner-created heap
// backing, backend buffer/callable, or mapped-memory allocation) is registered
// here so that pointer provenance, per-API policy, and free/unload lifetime can
// be enforced against a single source of truth.
//
// Registration is two-phase: RegisterRange() reserves the interval in a
// Provisional state (already reachable for rollback/cleanup but never returned
// to pointer lookups), and CommitRange() promotes it to Live once its backing
// memory is initialised. Lookups pin the matching range through an RAII
// WupsGuestRangeLease so that a concurrent free/unload cannot reclaim the
// backing while a host access is in flight.

enum class WupsOwnedRangeKind : std::uint8_t
{
	RplText,
	RplReadOnlyData,
	RplWritableData,
	Tls,
	GuestThreadStack,

	DefaultHeapAllocation,
	ExpHeapAllocation,
	UnitHeapAllocation,
	AllocatorAllocation,
	OwnedHeapBacking,

	BackendBuffer,
	BackendCallable,

	MappedCpu,
	MappedGx2,
};

enum class WupsOwnedRangeState : std::uint8_t
{
	Provisional,
	Live,
	Retiring,
	Released,
};

// Per-API acceptance policy. Owner match alone never authorises an access; the
// caller states what kind of memory the ABI is allowed to touch.
enum class WupsGuestPointerPolicy : std::uint8_t
{
	AnyOwnedMemory,
	WritableOwnedMemory,
	HeapBackedMemory,
	MappedMemory,
	ExecutableCallback,
};

struct WupsOwnedGuestRange
{
	std::uint64_t rangeId{};
	WupsOwnerToken owner{};

	std::uint32_t base{};
	std::uint32_t size{};

	WupsOwnedRangeKind kind{WupsOwnedRangeKind::BackendBuffer};
	WupsOwnedRangeState state{WupsOwnedRangeState::Provisional};

	bool readable{};
	bool writable{};
	bool executable{};

	bool heapEligible{};
	bool acceptsInteriorPointers{};
	bool activelyReleasedOnUnload{};

	std::uint64_t titleLifetime{};
	std::optional<std::uint64_t> parentRangeId{};

	std::uint32_t pinCount{};
};

class WupsGuestMemoryOwnershipRegistry;

// Movable RAII handle that keeps a live range pinned for the duration of a host
// access. While at least one lease exists the range cannot transition to
// Released, so the backing memory stays valid. Never store the resolved host
// pointer beyond the lease's lifetime.
class WupsGuestRangeLease
{
public:
	WupsGuestRangeLease() = default;
	WupsGuestRangeLease(WupsGuestRangeLease&& other) noexcept;
	WupsGuestRangeLease& operator=(WupsGuestRangeLease&& other) noexcept;
	~WupsGuestRangeLease();

	WupsGuestRangeLease(const WupsGuestRangeLease&) = delete;
	WupsGuestRangeLease& operator=(const WupsGuestRangeLease&) = delete;

	[[nodiscard]] bool Valid() const { return m_registry != nullptr; }
	[[nodiscard]] std::uint32_t Base() const { return m_base; }
	[[nodiscard]] std::uint32_t Size() const { return m_size; }
	[[nodiscard]] WupsOwnedRangeKind Kind() const { return m_kind; }
	[[nodiscard]] WupsOwnerToken Owner() const { return m_owner; }

	void Release();

private:
	friend class WupsGuestMemoryOwnershipRegistry;
	WupsGuestRangeLease(WupsGuestMemoryOwnershipRegistry* registry,
		std::uint64_t rangeId, std::uint32_t base, std::uint32_t size,
		WupsOwnedRangeKind kind, WupsOwnerToken owner)
		: m_registry(registry), m_rangeId(rangeId), m_base(base), m_size(size),
			m_kind(kind), m_owner(owner)
	{
	}

	WupsGuestMemoryOwnershipRegistry* m_registry{};
	std::uint64_t m_rangeId{};
	std::uint32_t m_base{};
	std::uint32_t m_size{};
	WupsOwnedRangeKind m_kind{};
	WupsOwnerToken m_owner{};
};

class WupsGuestMemoryOwnershipRegistry
{
public:
	WupsGuestMemoryOwnershipRegistry();
	~WupsGuestMemoryOwnershipRegistry();

	WupsGuestMemoryOwnershipRegistry(const WupsGuestMemoryOwnershipRegistry&) =
		delete;
	WupsGuestMemoryOwnershipRegistry& operator=(
		const WupsGuestMemoryOwnershipRegistry&) = delete;

	// Reserves an interval in the Provisional state. The caller supplies every
	// field except rangeId/pinCount; base/size are validated (non-zero size, no
	// 32-bit end overflow) and cross-owner overlap is rejected. Returns the
	// assigned rangeId.
	[[nodiscard]] std::optional<std::uint64_t> RegisterRange(
		WupsOwnedGuestRange range, std::string& error);

	// Promotes a Provisional range to Live so lookups can return it.
	[[nodiscard]] bool CommitRange(std::uint64_t rangeId, std::string& error);

	// Retires and removes a range. If pins remain the range is moved to Retiring
	// (hidden from new lookups) and removal is deferred until the final lease is
	// released.
	[[nodiscard]] bool UnregisterRange(std::uint64_t rangeId, std::string& error);

	// Hides a range from new lookups and waits for every existing lease before
	// removing it. Backing storage may be reclaimed only after this returns.
	[[nodiscard]] bool RetireRangeAndWait(std::uint64_t rangeId,
		std::string& error);

	// Pins the most specific live range owned by `owner` that fully contains
	// [address, address+size) and satisfies `access` and `policy`. Returns an
	// empty optional (and sets error) on any mismatch.
	[[nodiscard]] std::optional<WupsGuestRangeLease> PinRange(
		WupsOwnerToken owner, std::uint32_t address, std::uint32_t size,
		WupsGuestAccess access, WupsGuestPointerPolicy policy, std::string& error);

	// Non-pinning membership test with identical acceptance rules to PinRange.
	[[nodiscard]] bool BelongsTo(WupsOwnerToken owner, std::uint32_t address,
		std::uint32_t size, WupsGuestAccess access,
		WupsGuestPointerPolicy policy) const;

	// Looks up a live range whose base equals `address` and is owned by `owner`.
	// Used by base-pointer free paths. Returns the rangeId.
	[[nodiscard]] std::optional<std::uint64_t> FindOwnedBase(WupsOwnerToken owner,
		std::uint32_t address) const;

	[[nodiscard]] std::optional<WupsOwnedGuestRange> GetRange(
		std::uint64_t rangeId) const;

	void BeginOwner(WupsOwnerToken owner, std::uint64_t titleLifetime);
	// Hides every range of `owner` from new lookups without freeing them.
	void RevokeOwner(WupsOwnerToken owner);
	// Blocks until no lease pins any range of `owner`. Must not be called while
	// holding a lease on one of that owner's ranges.
	void WaitForOwnerPins(WupsOwnerToken owner);
	// Removes every range of `owner`. Idempotent.
	void ReleaseOwner(WupsOwnerToken owner);
	// Drops all state that does not belong to `titleLifetime` and starts a new
	// title epoch.
	void ResetTitle(std::uint64_t titleLifetime);
	void Shutdown();

	[[nodiscard]] std::size_t LiveRangeCount() const;
	[[nodiscard]] std::size_t OwnerRangeCount(WupsOwnerToken owner) const;

private:
	friend class WupsGuestRangeLease;
	void ReleaseLease(std::uint64_t rangeId);

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
