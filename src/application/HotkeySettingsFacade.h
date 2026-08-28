#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Application
{
	inline constexpr std::uint32_t kHotkeyControllerButtonCount = 50;
	enum class HotkeyAction : std::uint8_t
	{
		ToggleFullscreen,
		ToggleFullscreenAlternative,
		ExitFullscreen,
		TakeScreenshot,
		ToggleFastForward,
		EndEmulation,
		ExitApplication,
	};

	struct HotkeyBinding
	{
		HotkeyAction action{};
		std::uint16_t keyboardUsage{};
		std::uint8_t keyboardModifiers{};
		std::optional<std::uint32_t> controllerButton;
		std::string controllerLabel;
	};

	struct HotkeyController
	{
		std::uint64_t token{};
		std::string displayName;
	};

	struct HotkeySettingsModel
	{
		std::uint64_t revision{};
		std::optional<std::uint32_t> controllerModifier;
		std::string controllerModifierLabel;
		std::optional<HotkeyController> controller;
		std::vector<HotkeyBinding> bindings;
	};

	struct HotkeySettingsUpdate
	{
		std::uint64_t revision{};
		std::optional<std::uint32_t> controllerModifier;
		std::vector<HotkeyBinding> bindings;
	};

	enum class HotkeySettingsError : std::uint8_t
	{
		None,
		Conflict,
		InvalidBinding,
		DuplicateBinding,
		SaveFailed,
	};

	struct HotkeySettingsResult
	{
		HotkeySettingsError error{HotkeySettingsError::None};
		HotkeySettingsModel snapshot;
		std::string diagnostic;
		[[nodiscard]] explicit operator bool() const
		{
			return error == HotkeySettingsError::None;
		}
	};

	[[nodiscard]] HotkeySettingsError ValidateHotkeySettingsUpdate(
		const HotkeySettingsUpdate& update, bool endEmulationAvailable,
		std::string& diagnostic);

	class IHotkeySettingsService
	{
	  public:
		virtual ~IHotkeySettingsService() = default;
		[[nodiscard]] virtual HotkeySettingsModel GetHotkeySettings() const = 0;
		[[nodiscard]] virtual HotkeySettingsResult ApplyHotkeySettings(
			const HotkeySettingsUpdate& update) = 0;
	};
} // namespace Application
