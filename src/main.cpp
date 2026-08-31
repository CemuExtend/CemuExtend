#include "frontend/FrontendRuntime.h"
#include "application/ApplicationRuntime.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#if defined(CEMU_OVERLAY_BACKEND_CEF)
#include "webview/cef/CefOverlayRuntime.h"
#endif

#include "Cafe/CafeSystem.h"

#include "Common/cpu_features.h"

#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"

#include <cstdio>
#include <cstdlib>

#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cafe/OS/libs/vpad/vpad.h"

#if BOOST_OS_WINDOWS
#include <objbase.h>
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

#if defined(CEMU_OVERLAY_BACKEND_CEF)
namespace
{
	int ExecuteWindowsCefSubprocess()
	{
		int argc{};
		LPWSTR* wideArguments = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!wideArguments || argc <= 0)
			return -1;
		std::vector<std::string> arguments;
		std::vector<char*> pointers;
		arguments.reserve(static_cast<std::size_t>(argc));
		pointers.reserve(static_cast<std::size_t>(argc));
		for (int index = 0; index < argc; ++index)
		{
			const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
												   wideArguments[index], -1, nullptr, 0, nullptr, nullptr);
			if (length <= 0)
				arguments.emplace_back();
			else
			{
				std::string value(static_cast<std::size_t>(length), '\0');
				WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[index], -1,
									value.data(), length, nullptr, nullptr);
				value.pop_back();
				arguments.emplace_back(std::move(value));
			}
		}
		LocalFree(wideArguments);
		for (auto& argument : arguments)
			pointers.push_back(argument.data());
		return WebFrontend::CefOverlay::ExecuteSubprocess(argc, pointers.data());
	}
} // namespace
#endif
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
#if defined(CEMU_OVERLAY_BACKEND_CEF)
	if (const int cefProcessCode = ExecuteWindowsCefSubprocess(); cefProcessCode >= 0)
		return cefProcessCode;
#endif
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
#if defined(CEMU_OVERLAY_BACKEND_CEF)
	if (const int cefProcessCode = WebFrontend::CefOverlay::ExecuteSubprocess(argc, argv);
		cefProcessCode >= 0)
		return cefProcessCode;
#endif
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
#if defined(CEMU_OVERLAY_BACKEND_CEF)
	const int cefProcessCode = WebFrontend::CefOverlay::ExecuteSubprocess(argc, argv);
	if (cefProcessCode >= 0)
		return cefProcessCode;
#endif
#if BOOST_OS_LINUX && defined(ENABLE_VULKAN)
	if (getenv("CEMU_DETECT_RADV") != nullptr)
		return BreathOfTheWildChildProcessMain();
#endif

#if BOOST_OS_LINUX || BOOST_OS_BSD
	if (XInitThreads() == 0)
	{
		std::fputs("Cemu: XInitThreads() failed\n", stderr);
		return EXIT_FAILURE;
	}
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
