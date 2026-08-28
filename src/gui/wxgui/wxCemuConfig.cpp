#include "wxCemuConfig.h"
#include "Common/precompiled.h"
#include "config/CemuConfig.h"
#include "config/XMLConfig.h"
#include "util/helpers/helpers.h"
#include <wx/language.h>

XMLWxCemuConfig_t g_wxConfig(&GetConfigHandle);

namespace
{
	uint16 WxKeyToHid(int key)
	{
		if (key >= 'A' && key <= 'Z')
			return static_cast<uint16>(0x04 + key - 'A');
		if (key >= 'a' && key <= 'z')
			return static_cast<uint16>(0x04 + key - 'a');
		if (key >= '1' && key <= '9')
			return static_cast<uint16>(0x1e + key - '1');
		if (key == '0')
			return 0x27;
		if (key >= WXK_F1 && key <= WXK_F12)
			return static_cast<uint16>(0x3a + key - WXK_F1);
		if (key >= WXK_F13 && key <= WXK_F24)
			return static_cast<uint16>(0x68 + key - WXK_F13);
		switch (key)
		{
		case WXK_RETURN:
			return 0x28;
		case WXK_ESCAPE:
			return 0x29;
		case WXK_BACK:
			return 0x2a;
		case WXK_TAB:
			return 0x2b;
		case WXK_SPACE:
			return 0x2c;
		case '-':
			return 0x2d;
		case '=':
			return 0x2e;
		case '[':
			return 0x2f;
		case ']':
			return 0x30;
		case '\\':
			return 0x31;
		case ';':
			return 0x33;
		case '\'':
			return 0x34;
		case '`':
			return 0x35;
		case ',':
			return 0x36;
		case '.':
			return 0x37;
		case '/':
			return 0x38;
		case WXK_CAPITAL:
			return 0x39;
		case WXK_PRINT:
		case WXK_SNAPSHOT:
			return 0x46;
		case WXK_SCROLL:
			return 0x47;
		case WXK_PAUSE:
			return 0x48;
		case WXK_INSERT:
			return 0x49;
		case WXK_HOME:
			return 0x4a;
		case WXK_PAGEUP:
			return 0x4b;
		case WXK_DELETE:
			return 0x4c;
		case WXK_END:
			return 0x4d;
		case WXK_PAGEDOWN:
			return 0x4e;
		case WXK_RIGHT:
			return 0x4f;
		case WXK_LEFT:
			return 0x50;
		case WXK_DOWN:
			return 0x51;
		case WXK_UP:
			return 0x52;
		case WXK_NUMLOCK:
			return 0x53;
		case WXK_DIVIDE:
		case WXK_NUMPAD_DIVIDE:
			return 0x54;
		case WXK_MULTIPLY:
		case WXK_NUMPAD_MULTIPLY:
			return 0x55;
		case WXK_SUBTRACT:
		case WXK_NUMPAD_SUBTRACT:
			return 0x56;
		case WXK_ADD:
		case WXK_NUMPAD_ADD:
			return 0x57;
		case WXK_NUMPAD_ENTER:
			return 0x58;
		case WXK_NUMPAD_SPACE:
			return 0x2c;
		case WXK_NUMPAD_TAB:
			return 0x2b;
		case WXK_NUMPAD_F1:
			return 0x3a;
		case WXK_NUMPAD_F2:
			return 0x3b;
		case WXK_NUMPAD_F3:
			return 0x3c;
		case WXK_NUMPAD_F4:
			return 0x3d;
		case WXK_NUMPAD1:
			return 0x59;
		case WXK_NUMPAD2:
			return 0x5a;
		case WXK_NUMPAD3:
			return 0x5b;
		case WXK_NUMPAD4:
			return 0x5c;
		case WXK_NUMPAD5:
			return 0x5d;
		case WXK_NUMPAD6:
			return 0x5e;
		case WXK_NUMPAD7:
			return 0x5f;
		case WXK_NUMPAD8:
			return 0x60;
		case WXK_NUMPAD9:
			return 0x61;
		case WXK_NUMPAD0:
			return 0x62;
		case WXK_DECIMAL:
		case WXK_NUMPAD_DECIMAL:
			return 0x63;
		case WXK_NUMPAD_EQUAL:
			return 0x67;
		case WXK_SEPARATOR:
		case WXK_NUMPAD_SEPARATOR:
			return 0x85;
		case WXK_EXECUTE:
			return 0x74;
		case WXK_HELP:
			return 0x75;
		case WXK_SELECT:
			return 0x77;
		case WXK_CANCEL:
			return 0x9b;
		case WXK_CLEAR:
			return 0x9c;
		case WXK_MENU:
		case WXK_WINDOWS_MENU:
			return 0x65;
		default:
			return 0;
		}
	}

	int HidToWxKey(uint16 usage)
	{
		if (usage >= 0x04 && usage <= 0x1d)
			return 'A' + usage - 0x04;
		if (usage >= 0x1e && usage <= 0x26)
			return '1' + usage - 0x1e;
		if (usage == 0x27)
			return '0';
		if (usage >= 0x3a && usage <= 0x45)
			return WXK_F1 + usage - 0x3a;
		if (usage >= 0x68 && usage <= 0x73)
			return WXK_F13 + usage - 0x68;
		switch (usage)
		{
		case 0x28:
			return WXK_RETURN;
		case 0x29:
			return WXK_ESCAPE;
		case 0x2a:
			return WXK_BACK;
		case 0x2b:
			return WXK_TAB;
		case 0x2c:
			return WXK_SPACE;
		case 0x2d:
			return '-';
		case 0x2e:
			return '=';
		case 0x2f:
			return '[';
		case 0x30:
			return ']';
		case 0x31:
			return '\\';
		case 0x33:
			return ';';
		case 0x34:
			return '\'';
		case 0x35:
			return '`';
		case 0x36:
			return ',';
		case 0x37:
			return '.';
		case 0x38:
			return '/';
		case 0x39:
			return WXK_CAPITAL;
		case 0x46:
			return WXK_PRINT;
		case 0x47:
			return WXK_SCROLL;
		case 0x48:
			return WXK_PAUSE;
		case 0x49:
			return WXK_INSERT;
		case 0x4a:
			return WXK_HOME;
		case 0x4b:
			return WXK_PAGEUP;
		case 0x4c:
			return WXK_DELETE;
		case 0x4d:
			return WXK_END;
		case 0x4e:
			return WXK_PAGEDOWN;
		case 0x4f:
			return WXK_RIGHT;
		case 0x50:
			return WXK_LEFT;
		case 0x51:
			return WXK_DOWN;
		case 0x52:
			return WXK_UP;
		case 0x53:
			return WXK_NUMLOCK;
		case 0x54:
			return WXK_NUMPAD_DIVIDE;
		case 0x55:
			return WXK_NUMPAD_MULTIPLY;
		case 0x56:
			return WXK_NUMPAD_SUBTRACT;
		case 0x57:
			return WXK_NUMPAD_ADD;
		case 0x58:
			return WXK_NUMPAD_ENTER;
		case 0x59:
			return WXK_NUMPAD1;
		case 0x5a:
			return WXK_NUMPAD2;
		case 0x5b:
			return WXK_NUMPAD3;
		case 0x5c:
			return WXK_NUMPAD4;
		case 0x5d:
			return WXK_NUMPAD5;
		case 0x5e:
			return WXK_NUMPAD6;
		case 0x5f:
			return WXK_NUMPAD7;
		case 0x60:
			return WXK_NUMPAD8;
		case 0x61:
			return WXK_NUMPAD9;
		case 0x62:
			return WXK_NUMPAD0;
		case 0x63:
			return WXK_NUMPAD_DECIMAL;
		case 0x65:
			return WXK_MENU;
		default:
			return WXK_NONE;
		}
	}

	sHotkeyCfg FromFrontend(const FrontendHotkeyBindingConfig& binding)
	{
		uKeyboardHotkey keyboard{};
		keyboard.key = HidToWxKey(binding.keyboard_usage);
		keyboard.ctrl = (binding.keyboard_modifiers & 1) != 0;
		keyboard.shift = (binding.keyboard_modifiers & 2) != 0;
		keyboard.alt = (binding.keyboard_modifiers & 4) != 0;
		return {keyboard, binding.controller_button};
	}

	FrontendHotkeyBindingConfig ToFrontend(const sHotkeyCfg& binding)
	{
		return {WxKeyToHid(binding.keyboard.key), static_cast<std::uint8_t>((binding.keyboard.ctrl ? 1U : 0U) | (binding.keyboard.shift ? 2U : 0U) | (binding.keyboard.alt ? 4U : 0U)), binding.controller};
	}
} // namespace

void wxCemuConfig::AddRecentlyLaunchedFile(std::string_view file)
{
	recent_launch_files.insert(recent_launch_files.begin(), std::string(file));
	RemoveDuplicatesKeepOrder(recent_launch_files);
	while (recent_launch_files.size() > kMaxRecentEntries)
		recent_launch_files.pop_back();
}

void wxCemuConfig::AddRecentNfcFile(std::string_view file)
{
	recent_nfc_files.insert(recent_nfc_files.begin(), std::string(file));
	RemoveDuplicatesKeepOrder(recent_nfc_files);
	while (recent_nfc_files.size() > kMaxRecentEntries)
		recent_nfc_files.pop_back();
}

void wxCemuConfig::Load(XMLConfigParser& parser)
{
	language = parser.get<sint32>("language", wxLANGUAGE_DEFAULT);
	msw_theme = parser.get<sint32>("msw_theme", msw_theme);
	use_discord_presence = parser.get("use_discord_presence", true);
	fullscreen_menubar = parser.get("fullscreen_menubar", false);
	feral_gamemode = parser.get("feral_gamemode", false);
	check_update = GetConfig().frontend.check_updates.GetValue();
	receive_untested_updates = parser.get("receive_untested_updates", receive_untested_updates);
	save_screenshot = GetConfig().frontend.save_screenshots.GetValue();
	did_show_vulkan_warning = parser.get("vk_warning", did_show_vulkan_warning);
	did_show_graphic_pack_download = parser.get("gp_download", did_show_graphic_pack_download);
	did_show_macos_disclaimer = parser.get("macos_disclaimer", did_show_macos_disclaimer);
	fullscreen = GetConfig().frontend.start_fullscreen.GetValue();

	window_position.x = parser.get("window_position").get("x", -1);
	window_position.y = parser.get("window_position").get("y", -1);

	window_size.x = parser.get("window_size").get("x", -1);
	window_size.y = parser.get("window_size").get("y", -1);
	window_maximized = parser.get("window_maximized", false);

	pad_open = GetConfig().frontend.open_pad.GetValue();
	pad_position.x = parser.get("pad_position").get("x", -1);
	pad_position.y = parser.get("pad_position").get("y", -1);

	pad_size.x = parser.get("pad_size").get("x", -1);
	pad_size.y = parser.get("pad_size").get("y", -1);
	pad_maximized = parser.get("pad_maximized", false);

	auto gamelist = parser.get("GameList");
	game_list_style = gamelist.get("style", 0);
	game_list_column_order = gamelist.get("order", "");

	show_icon_column = parser.get("show_icon_column", true);

	// return default width if value in config file out of range
	auto loadColumnSize = [&gamelist](const char* name, uint32 defaultWidth) {
		sint64 val = gamelist.get(name, DefaultColumnSize::name);
		if (val < 0 || val > (sint64)std::numeric_limits<uint32>::max)
			return defaultWidth;
		return static_cast<uint32>(val);
	};
	column_width.name = loadColumnSize("name_width", DefaultColumnSize::name);
	column_width.version = loadColumnSize("version_width", DefaultColumnSize::version);
	column_width.dlc = loadColumnSize("dlc_width", DefaultColumnSize::dlc);
	column_width.game_time = loadColumnSize("game_time_width", DefaultColumnSize::game_time);
	column_width.game_started = loadColumnSize("game_started_width", DefaultColumnSize::game_started);
	column_width.region = loadColumnSize("region_width", DefaultColumnSize::region);
	column_width.title_id = loadColumnSize("title_id", DefaultColumnSize::title_id);

	recent_launch_files.clear();
	auto launch_parser = parser.get("RecentLaunchFiles");
	for (auto element = launch_parser.get("Entry"); element.valid(); element = launch_parser.get("Entry", element))
	{
		const std::string path = element.value("");
		if (path.empty())
			continue;

		try
		{
			recent_launch_files.emplace_back(path);
		} catch (const std::exception&)
		{
			cemuLog_log(LogType::Force, "config load error: can't load recently launched game file: {}", path);
		}
	}

	recent_nfc_files.clear();
	auto nfc_parser = parser.get("RecentNFCFiles");
	for (auto element = nfc_parser.get("Entry"); element.valid(); element = nfc_parser.get("Entry", element))
	{
		const std::string path = element.value("");
		if (path.empty())
			continue;
		try
		{
			recent_nfc_files.emplace_back(path);
		} catch (const std::exception&)
		{
			cemuLog_log(LogType::Force, "config load error: can't load recently launched nfc file: {}", path);
		}
	}

	// hotkeys
	auto xml_hotkeys = parser.get("Hotkeys");
	hotkeys.modifiers = xml_hotkeys.get("modifiers", sHotkeyCfg{});
	hotkeys.exitFullscreen = xml_hotkeys.get("ExitFullscreen", sHotkeyCfg{uKeyboardHotkey{WXK_ESCAPE}});
	hotkeys.toggleFullscreen = xml_hotkeys.get("ToggleFullscreen", sHotkeyCfg{uKeyboardHotkey{WXK_F11}});
	hotkeys.toggleFullscreenAlt = xml_hotkeys.get("ToggleFullscreenAlt", sHotkeyCfg{uKeyboardHotkey{WXK_CONTROL_M, true}}); // ALT+ENTER
	hotkeys.takeScreenshot = xml_hotkeys.get("TakeScreenshot", sHotkeyCfg{uKeyboardHotkey{WXK_F12}});
	hotkeys.toggleFastForward = xml_hotkeys.get("ToggleFastForward", sHotkeyCfg{});
	hotkeys.exitApplication = xml_hotkeys.get("ExitApplication", sHotkeyCfg{});
#ifdef CEMU_DEBUG_ASSERT
	hotkeys.endEmulation = xml_hotkeys.get("EndEmulation", sHotkeyCfg{uKeyboardHotkey{WXK_F5}});
#endif
	const auto& frontend = GetConfig().frontend.hotkeys;
	hotkeys.modifiers.controller = frontend.controller_modifier;
	hotkeys.toggleFullscreen = FromFrontend(frontend.toggle_fullscreen);
	hotkeys.toggleFullscreenAlt = FromFrontend(frontend.toggle_fullscreen_alternative);
	hotkeys.exitFullscreen = FromFrontend(frontend.exit_fullscreen);
	hotkeys.takeScreenshot = FromFrontend(frontend.take_screenshot);
	hotkeys.toggleFastForward = FromFrontend(frontend.toggle_fast_forward);
	hotkeys.exitApplication = FromFrontend(frontend.exit_application);
#ifdef CEMU_DEBUG_ASSERT
	hotkeys.endEmulation = FromFrontend(frontend.end_emulation);
#endif
}

void SyncWxHotkeysToFrontend()
{
	auto& frontend = GetConfig().frontend.hotkeys;
	const auto& hotkeys = GetWxGUIConfig().hotkeys;
	frontend.controller_modifier = hotkeys.modifiers.controller;
	frontend.toggle_fullscreen = ToFrontend(hotkeys.toggleFullscreen);
	frontend.toggle_fullscreen_alternative = ToFrontend(hotkeys.toggleFullscreenAlt);
	frontend.exit_fullscreen = ToFrontend(hotkeys.exitFullscreen);
	frontend.take_screenshot = ToFrontend(hotkeys.takeScreenshot);
	frontend.toggle_fast_forward = ToFrontend(hotkeys.toggleFastForward);
	frontend.exit_application = ToFrontend(hotkeys.exitApplication);
#ifdef CEMU_DEBUG_ASSERT
	frontend.end_emulation = ToFrontend(hotkeys.endEmulation);
#endif
}

void SyncWxFrontendSettingsToNeutral()
{
	auto& frontend = GetConfig().frontend;
	const auto& wx = GetWxGUIConfig();
	frontend.start_fullscreen = wx.fullscreen.GetValue();
	frontend.open_pad = wx.pad_open.GetValue();
	frontend.check_updates = wx.check_update.GetValue();
	frontend.save_screenshots = wx.save_screenshot.GetValue();
	SyncWxHotkeysToFrontend();
}

void wxCemuConfig::Save(XMLConfigParser& config)
{
	// general settings
	config.set<sint32>("language", language);
	config.set<sint32>("msw_theme", msw_theme);
	config.set<bool>("use_discord_presence", use_discord_presence);
	config.set<bool>("fullscreen_menubar", fullscreen_menubar);
	config.set<bool>("feral_gamemode", feral_gamemode);
	config.set<bool>("check_update", check_update);
	config.set<bool>("receive_untested_updates", receive_untested_updates);
	config.set<bool>("save_screenshot", save_screenshot);
	config.set<bool>("vk_warning", did_show_vulkan_warning);
	config.set<bool>("gp_download", did_show_graphic_pack_download);
	config.set<bool>("macos_disclaimer", did_show_macos_disclaimer);
	config.set<bool>("fullscreen", fullscreen);

	auto wpos = config.set("window_position");
	wpos.set<sint32>("x", window_position.x);
	wpos.set<sint32>("y", window_position.y);
	auto wsize = config.set("window_size");
	wsize.set<sint32>("x", window_size.x);
	wsize.set<sint32>("y", window_size.y);
	config.set<bool>("window_maximized", window_maximized);

	config.set<bool>("open_pad", pad_open);
	auto ppos = config.set("pad_position");
	ppos.set<sint32>("x", pad_position.x);
	ppos.set<sint32>("y", pad_position.y);
	auto psize = config.set("pad_size");
	psize.set<sint32>("x", pad_size.x);
	psize.set<sint32>("y", pad_size.y);
	config.set<bool>("pad_maximized", pad_maximized);
	config.set<bool>("show_icon_column", show_icon_column);

	auto gamelist = config.set("GameList");
	gamelist.set("style", game_list_style);
	gamelist.set("order", game_list_column_order);
	gamelist.set("name_width", column_width.name);
	gamelist.set("version_width", column_width.version);
	gamelist.set("dlc_width", column_width.dlc);
	gamelist.set("game_time_width", column_width.game_time);
	gamelist.set("game_started_width", column_width.game_started);
	gamelist.set("region_width", column_width.region);
	gamelist.set("title_id", column_width.title_id);

	auto launch_files_parser = config.set("RecentLaunchFiles");
	for (const auto& entry : recent_launch_files)
	{
		launch_files_parser.set("Entry", entry.c_str());
	}

	auto nfc_files_parser = config.set("RecentNFCFiles");
	for (const auto& entry : recent_nfc_files)
	{
		nfc_files_parser.set("Entry", entry.c_str());
	}

	// hotkeys
	auto xml_hotkeys = config.set("Hotkeys");
	xml_hotkeys.set("modifiers", hotkeys.modifiers);
	xml_hotkeys.set("ExitFullscreen", hotkeys.exitFullscreen);
	xml_hotkeys.set("ToggleFullscreen", hotkeys.toggleFullscreen);
	xml_hotkeys.set("ToggleFullscreenAlt", hotkeys.toggleFullscreenAlt);
	xml_hotkeys.set("TakeScreenshot", hotkeys.takeScreenshot);
	xml_hotkeys.set("ToggleFastForward", hotkeys.toggleFastForward);
	xml_hotkeys.set("ExitApplication", hotkeys.exitApplication);
}
