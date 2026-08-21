#include "Cafe/HW/Espresso/WupsBackendManagement.h"

#include "Cafe/HW/Espresso/WupsRuntime.h"

#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <set>

std::optional<std::uint32_t> WupsBackendManagementRuntime::CreatePluginData(
	CemodPackage package)
{
	std::lock_guard lock(m_mutex);
	if (m_pluginData.size() >= kMaximumDataHandles || m_nextDataHandle == 0)
		return std::nullopt;
	const auto handle = m_nextDataHandle++;
	m_pluginData.emplace(handle,
						 std::make_shared<const CemodPackage>(std::move(package)));
	return handle;
}

std::shared_ptr<const CemodPackage> WupsBackendManagementRuntime::FindPluginData(
	std::uint32_t handle) const
{
	std::lock_guard lock(m_mutex);
	const auto found = m_pluginData.find(handle);
	return found == m_pluginData.end() ? nullptr : found->second;
}

void WupsBackendManagementRuntime::DeletePluginData(
	std::span<const std::uint32_t> handles)
{
	std::lock_guard lock(m_mutex);
	for (const auto handle : handles)
		m_pluginData.erase(handle);
}

bool WupsBackendManagementRuntime::ScheduleNextLaunch(WupsProcessKey key,
													  std::span<const std::uint32_t> handles)
{
	if (handles.empty() || handles.size() > kMaximumPlanPlugins)
		return false;
	std::vector<std::shared_ptr<const CemodPackage>> plan;
	std::set<std::pair<std::string, std::string>> identities;
	std::lock_guard lock(m_mutex);
	plan.reserve(handles.size());
	for (const auto handle : handles)
	{
		const auto found = m_pluginData.find(handle);
		if (found == m_pluginData.end() || !found->second->wups)
			continue;
		const auto& metadata = found->second->wups->metadata;
		if (!identities.emplace(metadata.name, metadata.author).second)
			continue;
		plan.push_back(found->second);
	}
	m_pendingPlans.insert_or_assign(key, std::move(plan));
	return true;
}

bool WupsBackendManagementRuntime::HasPendingPlan(WupsProcessKey key) const
{
	std::lock_guard lock(m_mutex);
	return m_pendingPlans.contains(key);
}

std::optional<std::vector<CemodPackage>>
WupsBackendManagementRuntime::ConsumePendingPlan(WupsProcessKey key)
{
	std::lock_guard lock(m_mutex);
	const auto found = m_pendingPlans.find(key);
	if (found == m_pendingPlans.end())
		return std::nullopt;
	std::vector<CemodPackage> result;
	result.reserve(found->second.size());
	for (const auto& package : found->second)
		result.push_back(*package);
	m_pendingPlans.erase(found);
	return result;
}

std::optional<std::uint32_t> WupsBackendManagementRuntime::PublishContainer(
	const std::shared_ptr<WupsPluginRuntime>& runtime)
{
	if (!runtime)
		return std::nullopt;
	std::lock_guard lock(m_mutex);
	for (const auto& [handle, record] : m_containers)
		if (record.ownerHandle == runtime->OwnerHandle() &&
			record.generation == runtime->Generation())
			return handle;
	if (m_nextContainerHandle == 0)
		return std::nullopt;
	const auto handle = m_nextContainerHandle++;
	auto package = std::make_shared<const CemodPackage>(runtime->PackageCopy());
	m_containers.emplace(handle, WupsContainerSnapshot{handle,
													   runtime->OwnerHandle(), runtime->Generation(), m_nextOrder++, runtime,
													   std::move(package)});
	return handle;
}

void WupsBackendManagementRuntime::UnpublishContainer(
	std::uint64_t owner, std::uint32_t generation)
{
	std::lock_guard lock(m_mutex);
	std::erase_if(m_containers, [owner, generation](const auto& item) {
		return item.second.ownerHandle == owner &&
			   item.second.generation == generation;
	});
}

void WupsBackendManagementRuntime::UnpublishAll()
{
	std::lock_guard lock(m_mutex);
	m_containers.clear();
}

std::vector<WupsContainerSnapshot>
WupsBackendManagementRuntime::LoadedContainers() const
{
	std::lock_guard lock(m_mutex);
	std::vector<WupsContainerSnapshot> result;
	result.reserve(m_containers.size());
	for (const auto& [handle, record] : m_containers)
		if (record.runtime && (record.runtime->State() == WupsPluginState::Mapped ||
							   record.runtime->State() == WupsPluginState::Relocated ||
							   record.runtime->State() == WupsPluginState::Initialized ||
							   record.runtime->State() == WupsPluginState::Active))
			result.push_back(record);
	std::ranges::sort(result, {}, &WupsContainerSnapshot::stableOrder);
	return result;
}

std::optional<WupsContainerSnapshot>
WupsBackendManagementRuntime::FindContainer(std::uint32_t handle) const
{
	std::lock_guard lock(m_mutex);
	const auto found = m_containers.find(handle);
	if (found == m_containers.end() || !found->second.runtime)
		return std::nullopt;
	const auto state = found->second.runtime->State();
	if (state == WupsPluginState::Unloading || state == WupsPluginState::Unloaded ||
		state == WupsPluginState::Failed || state == WupsPluginState::Installed)
		return std::nullopt;
	return found->second;
}

CemodPackage MakeDynamicWupsPackage(std::vector<std::byte> bytes,
									WupsInspection inspection, const CemodPackage& caller)
{
	auto safeId = inspection.metadata.storageId.empty() ? inspection.metadata.name : inspection.metadata.storageId;
	for (auto& character : safeId)
		if (!std::isalnum(static_cast<unsigned char>(character)) &&
			character != '.' && character != '_' && character != '-')
			character = '_';
	if (safeId.empty())
		safeId = "plugin";
	std::uint64_t digest = 1469598103934665603ULL;
	for (const auto byte : bytes)
	{
		digest ^= std::to_integer<unsigned char>(byte);
		digest *= 1099511628211ULL;
	}
	CemodPackage result;
	result.manifest.packageVersion = 3;
	result.manifest.apiVersion = caller.manifest.apiVersion;
	result.manifest.executionMode = CemodExecutionMode::TrustedNative;
	result.manifest.payload = {CemodPayloadFormat::Wups, "plugin.wps"};
	result.manifest.scope = caller.manifest.scope;
	result.manifest.nativePermissions = caller.manifest.nativePermissions;
	result.manifest.modId = fmt::format("dynamic.{}.{:016x}", safeId, digest);
	result.manifest.titleIds = caller.manifest.titleIds;
	result.payload = std::move(bytes);
	result.wups = std::move(inspection);
	result.principal = caller.principal + "/dynamic-wups/" + result.manifest.modId;
	result.targetTitleId = caller.targetTitleId;
	result.signedPackage = false;
	return result;
}
