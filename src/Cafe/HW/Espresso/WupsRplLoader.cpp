#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsRuntime.h"

#include "Cafe/HW/Espresso/WupsGuestCallback.h"
#include "Cafe/OS/RPL/rpl.h"

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

		bool Relocate(RPLModule* module, std::uint64_t lifetimeId,
			std::string& error) override
		{
			return RPLLoader_LinkExternalModule(module, lifetimeId, error);
		}

		bool Invoke(RPLModule* module, std::uint64_t lifetimeId,
			std::uint32_t targetVirtualAddress,
			std::span<const std::uint32_t> argumentWords,
			std::uint32_t& result, std::string& error) override
		{
			return WupsGuestCallback::Invoke(module, lifetimeId, targetVirtualAddress,
				argumentWords, result, error);
		}

		bool Unload(RPLModule* module, std::uint64_t lifetimeId,
			std::string& error) override
		{
			return RPLLoader_UnloadExternalModule(module, lifetimeId, error);
		}
	};
}

std::shared_ptr<IWupsModuleLoader> CreateRplWupsModuleLoader()
{
	return std::make_shared<RplWupsModuleLoader>();
}
