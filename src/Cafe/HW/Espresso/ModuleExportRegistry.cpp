#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/ModuleExportRegistry.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace
{
	bool SafeIdentifier(std::string_view value, std::size_t maximum)
	{
		return !value.empty() && value.size() <= maximum &&
			std::ranges::all_of(value, [](unsigned char character) {
				return std::isalnum(character) || character == '_' ||
					character == '.' || character == '-';
			});
	}

	std::string SymbolKindName(WupsSymbolKind kind)
	{
		return kind == WupsSymbolKind::Function ? "function" : "data";
	}

	struct ExportKey
	{
		std::string module;
		std::string symbol;
		WupsSymbolKind kind{};

		[[nodiscard]] auto operator<=>(const ExportKey&) const = default;
	};

	struct RegistryProviderRecord
	{
		ModuleProviderHandle handle;
		ModuleProviderDescriptor descriptor;
		std::map<std::pair<std::string, WupsSymbolKind>, std::uint32_t> exports;
		std::atomic_size_t pins{};
		bool active{true};
	};
}

struct ModuleExportRegistry::Impl
{
	mutable std::mutex mutex;
	std::map<std::uint64_t, std::shared_ptr<RegistryProviderRecord>> providers;
	std::map<std::string, std::vector<std::shared_ptr<RegistryProviderRecord>>,
		std::less<>> modules;
	std::map<ExportKey, std::shared_ptr<RegistryProviderRecord>> exports;
	std::vector<ModuleRegistryDiagnostic> diagnostics;
	std::uint64_t nextHandle{1};
};

struct ModuleExportLease::Impl
{
	Impl(std::shared_ptr<RegistryProviderRecord> provider_,
		std::uint32_t address_, WupsSymbolKind kind_) :
		provider(std::move(provider_)), address(address_), kind(kind_)
	{
		++provider->pins;
	}

	~Impl()
	{
		--provider->pins;
	}

	std::shared_ptr<RegistryProviderRecord> provider;
	std::uint32_t address{};
	WupsSymbolKind kind{};
};

ModuleExportLease::ModuleExportLease() = default;
ModuleExportLease::~ModuleExportLease() = default;
ModuleExportLease::ModuleExportLease(ModuleExportLease&&) noexcept = default;
ModuleExportLease& ModuleExportLease::operator=(ModuleExportLease&&) noexcept = default;

ModuleExportLease::ModuleExportLease(std::unique_ptr<Impl> impl) :
	m_impl(std::move(impl))
{
}

ModuleExportLease::operator bool() const
{
	return m_impl != nullptr;
}

std::uint32_t ModuleExportLease::Address() const
{
	return m_impl ? m_impl->address : 0;
}

WupsSymbolKind ModuleExportLease::Kind() const
{
	return m_impl ? m_impl->kind : WupsSymbolKind::Function;
}

ModuleProviderDescriptor ModuleExportLease::Provider() const
{
	return m_impl ? m_impl->provider->descriptor : ModuleProviderDescriptor{};
}

ModuleExportRegistry::ModuleExportRegistry() :
	m_impl(std::make_unique<Impl>())
{
}

ModuleExportRegistry::~ModuleExportRegistry() = default;

unsigned ModuleExportRegistry::Priority(ModuleProviderKind kind)
{
	switch (kind)
	{
	case ModuleProviderKind::WupsBackend: return 0;
	case ModuleProviderKind::WumsModule: return 1;
	case ModuleProviderKind::AromaStandard: return 2;
	case ModuleProviderKind::CustomModule: return 3;
	}
	return std::numeric_limits<unsigned>::max();
}

bool ModuleExportRegistry::Publish(const ModuleProviderDescriptor& provider,
	std::span<const ModuleExportDescriptor> exports,
	ModuleProviderHandle& handle, std::string& error)
{
	error.clear();
	handle = {};
	if (!SafeIdentifier(provider.moduleName, 128) || provider.owner.owner == 0 ||
		provider.owner.generation == 0 || provider.owner.lifetime == 0)
	{
		error = "module provider has an invalid name or owner/generation/lifetime";
		return false;
	}

	std::map<std::pair<std::string, WupsSymbolKind>, std::uint32_t> checkedExports;
	for (const auto& symbol : exports)
	{
		if (!SafeIdentifier(symbol.name, 1024) || symbol.address == 0 ||
			(symbol.kind == WupsSymbolKind::Function && (symbol.address & 3U) != 0))
		{
			error = fmt::format("provider '{}' has invalid {} export '{}'",
				provider.moduleName, SymbolKindName(symbol.kind), symbol.name);
			return false;
		}
		if (!checkedExports.emplace(
			std::pair{symbol.name, symbol.kind}, symbol.address).second)
		{
			error = fmt::format("provider '{}' contains duplicate {} export '{}'",
				provider.moduleName, SymbolKindName(symbol.kind), symbol.name);
			return false;
		}
	}

	std::lock_guard lock(m_impl->mutex);
	if (m_impl->nextHandle == 0)
	{
		error = "module provider handle space is exhausted";
		return false;
	}
	if (const auto found = m_impl->modules.find(provider.moduleName);
		found != m_impl->modules.end())
	{
		for (const auto& existing : found->second)
		{
			if (existing->active &&
				(existing->descriptor.kind == provider.kind ||
					existing->descriptor.owner == provider.owner))
			{
				error = fmt::format(
					"duplicate provider '{}' (existing owner {} generation {} lifetime {})",
					provider.moduleName, existing->descriptor.owner.owner,
					existing->descriptor.owner.generation,
					existing->descriptor.owner.lifetime);
				m_impl->diagnostics.push_back({
					ModuleRegistryDiagnostic::Code::DuplicateProvider, error});
				return false;
			}
		}
	}
	for (const auto& [key, address] : checkedExports)
	{
		const ExportKey exportKey{provider.moduleName, key.first, key.second};
		if (const auto existing = m_impl->exports.find(exportKey);
			existing != m_impl->exports.end() && existing->second->active)
		{
			error = fmt::format(
				"ambiguous {} export '{}.{}' from providers owned by {} and {}",
				SymbolKindName(key.second), provider.moduleName, key.first,
				existing->second->descriptor.owner.owner, provider.owner.owner);
			m_impl->diagnostics.push_back({
				ModuleRegistryDiagnostic::Code::DuplicateExport, error});
			return false;
		}
	}

	auto record = std::make_shared<RegistryProviderRecord>();
	record->handle = ModuleProviderHandle{m_impl->nextHandle++};
	record->descriptor = provider;
	record->exports = std::move(checkedExports);
	m_impl->providers.emplace(record->handle.value, record);
	auto& moduleProviders = m_impl->modules[provider.moduleName];
	moduleProviders.push_back(record);
	std::ranges::sort(moduleProviders, [](const auto& left, const auto& right) {
		const auto leftPriority = Priority(left->descriptor.kind);
		const auto rightPriority = Priority(right->descriptor.kind);
		if (leftPriority != rightPriority)
			return leftPriority < rightPriority;
		return left->handle.value < right->handle.value;
	});
	for (const auto& [key, address] : record->exports)
		m_impl->exports.emplace(
			ExportKey{provider.moduleName, key.first, key.second}, record);
	handle = record->handle;
	return true;
}

bool ModuleExportRegistry::Unpublish(ModuleProviderHandle handle,
	const ModuleProviderOwner& owner, std::string& error)
{
	error.clear();
	std::lock_guard lock(m_impl->mutex);
	const auto found = m_impl->providers.find(handle.value);
	if (found == m_impl->providers.end() ||
		found->second->descriptor.owner != owner || !found->second->active)
	{
		error = "module provider removal rejected a stale handle or owner generation";
		m_impl->diagnostics.push_back({
			ModuleRegistryDiagnostic::Code::StaleProvider, error});
		return false;
	}
	const auto& record = found->second;
	const auto pinCount = record->pins.load();
	if (pinCount != 0)
	{
		error = fmt::format(
			"provider '{}' owner {} generation {} lifetime {} is pinned by {} import(s)",
			record->descriptor.moduleName, owner.owner, owner.generation,
			owner.lifetime, pinCount);
		m_impl->diagnostics.push_back({
			ModuleRegistryDiagnostic::Code::ProviderPinned, error});
		return false;
	}
	record->active = false;
	for (const auto& [key, address] : record->exports)
		m_impl->exports.erase(
			ExportKey{record->descriptor.moduleName, key.first, key.second});
	if (const auto module = m_impl->modules.find(record->descriptor.moduleName);
		module != m_impl->modules.end())
	{
		std::erase(module->second, record);
		if (module->second.empty())
			m_impl->modules.erase(module);
	}
	m_impl->providers.erase(found);
	return true;
}

bool ModuleExportRegistry::UnpublishOwner(const ModuleProviderOwner& owner,
	std::string& error)
{
	std::vector<ModuleProviderHandle> handles;
	{
		std::lock_guard lock(m_impl->mutex);
		for (const auto& [value, provider] : m_impl->providers)
			if (provider->descriptor.owner == owner)
				handles.push_back(provider->handle);
	}
	error.clear();
	bool success = true;
	for (const auto handle : std::ranges::reverse_view(handles))
	{
		std::string providerError;
		if (!Unpublish(handle, owner, providerError))
		{
			success = false;
			error.append(error.empty() ? "" : "; ").append(providerError);
		}
	}
	return success;
}

std::optional<ModuleExportLease> ModuleExportRegistry::Resolve(
	std::string_view moduleName, std::string_view symbolName,
	WupsSymbolKind kind, const ModuleProviderOwner& requester,
	std::string& error) const
{
	error.clear();
	if (requester.owner == 0 || requester.generation == 0 ||
		moduleName.empty() || symbolName.empty())
	{
		error = "module registry resolve request has invalid importer identity or name";
		return std::nullopt;
	}
	std::lock_guard lock(m_impl->mutex);
	const ExportKey key{std::string(moduleName), std::string(symbolName), kind};
	if (const auto found = m_impl->exports.find(key);
		found != m_impl->exports.end() && found->second->active)
	{
		const auto address = found->second->exports.at(
			std::pair{std::string(symbolName), kind});
		return ModuleExportLease(std::make_unique<ModuleExportLease::Impl>(
			found->second, address, kind));
	}
	const auto otherKind = kind == WupsSymbolKind::Function ?
		WupsSymbolKind::Data : WupsSymbolKind::Function;
	if (const auto wrong = m_impl->exports.find(
		ExportKey{std::string(moduleName), std::string(symbolName), otherKind});
		wrong != m_impl->exports.end() && wrong->second->active)
	{
		error = fmt::format(
			"registry symbol '{}.{}' is {}, not the requested {} (importer owner {} generation {})",
			moduleName, symbolName, SymbolKindName(otherKind), SymbolKindName(kind),
			requester.owner, requester.generation);
		m_impl->diagnostics.push_back({
			ModuleRegistryDiagnostic::Code::WrongSymbolKind, error});
		return std::nullopt;
	}
	error = fmt::format(
		"registry has no {} export '{}.{}' for importer owner {} generation {}",
		SymbolKindName(kind), moduleName, symbolName,
		requester.owner, requester.generation);
	m_impl->diagnostics.push_back({
		ModuleRegistryDiagnostic::Code::Unresolved, error});
	return std::nullopt;
}

bool ModuleExportRegistry::HasProvider(std::string_view moduleName) const
{
	std::lock_guard lock(m_impl->mutex);
	const auto found = m_impl->modules.find(moduleName);
	return found != m_impl->modules.end() && !found->second.empty();
}

std::optional<ModuleProviderDescriptor> ModuleExportRegistry::Provider(
	std::string_view moduleName) const
{
	std::lock_guard lock(m_impl->mutex);
	const auto found = m_impl->modules.find(moduleName);
	if (found == m_impl->modules.end() || found->second.empty())
		return std::nullopt;
	return found->second.front()->descriptor;
}

std::size_t ModuleExportRegistry::ProviderCount() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->providers.size();
}

std::size_t ModuleExportRegistry::PinCount(ModuleProviderHandle handle) const
{
	std::lock_guard lock(m_impl->mutex);
	const auto found = m_impl->providers.find(handle.value);
	return found == m_impl->providers.end() ? 0 : found->second->pins.load();
}

std::vector<ModuleRegistryDiagnostic> ModuleExportRegistry::Diagnostics() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->diagnostics;
}
