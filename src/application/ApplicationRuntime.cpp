#include "application/ApplicationRuntime.h"

#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/TitleList/SaveList.h"
#include "Cafe/TitleList/TitleList.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "config/NetworkSettings.h"
#include "input/InputManager.h"
#include "util/crypto/aes128.h"

#if BOOST_OS_LINUX
#define _putenv(__s) putenv((char*)(__s))
#elif BOOST_OS_MACOS || BOOST_OS_BSD
#define _putenv(__s) putenv((char*)(__s))
#endif

namespace
{
	std::vector<std::string*> s_putEnvStrings;

	// Some implementations of _putenv keep the supplied pointer instead of
	// copying the string. Retain the backing storage for the process lifetime.
	void PutEnvSafe(const char* value)
	{
		auto* string = new std::string(value);
		s_putEnvStrings.emplace_back(string);
		_putenv(string->c_str());
	}

	void ReconfigureGLDrivers()
	{
#ifdef ENABLE_OPENGL
		const fs::path nvCacheDir = ActiveSettings::GetCachePath("shaderCache/driver/nvidia/");
		std::error_code error;
		fs::create_directories(nvCacheDir, error);

		std::string cachePathOption("__GL_SHADER_DISK_CACHE_PATH=");
		cachePathOption.append(_pathToUtf8(nvCacheDir));
#if BOOST_OS_WINDOWS
		const std::wstring wideOption = boost::nowide::widen(cachePathOption);
		_wputenv(wideOption.c_str());
#else
		PutEnvSafe(cachePathOption.c_str());
#endif
		PutEnvSafe("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1");
#endif
	}

	void ReconfigureVkDrivers()
	{
#ifdef ENABLE_VULKAN
		PutEnvSafe("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1");
		PutEnvSafe("DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1");
#endif
	}

	void InitializeWorkingDirectory()
	{
#if BOOST_OS_WINDOWS
		std::wstring executablePath(4096, L'\0');
		const int length = GetModuleFileNameW(nullptr, executablePath.data(), executablePath.size());
		if (length >= 0)
			executablePath.resize(length);
		else
			executablePath.clear();
		SetCurrentDirectoryW(executablePath.c_str());
		SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#endif
	}
}

void CemuCommonInit()
{
	static std::once_flag initOnce;
	std::call_once(initOnce, [] {
		ReconfigureGLDrivers();
		ReconfigureVkDrivers();
		AES128_init();
		PPCTimer_init();
		InitializeWorkingDirectory();

		cemuLog_configureRuntime(ActiveSettings::GetUserDataPath("log.txt"),
			GetConfig().advanced_ppc_logging.GetValue(), LaunchSettings::Verbose());
		ExceptionHandler_Init();
		GetConfigHandle().Load();
		cemuLog_configureRuntime(ActiveSettings::GetUserDataPath("log.txt"),
			GetConfig().advanced_ppc_logging.GetValue(), LaunchSettings::Verbose());
		NetworkConfig::LoadOnce();
		ActiveSettings::Init();

		auto audioInitialization = std::async(std::launch::async, [] {
			IAudioAPI::InitializeStatic();
			IAudioInputAPI::InitializeStatic();
		});
		auto graphicPackInitialization = std::async(std::launch::async, [] {
			GraphicPack2::LoadAll();
		});
		InputManager::instance().ConfigureProfileDirectory(
			ActiveSettings::GetConfigPath("controllerProfiles"));
		InputManager::instance().Start();
		InputManager::instance().load();
		audioInitialization.wait();
		graphicPackInitialization.wait();

		CafeSystem::Initialize();
		CafeTitleList::Initialize(ActiveSettings::GetUserDataPath("title_list_cache.xml"));
		for (const auto& path : GetConfig().game_paths)
			CafeTitleList::AddScanPath(_utf8ToPath(path));
		const fs::path mlcPath = ActiveSettings::GetMlcPath();
		if (!mlcPath.empty())
			CafeTitleList::SetMLCPath(mlcPath);
		CafeTitleList::Refresh();

		CafeSaveList::Initialize();
		if (!mlcPath.empty())
		{
			CafeSaveList::SetMLCPath(mlcPath);
			CafeSaveList::Refresh();
		}
	});
}

void HandlePostUpdate()
{
	const auto filename = ActiveSettings::GetExecutablePath().replace_extension("exe.backup");
	if (!fs::exists(filename))
		return;

#if BOOST_OS_WINDOWS
	HANDLE lock;
	do
	{
		lock = CreateMutexW(nullptr, TRUE, L"Global\\cemu_update_lock");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (lock == nullptr);
	const DWORD waitResult = WaitForSingleObject(lock, 2000);
	CloseHandle(lock);
	if (waitResult == WAIT_OBJECT_0)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		std::error_code error;
		fs::remove(filename, error);
	}
#else
	while (fs::exists(filename))
	{
		std::error_code error;
		fs::remove(filename, error);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
#endif
}
