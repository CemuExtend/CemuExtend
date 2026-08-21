#include "Common/precompiled.h"
#include "Common/version.h"

#include "application/ApplicationRuntime.h"
#include "application/ApplicationHost.h"
#include "application/EmulationController.h"
#include "audio/IAudioAPI.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "config/ActiveSettings.h"
#include "frontend/CemuExtendFrontendBridge.h"
#include "frontend/FrontendRuntime.h"
#include "input/InputManager.h"
#include "webview/MainWindowState.h"
#include "webview/NativeWindowHost.h"
#include "webview/RendererHost.h"
#include "webview/RpcDispatcher.h"
#include "webview/ToolWindowSupport.h"
#include "webview/WebHostState.h"
#include "webview/WebHostServices.h"
#include "webview/generated/WebAssets.h"
#include "webview/generated/RpcMethods.h"
#include "webview/generated/WindowRoles.h"
#include "util/helpers/helpers.h"

#include <array>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <webview/webview.h>
#include <boost/nowide/cstdio.hpp>
#include <png.h>

namespace
{
	using WebFrontend::MainWindowState;
	using WebFrontend::RpcDispatcher;
	using WebFrontend::CreateNativeWindowHost;
	using WebFrontend::INativeWindowHost;
	using WebFrontend::MenuCommand;
	using WebFrontend::CreateRendererHost;
	using WebFrontend::IRendererHost;
	using WebFrontend::IToolWindowSupport;
	using WebFrontend::CreateToolWindowSupport;
	using WebFrontend::WebHostState;
	using WebFrontend::WebHostServices;
	class Runtime;
	struct RpcBinding
	{
		Runtime* runtime{};
		webview_t webview{};
		std::uint64_t windowId{};
	};
	struct RuntimeCallbackGate
	{
		std::mutex mutex;
		Runtime* target{};
	};

	std::string JsonString(std::string_view value)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string Base64(std::string_view value)
	{
		static constexpr std::string_view alphabet =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string output;
		output.reserve((value.size() + 2) / 3 * 4);
		for (std::size_t offset = 0; offset < value.size(); offset += 3)
		{
			const auto remaining = value.size() - offset;
			const auto first = static_cast<unsigned char>(value[offset]);
			const auto second = remaining > 1 ? static_cast<unsigned char>(value[offset + 1]) : 0;
			const auto third = remaining > 2 ? static_cast<unsigned char>(value[offset + 2]) : 0;
			const std::uint32_t bits = (first << 16) | (second << 8) | third;
			output.push_back(alphabet[(bits >> 18) & 0x3f]);
			output.push_back(alphabet[(bits >> 12) & 0x3f]);
			output.push_back(remaining > 1 ? alphabet[(bits >> 6) & 0x3f] : '=');
			output.push_back(remaining > 2 ? alphabet[bits & 0x3f] : '=');
		}
		return output;
	}

	std::optional<fs::path> ScreenshotPath(bool mainWindow)
	{
		static std::mutex pathMutex;
		std::scoped_lock lock(pathMutex);
		const auto directory = ActiveSettings::GetUserDataPath("screenshots");
		std::error_code error;
		fs::create_directories(directory, error);
		if (error) return {};
		const auto now = std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now());
		std::tm local{};
#if BOOST_OS_WINDOWS
		localtime_s(&local, &now);
#else
		localtime_r(&now, &local);
#endif
		const auto stem = fmt::format("Screenshot_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}{}",
			local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
			local.tm_hour, local.tm_min, local.tm_sec, mainWindow ? "" : "_GamePad");
		for (unsigned suffix = 0; suffix < 1000; ++suffix)
		{
			const auto candidate = directory / (suffix == 0 ? fmt::format("{}.png", stem) :
				fmt::format("{}_{}.png", stem, suffix + 1));
			if (!fs::exists(candidate, error) && !error) return candidate;
			error.clear();
		}
		return {};
	}

	bool WriteRgbPng(const fs::path& path, std::span<const std::uint8_t> pixels,
		int width, int height)
	{
		if (width <= 0 || height <= 0 || pixels.size() !=
			static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3)
			return false;
		auto temporary = path;
		temporary += ".tmp";
		const auto temporaryUtf8 = _pathToUtf8(temporary);
		FILE* file = boost::nowide::fopen(temporaryUtf8.c_str(), "wb");
		if (!file) return false;
		png_image image{};
		image.version = PNG_IMAGE_VERSION;
		image.width = static_cast<png_uint_32>(width);
		image.height = static_cast<png_uint_32>(height);
		image.format = PNG_FORMAT_RGB;
		const bool written = png_image_write_to_stdio(&image, file, 0,
			pixels.data(), 0, nullptr) != 0;
		png_image_free(&image);
		const bool closed = std::fclose(file) == 0;
		if (!written || !closed)
		{
			std::error_code ignored;
			fs::remove(temporary, ignored);
			return false;
		}
		std::error_code error;
		fs::rename(temporary, path, error);
		if (error)
		{
			fs::remove(temporary, error);
			return false;
		}
		return true;
	}

	std::string TitleIdString(std::uint64_t titleId)
	{
		std::array<char, 17> text{};
		std::snprintf(text.data(), text.size(), "%016llx",
			static_cast<unsigned long long>(titleId));
		return text.data();
	}

	using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer>;

	void WriteAccount(JsonWriter& writer, const Application::AccountInfo& account)
	{
		writer.StartObject();
		writer.Key("persistentId"); writer.Uint(account.persistentId);
		writer.Key("persistentIdHex");
		std::array<char, 9> persistentId{};
		std::snprintf(persistentId.data(), persistentId.size(), "%08x", account.persistentId);
		writer.String(persistentId.data());
		writer.Key("miiName");
		const auto miiName = boost::nowide::narrow(account.miiName);
		writer.String(miiName.data(), static_cast<rapidjson::SizeType>(miiName.size()));
		writer.Key("birthYear"); writer.Uint(account.birthYear);
		writer.Key("birthMonth"); writer.Uint(account.birthMonth);
		writer.Key("birthDay"); writer.Uint(account.birthDay);
		writer.Key("gender"); writer.Uint(account.gender);
		writer.Key("email"); writer.String(account.email.data(),
			static_cast<rapidjson::SizeType>(account.email.size()));
		writer.Key("country"); writer.Uint(account.country);
		writer.Key("validOnlineAccount"); writer.Bool(account.validOnlineAccount);
		writer.EndObject();
	}

	std::string_view AccountNetworkServiceName(Application::AccountNetworkService service)
	{
		switch (service)
		{
		case Application::AccountNetworkService::Nintendo: return "nintendo";
		case Application::AccountNetworkService::Pretendo: return "pretendo";
		case Application::AccountNetworkService::Custom: return "custom";
		case Application::AccountNetworkService::Plasma: return "plasma";
		default: return "offline";
		}
	}

	Application::AccountNetworkService ParseAccountNetworkService(std::string_view service)
	{
		if (service == "offline") return Application::AccountNetworkService::Offline;
		if (service == "nintendo") return Application::AccountNetworkService::Nintendo;
		if (service == "pretendo") return Application::AccountNetworkService::Pretendo;
		if (service == "custom") return Application::AccountNetworkService::Custom;
		if (service == "plasma") return Application::AccountNetworkService::Plasma;
		throw std::invalid_argument("unknown account network service");
	}

	std::string_view AccountFileStateName(Application::AccountFileState state)
	{
		switch (state)
		{
		case Application::AccountFileState::Corrupted: return "corrupted";
		case Application::AccountFileState::Ok: return "ok";
		default: return "missing";
		}
	}

	std::string_view AccountOnlineErrorName(Application::AccountOnlineError error)
	{
		switch (error)
		{
		case Application::AccountOnlineError::NoAccountId: return "noAccountId";
		case Application::AccountOnlineError::NoPasswordCached: return "noPasswordCached";
		case Application::AccountOnlineError::PasswordCacheEmpty: return "passwordCacheEmpty";
		case Application::AccountOnlineError::NoPrincipalId: return "noPrincipalId";
		default: return "none";
		}
	}

	std::string AccountJson(const Application::AccountInfo& account)
	{
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		WriteAccount(writer, account);
		return {buffer.GetString(), buffer.GetSize()};
	}

	void WriteGraphicPack(JsonWriter& writer, const Application::GraphicPackInfo& pack)
	{
		writer.StartObject();
		auto string = [&writer](const char* key, const std::string& value) {
			writer.Key(key); writer.String(value.data(),
				static_cast<rapidjson::SizeType>(value.size()));
		};
		string("key", pack.key); string("virtualPath", pack.virtualPath);
		string("name", pack.name); string("description", pack.description);
		writer.Key("version"); writer.Int(pack.version);
		writer.Key("universal"); writer.Bool(pack.universal);
		writer.Key("enabled"); writer.Bool(pack.enabled);
		writer.Key("activated"); writer.Bool(pack.activated);
		writer.Key("defaultEnabled"); writer.Bool(pack.defaultEnabled);
		writer.Key("hasShaders"); writer.Bool(pack.hasShaders);
		writer.Key("hasPatches"); writer.Bool(pack.hasPatches);
		writer.Key("hasCustomVsync"); writer.Bool(pack.hasCustomVsync);
		writer.Key("supportedVersion"); writer.Bool(pack.supportedVersion);
		writer.Key("titleIds"); writer.StartArray();
		for (const auto titleId : pack.titleIds)
		{
			const auto value = TitleIdString(titleId);
			writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
		}
		writer.EndArray();
		writer.Key("presetOrder"); writer.StartArray();
		for (const auto& category : pack.presetOrder)
			writer.String(category.data(), static_cast<rapidjson::SizeType>(category.size()));
		writer.EndArray();
		writer.Key("presets"); writer.StartArray();
		for (const auto& preset : pack.presets)
		{
			writer.StartObject();
			string("category", preset.category); string("name", preset.name);
			writer.Key("active"); writer.Bool(preset.active);
			writer.Key("visible"); writer.Bool(preset.visible);
			writer.EndObject();
		}
		writer.EndArray();
		writer.EndObject();
	}

	std::string GraphicPackMutationJson(const Application::GraphicPackResult& result)
	{
		if (!result)
			throw std::runtime_error(result.diagnostic.empty() ?
				"graphic pack operation failed" : result.diagnostic);
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		writer.StartObject();
		writer.Key("changed"); writer.Bool(result.changed);
		writer.Key("titleRunning"); writer.Bool(result.titleRunning);
		writer.Key("requiresRestart"); writer.Bool(result.requiresRestart);
		writer.Key("applied"); writer.Bool(result.applied);
		writer.Key("reloaded"); writer.Bool(result.reloaded);
		writer.Key("diagnostic"); writer.String(result.diagnostic.data(),
			static_cast<rapidjson::SizeType>(result.diagnostic.size()));
		if (result.info)
		{
			writer.Key("info"); WriteGraphicPack(writer, *result.info);
		}
		writer.EndObject();
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string_view GraphicPackInstallPhaseName(
		Application::GraphicPackInstallPhase phase)
	{
		switch (phase)
		{
		case Application::GraphicPackInstallPhase::Downloading: return "downloading";
		case Application::GraphicPackInstallPhase::Extracting: return "extracting";
		case Application::GraphicPackInstallPhase::Refreshing: return "refreshing";
		default: return "checking";
		}
	}

	std::string_view GraphicPackInstallErrorName(
		Application::GraphicPackInstallError error)
	{
		switch (error)
		{
		case Application::GraphicPackInstallError::ConfirmationRequired: return "confirmationRequired";
		case Application::GraphicPackInstallError::Cancelled: return "cancelled";
		case Application::GraphicPackInstallError::InvalidUrl: return "invalidUrl";
		case Application::GraphicPackInstallError::ConnectionFailed: return "connectionFailed";
		case Application::GraphicPackInstallError::InvalidArchive: return "invalidArchive";
		case Application::GraphicPackInstallError::Conflict: return "conflict";
		case Application::GraphicPackInstallError::IoFailure: return "ioFailure";
		default: return "none";
		}
	}

	std::string FrontendSettingsJson(
		const Application::FrontendSettingsSnapshot& snapshot)
	{
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		writer.StartObject();
		writer.Key("revision"); writer.Uint64(snapshot.revision);
		writer.Key("gamePaths"); writer.StartArray();
		for (const auto& path : snapshot.gamePaths)
		{
			const auto value = _pathToUtf8(path);
			writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
		}
		writer.EndArray();
		writer.Key("startFullscreen"); writer.Bool(snapshot.startFullscreen);
		writer.Key("openPad"); writer.Bool(snapshot.openPad);
		writer.Key("checkUpdates"); writer.Bool(snapshot.checkUpdates);
		writer.Key("saveScreenshots"); writer.Bool(snapshot.saveScreenshots);
		writer.Key("updateChecksSupported"); writer.Bool(snapshot.updateChecksSupported);
		writer.Key("portableMode"); writer.Bool(snapshot.portableMode);
		writer.Key("titleRunning"); writer.Bool(snapshot.titleRunning);
		writer.Key("setupCompleted"); writer.Bool(snapshot.setupCompleted);
		writer.Key("fullscreenOverride");
		if (snapshot.fullscreenOverride) writer.Bool(*snapshot.fullscreenOverride);
		else writer.Null();
		writer.EndObject();
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string_view FrontendSettingsErrorName(Application::FrontendSettingsError error)
	{
		switch (error)
		{
		case Application::FrontendSettingsError::Conflict: return "conflict";
		case Application::FrontendSettingsError::TitleRunning: return "titleRunning";
		case Application::FrontendSettingsError::FullscreenOverride: return "fullscreenOverride";
		case Application::FrontendSettingsError::UpdateUnsupported: return "updateUnsupported";
		case Application::FrontendSettingsError::InvalidPath: return "invalidPath";
		case Application::FrontendSettingsError::StorageFailed: return "storageFailed";
		case Application::FrontendSettingsError::SaveFailed: return "saveFailed";
		default: return "none";
		}
	}

	std::string FrontendSettingsResultJson(
		const Application::FrontendSettingsResult& result)
	{
		return std::string(R"({"ok":)") + (result ? "true" : "false") +
			R"(,"error":)" + JsonString(FrontendSettingsErrorName(result.error)) +
			R"(,"snapshot":)" + FrontendSettingsJson(result.snapshot) +
			R"(,"diagnostic":)" + JsonString(result.diagnostic) + "}";
	}

	std::string_view HotkeyActionName(Application::HotkeyAction action)
	{
		switch (action)
		{
		case Application::HotkeyAction::ToggleFullscreen: return "toggleFullscreen";
		case Application::HotkeyAction::ToggleFullscreenAlternative: return "toggleFullscreenAlternative";
		case Application::HotkeyAction::ExitFullscreen: return "exitFullscreen";
		case Application::HotkeyAction::TakeScreenshot: return "takeScreenshot";
		case Application::HotkeyAction::ToggleFastForward: return "toggleFastForward";
		case Application::HotkeyAction::EndEmulation: return "endEmulation";
		case Application::HotkeyAction::ExitApplication: return "exitApplication";
		}
		return "toggleFullscreen";
	}

	Application::HotkeyAction ParseHotkeyAction(std::string_view action)
	{
		if (action == "toggleFullscreen") return Application::HotkeyAction::ToggleFullscreen;
		if (action == "toggleFullscreenAlternative") return Application::HotkeyAction::ToggleFullscreenAlternative;
		if (action == "exitFullscreen") return Application::HotkeyAction::ExitFullscreen;
		if (action == "takeScreenshot") return Application::HotkeyAction::TakeScreenshot;
		if (action == "toggleFastForward") return Application::HotkeyAction::ToggleFastForward;
		if (action == "endEmulation") return Application::HotkeyAction::EndEmulation;
		if (action == "exitApplication") return Application::HotkeyAction::ExitApplication;
		throw std::invalid_argument("unknown hotkey action");
	}

	std::string_view HotkeySettingsErrorName(Application::HotkeySettingsError error)
	{
		switch (error)
		{
		case Application::HotkeySettingsError::Conflict: return "conflict";
		case Application::HotkeySettingsError::InvalidBinding: return "invalidBinding";
		case Application::HotkeySettingsError::DuplicateBinding: return "duplicateBinding";
		case Application::HotkeySettingsError::SaveFailed: return "saveFailed";
		default: return "none";
		}
	}

	const rapidjson::Value& RequiredMember(const rapidjson::Value& object,
		const char* name)
	{
		if (!object.IsObject())
			throw std::invalid_argument("RPC parameters must be an object");
		const auto member = object.FindMember(name);
		if (member == object.MemberEnd())
			throw std::invalid_argument(std::string(name) + " is required");
		return member->value;
	}

	std::string_view RequiredString(const rapidjson::Value& object, const char* name)
	{
		const auto& value = RequiredMember(object, name);
		if (!value.IsString())
			throw std::invalid_argument(std::string(name) + " must be a string");
		return {value.GetString(), value.GetStringLength()};
	}

	std::uint32_t RequiredUint(const rapidjson::Value& object, const char* name)
	{
		const auto& value = RequiredMember(object, name);
		if (!value.IsUint())
			throw std::invalid_argument(std::string(name) + " must be an unsigned integer");
		return value.GetUint();
	}

	std::uint64_t RequiredUint64(const rapidjson::Value& object, const char* name)
	{
		const auto& value = RequiredMember(object, name);
		if (!value.IsUint64())
			throw std::invalid_argument(std::string(name) + " must be an unsigned integer");
		return value.GetUint64();
	}

	std::uint32_t RequiredBoundedUint(const rapidjson::Value& object, const char* name,
		std::uint32_t minimum, std::uint32_t maximum)
	{
		const auto value = RequiredUint(object, name);
		if (value < minimum || value > maximum)
			throw std::invalid_argument(std::string(name) + " is outside the supported range");
		return value;
	}

	bool RequiredBool(const rapidjson::Value& object, const char* name)
	{
		const auto& value = RequiredMember(object, name);
		if (!value.IsBool())
			throw std::invalid_argument(std::string(name) + " must be a boolean");
		return value.GetBool();
	}

	double RequiredDouble(const rapidjson::Value& object, const char* name)
	{
		const auto& value = RequiredMember(object, name);
		if (!value.IsNumber() || !std::isfinite(value.GetDouble()))
			throw std::invalid_argument(std::string(name) + " must be a finite number");
		return value.GetDouble();
	}

	struct WindowDescriptor
	{
		std::string_view role;
		std::string_view title;
		int width;
		int height;
		bool modal;
	};

	constexpr std::array WindowDescriptors{
		WindowDescriptor{"general-settings", "General Settings", 900, 680, false},
		WindowDescriptor{"input-settings", "Input Settings", 980, 720, false},
		WindowDescriptor{"hotkey-settings", "Hotkey Settings", 860, 620, false},
		WindowDescriptor{"graphic-packs", "Graphic Packs", 1040, 760, false},
		WindowDescriptor{"title-manager", "Title Manager", 1100, 720, false},
		WindowDescriptor{"cemod-manager", "CemuExtend Manager", 980, 680, false},
		WindowDescriptor{"cemod-permissions", "CemuExtend Permissions", 760, 620, true},
		WindowDescriptor{"account-manager", "Account Manager", 760, 620, true},
		WindowDescriptor{"save-manager", "Save Manager", 920, 680, true},
		WindowDescriptor{"update-manager", "Updates", 820, 620, true},
		WindowDescriptor{"logging", "Logging", 980, 700, false},
		WindowDescriptor{"memory-searcher", "Memory Searcher", 1080, 720, false},
		WindowDescriptor{"ppc-debugger", "PPC Debugger", 1280, 800, false},
		WindowDescriptor{"audio-debugger", "Audio Debugger", 980, 700, false},
		WindowDescriptor{"texture-relations", "Texture Relations", 1100, 720, false},
		WindowDescriptor{"ppc-threads", "PPC Threads", 940, 680, false},
		WindowDescriptor{"emulated-usb-devices", "Emulated USB Devices", 820, 620, false},
		WindowDescriptor{"checksum-tool", "Checksum Tool", 820, 620, false},
		WindowDescriptor{"getting-started", "Getting Started", 900, 680, true},
		WindowDescriptor{"about", "About CemuExtend", 680, 560, true},
	};

	const WindowDescriptor& DescribeWindow(std::string_view role)
	{
		const auto found = std::ranges::find(WindowDescriptors, role, &WindowDescriptor::role);
		if (found == WindowDescriptors.end())
			throw std::invalid_argument("unknown window role");
		return *found;
	}

	std::uint64_t ParseTitleId(const rapidjson::Value& params)
	{
		const auto found = params.FindMember("titleId");
		if (found == params.MemberEnd() || !found->value.IsString() ||
			found->value.GetStringLength() != 16)
			throw std::invalid_argument("titleId must contain exactly 16 hexadecimal digits");
		std::uint64_t value{};
		const std::string_view text(found->value.GetString(), found->value.GetStringLength());
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw std::invalid_argument("titleId must contain exactly 16 hexadecimal digits");
		return value;
	}

	std::uint16_t UsbHidUsage(std::uint32_t key)
	{
		if (key >= 'A' && key <= 'Z') return static_cast<std::uint16_t>(0x04 + key - 'A');
		if (key >= 'a' && key <= 'z') return static_cast<std::uint16_t>(0x04 + key - 'a');
		if (key >= '1' && key <= '9') return static_cast<std::uint16_t>(0x1e + key - '1');
		if (key == '0') return 0x27;
		switch (key)
		{
		case '-': return 0x2d; case '=': return 0x2e; case '[': return 0x2f;
		case ']': return 0x30; case '\\': return 0x31; case ';': return 0x33;
		case '\'': return 0x34; case '`': return 0x35; case ',': return 0x36;
		case '.': return 0x37; case '/': return 0x38; default: break;
		}
#if BOOST_OS_WINDOWS
		switch (key)
		{
		case VK_RETURN: return 0x28; case VK_ESCAPE: return 0x29; case VK_BACK: return 0x2a;
		case VK_TAB: return 0x2b; case VK_SPACE: return 0x2c; case VK_CAPITAL: return 0x39;
		case VK_F1: return 0x3a; case VK_F2: return 0x3b; case VK_F3: return 0x3c;
		case VK_F4: return 0x3d; case VK_F5: return 0x3e; case VK_F6: return 0x3f;
		case VK_F7: return 0x40; case VK_F8: return 0x41; case VK_F9: return 0x42;
		case VK_F10: return 0x43; case VK_F11: return 0x44; case VK_F12: return 0x45;
		case VK_F13: return 0x68; case VK_F14: return 0x69; case VK_F15: return 0x6a;
		case VK_F16: return 0x6b; case VK_F17: return 0x6c; case VK_F18: return 0x6d;
		case VK_F19: return 0x6e; case VK_F20: return 0x6f; case VK_F21: return 0x70;
		case VK_F22: return 0x71; case VK_F23: return 0x72; case VK_F24: return 0x73;
		case VK_SNAPSHOT: return 0x46; case VK_SCROLL: return 0x47; case VK_PAUSE: return 0x48;
		case VK_INSERT: return 0x49; case VK_HOME: return 0x4a; case VK_PRIOR: return 0x4b;
		case VK_DELETE: return 0x4c; case VK_END: return 0x4d; case VK_NEXT: return 0x4e;
		case VK_RIGHT: return 0x4f; case VK_LEFT: return 0x50; case VK_DOWN: return 0x51;
		case VK_UP: return 0x52; case VK_NUMLOCK: return 0x53;
		case VK_DIVIDE: return 0x54; case VK_MULTIPLY: return 0x55;
		case VK_SUBTRACT: return 0x56; case VK_ADD: return 0x57;
		case VK_NUMPAD1: return 0x59; case VK_NUMPAD2: return 0x5a; case VK_NUMPAD3: return 0x5b;
		case VK_NUMPAD4: return 0x5c; case VK_NUMPAD5: return 0x5d; case VK_NUMPAD6: return 0x5e;
		case VK_NUMPAD7: return 0x5f; case VK_NUMPAD8: return 0x60; case VK_NUMPAD9: return 0x61;
		case VK_NUMPAD0: return 0x62; case VK_DECIMAL: return 0x63; case VK_APPS: return 0x65;
		case VK_OEM_MINUS: return 0x2d; case VK_OEM_PLUS: return 0x2e;
		case VK_OEM_4: return 0x2f; case VK_OEM_6: return 0x30; case VK_OEM_5: return 0x31;
		case VK_OEM_1: return 0x33; case VK_OEM_7: return 0x34; case VK_OEM_3: return 0x35;
		case VK_OEM_COMMA: return 0x36; case VK_OEM_PERIOD: return 0x37; case VK_OEM_2: return 0x38;
		case VK_LCONTROL: return 0xe0; case VK_LSHIFT: return 0xe1; case VK_LMENU: return 0xe2;
		case VK_LWIN: return 0xe3; case VK_RCONTROL: return 0xe4; case VK_RSHIFT: return 0xe5;
		case VK_RMENU: return 0xe6; case VK_RWIN: return 0xe7;
		default: return 0;
		}
#elif BOOST_OS_MACOS
		switch (key)
		{
		case 0x00: return 0x04; case 0x0b: return 0x05; case 0x08: return 0x06;
		case 0x02: return 0x07; case 0x0e: return 0x08; case 0x03: return 0x09;
		case 0x05: return 0x0a; case 0x04: return 0x0b; case 0x22: return 0x0c;
		case 0x26: return 0x0d; case 0x28: return 0x0e; case 0x25: return 0x0f;
		case 0x2e: return 0x10; case 0x2d: return 0x11; case 0x1f: return 0x12;
		case 0x23: return 0x13; case 0x0c: return 0x14; case 0x0f: return 0x15;
		case 0x01: return 0x16; case 0x11: return 0x17; case 0x20: return 0x18;
		case 0x09: return 0x19; case 0x0d: return 0x1a; case 0x07: return 0x1b;
		case 0x10: return 0x1c; case 0x06: return 0x1d;
		case 0x12: return 0x1e; case 0x13: return 0x1f; case 0x14: return 0x20;
		case 0x15: return 0x21; case 0x17: return 0x22; case 0x16: return 0x23;
		case 0x1a: return 0x24; case 0x1c: return 0x25; case 0x19: return 0x26;
		case 0x1d: return 0x27; case 0x1b: return 0x2d; case 0x18: return 0x2e;
		case 0x21: return 0x2f; case 0x1e: return 0x30; case 0x2a: return 0x31;
		case 0x29: return 0x33; case 0x27: return 0x34; case 0x32: return 0x35;
		case 0x2b: return 0x36; case 0x2f: return 0x37; case 0x2c: return 0x38;
		default: break;
		}
		static constexpr std::array<std::pair<std::uint32_t, std::uint16_t>, 41> usages{{
			{0x24,0x28},{0x35,0x29},{0x33,0x2a},{0x30,0x2b},{0x31,0x2c},
			{0x7a,0x3a},{0x78,0x3b},{0x63,0x3c},{0x76,0x3d},{0x60,0x3e},{0x61,0x3f},
			{0x62,0x40},{0x64,0x41},{0x65,0x42},{0x6d,0x43},{0x67,0x44},{0x6f,0x45},
			{0x69,0x68},{0x6b,0x69},{0x71,0x6a},{0x6a,0x6b},
			{0x40,0x6c},{0x4f,0x6d},{0x50,0x6e},{0x5a,0x6f},
			{0x72,0x49},{0x73,0x4a},{0x74,0x4b},{0x75,0x4c},{0x77,0x4d},{0x79,0x4e},
			{0x7c,0x4f},{0x7b,0x50},{0x7d,0x51},{0x7e,0x52},
			{0x3b,0xe0},{0x38,0xe1},{0x3a,0xe2},{0x37,0xe3},{0x3e,0xe4},{0x3c,0xe5}
		}};
		for (const auto& [native, usage] : usages) if (native == key) return usage;
		return 0;
#else
		switch (key)
		{
		case 0xff0d: return 0x28; case 0xff1b: return 0x29; case 0xff08: return 0x2a;
		case 0xff09: return 0x2b; case 0x20: return 0x2c; case 0xffe5: return 0x39;
		case 0xffbe: return 0x3a; case 0xffbf: return 0x3b; case 0xffc0: return 0x3c;
		case 0xffc1: return 0x3d; case 0xffc2: return 0x3e; case 0xffc3: return 0x3f;
		case 0xffc4: return 0x40; case 0xffc5: return 0x41; case 0xffc6: return 0x42;
		case 0xffc7: return 0x43; case 0xffc8: return 0x44; case 0xffc9: return 0x45;
		case 0xffca: return 0x68; case 0xffcb: return 0x69; case 0xffcc: return 0x6a;
		case 0xffcd: return 0x6b; case 0xffce: return 0x6c; case 0xffcf: return 0x6d;
		case 0xffd0: return 0x6e; case 0xffd1: return 0x6f; case 0xffd2: return 0x70;
		case 0xffd3: return 0x71; case 0xffd4: return 0x72; case 0xffd5: return 0x73;
		case 0xff63: return 0x49; case 0xff50: return 0x4a; case 0xff55: return 0x4b;
		case 0xffff: return 0x4c; case 0xff57: return 0x4d; case 0xff56: return 0x4e;
		case 0xff53: return 0x4f; case 0xff51: return 0x50; case 0xff54: return 0x51;
		case 0xff52: return 0x52; case 0xffe3: return 0xe0; case 0xffe1: return 0xe1;
		case 0xffe9: return 0xe2; case 0xffeb: return 0xe3; case 0xffe4: return 0xe4;
		case 0xffe2: return 0xe5; case 0xffea: return 0xe6; case 0xffec: return 0xe7;
		default: return 0;
		}
#endif
	}

	std::vector<std::uint32_t> DecodeUtf8(std::string_view text)
	{
		std::vector<std::uint32_t> result;
		for (std::size_t i = 0; i < text.size();)
		{
			const auto first = static_cast<unsigned char>(text[i++]);
			std::uint32_t codepoint{};
			std::size_t continuation{};
			if (first < 0x80) codepoint = first;
			else if ((first & 0xe0) == 0xc0) { codepoint = first & 0x1f; continuation = 1; }
			else if ((first & 0xf0) == 0xe0) { codepoint = first & 0x0f; continuation = 2; }
			else if ((first & 0xf8) == 0xf0) { codepoint = first & 0x07; continuation = 3; }
			else continue;
			if (i + continuation > text.size()) break;
			bool valid = true;
			for (std::size_t offset = 0; offset < continuation; ++offset)
			{
				const auto byte = static_cast<unsigned char>(text[i++]);
				if ((byte & 0xc0) != 0x80) { valid = false; break; }
				codepoint = (codepoint << 6) | (byte & 0x3f);
			}
			if (valid && codepoint <= 0x10ffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff))
				result.push_back(codepoint);
		}
		return result;
	}

	class Runtime final
	{
	public:
		Runtime()
			: m_nativeWindow(CreateNativeWindowHost())
		{
			m_webview = webview_create(
#if defined(NDEBUG)
				0,
#else
				1,
#endif
				m_nativeWindow->GetNativeWindow());
			try
			{
				if (!m_webview)
					throw std::runtime_error("failed to create the native webview window");
				m_mainBinding = {this, m_webview, 0};
				m_webViewWidget = webview_get_native_handle(
					m_webview, WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET);
				if (!m_webViewWidget)
					throw std::runtime_error("failed to acquire the native webview widget");
				m_nativeWindow->AttachWebView(m_webViewWidget);
				m_nativeWindow->SetCloseHandler([this] { (void)RequestShutdown(); });
				m_nativeWindow->SetMenuHandler(
					[this](MenuCommand command) { HandleMenu(command); });
				m_nativeWindow->SetMetricsHandler(
					[this](Host::WindowMetricsSnapshot metrics) { HandleMetrics(metrics); });
				m_nativeWindow->SetInputHandler(
					[this](const WebFrontend::NativeInputEvent& event) { HandleNativeInput(event); });
				m_nativeWindow->SetPadCloseHandler([this] {
					const auto expectedGeneration = m_padGeneration;
					PostToUi([this, expectedGeneration] {
						(void)ClosePadRenderRegion(expectedGeneration);
					});
				});
				m_mainWindowPublication = m_hostState->PublishMainWindow(
					m_nativeWindow->GetMainWindowHandle());
				m_rendererHost = CreateRendererHost(m_hostState, m_hostState, m_hostState);
				m_hostServices = std::make_shared<WebHostServices>(m_hostState, *m_nativeWindow,
					[this](std::function<void()> action) { return PostToUi(std::move(action)); },
					[this] { return RecreateCanvasForHost(); });
				RefreshHotkeyBindings(m_controller.GetHotkeySettings());
				m_hostServices->SetControllerObserver(
					[this](const ControllerState& current, const ControllerState& previous) {
						HandleControllerHotkeys(current, previous);
					});
				Application::ConnectHost({
					.windowMetrics = m_hostServices,
					.clipboard = m_hostServices,
					.externalLauncher = m_hostServices,
					.inputFocus = m_hostServices,
					.canvas = m_hostServices,
				});
				InputManager::instance().ConfigureHost(*m_hostServices, *m_hostServices,
					*m_hostServices, *m_hostServices);
				IAudioAPI::ConfigureNativeSurfaceProvider(m_hostServices.get());
				m_hostConnected = true;
				m_windowState = std::make_unique<MainWindowState>(reinterpret_cast<std::uintptr_t>(
					m_nativeWindow->GetNativeWindow()));
				m_callbackGate->target = this;
				RegisterRpc();
				m_applicationEvents = m_controller.Events().Subscribe(
					[gate = m_callbackGate](const Application::Event& event) {
						std::scoped_lock lock(gate->mutex);
						if (gate->target)
							gate->target->ForwardEvent(event);
					});
				m_titleEvents = m_controller.SubscribeTitleCatalog(
					[gate = m_callbackGate](const Application::TitleCatalogEvent&) {
						std::scoped_lock lock(gate->mutex);
						if (gate->target)
							gate->target->Emit("titles.changed", "{}");
					});
				if (webview_bind(m_webview, "cemuInvoke", &Runtime::Invoke, &m_mainBinding) != WEBVIEW_ERROR_OK)
					throw std::runtime_error("failed to install the native RPC binding");
				m_rpcBound = true;
				webview_set_title(m_webview, "CemuExtend");
				webview_set_size(m_webview, 1100, 720, WEBVIEW_HINT_NONE);
			}
			catch (...)
			{
				Cleanup();
				throw;
			}
		}

		~Runtime() { Cleanup(); }

		void Run()
		{
			LoadWebView(m_webview);
			m_nativeWindow->Show();
			webview_run(m_webview);
		}

	private:
		struct ToolWindow
		{
			std::uint64_t id{};
			std::string role;
			std::optional<std::uint64_t> titleContext;
			webview_t webview{};
			RpcBinding binding;
			std::unique_ptr<IToolWindowSupport> nativeSupport;
			bool rpcBound{};
			bool closeRequested{};
			bool closing{};
		};

		struct DeferredToolClose
		{
			std::shared_ptr<RuntimeCallbackGate> gate;
			std::uint64_t windowId{};
		};

		struct DeferredMainTermination
		{
			std::shared_ptr<RuntimeCallbackGate> gate;
		};

		struct BackgroundJob
		{
			std::uint64_t id{};
			std::uint64_t ownerWindow{};
			std::shared_ptr<std::atomic_bool> cancelled;
			std::jthread worker;
		};

		struct BackgroundJobEvent
		{
			std::shared_ptr<RuntimeCallbackGate> gate;
			std::uint64_t jobId{};
			std::uint64_t ownerWindow{};
			std::string type;
			std::string payload;
			bool final{};
		};

		static void DispatchToolCloseAfterReply(webview_t, void* context)
		{
			std::unique_ptr<DeferredToolClose> pending(
				static_cast<DeferredToolClose*>(context));
			std::scoped_lock lock(pending->gate->mutex);
			if (pending->gate->target)
			{
				auto* runtime = pending->gate->target;
				const auto id = pending->windowId;
				(void)runtime->PostToUi([runtime, id] { runtime->RequestToolWindowClose(id); });
			}
		}

		static void DispatchToolCloseAfterDrain(webview_t, void* context)
		{
			std::unique_ptr<DeferredToolClose> pending(
				static_cast<DeferredToolClose*>(context));
			std::scoped_lock lock(pending->gate->mutex);
			if (pending->gate->target)
			{
				auto* runtime = pending->gate->target;
				const auto id = pending->windowId;
				(void)runtime->PostToUi([runtime, id] { runtime->CloseToolWindow(id); });
			}
		}

		static void DispatchMainTerminationAfterReply(webview_t, void* context)
		{
			std::unique_ptr<DeferredMainTermination> pending(
				static_cast<DeferredMainTermination*>(context));
			std::scoped_lock lock(pending->gate->mutex);
			if (pending->gate->target)
			{
				pending->gate->target->m_mainReplyPending = false;
				pending->gate->target->MaybeTerminateAfterShutdown();
			}
		}

		static void PostBackgroundJobEvent(std::shared_ptr<RuntimeCallbackGate> gate,
			std::uint64_t jobId, std::uint64_t ownerWindow, std::string type,
			std::string payload, bool final)
		{
			auto event = std::make_shared<BackgroundJobEvent>(BackgroundJobEvent{
				std::move(gate), jobId, ownerWindow, std::move(type), std::move(payload), final});
			std::scoped_lock lock(event->gate->mutex);
			if (!event->gate->target)
				return;
			auto* runtime = event->gate->target;
			(void)runtime->PostToUi([event] {
				std::scoped_lock callbackLock(event->gate->mutex);
				if (event->gate->target)
					event->gate->target->DeliverBackgroundJobEvent(*event);
			});
		}

		void DeliverBackgroundJobEvent(const BackgroundJobEvent& event)
		{
			const auto job = m_backgroundJobs.find(event.jobId);
			if (job == m_backgroundJobs.end() || job->second->ownerWindow != event.ownerWindow)
				return;
			const bool ownerAlive = event.ownerWindow == 0 ||
				m_toolWindows.contains(event.ownerWindow);
			if (ownerAlive)
				EmitToWindow(event.ownerWindow, event.type, event.payload);
			if (event.final)
				m_backgroundJobs.erase(job);
		}

		std::uint64_t StartGraphicPackInstallJob(
			Application::GraphicPackInstallRequest request)
		{
			if (std::ranges::any_of(m_backgroundJobs,
				[this](const auto& entry) { return entry.second->ownerWindow == m_invokingWindow; }))
				throw std::runtime_error("this window already has a background operation in progress");
			constexpr std::size_t kMaximumBackgroundJobs = 4;
			if (m_backgroundJobs.size() >= kMaximumBackgroundJobs)
				throw std::runtime_error("too many background operations are in progress");
			const auto id = ++m_nextBackgroundJobId;
			const auto owner = m_invokingWindow;
			auto job = std::make_unique<BackgroundJob>();
			job->id = id;
			job->ownerWindow = owner;
			job->cancelled = std::make_shared<std::atomic_bool>();
			auto cancelled = job->cancelled;
			auto gate = m_callbackGate;
			auto* controller = &m_controller;
			m_backgroundJobs.emplace(id, std::move(job));
			m_backgroundJobs.at(id)->worker = std::jthread(
				[controller, gate = std::move(gate), cancelled, id, owner,
					request = std::move(request)](std::stop_token stopToken) mutable {
					auto isCancelled = [cancelled, stopToken] {
						return cancelled->load(std::memory_order_acquire) || stopToken.stop_requested();
					};
					auto progress = [gate, id, owner](
						const Application::GraphicPackInstallProgress& value) {
						PostBackgroundJobEvent(gate, id, owner, "jobs.progress",
							std::string(R"({"jobId":)") + std::to_string(id) +
							R"(,"windowId":)" + std::to_string(owner) +
							R"(,"phase":)" + JsonString(GraphicPackInstallPhaseName(value.phase)) +
							R"(,"completed":)" + std::to_string(value.completed) +
							R"(,"total":)" + std::to_string(value.total) +
							R"(,"currentPath":)" + JsonString(value.currentPath) + "}", false);
					};
					Application::GraphicPackInstallResult result;
					try
					{
						result = controller->InstallGraphicPacks(request, std::move(progress),
							std::move(isCancelled));
					}
					catch (const std::exception& error)
					{
						result = {Application::GraphicPackInstallError::IoFailure, error.what()};
					}
					catch (...)
					{
						result = {Application::GraphicPackInstallError::IoFailure,
							"graphic-pack worker failed with an unknown error"};
					}
					rapidjson::StringBuffer buffer;
					JsonWriter writer(buffer);
					writer.StartObject();
					writer.Key("jobId"); writer.Uint64(id);
					writer.Key("windowId"); writer.Uint64(owner);
					writer.Key("ok"); writer.Bool(static_cast<bool>(result));
					writer.Key("error"); writer.String(GraphicPackInstallErrorName(result.error).data());
					writer.Key("diagnostic"); writer.String(result.diagnostic.data(),
						static_cast<rapidjson::SizeType>(result.diagnostic.size()));
					writer.Key("upToDate"); writer.Bool(result.upToDate);
					writer.Key("removedEnabledPaths"); writer.StartArray();
					for (const auto& path : result.removedEnabledPaths)
						writer.String(path.data(), static_cast<rapidjson::SizeType>(path.size()));
					writer.EndArray(); writer.EndObject();
					PostBackgroundJobEvent(gate, id, owner, "jobs.completed",
						{buffer.GetString(), buffer.GetSize()}, true);
				});
			return id;
		}

		void CancelBackgroundJobsForWindow(std::uint64_t owner) noexcept
		{
			for (auto& [id, job] : m_backgroundJobs)
			{
				(void)id;
				if (job->ownerWindow == owner)
				{
					job->cancelled->store(true, std::memory_order_release);
					job->worker.request_stop();
				}
			}
		}

		void StopAllBackgroundJobs() noexcept
		{
			for (auto& [id, job] : m_backgroundJobs)
			{
				(void)id;
				job->cancelled->store(true, std::memory_order_release);
				job->worker.request_stop();
			}
			m_backgroundJobs.clear();
		}

		void LoadWebView(webview_t webview)
		{
			if (const char* devUrl = std::getenv("CEMU_WEB_UI_DEV_URL"); devUrl && *devUrl)
			{
				const std::string_view url(devUrl);
				if (!url.starts_with("http://127.0.0.1:") && !url.starts_with("http://localhost:"))
					throw std::runtime_error("CEMU_WEB_UI_DEV_URL must use a loopback HTTP origin");
				if (webview_navigate(webview, devUrl) != WEBVIEW_ERROR_OK)
					throw std::runtime_error("failed to navigate the webview to the development UI");
				return;
			}
			const std::string html(reinterpret_cast<const char*>(WebAssets::html),
				WebAssets::htmlSize);
			if (webview_set_html(webview, html.c_str()) != WEBVIEW_ERROR_OK)
				throw std::runtime_error("failed to load embedded web UI assets");
		}

		std::string_view RoleForWindow(std::uint64_t windowId) const
		{
			if (windowId == 0) return "main-library";
			const auto found = m_toolWindows.find(windowId);
			if (found == m_toolWindows.end())
				throw std::runtime_error("the RPC caller window is no longer active");
			return found->second->role;
		}

		std::uint64_t QueueToolWindow(std::string_view role, std::string requestId,
			std::optional<std::uint64_t> titleContext = std::nullopt)
		{
			if (m_rpc.IsShuttingDown())
				throw std::runtime_error("the application is shutting down");
			(void)DescribeWindow(role);
			if (std::ranges::find(WebFrontend::Generated::ImplementedWindowRoles, role) ==
				WebFrontend::Generated::ImplementedWindowRoles.end())
				throw std::runtime_error("the requested window has not been migrated yet");
			const std::string ownedRole(role);
			if (const auto existing = m_windowByRole.find(ownedRole);
				existing != m_windowByRole.end())
			{
				const auto id = existing->second;
				if (const auto found = m_toolWindows.find(id); found != m_toolWindows.end())
				{
					found->second->titleContext = titleContext;
					Emit("window.contextChanged", std::string(R"({"windowId":)") +
						std::to_string(id) + R"(,"titleId":)" +
						(titleContext ? JsonString(TitleIdString(*titleContext)) : "null") + "}");
				}
				(void)PostToUi([this, id] {
					if (const auto found = m_toolWindows.find(id);
						found != m_toolWindows.end() && found->second->nativeSupport)
						found->second->nativeSupport->Focus();
				});
				if (!requestId.empty())
					Emit("window.opened", std::string(R"({"requestId":)") +
						JsonString(requestId) + R"(,"windowId":)" + std::to_string(id) +
						R"(,"role":)" + JsonString(ownedRole) + "}");
				return id;
			}
			if (const auto pending = m_pendingWindowRoles.find(ownedRole);
				pending != m_pendingWindowRoles.end())
			{
				if (const auto context = m_pendingWindowContexts.find(ownedRole);
					context != m_pendingWindowContexts.end() && context->second != titleContext)
					throw std::runtime_error(
						"the window is already opening with a different title context");
				if (!requestId.empty())
					m_pendingWindowRequests[ownedRole].push_back(std::move(requestId));
				return pending->second;
			}
			const auto id = ++m_nextWindowId;
			m_pendingWindowRoles.emplace(ownedRole, id);
			m_pendingWindowContexts[ownedRole] = titleContext;
			if (!requestId.empty())
				m_pendingWindowRequests[ownedRole].push_back(std::move(requestId));
			if (!PostToUi([this, role = ownedRole, id] {
				auto notify = [this, &role, id](std::string_view event,
					std::string_view message = {}) {
					const auto requests = m_pendingWindowRequests.extract(role);
					if (requests.empty()) return;
					for (const auto& requestId : requests.mapped())
					{
						auto payload = std::string(R"({"requestId":)") + JsonString(requestId) +
							R"(,"windowId":)" + std::to_string(id) + R"(,"role":)" +
							JsonString(role);
						if (!message.empty()) payload += R"(,"message":)" + JsonString(message);
						Emit(event, payload + "}");
					}
				};
				if (m_rpc.IsShuttingDown())
				{
					m_pendingWindowRoles.erase(role);
					notify("window.openFailed", "the application is shutting down");
					return;
				}
				try
				{
					const auto context = m_pendingWindowContexts.contains(role) ?
						m_pendingWindowContexts.at(role) : std::optional<std::uint64_t>{};
					CreateToolWindow(role, id, context);
					m_pendingWindowContexts.erase(role);
					notify("window.opened");
				}
				catch (const std::exception& error)
				{
					m_pendingWindowRoles.erase(role);
					m_pendingWindowContexts.erase(role);
					notify("window.openFailed", error.what());
				}
			}))
			{
				m_pendingWindowRoles.erase(ownedRole);
				m_pendingWindowRequests.erase(ownedRole);
				m_pendingWindowContexts.erase(ownedRole);
				throw std::runtime_error("the UI dispatcher is shutting down");
			}
			return id;
		}

		void CreateToolWindow(std::string_view role, std::uint64_t id,
			std::optional<std::uint64_t> titleContext)
		{
			const auto& descriptor = DescribeWindow(role);
			if (const auto existing = m_windowByRole.find(std::string(role));
				existing != m_windowByRole.end())
			{
				const auto window = m_toolWindows.find(existing->second);
				if (window != m_toolWindows.end() && window->second->nativeSupport)
				{
					window->second->nativeSupport->Focus();
					m_pendingWindowRoles.erase(std::string(role));
					return;
				}
				m_windowByRole.erase(existing);
			}
			if (descriptor.modal)
			{
				for (const auto& [id, window] : m_toolWindows)
				{
					(void)id;
					if (DescribeWindow(window->role).modal)
						throw std::runtime_error("another modal window is already open");
				}
			}

			auto window = std::make_unique<ToolWindow>();
			window->id = id;
			window->role = role;
			window->titleContext = titleContext;
			window->nativeSupport = CreateToolWindowSupport(
				m_nativeWindow->GetNativeWindow(), descriptor.modal, [this, id] {
					(void)PostToUi([this, id] { RequestToolWindowClose(id); });
				});
			window->webview = webview_create(
#if defined(NDEBUG)
				0,
#else
				1,
#endif
				window->nativeSupport->GetWindow());
			if (!window->webview)
				throw std::runtime_error("failed to create tool webview window");
			try
			{
				window->binding = {this, window->webview, window->id};
				if (webview_bind(window->webview, "cemuInvoke", &Runtime::Invoke,
					&window->binding) != WEBVIEW_ERROR_OK)
					throw std::runtime_error("failed to bind tool window RPC");
				window->rpcBound = true;
				webview_set_title(window->webview, std::string(descriptor.title).c_str());
				webview_set_size(window->webview, descriptor.width, descriptor.height,
					WEBVIEW_HINT_NONE);
				LoadWebView(window->webview);
				m_windowByRole.emplace(window->role, id);
				m_toolWindows.emplace(id, std::move(window));
				m_toolWindows.at(id)->nativeSupport->Show();
				RefreshInputConfigurationFocus();
				m_pendingWindowRoles.erase(std::string(role));
				return;
			}
			catch (...)
			{
				m_windowByRole.erase(std::string(role));
				if (!window)
				{
					if (const auto inserted = m_toolWindows.find(id); inserted != m_toolWindows.end())
					{
						window = std::move(inserted->second);
						m_toolWindows.erase(inserted);
					}
				}
				if (!window) throw;
				if (window->rpcBound) webview_unbind(window->webview, "cemuInvoke");
				if (window->webview) webview_destroy(window->webview);
				window->nativeSupport.reset();
				throw;
			}
		}

		void CloseToolWindow(std::uint64_t id) noexcept
		{
			const auto found = m_toolWindows.find(id);
			if (found == m_toolWindows.end()) return;
			auto window = std::move(found->second);
			CancelBackgroundJobsForWindow(id);
			m_toolWindows.erase(found);
			m_windowByRole.erase(window->role);
			RefreshInputConfigurationFocus();
			if (window->rpcBound) webview_unbind(window->webview, "cemuInvoke");
			webview_destroy(window->webview);
			window->nativeSupport.reset();
			MaybeTerminateAfterShutdown();
		}

		void RefreshInputConfigurationFocus()
		{
			const bool editing = m_windowByRole.contains("input-settings") ||
				m_windowByRole.contains("hotkey-settings");
			m_hotkeyEditing.store(editing, std::memory_order_release);
			if (m_hostServices) m_hostServices->SetInputConfigurationFocused(editing);
		}

		void MaybeTerminateAfterShutdown() noexcept
		{
			if (m_terminateWhenToolsClosed && !m_mainReplyPending &&
				m_toolWindows.empty() && m_webview)
				webview_terminate(m_webview);
		}

		void RequestToolWindowClose(std::uint64_t id) noexcept
		{
			const auto found = m_toolWindows.find(id);
			if (found == m_toolWindows.end() || std::exchange(found->second->closing, true))
				return;
			auto& window = *found->second;
			if (window.rpcBound)
			{
				webview_unbind(window.webview, "cemuInvoke");
				window.rpcBound = false;
			}
			auto pending = std::make_unique<DeferredToolClose>();
			pending->gate = m_callbackGate;
			pending->windowId = id;
			if (webview_dispatch(window.webview, &Runtime::DispatchToolCloseAfterDrain,
				pending.get()) == WEBVIEW_ERROR_OK)
			{
				pending.release();
				return;
			}
			window.closing = false;
			cemuLog_log(LogType::Force,
				"Failed to drain tool window {} before close; keeping it alive", id);
		}

		void RequestAllToolWindowsClose() noexcept
		{
			std::vector<std::uint64_t> ids;
			ids.reserve(m_toolWindows.size());
			for (const auto& [id, window] : m_toolWindows)
			{
				(void)window;
				ids.push_back(id);
			}
			for (const auto id : ids) RequestToolWindowClose(id);
		}

		void CloseAllToolWindows() noexcept
		{
			m_pendingWindowRoles.clear();
			while (!m_toolWindows.empty()) CloseToolWindow(m_toolWindows.begin()->first);
		}

		void Cleanup() noexcept
		{
			if (std::exchange(m_cleanedUp, true))
				return;
			m_stopping.store(true, std::memory_order_release);
			m_eventStopping->store(true, std::memory_order_release);
			StopAllBackgroundJobs();
			{
				std::scoped_lock lock(m_callbackGate->mutex);
				m_callbackGate->target = nullptr;
			}
			m_titleEvents.Reset();
			m_applicationEvents.Reset();
			m_rpc.BeginShutdown();
			constexpr unsigned maximumShutdownAttempts = 500;
			unsigned shutdownAttempt{};
			while (!TryShutdownApplication(false) &&
				++shutdownAttempt < maximumShutdownAttempts)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			if (!m_applicationShutdown)
			{
				cemuLog_log(LogType::Force,
					"Cemu could not safely release emulation resources during final shutdown; terminating without destroying native renderer surfaces");
				std::_Exit(EXIT_FAILURE);
			}
			if (!DestroyMainRenderRegion())
			{
				cemuLog_log(LogType::Force,
					"Cemu could not safely detach native renderer surfaces during final shutdown");
				std::_Exit(EXIT_FAILURE);
			}
			if (m_windowState)
				(void)m_windowState->BeginShutdown();
			CloseAllToolWindows();
			if (!m_webview)
				return;
			m_nativeWindow->SetCloseHandler({});
			m_nativeWindow->SetMenuHandler({});
			m_nativeWindow->SetMetricsHandler({});
			m_nativeWindow->SetPadCloseHandler({});
			m_nativeWindow->SetInputHandler({});
			m_nativeWindow->UpdateTextInput({});
			if (m_hostServices)
				m_hostServices->Deactivate();
			if (m_hostConnected)
			{
				InputManager::instance().Shutdown();
				Application::DisconnectHost();
				InputManager::instance().ClearHost();
				IAudioAPI::ConfigureNativeSurfaceProvider(nullptr);
				m_hostConnected = false;
			}
			if (m_webViewWidget)
				m_nativeWindow->PrepareWebViewDestroy(m_webViewWidget);
			if (m_rpcBound)
				webview_unbind(m_webview, "cemuInvoke");
			webview_destroy(m_webview);
			m_webview = nullptr;
			m_webViewWidget = nullptr;
			if (m_mainWindowPublication)
			{
				m_hostState->ClearMainWindow(m_mainWindowPublication);
				m_mainWindowPublication = {};
			}
		}

		bool RequestShutdown(bool deferMainReply = false)
		{
			if (m_rpc.IsShuttingDown())
				return true;
			if (!TryShutdownApplication())
			{
				Emit("system.diagnostic",
					std::string(R"({"message":)") +
					JsonString("Cemu could not stop the running title; shutdown was cancelled") + "}");
				return false;
			}
			if (!DestroyMainRenderRegion())
				return false;
			m_nativeWindow->ShowLibrary();
			(void)m_windowState->FinishEmulation();
			m_rpc.BeginShutdown();
			(void)m_windowState->BeginShutdown();
			m_terminateWhenToolsClosed = true;
			m_mainReplyPending = deferMainReply;
			if (!m_toolWindows.empty())
				RequestAllToolWindowsClose();
			MaybeTerminateAfterShutdown();
			return true;
		}

		bool TryShutdownApplication(bool reportFailure = true)
		{
			if (m_applicationShutdown)
				return true;
			const auto result = m_controller.ShutdownApplication();
			if (result.stopped)
			{
				m_applicationShutdown = true;
				return true;
			}
			if (reportFailure)
				cemuLog_log(LogType::Force,
					"Web frontend shutdown could not release emulation resources: {}",
					result.diagnostic);
			return false;
		}

		void StopEmulation()
		{
			const auto result = m_controller.Stop();
			if (!result.stopped)
				return;
			if (!DestroyMainRenderRegion())
				return;
			m_nativeWindow->ShowLibrary();
			(void)m_windowState->FinishEmulation();
		}

		bool DestroyMainRenderRegion()
		{
			if (!ClosePadRenderRegion())
				return false;
			ReleaseNativeInput(true);
			if (m_rendererHost)
				m_rendererHost->PrepareMainDestroy();
			m_nativeWindow->DestroyMainRenderRegion();
			return true;
		}

		bool ClosePadRenderRegion(std::optional<std::uint64_t> expectedGeneration = {})
		{
			if (expectedGeneration && *expectedGeneration != m_padGeneration)
				return true;
			if (!m_nativeWindow->IsPadRenderRegionOpen())
				return true;
			m_nativeWindow->SetPadMetricsEnabled(false);
			auto metrics = m_nativeWindow->GetMetrics();
			metrics.padOpen = false;
			metrics.padWidth = 0;
			metrics.padHeight = 0;
			metrics.physicalPadWidth = 0;
			metrics.physicalPadHeight = 0;
			m_hostState->UpdateMetrics(metrics);
			ReleasePointerSurface(Host::PointerSurface::Pad);
			try
			{
				if (m_rendererHost)
					m_rendererHost->PreparePadDestroy();
			}
			catch (const std::exception& error)
			{
				m_nativeWindow->SetPadMetricsEnabled(true);
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				cemuLog_log(LogType::Force, "Unable to safely close the GamePad surface: {}",
					error.what());
				Emit("system.diagnostic", std::string(R"({"message":)") +
					JsonString(std::string("Unable to safely close GamePad view: ") + error.what()) + "}");
				return false;
			}
			m_nativeWindow->DestroyPadRenderRegion();
			++m_padGeneration;
			return true;
		}

		void TogglePadRenderRegion()
		{
			if (m_controller.State() != Application::EmulationState::Running)
			{
				Emit("system.diagnostic", R"({"message":"GamePad view is only available while a title is running"})");
				return;
			}
			if (m_nativeWindow->IsPadRenderRegionOpen())
			{
				(void)ClosePadRenderRegion();
				return;
			}
			try
			{
				++m_padGeneration;
				auto& region = m_nativeWindow->CreatePadRenderRegion();
				m_rendererHost->InitializePad(region);
				m_nativeWindow->SetPadMetricsEnabled(true);
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				region.SetVisible(true);
				region.RequestFocus();
			}
			catch (const std::exception& error)
			{
				(void)ClosePadRenderRegion();
				Emit("system.diagnostic", std::string(R"({"message":)") +
					JsonString(std::string("Unable to open GamePad view: ") + error.what()) + "}");
			}
		}

		void HandleMetrics(Host::WindowMetricsSnapshot metrics)
		{
			const auto previous = m_hostState->GetWindowMetrics();
			m_hostState->UpdateMetrics(metrics);
			if (previous.appActive != metrics.appActive)
			{
				m_controller.PointerFocusChanged(metrics.appActive);
				if (!metrics.appActive && m_hostServices)
				{
					ReleaseNativeInput(false);
				}
			}
		}

		Frontend::CemuExtendFrontendBridge& PointerBridge(Host::PointerSurface surface)
		{
			return m_pointerBridges[surface == Host::PointerSurface::Main ? 0 : 1];
		}

		std::uint32_t PointerButtonMask(std::uint32_t nativeButton) const
		{
			using Button = Frontend::CemuExtendMouseButton;
			switch (nativeButton)
			{
			case 1: return static_cast<std::uint32_t>(Button::Left);
			case 3: return static_cast<std::uint32_t>(Button::Right);
			case 2: return static_cast<std::uint32_t>(Button::Middle);
			case 8: return static_cast<std::uint32_t>(Button::X1);
			case 9: return static_cast<std::uint32_t>(Button::X2);
			default: return 0;
			}
		}

		Frontend::CemuExtendPointerDecision RefreshPointerPolicy(Host::PointerSurface surface)
		{
			auto& bridge = PointerBridge(surface);
			const auto policy = m_controller.GetPointerPolicy();
			const auto metrics = m_hostState->GetWindowMetrics();
			const bool hasCanvas = surface == Host::PointerSurface::Main
				? m_windowState && m_windowState->Snapshot().mode == WebFrontend::MainWindowContentMode::Playing
				: m_nativeWindow->IsPadRenderRegionOpen();
			const auto decision = bridge.ApplyPointerPolicy(policy.mode, policy.cursor,
				policy.flags, metrics.appActive, hasCanvas);
			m_nativeWindow->ApplyPointerPresentation({
				.surface = surface,
				.ownsPointer = decision.ownsPointer,
				.showCursor = decision.showCursor,
				.confine = decision.confine,
				.enteringCapture = decision.enteringCapture,
				.leavingPolicy = decision.leavingPolicy,
				.requestRawMouse = decision.requestRawMouse,
				.rawMouseEnabled = bridge.RawMouseRequested(),
				.cursor = decision.cursor,
			});
			return decision;
		}

		void SubmitPointer(const WebFrontend::NativeInputEvent& event,
			std::int32_t deltaX, std::int32_t deltaY, std::int32_t wheelX,
			std::int32_t wheelY, std::uint32_t changedButtons, bool raw)
		{
			auto& state = m_pointerStates[event.surface == Host::PointerSurface::Main ? 0 : 1];
			const auto metrics = m_hostState->GetWindowMetrics();
			m_controller.SubmitMouse({
				.surface = event.surface == Host::PointerSurface::Main
					? Application::PointerSurface::Tv : Application::PointerSurface::Drc,
				.x = state.x, .y = state.y, .deltaX = deltaX, .deltaY = deltaY,
				.wheelX = wheelX, .wheelY = wheelY,
				.buttons = PointerBridge(event.surface).MouseButtons(),
				.changedButtons = changedButtons,
				.contentWidth = state.width, .contentHeight = state.height,
				.insideContent = state.inside, .focused = metrics.appActive,
				.flags = raw
					? static_cast<std::uint8_t>(Frontend::CemuExtendMouseEventFlag::RawRelative)
					: static_cast<std::uint8_t>(0),
			});
		}

		void ReleasePointerSurface(Host::PointerSurface surface)
		{
			const auto index = surface == Host::PointerSurface::Main ? 0U : 1U;
			auto& bridge = m_pointerBridges[index];
			const auto released = bridge.UpdateButtons(
				Frontend::CemuExtendMouseTransition::Aggregate, 0xffffffffU, 0);
			if (released.changed)
			{
				WebFrontend::NativeInputEvent event{.kind = WebFrontend::NativeInputKind::PointerButton,
					.surface = surface};
				SubmitPointer(event, 0, 0, 0, 0, released.changed, false);
			}
			(void)bridge.ApplyPointerPolicy(0, 0, 0, false, false);
			bridge.ResetPointerPosition();
			m_pointerStates[index] = {};
			m_nativeWindow->ApplyPointerPresentation({.surface = surface,
				.showCursor = true, .leavingPolicy = true});
		}

		void ReleaseNativeInput(bool resetTextInput)
		{
			if (m_hostServices) m_hostServices->ReleaseKeys();
			m_controller.KeyboardFocusLost();
			ReleasePointerSurface(Host::PointerSurface::Main);
			ReleasePointerSurface(Host::PointerSurface::Pad);
			if (resetTextInput)
			{
				m_textInputSequence = 0;
				m_nativeWindow->UpdateTextInput({});
			}
		}

		void RefreshHotkeyBindings(const Application::HotkeySettingsModel& model)
		{
			std::scoped_lock lock(m_hotkeyMutex);
			m_hotkeySettings = model;
		}

		std::optional<Application::HotkeyAction> MatchKeyboardHotkey(
			std::uint16_t usage, std::uint8_t modifiers) const
		{
			if (!usage || m_hotkeyEditing.load(std::memory_order_acquire)) return {};
			std::scoped_lock lock(m_hotkeyMutex);
			const auto found = std::ranges::find_if(m_hotkeySettings.bindings,
				[usage, modifiers](const Application::HotkeyBinding& binding) {
					return binding.keyboardUsage == usage &&
						binding.keyboardModifiers == (modifiers & 0x0f);
				});
			return found == m_hotkeySettings.bindings.end() ? std::nullopt :
				std::optional{found->action};
		}

		void HandleControllerHotkeys(const ControllerState& current,
			const ControllerState& previous)
		{
			if (m_stopping.load(std::memory_order_acquire) ||
				m_hotkeyEditing.load(std::memory_order_acquire)) return;
			std::optional<Application::HotkeyAction> action;
			{
				std::scoped_lock lock(m_hotkeyMutex);
				if (!m_hotkeySettings.controllerModifier ||
					!current.buttons.GetButtonState(*m_hotkeySettings.controllerModifier))
					return;
				for (const auto button : current.buttons.GetButtonList())
				{
					if (previous.buttons.GetButtonState(button)) continue;
					const auto found = std::ranges::find_if(m_hotkeySettings.bindings,
						[button](const Application::HotkeyBinding& binding) {
							return binding.controllerButton == button;
						});
					if (found == m_hotkeySettings.bindings.end()) continue;
					action = found->action;
					break;
				}
			}
			if (action)
				(void)PostToUi([this, action = *action] { ExecuteHotkey(action); });
		}

		void ExecuteHotkey(Application::HotkeyAction action)
		{
			try
			{
				switch (action)
				{
				case Application::HotkeyAction::ToggleFullscreen:
				case Application::HotkeyAction::ToggleFullscreenAlternative:
					m_fullscreen = !m_fullscreen;
					m_nativeWindow->SetFullscreen(m_fullscreen);
					break;
				case Application::HotkeyAction::ExitFullscreen:
					if (m_fullscreen)
					{
						m_fullscreen = false;
						m_nativeWindow->SetFullscreen(false);
					}
					break;
				case Application::HotkeyAction::TakeScreenshot:
					RequestScreenshot();
					break;
				case Application::HotkeyAction::ToggleFastForward:
					ActiveSettings::SetTimerShiftFactor(
						ActiveSettings::GetTimerShiftFactor() < 3 ? 3 : 1);
					break;
				case Application::HotkeyAction::EndEmulation:
					StopEmulation();
					break;
				case Application::HotkeyAction::ExitApplication:
					(void)RequestShutdown();
					break;
				}
			}
			catch (const std::exception& error)
			{
				Emit("system.diagnostic", std::string(R"({"message":)") +
					JsonString(error.what()) + "}");
			}
		}

		void RequestScreenshot()
		{
			if (!g_renderer)
				throw std::runtime_error("a title must be running before taking a screenshot");
			const bool saveToDisk = m_controller.GetFrontendSettings().saveScreenshots;
			const auto gate = m_callbackGate;
			const auto request = g_renderer->RequestScreenshot(
				[saveToDisk, gate](const std::vector<std::uint8_t>& pixels, int width, int height,
					bool mainWindow) -> std::optional<std::string> {
					if (saveToDisk)
					{
						const auto path = ScreenshotPath(mainWindow);
						if (!path || !WriteRgbPng(*path, pixels, width, height))
							return std::string("Failed to save screenshot");
						return std::string("Screenshot saved to ") + _pathToUtf8(*path);
					}
					auto copy = std::make_shared<std::vector<std::uint8_t>>(pixels);
					bool queued{};
					{
						std::scoped_lock lock(gate->mutex);
						if (gate->target)
							queued = gate->target->PostToUi([gate, copy, width, height] {
								std::scoped_lock callbackLock(gate->mutex);
								if (!gate->target) return;
								if (!gate->target->m_nativeWindow->SetClipboardImage(*copy, width, height))
									gate->target->Emit("system.diagnostic",
										R"({"message":"Failed to copy screenshot to the clipboard"})");
							});
					}
					return queued ? std::optional<std::string>("Screenshot copied to clipboard") :
						std::optional<std::string>("Failed to copy screenshot to clipboard");
				});
			if (!request)
				throw std::runtime_error("a screenshot request is already active");
		}

		void HandleNativeInput(const WebFrontend::NativeInputEvent& event)
		{
			if (m_stopping.load(std::memory_order_acquire) || !m_hostServices)
				return;
			auto& state = m_pointerStates[event.surface == Host::PointerSurface::Main ? 0 : 1];
			auto& bridge = PointerBridge(event.surface);
			switch (event.kind)
			{
			case WebFrontend::NativeInputKind::PointerMove:
			{
				(void)RefreshPointerPolicy(event.surface);
				state = {event.x, event.y, event.contentWidth, event.contentHeight, event.insideContent};
				m_hostServices->UpdateMousePosition(event.surface, {event.x, event.y});
				const auto motion = bridge.UpdatePosition({event.x, event.y},
					{event.contentWidth / 2, event.contentHeight / 2}, bridge.RawMouseRequested());
				SubmitPointer(event, motion.delta.x, motion.delta.y, 0, 0, 0, motion.rawRelative);
				break;
			}
			case WebFrontend::NativeInputKind::PointerButton:
			{
				(void)RefreshPointerPolicy(event.surface);
				state = {event.x, event.y, event.contentWidth, event.contentHeight, event.insideContent};
				const auto mask = PointerButtonMask(event.button);
				const auto update = bridge.UpdateButtons(event.pressed
					? Frontend::CemuExtendMouseTransition::Down
					: Frontend::CemuExtendMouseTransition::Up, mask);
				if (event.button == 1 || event.button == 3)
					m_hostServices->UpdateMouseButton(event.surface,
						event.button == 1 ? Host::PointerButton::Left : Host::PointerButton::Right,
						event.pressed, {event.x, event.y});
				SubmitPointer(event, 0, 0, 0, 0, update.changed, false);
				break;
			}
			case WebFrontend::NativeInputKind::PointerWheel:
			{
				const auto wheelX = bridge.NormalizeWheel(event.wheelX, 120, true);
				const auto wheelY = bridge.NormalizeWheel(event.wheelY, 120, false);
				m_hostServices->UpdateMouseWheel(static_cast<float>(event.wheelY) / 120.0f,
					wheelY);
				SubmitPointer(event, 0, 0, wheelX, wheelY, 0, false);
				break;
			}
			case WebFrontend::NativeInputKind::RawMouse:
				(void)RefreshPointerPolicy(event.surface);
				if (!bridge.RawMouseRequested())
					break;
				bridge.MarkRawMouseSeen();
				bridge.RecordRawPosition({state.x, state.y});
				SubmitPointer(event, event.deltaX, event.deltaY, 0, 0, 0, true);
				break;
			case WebFrontend::NativeInputKind::Touch:
				m_hostServices->UpdateTouch(event.surface, {event.x, event.y}, event.pressed);
				break;
			case WebFrontend::NativeInputKind::Key:
			{
				m_hostServices->UpdateKey(event.key, event.pressed);
				const auto usage = event.usage ? event.usage : UsbHidUsage(event.key);
				if (!event.pressed && !event.repeat && !m_controller.SoftwareKeyboardActive())
					if (const auto action = MatchKeyboardHotkey(usage, event.modifiers))
						ExecuteHotkey(*action);
				if (usage)
					m_controller.SubmitKeyboard(usage, event.pressed, event.modifiers);
				break;
			}
			case WebFrontend::NativeInputKind::Character:
				if (!m_textInputSequence)
					for (const auto codepoint : DecodeUtf8(event.text))
						if (codepoint >= 0x20 && codepoint != 0x7f)
							m_controller.SubmitText(codepoint, event.repeat);
				break;
			case WebFrontend::NativeInputKind::FocusLost:
				m_controller.PointerFocusChanged(false);
				ReleaseNativeInput(false);
				break;
			case WebFrontend::NativeInputKind::DeviceChanged:
				m_hostServices->NotifyDeviceChanged();
				break;
			case WebFrontend::NativeInputKind::TextComposition:
				if (event.textSequence == m_textInputSequence && m_textInputSequence != 0)
					m_controller.SubmitTextComposition(event.text, event.preedit,
						event.textCursor, event.selectionLength);
				break;
			}
		}

		void RefreshTextInput()
		{
			const auto state = m_controller.GetTextInputState();
			m_textInputSequence = state.active ? state.sequence : 0;
			m_nativeWindow->UpdateTextInput({
				.active = state.active, .sequence = state.sequence,
				.initialText = state.initialText, .maximumLength = state.maximumLength,
				.caretX = state.caretX, .caretY = state.caretY,
				.lineHeight = state.lineHeight,
			});
			if (state.active)
				m_controller.KeyboardFocusLost();
		}

		bool RecreateCanvasForHost()
		{
			if (!m_windowState || m_windowState->Snapshot().mode !=
				WebFrontend::MainWindowContentMode::Playing)
				return false;
			if (!ClosePadRenderRegion())
				return false;
			ReleaseNativeInput(true);
			try
			{
				m_rendererHost->PrepareMainDestroy();
				m_nativeWindow->DestroyMainRenderRegion();
				auto& region = m_nativeWindow->CreateMainRenderRegion();
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				m_rendererHost->InitializeMain(region);
				m_nativeWindow->ShowRenderRegion();
				RefreshTextInput();
				return true;
			}
			catch (const std::exception& error)
			{
				cemuLog_log(LogType::Force, "Native canvas recreation failed: {}", error.what());
				try { m_rendererHost->AbandonMainInitialization(); }
				catch (...) {}
				m_nativeWindow->DestroyMainRenderRegion();
				m_nativeWindow->ShowLibrary();
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				return false;
			}
		}

		void HandleMenu(MenuCommand command)
		{
			try
			{
				switch (command)
				{
				case MenuCommand::EndEmulation: StopEmulation(); break;
				case MenuCommand::Exit: (void)RequestShutdown(); break;
				case MenuCommand::ToggleFullscreen:
					m_fullscreen = !m_fullscreen;
					m_nativeWindow->SetFullscreen(m_fullscreen);
					break;
				case MenuCommand::Load: Emit("menu.command", R"({"command":"load"})"); break;
				case MenuCommand::TogglePadView: TogglePadRenderRegion(); break;
				case MenuCommand::GeneralSettings: (void)QueueToolWindow("general-settings", {}); break;
				case MenuCommand::InputSettings: (void)QueueToolWindow("input-settings", {}); break;
				case MenuCommand::GraphicPacks: (void)QueueToolWindow("graphic-packs", {}); break;
				case MenuCommand::TitleManager: (void)QueueToolWindow("title-manager", {}); break;
				case MenuCommand::Logging: (void)QueueToolWindow("logging", {}); break;
				case MenuCommand::About: (void)QueueToolWindow("about", {}); break;
				}
			}
			catch (const std::exception& error)
			{
				Emit("system.diagnostic", std::string(R"({"message":)") +
					JsonString(error.what()) + "}");
			}
		}

		struct PendingEvent
		{
			std::shared_ptr<std::atomic_bool> stopping;
			std::function<void()> beforeDispatch;
			std::string script;
		};

		static void DispatchEvent(webview_t webview, void* argument)
		{
			std::unique_ptr<PendingEvent> pending(static_cast<PendingEvent*>(argument));
			if (pending->stopping->load(std::memory_order_acquire))
				return;
			if (pending->beforeDispatch)
				pending->beforeDispatch();
			if (!pending->script.empty())
				webview_eval(webview, pending->script.c_str());
		}

		bool PostToUi(std::function<void()> action)
		{
			if (m_stopping.load(std::memory_order_acquire))
				return false;
			auto pending = std::make_unique<PendingEvent>();
			pending->stopping = m_eventStopping;
			pending->beforeDispatch = std::move(action);
			if (webview_dispatch(m_webview, &Runtime::DispatchEvent, pending.get()) == WEBVIEW_ERROR_OK)
			{
				pending.release();
				return true;
			}
			return false;
		}

		void Emit(std::string_view type, std::string_view payloadJson,
			std::function<void()> beforeDispatch = {})
		{
			std::scoped_lock eventLock(m_eventMutex);
			if (m_stopping.load(std::memory_order_acquire))
				return;
			const auto sequence = ++m_eventSequence;
			const auto event = std::string(R"({"type":)") + JsonString(type) +
				R"(,"sequence":)" + std::to_string(sequence) + R"(,"payload":)" +
				std::string(payloadJson) + "}";
			const auto script = "window.__cemuDispatchEvent?.(JSON.parse(new TextDecoder().decode(Uint8Array.from(atob('" +
				Base64(event) + "'),c=>c.charCodeAt(0)))));";
			auto pending = std::make_unique<PendingEvent>();
			pending->stopping = m_eventStopping;
			pending->beforeDispatch = [this, beforeDispatch = std::move(beforeDispatch), script] {
				if (beforeDispatch) beforeDispatch();
				webview_eval(m_webview, script.c_str());
				for (const auto& [id, window] : m_toolWindows)
				{
					(void)id;
					if (window->webview) webview_eval(window->webview, script.c_str());
				}
			};
			if (webview_dispatch(m_webview, &Runtime::DispatchEvent, pending.get()) == WEBVIEW_ERROR_OK)
				pending.release();
		}

		void EmitToWindow(std::uint64_t windowId, std::string_view type,
			std::string_view payloadJson)
		{
			std::scoped_lock eventLock(m_eventMutex);
			if (m_stopping.load(std::memory_order_acquire))
				return;
			const auto sequence = ++m_eventSequence;
			const auto event = std::string(R"({"type":)") + JsonString(type) +
				R"(,"sequence":)" + std::to_string(sequence) + R"(,"payload":)" +
				std::string(payloadJson) + "}";
			const auto script = "window.__cemuDispatchEvent?.(JSON.parse(new TextDecoder().decode(Uint8Array.from(atob('" +
				Base64(event) + "'),c=>c.charCodeAt(0)))));";
			auto pending = std::make_unique<PendingEvent>();
			pending->stopping = m_eventStopping;
			pending->beforeDispatch = [this, windowId, script] {
				if (windowId == 0)
				{
					webview_eval(m_webview, script.c_str());
					return;
				}
				const auto found = m_toolWindows.find(windowId);
				if (found != m_toolWindows.end() && found->second->webview)
					webview_eval(found->second->webview, script.c_str());
			};
			if (webview_dispatch(m_webview, &Runtime::DispatchEvent, pending.get()) == WEBVIEW_ERROR_OK)
				pending.release();
		}

		void ForwardEvent(const Application::Event& event)
		{
			switch (event.type)
			{
			case Application::EventType::LoadingStarted: Emit("emulation.loading", "{}"); break;
			case Application::EventType::GameLoaded: Emit("emulation.loaded", "{}"); break;
			case Application::EventType::GameExited:
			{
				const auto expectedGeneration = m_windowState->Snapshot().generation;
				Emit("emulation.exited", "{}", [this, expectedGeneration] {
					const auto state = m_windowState->Snapshot();
					if (state.mode != WebFrontend::MainWindowContentMode::Playing ||
						state.generation != expectedGeneration)
						return;
					if (!DestroyMainRenderRegion())
						return;
					m_nativeWindow->ShowLibrary();
					(void)m_windowState->FinishEmulation();
				});
				break;
			}
			case Application::EventType::PpcProcessExited:
				Emit("emulation.processExited", std::string(R"({"status":)") +
					std::to_string(event.processStatus) + "}");
				break;
			case Application::EventType::PerformanceUpdated:
				Emit("emulation.performance", std::string(R"({"fps":)") +
					std::to_string(event.framesPerSecond) + "}");
				break;
			case Application::EventType::Diagnostic:
				Emit("system.diagnostic", std::string(R"({"message":)") +
					JsonString(event.diagnostic) + "}");
				break;
			case Application::EventType::GameListRefreshRequested: Emit("titles.changed", "{}"); break;
			case Application::EventType::TextInputWakeRequested:
				Emit("input.textWakeRequested", "{}", [this] { RefreshTextInput(); });
				break;
			}
		}

		static void Invoke(const char* sequence, const char* arguments, void* context)
		{
			auto& binding = *static_cast<RpcBinding*>(context);
			auto& self = *binding.runtime;
			rapidjson::Document array;
			array.Parse(arguments);
			std::string response;
			if (!array.IsArray() || array.Size() != 1 || !array[0].IsString())
				response = R"({"id":"","ok":false,"error":{"code":"invalid_binding_call","message":"cemuInvoke expects one JSON string"}})";
			else
			{
				const auto previousWindow = std::exchange(self.m_invokingWindow, binding.windowId);
				response = self.m_rpc.Dispatch(
					std::string_view(array[0].GetString(), array[0].GetStringLength()));
				self.m_invokingWindow = previousWindow;
			}
			const auto encoded = JsonString(response);
			const auto returned = webview_return(binding.webview, sequence, 0, encoded.c_str());
			if (binding.windowId == 0 && self.m_mainReplyPending)
			{
				auto pending = std::make_unique<DeferredMainTermination>();
				pending->gate = self.m_callbackGate;
				if (returned == WEBVIEW_ERROR_OK && webview_dispatch(binding.webview,
					&Runtime::DispatchMainTerminationAfterReply, pending.get()) == WEBVIEW_ERROR_OK)
					pending.release();
				else
				{
					self.m_mainReplyPending = false;
					self.MaybeTerminateAfterShutdown();
				}
			}
			else if (binding.windowId != 0)
			{
				const auto found = self.m_toolWindows.find(binding.windowId);
				if (found != self.m_toolWindows.end() &&
					std::exchange(found->second->closeRequested, false))
				{
					auto pending = std::make_unique<DeferredToolClose>();
					pending->gate = self.m_callbackGate;
					pending->windowId = binding.windowId;
					if (returned == WEBVIEW_ERROR_OK && webview_dispatch(binding.webview,
						&Runtime::DispatchToolCloseAfterReply, pending.get()) == WEBVIEW_ERROR_OK)
						pending.release();
					else
					{
						const auto id = binding.windowId;
						auto* runtime = &self;
						(void)self.PostToUi([runtime, id] { runtime->RequestToolWindowClose(id); });
					}
				}
			}
		}

		void RequireRole(std::initializer_list<std::string_view> roles) const
		{
			const auto role = RoleForWindow(m_invokingWindow);
			if (std::ranges::find(roles, role) == roles.end())
				throw std::runtime_error("this RPC method is not available to the current window role");
		}

		std::string AccountManagerJson() const
		{
			const auto snapshot = m_controller.GetAccountManagerSnapshot();
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartObject();
			writer.Key("accounts"); writer.StartArray();
			for (const auto& account : snapshot.accounts)
				WriteAccount(writer, account);
			writer.EndArray();
			writer.Key("countries"); writer.StartArray();
			for (const auto& country : snapshot.countries)
			{
				writer.StartObject();
				writer.Key("code"); writer.Uint(country.code);
				writer.Key("name"); writer.String(country.name.data(),
					static_cast<rapidjson::SizeType>(country.name.size()));
				writer.EndObject();
			}
			writer.EndArray();
			writer.Key("nextPersistentId"); writer.Uint(snapshot.nextPersistentId);
			writer.Key("hasFreeSlots"); writer.Bool(snapshot.hasFreeSlots);
			writer.Key("activePersistentId"); writer.Uint(snapshot.activePersistentId);
			writer.Key("titleRunning"); writer.Bool(snapshot.titleRunning);
			writer.Key("networkSettings"); writer.StartArray();
			for (const auto& setting : snapshot.networkSettings)
			{
				writer.StartObject();
				writer.Key("persistentId"); writer.Uint(setting.persistentId);
				writer.Key("service"); writer.String(AccountNetworkServiceName(setting.service).data());
				writer.Key("validation"); writer.StartObject();
				writer.Key("validAccount"); writer.Bool(setting.validation.validAccount);
				writer.Key("otp"); writer.String(AccountFileStateName(setting.validation.otp).data());
				writer.Key("seeprom"); writer.String(AccountFileStateName(setting.validation.seeprom).data());
				writer.Key("missingFiles"); writer.StartArray();
				for (const auto& file : setting.validation.missingFiles)
				{
					const auto value = boost::nowide::narrow(file);
					writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
				}
				writer.EndArray();
				writer.Key("accountError");
				writer.String(AccountOnlineErrorName(setting.validation.accountError).data());
				writer.EndObject();
				writer.EndObject();
			}
			writer.EndArray();
			const auto& environment = snapshot.onlineEnvironment;
			writer.Key("onlineEnvironment"); writer.StartObject();
			writer.Key("requiredFilesAvailable"); writer.Bool(environment.requiredFilesAvailable);
			writer.Key("otpPresent"); writer.Bool(environment.otpPresent);
			writer.Key("seepromPresent"); writer.Bool(environment.seepromPresent);
			writer.Key("consoleCertificateAvailable");
			writer.Bool(environment.consoleCertificateAvailable);
			writer.EndObject();
			writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string GraphicPacksJson() const
		{
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartArray();
			for (const auto& pack : m_controller.ListGraphicPacks())
				WriteGraphicPack(writer, pack);
			writer.EndArray();
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string InputSettingsJson() const
		{
			const auto model = m_controller.GetInputSettings();
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			auto typeName = [](Application::EmulatedControllerType type) {
				switch (type)
				{
				case Application::EmulatedControllerType::GamePad: return "gamePad";
				case Application::EmulatedControllerType::ProController: return "proController";
				case Application::EmulatedControllerType::ClassicController: return "classicController";
				case Application::EmulatedControllerType::Wiimote: return "wiimote";
				default: return "disabled";
				}
			};
			auto writeAxis = [&writer](const Application::ControllerAxisSettings& value) {
				writer.StartObject(); writer.Key("deadzone"); writer.Double(value.deadzone);
				writer.Key("range"); writer.Double(value.range); writer.EndObject();
			};
			writer.StartObject(); writer.Key("generation"); writer.Uint64(model.generation);
			writer.Key("profiles"); writer.StartArray();
			for (const auto& profile : model.profiles) writer.String(profile.data(), profile.size());
			writer.EndArray(); writer.Key("availableApis"); writer.StartArray();
			for (const auto& api : model.availableApis) writer.String(api.data(), api.size());
			writer.EndArray(); writer.Key("players"); writer.StartArray();
			for (const auto& player : model.players)
			{
				writer.StartObject(); writer.Key("player"); writer.Uint(player.player);
				writer.Key("type"); writer.String(typeName(player.type));
				writer.Key("gameProfileLocked"); writer.Bool(player.gameProfileLocked);
				writer.Key("profileName"); writer.String(player.profileName.data(), player.profileName.size());
				writer.Key("controllers"); writer.StartArray();
				for (const auto& controller : player.controllers)
				{
					writer.StartObject(); writer.Key("token"); writer.Uint64(controller.token);
					writer.Key("api"); writer.String(controller.api.data(), controller.api.size());
					writer.Key("displayName"); writer.String(controller.displayName.data(), controller.displayName.size());
					writer.Key("connected"); writer.Bool(controller.connected);
					writer.Key("hasBattery"); writer.Bool(controller.hasBattery);
					writer.Key("lowBattery"); writer.Bool(controller.lowBattery);
					writer.Key("hasMotion"); writer.Bool(controller.hasMotion);
					writer.Key("hasRumble"); writer.Bool(controller.hasRumble);
					if (controller.wiimoteExtension) { writer.Key("wiimoteExtension"); writer.String(controller.wiimoteExtension->data(), controller.wiimoteExtension->size()); }
					writer.Key("settings"); writer.StartObject(); writer.Key("axis"); writeAxis(controller.settings.axis);
					writer.Key("rotation"); writeAxis(controller.settings.rotation); writer.Key("trigger"); writeAxis(controller.settings.trigger);
					writer.Key("rumble"); writer.Double(controller.settings.rumble); writer.Key("motion"); writer.Bool(controller.settings.motion);
					if (controller.settings.packetDelay) { writer.Key("packetDelay"); writer.Uint(*controller.settings.packetDelay); }
					writer.EndObject();
					writer.EndObject();
				}
				writer.EndArray(); writer.Key("mappings"); writer.StartArray();
				for (const auto& mapping : player.mappings)
				{
					writer.StartObject(); writer.Key("mappingId"); writer.Uint64(mapping.mappingId);
					writer.Key("label"); writer.String(mapping.label.data(), mapping.label.size());
					writer.Key("binding"); writer.String(mapping.binding.data(), mapping.binding.size());
					if (mapping.controllerToken) { writer.Key("controllerToken"); writer.Uint64(*mapping.controllerToken); }
					writer.EndObject();
				}
				writer.EndArray(); writer.EndObject();
			}
			writer.EndArray(); writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string InputMutationJson(const Application::InputSettingsResult& result) const
		{
			if (!result)
				throw std::runtime_error(result.diagnostic.empty() ?
					"input settings operation failed" : result.diagnostic);
			return InputSettingsJson();
		}

		std::string HotkeySettingsJson(
			const Application::HotkeySettingsModel& model) const
		{
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartObject();
			writer.Key("revision"); writer.Uint64(model.revision);
			writer.Key("controllerModifier");
			if (model.controllerModifier) writer.Uint(*model.controllerModifier);
			else writer.Null();
			writer.Key("controllerModifierLabel"); writer.String(
				model.controllerModifierLabel.data(),
				static_cast<rapidjson::SizeType>(model.controllerModifierLabel.size()));
			writer.Key("controller");
			if (model.controller)
			{
				writer.StartObject();
				writer.Key("token"); writer.Uint64(model.controller->token);
				writer.Key("displayName"); writer.String(model.controller->displayName.data(),
					static_cast<rapidjson::SizeType>(model.controller->displayName.size()));
				writer.EndObject();
			}
			else writer.Null();
			writer.Key("bindings"); writer.StartArray();
			for (const auto& binding : model.bindings)
			{
				writer.StartObject();
				writer.Key("action"); writer.String(HotkeyActionName(binding.action).data());
				writer.Key("keyboardUsage"); writer.Uint(binding.keyboardUsage);
				writer.Key("keyboardModifiers"); writer.Uint(binding.keyboardModifiers);
				writer.Key("controllerButton");
				if (binding.controllerButton) writer.Uint(*binding.controllerButton);
				else writer.Null();
				writer.Key("controllerLabel"); writer.String(binding.controllerLabel.data(),
					static_cast<rapidjson::SizeType>(binding.controllerLabel.size()));
				writer.EndObject();
			}
			writer.EndArray(); writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string HotkeySettingsResultJson(
			const Application::HotkeySettingsResult& result) const
		{
			return std::string(R"({"ok":)") + (result ? "true" : "false") +
				R"(,"error":)" + JsonString(HotkeySettingsErrorName(result.error)) +
				R"(,"snapshot":)" + HotkeySettingsJson(result.snapshot) +
				R"(,"diagnostic":)" + JsonString(result.diagnostic) + "}";
		}

		static Application::EmulatedControllerType ParseInputControllerType(
			std::string_view value)
		{
			if (value == "disabled") return Application::EmulatedControllerType::Disabled;
			if (value == "gamePad") return Application::EmulatedControllerType::GamePad;
			if (value == "proController") return Application::EmulatedControllerType::ProController;
			if (value == "classicController") return Application::EmulatedControllerType::ClassicController;
			if (value == "wiimote") return Application::EmulatedControllerType::Wiimote;
			throw std::invalid_argument("unknown emulated controller type");
		}

		void RegisterRpc()
		{
			m_rpc.Register("system.bootstrap", [this](const rapidjson::Value&) {
				auto result = std::string(R"({"windowId":)") +
					std::to_string(m_invokingWindow) + R"(,"windowRole":)" +
					JsonString(RoleForWindow(m_invokingWindow)) + R"(,"appVersion":)" +
					JsonString(BUILD_VERSION_STRING) + R"(,"platform":")" +
#if BOOST_OS_WINDOWS
					"windows"
#elif BOOST_OS_MACOS
					"macos"
#else
					"linux"
#endif
					+ "\"";
				if (m_invokingWindow != 0)
				{
					const auto found = m_toolWindows.find(m_invokingWindow);
					if (found != m_toolWindows.end() && found->second->titleContext)
						result += R"(,"context":{"titleId":)" +
							JsonString(TitleIdString(*found->second->titleContext)) + "}";
				}
				result += R"(,"theme":"system","shuttingDown":)";
				result += m_rpc.IsShuttingDown() ? "true}" : "false}";
				if (m_invokingWindow == 0 &&
					!m_controller.GetFrontendSettings().setupCompleted &&
					!m_windowByRole.contains("getting-started") &&
					!m_pendingWindowRoles.contains("getting-started"))
					(void)QueueToolWindow("getting-started", "automatic-getting-started");
				return result;
			});
			m_rpc.Register("system.quit", [this](const rapidjson::Value&) {
				if (m_invokingWindow != 0)
					throw std::runtime_error("only the main window may quit the application");
				if (!RequestShutdown(true))
					throw std::runtime_error("the running title could not be stopped; shutdown was cancelled");
				return std::string("{}");
			});
			m_rpc.Register("window.close", [this](const rapidjson::Value&) {
				if (m_invokingWindow != 0)
				{
					const auto found = m_toolWindows.find(m_invokingWindow);
					if (found == m_toolWindows.end())
						throw std::runtime_error("the window is no longer active");
					found->second->closeRequested = true;
					return std::string("{}");
				}
				if (!RequestShutdown(true))
					throw std::runtime_error("the running title could not be stopped; window close was cancelled");
				return std::string("{}");
			});
			m_rpc.Register("window.open", [this](const rapidjson::Value& params) {
				if (m_invokingWindow != 0)
					throw std::runtime_error("only the main library window may open tool windows");
				const auto role = params.FindMember("role");
				if (role == params.MemberEnd() || !role->value.IsString())
					throw std::invalid_argument("role is required");
				const std::string_view roleName(role->value.GetString(),
					role->value.GetStringLength());
				const auto requestId = RequiredString(params, "requestId");
				if (requestId.empty() || requestId.size() > 128)
					throw std::invalid_argument("requestId must contain between 1 and 128 characters");
				std::optional<std::uint64_t> titleContext;
				if (const auto context = params.FindMember("context"); context != params.MemberEnd())
				{
					if (roleName != "graphic-packs" || !context->value.IsObject())
						throw std::invalid_argument("context is not supported for this window role");
					if (context->value.HasMember("titleId"))
						titleContext = ParseTitleId(context->value);
				}
				const auto id = QueueToolWindow(roleName, std::string(requestId), titleContext);
				return std::string(R"({"windowId":)") + std::to_string(id) + "}";
			});
			m_rpc.Register("window.focus", [this](const rapidjson::Value&) {
				if (m_invokingWindow == 0)
					m_nativeWindow->Show();
				else if (const auto found = m_toolWindows.find(m_invokingWindow);
					found != m_toolWindows.end() && found->second->nativeSupport)
					found->second->nativeSupport->Focus();
				return std::string("{}");
			});
			m_rpc.Register("system.openExternalUrl", [this](const rapidjson::Value& params) {
				const auto url = RequiredString(params, "url");
				constexpr std::array allowedOrigins{
					std::string_view("https://github.com/"),
					std::string_view("https://cemu.info/"),
				};
				if (std::ranges::none_of(allowedOrigins,
					[url](std::string_view origin) { return url.starts_with(origin); }))
					throw std::invalid_argument("the external URL origin is not allowed");
				if (!m_nativeWindow->OpenExternalUrl(std::string(url)))
					throw std::runtime_error("the operating system could not open the URL");
				return std::string("{}");
			});
			m_rpc.Register("about.get", [this](const rapidjson::Value&) {
				RequireRole({"about"});
				return std::string(R"({"name":"CemuExtend","version":)") +
					JsonString(BUILD_VERSION_STRING) + R"(,"commit":)" +
					JsonString(CEMU_EXTEND_COMMIT_HASH) + R"(,"buildDate":)" +
					JsonString(__DATE__ " " __TIME__) +
					R"(,"frontend":"webview-react","webviewEngine":)" +
					JsonString("webview/webview " WEBVIEW_VERSION_NUMBER) +
					R"(,"originalAuthors":["Exzap","Petergov"],"libraries":[)"
					R"({"name":"webview/webview","license":"MIT","url":"https://github.com/webview/webview"},)"
					R"({"name":"React","license":"MIT","url":"https://github.com/facebook/react"},)"
					R"({"name":"Bun","license":"MIT","url":"https://github.com/oven-sh/bun"},)"
					R"({"name":"Vulkan","license":"Apache-2.0","url":"https://github.com/KhronosGroup/Vulkan-Headers"}],"links":[)"
					R"({"label":"CemuExtend source","url":"https://github.com/PinkDiamondTeam/CemuExtend"},)"
					R"({"label":"Cemu project","url":"https://cemu.info/"}]})";
			});
			m_rpc.Register("settings.getFrontend", [this](const rapidjson::Value&) {
				RequireRole({"getting-started", "general-settings"});
				return FrontendSettingsJson(m_controller.GetFrontendSettings());
			});
			m_rpc.Register("settings.applyFrontend", [this](const rapidjson::Value& params) {
				RequireRole({"getting-started", "general-settings"});
				Application::FrontendSettingsUpdate update;
				update.expectedRevision = RequiredUint64(params, "revision");
				update.startFullscreen = RequiredBool(params, "startFullscreen");
				update.openPad = RequiredBool(params, "openPad");
				update.checkUpdates = RequiredBool(params, "checkUpdates");
				update.saveScreenshots = RequiredBool(params, "saveScreenshots");
				update.completeSetup = RequiredBool(params, "completeSetup");
				const auto& paths = RequiredMember(params, "gamePaths");
				if (!paths.IsArray())
					throw std::invalid_argument("gamePaths must be an array");
				for (const auto& path : paths.GetArray())
				{
					if (!path.IsString() || path.GetStringLength() == 0 ||
						path.GetStringLength() > 4096)
						throw std::invalid_argument("each game path must be a non-empty path string");
					update.gamePaths.emplace_back(_utf8ToPath(std::string_view(
						path.GetString(), path.GetStringLength())));
				}
				return FrontendSettingsResultJson(m_controller.ApplyFrontendSettings(update));
			});
			m_rpc.Register("accounts.getModel", [this](const rapidjson::Value&) {
				RequireRole({"account-manager", "general-settings"});
				return AccountManagerJson();
			});
			m_rpc.Register("accounts.create", [this](const rapidjson::Value& params) {
				RequireRole({"account-manager", "general-settings"});
				const auto result = m_controller.CreateAccount(RequiredUint(params, "persistentId"),
					boost::nowide::widen(RequiredString(params, "miiName")));
				if (!result || !result.account)
					throw std::runtime_error(result.diagnostic.empty() ?
						"account creation failed" : result.diagnostic);
				return AccountJson(*result.account);
			});
			m_rpc.Register("accounts.update", [this](const rapidjson::Value& params) {
				RequireRole({"account-manager", "general-settings"});
				Application::AccountUpdate update;
				update.miiName = boost::nowide::widen(RequiredString(params, "miiName"));
				update.birthYear = static_cast<std::uint16_t>(
					RequiredBoundedUint(params, "birthYear", 0, 2100));
				update.birthMonth = static_cast<std::uint8_t>(
					RequiredBoundedUint(params, "birthMonth", 0, 12));
				update.birthDay = static_cast<std::uint8_t>(
					RequiredBoundedUint(params, "birthDay", 0, 31));
				update.gender = static_cast<std::uint8_t>(
					RequiredBoundedUint(params, "gender", 0, 2));
				update.email = RequiredString(params, "email");
				update.country = RequiredUint(params, "country");
				const auto countries = m_controller.ListAccountCountries();
				if (std::ranges::none_of(countries,
					[&update](const Application::AccountCountry& country) {
						return country.code == update.country;
					}))
					throw std::invalid_argument("country is not supported");
				const auto result = m_controller.UpdateAccount(
					RequiredUint(params, "persistentId"), update);
				if (!result || !result.account)
					throw std::runtime_error(result.diagnostic.empty() ?
						"account update failed" : result.diagnostic);
				return AccountJson(*result.account);
			});
			m_rpc.Register("accounts.delete", [this](const rapidjson::Value& params) {
				RequireRole({"account-manager", "general-settings"});
				const auto result = m_controller.DeleteAccount(RequiredUint(params, "persistentId"));
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ?
						"account deletion failed" : result.diagnostic);
				return std::string("{}");
			});
			m_rpc.Register("accounts.setActive", [this](const rapidjson::Value& params) {
				RequireRole({"account-manager", "general-settings"});
				const auto result = m_controller.SetActiveAccount(
					RequiredUint(params, "persistentId"));
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ?
						"active account update failed" : result.diagnostic);
				return std::string("{}");
			});
			m_rpc.Register("accounts.setNetworkService", [this](const rapidjson::Value& params) {
				RequireRole({"account-manager", "general-settings"});
				const auto result = m_controller.SetAccountNetworkService(
					RequiredUint(params, "persistentId"),
					ParseAccountNetworkService(RequiredString(params, "service")));
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ?
						"network service update failed" : result.diagnostic);
				return std::string("{}");
			});
			m_rpc.Register("input.getModel", [this](const rapidjson::Value&) {
				RequireRole({"input-settings"}); return InputSettingsJson();
			});
			m_rpc.Register("input.enumerate", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"});
				const auto result = m_controller.EnumerateInputDevices(RequiredString(params, "api"));
				if (!result) throw std::runtime_error(result.diagnostic.empty() ? "input device enumeration failed" : result.diagnostic);
				rapidjson::StringBuffer buffer; JsonWriter writer(buffer); writer.StartArray();
				for (const auto& device : result.devices) { writer.StartObject(); writer.Key("token"); writer.Uint64(device.token); writer.Key("api"); writer.String(device.api.data(), device.api.size()); writer.Key("displayName"); writer.String(device.displayName.data(), device.displayName.size()); writer.Key("connected"); writer.Bool(device.connected); writer.EndObject(); }
				writer.EndArray(); return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("input.setType", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"}); return InputMutationJson(m_controller.SetEmulatedController(RequiredUint(params, "player"), ParseInputControllerType(RequiredString(params, "type")), RequiredBool(params, "preserveDevices")));
			});
			m_rpc.Register("input.addDevice", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"}); return InputMutationJson(m_controller.AddInputDevice(RequiredUint(params, "player"), RequiredUint64(params, "token")));
			});
			m_rpc.Register("input.removeDevice", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"}); return InputMutationJson(m_controller.RemoveInputDevice(RequiredUint(params, "player"), RequiredUint64(params, "token")));
			});
			m_rpc.Register("input.connectDevice", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"}); return InputMutationJson(m_controller.ConnectInputDevice(RequiredUint64(params, "token")));
			});
			m_rpc.Register("input.captureButton", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings", "hotkey-settings"});
				const auto captured = m_controller.CaptureInputButton(RequiredUint64(params, "token"));
				if (!captured) return std::string("null");
				return std::string(R"({"id":)") + std::to_string(captured->id) +
					R"(,"label":)" + JsonString(captured->label) + "}";
			});
			m_rpc.Register("input.setMapping", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"}); return InputMutationJson(m_controller.SetInputMapping(RequiredUint(params, "player"), RequiredUint64(params, "mappingId"), RequiredUint64(params, "controllerToken"), RequiredUint64(params, "buttonId")));
			});
			m_rpc.Register("input.clearMapping", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"}); std::optional<std::uint64_t> mapping;
				if (params.IsObject()) if (const auto found = params.FindMember("mappingId"); found != params.MemberEnd()) { if (!found->value.IsUint64()) throw std::invalid_argument("mappingId must be an unsigned integer"); mapping = found->value.GetUint64(); }
				return InputMutationJson(m_controller.ClearInputMapping(RequiredUint(params, "player"), mapping));
			});
			m_rpc.Register("input.setDeviceSettings", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"}); const auto& value = RequiredMember(params, "settings");
				auto axis = [](const rapidjson::Value& object) { return Application::ControllerAxisSettings{static_cast<float>(RequiredDouble(object, "deadzone")), static_cast<float>(RequiredDouble(object, "range"))}; };
				Application::PhysicalControllerSettings settings{axis(RequiredMember(value, "axis")), axis(RequiredMember(value, "rotation")), axis(RequiredMember(value, "trigger")), static_cast<float>(RequiredDouble(value, "rumble")), RequiredBool(value, "motion")};
				if (const auto found = value.FindMember("packetDelay"); found != value.MemberEnd()) { if (!found->value.IsUint()) throw std::invalid_argument("packetDelay must be an unsigned integer"); settings.packetDelay = found->value.GetUint(); }
				return InputMutationJson(m_controller.SetPhysicalControllerSettings(RequiredUint64(params, "token"), settings));
			});
			m_rpc.Register("input.calibrate", [this](const rapidjson::Value& params) { RequireRole({"input-settings"}); return InputMutationJson(m_controller.CalibrateInputDevice(RequiredUint64(params, "token"))); });
			m_rpc.Register("input.profileLoad", [this](const rapidjson::Value& params) { RequireRole({"input-settings"}); return InputMutationJson(m_controller.LoadInputProfile(RequiredUint(params, "player"), RequiredString(params, "profile"))); });
			m_rpc.Register("input.profileSave", [this](const rapidjson::Value& params) { RequireRole({"input-settings"}); return InputMutationJson(m_controller.SaveInputProfile(RequiredUint(params, "player"), RequiredString(params, "profile"))); });
			m_rpc.Register("input.profileDelete", [this](const rapidjson::Value& params) { RequireRole({"input-settings"}); return InputMutationJson(m_controller.DeleteInputProfile(RequiredString(params, "profile"))); });
			m_rpc.Register("hotkeys.get", [this](const rapidjson::Value&) {
				RequireRole({"hotkey-settings"});
				return HotkeySettingsJson(m_controller.GetHotkeySettings());
			});
			m_rpc.Register("hotkeys.apply", [this](const rapidjson::Value& params) {
				RequireRole({"hotkey-settings"});
				Application::HotkeySettingsUpdate update;
				update.revision = RequiredUint64(params, "revision");
				const auto& modifier = RequiredMember(params, "controllerModifier");
				if (!modifier.IsNull())
				{
					if (!modifier.IsUint())
						throw std::invalid_argument("controllerModifier must be null or an unsigned integer");
					update.controllerModifier = modifier.GetUint();
				}
				const auto& bindings = RequiredMember(params, "bindings");
				if (!bindings.IsArray())
					throw std::invalid_argument("bindings must be an array");
				for (const auto& value : bindings.GetArray())
				{
					Application::HotkeyBinding binding;
					binding.action = ParseHotkeyAction(RequiredString(value, "action"));
					binding.keyboardUsage = static_cast<std::uint16_t>(
						RequiredBoundedUint(value, "keyboardUsage", 0, 0xffff));
					binding.keyboardModifiers = static_cast<std::uint8_t>(
						RequiredBoundedUint(value, "keyboardModifiers", 0, 0x0f));
					const auto& button = RequiredMember(value, "controllerButton");
					if (!button.IsNull())
					{
						if (!button.IsUint())
							throw std::invalid_argument("controllerButton must be null or an unsigned integer");
						binding.controllerButton = button.GetUint();
					}
					update.bindings.push_back(binding);
				}
				const auto result = m_controller.ApplyHotkeySettings(update);
				if (result) RefreshHotkeyBindings(result.snapshot);
				return HotkeySettingsResultJson(result);
			});
			m_rpc.Register("graphicPacks.list", [this](const rapidjson::Value&) {
				RequireRole({"graphic-packs"});
				return GraphicPacksJson();
			});
			m_rpc.Register("graphicPacks.setEnabled", [this](const rapidjson::Value& params) {
				RequireRole({"graphic-packs"});
				const auto result = m_controller.SetGraphicPackEnabled(
					RequiredString(params, "key"), RequiredBool(params, "enabled"));
				if (result) m_controller.SaveGraphicPackState();
				return GraphicPackMutationJson(result);
			});
			m_rpc.Register("graphicPacks.setPreset", [this](const rapidjson::Value& params) {
				RequireRole({"graphic-packs"});
				const auto result = m_controller.SetGraphicPackPreset(
					RequiredString(params, "key"), RequiredString(params, "category"),
					RequiredString(params, "preset"));
				if (result) m_controller.SaveGraphicPackState();
				return GraphicPackMutationJson(result);
			});
			m_rpc.Register("graphicPacks.reload", [this](const rapidjson::Value& params) {
				RequireRole({"graphic-packs"});
				return GraphicPackMutationJson(
					m_controller.ReloadGraphicPack(RequiredString(params, "key")));
			});
			m_rpc.Register("graphicPacks.refresh", [this](const rapidjson::Value&) {
				RequireRole({"graphic-packs"});
				const auto result = m_controller.RefreshGraphicPacks();
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ?
						"graphic pack refresh failed" : result.diagnostic);
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartObject();
				writer.Key("removedEnabledPaths"); writer.StartArray();
				for (const auto& path : result.removedEnabledPaths)
					writer.String(path.data(), static_cast<rapidjson::SizeType>(path.size()));
				writer.EndArray();
				writer.Key("diagnostic"); writer.String(result.diagnostic.data(),
					static_cast<rapidjson::SizeType>(result.diagnostic.size()));
				writer.EndObject();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("graphicPacks.save", [this](const rapidjson::Value&) {
				RequireRole({"graphic-packs"});
				m_controller.SaveGraphicPackState();
				return std::string("{}");
			});
			m_rpc.Register("graphicPacks.install", [this](const rapidjson::Value& params) {
				RequireRole({"graphic-packs"});
				Application::GraphicPackInstallRequest request;
				const auto kind = RequiredString(params, "kind");
				if (kind == "community")
					request.kind = Application::GraphicPackInstallKind::Community;
				else if (kind == "customUrl")
				{
					request.kind = Application::GraphicPackInstallKind::CustomUrl;
					request.url = RequiredString(params, "url");
				}
				else
					throw std::invalid_argument("unknown graphic-pack install kind");
				request.replaceExisting = RequiredBool(params, "replaceExisting");
				const auto jobId = StartGraphicPackInstallJob(std::move(request));
				return std::string(R"({"jobId":)") + std::to_string(jobId) + "}";
			});
			m_rpc.Register("jobs.cancel", [this](const rapidjson::Value& params) {
				const auto jobId = static_cast<std::uint64_t>(RequiredUint(params, "jobId"));
				const auto job = m_backgroundJobs.find(jobId);
				if (job == m_backgroundJobs.end() || job->second->ownerWindow != m_invokingWindow)
					throw std::runtime_error("background job was not found for this window");
				job->second->cancelled->store(true, std::memory_order_release);
				job->second->worker.request_stop();
				return std::string("{}");
			});
			m_rpc.Register("titles.list", [this](const rapidjson::Value&) {
				rapidjson::Document document(rapidjson::kArrayType);
				auto& allocator = document.GetAllocator();
				for (const auto& game : m_controller.ListGames())
				{
					rapidjson::Value item(rapidjson::kObjectType);
					const auto titleId = TitleIdString(game.titleId);
					item.AddMember("titleId", rapidjson::Value(titleId.c_str(), allocator), allocator);
					item.AddMember("name", rapidjson::Value(game.name.c_str(), allocator), allocator);
					const auto path = _pathToUtf8(game.basePath);
					item.AddMember("path", rapidjson::Value(path.c_str(), allocator), allocator);
					item.AddMember("region", rapidjson::Value(game.regionName.c_str(), allocator), allocator);
					item.AddMember("version", game.version, allocator);
					item.AddMember("playTimeMinutes", game.playStats.minutesPlayed, allocator);
					if (game.playStats.available)
					{
						std::array<char, 11> date{};
						std::snprintf(date.data(), date.size(), "%04u-%02u-%02u",
							game.playStats.lastPlayedYear, game.playStats.lastPlayedMonth,
							game.playStats.lastPlayedDay);
						item.AddMember("lastPlayed", rapidjson::Value(date.data(), allocator), allocator);
					}
					else
						item.AddMember("lastPlayed", rapidjson::Value(rapidjson::kNullType), allocator);
					document.PushBack(item, allocator);
				}
				rapidjson::StringBuffer buffer;
				rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
				document.Accept(writer);
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("titles.refresh", [this](const rapidjson::Value&) {
				m_controller.RefreshTitles();
				return "{}";
			});
			m_rpc.Register("titles.launch", [this](const rapidjson::Value& params) {
				const auto game = m_controller.GetGame(ParseTitleId(params));
				if (!game)
					throw std::invalid_argument("titleId is not present in the game library");
				return Launch(game->basePath);
			});
			m_rpc.Register("emulation.stop", [this](const rapidjson::Value&) {
				const auto result = m_controller.Stop();
				if (!result.stopped)
					throw std::runtime_error(result.diagnostic);
				if (!DestroyMainRenderRegion())
					throw std::runtime_error("native renderer surfaces could not be detached safely");
				m_nativeWindow->ShowLibrary();
				(void)m_windowState->FinishEmulation();
				return "{}";
			});
			m_rpc.VerifyMethods(WebFrontend::Generated::RpcMethods);
		}

		std::string Launch(const fs::path& path)
		{
			if (!m_windowState->BeginLaunch())
				throw std::runtime_error("main window is not ready to launch a title");
			const auto frontendSettings = m_controller.GetFrontendSettings();
			const bool launchFullscreen = frontendSettings.fullscreenOverride.value_or(
				frontendSettings.startFullscreen);
			const bool previousFullscreen = m_fullscreen;
			const auto result = m_controller.Launch({path},
				[this, launchFullscreen](const Application::LaunchResult&) {
					if (m_fullscreen != launchFullscreen)
					{
						m_fullscreen = launchFullscreen;
						m_nativeWindow->SetFullscreen(m_fullscreen);
					}
					auto& region = m_nativeWindow->CreateMainRenderRegion();
					m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
					m_rendererHost->InitializeMain(region);
					if (!m_windowState->CommitLaunch())
						throw std::runtime_error("main window content transition failed");
					m_nativeWindow->ShowRenderRegion();
				},
				[this, previousFullscreen] {
					m_rendererHost->AbandonMainInitialization();
					DestroyMainRenderRegion();
					m_nativeWindow->ShowLibrary();
					(void)m_windowState->RollbackLaunch();
					if (m_fullscreen != previousFullscreen)
					{
						m_fullscreen = previousFullscreen;
						m_nativeWindow->SetFullscreen(previousFullscreen);
					}
				});
			if (!result)
			{
				if (m_controller.State() == Application::EmulationState::Running)
				{
					(void)m_nativeWindow->CreateMainRenderRegion();
					(void)m_windowState->CommitLaunch();
					m_nativeWindow->ShowRenderRegion();
				}
				else
				{
					DestroyMainRenderRegion();
					m_nativeWindow->ShowLibrary();
					(void)m_windowState->RollbackLaunch();
				}
				throw std::runtime_error(result.diagnostic.empty() ? "title launch failed" : result.diagnostic);
			}
			if (frontendSettings.openPad && !m_nativeWindow->IsPadRenderRegionOpen())
				TogglePadRenderRegion();
			return std::string(R"({"titleId":")") + TitleIdString(result.titleId) + "\"}";
		}

		std::unique_ptr<INativeWindowHost> m_nativeWindow;
		webview_t m_webview{};
		void* m_webViewWidget{};
		RpcBinding m_mainBinding;
		RpcDispatcher m_rpc;
		std::unordered_map<std::uint64_t, std::unique_ptr<ToolWindow>> m_toolWindows;
		std::unordered_map<std::string, std::uint64_t> m_windowByRole;
		std::unordered_map<std::string, std::uint64_t> m_pendingWindowRoles;
		std::unordered_map<std::string, std::vector<std::string>> m_pendingWindowRequests;
		std::unordered_map<std::string, std::optional<std::uint64_t>> m_pendingWindowContexts;
		std::unordered_map<std::uint64_t, std::unique_ptr<BackgroundJob>> m_backgroundJobs;
		std::uint64_t m_nextWindowId{};
		std::uint64_t m_nextBackgroundJobId{};
		std::uint64_t m_invokingWindow{};
		std::shared_ptr<WebHostState> m_hostState{std::make_shared<WebHostState>()};
		std::shared_ptr<WebHostServices> m_hostServices;
		std::unique_ptr<IRendererHost> m_rendererHost;
		Application::EmulationController m_controller;
		std::unique_ptr<MainWindowState> m_windowState;
		std::shared_ptr<RuntimeCallbackGate> m_callbackGate{std::make_shared<RuntimeCallbackGate>()};
		Application::EventSubscription m_applicationEvents;
		Application::TitleCatalogSubscription m_titleEvents;
		std::atomic_bool m_stopping{};
		std::shared_ptr<std::atomic_bool> m_eventStopping{std::make_shared<std::atomic_bool>()};
		std::mutex m_eventMutex;
		std::uint64_t m_eventSequence{};
		std::uint64_t m_padGeneration{};
		struct PointerState
		{
			std::int32_t x{};
			std::int32_t y{};
			std::int32_t width{};
			std::int32_t height{};
			bool inside{};
		};
		std::array<PointerState, 2> m_pointerStates;
		std::array<Frontend::CemuExtendFrontendBridge, 2> m_pointerBridges;
		mutable std::mutex m_hotkeyMutex;
		Application::HotkeySettingsModel m_hotkeySettings;
		std::atomic_bool m_hotkeyEditing{};
		std::uint64_t m_textInputSequence{};
		bool m_fullscreen{};
		bool m_rpcBound{};
		bool m_cleanedUp{};
		bool m_applicationShutdown{};
		bool m_hostConnected{};
		bool m_terminateWhenToolsClosed{};
		bool m_mainReplyPending{};
		Host::NativeSurfacePublication m_mainWindowPublication{};
	};
}

void Frontend::Run()
{
	CemuCommonInit();
#if BOOST_OS_WINDOWS
	std::exception_ptr uiFailure;
	std::thread uiThread([&uiFailure] {
		SetThreadName("cemu-web-ui");
		const auto initialized = CoInitializeEx(nullptr,
			COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		if (FAILED(initialized))
		{
			uiFailure = std::make_exception_ptr(
				std::runtime_error("failed to initialize the webview STA UI thread"));
			return;
		}
		try
		{
			Runtime runtime;
			runtime.Run();
		}
		catch (...)
		{
			uiFailure = std::current_exception();
		}
		CoUninitialize();
	});
	uiThread.join();
	if (uiFailure)
		std::rethrow_exception(uiFailure);
#else
	SetThreadName("cemu-web-ui");
	Runtime runtime;
	runtime.Run();
#endif
}
