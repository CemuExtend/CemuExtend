#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsRuntime.h"

#include "Cafe/HW/Espresso/WupsBackendManagement.h"
#include "Cafe/HW/Espresso/WupsDynLoadInterception.h"
#include "Cafe/HW/Espresso/WupsGuestCallback.h"
#include "Cafe/HW/Espresso/WumsRuntime.h"
#include "Cafe/OS/RPL/rpl.h"
#include "config/ActiveSettings.h"

namespace
{
	struct WupsResolverContext
	{
		CemodPackage package;
		WupsMetadata metadata;
	};

	class RplWupsModuleLoader final : public IWupsModuleLoader
	{
	public:
		bool Map(std::span<const std::byte> image, std::string_view moduleName,
			std::uint64_t owner, std::uint32_t generation,
			const CemodPackage& package, const WupsMetadata& metadata,
			const std::shared_ptr<IWupsRuntimeServices>& services,
			RPLModule*& module, std::uint64_t& lifetimeId, std::string& error) override
		{
			module = nullptr;
			lifetimeId = 0;
			// WUPS plugins can use absolute title addresses as executable trampoline
			// pools (libhookevent is one example). Placing a relocated external WPS
			// immediately after the title's code can overlap such a pool and let the
			// plugin overwrite its own text. Aroma keeps plugin text separate from
			// title code, so reserve equivalent separation before the first WPS map.
			if (!m_reservedTitleCodeGuard)
			{
				constexpr std::uint32_t kTitleCodeGuardSize = 16U * 1024U * 1024U;
				if (RPLLoader_AllocateCodeSpace(kTitleCodeGuardSize, 0x1000) == MPTR_NULL)
				{
					error = "failed to reserve title/WUPS executable-address separation";
					return false;
				}
				m_reservedTitleCodeGuard = true;
			}
			// RPL unload can be rejected (for example when no emulated CPU thread
			// is available). The module then remains registered until a checked
			// retry or title-wide cleanup, so its resolver must not borrow runtime
			// package/metadata storage.
			const auto resolverContext = std::make_shared<const WupsResolverContext>(
				WupsResolverContext{package, metadata});
			RPLLoadOptions options;
			options.callEntrypoint = false;
			// WUPS backend/WUMS imports are resolved by the injected registry and
			// must not become title RPL dependencies. Already-loaded Cafe RPLs
			// remain visible to the ordinary RPL resolution pass.
			options.registerDependency = false;
			options.useApplicationAllocator = true;
			options.allowWupsMarker = true;
			options.allowWumsMarker = false;
			options.owner = owner;
			options.generation = generation;
			options.resolveImport = [services, resolverContext, owner, generation](
				std::string_view importModule, std::string_view symbol, bool isData,
				std::string& resolveError) -> std::optional<MPTR> {
				return services->ResolveImport(resolverContext->package,
					resolverContext->metadata, owner, generation, importModule, symbol,
					isData ? WupsSymbolKind::Data : WupsSymbolKind::Function, resolveError);
			};
			const auto bytes = std::span<const uint8>(
				reinterpret_cast<const uint8*>(image.data()), image.size());
			module = RPLLoader_LoadExternalModuleFromMemory(
				bytes, moduleName, options, lifetimeId, error);
			return module != nullptr && lifetimeId != 0;
		}

	private:
		bool m_reservedTitleCodeGuard{};

	public:

		bool Relocate(RPLModule* module, std::uint64_t lifetimeId,
			std::string& error) override
		{
			return RPLLoader_LinkExternalModule(module, lifetimeId, error);
		}

		bool Invoke(RPLModule* module, std::uint64_t lifetimeId,
			std::uint32_t targetVirtualAddress,
			std::span<const std::uint32_t> argumentWords,
			std::uint32_t& result, std::string& error,
			bool aggregateByReference = false) override
		{
			return WupsGuestCallback::Invoke(module, lifetimeId, targetVirtualAddress,
				argumentWords, result, error, aggregateByReference);
		}

		bool ResolveAddress(RPLModule* module, std::uint64_t lifetimeId,
			std::uint32_t virtualAddress, std::uint32_t size,
			WupsSymbolKind kind, std::uint32_t& mappedAddress,
			std::string& error) override
		{
			error.clear();
			mappedAddress = 0;
			RPLModuleLease lease;
			if (!RPLLoader_AcquireExternalModuleLease(
				module, lifetimeId, lease, error))
				return false;
			const auto addressKind = kind == WupsSymbolKind::Function ?
				RPLModuleAddressKind::Executable :
				RPLModuleAddressKind::Writable;
			MPTR resolved{};
			if (!RPLLoader_ResolveModuleAddress(
				lease, virtualAddress, size, addressKind, resolved))
			{
				error = fmt::format(
					"external RPL virtual address 0x{:08x} is not a mapped {} range",
					virtualAddress,
					kind == WupsSymbolKind::Function ? "executable" : "writable");
				return false;
			}
			mappedAddress = resolved;
			return true;
		}

		bool QueryMappedLayout(RPLModule* module, std::uint64_t lifetimeId,
			WupsMappedLayout& layout, std::string& error) override
		{
			layout = {};
			RPLModuleLease lease;
			if (!RPLLoader_AcquireExternalModuleLease(
				module, lifetimeId, lease, error))
				return false;
			RPLMappedLayoutSnapshot snapshot;
			if (!RPLLoader_QueryMappedLayout(lease, snapshot))
			{
				error = "external RPL mapped layout is unavailable";
				return false;
			}
			layout.textBase = snapshot.textBase;
			layout.dataBase = snapshot.dataBase;
			layout.sections.reserve(snapshot.sections.size());
			for (auto& section : snapshot.sections)
				layout.sections.push_back({std::move(section.name), section.address,
					section.size, section.flags});
			return true;
		}

		bool Unload(RPLModule* module, std::uint64_t lifetimeId,
			std::string& error) override
		{
			return RPLLoader_UnloadExternalModule(module, lifetimeId, error);
		}
	};

	class RplWumsModuleLoader final : public IWumsModuleLoader
	{
	public:
		bool Map(std::span<const std::byte> image,
			std::string_view moduleName, const ModuleProviderOwner& owner,
			WumsImportResolver resolver, RPLModule*& module,
			std::uint64_t& lifetimeId, std::string& error) override
		{
			RPLLoadOptions options;
			options.callEntrypoint = false;
			options.registerDependency = false;
			options.useApplicationAllocator = true;
			options.allowWupsMarker = false;
			options.allowWumsMarker = true;
			options.owner = owner.owner;
			options.generation = owner.generation;
			options.resolveImport =
				[resolver = std::move(resolver)](
					std::string_view importModule,
					std::string_view symbol, bool isData,
					std::string& resolveError) -> std::optional<MPTR> {
					return resolver(importModule, symbol,
						isData ? WupsSymbolKind::Data :
							WupsSymbolKind::Function,
						resolveError);
				};
			const auto bytes = std::span<const uint8>(
				reinterpret_cast<const uint8*>(image.data()), image.size());
			module = RPLLoader_LoadExternalModuleFromMemory(
				bytes, moduleName, options, lifetimeId, error);
			return module != nullptr && lifetimeId != 0;
		}

		bool Link(RPLModule* module, std::uint64_t lifetimeId,
			std::string& error) override
		{
			return RPLLoader_LinkExternalModule(module, lifetimeId, error);
		}

		bool ResolveAddress(RPLModule* module, std::uint64_t lifetimeId,
			std::uint32_t virtualAddress, std::uint32_t size,
			WupsSymbolKind kind, std::uint32_t& mappedAddress,
			std::string& error) override
		{
			error.clear();
			mappedAddress = 0;
			RPLModuleLease lease;
			if (!RPLLoader_AcquireExternalModuleLease(
				module, lifetimeId, lease, error))
				return false;
			MPTR resolved{};
			if (!RPLLoader_ResolveModuleAddress(
				lease, virtualAddress, size,
				kind == WupsSymbolKind::Function ?
					RPLModuleAddressKind::Executable :
					RPLModuleAddressKind::Writable,
				resolved))
			{
				error = fmt::format(
					"WUMS address 0x{:08x} is not a mapped {} range",
					virtualAddress,
					kind == WupsSymbolKind::Function ?
						"executable" : "writable");
				return false;
			}
			mappedAddress = resolved;
			return true;
		}

		bool Invoke(RPLModule* module, std::uint64_t lifetimeId,
			std::uint32_t target,
			std::span<const std::uint32_t> arguments,
			std::uint32_t& result, std::string& error) override
		{
			// WUMS module entrypoints take scalar arguments, not a
			// wups_loader_*_args_t aggregate.
			return WupsGuestCallback::Invoke(
				module, lifetimeId, target, arguments, result, error);
		}

		bool Unload(RPLModule* module, std::uint64_t lifetimeId,
			std::string& error) override
		{
			return RPLLoader_UnloadExternalModule(
				module, lifetimeId, error);
		}
	};

	class RplWumsRuntimeServices final : public IWumsRuntimeServices
	{
	public:
		bool PrepareHook(const WumsInspection& inspection,
			const ModuleProviderOwner&, WumsHookType type,
			WumsHookInvocation& invocation, std::string& error) override
		{
			invocation = {};
			switch (type)
			{
			case WumsHookType::Init:
			case WumsHookType::RelocationsDone:
				error = fmt::format(
					"module '{}' hook {} requires the versioned WUMS "
					"module_information argument provider",
					inspection.metadata.moduleName,
					static_cast<unsigned>(type));
				return false;
			case WumsHookType::InitReentFunctions:
				error = fmt::format(
					"module '{}' INIT_REENT_FUNCTIONS requires the WUPS/WUMS "
					"reent backend",
					inspection.metadata.moduleName);
				return false;
			case WumsHookType::GetCustomRplAllocator:
				error = fmt::format(
					"module '{}' custom RPL allocator cannot be installed by "
					"the application-allocator adapter",
					inspection.metadata.moduleName);
				return false;
			default:
				return true;
			}
		}

		void ReleaseModule(const WumsInspection&,
			const ModuleProviderOwner&) override
		{
		}
	};
}

std::shared_ptr<IWupsModuleLoader> CreateRplWupsModuleLoader()
{
	return std::make_shared<RplWupsModuleLoader>();
}

std::shared_ptr<IWumsModuleLoader> CreateRplWumsModuleLoader()
{
	return std::make_shared<RplWumsModuleLoader>();
}

std::shared_ptr<IWumsRuntimeServices> CreateRplWumsRuntimeServices()
{
	return std::make_shared<RplWumsRuntimeServices>();
}

std::shared_ptr<IWupsRuntimeServices> CreateRplAromaCompatibilityRuntime()
{
	return CreateRplAromaCompatibilityRuntime({});
}

std::shared_ptr<IWupsRuntimeServices> CreateRplAromaCompatibilityRuntime(
	std::shared_ptr<WupsBackendManagementRuntime> management)
{
	auto registry = std::make_shared<ModuleExportRegistry>();
	auto patchPlatform = CreateCemuWupsPatchPlatform();
	auto patchManager =
		std::make_shared<WupsFunctionPatchManager>(patchPlatform);
	auto platform = CreateCemuWupsPlatform();
	AromaRuntimeOptions options;
	auto userDataRoot = ActiveSettings::GetUserDataPath();
	if (userDataRoot.empty())
	{
		std::error_code pathError;
		userDataRoot = std::filesystem::temp_directory_path(pathError);
		if (!pathError)
			userDataRoot /= "cemuextend-user-data";
		else
			userDataRoot.clear();
	}
	if (!userDataRoot.empty())
	{
		options.storageRoot = userDataRoot / "cemuextend/wups/storage";
		options.contentRoots = {
			userDataRoot / "cemuextend/wups/content",
			userDataRoot / "sdcard",
		};
	}
	options.platform = platform;
	options.functionPatcher = CreateWupsFunctionPatcherFacade(
		platform, patchManager);
	options.exportRegistry = registry;
	options.patchManager = patchManager;
	options.patchPlatform = patchPlatform;
	options.backendManagement = std::move(management);
	auto runtime = std::make_shared<AromaCompatibilityRuntime>(
		std::move(options), WupsProcessKind::Game);
	const std::weak_ptr<AromaCompatibilityRuntime> weakRuntime = runtime;
	// Lets coreinit's OSDynLoad_Acquire/FindExport resolve "homebrew_*"
	// virtual modules dynamically, for libwups helper libraries (e.g. the
	// config API) that don't resolve their backend through a static RPL
	// import. See WupsDynLoadInterception.h.
	cafe::wups::SetActiveRuntime(weakRuntime);
	const auto observer = RPLLoader_AddModuleEventObserver(
		[weakRuntime](const RPLModuleEvent& event) {
			const auto locked = weakRuntime.lock();
			if (!locked)
				return;
			if (event.type == RPLModuleEventType::Linked)
				locked->OnModuleLoaded(event.moduleName, event.lifetimeId);
			else if (event.type == RPLModuleEventType::Unloading)
				locked->OnModuleUnloading(event.moduleName, event.lifetimeId);
		});
	runtime->SetModuleEventDetach([observer] {
		RPLLoader_RemoveModuleEventObserver(observer);
	});
	return runtime;
}
