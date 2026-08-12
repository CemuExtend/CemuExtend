#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct RPLModule;

#define RPL_INVALID_HANDLE		0xFFFFFFFF

enum class RPLExternalMarker : uint8
{
	None,
	Wups,
	Wums,
};

enum class RPLModuleEventType : uint8
{
	Mapped,
	Linked,
	Unloading,
	Unloaded,
};

enum class RPLModuleAddressKind : uint8
{
	Readable,
	Writable,
	Executable,
};

struct RPLModuleEvent
{
	RPLModuleEventType type{};
	RPLModule* module{};
	std::string moduleName;
	uint64 lifetimeId{};
	uint64 owner{};
	uint32 generation{};
	bool external{};
};

struct RPLMappedAddressInfo
{
	std::string moduleName;
	uint64 lifetimeId{};
	uint64 owner{};
	uint32 generation{};
	uint32 sectionFlags{};
	bool external{};
};

struct RPLResolvedExport
{
	MPTR address{};
	std::string moduleName;
	uint64 lifetimeId{};
	bool hle{};
};

struct RPLMappedSectionSnapshot
{
	std::string name;
	uint32 address{};
	uint32 size{};
	uint32 flags{};
};

struct RPLMappedLayoutSnapshot
{
	std::vector<RPLMappedSectionSnapshot> sections;
	uint32 textBase{};
	uint32 dataBase{};
};

using RPLExternalImportResolver = std::function<std::optional<MPTR>(
	std::string_view moduleName, std::string_view symbolName, bool isData, std::string& error)>;
using RPLModuleEventCallback = std::function<void(const RPLModuleEvent&)>;

struct RPLLoadOptions
{
	bool callEntrypoint{};
	bool registerDependency{};
	bool useApplicationAllocator{};
	bool allowWupsMarker{};
	bool allowWumsMarker{};
	uint64 owner{};
	uint32 generation{};
	RPLExternalImportResolver resolveImport;
};

// A lease pins one external module lifetime against unload. The RPLModule*
// supplied to acquisition is only an identity token and is never dereferenced
// until the loader has matched both it and lifetimeId under its registry lock.
class RPLModuleLease
{
public:
	RPLModuleLease();
	~RPLModuleLease();
	RPLModuleLease(RPLModuleLease&&) noexcept;
	RPLModuleLease& operator=(RPLModuleLease&&) noexcept;
	RPLModuleLease(const RPLModuleLease&) = delete;
	RPLModuleLease& operator=(const RPLModuleLease&) = delete;

	[[nodiscard]] explicit operator bool() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;

	friend bool RPLLoader_AcquireExternalModuleLease(RPLModule*, uint64,
		RPLModuleLease&, std::string&);
	friend bool RPLLoader_ResolveModuleAddress(const RPLModuleLease&, uint32,
		uint32, RPLModuleAddressKind, MPTR&);
	friend uint32 RPLLoader_GetModuleSDA1Base(const RPLModuleLease&);
	friend uint32 RPLLoader_GetModuleSDA2Base(const RPLModuleLease&);
	friend bool RPLLoader_QueryMappedLayout(const RPLModuleLease&,
		RPLMappedLayoutSnapshot&);
};

void RPLLoader_InitState();
[[nodiscard]] bool RPLLoader_UnloadAll();

uint8* RPLLoader_AllocateTrampolineCodeSpace(sint32 size);

MPTR RPLLoader_AllocateCodeSpace(uint32 size, uint32 alignment);

uint32 RPLLoader_GetMaxCodeOffset();
uint32 RPLLoader_GetDataAllocatorAddr();

RPLModule* RPLLoader_LoadFromMemory(uint8* rplData, sint32 size, std::string_view name);
RPLModule* RPLLoader_LoadExternalModuleFromMemory(std::span<const uint8> image,
	std::string_view name, const RPLLoadOptions& options, uint64& lifetimeId,
	std::string& error);
bool RPLLoader_LinkExternalModule(RPLModule* module, uint64 lifetimeId,
	std::string& error);
bool RPLLoader_UnloadExternalModule(RPLModule* module, uint64 lifetimeId,
	std::string& error);
bool RPLLoader_AcquireExternalModuleLease(RPLModule* module, uint64 lifetimeId,
	RPLModuleLease& lease, std::string& error);
bool RPLLoader_ResolveModuleAddress(const RPLModuleLease& lease, uint32 virtualAddress,
	uint32 size, RPLModuleAddressKind kind, MPTR& mappedAddress);
bool RPLLoader_QueryMappedLayout(const RPLModuleLease& lease,
	RPLMappedLayoutSnapshot& layout);
bool RPLLoader_QueryMappedAddress(uint32 address, uint32 size,
	RPLMappedAddressInfo& info);
bool RPLLoader_FindLoadedExport(std::string_view moduleName,
	std::string_view symbolName, bool isData, RPLResolvedExport& result);
bool RPLLoader_IsModuleAlive(const RPLModule* module, uint64 lifetimeId);
uint32 RPLLoader_GetModuleSDA1Base(const RPLModuleLease& lease);
uint32 RPLLoader_GetModuleSDA2Base(const RPLModuleLease& lease);
uint64 RPLLoader_AddModuleEventObserver(RPLModuleEventCallback callback);
bool RPLLoader_RemoveModuleEventObserver(uint64 observerId);
uint32 rpl_mapHLEImport(RPLModule* rplLoaderContext, const char* rplName, const char* funcName, bool functionMustExist);
void RPLLoader_Link();
void RPLLoader_InstallLegacyCoreinitEntrypoints();

MPTR RPLLoader_FindRPLExport(RPLModule* rplLoaderContext, const char* symbolName, bool isData);
uint32 RPLLoader_GetModuleEntrypoint(RPLModule* rplLoaderContext);

void RPLLoader_SetMainModule(RPLModule* rplLoaderContext);
uint32 RPLLoader_GetMainModuleHandle();

void RPLLoader_CallEntrypoints();
void RPLLoader_CallCoreinitEntrypoint();
void RPLLoader_NotifyControlPassedToApplication();

void RPLLoader_AddDependency(std::string_view name, bool isMainExecutable = false);
void RPLLoader_RemoveDependency(uint32 handle);
bool RPLLoader_HasDependency(std::string_view name);
void RPLLoader_UpdateDependencies();

void RPLLoader_LoadCoreinit();

uint32 RPLLoader_GetHandleByModuleName(const char* name);
uint32 RPLLoader_GetMaxTLSModuleIndex();
bool RPLLoader_GetTLSDataByTLSIndex(sint16 tlsModuleIndex, uint8** tlsData, sint32* tlsSize);
sint16 RPLLoader_FindTLSModuleIndexByDataAddr(uint32 address);

// Detects and undoes writes into module sections that must stay constant after
// linking. Verification is cheap enough to run a few times per second.
void RPLLoader_VerifyReadOnlyData();

uint32 RPLLoader_FindModuleOrHLEExport(uint32 moduleHandle, bool isData, const char* exportName);

uint32 RPLLoader_GetSDA1Base();
uint32 RPLLoader_GetSDA2Base();

sint32 RPLLoader_GetModuleCount();
RPLModule** RPLLoader_GetModuleList();

MEMPTR<void> RPLLoader_AllocateCodeCaveMem(uint32 alignment, uint32 size);
void RPLLoader_ReleaseCodeCaveMem(MEMPTR<void> addr);

// exports

uint32 RPLLoader_MakePPCCallable(void(*ppcCallableExport)(struct PPCInterpreter_t* hCPU));

// elf loader

uint32 ELF_LoadFromMemory(uint8* elfData, sint32 size, const char* name);
