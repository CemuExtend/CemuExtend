#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "util/helpers/helpers.h"

CemuConfig::ModInputConfiguration CemuConfig::GetModInputConfiguration() const
{
	std::shared_lock lock(mod_input_mutex);
	return {.device = mod_input_device, .volume = mod_input_volume};
}

void CemuConfig::SetModInputDevice(std::wstring device)
{
	std::unique_lock lock(mod_input_mutex);
	mod_input_device = std::move(device);
}

void CemuConfig::SetModInputVolume(sint32 volume)
{
	std::unique_lock lock(mod_input_mutex);
	mod_input_volume = volume;
}

namespace
{
	std::uint16_t LegacyWxKeyToUsbUsage(int key)
	{
		if (key >= 'A' && key <= 'Z')
			return static_cast<std::uint16_t>(0x04 + key - 'A');
		if (key >= 'a' && key <= 'z')
			return static_cast<std::uint16_t>(0x04 + key - 'a');
		if (key >= '1' && key <= '9')
			return static_cast<std::uint16_t>(0x1e + key - '1');
		if (key == '0')
			return 0x27;
		if (key >= 340 && key <= 351)
			return static_cast<std::uint16_t>(0x3a + key - 340);
		if (key >= 352 && key <= 363)
			return static_cast<std::uint16_t>(0x68 + key - 352);
		if (key >= 324 && key <= 333)
			return key == 324 ? 0x62 : static_cast<std::uint16_t>(0x58 + key - 324);
		switch (key)
		{
		case 8:
			return 0x2a;
		case 9:
			return 0x2b;
		case 13:
			return 0x28;
		case 27:
			return 0x29;
		case 32:
			return 0x2c;
		case 127:
			return 0x4c;
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
		case 310:
			return 0x48;
		case 311:
			return 0x39;
		case 312:
			return 0x4d;
		case 313:
			return 0x4a;
		case 314:
			return 0x50;
		case 315:
			return 0x52;
		case 316:
			return 0x4f;
		case 317:
			return 0x51;
		case 319:
			return 0x46;
		case 303:
			return 0x9b;
		case 305:
			return 0x9c;
		case 318:
			return 0x77;
		case 320:
			return 0x74;
		case 321:
			return 0x46;
		case 322:
			return 0x49;
		case 323:
			return 0x75;
		case 334:
			return 0x55;
		case 335:
			return 0x57;
		case 337:
			return 0x56;
		case 338:
			return 0x63;
		case 336:
			return 0x85;
		case 339:
			return 0x54;
		case 364:
			return 0x53;
		case 365:
			return 0x47;
		case 366:
			return 0x4b;
		case 367:
			return 0x4e;
		case 368:
			return 0x2c;
		case 369:
			return 0x2b;
		case 370:
			return 0x58;
		case 371:
			return 0x3a;
		case 372:
			return 0x3b;
		case 373:
			return 0x3c;
		case 374:
			return 0x3d;
		case 375:
			return 0x5f;
		case 376:
			return 0x5c;
		case 377:
			return 0x60;
		case 378:
			return 0x5e;
		case 379:
			return 0x5a;
		case 380:
			return 0x61;
		case 381:
			return 0x5b;
		case 382:
			return 0x59;
		case 383:
			return 0x5d;
		case 384:
			return 0x62;
		case 385:
			return 0x63;
		case 386:
			return 0x67;
		case 387:
			return 0x55;
		case 388:
			return 0x57;
		case 389:
			return 0x85;
		case 390:
			return 0x56;
		case 391:
			return 0x63;
		case 392:
			return 0x54;
		default:
			return 0;
		}
	}

	bool LoadLegacyHotkey(XMLConfigParser& hotkeys, const char* name,
						  FrontendHotkeyBindingConfig& binding)
	{
		const std::string value = hotkeys.get(name, "");
		if (value.empty())
			return false;
		unsigned raw{};
		int controller{-1};
		std::istringstream stream(value);
		if (!(stream >> raw >> controller) || controller < -1 ||
			controller >= FrontendHotkeyBindingConfig::kControllerButtonCount)
			return false;
		binding.keyboard_usage = LegacyWxKeyToUsbUsage(static_cast<int>(raw & 0x1fff));
		binding.keyboard_modifiers = static_cast<std::uint8_t>(
			((raw & 0x4000) ? 1U : 0U) | ((raw & 0x8000) ? 2U : 0U) |
			((raw & 0x2000) ? 4U : 0U));
		if (!binding.keyboard_usage)
			binding.keyboard_modifiers = 0;
		binding.controller_button = static_cast<std::int16_t>(controller);
		return true;
	}
} // namespace

std::optional<CemuExtendTitleGrant> CemuConfig::GetCemuExtendGrant(uint64 titleId) const
{
	std::shared_lock lock(cemuextend_grants_mutex);
	if (const auto found = cemuextend_grants.find(titleId); found != cemuextend_grants.end())
		return found->second;
	return std::nullopt;
}

void CemuConfig::SetCemuExtendGrant(uint64 titleId, CemuExtendTitleGrant grant)
{
	std::unique_lock lock(cemuextend_grants_mutex);
	cemuextend_grants[titleId] = grant;
}

void CemuConfig::RemoveCemuExtendGrant(uint64 titleId)
{
	std::unique_lock lock(cemuextend_grants_mutex);
	cemuextend_grants.erase(titleId);
}

std::optional<CemuExtendModGrant> CemuConfig::GetCemuExtendModGrant(uint64 titleId,
																	std::string_view principal) const
{
	std::shared_lock lock(cemuextend_grants_mutex);
	const auto title = cemuextend_mod_grants.find(titleId);
	if (title == cemuextend_mod_grants.end())
		return std::nullopt;
	const auto mod = title->second.find(std::string(principal));
	return mod == title->second.end() ? std::nullopt : std::optional{mod->second};
}

void CemuConfig::SetCemuExtendModGrant(uint64 titleId, std::string principal,
									   CemuExtendModGrant grant)
{
	if (titleId == 0 || principal.empty())
		return;
	std::unique_lock lock(cemuextend_grants_mutex);
	cemuextend_mod_grants[titleId][std::move(principal)] = grant;
}

void CemuConfig::RemoveCemuExtendModGrant(uint64 titleId, std::string_view principal)
{
	std::unique_lock lock(cemuextend_grants_mutex);
	const auto title = cemuextend_mod_grants.find(titleId);
	if (title == cemuextend_mod_grants.end())
		return;
	title->second.erase(std::string(principal));
	if (title->second.empty())
		cemuextend_mod_grants.erase(title);
}

std::optional<CemuExtendModTrustAnchor> CemuConfig::GetCemuExtendModTrustAnchor(uint64 titleId,
																				std::string_view modId) const
{
	std::shared_lock lock(cemuextend_grants_mutex);
	const auto title = cemuextend_mod_trust.find(titleId);
	if (title == cemuextend_mod_trust.end())
		return std::nullopt;
	const auto mod = title->second.find(std::string(modId));
	return mod == title->second.end() ? std::nullopt : std::optional{mod->second};
}

void CemuConfig::SetCemuExtendModTrustAnchor(uint64 titleId, std::string modId,
											 CemuExtendModTrustAnchor anchor)
{
	if (titleId == 0 || modId.empty())
		return;
	std::unique_lock lock(cemuextend_grants_mutex);
	cemuextend_mod_trust[titleId][std::move(modId)] = anchor;
}

void CemuConfig::RemoveCemuExtendModTrustAnchor(uint64 titleId, std::string_view modId)
{
	std::unique_lock lock(cemuextend_grants_mutex);
	const auto title = cemuextend_mod_trust.find(titleId);
	if (title == cemuextend_mod_trust.end())
		return;
	title->second.erase(std::string(modId));
	if (title->second.empty())
		cemuextend_mod_trust.erase(title);
}

std::optional<CemuExtendPermissionApproval> CemuConfig::GetCemuExtendPermissionApproval(
	uint64 titleId, std::string_view approvalKey) const
{
	std::shared_lock lock(cemuextend_grants_mutex);
	const auto title = cemuextend_permission_approvals.find(titleId);
	if (title == cemuextend_permission_approvals.end())
		return std::nullopt;
	const auto approval = title->second.find(std::string(approvalKey));
	return approval == title->second.end() ? std::nullopt : std::optional{approval->second};
}

void CemuConfig::SetCemuExtendPermissionApproval(uint64 titleId, std::string approvalKey,
												 CemuExtendPermissionApproval approval)
{
	if (titleId == 0 || approvalKey.empty() || approval.packageDigest.empty() ||
		approval.modIdentity.empty())
		return;
	std::unique_lock lock(cemuextend_grants_mutex);
	cemuextend_permission_approvals[titleId][std::move(approvalKey)] = std::move(approval);
}

void CemuConfig::RemoveCemuExtendPermissionApproval(uint64 titleId, std::string_view approvalKey)
{
	std::unique_lock lock(cemuextend_grants_mutex);
	const auto title = cemuextend_permission_approvals.find(titleId);
	if (title == cemuextend_permission_approvals.end())
		return;
	title->second.erase(std::string(approvalKey));
	if (title->second.empty())
		cemuextend_permission_approvals.erase(title);
}

bool CemuConfig::IsCemuExtendModEnabled(std::string_view modIdentity) const
{
	if (modIdentity.empty())
		return true;
	std::shared_lock lock(cemuextend_grants_mutex);
	return !cemuextend_disabled_mods.contains(std::string(modIdentity));
}

void CemuConfig::SetCemuExtendModEnabled(std::string modIdentity, bool enabled)
{
	if (modIdentity.empty() || modIdentity.size() > 256)
		return;
	std::unique_lock lock(cemuextend_grants_mutex);
	if (enabled)
		cemuextend_disabled_mods.erase(modIdentity);
	else
		cemuextend_disabled_mods.insert(std::move(modIdentity));
}

std::optional<uint64> CemuConfig::GetCemuExtendModUpdateTrust(uint64 titleId,
															  std::string_view modIdentity) const
{
	if (titleId == 0 || modIdentity.empty())
		return std::nullopt;
	std::shared_lock lock(cemuextend_grants_mutex);
	const auto title = cemuextend_mod_update_trust.find(titleId);
	if (title == cemuextend_mod_update_trust.end())
		return std::nullopt;
	const auto entry = title->second.find(std::string(modIdentity));
	return entry == title->second.end() ? std::nullopt : std::optional{entry->second};
}

void CemuConfig::SetCemuExtendModUpdateTrust(uint64 titleId, std::string modIdentity, uint64 granted)
{
	if (titleId == 0 || modIdentity.empty() || modIdentity.size() > 256)
		return;
	std::unique_lock lock(cemuextend_grants_mutex);
	cemuextend_mod_update_trust[titleId][std::move(modIdentity)] = granted;
}

void CemuConfig::RemoveCemuExtendModUpdateTrust(uint64 titleId, std::string_view modIdentity)
{
	std::unique_lock lock(cemuextend_grants_mutex);
	const auto title = cemuextend_mod_update_trust.find(titleId);
	if (title == cemuextend_mod_update_trust.end())
		return;
	title->second.erase(std::string(modIdentity));
	if (title->second.empty())
		cemuextend_mod_update_trust.erase(title);
}

void CemuConfig::SetMLCPath(fs::path path, bool save)
{
	mlc_path.SetValue(_pathToUtf8(path));
	if (save)
		GetConfigHandle().Save();
}

XMLConfigParser CemuConfig::Load(XMLConfigParser& parser)
{
	auto rootParser = parser;
	auto loadHotkey = [](XMLConfigParser& hotkeys, const char* name,
						 FrontendHotkeyBindingConfig& binding) {
		const auto value = hotkeys.get(name, "");
		if (!value || !*value)
			return;
		std::istringstream stream(value);
		unsigned usage{};
		unsigned modifiers{};
		int controller{-1};
		if (stream >> usage >> modifiers >> controller && usage <= 0xffff &&
			modifiers <= 0x0f && controller >= -1 &&
			controller < FrontendHotkeyBindingConfig::kControllerButtonCount &&
			!(usage >= 0xe0 && usage <= 0xe7))
		{
			binding.keyboard_usage = static_cast<std::uint16_t>(usage);
			binding.keyboard_modifiers = static_cast<std::uint8_t>(modifiers);
			binding.controller_button = static_cast<std::int16_t>(controller);
		}
	};
	// These keys were historically owned by wxCemuConfig at the document root.
	// Keep them as migration defaults while storing frontend-neutral values below.
	bool legacyFullscreen = parser.get("fullscreen", false);
	bool legacyOpenPad = parser.get("open_pad", false);
	bool legacyCheckUpdates = parser.get("check_update", true);
	bool legacySaveScreenshots = parser.get("save_screenshot", true);
	auto new_parser = parser.get("content");
	if (new_parser.valid())
	{
		parser = new_parser;
		legacyFullscreen = parser.get("fullscreen", legacyFullscreen);
		legacyOpenPad = parser.get("open_pad", legacyOpenPad);
		legacyCheckUpdates = parser.get("check_update", legacyCheckUpdates);
		legacySaveScreenshots = parser.get("save_screenshot", legacySaveScreenshots);
	}

	// general settings
	log_flag = parser.get("logflag", log_flag.GetInitValue());
	cemuLog_setActiveLoggingFlags(GetConfig().log_flag.GetValue());
	advanced_ppc_logging = parser.get("advanced_ppc_logging", advanced_ppc_logging.GetInitValue());

	const char* mlc = parser.get("mlc_path", "");
	mlc_path = mlc;

	permanent_storage = parser.get("permanent_storage", permanent_storage);

	proxy_server = parser.get("proxy_server", "");
	disable_screensaver = parser.get("disable_screensaver", disable_screensaver);
	play_boot_sound = parser.get("play_boot_sound", play_boot_sound);
	auto frontendNode = parser.get("Frontend");
	frontend.start_fullscreen = parser.get("fullscreen",
										   frontendNode.get("StartFullscreen", legacyFullscreen));
	frontend.open_pad = parser.get("open_pad",
								   frontendNode.get("OpenPad", legacyOpenPad));
	frontend.check_updates = parser.get("check_update",
										frontendNode.get("CheckUpdates", legacyCheckUpdates));
	frontend.save_screenshots = parser.get("save_screenshot",
										   frontendNode.get("SaveScreenshots", legacySaveScreenshots));
	frontend.ui_language = frontendNode.get("UiLanguage", "system");
	// Load is only called for an existing configuration. Treat old configurations
	// as already onboarded unless the new frontend explicitly persisted otherwise.
	frontend.setup_completed = frontendNode.get("SetupCompleted", true);
	auto frontendHotkeys = frontendNode.get("Hotkeys");
	if (frontendHotkeys.valid())
	{
		const auto modifier = frontendHotkeys.get("ControllerModifier", -1);
		frontend.hotkeys.controller_modifier = modifier >= -1 &&
													   modifier < FrontendHotkeyBindingConfig::kControllerButtonCount
												   ? static_cast<std::int16_t>(modifier)
												   : -1;
		loadHotkey(frontendHotkeys, "ToggleFullscreen", frontend.hotkeys.toggle_fullscreen);
		loadHotkey(frontendHotkeys, "ToggleFullscreenAlternative",
				   frontend.hotkeys.toggle_fullscreen_alternative);
		loadHotkey(frontendHotkeys, "ExitFullscreen", frontend.hotkeys.exit_fullscreen);
		loadHotkey(frontendHotkeys, "TakeScreenshot", frontend.hotkeys.take_screenshot);
		loadHotkey(frontendHotkeys, "ToggleFastForward", frontend.hotkeys.toggle_fast_forward);
		loadHotkey(frontendHotkeys, "EndEmulation", frontend.hotkeys.end_emulation);
		loadHotkey(frontendHotkeys, "ExitApplication", frontend.hotkeys.exit_application);
	}
	else
	{
		auto legacyHotkeys = rootParser.get("Hotkeys");
		if (legacyHotkeys.valid())
		{
			FrontendHotkeyBindingConfig modifier;
			if (LoadLegacyHotkey(legacyHotkeys, "modifiers", modifier))
				frontend.hotkeys.controller_modifier = modifier.controller_button;
			(void)LoadLegacyHotkey(legacyHotkeys, "ToggleFullscreen",
								   frontend.hotkeys.toggle_fullscreen);
			(void)LoadLegacyHotkey(legacyHotkeys, "ToggleFullscreenAlt",
								   frontend.hotkeys.toggle_fullscreen_alternative);
			(void)LoadLegacyHotkey(legacyHotkeys, "ExitFullscreen",
								   frontend.hotkeys.exit_fullscreen);
			(void)LoadLegacyHotkey(legacyHotkeys, "TakeScreenshot",
								   frontend.hotkeys.take_screenshot);
			(void)LoadLegacyHotkey(legacyHotkeys, "ToggleFastForward",
								   frontend.hotkeys.toggle_fast_forward);
			(void)LoadLegacyHotkey(legacyHotkeys, "EndEmulation",
								   frontend.hotkeys.end_emulation);
			(void)LoadLegacyHotkey(legacyHotkeys, "ExitApplication",
								   frontend.hotkeys.exit_application);
		}
	}
	console_language = parser.get("console_language", console_language.GetInitValue());

	game_paths.clear();
	auto game_path_parser = parser.get("GamePaths");
	for (auto element = game_path_parser.get("Entry"); element.valid(); element = game_path_parser.get("Entry", element))
	{
		const std::string path = element.value("");
		if (path.empty())
			continue;
		try
		{
			game_paths.emplace_back(path);
		} catch (const std::exception&)
		{
			cemuLog_log(LogType::Force, "config load error: can't load game path: {}", path);
		}
	}

	{
		std::unique_lock grantsLock(cemuextend_grants_mutex);
		cemuextend_grants.clear();
		cemuextend_mod_grants.clear();
		cemuextend_mod_trust.clear();
		cemuextend_permission_approvals.clear();
		auto bridge = parser.get("CemuExtend");
		for (auto title = bridge.get("Title"); title.valid(); title = bridge.get("Title", title))
		{
			const auto titleId = title.get_attribute<uint64>("id", 0);
			if (titleId == 0)
				continue;
			cemuextend_grants[titleId] = {
				title.get_attribute<uint32>("read", 0),
				title.get_attribute<uint32>("write", 0),
				title.get_attribute<uint32>("inject", 0),
			};
		}
		for (auto mod = bridge.get("Mod"); mod.valid(); mod = bridge.get("Mod", mod))
		{
			const auto titleId = mod.get_attribute<uint64>("title", 0);
			const std::string principal = mod.get_attribute("principal", "");
			if (titleId == 0 || principal.empty() || principal.size() > 256)
				continue;
			cemuextend_mod_grants[titleId][principal] = {
				mod.get_attribute<uint32>("permissions", 0) & 0x3fU,
				mod.get_attribute<uint32>("approved_requests", 0) & 0x3fU,
				mod.get_attribute<bool>("approved", false)};
		}
		for (auto trust = bridge.get("ModTrust"); trust.valid(); trust = bridge.get("ModTrust", trust))
		{
			const auto titleId = trust.get_attribute<uint64>("title", 0);
			const std::string modId = trust.get_attribute("mod_id", "");
			if (titleId == 0 || modId.empty() || modId.size() > 128)
				continue;
			cemuextend_mod_trust[titleId][modId] = {
				trust.get_attribute<uint32>("permissions", 0) & 0x3fU,
				trust.get_attribute<uint32>("approved_requests", 0) & 0x3fU};
		}
		for (auto approval = bridge.get("PermissionApproval"); approval.valid();
			 approval = bridge.get("PermissionApproval", approval))
		{
			const auto titleId = approval.get_attribute<uint64>("title", 0);
			const std::string key = approval.get_attribute("key", "");
			const std::string digest = approval.get_attribute("package_digest", "");
			const std::string identity = approval.get_attribute("mod_identity", "");
			if (titleId == 0 || key.empty() || digest.empty() || identity.empty() ||
				digest.size() > 128 || identity.size() > 256 || key.size() > 512)
				continue;
			cemuextend_permission_approvals[titleId][key] = {
				digest, identity,
				approval.get_attribute<uint64>("requested", 0),
				approval.get_attribute<uint64>("granted", 0),
				approval.get_attribute<bool>("approved", false),
				approval.get_attribute<bool>("headless_denial", false)};
		}
		for (auto trust = bridge.get("UpdateTrust"); trust.valid();
			 trust = bridge.get("UpdateTrust", trust))
		{
			const auto titleId = trust.get_attribute<uint64>("title", 0);
			const std::string identity = trust.get_attribute("mod_identity", "");
			if (titleId == 0 || identity.empty() || identity.size() > 256)
				continue;
			cemuextend_mod_update_trust[titleId][identity] =
				trust.get_attribute<uint64>("granted", 0);
		}
		for (auto disabled = bridge.get("DisabledMod"); disabled.valid();
			 disabled = bridge.get("DisabledMod", disabled))
		{
			const std::string identity = disabled.get_attribute("mod_identity", "");
			// Entries carrying a title come from the first, per-title version of this
			// switch. That scope could disable a mod for a title the user never plays
			// while leaving it loaded in the one they do, so those are dropped rather
			// than reinterpreted as switching the mod off everywhere.
			if (identity.empty() || identity.size() > 256 ||
				disabled.get_attribute<uint64>("title", 0) != 0)
				continue;
			cemuextend_disabled_mods.insert(identity);
		}
	}

	std::unique_lock _lock(game_cache_entries_mutex);
	game_cache_entries.clear();
	auto game_cache_parser = parser.get("GameCache");
	for (auto element = game_cache_parser.get("Entry"); element.valid(); element = game_cache_parser.get("Entry", element))
	{
		const char* rpx = element.get("path", "");
		try
		{
			GameEntry entry{};
			entry.rpx_file = boost::nowide::widen(rpx);
			entry.title_id = element.get<decltype(entry.title_id)>("title_id");
			entry.legacy_name = boost::nowide::widen(element.get("name", ""));
			entry.custom_name = element.get("custom_name", "");
			entry.legacy_region = element.get("region", 0);
			entry.legacy_version = element.get("version", 0);
			entry.legacy_update_version = element.get("version", 0);
			entry.legacy_dlc_version = element.get("dlc_version", 0);
			entry.legacy_time_played = element.get<decltype(entry.legacy_time_played)>("time_played");
			entry.legacy_last_played = element.get<decltype(entry.legacy_last_played)>("last_played");
			entry.favorite = element.get("favorite", false);
			game_cache_entries.emplace_back(entry);

			if (entry.title_id != 0)
			{
				if (entry.favorite)
					game_cache_favorites.emplace(entry.title_id);
				else
					game_cache_favorites.erase(entry.title_id);
			}
		} catch (const std::exception&)
		{
			cemuLog_log(LogType::Force, "config load error: can't load game cache entry: {}", rpx);
		}
	}
	_lock.unlock();

	graphic_pack_entries.clear();
	auto graphic_pack_parser = parser.get("GraphicPack");
	for (auto element = graphic_pack_parser.get("Entry"); element.valid(); element = graphic_pack_parser.get("Entry", element))
	{
		std::string filename = element.get_attribute("filename", "");
		if (filename.empty()) // legacy loading
		{
			filename = element.get("filename", "");
			fs::path path = fs::path(filename).lexically_normal();
			graphic_pack_entries.try_emplace(path);
			const std::string category = element.get("category", "");
			const std::string preset = element.get("preset", "");
			graphic_pack_entries[filename].try_emplace(category, preset);
		}
		else
		{
			fs::path path = fs::path(filename).lexically_normal();
			graphic_pack_entries.try_emplace(path);

			const bool disabled = element.get_attribute("disabled", false);
			if (disabled)
			{
				graphic_pack_entries[path].try_emplace("_disabled", "true");
			}

			for (auto preset = element.get("Preset"); preset.valid(); preset = element.get("Preset", preset))
			{
				const std::string category = preset.get("category", "");
				const std::string active_preset = preset.get("preset", "");
				graphic_pack_entries[path].try_emplace(category, active_preset);
			}
		}
	}

	// graphics
	auto graphic = parser.get("Graphic");
	graphic_api = graphic.get("api", kDefaultGraphicsAPI);
	graphic.get("device", legacy_graphic_device_uuid);
	if (graphic.get("vkDevice").valid())
		graphic.get("vkDevice", vk_graphic_device_uuid);
	else
		vk_graphic_device_uuid = legacy_graphic_device_uuid;
	mtl_graphic_device_uuid = graphic.get("mtlDevice", 0);
	vsync = graphic.get("VSync", 0);
	overrideAppGammaPreference = graphic.get("OverrideAppGammaPreference", false);
	overrideGammaValue = graphic.get("OverrideGammaValue", 2.2f);
	if (overrideGammaValue < 0)
		overrideGammaValue = 2.2f;
	userDisplayGamma = graphic.get("UserDisplayGamma", 2.2f);
	if (userDisplayGamma < 0)
		userDisplayGamma = 2.2f;
	gx2drawdone_sync = graphic.get("GX2DrawdoneSync", true);
	upscale_filter = graphic.get("UpscaleFilter", kBicubicHermiteFilter);
	downscale_filter = graphic.get("DownscaleFilter", kLinearFilter);
	fullscreen_scaling = graphic.get("FullscreenScaling", kKeepAspectRatio);
	async_compile = graphic.get("AsyncCompile", async_compile);
	vk_accurate_barriers = graphic.get("vkAccurateBarriers", true); // this used to be "VulkanAccurateBarriers" but because we changed the default to true in 1.27.1 the option name had to be changed
#ifdef ENABLE_METAL
	force_mesh_shaders = graphic.get("ForceMeshShaders", false);
#endif

	auto overlay_node = graphic.get("Overlay");
	if (overlay_node.valid())
	{
		overlay.position = overlay_node.get("Position", ScreenPosition::kDisabled);
		overlay.text_color = overlay_node.get("TextColor", 0xFFFFFFFF);
		overlay.text_scale = overlay_node.get("TextScale", 100);
		overlay.fps = overlay_node.get("FPS", true);
		overlay.drawcalls = overlay_node.get("DrawCalls", false);
		overlay.cpu_usage = overlay_node.get("CPUUsage", false);
		overlay.cpu_per_core_usage = overlay_node.get("CPUPerCoreUsage", false);
		overlay.ram_usage = overlay_node.get("RAMUsage", false);
		overlay.vram_usage = overlay_node.get("VRAMUsage", false);
		overlay.debug = overlay_node.get("Debug", false);

		notification.controller_profiles = overlay_node.get("ControllerProfiles", true);
		notification.controller_battery = overlay_node.get("ControllerBattery", true);
		notification.shader_compiling = overlay_node.get("ShaderCompiling", true);
	}
	else
	{
		// legacy support
		overlay.position = graphic.get("OverlayPosition", ScreenPosition::kDisabled);
		overlay.text_color = graphic.get("OverlayTextColor", 0xFFFFFFFF);
		overlay.fps = graphic.get("OverlayFPS", true);
		overlay.drawcalls = graphic.get("OverlayDrawCalls", false);
		overlay.cpu_usage = graphic.get("OverlayCPUUsage", false);
		overlay.cpu_per_core_usage = graphic.get("OverlayCPUPerCoreUsage", false);
		overlay.ram_usage = graphic.get("OverlayRAMUsage", false);

		notification.controller_profiles = graphic.get("OverlayControllerProfiles", true);
		notification.controller_battery = graphic.get("OverlayControllerBattery", true);
		notification.shader_compiling = graphic.get("ShaderCompiling", true);
	}

	auto notification_node = graphic.get("Notification");
	if (notification_node.valid())
	{
		notification.position = notification_node.get("Position", ScreenPosition::kTopLeft);
		notification.text_color = notification_node.get("TextColor", 0xFFFFFFFF);
		notification.text_scale = notification_node.get("TextScale", 100);
		notification.controller_profiles = notification_node.get("ControllerProfiles", true);
		notification.controller_battery = notification_node.get("ControllerBattery", false);
		notification.shader_compiling = notification_node.get("ShaderCompiling", true);
		notification.friends = notification_node.get("FriendService", true);
	}

	// audio
	auto audio = parser.get("Audio");
	audio_api = audio.get("api", 0);
	audio_delay = audio.get("delay", 2);
	tv_channels = audio.get("TVChannels", kStereo);
	pad_channels = audio.get("PadChannels", kStereo);
	input_channels = audio.get("InputChannels", kMono);
	tv_volume = audio.get("TVVolume", 20);
	pad_volume = audio.get("PadVolume", 0);
	input_volume = audio.get("InputVolume", 20);
	mod_input_volume = audio.get("ModInputVolume", 100);
	portal_volume = audio.get("PortalVolume", 20);

	const auto tv = audio.get("TVDevice", "");
	try
	{
		tv_device = boost::nowide::widen(tv);
	} catch (const std::exception&)
	{
		cemuLog_log(LogType::Force, "config load error: can't load tv device: {}", tv);
	}

	const auto pad = audio.get("PadDevice", "");
	try
	{
		pad_device = boost::nowide::widen(pad);
	} catch (const std::exception&)
	{
		cemuLog_log(LogType::Force, "config load error: can't load pad device: {}", pad);
	}

	const auto input_device_name = audio.get("InputDevice", "");
	try
	{
		input_device = boost::nowide::widen(input_device_name);
	} catch (const std::exception&)
	{
		cemuLog_log(LogType::Force, "config load error: can't load input device: {}", input_device_name);
	}

	const auto portal_device_name = audio.get("PortalDevice", "");
	const auto mod_input_device_name = audio.get("ModInputDevice", "default");
	try
	{
		mod_input_device = boost::nowide::widen(mod_input_device_name);
	} catch (const std::exception&)
	{
		cemuLog_log(LogType::Force, "config load error: can't load Mod microphone device: {}", mod_input_device_name);
	}

	try
	{
		portal_device = boost::nowide::widen(portal_device_name);
	} catch (const std::exception&)
	{
		cemuLog_log(LogType::Force, "config load error: can't load input device: {}", portal_device_name);
	}

	// account
	auto acc = parser.get("Account");
	account.m_persistent_id = acc.get("PersistentId", account.m_persistent_id);
	// legacy online settings, we only parse these for upgrading purposes
	account.legacy_online_enabled = acc.get("OnlineEnabled", account.legacy_online_enabled);
	account.legacy_active_service = acc.get("ActiveService", account.legacy_active_service);
	// per-account online setting
	auto accService = parser.get("AccountService");
	account.service_select.clear();
	for (auto element = accService.get("SelectedService"); element.valid(); element = accService.get("SelectedService", element))
	{
		uint32 persistentId = element.get_attribute<uint32>("PersistentId", 0);
		sint32 serviceIndex = element.get_attribute<sint32>("Service", 0);
		NetworkService networkService = static_cast<NetworkService>(serviceIndex);
		if (persistentId < CemuConfig::kMinimumAccountPersistentId)
			continue;
		if (networkService == NetworkService::Offline || networkService == NetworkService::Nintendo || networkService == NetworkService::Pretendo || networkService == NetworkService::Custom || networkService == NetworkService::Plasma)
			account.service_select.emplace(persistentId, networkService);
	}
	// debug
	auto debug = parser.get("Debug");
#if BOOST_OS_WINDOWS
	crash_dump = debug.get("CrashDumpWindows", crash_dump);
#elif BOOST_OS_UNIX
	crash_dump = debug.get("CrashDumpUnix", crash_dump);
#endif
	gdb_port = debug.get("GDBPort", 1337);
#ifdef ENABLE_METAL
	gpu_capture_dir = debug.get("GPUCaptureDir", "");
	framebuffer_fetch = debug.get("FramebufferFetch", true);
#endif

	// tcpgecko
	auto tcpGeckoNode = parser.get("TcpGecko");
	tcpgecko.enabled = tcpGeckoNode.get("Enabled", tcpgecko.enabled);
	tcpgecko.port = tcpGeckoNode.get("Port", tcpgecko.port);
	tcpgecko.allow_lan = tcpGeckoNode.get("AllowLan", tcpgecko.allow_lan);
	tcpgecko.handler_version = tcpGeckoNode.get("HandlerVersion", tcpgecko.handler_version);

	// input
	auto input = parser.get("Input");
	auto dsuc = input.get("DSUC");
	dsu_client.host = dsuc.get_attribute("host", dsu_client.host);
	dsu_client.port = dsuc.get_attribute("port", dsu_client.port);

	// emulatedusbdevices
	auto usbdevices = parser.get("EmulatedUsbDevices");
	emulated_usb_devices.emulate_skylander_portal = usbdevices.get("EmulateSkylanderPortal", emulated_usb_devices.emulate_skylander_portal);
	emulated_usb_devices.emulate_infinity_base = usbdevices.get("EmulateInfinityBase", emulated_usb_devices.emulate_infinity_base);
	emulated_usb_devices.emulate_dimensions_toypad = usbdevices.get("EmulateDimensionsToypad", emulated_usb_devices.emulate_dimensions_toypad);

	return parser;
}

XMLConfigParser CemuConfig::Save(XMLConfigParser& parser)
{
	auto config = parser.set("content");
	// general settings
	config.set("logflag", log_flag.GetValue());
	config.set("advanced_ppc_logging", advanced_ppc_logging.GetValue());
	config.set("mlc_path", mlc_path.GetValue().c_str());
	config.set<bool>("permanent_storage", permanent_storage);
	config.set("proxy_server", proxy_server.GetValue().c_str());
	config.set<bool>("play_boot_sound", play_boot_sound);
	auto frontendNode = config.set("Frontend");
	frontendNode.set("StartFullscreen", frontend.start_fullscreen.GetValue());
	frontendNode.set("OpenPad", frontend.open_pad.GetValue());
	frontendNode.set("CheckUpdates", frontend.check_updates.GetValue());
	frontendNode.set("SaveScreenshots", frontend.save_screenshots.GetValue());
	frontendNode.set("SetupCompleted", frontend.setup_completed.GetValue());
	frontendNode.set("UiLanguage", frontend.ui_language.GetValue().c_str());
	auto hotkeysNode = frontendNode.set("Hotkeys");
	hotkeysNode.set("ControllerModifier", frontend.hotkeys.controller_modifier);
	auto saveHotkey = [&hotkeysNode](const char* name,
									 const FrontendHotkeyBindingConfig& binding) {
		hotkeysNode.set(name, fmt::format("{} {} {}", binding.keyboard_usage,
										  binding.keyboard_modifiers, binding.controller_button));
	};
	saveHotkey("ToggleFullscreen", frontend.hotkeys.toggle_fullscreen);
	saveHotkey("ToggleFullscreenAlternative", frontend.hotkeys.toggle_fullscreen_alternative);
	saveHotkey("ExitFullscreen", frontend.hotkeys.exit_fullscreen);
	saveHotkey("TakeScreenshot", frontend.hotkeys.take_screenshot);
	saveHotkey("ToggleFastForward", frontend.hotkeys.toggle_fast_forward);
	saveHotkey("EndEmulation", frontend.hotkeys.end_emulation);
	saveHotkey("ExitApplication", frontend.hotkeys.exit_application);

	// config.set("cpu_mode", cpu_mode.GetValue());
	// config.set("console_region", console_region.GetValue());
	config.set("console_language", console_language.GetValue());

	// game paths
	auto game_path_parser = config.set("GamePaths");
	for (const auto& entry : game_paths)
	{
		game_path_parser.set("Entry", entry.c_str());
	}

	{
		auto bridge = config.set("CemuExtend");
		std::shared_lock grantsLock(cemuextend_grants_mutex);
		for (const auto& [titleId, grant] : cemuextend_grants)
		{
			auto title = bridge.set("Title");
			title.set_attribute("id", static_cast<sint64>(titleId));
			title.set_attribute("read", grant.read_mask);
			title.set_attribute("write", grant.write_mask);
			title.set_attribute("inject", grant.inject_mask);
		}
		for (const auto& [titleId, mods] : cemuextend_mod_grants)
			for (const auto& [principal, grant] : mods)
			{
				auto mod = bridge.set("Mod");
				mod.set_attribute("title", static_cast<sint64>(titleId));
				mod.set_attribute("principal", principal.c_str());
				mod.set_attribute("permissions", grant.permissions & 0x3fU);
				mod.set_attribute("approved_requests", grant.approved_request_mask & 0x3fU);
				mod.set_attribute("approved", grant.approved);
			}
		for (const auto& [titleId, mods] : cemuextend_mod_trust)
			for (const auto& [modId, anchor] : mods)
			{
				auto trust = bridge.set("ModTrust");
				trust.set_attribute("title", static_cast<sint64>(titleId));
				trust.set_attribute("mod_id", modId.c_str());
				trust.set_attribute("permissions", anchor.permissions & 0x3fU);
				trust.set_attribute("approved_requests", anchor.approved_request_mask & 0x3fU);
			}
		for (const auto& [titleId, approvals] : cemuextend_permission_approvals)
			for (const auto& [key, approval] : approvals)
			{
				auto node = bridge.set("PermissionApproval");
				node.set_attribute("title", static_cast<sint64>(titleId));
				node.set_attribute("key", key.c_str());
				node.set_attribute("package_digest", approval.packageDigest.c_str());
				node.set_attribute("mod_identity", approval.modIdentity.c_str());
				node.set_attribute("requested", static_cast<sint64>(approval.requestedPermissions));
				node.set_attribute("granted", static_cast<sint64>(approval.grantedPermissions));
				node.set_attribute("approved", approval.approved);
				node.set_attribute("headless_denial", approval.explicitHeadlessDenial);
			}
		for (const auto& identity : cemuextend_disabled_mods)
		{
			auto node = bridge.set("DisabledMod");
			node.set_attribute("mod_identity", identity.c_str());
		}
		for (const auto& [titleId, identities] : cemuextend_mod_update_trust)
			for (const auto& [identity, granted] : identities)
			{
				auto node = bridge.set("UpdateTrust");
				node.set_attribute("title", static_cast<sint64>(titleId));
				node.set_attribute("mod_identity", identity.c_str());
				node.set_attribute("granted", static_cast<sint64>(granted));
			}
	}

	// game list cache
	std::unique_lock _lock(game_cache_entries_mutex);
	auto game_cache_parser = config.set("GameCache");
	for (const auto& game : game_cache_entries)
	{
		auto entry = game_cache_parser.set("Entry");

		entry.set("title_id", (sint64)game.title_id);
		entry.set("name", boost::nowide::narrow(game.legacy_name).c_str());
		entry.set("custom_name", game.custom_name.c_str());
		entry.set("region", (sint32)game.legacy_region);
		entry.set("version", (sint32)game.legacy_update_version);
		entry.set("dlc_version", (sint32)game.legacy_dlc_version);
		entry.set("path", boost::nowide::narrow(game.rpx_file).c_str());
		entry.set("time_played", game.legacy_time_played);
		entry.set("last_played", game.legacy_last_played);
		entry.set("favorite", game.favorite);
	}
	_lock.unlock();

	auto graphic_pack_parser = config.set("GraphicPack");
	for (const auto& game : graphic_pack_entries)
	{
		auto entry = graphic_pack_parser.set("Entry");
		entry.set_attribute("filename", _pathToUtf8(game.first).c_str());
		for (const auto& kv : game.second)
		{
			// TODO: less hacky pls
			if (boost::iequals(kv.first, "_disabled"))
			{
				entry.set_attribute("disabled", true);
				continue;
			}

			auto preset = entry.set("Preset");
			if (!kv.first.empty())
				preset.set("category", kv.first.c_str());

			preset.set("preset", kv.second.c_str());
		}
	}

	// graphics
	auto graphic = config.set("Graphic");
	graphic.set("api", graphic_api);
	graphic.set("device", legacy_graphic_device_uuid);
	graphic.set("vkDevice", vk_graphic_device_uuid);
	graphic.set("mtlDevice", mtl_graphic_device_uuid);
	graphic.set("VSync", vsync);
	graphic.set("OverrideAppGammaPreference", overrideAppGammaPreference);
	graphic.set("OverrideGammaValue", overrideGammaValue);
	graphic.set("UserDisplayGamma", userDisplayGamma);
	graphic.set("GX2DrawdoneSync", gx2drawdone_sync);
#ifdef ENABLE_METAL
	graphic.set("ForceMeshShaders", force_mesh_shaders);
#endif
	// graphic.set("PrecompiledShaders", precompiled_shaders.GetValue());
	graphic.set("UpscaleFilter", upscale_filter);
	graphic.set("DownscaleFilter", downscale_filter);
	graphic.set("FullscreenScaling", fullscreen_scaling);
	graphic.set("AsyncCompile", async_compile.GetValue());
	graphic.set("vkAccurateBarriers", vk_accurate_barriers);

	auto overlay_node = graphic.set("Overlay");
	overlay_node.set("Position", overlay.position);
	overlay_node.set("TextColor", overlay.text_color);
	overlay_node.set("TextScale", overlay.text_scale);
	overlay_node.set("FPS", overlay.fps);
	overlay_node.set("DrawCalls", overlay.drawcalls);
	overlay_node.set("CPUUsage", overlay.cpu_usage);
	overlay_node.set("CPUPerCoreUsage", overlay.cpu_per_core_usage);
	overlay_node.set("RAMUsage", overlay.ram_usage);
	overlay_node.set("VRAMUsage", overlay.vram_usage);
	overlay_node.set("Debug", overlay.debug);

	auto notification_node = graphic.set("Notification");
	notification_node.set("Position", notification.position);
	notification_node.set("TextColor", notification.text_color);
	notification_node.set("TextScale", notification.text_scale);
	notification_node.set("ControllerProfiles", notification.controller_profiles);
	notification_node.set("ControllerBattery", notification.controller_battery);
	notification_node.set("ShaderCompiling", notification.shader_compiling);
	notification_node.set("FriendService", notification.friends);

	// audio
	auto audio = config.set("Audio");
	audio.set("api", audio_api);
	audio.set("delay", audio_delay);
	audio.set("TVChannels", tv_channels);
	audio.set("PadChannels", pad_channels);
	audio.set("InputChannels", input_channels);
	audio.set("TVVolume", tv_volume);
	audio.set("PadVolume", pad_volume);
	audio.set("InputVolume", input_volume);
	audio.set("ModInputVolume", mod_input_volume);
	audio.set("PortalVolume", portal_volume);
	audio.set("TVDevice", boost::nowide::narrow(tv_device).c_str());
	audio.set("PadDevice", boost::nowide::narrow(pad_device).c_str());
	audio.set("InputDevice", boost::nowide::narrow(input_device).c_str());
	audio.set("ModInputDevice", boost::nowide::narrow(mod_input_device).c_str());
	audio.set("PortalDevice", boost::nowide::narrow(portal_device).c_str());

	// account
	auto acc = config.set("Account");
	acc.set("PersistentId", account.m_persistent_id.GetValue());
	// legacy online mode setting
	acc.set("OnlineEnabled", account.legacy_online_enabled.GetValue());
	acc.set("ActiveService", account.legacy_active_service.GetValue());
	// per-account online setting
	auto accService = config.set("AccountService");
	for (auto& it : account.service_select)
	{
		auto entry = accService.set("SelectedService");
		entry.set_attribute("PersistentId", it.first);
		entry.set_attribute("Service", static_cast<sint32>(it.second));
	}
	// debug
	auto debug = config.set("Debug");
#if BOOST_OS_WINDOWS
	debug.set("CrashDumpWindows", crash_dump.GetValue());
#elif BOOST_OS_UNIX
	debug.set("CrashDumpUnix", crash_dump.GetValue());
#endif
	debug.set("GDBPort", gdb_port);
#ifdef ENABLE_METAL
	debug.set("GPUCaptureDir", gpu_capture_dir);
	debug.set("FramebufferFetch", framebuffer_fetch);
#endif

	// tcpgecko
	auto tcpGeckoNode = config.set("TcpGecko");
	tcpGeckoNode.set("Enabled", tcpgecko.enabled.GetValue());
	tcpGeckoNode.set("Port", tcpgecko.port.GetValue());
	tcpGeckoNode.set("AllowLan", tcpgecko.allow_lan.GetValue());
	tcpGeckoNode.set("HandlerVersion", tcpgecko.handler_version.GetValue());

	// input
	auto input = config.set("Input");
	auto dsuc = input.set("DSUC");
	dsuc.set_attribute("host", dsu_client.host);
	dsuc.set_attribute("port", dsu_client.port);

	// emulated usb devices
	auto usbdevices = config.set("EmulatedUsbDevices");
	usbdevices.set("EmulateSkylanderPortal", emulated_usb_devices.emulate_skylander_portal.GetValue());
	usbdevices.set("EmulateInfinityBase", emulated_usb_devices.emulate_infinity_base.GetValue());
	usbdevices.set("EmulateDimensionsToypad", emulated_usb_devices.emulate_dimensions_toypad.GetValue());

	return config;
}

GameEntry* CemuConfig::GetGameEntryByTitleId(uint64 titleId)
{
	// assumes game_cache_entries_mutex is already held
	for (auto& it : game_cache_entries)
	{
		if (it.title_id == titleId)
			return &it;
	}
	return nullptr;
}

GameEntry* CemuConfig::CreateGameEntry(uint64 titleId)
{
	// assumes game_cache_entries_mutex is already held
	GameEntry gameEntry;
	gameEntry.title_id = titleId;
	game_cache_entries.emplace_back(gameEntry);
	return &game_cache_entries.back();
}

bool CemuConfig::IsGameListFavorite(uint64 titleId)
{
	std::unique_lock _lock(game_cache_entries_mutex);
	return game_cache_favorites.find(titleId) != game_cache_favorites.end();
}

void CemuConfig::SetGameListFavorite(uint64 titleId, bool isFavorite)
{
	std::unique_lock _lock(game_cache_entries_mutex);
	GameEntry* gameEntry = GetGameEntryByTitleId(titleId);
	if (!gameEntry)
		gameEntry = CreateGameEntry(titleId);
	gameEntry->favorite = isFavorite;
	if (isFavorite)
		game_cache_favorites.emplace(titleId);
	else
		game_cache_favorites.erase(titleId);
}

bool CemuConfig::GetGameListCustomName(uint64 titleId, std::string& customName)
{
	std::unique_lock _lock(game_cache_entries_mutex);
	GameEntry* gameEntry = GetGameEntryByTitleId(titleId);
	if (gameEntry && !gameEntry->custom_name.empty())
	{
		customName = gameEntry->custom_name;
		return true;
	}
	return false;
}

void CemuConfig::SetGameListCustomName(uint64 titleId, std::string customName)
{
	std::unique_lock _lock(game_cache_entries_mutex);
	GameEntry* gameEntry = GetGameEntryByTitleId(titleId);
	if (!gameEntry)
	{
		if (customName.empty())
			return;
		gameEntry = CreateGameEntry(titleId);
	}
	gameEntry->custom_name = std::move(customName);
}

NetworkService CemuConfig::GetAccountNetworkService(uint32 persistentId)
{
	auto it = account.service_select.find(persistentId);
	if (it != account.service_select.end())
	{
		NetworkService serviceIndex = it->second;
		// make sure the returned service is valid
		if (serviceIndex != NetworkService::Offline &&
			serviceIndex != NetworkService::Nintendo &&
			serviceIndex != NetworkService::Pretendo &&
			serviceIndex != NetworkService::Custom &&
			serviceIndex != NetworkService::Plasma)
			return NetworkService::Offline;
		if (static_cast<NetworkService>(serviceIndex) == NetworkService::Custom && !NetworkConfig::XMLExists())
			return NetworkService::Offline; // custom is selected but no custom config exists
		return serviceIndex;
	}
	// if not found, return the legacy value
	if (!account.legacy_online_enabled)
		return NetworkService::Offline;
	return static_cast<NetworkService>(account.legacy_active_service.GetValue() + 1); // +1 because "Offline" now takes index 0
}

void CemuConfig::SetAccountSelectedService(uint32 persistentId, NetworkService serviceIndex)
{
	account.service_select[persistentId] = serviceIndex;
}
