#include "Cafe/HW/Espresso/WupsGuestMemoryOwnership.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

namespace
{
// Ordering key for the interval index. Bases may repeat (a heap backing and its
// first sub-allocation can share a start address), so the rangeId disambiguates.
struct IntervalKey
{
	std::uint32_t base{};
	std::uint64_t rangeId{};

	[[nodiscard]] friend bool operator<(const IntervalKey& a, const IntervalKey& b)
	{
		if (a.base != b.base)
			return a.base < b.base;
		return a.rangeId < b.rangeId;
	}
	[[nodiscard]] friend bool operator==(const IntervalKey&,
		const IntervalKey&) = default;
};

// Treap node augmented with the maximum interval end in its subtree, giving
// expected O(log n) stabbing and overlap queries without a linear scan.
struct IntervalNode
{
	IntervalKey key;
	std::uint64_t lo{};   // inclusive base
	std::uint64_t hi{};   // exclusive end (base + size), fits in 33 bits
	std::uint64_t maxEnd{};
	std::uint32_t priority{};
	IntervalNode* left{};
	IntervalNode* right{};
};

[[nodiscard]] std::uint64_t SubtreeMax(const IntervalNode* node)
{
	return node ? node->maxEnd : 0;
}

void Update(IntervalNode* node)
{
	node->maxEnd = std::max({node->hi, SubtreeMax(node->left),
		SubtreeMax(node->right)});
}

class IntervalTree
{
public:
	~IntervalTree() { Destroy(m_root); }

	void Insert(IntervalKey key, std::uint64_t lo, std::uint64_t hi,
		std::uint32_t priority)
	{
		m_root = Insert(m_root, key, lo, hi, priority);
	}

	void Erase(IntervalKey key)
	{
		m_root = Erase(m_root, key);
	}

	// Visits every stored interval [lo, hi) that contains `point`.
	template <typename Visitor>
	void StabPoint(std::uint64_t point, Visitor&& visitor) const
	{
		StabPoint(m_root, point, visitor);
	}

	// Returns true if any stored interval overlaps [lo, hi) and satisfies
	// `predicate(rangeId)`.
	template <typename Predicate>
	[[nodiscard]] bool AnyOverlap(std::uint64_t lo, std::uint64_t hi,
		Predicate&& predicate) const
	{
		return AnyOverlap(m_root, lo, hi, predicate);
	}

private:
	static void Destroy(IntervalNode* node)
	{
		if (!node)
			return;
		Destroy(node->left);
		Destroy(node->right);
		delete node;
	}

	static IntervalNode* RotateRight(IntervalNode* node)
	{
		IntervalNode* l = node->left;
		node->left = l->right;
		l->right = node;
		Update(node);
		Update(l);
		return l;
	}

	static IntervalNode* RotateLeft(IntervalNode* node)
	{
		IntervalNode* r = node->right;
		node->right = r->left;
		r->left = node;
		Update(node);
		Update(r);
		return r;
	}

	static IntervalNode* Insert(IntervalNode* node, IntervalKey key,
		std::uint64_t lo, std::uint64_t hi, std::uint32_t priority)
	{
		if (!node)
		{
			auto* fresh = new IntervalNode{key, lo, hi, hi, priority, nullptr,
				nullptr};
			return fresh;
		}
		if (key < node->key)
		{
			node->left = Insert(node->left, key, lo, hi, priority);
			if (node->left->priority < node->priority)
				node = RotateRight(node);
		}
		else
		{
			node->right = Insert(node->right, key, lo, hi, priority);
			if (node->right->priority < node->priority)
				node = RotateLeft(node);
		}
		Update(node);
		return node;
	}

	static IntervalNode* Erase(IntervalNode* node, IntervalKey key)
	{
		if (!node)
			return nullptr;
		if (key < node->key)
			node->left = Erase(node->left, key);
		else if (node->key < key)
			node->right = Erase(node->right, key);
		else
		{
			if (!node->left && !node->right)
			{
				delete node;
				return nullptr;
			}
			if (!node->left)
			{
				node = RotateLeft(node);
				node->left = Erase(node->left, key);
			}
			else if (!node->right)
			{
				node = RotateRight(node);
				node->right = Erase(node->right, key);
			}
			else if (node->left->priority < node->right->priority)
			{
				node = RotateRight(node);
				node->right = Erase(node->right, key);
			}
			else
			{
				node = RotateLeft(node);
				node->left = Erase(node->left, key);
			}
		}
		if (node)
			Update(node);
		return node;
	}

	template <typename Visitor>
	static void StabPoint(const IntervalNode* node, std::uint64_t point,
		Visitor& visitor)
	{
		if (!node || point >= node->maxEnd)
			return;
		StabPoint(node->left, point, visitor);
		if (node->lo <= point && point < node->hi)
			visitor(node->key.rangeId);
		// Right subtree bases are >= node->base; only recurse if point can still
		// land inside one of them.
		if (point >= node->lo)
			StabPoint(node->right, point, visitor);
	}

	template <typename Predicate>
	static bool AnyOverlap(const IntervalNode* node, std::uint64_t lo,
		std::uint64_t hi, Predicate& predicate)
	{
		if (!node || lo >= node->maxEnd)
			return false;
		if (AnyOverlap(node->left, lo, hi, predicate))
			return true;
		if (node->lo < hi && lo < node->hi && predicate(node->key.rangeId))
			return true;
		if (node->lo < hi)
			return AnyOverlap(node->right, lo, hi, predicate);
		return false;
	}

	IntervalNode* m_root{};
};
} // namespace

struct WupsGuestMemoryOwnershipRegistry::Impl
{
	mutable std::mutex mutex;
	std::condition_variable pinCv;

	std::unordered_map<std::uint64_t, WupsOwnedGuestRange> ranges;
	IntervalTree tree;
	std::uint64_t nextRangeId{1};
	std::uint64_t currentTitleLifetime{0};
	std::mt19937 rng{0x9E3779B9u};

	[[nodiscard]] std::uint32_t Priority() { return rng(); }

	// Access/policy gate shared by PinRange and BelongsTo. `owner` and containment
	// are pre-checked by the caller.
	[[nodiscard]] static bool Accepts(const WupsOwnedGuestRange& range,
		std::uint32_t address, std::uint32_t size, WupsGuestAccess access,
		WupsGuestPointerPolicy policy)
	{
		if (range.state != WupsOwnedRangeState::Live)
			return false;
		// Interior pointers are refused unless the range opts in; such ranges may
		// only be addressed from their exact base.
		if (!range.acceptsInteriorPointers && address != range.base)
			return false;
		switch (access)
		{
		case WupsGuestAccess::Read:
			if (!range.readable)
				return false;
			break;
		case WupsGuestAccess::Write:
			if (!range.writable)
				return false;
			break;
		case WupsGuestAccess::Execute:
			if (!range.executable)
				return false;
			break;
		}
		switch (policy)
		{
		case WupsGuestPointerPolicy::AnyOwnedMemory:
			return range.readable;
		case WupsGuestPointerPolicy::WritableOwnedMemory:
			return range.writable;
		case WupsGuestPointerPolicy::HeapBackedMemory:
			return range.heapEligible;
		case WupsGuestPointerPolicy::MappedMemory:
			return range.kind == WupsOwnedRangeKind::MappedCpu ||
				range.kind == WupsOwnedRangeKind::MappedGx2;
		case WupsGuestPointerPolicy::ExecutableCallback:
			return range.executable &&
				(range.kind == WupsOwnedRangeKind::RplText ||
					range.kind == WupsOwnedRangeKind::BackendCallable);
		}
		return false;
	}

	// Returns the id of the narrowest live range owned by `owner` that fully
	// contains [address, address+size) and satisfies access/policy.
	[[nodiscard]] std::optional<std::uint64_t> FindMostSpecific(
		WupsOwnerToken owner, std::uint32_t address, std::uint32_t size,
		WupsGuestAccess access, WupsGuestPointerPolicy policy) const
	{
		const std::uint64_t end = static_cast<std::uint64_t>(address) + size;
		std::optional<std::uint64_t> best;
		std::uint64_t bestSize = std::numeric_limits<std::uint64_t>::max();
		tree.StabPoint(address, [&](std::uint64_t rangeId) {
			const auto it = ranges.find(rangeId);
			if (it == ranges.end())
				return;
			const WupsOwnedGuestRange& r = it->second;
			if (r.owner != owner)
				return;
			const std::uint64_t rEnd = static_cast<std::uint64_t>(r.base) + r.size;
			if (rEnd < end)
				return; // does not fully contain the query interval
			if (!Accepts(r, address, size, access, policy))
				return;
			if (r.size < bestSize)
			{
				bestSize = r.size;
				best = rangeId;
			}
		});
		return best;
	}
};

// ---- WupsGuestRangeLease -----------------------------------------------------

WupsGuestRangeLease::WupsGuestRangeLease(WupsGuestRangeLease&& other) noexcept
{
	*this = std::move(other);
}

WupsGuestRangeLease& WupsGuestRangeLease::operator=(
	WupsGuestRangeLease&& other) noexcept
{
	if (this != &other)
	{
		Release();
		m_registry = other.m_registry;
		m_rangeId = other.m_rangeId;
		m_base = other.m_base;
		m_size = other.m_size;
		m_kind = other.m_kind;
		m_owner = other.m_owner;
		other.m_registry = nullptr;
	}
	return *this;
}

WupsGuestRangeLease::~WupsGuestRangeLease()
{
	Release();
}

void WupsGuestRangeLease::Release()
{
	if (m_registry)
	{
		m_registry->ReleaseLease(m_rangeId);
		m_registry = nullptr;
	}
}

// ---- WupsGuestMemoryOwnershipRegistry ---------------------------------------

WupsGuestMemoryOwnershipRegistry::WupsGuestMemoryOwnershipRegistry()
	: m_impl(std::make_unique<Impl>())
{
}

WupsGuestMemoryOwnershipRegistry::~WupsGuestMemoryOwnershipRegistry() = default;

std::optional<std::uint64_t> WupsGuestMemoryOwnershipRegistry::RegisterRange(
	WupsOwnedGuestRange range, std::string& error)
{
	if (range.size == 0)
	{
		error = "ownership range has zero size";
		return std::nullopt;
	}
	const std::uint64_t lo = range.base;
	const std::uint64_t hi = lo + range.size;
	if (hi > std::numeric_limits<std::uint32_t>::max() + std::uint64_t{1})
	{
		error = "ownership range end overflows the 32-bit guest space";
		return std::nullopt;
	}
	std::lock_guard lock(m_impl->mutex);
	// Reject any overlap with a range owned by a different owner.
	const bool crossOwner = m_impl->tree.AnyOverlap(lo, hi,
		[&](std::uint64_t existingId) {
			const auto it = m_impl->ranges.find(existingId);
			return it != m_impl->ranges.end() && it->second.owner != range.owner;
		});
	if (crossOwner)
	{
		error = "ownership range overlaps memory owned by another plugin";
		return std::nullopt;
	}
	const std::uint64_t rangeId = m_impl->nextRangeId++;
	range.rangeId = rangeId;
	range.state = WupsOwnedRangeState::Provisional;
	range.pinCount = 0;
	if (range.titleLifetime == 0)
		range.titleLifetime = m_impl->currentTitleLifetime;
	m_impl->tree.Insert({range.base, rangeId}, lo, hi, m_impl->Priority());
	m_impl->ranges.emplace(rangeId, std::move(range));
	return rangeId;
}

bool WupsGuestMemoryOwnershipRegistry::CommitRange(std::uint64_t rangeId,
	std::string& error)
{
	std::lock_guard lock(m_impl->mutex);
	const auto it = m_impl->ranges.find(rangeId);
	if (it == m_impl->ranges.end())
	{
		error = "ownership range does not exist";
		return false;
	}
	if (it->second.state == WupsOwnedRangeState::Retiring ||
		it->second.state == WupsOwnedRangeState::Released)
	{
		error = "ownership range is being torn down";
		return false;
	}
	it->second.state = WupsOwnedRangeState::Live;
	return true;
}

bool WupsGuestMemoryOwnershipRegistry::UnregisterRange(std::uint64_t rangeId,
	std::string& error)
{
	std::lock_guard lock(m_impl->mutex);
	const auto it = m_impl->ranges.find(rangeId);
	if (it == m_impl->ranges.end())
	{
		error = "ownership range does not exist";
		return false;
	}
	if (it->second.pinCount != 0)
	{
		// Hide from new lookups; the final lease release performs removal.
		it->second.state = WupsOwnedRangeState::Retiring;
		return true;
	}
	m_impl->tree.Erase({it->second.base, rangeId});
	m_impl->ranges.erase(it);
	return true;
}

std::optional<WupsGuestRangeLease> WupsGuestMemoryOwnershipRegistry::PinRange(
	WupsOwnerToken owner, std::uint32_t address, std::uint32_t size,
	WupsGuestAccess access, WupsGuestPointerPolicy policy, std::string& error)
{
	if (size == 0)
	{
		error = "guest range has zero size";
		return std::nullopt;
	}
	if (static_cast<std::uint64_t>(address) + size >
		std::numeric_limits<std::uint32_t>::max() + std::uint64_t{1})
	{
		error = "guest range end overflows the 32-bit guest space";
		return std::nullopt;
	}
	std::lock_guard lock(m_impl->mutex);
	const auto rangeId = m_impl->FindMostSpecific(owner, address, size, access,
		policy);
	if (!rangeId)
	{
		error = "no owned live range authorises this access";
		return std::nullopt;
	}
	WupsOwnedGuestRange& range = m_impl->ranges.at(*rangeId);
	++range.pinCount;
	return WupsGuestRangeLease(this, *rangeId, range.base, range.size, range.kind,
		range.owner);
}

bool WupsGuestMemoryOwnershipRegistry::BelongsTo(WupsOwnerToken owner,
	std::uint32_t address, std::uint32_t size, WupsGuestAccess access,
	WupsGuestPointerPolicy policy) const
{
	if (size == 0)
		return false;
	if (static_cast<std::uint64_t>(address) + size >
		std::numeric_limits<std::uint32_t>::max() + std::uint64_t{1})
		return false;
	std::lock_guard lock(m_impl->mutex);
	return m_impl->FindMostSpecific(owner, address, size, access, policy)
		.has_value();
}

std::optional<std::uint64_t> WupsGuestMemoryOwnershipRegistry::FindOwnedBase(
	WupsOwnerToken owner, std::uint32_t address) const
{
	std::lock_guard lock(m_impl->mutex);
	std::optional<std::uint64_t> found;
	m_impl->tree.StabPoint(address, [&](std::uint64_t rangeId) {
		if (found)
			return;
		const auto it = m_impl->ranges.find(rangeId);
		if (it == m_impl->ranges.end())
			return;
		const WupsOwnedGuestRange& r = it->second;
		if (r.state == WupsOwnedRangeState::Live && r.owner == owner &&
			r.base == address)
			found = rangeId;
	});
	return found;
}

std::optional<WupsOwnedGuestRange> WupsGuestMemoryOwnershipRegistry::GetRange(
	std::uint64_t rangeId) const
{
	std::lock_guard lock(m_impl->mutex);
	const auto it = m_impl->ranges.find(rangeId);
	if (it == m_impl->ranges.end())
		return std::nullopt;
	return it->second;
}

void WupsGuestMemoryOwnershipRegistry::BeginOwner(WupsOwnerToken owner,
	std::uint64_t titleLifetime)
{
	std::lock_guard lock(m_impl->mutex);
	if (titleLifetime != 0)
		m_impl->currentTitleLifetime = titleLifetime;
	(void)owner;
}

void WupsGuestMemoryOwnershipRegistry::RevokeOwner(WupsOwnerToken owner)
{
	std::lock_guard lock(m_impl->mutex);
	for (auto& [rangeId, range] : m_impl->ranges)
		if (range.owner == owner && range.state == WupsOwnedRangeState::Live)
			range.state = WupsOwnedRangeState::Retiring;
}

void WupsGuestMemoryOwnershipRegistry::WaitForOwnerPins(WupsOwnerToken owner)
{
	std::unique_lock lock(m_impl->mutex);
	m_impl->pinCv.wait(lock, [&] {
		for (const auto& [rangeId, range] : m_impl->ranges)
			if (range.owner == owner && range.pinCount != 0)
				return false;
		return true;
	});
}

void WupsGuestMemoryOwnershipRegistry::ReleaseOwner(WupsOwnerToken owner)
{
	std::lock_guard lock(m_impl->mutex);
	for (auto it = m_impl->ranges.begin(); it != m_impl->ranges.end();)
	{
		if (it->second.owner == owner && it->second.pinCount == 0)
		{
			m_impl->tree.Erase({it->second.base, it->first});
			it = m_impl->ranges.erase(it);
		}
		else
		{
			if (it->second.owner == owner)
				it->second.state = WupsOwnedRangeState::Retiring;
			++it;
		}
	}
}

void WupsGuestMemoryOwnershipRegistry::ResetTitle(std::uint64_t titleLifetime)
{
	std::lock_guard lock(m_impl->mutex);
	for (auto it = m_impl->ranges.begin(); it != m_impl->ranges.end();)
	{
		if (it->second.titleLifetime != titleLifetime &&
			it->second.pinCount == 0)
		{
			m_impl->tree.Erase({it->second.base, it->first});
			it = m_impl->ranges.erase(it);
		}
		else
		{
			++it;
		}
	}
	m_impl->currentTitleLifetime = titleLifetime;
}

void WupsGuestMemoryOwnershipRegistry::Shutdown()
{
	std::lock_guard lock(m_impl->mutex);
	for (auto& [rangeId, range] : m_impl->ranges)
	{
		if (range.pinCount == 0)
			m_impl->tree.Erase({range.base, rangeId});
	}
	std::erase_if(m_impl->ranges,
		[](const auto& entry) { return entry.second.pinCount == 0; });
}

std::size_t WupsGuestMemoryOwnershipRegistry::LiveRangeCount() const
{
	std::lock_guard lock(m_impl->mutex);
	std::size_t count = 0;
	for (const auto& [rangeId, range] : m_impl->ranges)
		if (range.state == WupsOwnedRangeState::Live)
			++count;
	return count;
}

std::size_t WupsGuestMemoryOwnershipRegistry::OwnerRangeCount(
	WupsOwnerToken owner) const
{
	std::lock_guard lock(m_impl->mutex);
	std::size_t count = 0;
	for (const auto& [rangeId, range] : m_impl->ranges)
		if (range.owner == owner &&
			range.state != WupsOwnedRangeState::Released)
			++count;
	return count;
}

void WupsGuestMemoryOwnershipRegistry::ReleaseLease(std::uint64_t rangeId)
{
	{
		std::lock_guard lock(m_impl->mutex);
		const auto it = m_impl->ranges.find(rangeId);
		if (it == m_impl->ranges.end())
			return;
		if (it->second.pinCount != 0)
			--it->second.pinCount;
		if (it->second.pinCount == 0 &&
			it->second.state == WupsOwnedRangeState::Retiring)
		{
			m_impl->tree.Erase({it->second.base, rangeId});
			m_impl->ranges.erase(it);
		}
	}
	m_impl->pinCv.notify_all();
}
