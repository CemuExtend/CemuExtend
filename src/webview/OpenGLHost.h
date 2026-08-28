#pragma once

#include "host/contracts/HostContracts.h"

#include <memory>

#ifdef ENABLE_OPENGL
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#endif

namespace WebFrontend
{
#ifdef ENABLE_OPENGL
	class INativeOpenGLHost : public OpenGLCanvasCallbacks
	{
	  public:
		virtual void AttachPad(Host::NativeWindowHandle surface) = 0;
		virtual void ActivatePad() = 0;
		virtual void DeactivatePad() = 0;
		virtual void DetachPad() = 0;
	};

	[[nodiscard]] std::unique_ptr<INativeOpenGLHost> CreateNativeOpenGLHost(
		Host::NativeWindowHandle surface,
		std::shared_ptr<Host::IWindowMetrics> windowMetrics);
#endif
} // namespace WebFrontend
