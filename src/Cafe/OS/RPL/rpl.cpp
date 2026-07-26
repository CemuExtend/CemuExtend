#include <zlib.h>

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/RPLExternalModulePolicy.h"
#include "Cafe/OS/RPL/RPLTLSMapping.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "util/ChunkedHeap/ChunkedHeap.h"

#include "util/crypto/crc32.h"
#include "config/ActiveSettings.h"
#include "Cafe/OS/libs/coreinit/coreinit_DynLoad.h"
#include "COSModule.h"

#include <atomic>
#include <mutex>

class PPCCodeHeap : public VHeap
{
public:
	PPCCodeHeap(void* heapBase, uint32 heapSize) : VHeap(heapBase, heapSize) { };

	void* alloc(uint32 size, uint32 alignment = 4) override
	{
		return VHeap::alloc(size, alignment);
	}

	void free(void* addr) override
	{
		uint32 allocSize = getAllocationSizeFromAddr(addr);
		MPTR ppcAddr = memory_getVirtualOffsetFromPointer(addr);
		PPCRecompiler_invalidateRange(ppcAddr, ppcAddr + allocSize);
		VHeap::free(addr);
	}
};

VHeap rplLoaderHeap_workarea(nullptr, MEMORY_RPLLOADER_AREA_SIZE);
PPCCodeHeap rplLoaderHeap_lowerAreaCodeMem2(nullptr, MEMORY_CODE_TRAMPOLINE_AREA_SIZE);
PPCCodeHeap rplLoaderHeap_codeArea2(nullptr, MEMORY_CODEAREA_SIZE);

ChunkedFlatAllocator<64 * 1024> g_heapTrampolineArea;

std::vector<RPLDependency*> rplDependencyList;

RPLModule* rplModuleList[256];
sint32 rplModuleCount = 0;

bool rplLoader_applicationHasMemoryControl = false;
uint32 rplLoader_maxCodeAddress = 0; // highest used code address
uint32 rplLoader_currentTLSModuleIndex = 1; // value 0 is reserved
uint32 rplLoader_currentHandleCounter = 0x00001000;
sint16 rplLoader_currentTlsModuleIndex = 0x0001;
RPLModule* rplLoader_mainModule = nullptr;
uint32 rplLoader_sdataAddr = MPTR_NULL; // r13
uint32 rplLoader_sdata2Addr = MPTR_NULL; // r2
uint32 rplLoader_currentDataAllocatorAddr = 0x10000000;

std::atomic_uint64_t g_rplModuleLifetimeCounter{1};
std::recursive_mutex g_rplLoaderMutex;
std::mutex g_rplModuleEventMutex;
std::map<uint64, RPLModuleEventCallback> g_rplModuleEventObservers;
uint64 g_rplModuleEventObserverCounter = 1;
thread_local std::vector<uint8> g_rplTlsTemplateScratch;

std::map<void(*)(PPCInterpreter_t* hCPU), uint32> g_map_callableExports;

struct RPLMappingRegion
{
	MPTR baseAddress;
	uint32 endAddress;
	uint32 calcEndAddress; // used to verify endAddress
};

struct RPLRegionMappingTable
{
	RPLMappingRegion region[4];
};

#define RPL_MAPPING_REGION_DATA			0
#define RPL_MAPPING_REGION_LOADERINFO	1
#define RPL_MAPPING_REGION_TEXT			2
#define RPL_MAPPING_REGION_TEMP			3

void RPLLoader_UnloadModule(RPLDependency* rplDependency, bool skipPPCCalls);
void RPLLoader_RemoveDependency(std::string_view name);
void RPLLoader_DestroyModule(RPLModule* rpl, RPLDependency* rplDependency,
	bool skipPPCCalls, bool releaseData);

RPLModule* RPLLoader_FindLiveModule(const RPLModule* identity, uint64 lifetimeId)
{
	if (!identity || lifetimeId == 0)
		return nullptr;
	for (sint32 index = 0; index < rplModuleCount; ++index)
	{
		RPLModule* registered = rplModuleList[index];
		if (registered == identity && registered->externalLifetimeId == lifetimeId)
			return registered;
	}
	return nullptr;
}

uint64 RPLLoader_AllocateLifetimeId()
{
	uint64 candidate = g_rplModuleLifetimeCounter.load(std::memory_order_relaxed);
	while (candidate != std::numeric_limits<uint64>::max())
		if (g_rplModuleLifetimeCounter.compare_exchange_weak(candidate, candidate + 1,
			std::memory_order_relaxed))
			return candidate;
	return 0;
}

struct RPLModuleLease::Impl
{
	explicit Impl(RPLModule* module_) :
		module(module_)
	{
		++module->externalAccessCount;
	}

	~Impl()
	{
		std::lock_guard lock(g_rplLoaderMutex);
		cemu_assert_debug(module->externalAccessCount != 0);
		if (module->externalAccessCount != 0)
			--module->externalAccessCount;
	}

	RPLModule* module;
};

RPLModuleLease::RPLModuleLease() = default;
RPLModuleLease::~RPLModuleLease() = default;
RPLModuleLease::RPLModuleLease(RPLModuleLease&&) noexcept = default;
RPLModuleLease& RPLModuleLease::operator=(RPLModuleLease&&) noexcept = default;

RPLModuleLease::operator bool() const
{
	return m_impl != nullptr;
}

class RPLExternalEventGuard
{
public:
	explicit RPLExternalEventGuard(RPLModule* module_) :
		module(module_ && module_->externalModule ? module_ : nullptr),
		previous(module ? module->externalEventInFlight : false)
	{
		if (module)
			module->externalEventInFlight = true;
	}

	~RPLExternalEventGuard()
	{
		if (module)
			module->externalEventInFlight = previous;
	}

private:
	RPLModule* module;
	bool previous;
};

void RPLLoader_EmitModuleEvent(RPLModuleEventType type, RPLModule* module)
{
	std::lock_guard loaderLock(g_rplLoaderMutex);
	RPLModuleEvent event{
		type,
		type == RPLModuleEventType::Unloaded ? nullptr : module,
		module ? module->moduleName : std::string{},
		module ? module->externalLifetimeId : 0,
		module ? module->externalOwner : 0,
		module ? module->externalGeneration : 0,
		module && module->externalModule,
	};
	std::vector<RPLModuleEventCallback> callbacks;
	{
		std::lock_guard lock(g_rplModuleEventMutex);
		callbacks.reserve(g_rplModuleEventObservers.size());
		for (const auto& [id, callback] : g_rplModuleEventObservers)
			callbacks.push_back(callback);
	}
	RPLExternalEventGuard eventGuard(module);
	for (const auto& callback : callbacks)
	{
		try
		{
			callback(event);
		}
		catch (const std::exception& exception)
		{
			cemuLog_log(LogType::Force,
				"RPLLoader: module event observer threw an exception: {}", exception.what());
		}
		catch (...)
		{
			cemuLog_log(LogType::Force,
				"RPLLoader: module event observer threw a non-standard exception");
		}
	}
}

uint8* RPLLoader_AllocateTrampolineCodeSpace(RPLModule* rplLoaderContext, sint32 size)
{	
	if (rplLoaderContext)
	{
		// allocation owned by rpl
		return (uint8*)rplLoaderContext->heapTrampolineArea.alloc(size, 4);
	}
	// allocation owned by global context
	auto result = (uint8*)g_heapTrampolineArea.alloc(size, 4);
	rplLoader_maxCodeAddress = std::max(rplLoader_maxCodeAddress, memory_getVirtualOffsetFromPointer(g_heapTrampolineArea.getCurrentBlockPtr()) + g_heapTrampolineArea.getCurrentBlockOffset());
	return result;
}

uint8* RPLLoader_AllocateTrampolineCodeSpace(sint32 size)
{
	return RPLLoader_AllocateTrampolineCodeSpace(nullptr, size);
}

MPTR RPLLoader_AllocateCodeSpace(uint32 size, uint32 alignment)
{
	cemu_assert_debug((alignment & (alignment - 1)) == 0); // alignment must be a power of 2
	MPTR codeAddr = memory_getVirtualOffsetFromPointer(rplLoaderHeap_codeArea2.alloc(size, alignment));
	rplLoader_maxCodeAddress = std::max(rplLoader_maxCodeAddress, codeAddr + size);
	PPCRecompiler_allocateRange(codeAddr, size);
	return codeAddr;
}

uint32 RPLLoader_AllocateDataSpace(RPLModule* rpl, uint32 size, uint32 alignment)
{
	if (rplLoader_applicationHasMemoryControl &&
		(!rpl->externalModule || rpl->externalUseApplicationAllocator))
	{
		StackAllocator<uint32be> memPtr;
		*(memPtr.GetPointer()) = 0;
		PPCCoreCallback(rpl->funcAlloc.value(), size, alignment, memPtr.GetPointer());
		return (uint32)*(memPtr.GetPointer());
	}
	if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
		rplLoader_currentDataAllocatorAddr > std::numeric_limits<uint32>::max() - (alignment - 1))
		return MPTR_NULL;
	rplLoader_currentDataAllocatorAddr = (rplLoader_currentDataAllocatorAddr + alignment - 1) & ~(alignment - 1);
	if (size > std::numeric_limits<uint32>::max() - rplLoader_currentDataAllocatorAddr)
		return MPTR_NULL;
	uint32 mem = rplLoader_currentDataAllocatorAddr;
	rplLoader_currentDataAllocatorAddr += size;
	return mem;
}

void RPLLoader_FreeData(RPLModule* rpl, void* ptr)
{
	if (!ptr || !rpl->funcFree)
		return;
	PPCCoreCallback(rpl->funcFree.value(), ptr);
}

uint32 RPLLoader_GetDataAllocatorAddr()
{
	return (rplLoader_currentDataAllocatorAddr + 0xFFF) & (~0xFFF);
}

uint32 RPLLoader_GetMaxCodeOffset()
{
	return rplLoader_maxCodeAddress;
}

#define PPCASM_OPC_R_TEMPL_SIMM(_rD, _rA, _IMM) (((_rD)<<21)|((_rA)<<16)|((_IMM)&0xFFFF))

// generates 32-bit jump. Modifies R11 and CTR
MPTR _generateTrampolineFarJump(RPLModule* rplLoaderContext, MPTR destAddr)
{
	auto itr = rplLoaderContext->trampolineMap.find(destAddr);
	if (itr != rplLoaderContext->trampolineMap.end())
		return itr->second;

	MPTR trampolineAddr = memory_getVirtualOffsetFromPointer(RPLLoader_AllocateTrampolineCodeSpace(rplLoaderContext, 4*4));
	uint32 destAddrU32 = (uint32)destAddr;
	uint32 ppcOpcode = 0;
	// ADDI R11, R0, ...
	ppcOpcode = PPCASM_OPC_R_TEMPL_SIMM(11, 0, destAddrU32 & 0xFFFF);
	ppcOpcode |= (14 << 26);
	memory_writeU32(trampolineAddr + 0x0, ppcOpcode);
	// ADDIS R11, R11, ...<<16
	ppcOpcode = PPCASM_OPC_R_TEMPL_SIMM(11, 11, ((destAddrU32 >> 16) + ((destAddrU32 >> 15) & 1)) & 0xFFFF);
	ppcOpcode |= (15 << 26);
	memory_writeU32(trampolineAddr + 0x4, ppcOpcode);
	// MTCTR r11
	memory_writeU32(trampolineAddr + 0x8, 0x7D6903A6);
	// BCTR
	memory_writeU32(trampolineAddr + 0xC, 0x4E800420);
	// if the destination is a known symbol, create a proxy (duplicate) symbol at the jump
	rplSymbolStorage_createJumpProxySymbol(trampolineAddr, destAddr);
	rplLoaderContext->trampolineMap.emplace(destAddr, trampolineAddr);
	return trampolineAddr;
}

void* RPLLoader_AllocWorkarea(uint32 size, uint32 alignment, uint32* allocSize)
{
	size = (size + 31)&~31;
	*allocSize = size;
	void* allocAddr = rplLoaderHeap_workarea.alloc(size, alignment);
	cemu_assert(allocAddr != nullptr);
	memset(allocAddr, 0, size);
	return allocAddr;
}

void RPLLoader_FreeWorkarea(void* allocAddr)
{
	rplLoaderHeap_workarea.free(allocAddr);
}

bool RPLLoader_CheckBounds(RPLModule* rplLoaderContext, uint32 offset, uint32 size)
{
	if ((offset + size) > rplLoaderContext->RPLRawData.size_bytes())
		return false;
	return true;
}

bool RPLLoader_ProcessHeaders(std::string_view moduleName, uint8* rplData, uint32 rplSize, RPLModule** rplLoaderContextOut)
{
	rplHeaderNew_t* rplHeader = (rplHeaderNew_t*)rplData;
	*rplLoaderContextOut = nullptr;
	if (rplHeader->version04 != 0x01)
		return false;
	if (rplHeader->ukn05 != 0x02)
		return false;
	if (rplHeader->magic2_0 != 0xCA)
		return false;
	if (rplHeader->magic2_1 != 0xFE)
		return false;
	if (rplHeader->ukn06 > 1)
		return false;
	if (rplHeader->ukn12 != 0x14)
		return false;
	if (rplHeader->ukn14 != 0x01)
		return false;
	if (rplHeader->sectionTableEntryCount < 2)
		return false; // RPL must end with two sections: CRCS + FILEINFO
	// setup RPL info struct
	RPLModule* rplLoaderContext = new RPLModule();
	rplLoaderContext->RPLRawData = std::span<uint8>(rplData, rplSize);
	rplLoaderContext->heapTrampolineArea.setBaseAllocator(&rplLoaderHeap_lowerAreaCodeMem2);
	// load section table
	if ((uint32)rplHeader->sectionTableEntrySize != sizeof(rplSectionEntryNew_t))
		assert_dbg();
	sint32 sectionCount = (sint32)rplHeader->sectionTableEntryCount;
	sint32 sectionTableSize = (sint32)rplHeader->sectionTableEntrySize * sectionCount;
	rplLoaderContext->sectionTablePtr = (rplSectionEntryNew_t*)malloc(sectionTableSize);
	memcpy(rplLoaderContext->sectionTablePtr, rplData + (uint32)(rplHeader->sectionTableOffset), sectionTableSize);
	rplLoaderContext->debugSectionLoadMask.resize(sectionCount);
	// copy rpl header
	memcpy(&rplLoaderContext->rplHeader, rplHeader, sizeof(rplHeaderNew_t));
	// verify that section n-1 is FILEINFO
	rplSectionEntryNew_t* fileinfoSection = rplLoaderContext->sectionTablePtr + ((uint32)rplLoaderContext->rplHeader.sectionTableEntryCount - 1);
	if (fileinfoSection->fileOffset == 0 || (uint32)fileinfoSection->fileOffset >= rplSize || (uint32)fileinfoSection->type != SHT_RPL_FILEINFO)
	{
		cemuLog_logDebug(LogType::Force, "RPLLoader: Last section not FILEINFO");
	}
	// verify that section n-2 is CRCs
	rplSectionEntryNew_t* crcSection = rplLoaderContext->sectionTablePtr + ((uint32)rplLoaderContext->rplHeader.sectionTableEntryCount - 2);
	if (crcSection->fileOffset == 0 || (uint32)crcSection->fileOffset >= rplSize || (uint32)crcSection->type != SHT_RPL_CRCS)
	{
		cemuLog_logDebug(LogType::Force, "RPLLoader: The section before FILEINFO must be CRCs");
	}
	// load FILEINFO section
	if (fileinfoSection->sectionSize < sizeof(RPLFileInfoData))
	{
		cemuLog_log(LogType::Force, "RPLLoader: FILEINFO section size is below expected size");
		delete rplLoaderContext;
		return false;
	}

	// read RPL mapping info
	uint8* fileInfoRawPtr = (uint8*)(rplData + fileinfoSection->fileOffset);
	if (((uint64)fileinfoSection->fileOffset+fileinfoSection->sectionSize) > (uint64)rplSize)
	{
		cemuLog_log(LogType::Force, "RPLLoader: FILEINFO section outside of RPL file bounds");
		return false;
	}
	rplLoaderContext->sectionData_fileInfo.resize(fileinfoSection->sectionSize);
	memcpy(rplLoaderContext->sectionData_fileInfo.data(), fileInfoRawPtr, rplLoaderContext->sectionData_fileInfo.size());

	RPLFileInfoData* fileInfoPtr = (RPLFileInfoData*)rplLoaderContext->sectionData_fileInfo.data();
	if (fileInfoPtr->fileInfoMagic != 0xCAFE0402)
	{
		cemuLog_log(LogType::Force, "RPLLoader: Invalid FILEINFO magic");
		return false;
	}

	// process FILEINFO
	rplLoaderContext->fileInfo.textRegionSize = fileInfoPtr->textRegionSize;
	rplLoaderContext->fileInfo.dataRegionSize = fileInfoPtr->dataRegionSize;
	rplLoaderContext->fileInfo.baseAlign = fileInfoPtr->baseAlign;
	rplLoaderContext->fileInfo.ukn14 = fileInfoPtr->ukn14;
	rplLoaderContext->fileInfo.trampolineAdjustment = fileInfoPtr->trampolineAdjustment;
	rplLoaderContext->fileInfo.ukn4C = fileInfoPtr->ukn4C;
	rplLoaderContext->fileInfo.tlsModuleIndex = fileInfoPtr->tlsModuleIndex;
	rplLoaderContext->fileInfo.sdataBase1 = fileInfoPtr->sdataBase1;
	rplLoaderContext->fileInfo.sdataBase2 = fileInfoPtr->sdataBase2;
	rplLoaderContext->fileInfo.flags = fileInfoPtr->flags;

	// init section address table
	rplLoaderContext->sectionAddressTable2.resize(sectionCount);
	// init modulename
	rplLoaderContext->moduleName.assign(moduleName);

	// load CRC section
	uint32 crcTableExpectedSize = sectionCount * sizeof(uint32be);
	if (!RPLLoader_CheckBounds(rplLoaderContext, crcSection->fileOffset, crcTableExpectedSize))
	{
		cemuLog_log(LogType::Force, "RPLLoader: CRC section outside of RPL file bounds");
		crcSection->sectionSize = 0;
	}
	else if (crcSection->sectionSize < crcTableExpectedSize)
	{
		cemuLog_log(LogType::Force, "RPLLoader: CRC section size (0x{:x}) less than required (0x{:x})", (uint32)crcSection->sectionSize, crcTableExpectedSize);
	}
	else if (crcSection->sectionSize != crcTableExpectedSize)
	{
		cemuLog_log(LogType::Force, "RPLLoader: CRC section size (0x{:x}) does not match expected size (0x{:x})", (uint32)crcSection->sectionSize, crcTableExpectedSize);
	}

	uint32 crcActualSectionCount = crcSection->sectionSize / sizeof(uint32); // how many CRCs are actually stored

	rplLoaderContext->crcTable.resize(sectionCount);
	if (crcActualSectionCount > 0)
	{
		uint32be* crcTableData = (uint32be*)(rplData + crcSection->fileOffset);
		for (uint32 i = 0; i < crcActualSectionCount; i++)
			rplLoaderContext->crcTable[i] = crcTableData[i];
	}

	// verify CRC of FILEINFO section
	uint32 crcCalcFileinfo = crc32_calc(0, rplLoaderContext->sectionData_fileInfo.data(), rplLoaderContext->sectionData_fileInfo.size());
	uint32 crcFileinfo = rplLoaderContext->GetSectionCRC(sectionCount - 1);
	if (crcCalcFileinfo != crcFileinfo)
	{
		cemuLog_log(LogType::Force, "RPLLoader: FILEINFO section has CRC mismatch - Calculated: {:08x} Actual: {:08x}", crcCalcFileinfo, crcFileinfo);
	}

	rplLoaderContext->sectionAddressTable2[sectionCount - 1].ptr = rplLoaderContext->sectionData_fileInfo.data();
	rplLoaderContext->sectionAddressTable2[sectionCount - 2].ptr = nullptr;// rplLoaderContext->crcTablePtr;

	// set output
	*rplLoaderContextOut = rplLoaderContext;
	return true;
}

class RPLUncompressedSection
{
public:
	std::vector<uint8> sectionData;
};

rplSectionEntryNew_t* RPLLoader_GetSection(RPLModule* rplLoaderContext, sint32 sectionIndex)
{
	sint32 sectionCount = rplLoaderContext->rplHeader.sectionTableEntryCount;
	if (sectionIndex < 0 || sectionIndex >= sectionCount)
	{
		cemuLog_log(LogType::Force, "RPLLoader: Section index out of bounds");
		rplLoaderContext->hasError = true;
		return nullptr;
	}
	rplSectionEntryNew_t* section = rplLoaderContext->sectionTablePtr + sectionIndex;
	return section;
}

RPLUncompressedSection* RPLLoader_LoadUncompressedSection(RPLModule* rplLoaderContext, sint32 sectionIndex)
{
	const rplSectionEntryNew_t* section = RPLLoader_GetSection(rplLoaderContext, sectionIndex);
	if (section == nullptr)
		return nullptr;

	RPLUncompressedSection* uSection = new RPLUncompressedSection();

	if ((uint32)section->type == 0x8)
	{
		uSection->sectionData.resize(section->sectionSize);
		std::fill(uSection->sectionData.begin(), uSection->sectionData.end(), 0);
		return uSection;
	}

	// check if raw size does not exceed bounds of rpl
	if (!RPLLoader_CheckBounds(rplLoaderContext, section->fileOffset, section->sectionSize))
	{
		// BSS
		cemuLog_log(LogType::Force, "RPLLoader: Raw data for section {} exceeds bounds of RPL file", sectionIndex);
		rplLoaderContext->hasError = true;
		delete uSection;
		return nullptr;
	}

	uint32 sectionFlags = section->flags;
	if ((sectionFlags & SHF_RPL_COMPRESSED) != 0)
	{
		// decompress
		if (!RPLLoader_CheckBounds(rplLoaderContext, section->fileOffset, sizeof(uint32be)) )
		{
			cemuLog_log(LogType::Force, "RPLLoader: Uncompressed data of section {} is too large", sectionIndex);
			rplLoaderContext->hasError = true;
			delete uSection;
			return nullptr;
		}
		uint32 uncompressedSize = *(uint32be*)(rplLoaderContext->RPLRawData.data() + (uint32)section->fileOffset);
		if (uncompressedSize >= 1*1024*1024*1024) // sections bigger than 1GB not allowed
		{
			cemuLog_log(LogType::Force, "RPLLoader: Uncompressed data of section {} is too large", sectionIndex);
			rplLoaderContext->hasError = true;
			delete uSection;
			return nullptr;
		}
		int ret;
		z_stream strm;
		strm.zalloc = Z_NULL;
		strm.zfree = Z_NULL;
		strm.opaque = Z_NULL;
		ret = inflateInit(&strm);
		if (ret == Z_OK)
		{
			strm.avail_in = (uint32)section->sectionSize - 4;
			strm.next_in = rplLoaderContext->RPLRawData.data() + (uint32)section->fileOffset + 4;
			strm.avail_out = uncompressedSize;
			uSection->sectionData.resize(uncompressedSize);
			strm.next_out = uSection->sectionData.data();
			ret = inflate(&strm, Z_FULL_FLUSH);
			inflateEnd(&strm);
			if ((ret != Z_OK && ret != Z_STREAM_END) || strm.avail_in != 0 || strm.avail_out != 0)
			{
				cemuLog_log(LogType::Force, "RPLLoader: Error while inflating data for section {}", sectionIndex);
				rplLoaderContext->hasError = true;
				delete uSection;
				return nullptr;
			}
		}
	}
	else
	{
		// no decompression
		uSection->sectionData.resize(section->sectionSize);
		const uint8* sectionDataBegin = rplLoaderContext->RPLRawData.data() + (uint32)section->fileOffset;
		std::copy(sectionDataBegin, sectionDataBegin + section->sectionSize, uSection->sectionData.data());
	}
	return uSection;
}

bool RPLLoader_LoadSingleSection(RPLModule* rplLoaderContext, sint32 sectionIndex, RPLMappingRegion* regionMappingInfo, MPTR mappedAddress)
{
	rplSectionEntryNew_t* section = RPLLoader_GetSection(rplLoaderContext, sectionIndex);
	if (section == nullptr)
		return false;

	uint32 mappingOffset = (uint32)section->virtualAddress - (uint32)regionMappingInfo->baseAddress;
	if (mappingOffset >= 0x10000000)
		cemuLog_logDebug(LogType::Force, "Suspicious section mapping offset: 0x{:08x}", mappingOffset);
	uint32 sectionAddress = mappedAddress + mappingOffset;

	cemu_assert(rplLoaderContext->debugSectionLoadMask[sectionIndex] == false);
	rplLoaderContext->debugSectionLoadMask[sectionIndex] = true;

	// extract section
	RPLUncompressedSection* uncompressedSection = RPLLoader_LoadUncompressedSection(rplLoaderContext, sectionIndex);
	if (uncompressedSection == nullptr)
	{
		rplLoaderContext->hasError = true;
		return false;
	}

	// copy to mapped address
	const uint64 sectionBegin = static_cast<uint32>(section->virtualAddress);
	const uint64 sectionEnd = sectionBegin + uncompressedSection->sectionData.size();
	const uint64 regionBegin = regionMappingInfo->baseAddress;
	const uint64 regionEnd = regionMappingInfo->endAddress;
	if (sectionBegin < regionBegin || sectionEnd < sectionBegin || sectionEnd > regionEnd)
	{
		cemuLog_log(LogType::Force,
			"RPLLoader: Section {} (0x{:08x} to 0x{:08x}) is not fully contained "
			"in its bounding region (0x{:08x} to 0x{:08x})",
			sectionIndex, sectionBegin, sectionEnd, regionBegin, regionEnd);
		if (rplLoaderContext->externalModule)
		{
			rplLoaderContext->hasError = true;
			delete uncompressedSection;
			return false;
		}
	}
	rplLoaderContext->sectionAddressTable2[sectionIndex].ptr =
		memory_getPointerFromVirtualOffset(sectionAddress);
	uint8* sectionAddressPtr = memory_getPointerFromVirtualOffset(sectionAddress);
	std::copy(uncompressedSection->sectionData.begin(), uncompressedSection->sectionData.end(), sectionAddressPtr);

	// update size in section (todo - use separate field)
	if (uncompressedSection->sectionData.size() < section->sectionSize)
		cemuLog_log(LogType::Force, "RPLLoader: Section {} uncompresses to {} bytes but sectionSize is {}", sectionIndex, uncompressedSection->sectionData.size(), (uint32)section->sectionSize);

	section->sectionSize = uncompressedSection->sectionData.size();

	delete uncompressedSection;
	return true;
}

bool RPLLoader_LoadSections(sint32 aProcId, RPLModule* rplLoaderContext)
{
	RPLRegionMappingTable regionMappingTable;
	memset(&regionMappingTable, 0, sizeof(RPLRegionMappingTable));
	regionMappingTable.region[0].baseAddress = 0xFFFFFFFF;
	regionMappingTable.region[1].baseAddress = 0xFFFFFFFF;
	regionMappingTable.region[2].baseAddress = 0xFFFFFFFF;
	regionMappingTable.region[3].baseAddress = 0xFFFFFFFF;
	for (sint32 i = 0; i < (sint32)rplLoaderContext->rplHeader.sectionTableEntryCount; i++)
	{
		rplSectionEntryNew_t* section = rplLoaderContext->sectionTablePtr + i;
		uint32 sectionType = section->type;
		uint32 sectionFlags = section->flags;
		uint32 sectionVirtualAddr = section->virtualAddress;
		uint32 sectionFileOffset = section->fileOffset;
		uint32 sectionSize = section->sectionSize;
		if(sectionSize == 0)
			continue;
		if (sectionType == SHT_RPL_CRCS)
			continue;
		if (sectionType == SHT_RPL_FILEINFO)
			continue;
		//if (sectionType == SHT_RPL_IMPORTS) -> The official loader seems to skip these, leading to incorrect boundary calculations
		//	continue;
		if ((sectionFlags & 2) == 0)
		{
			uint32 endFileOffset = sectionFileOffset + sectionSize;
			regionMappingTable.region[RPL_MAPPING_REGION_TEMP].baseAddress = std::min(regionMappingTable.region[RPL_MAPPING_REGION_TEMP].baseAddress, sectionFileOffset);
			regionMappingTable.region[RPL_MAPPING_REGION_TEMP].endAddress = std::max(regionMappingTable.region[RPL_MAPPING_REGION_TEMP].endAddress, endFileOffset);
			continue;
		}
		if ((sectionFlags & 4) != 0 && sectionType != SHT_RPL_EXPORTS && sectionType != SHT_RPL_IMPORTS)
		{
			regionMappingTable.region[RPL_MAPPING_REGION_TEXT].baseAddress = std::min(regionMappingTable.region[RPL_MAPPING_REGION_TEXT].baseAddress, sectionVirtualAddr);
			continue;
		}
		if ((sectionFlags & 1) != 0)
		{
			regionMappingTable.region[RPL_MAPPING_REGION_DATA].baseAddress = std::min(regionMappingTable.region[RPL_MAPPING_REGION_DATA].baseAddress, sectionVirtualAddr);
			continue;
		}
		else
		{ 
			regionMappingTable.region[RPL_MAPPING_REGION_LOADERINFO].baseAddress = std::min(regionMappingTable.region[RPL_MAPPING_REGION_LOADERINFO].baseAddress, sectionVirtualAddr);
			continue;

		}
	}
	for (sint32 i = 0; i < 4; i++)
	{
		if (regionMappingTable.region[i].baseAddress == 0xFFFFFFFF)
			regionMappingTable.region[i].baseAddress = 0;
	}
	regionMappingTable.region[RPL_MAPPING_REGION_TEXT].endAddress = (regionMappingTable.region[RPL_MAPPING_REGION_TEXT].baseAddress + rplLoaderContext->fileInfo.textRegionSize) - rplLoaderContext->fileInfo.trampolineAdjustment;
	regionMappingTable.region[RPL_MAPPING_REGION_DATA].endAddress = regionMappingTable.region[RPL_MAPPING_REGION_DATA].baseAddress + rplLoaderContext->fileInfo.dataRegionSize;
	regionMappingTable.region[RPL_MAPPING_REGION_LOADERINFO].endAddress = (regionMappingTable.region[RPL_MAPPING_REGION_LOADERINFO].baseAddress + rplLoaderContext->fileInfo.ukn14) - rplLoaderContext->fileInfo.ukn4C;

	// calculate region size
	uint32 regionDataSize = regionMappingTable.region[RPL_MAPPING_REGION_DATA].endAddress - regionMappingTable.region[RPL_MAPPING_REGION_DATA].baseAddress;
	uint32 regionLoaderinfoSize = regionMappingTable.region[RPL_MAPPING_REGION_LOADERINFO].endAddress - regionMappingTable.region[RPL_MAPPING_REGION_LOADERINFO].baseAddress;
	uint32 regionTextSize = regionMappingTable.region[RPL_MAPPING_REGION_TEXT].endAddress - regionMappingTable.region[RPL_MAPPING_REGION_TEXT].baseAddress;

	rplLoaderContext->regionMappingBase_data = RPLLoader_AllocateDataSpace(rplLoaderContext, regionDataSize, 0x1000);
	rplLoaderContext->regionMappingBase_loaderInfo = RPLLoader_AllocateDataSpace(rplLoaderContext, regionLoaderinfoSize, 0x1000);
	rplLoaderContext->regionMappingBase_text = rplLoaderHeap_codeArea2.alloc(regionTextSize + 0x1000, 0x1000);
	if ((regionDataSize && rplLoaderContext->regionMappingBase_data == MPTR_NULL) ||
		(regionLoaderinfoSize && rplLoaderContext->regionMappingBase_loaderInfo == MPTR_NULL) ||
		!rplLoaderContext->regionMappingBase_text)
	{
		cemuLog_log(LogType::Force, "RPLLoader: Failed to allocate mapped regions for {}",
			rplLoaderContext->moduleName);
		rplLoaderContext->hasError = true;
		return false;
	}
	rplLoader_maxCodeAddress = std::max(rplLoader_maxCodeAddress, rplLoaderContext->regionMappingBase_text.GetMPTR() + regionTextSize + 0x1000);
	PPCRecompiler_allocateRange(rplLoaderContext->regionMappingBase_text.GetMPTR(), regionTextSize + 0x1000);

	// workaround for DKC Tropical Freeze
	if (rplLoaderContext->moduleName == "rs10_production")
	{
		// allocate additional 12MB of unused data to get below a size of 0x3E200000 for the main ExpHeap
		// otherwise the game will assume it's running on a Devkit unit with 2GB of RAM and subtract 1GB from available space
		RPLLoader_AllocateDataSpace(rplLoaderContext, 12*1024*1024, 0x1000);
	}
	// set region sizes
	rplLoaderContext->regionSize_data = regionDataSize;
	rplLoaderContext->regionSize_loaderInfo = regionLoaderinfoSize;
	rplLoaderContext->regionSize_text = regionTextSize;

	// set original base addresses
	rplLoaderContext->regionOrigAddr_text = regionMappingTable.region[RPL_MAPPING_REGION_TEXT].baseAddress;
	rplLoaderContext->regionOrigAddr_data = regionMappingTable.region[RPL_MAPPING_REGION_DATA].baseAddress;

	// load data sections
	for (sint32 i = 0; i < (sint32)rplLoaderContext->rplHeader.sectionTableEntryCount; i++)
	{
		rplSectionEntryNew_t* section = rplLoaderContext->sectionTablePtr + i;
		uint32 sectionType = section->type;
		uint32 sectionFlags = section->flags;
		if (section->sectionSize == 0)
			continue;
		if( rplLoaderContext->sectionAddressTable2[i].ptr != nullptr )
			continue;
		if ((sectionFlags & 2) == 0)
			continue;
		if ((sectionFlags & 1) == 0)
			continue;

		RPLLoader_LoadSingleSection(rplLoaderContext, i, regionMappingTable.region + RPL_MAPPING_REGION_DATA, rplLoaderContext->regionMappingBase_data);
	}
	// load loaderinfo sections
	for (sint32 i = 0; i < (sint32)rplLoaderContext->rplHeader.sectionTableEntryCount; i++)
	{
		rplSectionEntryNew_t* section = rplLoaderContext->sectionTablePtr + i;
		uint32 sectionType = section->type;
		uint32 sectionFlags = section->flags;
		if (section->sectionSize == 0)
			continue;
		if (rplLoaderContext->sectionAddressTable2[i].ptr != nullptr)
			continue;
		if ((sectionFlags & 2) == 0)
			continue;
		if(sectionType != SHT_RPL_EXPORTS && sectionType != SHT_RPL_IMPORTS && (sectionFlags&5) != 0 )
			continue;
		bool readRaw = false;

		RPLLoader_LoadSingleSection(rplLoaderContext, i, regionMappingTable.region + RPL_MAPPING_REGION_LOADERINFO, rplLoaderContext->regionMappingBase_loaderInfo);

		if (sectionType == SHT_RPL_EXPORTS)
		{
			uint8* sectionAddress = (uint8*)rplLoaderContext->sectionAddressTable2[i].ptr;
			if ((sectionFlags & 4) != 0)
			{
				rplLoaderContext->exportFCount = *(uint32be*)(sectionAddress + 0);
				rplLoaderContext->exportFDataPtr = (rplExportTableEntry_t*)(sectionAddress + 8);
			}
			else
			{
				rplLoaderContext->exportDCount = *(uint32be*)(sectionAddress + 0);
				rplLoaderContext->exportDDataPtr = (rplExportTableEntry_t*)(sectionAddress + 8);
			}
		}
	}
	// load text sections
	uint32 textSectionMappedBase = rplLoaderContext->regionMappingBase_text.GetMPTR() + (uint32)rplLoaderContext->fileInfo.trampolineAdjustment; // leave some space for trampolines before the code section begins
	for (sint32 i = 0; i < (sint32)rplLoaderContext->rplHeader.sectionTableEntryCount; i++)
	{
		rplSectionEntryNew_t* section = rplLoaderContext->sectionTablePtr + i;
		uint32 sectionType = section->type;
		uint32 sectionFlags = section->flags;
		if( section->sectionSize == 0 )
			continue;
		if (rplLoaderContext->sectionAddressTable2[i].ptr != nullptr)
			continue;
		if ((sectionFlags & 2) == 0)
			continue;
		if ((sectionFlags & 4) == 0)
			continue;
		if( sectionType == SHT_RPL_EXPORTS)
			continue;

		if (section->type == 0x8)
		{
			cemuLog_log(LogType::Force, "RPLLoader: Unsupported text section type 0x8");
			cemu_assert_debug(false);
		}

		RPLLoader_LoadSingleSection(rplLoaderContext, i, regionMappingTable.region + RPL_MAPPING_REGION_TEXT, textSectionMappedBase);
	}
	// load temp region sections
	uint32 tempRegionSize = regionMappingTable.region[RPL_MAPPING_REGION_TEMP].endAddress - regionMappingTable.region[RPL_MAPPING_REGION_TEMP].baseAddress;
	uint8* tempRegionPtr;
	uint32 tempRegionAllocSize = 0;
	tempRegionPtr = (uint8*)RPLLoader_AllocWorkarea(tempRegionSize, 0x20, &tempRegionAllocSize);
	rplLoaderContext->tempRegionPtr = tempRegionPtr;
	rplLoaderContext->tempRegionAllocSize = tempRegionAllocSize;
	memcpy(tempRegionPtr, rplLoaderContext->RPLRawData.data()+regionMappingTable.region[RPL_MAPPING_REGION_TEMP].baseAddress, tempRegionSize);
	// load temp region sections
	for (sint32 i = 0; i < (sint32)rplLoaderContext->rplHeader.sectionTableEntryCount; i++)
	{
		rplSectionEntryNew_t* section = rplLoaderContext->sectionTablePtr + i;
		uint32 sectionType = section->type;
		uint32 sectionFlags = section->flags;
		if (section->sectionSize == 0)
			continue;
		if (rplLoaderContext->sectionAddressTable2[i].ptr != nullptr)
			continue;
		if (sectionType == SHT_RPL_FILEINFO || sectionType == SHT_RPL_CRCS)
			continue;
		// calculate offset within temp section
		uint32 sectionFileOffset = section->fileOffset;
		uint32 sectionSize = section->sectionSize;
		cemu_assert_debug(sectionFileOffset >= regionMappingTable.region[RPL_MAPPING_REGION_TEMP].baseAddress);
		cemu_assert_debug((sectionFileOffset + sectionSize) <= regionMappingTable.region[RPL_MAPPING_REGION_TEMP].endAddress);
		rplLoaderContext->sectionAddressTable2[i].ptr = (tempRegionPtr + (sectionFileOffset - regionMappingTable.region[RPL_MAPPING_REGION_TEMP].baseAddress));

		uint32 sectionEndAddress = sectionFileOffset + sectionSize;
		regionMappingTable.region[RPL_MAPPING_REGION_TEMP].calcEndAddress = std::max(regionMappingTable.region[RPL_MAPPING_REGION_TEMP].calcEndAddress, sectionEndAddress);
	}
	// todo: Verify calcEndAddress<=endAddress for each region

	// dump loaded sections
	/*
	for (sint32 i = 0; i < (sint32)rplLoaderContext->rplHeader.sectionTableEntryCount; i++)
	{
		rplSectionEntryNew_t* section = rplLoaderContext->sectionTablePtr + i;
		uint32 sectionType = section->type;
		uint32 sectionFlags = section->flags;
		if (section->sectionSize == 0)
			continue;
		if (rplLoaderContext->sectionAddressTable2[i].ptr == nullptr)
			continue;
		FileStream* fs = FileStream::createFile2(fmt::format("dump/rpl_sections/{}_{:08x}_type{:08x}.bin", i, (uint32)section->virtualAddress, (uint32)sectionType));
		fs->writeData(rplLoaderContext->sectionAddressTable2[i].ptr, section->sectionSize);
		delete fs;
	}
	*/
	return true;
}

struct RPLFileSymtabEntry
{
	/* +0x0 */ uint32be ukn00;
	/* +0x4 */ uint32be symbolAddress;
	/* +0x8 */ uint32be ukn08;
	/* +0xC */ uint8    info;
	/* +0xD */ uint8    ukn0D;
	/* +0xE */ uint16be sectionIndex;
};

struct RPLSharedImportTracking
{
	RPLModule* rplLoaderContext; // rpl loader context of module with exports
	rplSectionEntryNew_t* exportSection; // export section
	char modulename[RPL_MODULE_NAME_LENGTH];
};

static_assert(sizeof(RPLFileSymtabEntry) == 0x10, "rplSymtabEntry_t has invalid size");

typedef struct
{
	uint64 hash1;
	uint64 hash2;
	uint32 address;
}mappedFunctionImport_t;

std::vector<mappedFunctionImport_t> list_mappedFunctionImports = std::vector<mappedFunctionImport_t>();

void _calculateMappedImportNameHash(const char* rplName, const char* funcName, uint64* h1Out, uint64* h2Out)
{
	uint64 h1 = 0;
	uint64 h2 = 0;
	// rplName
	while (*rplName)
	{
		uint64 v = (uint64)*rplName;
		h1 += v;
		h2 ^= v;
		h1 = std::rotl<uint64>(h1, 3);
		h2 = std::rotl<uint64>(h2, 7);
		rplName++;
	}
	// funcName
	while (*funcName)
	{
		uint64 v = (uint64)*funcName;
		h1 += v;
		h2 ^= v;
		h1 = std::rotl<uint64>(h1, 3);
		h2 = std::rotl<uint64>(h2, 7);
		funcName++;
	}
	*h1Out = h1;
	*h2Out = h2;
}

uint32 RPLLoader_MakePPCCallable(void(*ppcCallableExport)(PPCInterpreter_t* hCPU))
{
	auto it = g_map_callableExports.find(ppcCallableExport);
	if (it != g_map_callableExports.end())
		return it->second;
	// get HLE function index
	sint32 functionIndex = PPCInterpreter_registerHLECall(ppcCallableExport, fmt::format("PPCCallback{:x}", (uintptr_t)ppcCallableExport));
	MPTR codeAddr = memory_getVirtualOffsetFromPointer(RPLLoader_AllocateTrampolineCodeSpace(4));
	uint32 opcode = (1 << 26) | functionIndex;
	memory_write<uint32>(codeAddr, opcode);
	g_map_callableExports[ppcCallableExport] = codeAddr;
	return codeAddr;
}

uint32 rpl_mapHLEImport(RPLModule* rplLoaderContext, const char* rplName, const char* funcName, bool functionMustExist)
{
	// External WUPS resolution intentionally has no title-RPL context.  A null
	// context is valid here (and is only relevant to the legacy unsupported
	// import path), but names always cross an untrusted RPL boundary.
	if (!rplName || !funcName || rplName[0] == '\0' || funcName[0] == '\0')
		return MPTR_NULL;
	if (strnlen(rplName, 512) == 512 || strnlen(funcName, 1025) == 1025)
		return MPTR_NULL;
	// calculate import name hash
	uint64 mappedImportHash1;
	uint64 mappedImportHash2;
	_calculateMappedImportNameHash(rplName, funcName, &mappedImportHash1, &mappedImportHash2);
	// find already mapped name
	for (auto& importItr : list_mappedFunctionImports)
	{
		if (importItr.hash1 == mappedImportHash1 && importItr.hash2 == mappedImportHash2)
		{
			return importItr.address;
		}
	}
	// copy lib file name and cut off .rpl from libName if present
	char libName[512];
	strcpy_s(libName, rplName);
	for (sint32 i = 0; i < sizeof(libName); i++)
	{
		if (libName[i] == '\0')
			break;
		if (libName[i] == '.')
		{
			libName[i] = '\0';
			break;
		}
	}
	// find import in list of known exports
	sint32 functionIndex = osLib_getFunctionIndex(libName, funcName);
	if (functionIndex >= 0)
	{
		MPTR codeAddr = memory_getVirtualOffsetFromPointer(RPLLoader_AllocateTrampolineCodeSpace(4));
		uint32 opcode = (1 << 26) | functionIndex;
		memory_write<uint32>(codeAddr, opcode);
		// register mapped import
		mappedFunctionImport_t newImport;
		newImport.hash1 = mappedImportHash1;
		newImport.hash2 = mappedImportHash2;
		newImport.address = codeAddr;
		list_mappedFunctionImports.push_back(newImport);
		// remember in symbol storage for debugger
		rplSymbolStorage_store(libName, funcName, codeAddr);
		return codeAddr;
	}
	else
	{
		if (functionMustExist == false)
			return MPTR_NULL;
	}
	// create code for unsupported import
	uint32 codeStart = memory_getVirtualOffsetFromPointer(RPLLoader_AllocateTrampolineCodeSpace(256));
	uint32 currentAddress = codeStart;
	uint32 opcode = (1 << 26) | (0xFFD0); // opcode for HLE: Unsupported import
	memory_write<uint32>(codeStart + 0, opcode);
	memory_write<uint32>(codeStart + 4, 0x4E800020);
	currentAddress += 8;
	// write name of lib/function
	sint32 libNameLength = std::min(128, (sint32)strlen(libName));
	sint32 funcNameLength = std::min(128, (sint32)strlen(funcName));
	memcpy(memory_getPointerFromVirtualOffset(currentAddress), libName, libNameLength);
	currentAddress += libNameLength;
	memory_writeU8(currentAddress, '.');
	currentAddress++;
	memcpy(memory_getPointerFromVirtualOffset(currentAddress), funcName, funcNameLength);
	currentAddress += funcNameLength;
	memory_writeU8(currentAddress, '\0');
	currentAddress++;
	// align address to 4 byte boundary
	currentAddress = (currentAddress + 3)&~3;
	// register mapped import
	mappedFunctionImport_t newImport;
	newImport.hash1 = mappedImportHash1;
	newImport.hash2 = mappedImportHash2;
	newImport.address = codeStart;
	list_mappedFunctionImports.push_back(newImport);
	// remember in symbol storage for debugger
	rplSymbolStorage_store(libName, funcName, codeStart);
	// return address of code start
	return codeStart;
}

MPTR RPLLoader_FindRPLExport(RPLModule* rplLoaderContext, const char* symbolName, bool isData)
{
	if (!rplLoaderContext || !symbolName)
		return MPTR_NULL;
	const auto* entries = isData ? rplLoaderContext->exportDDataPtr :
		rplLoaderContext->exportFDataPtr;
	const uint32 count = isData ? rplLoaderContext->exportDCount :
		rplLoaderContext->exportFCount;
	if (entries)
	{
		const char* exportNameData = reinterpret_cast<const char*>(
			reinterpret_cast<const uint8*>(entries) - 8);
		for (uint32 index = 0; index < count; ++index)
		{
			const char* name = exportNameData + (uint32)entries[index].nameOffset;
			if (strcmp(name, symbolName) == 0)
				return (uint32)entries[index].virtualOffset;
		}
	}
	return MPTR_NULL;
}

MPTR _findHLEExport(RPLModule* rplLoaderContext, RPLSharedImportTracking* sharedImportTrackingEntry, char* libname, char* symbolName, bool isData)
{
	// WUPS plugin RPLs must not receive the ordinary coreinit default-heap
	// data-export slots for these three symbols: those slots are the *game*'s
	// own gCoreinitData function pointers (registered via
	// osLib_addVirtualPointer in coreinit.cpp), and letting a plugin's libc
	// allocate through them shares the game's heap with the plugin, which can
	// deadlock a title's own reentrant heap locking against unrelated Cemu
	// HLE plumbing (see WupsPluginHeap.h for the full story). Give the
	// external import resolver first refusal for these specific data exports,
	// *before* the normal osLib_getPointer virtual-pointer lookup below ever
	// sees them, so it can redirect them into an isolated plugin heap instead.
	// If the resolver declines (no isolated heap available on this platform),
	// resolution falls through to the normal path below exactly as before.
	if (isData && rplLoaderContext->externalModule &&
		rplLoaderContext->externalImportResolver &&
		strcmp(libname, "coreinit") == 0 &&
		(strcmp(symbolName, "MEMAllocFromDefaultHeap") == 0 ||
			strcmp(symbolName, "MEMAllocFromDefaultHeapEx") == 0 ||
			strcmp(symbolName, "MEMFreeToDefaultHeap") == 0))
	{
		std::string resolverError;
		const auto resolved = rplLoaderContext->externalImportResolver(
			libname, symbolName, isData, resolverError);
		if (resolved && *resolved != MPTR_NULL)
			return *resolved;
		cemuLog_log(LogType::Force,
			"WUPS: heap-import intercept for coreinit.{} did NOT redirect ({})",
			symbolName, resolverError.empty() ? "resolver returned null" : resolverError);
		if (!resolverError.empty())
			rplLoaderContext->externalLinkError = std::move(resolverError);
	}
	if (isData)
	{
		// data export
		MPTR weakExportAddr = osLib_getPointer(libname, symbolName);
		if (weakExportAddr != 0xFFFFFFFF)
			return weakExportAddr;
	}
	else
	{
		// Resolve a real HLE export before consulting external registries. The
		// legacy unsupported-import trampoline is only created below for title RPLs.
		MPTR mappedFunctionAddr = rpl_mapHLEImport(rplLoaderContext, libname, symbolName, false);
		if (mappedFunctionAddr != MPTR_NULL)
			return mappedFunctionAddr;
	}
	if (rplLoaderContext->externalModule && rplLoaderContext->externalImportResolver)
	{
		std::string resolverError;
		const auto resolved = rplLoaderContext->externalImportResolver(
			libname, symbolName, isData, resolverError);
		if (resolved && *resolved != MPTR_NULL)
			return *resolved;
		if (!resolverError.empty())
			rplLoaderContext->externalLinkError = std::move(resolverError);
	}
	if (rplLoaderContext->externalModule)
	{
		if (rplLoaderContext->externalLinkError.empty())
			rplLoaderContext->externalLinkError = fmt::format(
				"unresolved mandatory {} import {}.{}",
				isData ? "data" : "function", libname, symbolName);
		return MPTR_NULL;
	}
	if (isData)
	{
		cemuLog_logDebug(LogType::Force, "Unsupported data export ({}): {}.{}",
			rplLoaderContext->moduleName, libname, symbolName);
		return MPTR_NULL;
	}
	return rpl_mapHLEImport(rplLoaderContext, libname, symbolName, true);
}

uint32 RPLLoader_FindModuleExport(RPLModule* rplLoaderContext, bool isData, const char* exportName)
{
	if (isData == false)
	{
		// find function export
		char* exportNameData = (char*)((uint8*)rplLoaderContext->exportFDataPtr - 8);
		for (uint32 f = 0; f < rplLoaderContext->exportFCount; f++)
		{
			char* name = exportNameData + (uint32)rplLoaderContext->exportFDataPtr[f].nameOffset;
			if (strcmp(name, exportName) == 0)
			{
				uint32 exportAddress = rplLoaderContext->exportFDataPtr[f].virtualOffset;
				return exportAddress;
			}
		}
	}
	else
	{
		// find data export
		char* exportNameData = (char*)((uint8*)rplLoaderContext->exportDDataPtr - 8);
		for (uint32 f = 0; f < rplLoaderContext->exportDCount; f++)
		{
			char* name = exportNameData + (uint32)rplLoaderContext->exportDDataPtr[f].nameOffset;
			if (strcmp(name, exportName) == 0)
			{
				uint32 exportAddress = rplLoaderContext->exportDDataPtr[f].virtualOffset;
				return exportAddress;
			}
		}
	}
	return 0;
}

bool RPLLoader_FixImportSymbols(RPLModule* rplLoaderContext, sint32 symtabSectionIndex, rplSectionEntryNew_t* symTabSection, std::span<RPLSharedImportTracking> sharedImportTracking, uint32 linkMode)
{
	uint32 sectionSize = symTabSection->sectionSize;
	uint32 symbolEntrySize = symTabSection->ukn24;
	if (symbolEntrySize == 0)
		symbolEntrySize = 0x10;
	cemu_assert(symbolEntrySize == 0x10);
	cemu_assert((sectionSize % symbolEntrySize) == 0);
	uint32 symbolCount = sectionSize / symbolEntrySize;
	cemu_assert(symbolCount >= 2);

	uint16 sectionCount = rplLoaderContext->rplHeader.sectionTableEntryCount;
	uint8* symtabData = (uint8*)rplLoaderContext->sectionAddressTable2[symtabSectionIndex].ptr;

	uint32 strtabSectionIndex = symTabSection->symtabSectionIndex;
	uint8* strtabData = (uint8*)rplLoaderContext->sectionAddressTable2[strtabSectionIndex].ptr;
	uint32 strtabSize = rplLoaderContext->sectionTablePtr[strtabSectionIndex].sectionSize;

	for (uint32 i = 0; i < symbolCount; i++)
	{
		RPLFileSymtabEntry* sym = (RPLFileSymtabEntry*)(symtabData + i*symbolEntrySize);
		uint16 symSectionIndex = sym->sectionIndex;
		if (symSectionIndex == 0 || symSectionIndex >= sectionCount)
			continue;
		void* symbolSectionAddress = rplLoaderContext->sectionAddressTable2[symSectionIndex].ptr;
		if (symbolSectionAddress == nullptr)
		{
			sym->symbolAddress = 0xCD000000 | i;
			continue;
		}
		rplSectionEntryNew_t* symbolSection = rplLoaderContext->sectionTablePtr + symSectionIndex;
		uint32 symbolOffset = sym->symbolAddress - symbolSection->virtualAddress;
		
		if (symSectionIndex >= sharedImportTracking.size())
		{
			cemuLog_log(LogType::Force, "RPL-Loader: Symbol {} references invalid section", i);
		}
		else if (sharedImportTracking[symSectionIndex].rplLoaderContext != nullptr)
		{
			if (linkMode == 0)
			{
				continue; // ?
			}
			if (symbolOffset < 8)
			{
				cemu_assert(symbolSectionAddress >= memory_base && symbolSectionAddress <= (memory_base + 0x100000000ULL));
				uint32 symbolSectionMPTR = memory_getVirtualOffsetFromPointer(symbolSectionAddress);
				uint32 symbolRelativeAddress = (uint32)sym->symbolAddress - (uint32)symbolSection->virtualAddress;

				sym->symbolAddress = (symbolSectionMPTR + symbolRelativeAddress);
				continue; // ?
			}

			if (sharedImportTracking[symSectionIndex].rplLoaderContext == HLE_MODULE_PTR)
			{
				// get address
				uint32 nameOffset = sym->ukn00;
				char* symbolName = (char*)strtabData + nameOffset;
				if (nameOffset >= strtabSize)
				{
					cemuLog_log(LogType::Force, "RPLLoader: Symbol {} in section {} has out-of-bounds name offset", i, symSectionIndex);
					continue;
				}

				uint32 exportAddress;
				if (nameOffset == 0)
				{
					cemu_assert_debug(symbolName[0] == '\0');
					exportAddress = 0;
				}
				else
				{
					bool isDataExport = (rplLoaderContext->sectionTablePtr[symSectionIndex].flags & 0x4) == 0;
					exportAddress = _findHLEExport(rplLoaderContext, sharedImportTracking.data() + symSectionIndex, sharedImportTracking[symSectionIndex].modulename, symbolName, isDataExport);
				}

				sym->symbolAddress = exportAddress;
			}
			else
			{
				RPLModule* ctxExportModule = sharedImportTracking[symSectionIndex].rplLoaderContext;
				uint32 nameOffset = sym->ukn00;
				char* symbolName = (char*)strtabData + nameOffset;

				bool foundExport = false;
				if ((rplLoaderContext->sectionTablePtr[symSectionIndex].flags & 0x4) != 0 &&
					ctxExportModule->exportFDataPtr)
				{
					// find function export
					char* exportNameData = (char*)((uint8*)ctxExportModule->exportFDataPtr - 8);
					for (uint32 f = 0; f < ctxExportModule->exportFCount; f++)
					{
						char* name = exportNameData + (uint32)ctxExportModule->exportFDataPtr[f].nameOffset;
						if (strcmp(name, symbolName) == 0)
						{
							uint32 exportAddress = ctxExportModule->exportFDataPtr[f].virtualOffset;
							sym->symbolAddress = exportAddress;
							foundExport = true;
							break;
						}
					}
				}
				else if (ctxExportModule->exportDDataPtr)
				{
					// find data export
					char* exportNameData = (char*)((uint8*)ctxExportModule->exportDDataPtr - 8);
					for (uint32 f = 0; f < ctxExportModule->exportDCount; f++)
					{
						char* name = exportNameData + (uint32)ctxExportModule->exportDDataPtr[f].nameOffset;
						if (strcmp(name, symbolName) == 0)
						{
							uint32 exportAddress = ctxExportModule->exportDDataPtr[f].virtualOffset;
							sym->symbolAddress = exportAddress;
							foundExport = true;
							break;
						}
					}
				}
				if (foundExport == false)
				{
					if (rplLoaderContext->externalModule &&
						rplLoaderContext->externalLinkError.empty() && nameOffset > 0)
					{
						rplLoaderContext->externalLinkError = fmt::format(
							"unresolved mandatory {} import {}.{}",
							(rplLoaderContext->sectionTablePtr[symSectionIndex].flags & 0x4) != 0 ?
								"function" : "data",
							sharedImportTracking[symSectionIndex].modulename, symbolName);
					}
#ifdef CEMU_DEBUG_ASSERT
					if (nameOffset > 0)
					{
						cemuLog_logDebug(LogType::Force, "export not found - force lookup in function exports");
						// workaround - force look up export in function exports
						if (!ctxExportModule->exportFDataPtr)
							continue;
						char* exportNameData = (char*)((uint8*)ctxExportModule->exportFDataPtr - 8);
						for (uint32 f = 0; f < ctxExportModule->exportFCount; f++)
						{
							char* name = exportNameData + (uint32)ctxExportModule->exportFDataPtr[f].nameOffset;
							if (strcmp(name, symbolName) == 0)
							{
								uint32 exportAddress = ctxExportModule->exportFDataPtr[f].virtualOffset;
								sym->symbolAddress = exportAddress;
								foundExport = true;
								break;
							}
						}
					}
#endif
					continue;
				}
			}
		}
		else
		{
			uint32 symbolType = sym->info & 0xF;
			if (symbolType == 6)
				continue;
			if (((uint32)symbolSection->type != SHT_RPL_IMPORTS && linkMode != 2) ||
				((uint32)symbolSection->type == SHT_RPL_IMPORTS && linkMode != 1 && linkMode != 2)
				)
			{
				// update virtual address to match actual mapped address
				cemu_assert(symbolSectionAddress >= memory_base && symbolSectionAddress <= (memory_base + 0x100000000ULL));
				uint32 symbolSectionMPTR = memory_getVirtualOffsetFromPointer(symbolSectionAddress);
				uint32 symbolRelativeAddress = (uint32)sym->symbolAddress - (uint32)symbolSection->virtualAddress;
				sym->symbolAddress = (symbolSectionMPTR + symbolRelativeAddress);
			}
		}
	}
	return true;
}

bool RPLLoader_ApplySingleReloc(RPLModule* rplLoaderContext, uint32 uknR3, uint8* relocTargetSectionAddress, uint32 relocType, bool isSymbolBinding2, uint32 relocOffset, uint32 relocAddend, uint32 symbolAddress, sint16 tlsModuleIndex)
{
	MPTR relocTargetSectionMPTR = memory_getVirtualOffsetFromPointer(relocTargetSectionAddress);
	MPTR relocAddrMPTR = relocTargetSectionMPTR + relocOffset;
	uint8* relocAddr = memory_getPointerFromVirtualOffset(relocAddrMPTR);

	if (relocType == RPL_RELOC_HA16)
	{
		uint32 relocDestAddr = symbolAddress + relocAddend;
		uint32 p = (relocDestAddr >> 16);
		p += (relocDestAddr >> 15) & 1;
		*(uint16be*)(relocAddr) = (uint16)p;
	}
	else if (relocType == RPL_RELOC_LO16)
	{
		uint32 relocDestAddr = symbolAddress + relocAddend;
		uint32 p = relocDestAddr;
		*(uint16be*)(relocAddr) = (uint16)p;
	}
	else if (relocType == RPL_RELOC_HI16)
	{
		uint32 relocDestAddr = symbolAddress + relocAddend;
		uint32 p = relocDestAddr>>16;
		*(uint16be*)(relocAddr) = (uint16)p;
	}
	else if (relocType == RPL_RELOC_REL24)
	{
		// todo - effect of isSymbolBinding2?
		uint32 opc = *(uint32be*)relocAddr;
		uint32 relocDestAddr = symbolAddress + relocAddend;
		uint32 jumpDistance = relocDestAddr - memory_getVirtualOffsetFromPointer(relocAddr);
		if ((jumpDistance>>25) != 0 && (jumpDistance >> 25) != 0x7F)
		{
			// can't reach with 24bit jump, use trampoline + absolute branch
			MPTR trampolineAddr = _generateTrampolineFarJump(rplLoaderContext, relocDestAddr);
			// make absolute branch
			cemu_assert_debug((opc >> 26) == 18); // should be B/BL instruction
			opc &= ~0x03fffffc;
			opc |= (trampolineAddr & 0x3FFFFFC);
			opc |= (1 << 1); // absolute jump
			*(uint32be*)relocAddr = opc;
		}
		else
		{
			// within range, update jump opcode
			if ((jumpDistance & 3) != 0)
				cemuLog_log(LogType::Force, "RPL-Loader: Encountered unaligned RPL_RELOC_REL24");
			opc &= ~0x03fffffc;
			opc |= (jumpDistance &0x03fffffc);
			*(uint32be*)relocAddr = opc;
		}
	}
	else if (relocType == RPL_RELOC_REL14)
	{
		// seen in Your Shape: Fitness Evolved
		// todo - effect of isSymbolBinding2?
		uint32 opc = *(uint32be*)relocAddr;
		uint32 relocDestAddr = symbolAddress + relocAddend;
		uint32 jumpDistance = relocDestAddr - memory_getVirtualOffsetFromPointer(relocAddr);
		if ((jumpDistance & ~0x7fff) != 0xFFFF8000 && (jumpDistance & ~0x7fff) != 0x00000000)
		{
			cemu_assert_debug(false);
		}
		else
		{
			// within range, update jump opcode
			if ((jumpDistance & 3) != 0)
				cemuLog_log(LogType::Force, "RPL-Loader: Encountered unaligned RPL_RELOC_REL14");
			opc &= ~0xfffc;
			opc |= (jumpDistance & 0xfffc);
			*(uint32be*)relocAddr = opc;
		}
	}
	else if (relocType == RPL_RELOC_ADDR32)
	{
		uint32 relocDestAddr = symbolAddress + relocAddend;
		uint32 p = relocDestAddr;
		*(uint32be*)(relocAddr) = (uint32)p;
	}
	else if (relocType == R_PPC_DTPMOD32)
	{
		// patch tls_index.moduleIndex
		*(uint32be*)(relocAddr) = (uint32)(sint32)tlsModuleIndex;
	}
	else if (relocType == R_PPC_DTPREL32)
	{
		// patch tls_index.size
		*(uint32be*)(relocAddr) = (uint32)(sint32)(symbolAddress + relocAddend);
	}
	else if (relocType == R_PPC_REL16_HA)
	{
		// used by WUT
		uint32 relAddr = (symbolAddress + relocAddend) - relocAddrMPTR;
		uint32 p = (relAddr >> 16);
		p += (relAddr >> 15) & 1;
		*(uint16be*)(relocAddr) = (uint16)p;
	}
	else if (relocType == R_PPC_REL16_HI)
	{
		// used by WUT
		uint32 relAddr = (symbolAddress + relocAddend) - relocAddrMPTR;
		uint32 p = (relAddr >> 16);
		*(uint16be*)(relocAddr) = (uint16)p;
	}
	else if (relocType == R_PPC_REL16_LO)
	{
		// used by WUT
		uint32 relAddr = (symbolAddress + relocAddend) - relocAddrMPTR;
		uint32 p = (relAddr & 0xFFFF);
		*(uint16be*)(relocAddr) = (uint16)p;
	}
	else if (relocType == 0x6D) // SDATA reloc
	{
		uint32 opc = *(uint32be*)relocAddr;

		uint32 registerIndex = (opc >> 16) & 0x1F;
		uint32 destination = (symbolAddress + relocAddend);;

		if (registerIndex == 2)
		{
			const uint32 sda2 = rplLoaderContext->mappedSda2Base ?
				rplLoaderContext->mappedSda2Base : rplLoader_sdata2Addr;
			uint32 offset = destination - sda2;
			uint32 newOpc = (opc & 0xffe00000) | (offset & 0xffff) | (registerIndex << 16);
			*(uint32be*)relocAddr = newOpc;
		}
		else if (registerIndex == 13)
		{
			const uint32 sda1 = rplLoaderContext->mappedSda1Base ?
				rplLoaderContext->mappedSda1Base : rplLoader_sdataAddr;
			uint32 offset = destination - sda1;
			uint32 newOpc = (opc & 0xffe00000) | (offset & 0xffff) | (registerIndex << 16);
			*(uint32be*)relocAddr = newOpc;
		}
		else
		{
			cemuLog_log(LogType::Force, "RPLLoader: sdata reloc uses register other than r2/r13");
			cemu_assert(false);
		}
	}
	else if (relocType == 0xFB)
	{
		// relative offset - high
		uint32 relocDestAddr = symbolAddress + relocAddend;
		uint32 relativeOffset = relocDestAddr - relocAddrMPTR;
		uint16 prevValue = *(uint16be*)relocAddr;
		uint32 newImm = ((relativeOffset >> 16) + ((relativeOffset >> 15) & 0x1));
		newImm &= 0xFFFF;
		*(uint16be*)relocAddr = newImm;
		if (symbolAddress != 0)
		{
			cemu_assert_debug((uint32)prevValue == newImm);
		}
	}
	else if (relocType == 0xFD)
	{
		// relative offset - low
		uint32 relocDestAddr = symbolAddress + relocAddend;
		uint32 relativeOffset = relocDestAddr - relocAddrMPTR;
		uint16 prevValue = *(uint16be*)relocAddr;
		uint32 newImm = relativeOffset;
		newImm &= 0xFFFF;
		*(uint16be*)relocAddr = newImm;
		if (symbolAddress != 0)
		{
			cemu_assert_debug((uint32)prevValue == newImm);
		}
	}
	else
	{
		cemuLog_log(LogType::Force, "RPLLoader: Unsupported reloc type 0x{:02x}", relocType);
		if (rplLoaderContext->externalModule)
		{
			if (rplLoaderContext->externalLinkError.empty())
				rplLoaderContext->externalLinkError = fmt::format(
					"external RPL uses unsupported relocation type 0x{:02x}", relocType);
			return false;
		}
		cemu_assert_debug(false); // unknown relocation in a legacy title RPL
	}
	return true;
}

bool RPLLoader_ApplyRelocs(RPLModule* rplLoaderContext, sint32 relaSectionIndex, rplSectionEntryNew_t* section, uint32 linkMode)
{
	uint32 relocTargetSectionIndex = section->relocTargetSectionIndex;
	if (relocTargetSectionIndex >= (uint32)rplLoaderContext->rplHeader.sectionTableEntryCount)
		assert_dbg();
	uint32 symtabSectionIndex = section->symtabSectionIndex;
	uint8* relocTargetSectionAddress = (uint8*)(rplLoaderContext->sectionAddressTable2[relocTargetSectionIndex].ptr);
	cemu_assert(relocTargetSectionAddress);
	// get symtab info
	rplSectionEntryNew_t* symtabSection = rplLoaderContext->sectionTablePtr + symtabSectionIndex;
	uint32 symtabSectionSize = symtabSection->sectionSize;
	uint32 symbolEntrySize = symtabSection->ukn24;
	if (symbolEntrySize == 0)
		symbolEntrySize = 0x10;
	cemu_assert(symbolEntrySize == 0x10);
	cemu_assert((symtabSectionSize % symbolEntrySize) == 0);
	uint32 symbolCount = symtabSectionSize / symbolEntrySize;
	cemu_assert(symbolCount >= 2);
	uint8* symtabData = (uint8*)rplLoaderContext->sectionAddressTable2[symtabSectionIndex].ptr;
	const uint32 diagStrtabIndex = symtabSection->symtabSectionIndex;
	uint8* diagStrtabData = diagStrtabIndex < (uint32)rplLoaderContext->rplHeader.sectionTableEntryCount ?
		(uint8*)rplLoaderContext->sectionAddressTable2[diagStrtabIndex].ptr : nullptr;
	const uint32 diagStrtabSize = diagStrtabData ?
		(uint32)rplLoaderContext->sectionTablePtr[diagStrtabIndex].sectionSize : 0;
	// decompress reloc section if needed
	uint8* relocData;
	uint32 relocSize;
	if ((uint32)(section->flags) & SHF_RPL_COMPRESSED)
	{
		uint8* relocRawData = (uint8*)rplLoaderContext->sectionAddressTable2[relaSectionIndex].ptr;
		uint32 relocUncompressedSize = *(uint32be*)relocRawData;
		relocData = (uint8*)malloc(relocUncompressedSize);
		relocSize = relocUncompressedSize;
		// decompress
		int ret;
		z_stream strm;
		strm.zalloc = Z_NULL;
		strm.zfree = Z_NULL;
		strm.opaque = Z_NULL;
		ret = inflateInit(&strm);
		if (ret == Z_OK)
		{
			strm.avail_in = (uint32)section->sectionSize - 4;
			strm.next_in = relocRawData + 4;
			strm.avail_out = relocUncompressedSize;
			strm.next_out = relocData;
			ret = inflate(&strm, Z_FULL_FLUSH);
			cemu_assert_debug(ret == Z_OK || ret == Z_STREAM_END);
			cemu_assert_debug(strm.avail_in == 0 && strm.avail_out == 0);
			inflateEnd(&strm);
		}
	}
	else
	{
		relocData = (uint8*)rplLoaderContext->sectionAddressTable2[relaSectionIndex].ptr;
		relocSize = section->sectionSize;
	}
	// check CRC
	uint32 calcCRC = crc32_calc(0, relocData, relocSize);
	uint32 crc = rplLoaderContext->GetSectionCRC(relaSectionIndex);
	if (calcCRC != crc)
	{
		cemuLog_log(LogType::Force, "RPLLoader {} - Relocation section {} has CRC mismatch - Calc: {:08x} Actual: {:08x}", rplLoaderContext->moduleName.c_str(), relaSectionIndex, calcCRC, crc);
	}
	// process relocations
	sint32 relocCount = relocSize / sizeof(rplRelocNew_t);
	rplRelocNew_t* reloc = (rplRelocNew_t*)relocData;
	for (sint32 i = 0; i < relocCount; i++)
	{
		uint32 relocType = (uint32)reloc->symbolIndexAndType & 0xFF;
		uint32 relocSymbolIndex = (uint32)reloc->symbolIndexAndType >> 8;
		if (relocType == 0)
		{
			// next
			reloc++;
			continue;
		}
		if (relocSymbolIndex >= symbolCount)
		{
			cemuLog_logDebug(LogType::Force, "reloc with symbol index out of range 0x{:04x}", (uint32)relocSymbolIndex);
			reloc++;
			continue;
		}
		// get symbol
		RPLFileSymtabEntry* sym = (RPLFileSymtabEntry*)(symtabData + symbolEntrySize*relocSymbolIndex);

		if ((uint32)sym->sectionIndex >= (uint32)rplLoaderContext->rplHeader.sectionTableEntryCount)
		{
			cemuLog_logDebug(LogType::Force, "reloc with sectionIndex out of range 0x{:04x}", (uint32)sym->sectionIndex);
			reloc++;
			continue;
		}
		// exclude symbols that arent ready yet
		if (linkMode == 0)
		{
			if ((uint32)rplLoaderContext->sectionTablePtr[(uint32)sym->sectionIndex].type == SHT_RPL_IMPORTS)
			{
				reloc++;
				continue;
			}
		}
		uint32 symbolAddress = sym->symbolAddress;
		uint8 symbolType = (sym->info >> 0) & 0xF;
		uint8 symbolBinding = (sym->info >> 4) & 0xF;
		if ((symbolAddress&0xFF000000) == 0xCD000000)
		{
			cemu_assert_unimplemented();
			// next
			reloc++;
			continue;
		}
		sint16 tlsModuleIndex = -1;
		if (relocType == R_PPC_DTPMOD32 || relocType == R_PPC_DTPREL32)
		{
			// TLS-relocation
			if (symbolType != 6)
				assert_dbg(); // not a TLS symbol
			if (rplLoaderContext->fileInfo.tlsModuleIndex == -1)
			{
				cemuLog_log(LogType::Force, "RPLLoader: TLS relocs applied to non-TLS module");
				cemu_assert_debug(false); // module not a TLS-module
			}
			tlsModuleIndex = rplLoaderContext->fileInfo.tlsModuleIndex;
		}
		uint32 relocOffset = (uint32)reloc->relocOffset - (uint32)rplLoaderContext->sectionTablePtr[relocTargetSectionIndex].virtualAddress;
		if (rplLoaderContext->externalModule && diagStrtabData)
		{
			const uint32 diagNameOffset = sym->ukn00;
			if (diagNameOffset != 0 && diagNameOffset < diagStrtabSize)
			{
				const char* diagName = (const char*)diagStrtabData + diagNameOffset;
				if (symbolAddress == 0 || strncmp(diagName, "__wrap_", 7) == 0)
					cemuLog_log(LogType::Force,
						"RPLreloc: sym '{}' secIdx {} symAddr 0x{:08x} type {} site 0x{:08x}",
						diagName, (uint32)sym->sectionIndex, symbolAddress, relocType,
						memory_getVirtualOffsetFromPointer(relocTargetSectionAddress) + relocOffset);
			}
		}
		RPLLoader_ApplySingleReloc(rplLoaderContext, 0, relocTargetSectionAddress, relocType, symbolBinding == 2, relocOffset, reloc->relocAddend, symbolAddress, tlsModuleIndex);

		// next reloc
		reloc++;
	}

	if ((uint32)(section->flags) & SHF_RPL_COMPRESSED)
		free(relocData);
	return true;
}

bool RPLLoader_HandleRelocs(RPLModule* rplLoaderContext, std::span<RPLSharedImportTracking> sharedImportTracking, uint32 linkMode)
{
	// resolve relocs
	for (sint32 i = 0; i < (sint32)rplLoaderContext->rplHeader.sectionTableEntryCount; i++)
	{
		rplSectionEntryNew_t* section = rplLoaderContext->sectionTablePtr + i;
		uint32 sectionType = section->type;
		if( sectionType != SHT_SYMTAB )
			continue;
		RPLLoader_FixImportSymbols(rplLoaderContext, i, section, sharedImportTracking, linkMode);
	}

	// apply relocs again after we have fixed the import section
	for (sint32 i = 0; i < (sint32)rplLoaderContext->rplHeader.sectionTableEntryCount; i++)
	{
		rplSectionEntryNew_t* section = rplLoaderContext->sectionTablePtr + i;
		uint32 sectionType = section->type;
		if (sectionType != SHT_RELA)
			continue;
		RPLLoader_ApplyRelocs(rplLoaderContext, i, section, linkMode);
	}
	return true;
}

std::string _RPLLoader_ExtractModuleNameFromPath(std::string_view input)
{
	// scan to last '/'
	cemu_assert(!input.empty());
	size_t startIndex = input.size() - 1;
	while (startIndex > 0)
	{
		if (input[startIndex] == '/')
		{
			startIndex++;
			break;
		}
		startIndex--;
	}
	// cut off after '.'
	size_t endIndex = startIndex;
	while (endIndex < input.size())
	{
		if (input[endIndex] == '.')
			break;
		endIndex++;
	}
	size_t nameLen = endIndex - startIndex;
	cemu_assert(nameLen != 0);
	nameLen = std::min<size_t>(nameLen, RPL_MODULE_NAME_LENGTH-1);
	std::string output;
	output.append(input.data() + startIndex, nameLen);
	// convert to lower case
	std::for_each(output.begin(), output.end(), [](char& c) {c = _ansiToLower(c);});
	return output;
}

void RPLLoader_InitState()
{
	std::lock_guard lock(g_rplLoaderMutex);
	cemu_assert_debug(!rplLoaderHeap_lowerAreaCodeMem2.hasAllocations());
	cemu_assert_debug(!rplLoaderHeap_codeArea2.hasAllocations());
	cemu_assert_debug(!rplLoaderHeap_workarea.hasAllocations());
	rplLoaderHeap_lowerAreaCodeMem2.setHeapBase(memory_getPointerFromVirtualOffset(MEMORY_CODE_TRAMPOLINE_AREA_ADDR));
	rplLoaderHeap_codeArea2.setHeapBase(memory_getPointerFromVirtualOffset(MEMORY_CODEAREA_ADDR));
	rplLoaderHeap_workarea.setHeapBase(memory_getPointerFromVirtualOffset(MEMORY_RPLLOADER_AREA_ADDR));
	g_heapTrampolineArea.setBaseAllocator(&rplLoaderHeap_lowerAreaCodeMem2);
    RPLLoader_UnloadAll();
}

void RPLLoader_BeginCemuhookCRC(RPLModule* rpl)
{
	// calculate some values required for CRC
	sint32 sectionSymTableIndex = -1;
	sint32 sectionStrTableIndex = -1;
	for (sint32 i = 0; i < rpl->rplHeader.sectionTableEntryCount; i++)
	{
		if (rpl->sectionTablePtr[i].type == SHT_SYMTAB)
			sectionSymTableIndex = i;
		if (rpl->sectionTablePtr[i].type == SHT_STRTAB && i != rpl->rplHeader.nameSectionIndex && sectionStrTableIndex == -1)
			sectionStrTableIndex = i;
	}
	// init patches CRC
	rpl->patchCRC = 0;
	static const uint8 rplMagic[4] = { 0x7F, 'R', 'P', 'X' };
	rpl->patchCRC = crc32_calc(rpl->patchCRC, rplMagic, sizeof(rplMagic));
	sint32 sectionCount = rpl->rplHeader.sectionTableEntryCount;
	rpl->patchCRC = crc32_calc(rpl->patchCRC, &sectionCount, sizeof(sectionCount));
	rpl->patchCRC = crc32_calc(rpl->patchCRC, &sectionSymTableIndex, sizeof(sectionSymTableIndex));
	rpl->patchCRC = crc32_calc(rpl->patchCRC, &sectionStrTableIndex, sizeof(sectionStrTableIndex));
	sint32 sectionSectNameTableIndex = rpl->rplHeader.nameSectionIndex;
	rpl->patchCRC = crc32_calc(rpl->patchCRC, &sectionSectNameTableIndex, sizeof(sectionSectNameTableIndex));

	// sections
	for (sint32 i = 0; i < rpl->rplHeader.sectionTableEntryCount; i++)
	{
		auto sect = rpl->sectionTablePtr + i;
		uint32 nameOffset = sect->nameOffset;
		uint32 shType = sect->type;
		uint32 flags = sect->flags;
		uint32 virtualAddress = sect->virtualAddress;
		uint32 alignment = sect->alignment;
		uint32 sectionFileOffset = sect->fileOffset;
		uint32 sectionCompressedSize = sect->sectionSize;
		uint32 rawSize = 0;
		bool memoryAllocated = false;
		void* rawData = nullptr;
		if (shType == SHT_NOBITS)
		{
			rawData = NULL;
			rawSize = sectionCompressedSize;
		}
		else if ((flags&SHF_RPL_COMPRESSED) != 0)
		{
			uint32 decompressedSize = _swapEndianU32(*(uint32*)(rpl->RPLRawData.data() + sectionFileOffset));
			rawSize = decompressedSize;
			if (rawSize >= 1024*1024*1024)
			{
				cemuLog_logDebug(LogType::Force, "RPLLoader-CRC: Cannot load section {} which is too large ({} bytes)", i, decompressedSize);
				cemu_assert_debug(false);
				continue;
			}
			rawData = (uint8*)malloc(decompressedSize);
			if (rawData == nullptr)
			{
				cemuLog_logDebug(LogType::Force, "RPLLoader-CRC: Failed to allocate memory for uncompressed section {} ({} bytes)", i, decompressedSize);
				cemu_assert_debug(false);
				continue;
			}
			memoryAllocated = true;
			// decompress
			z_stream strm;
			strm.zalloc = Z_NULL;
			strm.zfree = Z_NULL;
			strm.opaque = Z_NULL;
			inflateInit(&strm);
			strm.avail_in = sectionCompressedSize - 4;
			strm.next_in = rpl->RPLRawData.data() + (uint32)sectionFileOffset + 4;
			strm.avail_out = decompressedSize;
			strm.next_out = (Bytef*)rawData;
			auto ret = inflate(&strm, Z_FULL_FLUSH);
			if (ret != Z_OK && ret != Z_STREAM_END || strm.avail_in != 0 || strm.avail_out != 0)
			{
				cemuLog_logDebug(LogType::Force, "RPLLoader-CRC: Unable to decompress section {}", i);
				cemuLog_logDebug(LogType::Force, "zRet {} availIn {} availOut {}", ret, (sint32)strm.avail_in, (sint32)strm.avail_out);
				cemu_assert_debug(false);
				free(rawData);
				inflateEnd(&strm);
				continue;
			}
			inflateEnd(&strm);
		}
		else
		{
			rawSize = sectionCompressedSize;
			rawData = rpl->RPLRawData.data() + sectionFileOffset;
		}

		rpl->patchCRC = crc32_calc(rpl->patchCRC, &nameOffset, sizeof(nameOffset));
		rpl->patchCRC = crc32_calc(rpl->patchCRC, &shType, sizeof(shType));
		rpl->patchCRC = crc32_calc(rpl->patchCRC, &flags, sizeof(flags));
		rpl->patchCRC = crc32_calc(rpl->patchCRC, &virtualAddress, sizeof(virtualAddress));
		rpl->patchCRC = crc32_calc(rpl->patchCRC, &rawSize, sizeof(rawSize));
		rpl->patchCRC = crc32_calc(rpl->patchCRC, &alignment, sizeof(alignment));

		if (rawData != nullptr && rawSize > 0)
		{
			rpl->patchCRC = crc32_calc(rpl->patchCRC, rawData, rawSize);
		}
		if (memoryAllocated && rawData)
			free(rawData);
	}
}

void RPLLoader_incrementModuleDependencyRefs(RPLModule* rpl)
{
	for (uint32 i = 0; i < (uint32)rpl->rplHeader.sectionTableEntryCount; i++)
	{
		if (rpl->sectionTablePtr[i].type != (uint32be)SHT_RPL_IMPORTS)
			continue;
		char* libName = (char*)((uint8*)rpl->sectionAddressTable2[i].ptr + 8);
		RPLLoader_AddDependency(libName);
	}
}

void RPLLoader_decrementModuleDependencyRefs(RPLModule* rpl)
{
	for (uint32 i = 0; i < (uint32)rpl->rplHeader.sectionTableEntryCount; i++)
	{
		if (rpl->sectionTablePtr[i].type != (uint32be)SHT_RPL_IMPORTS)
			continue;
		char* libName = (char*)((uint8*)rpl->sectionAddressTable2[i].ptr + 8);
		RPLLoader_RemoveDependency(libName);
	}
}

void RPLLoader_UpdateEntrypoint(RPLModule* rpl)
{
	uint32 virtualEntrypoint = rpl->rplHeader.entrypoint;
	uint32 entrypoint = 0xFFFFFFFF;
	for (sint32 i = 0; i < (sint32)rpl->rplHeader.sectionTableEntryCount; i++)
	{
		rplSectionEntryNew_t* section = rpl->sectionTablePtr + i;
		uint32 sectionStartAddr = (uint32)section->virtualAddress;
		uint32 sectionEndAddr = (uint32)section->virtualAddress + (uint32)section->sectionSize;
		if (virtualEntrypoint >= sectionStartAddr && virtualEntrypoint < sectionEndAddr)
		{
			cemu_assert_debug(entrypoint == 0xFFFFFFFF);
			entrypoint = (virtualEntrypoint - sectionStartAddr + memory_getVirtualOffsetFromPointer(rpl->sectionAddressTable2[i].ptr));
		}
	}
	cemu_assert(entrypoint != 0xFFFFFFFF);
	rpl->entrypoint = entrypoint;
}

void RPLLoader_InitModuleAllocator(RPLModule* rpl)
{
	if (!rplLoader_applicationHasMemoryControl ||
		(rpl->externalModule && !rpl->externalUseApplicationAllocator))
	{
		rpl->funcAlloc = 0;
		rpl->funcFree = 0;
		return;
	}
	coreinit::OSDynLoad_GetAllocator(&rpl->funcAlloc, &rpl->funcFree);
}

void RPLLoader_DiscardPartiallyLoadedModule(RPLModule* rpl)
{
	if (!rpl)
		return;
	if (rpl->regionMappingBase_text)
		rplLoaderHeap_codeArea2.free(rpl->regionMappingBase_text.GetPtr());
	if (rpl->funcFree)
	{
		RPLLoader_FreeData(rpl, MEMPTR<void>(rpl->regionMappingBase_data).GetPtr());
		RPLLoader_FreeData(rpl, MEMPTR<void>(rpl->regionMappingBase_loaderInfo).GetPtr());
	}
	rpl->heapTrampolineArea.releaseAll();
	if (rpl->tempRegionPtr)
		RPLLoader_FreeWorkarea(rpl->tempRegionPtr);
	free(rpl->sectionTablePtr);
	delete rpl;
}

// Map an RPL into memory, but do not resolve relocations and imports yet.
// externalOptions is intentionally absent for the legacy title path.
RPLModule* RPLLoader_LoadFromMemoryInternal(uint8* rplData, sint32 size,
	std::string_view name, const RPLLoadOptions* externalOptions)
{
	std::lock_guard lock(g_rplLoaderMutex);
	std::string moduleName = _RPLLoader_ExtractModuleNameFromPath(name);
	RPLModule* rpl = nullptr;
	if (RPLLoader_ProcessHeaders({ moduleName }, rplData, size, &rpl) == false)
	{
		delete rpl;
		return nullptr;
	}
	rpl->externalLifetimeId = RPLLoader_AllocateLifetimeId();
	if (rpl->externalLifetimeId == 0)
	{
		cemuLog_log(LogType::Force, "RPLLoader: exhausted module lifetime IDs");
		RPLLoader_DiscardPartiallyLoadedModule(rpl);
		return nullptr;
	}
	if (externalOptions)
	{
		rpl->externalModule = true;
		rpl->externalCallEntrypoint = externalOptions->callEntrypoint;
		rpl->externalRegisterDependency = externalOptions->registerDependency;
		rpl->externalUseApplicationAllocator = externalOptions->useApplicationAllocator;
		rpl->externalOwner = externalOptions->owner;
		rpl->externalGeneration = externalOptions->generation;
		rpl->externalImportResolver = externalOptions->resolveImport;
		rpl->externalModuleHandle = rplLoader_currentHandleCounter++;
		rpl->fileInfo.tlsModuleIndex = rplLoader_currentTlsModuleIndex++;
		rpl->entrypointCalled = !externalOptions->callEntrypoint;
	}
	RPLLoader_InitModuleAllocator(rpl);
	RPLLoader_BeginCemuhookCRC(rpl);
	if (RPLLoader_LoadSections(0, rpl) == false)
	{
		RPLLoader_DiscardPartiallyLoadedModule(rpl);
		return nullptr;
	}
	cemuLog_logDebug(LogType::Force, "Load {} Code-Offset: -0x{:x}", name, rpl->regionMappingBase_text.GetMPTR() - 0x02000000);
	// sdata (r2/r13)
	uint32 sdataBaseAddress = rpl->fileInfo.sdataBase1; // base + 0x8000
	uint32 sdataBaseAddress2 = rpl->fileInfo.sdataBase2; // base + 0x8000
	for (uint32 i = 0; i < (uint32)rpl->rplHeader.sectionTableEntryCount; i++)
	{
		if(rpl->sectionTablePtr[i].sectionSize == 0)
			continue;
		uint32 sectionFlags = rpl->sectionTablePtr[i].flags;
		uint32 sectionVirtualAddress = rpl->sectionTablePtr[i].virtualAddress;
		uint32 sectionSize = rpl->sectionTablePtr[i].sectionSize;
		if( (sectionFlags&4) != 0 )
			continue;
		if(sdataBaseAddress == 0x00008000 && sdataBaseAddress2 == 0x00008000)
			continue; // sdata not used (this workaround is needed for Wii U Party to boot)
		// sdata 1
		if ((sdataBaseAddress - 0x8000) >= (sectionVirtualAddress) &&
			(sdataBaseAddress - 0x8000) <= (sectionVirtualAddress + sectionSize))
		{
			rpl->mappedSda1Base = memory_getVirtualOffsetFromPointer(
				rpl->sectionAddressTable2[i].ptr) + (sdataBaseAddress - sectionVirtualAddress);
			if (!rpl->externalModule)
				rplLoader_sdataAddr = rpl->mappedSda1Base;
		}
		// sdata 2
		if ((sdataBaseAddress2 - 0x8000) >= (sectionVirtualAddress) &&
			(sdataBaseAddress2 - 0x8000) <= (sectionVirtualAddress + sectionSize))
		{
			rpl->mappedSda2Base = memory_getVirtualOffsetFromPointer(
				rpl->sectionAddressTable2[i].ptr) + (sdataBaseAddress2 - sectionVirtualAddress);
			if (!rpl->externalModule)
				rplLoader_sdata2Addr = rpl->mappedSda2Base;
		}

	}
	// cancel if error
	if (rpl->hasError)
	{
		cemuLog_log(LogType::Force, "RPLLoader: Unable to load RPL due to errors");
		RPLLoader_DiscardPartiallyLoadedModule(rpl);
		return nullptr;
	}
	// find TLS section
	uint32 tlsStartAddress = 0xFFFFFFFF;
	uint32 tlsEndAddress = 0;
	for (uint32 i = 0; i < (uint32)rpl->rplHeader.sectionTableEntryCount; i++)
	{
		if (((uint32)rpl->sectionTablePtr[i].flags & SHF_TLS_MASK) == 0)
			continue;
		uint32 sectionVirtualAddress = rpl->sectionTablePtr[i].virtualAddress;
		uint32 sectionSize = rpl->sectionTablePtr[i].sectionSize;
		tlsStartAddress = std::min(tlsStartAddress, sectionVirtualAddress);
		tlsEndAddress = std::max(tlsEndAddress, sectionVirtualAddress+sectionSize);
	}
	if (tlsStartAddress == 0xFFFFFFFF)
	{
		tlsStartAddress = 0;
		tlsEndAddress = 0;
	}
	rpl->tlsStartAddress = tlsStartAddress;
	rpl->tlsEndAddress = tlsEndAddress;

	// add to module list
	cemu_assert(rplModuleCount < 256);
	rplModuleList[rplModuleCount] = rpl;
	rplModuleCount++;

	// track dependencies
	if (!rpl->externalModule || rpl->externalRegisterDependency)
		RPLLoader_incrementModuleDependencyRefs(rpl);

	// update entrypoint
	RPLLoader_UpdateEntrypoint(rpl);
	RPLLoader_EmitModuleEvent(RPLModuleEventType::Mapped, rpl);
	return rpl;
}

RPLModule* RPLLoader_LoadFromMemory(uint8* rplData, sint32 size, std::string_view name)
{
	return RPLLoader_LoadFromMemoryInternal(rplData, size, name, nullptr);
}

bool RPLLoader_ValidateExternalImage(std::span<const uint8> image,
	const RPLLoadOptions& options, RPLExternalMarker& marker, std::string& error)
{
	constexpr uint64 kMaximumExpandedImageSize = 64ULL * 1024ULL * 1024ULL;
	auto u16 = [&image](size_t offset) {
		return (uint16(image[offset]) << 8) | uint16(image[offset + 1]);
	};
	auto u32 = [&image](size_t offset) {
		return (uint32(image[offset]) << 24) | (uint32(image[offset + 1]) << 16) |
			(uint32(image[offset + 2]) << 8) | uint32(image[offset + 3]);
	};
	auto range = [&image](uint64 offset, uint64 size) {
		return offset <= image.size() && size <= image.size() - offset;
	};
	if (image.size() < sizeof(rplHeaderNew_t) ||
		image.size() > static_cast<size_t>(std::numeric_limits<sint32>::max()) ||
		u32(0) != 0x7f454c46 || image[4] != 1 || image[5] != 2 || image[6] != 1 ||
		image[7] != 0xca || image[8] != 0xfe || u16(16) != 0xfe01 ||
		u16(18) != 20 || u32(20) != 1 || u16(40) != sizeof(rplHeaderNew_t))
	{
		error = "external module is not a Wii U PPC32 big-endian RPL";
		return false;
	}
	marker = RPLExternalMarker::None;
	if (image[9] == 'P' && image[10] == 'L')
		marker = RPLExternalMarker::Wups;
	else if (image[9] == 0xaf && image[10] == 0xfe)
		marker = RPLExternalMarker::Wums;
	if ((marker == RPLExternalMarker::Wups && !options.allowWupsMarker) ||
		(marker == RPLExternalMarker::Wums && !options.allowWumsMarker))
	{
		error = marker == RPLExternalMarker::Wups ?
			"external module has a WUPS marker but allowWupsMarker is false" :
			"external module has a WUMS marker but allowWumsMarker is false";
		return false;
	}
	const uint32 sectionOffset = u32(32);
	const uint16 sectionSize = u16(46);
	const uint16 sectionCount = u16(48);
	const uint16 nameSection = u16(50);
	if (u32(28) != 0 || u16(44) != 0 || sectionSize != sizeof(rplSectionEntryNew_t) ||
		sectionCount < 2 || sectionCount > 512 || nameSection >= sectionCount ||
		!range(sectionOffset, uint64(sectionSize) * sectionCount))
	{
		error = "external RPL section table is invalid";
		return false;
	}
	auto sectionField = [&](uint32 index, size_t fieldOffset) {
		return u32(sectionOffset + size_t(index) * sectionSize + fieldOffset);
	};
	std::vector<uint32> expandedSectionSizes(sectionCount);
	std::vector<RPLLoaderInternal::ExternalSectionMapping> sectionMappings(sectionCount);
	uint64 totalExpandedSize = 0;
	bool entrypointMapped = false;
	for (uint32 index = 0; index < sectionCount; ++index)
	{
		const size_t offset = sectionOffset + size_t(index) * sectionSize;
		const uint32 type = u32(offset + 4);
		const uint32 flags = u32(offset + 8);
		const uint32 virtualAddress = u32(offset + 12);
		const uint32 fileOffset = u32(offset + 16);
		const uint32 storedSize = u32(offset + 20);
		const uint32 alignment = u32(offset + 32);
		if (alignment && ((alignment & (alignment - 1)) != 0 || alignment > 0x10000))
		{
			error = fmt::format("external RPL section {} has invalid alignment", index);
			return false;
		}
		if (storedSize && type != SHT_NOBITS && !range(fileOffset, storedSize))
		{
			error = fmt::format("external RPL section {} exceeds the image", index);
			return false;
		}
		uint32 expandedSize = storedSize;
		if ((flags & SHF_RPL_COMPRESSED) != 0)
		{
			if (type == SHT_NOBITS || storedSize < sizeof(uint32) ||
				!range(fileOffset, sizeof(uint32)))
			{
				error = fmt::format(
					"external RPL section {} has an invalid compressed payload", index);
				return false;
			}
			expandedSize = u32(fileOffset);
		}
		if (expandedSize > kMaximumExpandedImageSize ||
			totalExpandedSize > kMaximumExpandedImageSize - expandedSize)
		{
			error = "external RPL expanded sections exceed the 64 MiB limit";
			return false;
		}
		totalExpandedSize += expandedSize;
		expandedSectionSizes[index] = expandedSize;
		sectionMappings[index] = {type, flags, virtualAddress, expandedSize};
		if (expandedSize && virtualAddress >
			std::numeric_limits<uint32>::max() - expandedSize)
		{
			error = fmt::format("external RPL section {} address wraps", index);
			return false;
		}
		if (alignment && virtualAddress && (virtualAddress & (alignment - 1)) != 0)
		{
			error = fmt::format("external RPL section {} is misaligned", index);
			return false;
		}
		const uint32 entrypoint = u32(24);
		if ((flags & (2U | SHF_EXECUTE)) == (2U | SHF_EXECUTE) &&
			entrypoint >= virtualAddress && entrypoint - virtualAddress < expandedSize)
			entrypointMapped = true;
	}
	const size_t crcHeader = sectionOffset + size_t(sectionCount - 2) * sectionSize;
	const size_t fileInfoHeader = sectionOffset + size_t(sectionCount - 1) * sectionSize;
	if (u32(crcHeader + 4) != SHT_RPL_CRCS ||
		u32(fileInfoHeader + 4) != SHT_RPL_FILEINFO ||
		u32(crcHeader + 20) != uint32(sectionCount) * sizeof(uint32) ||
		u32(fileInfoHeader + 20) < sizeof(RPLFileInfoData) ||
		(u32(crcHeader + 8) & SHF_RPL_COMPRESSED) != 0 ||
		(u32(fileInfoHeader + 8) & SHF_RPL_COMPRESSED) != 0)
	{
		error = "external RPL must end with exact uncompressed CRC and FILEINFO sections";
		return false;
	}
	const uint32 fileInfoOffset = u32(fileInfoHeader + 16);
	if (u32(fileInfoOffset) != 0xcafe0402)
	{
		error = "external RPL FILEINFO magic is invalid";
		return false;
	}
	const uint32 textRegionSize = u32(fileInfoOffset + 4);
	const uint32 dataRegionSize = u32(fileInfoOffset + 12);
	const uint32 dataAlignment = u32(fileInfoOffset + 16);
	const uint32 loaderRegionSize = u32(fileInfoOffset + 20);
	const uint32 trampolineAdjustment = u32(fileInfoOffset + 32);
	const uint32 loaderAdjustment = u32(fileInfoOffset + 76);
	if (textRegionSize > kMaximumExpandedImageSize ||
		dataRegionSize > kMaximumExpandedImageSize ||
		loaderRegionSize > kMaximumExpandedImageSize ||
		trampolineAdjustment > textRegionSize ||
		loaderAdjustment > loaderRegionSize ||
		(dataAlignment && ((dataAlignment & (dataAlignment - 1)) != 0 ||
			dataAlignment > 0x10000)))
	{
		error = "external RPL FILEINFO describes invalid mapping regions";
		return false;
	}
	const RPLLoaderInternal::ExternalFileInfoMapping fileInfoMapping{
		textRegionSize,
		dataRegionSize,
		loaderRegionSize,
		trampolineAdjustment,
		loaderAdjustment,
	};
	if (const auto violation = RPLLoaderInternal::FindExternalMappingViolation(
		sectionMappings, fileInfoMapping))
	{
		const char* regionName = "unknown";
		switch (violation->region)
		{
		case RPLLoaderInternal::ExternalMappingRegion::Text:
			regionName = "text";
			break;
		case RPLLoaderInternal::ExternalMappingRegion::Data:
			regionName = "data";
			break;
		case RPLLoaderInternal::ExternalMappingRegion::Loader:
			regionName = "loader";
			break;
		case RPLLoaderInternal::ExternalMappingRegion::None:
			break;
		}
		if (violation->reason ==
			RPLLoaderInternal::ExternalMappingViolation::Reason::RegionAddressOverflow)
			error = fmt::format(
				"external RPL FILEINFO {} mapping region for section {} overflows "
				"the 32-bit guest address space: [0x{:08x}, 0x{:x})",
				regionName, violation->sectionIndex, violation->regionBegin,
				violation->regionEnd);
		else
			error = fmt::format(
				"external RPL section {} expanded range [0x{:08x}, 0x{:08x}) "
				"exceeds its FILEINFO {} mapping region [0x{:08x}, 0x{:08x})",
				violation->sectionIndex, violation->sectionBegin, violation->sectionEnd,
				regionName, violation->regionBegin, violation->regionEnd);
		return false;
	}
	if (!entrypointMapped)
	{
		error = "external RPL entrypoint is outside executable module memory";
		return false;
	}

	auto readSection = [&](uint32 index, std::vector<uint8>& output) {
		const uint32 type = sectionField(index, 4);
		const uint32 flags = sectionField(index, 8);
		const uint32 fileOffset = sectionField(index, 16);
		const uint32 storedSize = sectionField(index, 20);
		const uint32 expandedSize = expandedSectionSizes[index];
		output.clear();
		if (type == SHT_NOBITS)
		{
			output.resize(expandedSize);
			return true;
		}
		if ((flags & SHF_RPL_COMPRESSED) == 0)
		{
			output.assign(image.begin() + fileOffset,
				image.begin() + fileOffset + storedSize);
			return true;
		}
		output.resize(expandedSize);
		uLongf outputSize = expandedSize;
		const int status = uncompress(output.data(), &outputSize,
			image.data() + fileOffset + sizeof(uint32),
			storedSize - sizeof(uint32));
		return status == Z_OK && outputSize == expandedSize;
	};
	auto nulTerminatedAt = [](std::span<const uint8> data, uint32 offset) {
		return offset < data.size() &&
			std::find(data.begin() + offset, data.end(), uint8{0}) != data.end();
	};
	std::vector<uint8> sectionData;
	std::vector<uint8> linkedData;
	for (uint32 index = 0; index < sectionCount; ++index)
		if ((sectionField(index, 8) & SHF_RPL_COMPRESSED) != 0 &&
			!readSection(index, sectionData))
		{
			error = fmt::format("external RPL section {} cannot be decompressed", index);
			return false;
		}
	for (uint32 index = 0; index < sectionCount; ++index)
	{
		const uint32 type = sectionField(index, 4);
		if (type != SHT_RPL_IMPORTS && type != SHT_RPL_EXPORTS &&
			type != SHT_SYMTAB && type != SHT_RELA)
			continue;
		if (!readSection(index, sectionData))
		{
			error = fmt::format("external RPL section {} cannot be decompressed", index);
			return false;
		}
		if (type == SHT_RPL_IMPORTS)
		{
			if (sectionData.size() < 9 ||
				!nulTerminatedAt(sectionData, 8))
			{
				error = fmt::format(
					"external RPL import section {} has no terminated module name", index);
				return false;
			}
			continue;
		}
		if (type == SHT_RPL_EXPORTS)
		{
			if (sectionData.size() < 8)
			{
				error = fmt::format("external RPL export section {} is truncated", index);
				return false;
			}
			const auto be32 = [&sectionData](size_t offset) {
				return (uint32(sectionData[offset]) << 24) |
					(uint32(sectionData[offset + 1]) << 16) |
					(uint32(sectionData[offset + 2]) << 8) |
					uint32(sectionData[offset + 3]);
			};
			const uint32 count = be32(0);
			if (count > (sectionData.size() - 8) / sizeof(rplExportTableEntry_t))
			{
				error = fmt::format(
					"external RPL export section {} has an invalid descriptor count", index);
				return false;
			}
			for (uint32 exportIndex = 0; exportIndex < count; ++exportIndex)
			{
				const uint32 nameOffset = be32(8 + exportIndex * 8 + 4);
				if (!nulTerminatedAt(sectionData, nameOffset))
				{
					error = fmt::format(
						"external RPL export section {} has an invalid symbol name", index);
					return false;
				}
			}
			continue;
		}
		if (type == SHT_SYMTAB)
		{
			const uint32 entrySize = sectionField(index, 36) == 0 ?
				sizeof(RPLFileSymtabEntry) : sectionField(index, 36);
			const uint32 stringSection = sectionField(index, 24);
			if (entrySize != sizeof(RPLFileSymtabEntry) ||
				sectionData.size() % entrySize != 0 ||
				stringSection >= sectionCount ||
				sectionField(stringSection, 4) != SHT_STRTAB ||
				!readSection(stringSection, linkedData))
			{
				error = fmt::format("external RPL symbol table {} is invalid", index);
				return false;
			}
			for (size_t symbolOffset = 0; symbolOffset < sectionData.size();
				symbolOffset += entrySize)
			{
				const uint32 nameOffset =
					(uint32(sectionData[symbolOffset]) << 24) |
					(uint32(sectionData[symbolOffset + 1]) << 16) |
					(uint32(sectionData[symbolOffset + 2]) << 8) |
					uint32(sectionData[symbolOffset + 3]);
				if (!nulTerminatedAt(linkedData, nameOffset))
				{
					error = fmt::format(
						"external RPL symbol table {} has an invalid name", index);
					return false;
				}
			}
			continue;
		}
		const uint32 symbolSection = sectionField(index, 24);
		const uint32 targetSection = sectionField(index, 28);
		if (sectionData.size() % sizeof(rplRelocNew_t) != 0 ||
			symbolSection >= sectionCount || targetSection >= sectionCount ||
			sectionField(symbolSection, 4) != SHT_SYMTAB ||
			expandedSectionSizes[symbolSection] < 2 * sizeof(RPLFileSymtabEntry) ||
			expandedSectionSizes[symbolSection] % sizeof(RPLFileSymtabEntry) != 0)
		{
			error = fmt::format("external RPL relocation section {} is invalid", index);
			return false;
		}
		const uint32 symbolCount =
			expandedSectionSizes[symbolSection] / sizeof(RPLFileSymtabEntry);
		const uint32 targetAddress = sectionField(targetSection, 12);
		const uint32 targetSize = expandedSectionSizes[targetSection];
		const auto relocBe32 = [&sectionData](size_t offset) {
			return (uint32(sectionData[offset]) << 24) |
				(uint32(sectionData[offset + 1]) << 16) |
				(uint32(sectionData[offset + 2]) << 8) |
				uint32(sectionData[offset + 3]);
		};
		for (size_t relocOffset = 0; relocOffset < sectionData.size();
			relocOffset += sizeof(rplRelocNew_t))
		{
			const uint32 destination = relocBe32(relocOffset);
			const uint32 symbolAndType = relocBe32(relocOffset + 4);
			const uint32 relocationType = symbolAndType & 0xff;
			const uint32 symbolIndex = symbolAndType >> 8;
			if (relocationType == 0)
				continue;
			const bool supported =
				relocationType == RPL_RELOC_ADDR32 ||
				relocationType == RPL_RELOC_LO16 ||
				relocationType == RPL_RELOC_HI16 ||
				relocationType == RPL_RELOC_HA16 ||
				relocationType == RPL_RELOC_REL24 ||
				relocationType == RPL_RELOC_REL14 ||
				relocationType == R_PPC_DTPMOD32 ||
				relocationType == R_PPC_DTPREL32 ||
				relocationType == R_PPC_REL16_HA ||
				relocationType == R_PPC_REL16_HI ||
				relocationType == R_PPC_REL16_LO ||
				relocationType == 0x6d;
			if (!supported || symbolIndex >= symbolCount ||
				destination < targetAddress ||
				destination - targetAddress > targetSize ||
				sizeof(uint32) > targetSize - (destination - targetAddress))
			{
				error = fmt::format(
					"external RPL relocation section {} contains an invalid entry",
					index);
				return false;
			}
		}
	}
	return true;
}

RPLModule* RPLLoader_LoadExternalModuleFromMemory(std::span<const uint8> image,
	std::string_view name, const RPLLoadOptions& options, uint64& lifetimeId,
	std::string& error)
{
	std::lock_guard lock(g_rplLoaderMutex);
	error.clear();
	lifetimeId = 0;
	if (name.empty() || name.size() >= RPL_MODULE_PATH_LENGTH)
	{
		error = "external RPL name is empty or too long";
		return nullptr;
	}
	const size_t leaf = name.find_last_of('/') == std::string_view::npos ?
		0 : name.find_last_of('/') + 1;
	if (leaf >= name.size() || name[leaf] == '.')
	{
		error = "external RPL name has no module basename";
		return nullptr;
	}
	RPLExternalMarker marker{};
	if (!RPLLoader_ValidateExternalImage(image, options, marker, error))
		return nullptr;
	if (!options.useApplicationAllocator)
	{
		error =
			"external RPL loading with useApplicationAllocator=false is unsupported: "
			"the pre-application bump allocator cannot reclaim module mappings; load "
			"after application memory control with useApplicationAllocator=true";
		return nullptr;
	}
	if (!rplLoader_applicationHasMemoryControl)
	{
		error =
			"external RPL application allocation is unavailable before application memory control";
		return nullptr;
	}
	if (!PPCInterpreter_getCurrentInstance())
	{
		error = "external RPL application allocation requires an emulated CPU thread";
		return nullptr;
	}
	const auto moduleName = _RPLLoader_ExtractModuleNameFromPath(name);
	if (RPLLoader_FindModuleByName(moduleName))
	{
		error = fmt::format("RPL module name '{}' is already loaded", moduleName);
		return nullptr;
	}
	std::vector<uint8> ownedImage(image.begin(), image.end());
	RPLModule* module = RPLLoader_LoadFromMemoryInternal(ownedImage.data(),
		static_cast<sint32>(ownedImage.size()), name, &options);
	if (!module)
	{
		error = fmt::format("failed to map external {} module '{}'",
			marker == RPLExternalMarker::Wups ? "WUPS" :
			marker == RPLExternalMarker::Wums ? "WUMS" : "RPL", name);
		return nullptr;
	}
	module->ownedRPLRawData = std::move(ownedImage);
	module->RPLRawData = module->ownedRPLRawData;
	lifetimeId = module->externalLifetimeId;
	return module;
}

void RPLLoader_FlushMemory(RPLModule* rpl)
{
	// invalidate recompiler cache
	PPCRecompiler_invalidateRange(rpl->regionMappingBase_text.GetMPTR(), rpl->regionMappingBase_text.GetMPTR() + rpl->regionSize_text);
	rpl->heapTrampolineArea.forEachBlock([](void* mem, uint32 size)
	{
		MEMPTR<void> memVirtual = mem;
		PPCRecompiler_invalidateRange(memVirtual.GetMPTR(), memVirtual.GetMPTR() + size);
	});
}

// resolve relocs and imports of all modules. Or resolve exports
void RPLLoader_LinkSingleModule(RPLModule* rplLoaderContext, bool resolveOnlyExports)
{
	// setup shared import tracking
	std::vector<RPLSharedImportTracking> sharedImportTracking;

	sharedImportTracking.resize(rplLoaderContext->rplHeader.sectionTableEntryCount - 2);

	memset(sharedImportTracking.data(), 0, sizeof(RPLSharedImportTracking) * sharedImportTracking.size());

	for (uint32 i = 0; i < (uint32)rplLoaderContext->rplHeader.sectionTableEntryCount; i++)
	{
		if( rplLoaderContext->sectionTablePtr[i].type != (uint32be)SHT_RPL_IMPORTS )
			continue;
		cemu_assert(rplLoaderContext->sectionTablePtr[i].sectionSize >= 9);
		char* libName = (char*)((uint8*)rplLoaderContext->sectionAddressTable2[i].ptr + 8);
		// make module name
		std::string importModuleName = _RPLLoader_ExtractModuleNameFromPath(libName);
			bool foundModule = false;
			for (sint32 f = 0; f < rplModuleCount; f++)
			{
				RPLModule* candidate = rplModuleList[f];
				const bool visibleToImporter =
					RPLLoaderInternal::IsVisibleThroughOrdinaryModuleScan(
						candidate->externalModule,
						candidate->externalRegisterDependency);
				if (visibleToImporter && candidate->moduleName == importModuleName)
				{
					sharedImportTracking[i].rplLoaderContext = candidate;
				memset(sharedImportTracking[i].modulename, 0, sizeof(sharedImportTracking[i].modulename));
				strcpy_s(sharedImportTracking[i].modulename, importModuleName.c_str());
				foundModule = true;
				break;
			}
		}
		if( foundModule )
			continue;
		// if not found, we assume it's a HLE lib
		sharedImportTracking[i].rplLoaderContext = HLE_MODULE_PTR;
		sharedImportTracking[i].exportSection = nullptr;
		strcpy_s(sharedImportTracking[i].modulename, libName);
	}

	if (resolveOnlyExports)
		RPLLoader_HandleRelocs(rplLoaderContext, sharedImportTracking, 2);
	else
		RPLLoader_HandleRelocs(rplLoaderContext, sharedImportTracking, 0);

	RPLLoader_FlushMemory(rplLoaderContext);
}

void RPLLoader_LoadSectionDebugSymbols(RPLModule* rplLoaderContext, rplSectionEntryNew_t* section, int symtabSectionIndex)
{
	uint32 sectionSize = section->sectionSize;
	uint32 symbolEntrySize = section->ukn24;
	if (symbolEntrySize == 0)
		symbolEntrySize = 0x10;
	cemu_assert(symbolEntrySize == 0x10);
	cemu_assert((sectionSize % symbolEntrySize) == 0);
	uint32 symbolCount = sectionSize / symbolEntrySize;
	cemu_assert(symbolCount >= 2);

	uint16 sectionCount = rplLoaderContext->rplHeader.sectionTableEntryCount;
	uint8* symtabData = (uint8*)rplLoaderContext->sectionAddressTable2[symtabSectionIndex].ptr;

	uint32 strtabSectionIndex = section->symtabSectionIndex;
	uint8* strtabData = (uint8*)rplLoaderContext->sectionAddressTable2[strtabSectionIndex].ptr;

	for (uint32 i = 0; i < symbolCount; i++)
	{
		RPLFileSymtabEntry* sym = (RPLFileSymtabEntry*)(symtabData + i * symbolEntrySize);

		uint16 symSectionIndex = sym->sectionIndex;
		if (symSectionIndex == 0 || symSectionIndex >= sectionCount)
			continue;
		void* symbolSectionAddress = rplLoaderContext->sectionAddressTable2[symSectionIndex].ptr;
		if (symbolSectionAddress == nullptr)
			continue;
		rplSectionEntryNew_t* symbolSection = rplLoaderContext->sectionTablePtr + symSectionIndex;
		if(symbolSection->type == SHT_RPL_EXPORTS || symbolSection->type == SHT_RPL_IMPORTS)
			continue; // exports and imports are handled separately

		uint32 symbolOffset = sym->symbolAddress - symbolSection->virtualAddress;

		uint32 nameOffset = sym->ukn00;
		if (nameOffset > 0)
		{
			char* symbolName = (char*)strtabData + nameOffset;
			if (sym->info == 0x12)
			{
				rplSymbolStorage_store(rplLoaderContext->moduleName.c_str(), symbolName, sym->symbolAddress);
			}
		}
	}
}

void RPLLoader_LoadDebugSymbols(RPLModule* rplLoaderContext)
{
	for (sint32 i = 0; i < (sint32)rplLoaderContext->rplHeader.sectionTableEntryCount; i++)
	{
		rplSectionEntryNew_t* section = rplLoaderContext->sectionTablePtr + i;
		uint32 sectionType = section->type;
		if (sectionType != SHT_SYMTAB)
			continue;
		RPLLoader_LoadSectionDebugSymbols(rplLoaderContext, section, i);
	}
}

void RPLLoader_DestroyModule(RPLModule* rpl, RPLDependency* rplDependency,
	bool skipPPCCalls, bool releaseData)
{
	std::lock_guard lock(g_rplLoaderMutex);
	/*
	  A note:
	  Mario Party 10's mg0408.rpl (minigame Spike Ball Scramble) has a bug where it keeps running code (function 0x02086BCC for example) after RPL unload
	  It seems to rely on the RPL loader not zeroing released memory
	*/

	if (rplDependency && rplDependency->rplHLEModule)
	{
		cemu_assert_debug(!rplDependency->rplLoaderContext);
		// HLE module unload logic is handled by parent functions for now
		return;
	}
	if (!rpl)
		return;
	if (rpl->externalAccessCount != 0 || rpl->externalEventInFlight)
	{
		cemuLog_log(LogType::Force,
			"RPLLoader: refused to destroy externally observed or leased module '{}'",
			rpl->moduleName);
		return;
	}
	rpl->externalUnloading = rpl->externalModule;
	RPLLoader_EmitModuleEvent(RPLModuleEventType::Unloading, rpl);
	// decrease reference counters of all dependencies
	if (!rpl->externalModule || rpl->externalRegisterDependency)
		RPLLoader_decrementModuleDependencyRefs(rpl);
	// save module config for this module in the debugger
	g_debuggerDispatcher.NotifyModuleUnloaded(rpl);
	// call rpl_entry with reason unload
	if (!skipPPCCalls && (!rpl->externalModule ||
		(rpl->entrypointCalled && rpl->externalCallEntrypoint)))
	{
		cemu_assert_debug(PPCInterpreter_getCurrentInstance()); // must be running on a CPU emulation thread
		if (rpl->entrypoint)
		{
			const uint32 handle = rplDependency ? rplDependency->coreinitHandle :
				rpl->externalModuleHandle;
			PPCCoreCallback(rpl->entrypoint, handle, 2); // 2 -> unload
		}
	}
	// release memory
	if (rpl->regionMappingBase_text)
	{
		rplLoaderHeap_codeArea2.free(rpl->regionMappingBase_text.GetPtr());
		rpl->regionMappingBase_text = nullptr;
	}

	// for some reason freeing the data allocations causes a crash in MP10 on boot
	if (releaseData && rpl->funcFree)
	{
		RPLLoader_FreeData(rpl, MEMPTR<void>(rpl->regionMappingBase_data).GetPtr());
		rpl->regionMappingBase_data = 0;
		RPLLoader_FreeData(rpl, MEMPTR<void>(rpl->regionMappingBase_loaderInfo).GetPtr());
		rpl->regionMappingBase_loaderInfo = 0;
	}
	rpl->heapTrampolineArea.releaseAll();

	// todo - remove from rplSymbolStorage_store

	if (rpl->sectionTablePtr)
	{
		free(rpl->sectionTablePtr);
		rpl->sectionTablePtr = nullptr;
	}

	// unload temp region
	if (rpl->tempRegionPtr)
	{
		RPLLoader_FreeWorkarea(rpl->tempRegionPtr);
		rpl->tempRegionPtr = nullptr;
	}

	// remove from rpl module list
	for (sint32 i = 0; i < rplModuleCount; i++)
	{
		if (rplModuleList[i] == rpl)
		{
			rplModuleList[i] = rplModuleList[rplModuleCount-1];
			rplModuleCount--;
			break;
		}
	}

	RPLLoader_EmitModuleEvent(RPLModuleEventType::Unloaded, rpl);
	delete rpl;
}

void RPLLoader_UnloadModule(RPLDependency* rplDependency, bool skipPPCCalls)
{
	std::lock_guard lock(g_rplLoaderMutex);
	if (!rplDependency)
		return;
	RPLLoader_DestroyModule(rplDependency->rplLoaderContext, rplDependency,
		skipPPCCalls, !skipPPCCalls);
}

void RPLLoader_FixModuleTLSIndex(RPLModule* rplLoaderContext)
{
	sint16 tlsModuleIndex = -1;
	for (auto& dep : rplDependencyList)
	{
		if (rplLoaderContext->moduleName == dep->moduleName)
		{
			tlsModuleIndex = dep->tlsModuleIndex;
			break;
		}
	}
	cemu_assert(tlsModuleIndex != -1);
	rplLoaderContext->fileInfo.tlsModuleIndex = tlsModuleIndex;
}

void RPLLoader_Link()
{
	std::lock_guard lock(g_rplLoaderMutex);
	// calculate TLS index
	for (sint32 i = 0; i < rplModuleCount; i++)
	{
		if (rplModuleList[i]->isLinked || rplModuleList[i]->externalModule)
			continue;
		RPLLoader_FixModuleTLSIndex(rplModuleList[i]);
	}
	// resolve relocs
	for (sint32 i = 0; i < rplModuleCount; i++)
	{
		if (rplModuleList[i]->isLinked || rplModuleList[i]->externalModule)
			continue;
		RPLLoader_LinkSingleModule(rplModuleList[i], false);
	}
	// resolve imports and load debug symbols
	for (sint32 i = 0; i < rplModuleCount; i++)
	{
		if (rplModuleList[i]->isLinked || rplModuleList[i]->externalModule)
			continue;
		RPLLoader_LinkSingleModule(rplModuleList[i], true);
		RPLLoader_LoadDebugSymbols(rplModuleList[i]);
		rplModuleList[i]->isLinked = true; // mark as linked
		GraphicPack2::NotifyModuleLoaded(rplModuleList[i]);
		g_debuggerDispatcher.NotifyModuleLoaded(rplModuleList[i]);
		RPLLoader_EmitModuleEvent(RPLModuleEventType::Linked, rplModuleList[i]);
	}
}

bool RPLLoader_LinkExternalModule(RPLModule* module, uint64 lifetimeId,
	std::string& error)
{
	error.clear();
	RPLModuleLease operationLease;
	if (!RPLLoader_AcquireExternalModuleLease(
		module, lifetimeId, operationLease, error))
	{
		error = "external RPL link rejected a stale or unloading module lifetime";
		return false;
	}
	std::lock_guard lock(g_rplLoaderMutex);
	if (module->isLinked)
		return true;
	if (module->externalCallEntrypoint &&
		(!module->entrypoint || !PPCInterpreter_getCurrentInstance()))
	{
		error = !module->entrypoint ? "external RPL has no entrypoint" :
			"external RPL entrypoint requires an emulated CPU thread";
		return false;
	}
	if (module->externalRegisterDependency)
		RPLLoader_UpdateDependencies();
	module->externalLinkError.clear();
	RPLLoader_LinkSingleModule(module, false);
	if (module->externalLinkError.empty())
		RPLLoader_LinkSingleModule(module, true);
	if (!module->externalLinkError.empty() || module->hasError)
	{
		error = module->externalLinkError.empty() ?
			"external RPL relocation failed" : module->externalLinkError;
		return false;
	}
	RPLLoader_LoadDebugSymbols(module);
	module->isLinked = true;
	GraphicPack2::NotifyModuleLoaded(module);
	g_debuggerDispatcher.NotifyModuleLoaded(module);
	RPLLoader_EmitModuleEvent(RPLModuleEventType::Linked, module);
	if (module->externalCallEntrypoint)
	{
		PPCCoreCallback(module->entrypoint, module->externalModuleHandle, 1);
		module->entrypointCalled = true;
	}
	return true;
}

uint32 RPLLoader_GetModuleEntrypoint(RPLModule* rplLoaderContext)
{
	return rplLoaderContext->entrypoint;
}

// takes a module name without extension, returns true if the RPL module is a known Cafe OS module
bool RPLLoader_IsKnownCafeOSModule(std::string_view name)
{
	static std::unordered_set<std::string> s_systemModules556 = {
			"avm","camera","cemuextend","coreinit","dc","dmae","drmapp","erreula",
			"gx2","h264","lzma920","mic","nfc","nio_prof","nlibcurl",
			"nlibnss","nlibnss2","nn_ac","nn_acp","nn_act","nn_aoc","nn_boss",
			"nn_ccr","nn_cmpt","nn_dlp","nn_ec","nn_fp","nn_hai","nn_hpad",
			"nn_idbe","nn_ndm","nn_nets2","nn_nfp","nn_nim","nn_olv","nn_pdm",
			"nn_save","nn_sl","nn_spm","nn_temp","nn_uds","nn_vctl","nsysccr",
			"nsyshid","nsyskbd","nsysnet","nsysuhs","nsysuvd","ntag","padscore",
			"proc_ui","sndcore2","snduser2","snd_core","snd_user","swkbd","sysapp",
			"tcl","tve","uac","uac_rpl","usb_mic","uvc","uvd","vpad","vpadbase",
			"zlib125"};
	std::string nameLower{name};
	std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), _ansiToLower);
	return s_systemModules556.contains(nameLower);
}

COSModule* RPLLoader_GetHLECafeOSModule(std::string_view moduleName)
{
	std::span<COSModule*> cosModules = GetCOSModules();
	for (auto& module : cosModules)
	{
		if (module->GetName() == moduleName)
			return module;
	}
	return nullptr;
}

bool RPLLoader_CanUseNativeSwkbd()
{
	bool hasSwkbdPack = fsc_doesFileExist("vol/storage_mlc01/sys/title/0005001b/1004f000/content/00/swkbd/swkbd.pack");
	return hasSwkbdPack;
}

bool RPLLoader_CanUseNativeErrEula()
{
	bool hasErrEulaPack = fsc_doesFileExist("vol/storage_mlc01/sys/title/0005001b/10051000/content/00/erreula/erreula.pack");
	return hasErrEulaPack;
}

// increment reference counter for module
void RPLLoader_AddDependency(std::string_view name, bool isMainExecutable)
{
	std::lock_guard lock(g_rplLoaderMutex);
	cemu_assert(!name.empty());
	std::string moduleName = _RPLLoader_ExtractModuleNameFromPath(name);
	// check if dependency already exists
	for (auto& dep : rplDependencyList)
	{
		if (moduleName == dep->moduleName)
		{
			dep->referenceCount++;
			return;
		}
	}
	// add new entry
	RPLDependency* newDependency = new RPLDependency();
	newDependency->isMainExecutable = isMainExecutable;
	newDependency->moduleName = moduleName;
	newDependency->referenceCount = 1;
	newDependency->coreinitHandle = rplLoader_currentHandleCounter;
	newDependency->tlsModuleIndex = rplLoader_currentTlsModuleIndex;
	newDependency->isCafeOSModule = RPLLoader_IsKnownCafeOSModule(moduleName);
	rplLoader_currentTlsModuleIndex++; // todo - delay handle and tls allocation until the module is actually loaded. It may not exist
	rplLoader_currentHandleCounter++;
	if (rplLoader_currentTlsModuleIndex == 0x7FFF)
		cemuLog_log(LogType::Force, "RPLLoader: Exhausted TLS module indices pool");
	if (moduleName.size() >= RPL_MODULE_PATH_LENGTH)
		cemuLog_log(LogType::Force, "RPLLoader_AddDependency(): RPL module name too long \"{}\"", moduleName);
	std::string fileName = moduleName;
	fileName.append(isMainExecutable ? ".rpx" : ".rpl");
	// load order:
	// 1) cafeLibs (Cemu specific)
	// 2) SLC /vol/system/title/00050010/1000400a/code (Cemu HLE modules)
	// 3) The game's code directory
	// note: Some games ship with copies of system RPLs which are never actually loaded since the SLC lookup takes precedence (Example games include MH3G which has erreula.rpl and swkbd.rpl, Disney Epic Mickey 2 which comes with erreula.rpl)
	const auto cafeLibsFilePath = ActiveSettings::GetUserDataPath("cafeLibs/{}", fileName);
	std::error_code ec;
	bool rplExistsInCafeLibs = fs::exists(cafeLibsFilePath, ec) && ActiveSettings::LoadSharedLibrariesEnabled(); // load from cafeLibs only if the option is enabled
	bool isBlacklisted = false;
	if (rplExistsInCafeLibs)
	{
		if (newDependency->moduleName == "swkbd" && !RPLLoader_CanUseNativeSwkbd())
			isBlacklisted = true;
		if (newDependency->moduleName == "erreula" && !RPLLoader_CanUseNativeErrEula())
			isBlacklisted = true;
	}
	if (isBlacklisted)
		cemuLog_log(LogType::Force, "Game tried to load {}.rpl from cafeLibs/ but the necessary MLC data files are not present. Using Cemu's implementation instead", moduleName);
	if (!rplExistsInCafeLibs || isBlacklisted)
		newDependency->rplHLEModule = RPLLoader_GetHLECafeOSModule(moduleName);
	rplDependencyList.push_back(newDependency);
}

// decrement reference counter for dependency by module path
void RPLLoader_RemoveDependency(std::string_view name)
{
	std::lock_guard lock(g_rplLoaderMutex);
	cemu_assert_debug(!name.empty());
	if (name.empty())
		return;
	std::string moduleName = _RPLLoader_ExtractModuleNameFromPath(name);
	// find dependency and decrement ref count
	for (auto& dep : rplDependencyList)
	{
		if (dep->moduleName == moduleName)
		{
			dep->referenceCount--;
			return;
		}
	}
}

bool RPLLoader_HasDependency(std::string_view name)
{
	std::lock_guard lock(g_rplLoaderMutex);
	if (name.empty())
		return false;
	std::string moduleName = _RPLLoader_ExtractModuleNameFromPath(name);
	for (const auto& dep : rplDependencyList)
	{
		if (dep->moduleName == moduleName)
			return true;
	}
	return false;
}

// decrement reference counter for dependency by module handle
void RPLLoader_RemoveDependency(uint32 handle)
{
	std::lock_guard lock(g_rplLoaderMutex);
	for (auto& dep : rplDependencyList)
	{
		if (dep->coreinitHandle == handle)
		{
			cemu_assert_debug(dep->referenceCount != 0);
			if(dep->referenceCount > 0)
				dep->referenceCount--;
			return;
		}
	}
}

RPLDependency* RPLLoader_GetDependencyByRPLModule(RPLModule* rpl)
{
	cemu_assert_debug(rpl);
	for (auto& dep : rplDependencyList)
	{
		if (dep->rplLoaderContext == rpl)
			return dep;
	}
	cemu_assert_suspicious(); // should never happen. Modules get loaded via dependency tracking so a dependency entry needs to exist
	return nullptr;
}

uint32 RPLLoader_GetHandleByModuleName(const char* name)
{
	std::lock_guard lock(g_rplLoaderMutex);
	std::string moduleName = _RPLLoader_ExtractModuleNameFromPath(name);
	for (sint32 index = 0; index < rplModuleCount; ++index)
		if (rplModuleList[index]->externalModule &&
			rplModuleList[index]->externalRegisterDependency &&
			rplModuleList[index]->moduleName == moduleName)
			return rplModuleList[index]->externalModuleHandle;
	// search for existing dependency
	for (auto& dep : rplDependencyList)
	{
		if (dep->moduleName == moduleName)
		{
			cemu_assert_debug(dep->loadAttempted);
			if (!dep->isCafeOSModule && !dep->rplLoaderContext)
				return RPL_INVALID_HANDLE; // module not found
			return dep->coreinitHandle;
		}
	}
	return RPL_INVALID_HANDLE;
}

uint32 RPLLoader_GetMaxTLSModuleIndex()
{
	std::lock_guard lock(g_rplLoaderMutex);
	return rplLoader_currentTlsModuleIndex - 1;
}

bool RPLLoader_GetTLSDataByTLSIndex(sint16 tlsModuleIndex, uint8** tlsData, sint32* tlsSize)
{
	std::lock_guard lock(g_rplLoaderMutex);
	if (!tlsData || !tlsSize)
		return false;
	*tlsData = nullptr;
	*tlsSize = 0;
	RPLModule* rplLoaderContext = nullptr;
	for (auto& dep : rplDependencyList)
	{
		if (dep->tlsModuleIndex == tlsModuleIndex)
		{
			rplLoaderContext = dep->rplLoaderContext;
			break;
		}
	}
	if (!rplLoaderContext)
		for (sint32 index = 0; index < rplModuleCount; ++index)
			if (rplModuleList[index]->externalModule &&
				rplModuleList[index]->fileInfo.tlsModuleIndex == tlsModuleIndex)
			{
				rplLoaderContext = rplModuleList[index];
				break;
			}
	if (rplLoaderContext == nullptr)
		return false;
	// The module's TLS template spans its .tdata/.tbss sections. Their combined extent
	// (by virtual address) is already resolved when the module is loaded, so trust the
	// cached tlsStartAddress/tlsEndAddress here. The previous WUPS rewrite re-derived it
	// from the section table with a strict mapped-address contiguity requirement plus a
	// hard 64KB size limit; that rejected legitimate TLS layouts (e.g. Minecraft: Wii U
	// Edition, whose .tdata/.tbss are not mapped contiguously) and returned false, which
	// then tripped the fatal cemu_assert in coreinit::__tls_get_addr (SIGTRAP on boot).
	if (rplLoaderContext->tlsEndAddress <= rplLoaderContext->tlsStartAddress)
		return false; // module has no TLS data
	const uint32 tlsDataSize = rplLoaderContext->tlsEndAddress - rplLoaderContext->tlsStartAddress;
	cemu_assert_debug(tlsDataSize < 0x10000); // suspiciously large TLS area (non-fatal, informational)
	if (tlsDataSize > static_cast<uint32>(std::numeric_limits<sint32>::max()))
		return false;
	g_rplTlsTemplateScratch.resize(tlsDataSize);
	std::memcpy(g_rplTlsTemplateScratch.data(),
		memory_getPointerFromVirtualOffset(rplLoaderContext->tlsStartAddress), tlsDataSize);
	*tlsData = g_rplTlsTemplateScratch.data();
	*tlsSize = static_cast<sint32>(tlsDataSize);
	return true;
}

bool RPLLoader_LoadFromVirtualPath(RPLDependency* dependency, std::string_view filePath)
{
	uint32 rplSize = 0;
	uint8* rplData = fsc_extractFile(std::string(filePath).c_str(), &rplSize);
	if (rplData)
	{
		cemuLog_logDebug(LogType::Force, "Loading: {}", filePath);
		dependency->rplLoaderContext = RPLLoader_LoadFromMemory(rplData, rplSize, filePath);
		free(rplData);
		return true;
	}
	return false;
}

std::span<COSModule*> GetCOSModules();

void RPLLoader_LoadDependency(RPLDependency* dependency)
{
	// if its a HLE module then notify that it has been mapped
	if (dependency->rplHLEModule)
	{
		dependency->rplHLEModule->RPLMapped();
		// load chained dependencies
		// this is necessary for something like HLE GX2.rpl which uses TCL.rpl functions
		auto depList = dependency->rplHLEModule->GetDependencies();
		for (const auto& dep : depList)
			RPLLoader_AddDependency(dep);
		return;
	}
	// check if module is already loaded
	for (sint32 i = 0; i < rplModuleCount; i++)
	{
		if (rplModuleList[i]->externalModule &&
			!rplModuleList[i]->externalRegisterDependency)
			continue;
		if (rplModuleList[i]->moduleName != dependency->moduleName)
			continue;
		dependency->rplLoaderContext = rplModuleList[i];
		return;
	}
	// attempt to load RPLs from Cemu's /cafeLibs/ directory first
	std::string rplFilename = dependency->moduleName;
	rplFilename.append(dependency->isMainExecutable ? ".rpx" : ".rpl");
	if (ActiveSettings::LoadSharedLibrariesEnabled() && !dependency->isMainExecutable)
	{
		const auto cafeLibsFilePath = ActiveSettings::GetUserDataPath("cafeLibs/{}", rplFilename);
		auto fileData = FileStream::LoadIntoMemory(cafeLibsFilePath);
		if (fileData)
		{
			cemuLog_log(LogType::Force, "Loading RPL: /cafeLibs/{}", rplFilename);
			dependency->rplLoaderContext = RPLLoader_LoadFromMemory(fileData->data(), fileData->size(), rplFilename);
			return;
		}
	}
	// attempt to load rpl from code directory of current title
	std::string rplPath = "/internal/current_title/code/";
	rplPath.append(rplFilename);
	if (RPLLoader_LoadFromVirtualPath(dependency, rplPath))
		return;
	cemuLog_logDebug(LogType::Force, "Failed to load dependency {}", rplFilename);
}

// loads and unloads modules based on the current dependency list
void RPLLoader_UpdateDependencies()
{
	std::lock_guard lock(g_rplLoaderMutex);
	bool repeat = true;
	while (repeat)
	{
		repeat = false;
		for(auto idx = 0; idx<rplDependencyList.size(); )
		{
			auto dependency = rplDependencyList[idx];
			// debug_printf("DEP 0x%02x %s\n", dependency->referenceCount, dependency->modulename);
			if(dependency->referenceCount == 0)
			{
				// unload RPLs
				// todo - should we let HLE modules know if they are being unloaded?
				if (dependency->rplLoaderContext)
				{
					RPLLoader_UnloadModule(dependency, false);
					dependency->rplLoaderContext = nullptr;
				}
				else if (dependency->rplHLEModule)
				{
					dependency->rplHLEModule->rpl_entry(dependency->coreinitHandle, coreinit::RplEntryReason::Unloaded);
					dependency->rplHLEModule->RPLUnmapped();
					// untrack chained dependencies
					auto depList = dependency->rplHLEModule->GetDependencies();
					for (const auto& dep : depList)
						RPLLoader_RemoveDependency(dep);
				}
				// remove from dependency list
				rplDependencyList.erase(rplDependencyList.begin()+idx);
				idx--;
				repeat = true; // unload can effect reference count of other dependencies
				break;
			}
			else if (!dependency->loadAttempted)
			{
				// load
				dependency->loadAttempted = true;
				RPLLoader_LoadDependency(dependency);
				repeat = true;
				idx++;
				break;
			}
			idx++;
		}
	}
	RPLLoader_Link();
}

void RPLLoader_LoadCoreinit()
{
	std::lock_guard lock(g_rplLoaderMutex);
	RPLLoader_AddDependency("coreinit");
	for (auto& dep : rplDependencyList)
	{
		if (dep->moduleName == "coreinit")
		{
			dep->loadAttempted = true;
			RPLLoader_LoadDependency(dep);
			return;
		}
	}
	cemu_assert_suspicious();
}

void RPLLoader_SetMainModule(RPLModule* rplLoaderContext)
{
	std::lock_guard lock(g_rplLoaderMutex);
	rplLoaderContext->entrypointCalled = true;
	rplLoader_mainModule = rplLoaderContext;
}

uint32 RPLLoader_GetMainModuleHandle()
{
	std::lock_guard lock(g_rplLoaderMutex);
	for (auto& dep : rplDependencyList)
	{
		if (dep->rplLoaderContext == rplLoader_mainModule)
		{
			return dep->coreinitHandle;
		}
	}
	cemu_assert(false);
	return 0;
}

RPLModule* RPLLoader_FindModuleByCodeAddr(uint32 addr)
{
	std::lock_guard lock(g_rplLoaderMutex);
	for (sint32 i = 0; i < rplModuleCount; i++)
	{
		uint32 startAddr = rplModuleList[i]->regionMappingBase_text.GetMPTR();
		uint32 endAddr = rplModuleList[i]->regionMappingBase_text.GetMPTR() + rplModuleList[i]->regionSize_text;
		if (addr >= startAddr && addr < endAddr)
			return rplModuleList[i];
	}
	return nullptr;
}

RPLModule* RPLLoader_FindModuleByDataAddr(uint32 addr)
{
	std::lock_guard lock(g_rplLoaderMutex);
	for (sint32 i = 0; i < rplModuleCount; i++)
	{
		// data
		uint32 startAddr = rplModuleList[i]->regionMappingBase_data;
		uint32 endAddr = rplModuleList[i]->regionMappingBase_data + rplModuleList[i]->regionSize_data;
		if (addr >= startAddr && addr < endAddr)
			return rplModuleList[i];
		// loaderinfo
		startAddr = rplModuleList[i]->regionMappingBase_loaderInfo;
		endAddr = rplModuleList[i]->regionMappingBase_loaderInfo + rplModuleList[i]->regionSize_loaderInfo;
		if (addr >= startAddr && addr < endAddr)
			return rplModuleList[i];
	}
	return nullptr;
}

RPLModule* RPLLoader_FindModuleByName(std::string module)
{
	std::lock_guard lock(g_rplLoaderMutex);
	for (sint32 i = 0; i < rplModuleCount; i++)
	{
		if (rplModuleList[i]->moduleName == module) return rplModuleList[i];
	}
	return nullptr;
}

void RPLLoader_CallEntrypoints()
{
	std::lock_guard lock(g_rplLoaderMutex);
	// for HLE modules we need to check the dependency list
	for (auto& dependency : rplDependencyList)
	{
		if (!dependency->rplHLEModule)
			continue;
		if (dependency->hleEntrypointCalled)
			continue;
		dependency->rplHLEModule->rpl_entry(dependency->coreinitHandle, coreinit::RplEntryReason::Loaded);
		dependency->hleEntrypointCalled = true;
	}
	// iterate loaded RPL modules
	for (sint32 i = 0; i < rplModuleCount; i++)
	{
		if (rplModuleList[i]->externalModule)
			continue;
		if (rplModuleList[i]->entrypointCalled)
			continue;
		uint32 moduleHandle = RPLLoader_GetHandleByModuleName(rplModuleList[i]->moduleName.c_str());
		MPTR entryPoint = RPLLoader_GetModuleEntrypoint(rplModuleList[i]);
		PPCCoreCallback(entryPoint, moduleHandle, 1); // 1 -> load, 2 -> unload
		rplModuleList[i]->entrypointCalled = true;
	}
}

// calls the entrypoint of coreinit and marks it as called so that RPLLoader_CallEntrypoints() wont call it again later
void RPLLoader_CallCoreinitEntrypoint()
{
	std::lock_guard lock(g_rplLoaderMutex);
	// for HLE modules we need to check the dependency list
	for (auto& dependency : rplDependencyList)
	{
		if (dependency->moduleName != "coreinit")
			continue;
		if (!dependency->rplHLEModule)
			continue;
		if (dependency->hleEntrypointCalled)
			continue;
		dependency->rplHLEModule->rpl_entry(dependency->coreinitHandle, coreinit::RplEntryReason::Loaded);
		dependency->hleEntrypointCalled = true;
		return;
	}
	cemu_assert_unimplemented(); // coreinit.rpl present in cafelibs? We currently do not support native coreinit and no thread context exists yet to do a PPC call
}

void RPLLoader_NotifyControlPassedToApplication()
{
	std::lock_guard lock(g_rplLoaderMutex);
	rplLoader_applicationHasMemoryControl = true;
}

uint32 RPLLoader_FindModuleOrHLEExport(uint32 moduleHandle, bool isData, const char* exportName)
{
	std::lock_guard lock(g_rplLoaderMutex);
	// find dependency from handle
	RPLModule* rplLoaderContext = nullptr;
	RPLDependency* dependency = nullptr;
	for (auto& dep : rplDependencyList)
	{
		if (dep->coreinitHandle == moduleHandle)
		{
			rplLoaderContext = dep->rplLoaderContext;
			dependency = dep;
			break;
		}
	}

	uint32 exportResult = 0;
	if (rplLoaderContext)
	{
		exportResult = RPLLoader_FindModuleExport(rplLoaderContext, isData, exportName);
	}
	else
	{
		// attempt to find HLE export
		if (isData)
		{
			MPTR weakExportAddr = osLib_getPointer(dependency->moduleName.c_str(), exportName);
			cemu_assert_debug(weakExportAddr != 0xFFFFFFFF);
			exportResult = weakExportAddr;
		}
		else
		{
			exportResult = rpl_mapHLEImport(rplLoaderContext, dependency->moduleName.c_str(), exportName, true);
		}
	}

	return exportResult;
}

uint32 RPLLoader_GetSDA1Base()
{
	cemu_assert_debug(rplModuleCount > 0); // this should not be called before the main executable was loaded
	return rplLoader_sdataAddr;
}

uint32 RPLLoader_GetSDA2Base()
{
	cemu_assert_debug(rplModuleCount > 0);
	return rplLoader_sdata2Addr;
}

RPLModule** RPLLoader_GetModuleList()
{
	return rplModuleList;
}

sint32 RPLLoader_GetModuleCount()
{
	std::lock_guard lock(g_rplLoaderMutex);
	return rplModuleCount;
}

bool RPLLoader_IsModuleAlive(const RPLModule* module, uint64 lifetimeId)
{
	std::lock_guard lock(g_rplLoaderMutex);
	return RPLLoader_FindLiveModule(module, lifetimeId) != nullptr;
}

bool RPLLoader_AcquireExternalModuleLease(RPLModule* module, uint64 lifetimeId,
	RPLModuleLease& lease, std::string& error)
{
	error.clear();
	lease.m_impl.reset();
	std::lock_guard lock(g_rplLoaderMutex);
	module = RPLLoader_FindLiveModule(module, lifetimeId);
	if (!module || !module->externalModule || module->externalUnloading)
	{
		error = "external RPL access rejected a stale or unloading module lifetime";
		return false;
	}
	lease.m_impl = std::make_unique<RPLModuleLease::Impl>(module);
	return true;
}

bool RPLLoader_ResolveModuleAddress(const RPLModuleLease& lease, uint32 virtualAddress,
	uint32 size, RPLModuleAddressKind kind, MPTR& mappedAddress)
{
	mappedAddress = MPTR_NULL;
	if (!lease.m_impl || size == 0)
		return false;
	const RPLModule* module = lease.m_impl->module;
	for (uint32 index = 0; index < (uint32)module->rplHeader.sectionTableEntryCount; ++index)
	{
		const auto& section = module->sectionTablePtr[index];
		const uint32 flags = section.flags;
		if ((flags & 2) == 0 || !module->sectionAddressTable2[index].ptr)
			continue;
		if (kind == RPLModuleAddressKind::Writable && (flags & 1) == 0)
			continue;
		if (kind == RPLModuleAddressKind::Executable && (flags & 4) == 0)
			continue;
		const uint32 sectionAddress = section.virtualAddress;
		const uint32 sectionSize = section.sectionSize;
		if (virtualAddress < sectionAddress)
			continue;
		const uint32 offset = virtualAddress - sectionAddress;
		if (offset > sectionSize || size > sectionSize - offset)
			continue;
		const MPTR result = memory_getVirtualOffsetFromPointer(
			module->sectionAddressTable2[index].ptr) + offset;
		if (!memory_isAddressRangeAccessible(result, size))
			return false;
		mappedAddress = result;
		return true;
	}
	return false;
}

bool RPLLoader_QueryMappedLayout(const RPLModuleLease& lease,
	RPLMappedLayoutSnapshot& layout)
{
	layout = {};
	if (!lease.m_impl)
		return false;
	const auto* module = lease.m_impl->module;
	if (!module || !module->sectionTablePtr)
		return false;
	layout.textBase = module->regionMappingBase_text.GetMPTR();
	layout.dataBase = module->regionMappingBase_data;
	const uint32 sectionCount = module->rplHeader.sectionTableEntryCount;
	const uint32 namesIndex = module->rplHeader.nameSectionIndex;
	const char* names{};
	uint32 namesSize{};
	if (namesIndex < sectionCount &&
		module->sectionAddressTable2[namesIndex].ptr)
	{
		names = static_cast<const char*>(
			module->sectionAddressTable2[namesIndex].ptr);
		namesSize = module->sectionTablePtr[namesIndex].sectionSize;
	}
	for (uint32 index = 0; index < sectionCount; ++index)
	{
		const auto& section = module->sectionTablePtr[index];
		const uint32 flags = section.flags;
		const uint32 size = section.sectionSize;
		auto* pointer = module->sectionAddressTable2[index].ptr;
		if ((flags & 2U) == 0 || size == 0 || !pointer)
			continue;
		std::string name;
		const uint32 nameOffset = section.nameOffset;
		if (names && nameOffset < namesSize)
		{
			const auto maximum = namesSize - nameOffset;
			const auto length = strnlen(names + nameOffset, maximum);
			if (length < maximum)
				name.assign(names + nameOffset, length);
		}
		layout.sections.push_back({std::move(name),
			memory_getVirtualOffsetFromPointer(pointer), size, flags});
	}
	return true;
}

bool RPLLoader_QueryMappedAddress(uint32 address, uint32 size,
	RPLMappedAddressInfo& info)
{
	std::lock_guard lock(g_rplLoaderMutex);
	info = {};
	if (size == 0 || address > std::numeric_limits<uint32>::max() - size)
		return false;
	for (sint32 moduleIndex = 0; moduleIndex < rplModuleCount; ++moduleIndex)
	{
		const auto* module = rplModuleList[moduleIndex];
		for (uint32 sectionIndex = 0;
			sectionIndex < static_cast<uint32>(
				module->rplHeader.sectionTableEntryCount);
			++sectionIndex)
		{
			const auto& section = module->sectionTablePtr[sectionIndex];
			const uint32 flags = section.flags;
			const uint32 sectionSize = section.sectionSize;
			auto* sectionPointer = module->sectionAddressTable2[sectionIndex].ptr;
			if ((flags & 2U) == 0 || sectionSize == 0 || !sectionPointer)
				continue;
			const MPTR mapped = memory_getVirtualOffsetFromPointer(sectionPointer);
			if (address < mapped || address - mapped > sectionSize ||
				size > sectionSize - (address - mapped))
				continue;
			info = {
				module->moduleName,
				module->externalLifetimeId,
				module->externalOwner,
				module->externalGeneration,
				flags,
				module->externalModule,
			};
			return true;
		}
	}
	return false;
}

bool RPLLoader_FindLoadedExport(std::string_view moduleName,
	std::string_view symbolName, bool isData, RPLResolvedExport& result)
{
	std::lock_guard lock(g_rplLoaderMutex);
	result = {};
	if (moduleName.empty() || symbolName.empty() ||
		moduleName.size() >= RPL_MODULE_PATH_LENGTH || symbolName.size() > 1024)
		return false;
	const auto slash = moduleName.find_last_of('/');
	const auto basenameStart = slash == std::string_view::npos ? 0 : slash + 1;
	const auto basenameEnd = moduleName.find('.', basenameStart);
	if (basenameStart == moduleName.size() || basenameEnd == basenameStart)
		return false;
	const auto canonicalName = _RPLLoader_ExtractModuleNameFromPath(moduleName);
	for (sint32 moduleIndex = 0; moduleIndex < rplModuleCount; ++moduleIndex)
	{
		auto* module = rplModuleList[moduleIndex];
		// External WUPS/WUMS providers are resolved through the generation-
		// pinned ModuleExportRegistry and must not shadow Cafe RPL exports.
		if (module->externalModule || module->moduleName != canonicalName)
			continue;
		const std::string symbol(symbolName);
		const auto address =
			RPLLoader_FindRPLExport(module, symbol.c_str(), isData);
		if (address == MPTR_NULL)
			return false;
		result = {
			address, module->moduleName,
			module->externalLifetimeId, false};
		return true;
	}
	if (isData)
	{
		const auto address = osLib_getPointer(
			canonicalName.c_str(), std::string(symbolName).c_str());
		if (address == 0xFFFFFFFF || address == MPTR_NULL)
			return false;
		result = {address, canonicalName, 0, true};
		return true;
	}
	const std::string symbol(symbolName);
	const auto address = rpl_mapHLEImport(
		nullptr, canonicalName.c_str(), symbol.c_str(), false);
	if (address == MPTR_NULL)
		return false;
	result = {address, canonicalName, 0, true};
	return true;
}

uint32 RPLLoader_GetModuleSDA1Base(const RPLModuleLease& lease)
{
	return lease.m_impl ? lease.m_impl->module->mappedSda1Base : MPTR_NULL;
}

uint32 RPLLoader_GetModuleSDA2Base(const RPLModuleLease& lease)
{
	return lease.m_impl ? lease.m_impl->module->mappedSda2Base : MPTR_NULL;
}

uint64 RPLLoader_AddModuleEventObserver(RPLModuleEventCallback callback)
{
	if (!callback)
		return 0;
	std::lock_guard lock(g_rplModuleEventMutex);
	const uint64 id = g_rplModuleEventObserverCounter++;
	g_rplModuleEventObservers.emplace(id, std::move(callback));
	return id;
}

bool RPLLoader_RemoveModuleEventObserver(uint64 observerId)
{
	std::lock_guard lock(g_rplModuleEventMutex);
	return g_rplModuleEventObservers.erase(observerId) != 0;
}

bool RPLLoader_UnloadExternalModule(RPLModule* module, uint64 lifetimeId,
	std::string& error)
{
	std::lock_guard lock(g_rplLoaderMutex);
	error.clear();
	module = RPLLoader_FindLiveModule(module, lifetimeId);
	if (!module || !module->externalModule || module->externalUnloading)
	{
		error = "external RPL unload rejected a stale or unloading module lifetime";
		return false;
	}
	if (module->externalEventInFlight)
	{
		error = "external RPL unload is not allowed from a module event callback";
		return false;
	}
	if (module->externalAccessCount != 0)
	{
		error = "external RPL unload is not allowed while module access is leased";
		return false;
	}
	if ((module->funcFree || (module->externalCallEntrypoint && module->entrypointCalled)) &&
		!PPCInterpreter_getCurrentInstance())
	{
		error = "external RPL unload requires an emulated CPU thread";
		return false;
	}
	const bool updateDependencies = module->externalRegisterDependency;
	RPLLoader_DestroyModule(module, nullptr, false, true);
	if (updateDependencies)
		RPLLoader_UpdateDependencies();
	return true;
}

template<typename TAddr, typename TSize>
class SimpleHeap
{
	struct allocEntry_t
	{
		TAddr start;
		TAddr end;
		allocEntry_t(TAddr start, TAddr end) : start(start), end(end) {};
	};

public:
	SimpleHeap(TAddr baseAddress, TSize size) : m_base(baseAddress), m_size(size)
	{

	}

	TAddr alloc(TSize size, TSize alignment)
	{
		cemu_assert_debug(alignment != 0);
		TAddr allocBase = m_base;
		allocBase = (allocBase + alignment - 1)&~(alignment-1);
		while (true)
		{
			bool hasCollision = false;
			for (auto& itr : list_allocatedEntries)
			{
				if (allocBase < itr.end && (allocBase + size) >= itr.start)
				{
					allocBase = itr.end;
					allocBase = (allocBase + alignment - 1)&~(alignment - 1);
					hasCollision = true;
					break;
				}
			}
			if(hasCollision == false)
				break;
		}
		if ((allocBase + size) > (m_base + m_size))
			return 0;
		list_allocatedEntries.emplace_back(allocBase, allocBase + size);
		return allocBase;
	}

	void free(TAddr addr)
	{
		for (sint32 i = 0; i < list_allocatedEntries.size(); i++)
		{
			if (list_allocatedEntries[i].start == addr)
			{
				list_allocatedEntries.erase(list_allocatedEntries.begin() + i);
				return;
			}
		}
		cemu_assert(false);
		return;
	}

private:
	TAddr  m_base;
	TSize  m_size;
	std::vector<allocEntry_t> list_allocatedEntries;
};

SimpleHeap<uint32, uint32> heapCodeCaveArea(MEMORY_CODECAVEAREA_ADDR, MEMORY_CODECAVEAREA_SIZE);

MEMPTR<void> RPLLoader_AllocateCodeCaveMem(uint32 alignment, uint32 size)
{
	uint32 addr = heapCodeCaveArea.alloc(size, 256);
	return MEMPTR<void>{addr};
}

void RPLLoader_ReleaseCodeCaveMem(MEMPTR<void> addr)
{
	heapCodeCaveArea.free(addr.GetMPTR());
}

void RPLLoader_UnloadAll()
{
	std::lock_guard lock(g_rplLoaderMutex);
	for (sint32 index = 0; index < rplModuleCount; ++index)
		if (rplModuleList[index]->externalAccessCount != 0 ||
			rplModuleList[index]->externalEventInFlight)
		{
			cemuLog_log(LogType::Force,
				"RPLLoader: unload-all deferred because external module '{}' is active",
				rplModuleList[index]->moduleName);
			return;
		}
	// unload all RPL modules
	while (rplModuleCount > 0)
	{
		if (rplModuleList[0]->externalModule)
		{
			RPLLoader_DestroyModule(rplModuleList[0], nullptr, true, false);
			continue;
		}
		RPLDependency* dep = RPLLoader_GetDependencyByRPLModule(rplModuleList[0]);
		RPLLoader_UnloadModule(dep, true);
	}
	// notify every remaining HLE module its unloaded and unmapped
	// and do it in reverse order so that coreinit comes last
	RPLLoader_RemoveDependency("coreinit"); // undo manual ref count from RPLLoader_LoadCoreinit()
	for (sint32 i = (sint32)rplDependencyList.size()-1; i>=0; i--)
	{
		RPLDependency* dependency = rplDependencyList[i];
		cemu_assert_debug(dependency->referenceCount >= 0); // sanity check for ref count
		if (!dependency->rplHLEModule)
			continue;
		cemu_assert_debug(dependency->hleEntrypointCalled); // entrypoint should have been called
		dependency->rplHLEModule->rpl_entry(dependency->coreinitHandle, coreinit::RplEntryReason::Unloaded);
		dependency->rplHLEModule->RPLUnmapped();
	}
	rplDependencyList.clear();
	// unload all remaining symbols
	rplSymbolStorage_unloadAll();
	// free all code imports
	g_heapTrampolineArea.releaseAll();
	list_mappedFunctionImports.clear();
	g_map_callableExports.clear();
	rplLoader_applicationHasMemoryControl = false;
	rplLoader_maxCodeAddress = 0;
	rplLoader_currentDataAllocatorAddr = 0x10000000;
	rplLoader_currentTLSModuleIndex = 1;
	rplLoader_currentTlsModuleIndex = 1;
	rplLoader_sdataAddr = MPTR_NULL;
	rplLoader_sdata2Addr = MPTR_NULL;
	rplLoader_mainModule = nullptr;
}
