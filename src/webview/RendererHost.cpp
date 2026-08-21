#include "Common/precompiled.h"

#include "webview/RendererHost.h"

#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "config/ActiveSettings.h"

#ifdef ENABLE_OPENGL
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#endif
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#endif
#ifdef ENABLE_METAL
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#endif

#include <mutex>
#include <stdexcept>

namespace WebFrontend
{
	namespace
	{
		class RendererHost final : public IRendererHost
		{
		public:
			RendererHost(std::shared_ptr<Host::IWindowMetrics> windowMetrics,
				std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces,
				std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher)
				: m_windowMetrics(std::move(windowMetrics)),
				  m_nativeSurfaces(std::move(nativeSurfaces)),
				  m_nativeSurfacePublisher(std::move(nativeSurfacePublisher))
			{
				static std::once_flag initialization;
				std::call_once(initialization, [] {
					LatteOverlay_init();
#ifdef ENABLE_VULKAN
					(void)InitializeGlobalVulkan();
#endif
				});
			}

			~RendererHost() override { PrepareMainDestroy(); }

			void InitializeMain(Host::IRenderRegion& region) override
			{
				if (m_mainPublication)
					throw std::logic_error("the main renderer surface is already initialized");
				const auto bounds = region.GetBounds();
				if (bounds.width <= 0 || bounds.height <= 0)
					throw std::runtime_error("the native render region has no drawable area");
				m_mainPublication = m_nativeSurfacePublisher->PublishCanvas(
					true, region.GetSurfaceHandle());
				try
				{
					switch (ActiveSettings::GetGraphicsAPI())
					{
#ifdef ENABLE_VULKAN
					case kVulkan:
						if (!g_vulkan_available)
							throw std::runtime_error("the Vulkan loader is unavailable");
						g_renderer = std::make_unique<VulkanRenderer>(
							m_windowMetrics, m_nativeSurfaces);
						VulkanRenderer::GetInstance()->InitializeSurface(
							{bounds.width, bounds.height}, true);
						m_api = kVulkan;
						break;
#endif
#ifdef ENABLE_METAL
					case kMetal:
						g_renderer = std::make_unique<MetalRenderer>(
							m_windowMetrics, m_nativeSurfaces);
						MetalRenderer::GetInstance()->InitializeLayer(
							{bounds.width, bounds.height}, true);
						m_api = kMetal;
						break;
#endif
#ifdef ENABLE_OPENGL
					case kOpenGL:
						throw std::runtime_error(
							"OpenGL requires a native context adapter that is not initialized");
#endif
					default:
						throw std::runtime_error("the configured graphics API is unavailable");
					}
				}
				catch (...)
				{
					m_nativeSurfacePublisher->ClearCanvas(true, m_mainPublication);
					m_mainPublication = {};
					g_renderer.reset();
					throw;
				}
			}

			void PrepareMainDestroy() override
			{
				if (!m_mainPublication)
					return;
#ifdef ENABLE_METAL
				if (m_api == kMetal && g_renderer &&
					g_renderer->GetType() == RendererAPI::Metal)
					static_cast<MetalRenderer*>(g_renderer.get())->ShutdownLayer(true);
#endif
				m_nativeSurfacePublisher->ClearCanvas(true, m_mainPublication);
				m_mainPublication = {};
			}

			void AbandonMainInitialization() override
			{
				PrepareMainDestroy();
				g_renderer.reset();
			}

		private:
			std::shared_ptr<Host::IWindowMetrics> m_windowMetrics;
			std::shared_ptr<Host::INativeSurfaceProvider> m_nativeSurfaces;
			std::shared_ptr<Host::INativeSurfacePublisher> m_nativeSurfacePublisher;
			Host::NativeSurfacePublication m_mainPublication{};
			GraphicAPI m_api{kVulkan};
		};
	}

	std::unique_ptr<IRendererHost> CreateRendererHost(
		std::shared_ptr<Host::IWindowMetrics> windowMetrics,
		std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces,
		std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher)
	{
		return std::make_unique<RendererHost>(std::move(windowMetrics),
			std::move(nativeSurfaces), std::move(nativeSurfacePublisher));
	}
}
