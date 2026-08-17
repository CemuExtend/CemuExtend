#include "Cafe/OS/libs/HostInputFocus.h"

#include "host/contracts/HostContracts.h"

#include <atomic>

namespace
{
	std::atomic<std::shared_ptr<Host::IInputFocus>> s_inputFocus;
}

void CafeHost::ConfigureInputFocus(std::shared_ptr<Host::IInputFocus> inputFocus)
{
	s_inputFocus.store(std::move(inputFocus), std::memory_order_release);
}

bool CafeHost::InputConfigurationHasFocus()
{
	if (auto inputFocus = s_inputFocus.load(std::memory_order_acquire))
		return inputFocus->InputConfigurationHasFocus();
	return false;
}
