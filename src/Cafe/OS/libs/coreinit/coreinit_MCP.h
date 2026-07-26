#pragma once

#pragma pack(push, 1)
struct SysProdSettings
{
	uint32be platformRegion;		// 0x00 (product_area)
	uint16be eepromVersion;		// 0x04
	uint8 padding06[2];
	uint32be gameRegion;		// 0x08
	uint8 unknown0C[4];
	char ntscPal[5];			// 0x10
	char wifi5GhzCountryCode[4];	// 0x15
	uint8 wifi5GhzCountryCodeRevision;
	char codeId[8];				// 0x1A
	char serialId[12];			// 0x22
	uint8 unknown2E[4];
	char modelNumber[16];		// 0x32
	uint32be version;			// 0x42
};
#pragma pack(pop)

static_assert(sizeof(SysProdSettings) == 0x46);
static_assert(offsetof(SysProdSettings, codeId) == 0x1A);
static_assert(offsetof(SysProdSettings, serialId) == 0x22);

typedef uint32 MCPHANDLE;

MCPHANDLE MCP_Open();
void MCP_Close(MCPHANDLE mcpHandle);
sint32 MCP_GetSysProdSettings(MCPHANDLE mcpHandle, SysProdSettings* sysProdSettings);

void coreinitExport_UCReadSysConfig(PPCInterpreter_t* hCPU);

namespace coreinit
{
	void InitializeMCP();
}
