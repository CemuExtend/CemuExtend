#include "Common/precompiled.h"

#include "application/ApplicationPaths.h"

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

#include <cstdlib>
#include <set>

#if BOOST_OS_WINDOWS
#include <shlobj.h>
#elif BOOST_OS_MACOS
#include <mach-o/dyld.h>
#endif

namespace Application
{
	namespace
	{
		fs::path EnvironmentPath(const char* name, const fs::path& fallback)
		{
			if (const char* value = std::getenv(name); value && *value)
				return _utf8ToPath(value);
			return fallback;
		}

		fs::path NativeExecutablePath()
		{
#if BOOST_OS_WINDOWS
			std::wstring buffer(32768, L'\0');
			const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (length != 0 && length < buffer.size())
			{
				buffer.resize(length);
				return fs::path(buffer);
			}
#elif BOOST_OS_LINUX || BOOST_OS_BSD
			std::error_code error;
			auto path = fs::read_symlink("/proc/self/exe", error);
			if (!error)
				return path;
#elif BOOST_OS_MACOS
			uint32_t size = 0;
			_NSGetExecutablePath(nullptr, &size);
			std::string buffer(size, '\0');
			if (_NSGetExecutablePath(buffer.data(), &size) == 0)
			{
				std::error_code error;
				auto path = fs::weakly_canonical(_utf8ToPath(buffer.c_str()), error);
				return error ? _utf8ToPath(buffer.c_str()) : path;
			}
#endif
			std::error_code fallbackError;
			return fs::current_path(fallbackError) / "Cemu";
		}

		fs::path HomeDirectory()
		{
			return EnvironmentPath("HOME", fs::current_path());
		}

		fs::path InstalledDataPath(const fs::path& nativeExecutable)
		{
#if BOOST_OS_MACOS
			const auto contents = nativeExecutable.parent_path().parent_path();
			if (contents.filename() == "Contents")
				return contents / "SharedSupport";
#elif BOOST_OS_LINUX || BOOST_OS_BSD
			const auto shared = nativeExecutable.parent_path().parent_path() / "share" / "Cemu";
			std::error_code error;
			if (fs::is_directory(shared, error))
				return shared;
#endif
			return nativeExecutable.parent_path();
		}

#if BOOST_OS_WINDOWS
		fs::path RoamingDataPath()
		{
			PWSTR value = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &value)))
			{
				CoTaskMemFree(value);
				return {};
			}
			fs::path result(value);
			CoTaskMemFree(value);
			return result;
		}
#endif
	}

	static void InitializePathsOnce()
	{
		const fs::path nativeExecutable = NativeExecutablePath();
		fs::path executable = nativeExecutable;
#if BOOST_OS_LINUX || BOOST_OS_BSD
		// Keep portable-mode compatibility when running from an AppImage while
		// retaining the mounted executable path for resource discovery.
		executable = EnvironmentPath("APPIMAGE", nativeExecutable);
#endif
		fs::path portable = executable.parent_path() / "portable";
#if BOOST_OS_MACOS
		const auto contents = nativeExecutable.parent_path().parent_path();
		if (contents.filename() == "Contents" && contents.parent_path().extension() == ".app")
			portable = contents.parent_path().parent_path() / "portable";
#endif
		std::error_code error;
		bool isPortable = false;
#ifdef CEMU_ALLOW_PORTABLE
		isPortable = fs::is_directory(portable, error);
#endif

		fs::path userData;
		fs::path config;
		fs::path cache;
		fs::path data = InstalledDataPath(nativeExecutable);
		if (isPortable)
		{
			userData = config = cache = portable;
			data = executable.parent_path();
		}
		else
		{
#if BOOST_OS_WINDOWS
			auto roaming = RoamingDataPath();
			if (roaming.empty())
				roaming = EnvironmentPath("APPDATA", {});
			if (roaming.empty())
				throw std::runtime_error("Cemu cannot determine the Windows roaming-data directory");
			userData = config = cache = roaming / "Cemu";
			if (fs::exists(executable.parent_path() / "settings.xml", error))
			{
				isPortable = true;
				userData = config = cache = executable.parent_path();
			}
#elif BOOST_OS_MACOS
			const auto home = HomeDirectory();
			userData = config = home / "Library" / "Application Support" / "Cemu";
			cache = home / "Library" / "Caches" / "Cemu";
#else
			const auto home = HomeDirectory();
			userData = EnvironmentPath("XDG_DATA_HOME", home / ".local" / "share") / "Cemu";
			config = EnvironmentPath("XDG_CONFIG_HOME", home / ".config") / "Cemu";
			cache = EnvironmentPath("XDG_CACHE_HOME", home / ".cache") / "Cemu";
#endif
		}

		std::set<fs::path> failedWriteAccess;
		ActiveSettings::SetPaths(isPortable, executable, userData, config, cache, data,
			failedWriteAccess);
		if (!failedWriteAccess.empty())
		{
			std::string paths;
			for (const auto& path : failedWriteAccess)
			{
				if (!paths.empty())
					paths += ", ";
				paths += _pathToUtf8(path);
			}
			throw std::runtime_error("Cemu cannot write to required application directories: " + paths);
		}
		GetConfigHandle().SetFilename(
			ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
	}

	void InitializePaths()
	{
		static std::once_flag initialized;
		std::call_once(initialized, InitializePathsOnce);
	}
}
