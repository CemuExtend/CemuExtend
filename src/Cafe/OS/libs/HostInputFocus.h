#pragma once

#include <memory>

namespace Host { class IInputFocus; }

namespace CafeHost
{
	void ConfigureInputFocus(std::shared_ptr<Host::IInputFocus> inputFocus);
	[[nodiscard]] bool InputConfigurationHasFocus();
}
