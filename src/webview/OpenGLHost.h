#pragma once

#include "host/contracts/HostContracts.h"

#include <memory>

class OpenGLCanvasCallbacks;

namespace WebFrontend
{
	[[nodiscard]] std::unique_ptr<OpenGLCanvasCallbacks> CreateNativeOpenGLHost(
		Host::NativeWindowHandle surface,
		std::shared_ptr<Host::IWindowMetrics> windowMetrics);
}
