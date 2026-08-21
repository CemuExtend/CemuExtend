#include "application/HotkeySettingsFacade.h"

#include <array>
#include <set>

namespace Application
{
	HotkeySettingsError ValidateHotkeySettingsUpdate(
		const HotkeySettingsUpdate& update, bool endEmulationAvailable,
		std::string& diagnostic)
	{
		const std::size_t expectedBindingCount = endEmulationAvailable ? 7 : 6;
		if (update.bindings.size() != expectedBindingCount ||
			(update.controllerModifier &&
				*update.controllerModifier >= kHotkeyControllerButtonCount))
		{
			diagnostic = "hotkey binding set is incomplete or invalid";
			return HotkeySettingsError::InvalidBinding;
		}
		std::array<bool, 7> actions{};
		std::set<std::pair<std::uint16_t, std::uint8_t>> keyboardBindings;
		std::set<std::uint32_t> controllerBindings;
		for (const auto& binding : update.bindings)
		{
			const auto action = static_cast<std::size_t>(binding.action);
			if (action >= actions.size() || actions[action] ||
				(!endEmulationAvailable && binding.action == HotkeyAction::EndEmulation) ||
				binding.keyboardModifiers > 0x0f ||
				(binding.keyboardUsage == 0 && binding.keyboardModifiers != 0) ||
				(binding.keyboardUsage >= 0xe0 && binding.keyboardUsage <= 0xe7) ||
				(binding.controllerButton &&
					*binding.controllerButton >= kHotkeyControllerButtonCount))
			{
				diagnostic = "hotkey binding contains an invalid action, key, or button";
				return HotkeySettingsError::InvalidBinding;
			}
			actions[action] = true;
			if (binding.keyboardUsage && !keyboardBindings.emplace(
				binding.keyboardUsage, binding.keyboardModifiers).second)
			{
				diagnostic = "keyboard hotkeys must be unique";
				return HotkeySettingsError::DuplicateBinding;
			}
			if (binding.controllerButton &&
				((update.controllerModifier && *binding.controllerButton ==
					*update.controllerModifier) ||
				 !controllerBindings.emplace(*binding.controllerButton).second))
			{
				diagnostic = "controller hotkeys must be unique and differ from the modifier";
				return HotkeySettingsError::DuplicateBinding;
			}
		}
		for (std::size_t action = 0; action < actions.size(); ++action)
		{
			if (!endEmulationAvailable &&
				action == static_cast<std::size_t>(HotkeyAction::EndEmulation)) continue;
			if (!actions[action])
			{
				diagnostic = "hotkey binding set is incomplete";
				return HotkeySettingsError::InvalidBinding;
			}
		}
		diagnostic.clear();
		return HotkeySettingsError::None;
	}
}
