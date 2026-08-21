#include "frontend/FrontendRuntime.h"
#include "application/ApplicationRuntime.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"

#include "Cafe/CafeSystem.h"

#include "Common/cpu_features.h"

#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"

#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cafe/OS/libs/vpad/vpad.h"

#if BOOST_OS_WINDOWS
#pragma comment(lib, "Dbghelp.lib")
#endif

#ifdef HAS_SDL
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

#if BOOST_OS_WINDOWS
extern "C"
{
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}
#endif

void mainEmulatorLLE();
void ppcAsmTest();
void gx2CopySurfaceTest();
void ExpressionParser_test();
void FSTVolumeTest();
void CRCTest();

void UnitTests()
{
	ExpressionParser_test();
	gx2CopySurfaceTest();
	ppcAsmTest();
	FSTVolumeTest();
	CRCTest();
}

void ToolShaderCacheMerger();

#if BOOST_OS_WINDOWS

// entrypoint for release builds
int wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)))
		cemuLog_log(LogType::Force, "CoInitializeEx() failed");
#ifdef HAS_SDL
	SDL_SetMainReady();
#endif
	if (!LaunchSettings::HandleCommandline(lpCmdLine))
		return 0;
	Frontend::Run();
	return 0;
}

// entrypoint for debug builds with console
int main(int argc, char* argv[])
{
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)))
		cemuLog_log(LogType::Force, "CoInitializeEx() failed");
#ifdef HAS_SDL
	SDL_SetMainReady();
#endif
	if (!LaunchSettings::HandleCommandline(argc, argv))
		return 0;
	Frontend::Run();
	return 0;
}

#else

int BreathOfTheWildChildProcessMain();
int main(int argc, char* argv[])
{
#if BOOST_OS_LINUX && defined(ENABLE_VULKAN)
	if (getenv("CEMU_DETECT_RADV") != nullptr)
		return BreathOfTheWildChildProcessMain();
#endif

#if BOOST_OS_LINUX || BOOST_OS_BSD
	XInitThreads();
#endif
	if (!LaunchSettings::HandleCommandline(argc, argv))
		return 0;
	Frontend::Run();
	return 0;
}
#endif

extern "C" DLLEXPORT uint64 gameMeta_getTitleId()
{
	return CafeSystem::GetForegroundTitleId();
}
