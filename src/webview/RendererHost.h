#pragma once

#include "host/contracts/HostContracts.h"

#include <functional>
#include <memory>

namespace WebFrontend
{
	class IRendererHost
	{
	  public:
		virtual ~IRendererHost() = default;
		virtual void InitializeMain(Host::IRenderRegion& region) = 0;
		virtual void AbandonMainInitialization() = 0;
		virtual void PrepareMainDestroy() = 0;
		virtual void InitializePad(Host::IRenderRegion& region) = 0;
		virtual void PreparePadDestroy() = 0;
	};

	[[nodiscard]] std::unique_ptr<IRendererHost> CreateRendererHost(
		std::shared_ptr<Host::IWindowMetrics> windowMetrics,
		std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces,
		std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher,
		std::function<void(bool mainWindow)> framePresented = {});
} // namespace WebFrontend
