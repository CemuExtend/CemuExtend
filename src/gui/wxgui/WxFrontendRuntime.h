#pragma once

#include <memory>

class WxWindowState;
class WxMainWindowRegistry;

namespace WxFrontendRuntime
{
	std::shared_ptr<WxWindowState> GetWindowState();
	std::shared_ptr<WxMainWindowRegistry> GetMainWindowRegistry();
}
