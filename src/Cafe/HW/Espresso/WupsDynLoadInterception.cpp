#include "Cafe/HW/Espresso/WupsDynLoadInterception.h"

#include "Cafe/HW/Espresso/WupsRuntime.h"
#include "Cafe/HW/Espresso/WupsServices.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cemu/Logging/CemuLogging.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace cafe::wups
{
	namespace
	{
		constexpr std::uint32_t kSyntheticHandleBase = 0xe0000000u;
		constexpr std::uint32_t kSyntheticHandleLimit = 0xefffffffu;

		struct VirtualModule
		{
			std::string name;
			WupsOwnerToken owner;
		};

		std::mutex g_mutex;
		std::weak_ptr<AromaCompatibilityRuntime> g_runtime;
		std::unordered_map<std::uint32_t, VirtualModule> g_handleToModule;
		std::uint32_t g_nextHandle = kSyntheticHandleBase;

		std::optional<WupsOwnerToken> ResolveCaller()
		{
			const auto scoped = WupsGuestOwnerScope::Current();
			std::optional<WupsOwnerToken> mapped;
			if (const auto* cpu = PPCInterpreter_getCurrentInstance())
			{
				for (const auto address : {cpu->spr.LR, cpu->instructionPointer})
				{
					RPLMappedAddressInfo info;
					if (address != 0 && RPLLoader_QueryMappedAddress(address, 4, info) &&
						info.external && info.owner != 0)
					{
						mapped = WupsOwnerToken{info.owner, info.generation};
						break;
					}
				}
			}
			if (scoped && mapped && *scoped != *mapped)
				return std::nullopt;
			return scoped ? scoped : mapped;
		}
	} // namespace

	void SetActiveRuntime(std::weak_ptr<AromaCompatibilityRuntime> runtime)
	{
		std::lock_guard lock(g_mutex);
		g_runtime = std::move(runtime);
		g_handleToModule.clear();
		g_nextHandle = kSyntheticHandleBase;
	}

	std::shared_ptr<cemuextend_hle::Cex2Owner> ResolveCurrentCex2Owner()
	{
		const auto owner = ResolveCaller();
		if (!owner)
			return {};
		std::shared_ptr<AromaCompatibilityRuntime> runtime;
		{
			std::lock_guard lock(g_mutex);
			runtime = g_runtime.lock();
		}
		return runtime ? runtime->Cex2OwnerFor(*owner) : nullptr;
	}

	bool TryAcquireHomebrewModule(std::string_view moduleName,
								  std::uint32_t& handleOut)
	{
		if (moduleName != "homebrew_wupsbackend")
			return false;
		const auto owner = ResolveCaller();
		if (!owner)
			return false;
		std::lock_guard lock(g_mutex);
		if (g_runtime.expired())
			return false;
		for (const auto& [handle, module] : g_handleToModule)
		{
			if (module.name == moduleName && module.owner == *owner)
			{
				handleOut = handle;
				return true;
			}
		}
		if (g_nextHandle > kSyntheticHandleLimit)
			return false;
		const auto handle = g_nextHandle++;
		g_handleToModule.emplace(handle, VirtualModule{std::string(moduleName), *owner});
		handleOut = handle;
		return true;
	}

	bool IsHomebrewModuleHandle(std::uint32_t handle)
	{
		return handle >= kSyntheticHandleBase && handle <= kSyntheticHandleLimit;
	}

	void ReleaseHomebrewModule(std::uint32_t handle)
	{
		if (!IsHomebrewModuleHandle(handle))
			return;
		const auto owner = ResolveCaller();
		std::lock_guard lock(g_mutex);
		const auto found = g_handleToModule.find(handle);
		if (found != g_handleToModule.end() && owner && found->second.owner == *owner)
			g_handleToModule.erase(found);
	}

	bool TryFindHomebrewExport(std::uint32_t handle, bool isData,
							   std::string_view exportName, std::uint32_t& addressOut)
	{
		if (!IsHomebrewModuleHandle(handle))
			return false;
		std::shared_ptr<AromaCompatibilityRuntime> runtime;
		VirtualModule module;
		{
			std::lock_guard lock(g_mutex);
			runtime = g_runtime.lock();
			const auto found = g_handleToModule.find(handle);
			if (found == g_handleToModule.end())
				return false;
			module = found->second;
		}
		if (!runtime)
			return false;
		const auto scoped = ResolveCaller();
		if (!scoped || *scoped != module.owner)
			return false;
		std::string error;
		const auto resolved = runtime->ResolveRuntimeModuleExport(*scoped, module.name,
																  exportName, isData ? WupsSymbolKind::Data : WupsSymbolKind::Function,
																  error);
		if (!resolved)
		{
			cemuLog_log(LogType::Force,
						"WUPS: virtual export '{}.{}' failed for owner {} generation {}: {}",
						module.name, exportName, scoped->owner, scoped->generation, error);
			return false;
		}
		addressOut = *resolved;
		return true;
	}
} // namespace cafe::wups
