#pragma once

#include "Cafe/HW/Espresso/WupsBinary.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class ModuleProviderKind : std::uint8_t
{
	WupsBackend,
	WumsModule,
	AromaStandard,
	CustomModule,
};

struct ModuleProviderOwner
{
	std::uint64_t owner{};
	std::uint32_t generation{};
	std::uint64_t lifetime{};

	[[nodiscard]] auto operator<=>(const ModuleProviderOwner&) const = default;
};

struct ModuleProviderDescriptor
{
	ModuleProviderKind kind{ModuleProviderKind::CustomModule};
	std::string moduleName;
	std::string version;
	ModuleProviderOwner owner;
};

struct ModuleExportDescriptor
{
	std::string name;
	WupsSymbolKind kind{WupsSymbolKind::Function};
	std::uint32_t address{};
};

struct ModuleProviderHandle
{
	std::uint64_t value{};

	[[nodiscard]] explicit operator bool() const { return value != 0; }
	[[nodiscard]] auto operator<=>(const ModuleProviderHandle&) const = default;
};

class ModuleExportLease
{
public:
	ModuleExportLease();
	~ModuleExportLease();
	ModuleExportLease(ModuleExportLease&&) noexcept;
	ModuleExportLease& operator=(ModuleExportLease&&) noexcept;
	ModuleExportLease(const ModuleExportLease&) = delete;
	ModuleExportLease& operator=(const ModuleExportLease&) = delete;

	[[nodiscard]] explicit operator bool() const;
	[[nodiscard]] std::uint32_t Address() const;
	[[nodiscard]] WupsSymbolKind Kind() const;
	[[nodiscard]] ModuleProviderDescriptor Provider() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;

	explicit ModuleExportLease(std::unique_ptr<Impl> impl);
	friend class ModuleExportRegistry;
};

struct ModuleRegistryDiagnostic
{
	enum class Code : std::uint8_t
	{
		DuplicateProvider,
		DuplicateExport,
		WrongSymbolKind,
		Unresolved,
		ProviderPinned,
		StaleProvider,
	};

	Code code{Code::Unresolved};
	std::string message;
};

// Registry providers are an explicit fallback after Cemu's ordinary Cafe
// RPL/HLE lookup. The registry never contains normal Cafe exports.
class ModuleExportRegistry
{
public:
	ModuleExportRegistry();
	~ModuleExportRegistry();

	ModuleExportRegistry(const ModuleExportRegistry&) = delete;
	ModuleExportRegistry& operator=(const ModuleExportRegistry&) = delete;

	[[nodiscard]] bool Publish(const ModuleProviderDescriptor& provider,
		std::span<const ModuleExportDescriptor> exports,
		ModuleProviderHandle& handle, std::string& error);
	[[nodiscard]] bool Unpublish(ModuleProviderHandle handle,
		const ModuleProviderOwner& owner, std::string& error);
	[[nodiscard]] bool UnpublishOwner(const ModuleProviderOwner& owner,
		std::string& error);

	[[nodiscard]] std::optional<ModuleExportLease> Resolve(
		std::string_view moduleName, std::string_view symbolName,
		WupsSymbolKind kind, const ModuleProviderOwner& requester,
		std::string& error) const;

	[[nodiscard]] bool HasProvider(std::string_view moduleName) const;
	[[nodiscard]] std::optional<ModuleProviderDescriptor> Provider(
		std::string_view moduleName) const;
	[[nodiscard]] std::size_t ProviderCount() const;
	[[nodiscard]] std::size_t PinCount(ModuleProviderHandle handle) const;
	[[nodiscard]] std::vector<ModuleRegistryDiagnostic> Diagnostics() const;

	[[nodiscard]] static unsigned Priority(ModuleProviderKind kind);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
