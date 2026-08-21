#include <boost/container/small_vector.hpp>

#include "input/api/Keyboard/KeyboardController.h"
#include "input/InputManager.h"

KeyboardController::KeyboardController()
	: base_type("keyboard", "Keyboard")
{
}

std::string KeyboardController::get_button_name(uint64 button) const
{
	return InputManager::instance().GetHostKeyName(button);
}

ControllerState KeyboardController::raw_state()
{
	ControllerState result{};

	auto& inputManager = InputManager::instance();
	if (inputManager.IsHostDebuggerFocused())
		return result;

	auto pressedKeys = inputManager.GetPressedHostKeys();
	result.buttons.SetPressedButtons(pressedKeys);
	return result;
}
