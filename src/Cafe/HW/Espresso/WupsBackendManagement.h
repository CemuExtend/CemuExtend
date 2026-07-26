#pragma once

#include "Cafe/HW/Espresso/CemodPackage.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

class WupsPluginRuntime;

struct WupsProcessKey
{
	std::uint8_t process{};
	std::uint64_t titleId{};
	[[nodiscard]] auto operator<=>(const WupsProcessKey&) const = default;
};

struct WupsContainerSnapshot
{
	std::uint32_t publicHandle{};
	std::uint64_t ownerHandle{};
	std::uint32_t generation{};
	std::uint64_t stableOrder{};
	std::shared_ptr<WupsPluginRuntime> runtime;
	std::shared_ptr<const CemodPackage> package;
};

class WupsBackendManagementRuntime
{
public:
	static constexpr std::size_t kMaximumDataHandles = 4096;
	static constexpr std::size_t kMaximumPlanPlugins = 256;

	[[nodiscard]] std::optional<std::uint32_t> CreatePluginData(
		CemodPackage package);
	[[nodiscard]] std::shared_ptr<const CemodPackage> FindPluginData(
		std::uint32_t handle) const;
	void DeletePluginData(std::span<const std::uint32_t> handles);

	[[nodiscard]] bool ScheduleNextLaunch(WupsProcessKey key,
		std::span<const std::uint32_t> handles);
	[[nodiscard]] bool HasPendingPlan(WupsProcessKey key) const;
	[[nodiscard]] std::optional<std::vector<CemodPackage>> ConsumePendingPlan(
		WupsProcessKey key);

	[[nodiscard]] std::optional<std::uint32_t> PublishContainer(
		const std::shared_ptr<WupsPluginRuntime>& runtime);
	void UnpublishContainer(std::uint64_t owner, std::uint32_t generation);
	void UnpublishAll();
	[[nodiscard]] std::vector<WupsContainerSnapshot> LoadedContainers() const;
	[[nodiscard]] std::optional<WupsContainerSnapshot> FindContainer(
		std::uint32_t handle) const;

private:
	mutable std::mutex m_mutex;
	std::map<std::uint32_t, std::shared_ptr<const CemodPackage>> m_pluginData;
	std::map<std::uint32_t, WupsContainerSnapshot> m_containers;
	std::map<WupsProcessKey, std::vector<std::shared_ptr<const CemodPackage>>>
		m_pendingPlans;
	std::uint32_t m_nextDataHandle{1};
	std::uint32_t m_nextContainerHandle{1};
	std::uint64_t m_nextOrder{1};
};

[[nodiscard]] CemodPackage MakeDynamicWupsPackage(
	std::vector<std::byte> bytes, WupsInspection inspection,
	const CemodPackage& caller);

