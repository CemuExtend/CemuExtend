#include "Common/precompiled.h"

#include "webview/RendererHost.h"
#include "webview/OpenGLHost.h"

#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
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
						 std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher,
						 std::function<void(bool)> framePresented)
				: m_windowMetrics(std::move(windowMetrics)),
				  m_nativeSurfaces(std::move(nativeSurfaces)),
				  m_nativeSurfacePublisher(std::move(nativeSurfacePublisher)),
				  m_framePresented(std::move(framePresented))
			{
				static std::once_flag initialization;
				std::call_once(initialization, [] {
					LatteOverlay_init();
#ifdef ENABLE_VULKAN
					(void)InitializeGlobalVulkan();
#endif
				});
			}

			~RendererHost() override
			{
				PreparePadDestroy();
				PrepareMainDestroy();
			}

			void InitializeMain(Host::IRenderRegion& region) override
			{
				if (m_mainPublication)
					throw std::logic_error("the main renderer surface is already initialized");
				const auto bounds = region.GetBounds();
				if (bounds.width <= 0 || bounds.height <= 0)
					throw std::runtime_error("the native render region has no drawable area");
				m_mainPublication = m_nativeSurfacePublisher->PublishCanvas(
					true, region.GetSurfaceHandle());
				const auto metrics = m_windowMetrics->GetWindowMetrics();
				try
				{
					switch (ActiveSettings::GetGraphicsAPI())
					{
#ifdef ENABLE_VULKAN
					case kVulkan:
						if (!g_vulkan_available)
							throw std::runtime_error("the Vulkan loader is unavailable");
						g_renderer = std::make_unique<VulkanRenderer>(
							m_windowMetrics, m_nativeSurfaces, m_framePresented);
						VulkanRenderer::GetInstance()->InitializeSurface(
							{metrics.physicalWidth > 0 ? metrics.physicalWidth : bounds.width,
							 metrics.physicalHeight > 0 ? metrics.physicalHeight : bounds.height},
							true);
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
						m_openGLHost = CreateNativeOpenGLHost(
							region.GetSurfaceHandle(), m_windowMetrics);
						g_renderer = std::make_unique<OpenGLRenderer>(
							m_windowMetrics, m_nativeSurfaces);
						m_api = kOpenGL;
						break;
#endif
					default:
						throw std::runtime_error("the configured graphics API is unavailable");
					}
				} catch (...)
				{
					m_nativeSurfacePublisher->ClearCanvas(true, m_mainPublication);
					m_mainPublication = {};
					g_renderer.reset();
#ifdef ENABLE_OPENGL
					m_openGLHost.reset();
#endif
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
#ifdef ENABLE_OPENGL
				m_openGLHost.reset();
#endif
			}

			void AbandonMainInitialization() override
			{
#ifdef ENABLE_OPENGL
				if (m_api == kOpenGL && m_openGLHost)
					g_renderer.reset();
#endif
				PrepareMainDestroy();
				if (g_renderer)
					g_renderer.reset();
			}

			void InitializePad(Host::IRenderRegion& region) override
			{
				if (m_padSurfacePublication || m_padWindowPublication)
					throw std::logic_error("the GamePad renderer surface is already initialized");
				if (!g_renderer)
					throw std::logic_error("the main renderer must be initialized before the GamePad surface");
				const auto bounds = region.GetBounds();
				if (bounds.width <= 0 || bounds.height <= 0)
					throw std::runtime_error("the native GamePad render region has no drawable area");
				m_padWindowPublication = m_nativeSurfacePublisher->PublishPadWindow(
					region.GetWindowHandle());
				m_padSurfacePublication = m_nativeSurfacePublisher->PublishCanvas(
					false, region.GetSurfaceHandle());
				try
				{
#ifdef ENABLE_OPENGL
					if (m_api == kOpenGL)
						m_openGLHost->AttachPad(region.GetSurfaceHandle());
#endif
					if (m_api == kMetal)
					{
#ifdef ENABLE_METAL
						LatteAsyncCommands_runWithRendererPaused([bounds] {
							MetalRenderer::GetInstance()->InitializeLayer(
								{bounds.width, bounds.height}, false);
						});
#else
						throw std::runtime_error("the configured Metal backend is unavailable");
#endif
					}
					else
					{
						const auto metrics = m_windowMetrics->GetWindowMetrics();
						LatteAsyncCommands_runOnRendererThread([this, bounds, metrics] {
							switch (m_api)
							{
#ifdef ENABLE_VULKAN
							case kVulkan:
								VulkanRenderer::GetInstance()->InitializeSurface(
									{metrics.physicalPadWidth > 0 ? metrics.physicalPadWidth : bounds.width,
									 metrics.physicalPadHeight > 0 ? metrics.physicalPadHeight : bounds.height},
									false);
								break;
#endif
#ifdef ENABLE_OPENGL
							case kOpenGL:
								m_openGLHost->ActivatePad();
								break;
#endif
							default:
								throw std::runtime_error("the configured GamePad graphics API is unavailable");
							}
						});
					}
					m_padRendererInitialized = true;
				} catch (...)
				{
#ifdef ENABLE_OPENGL
					if (m_api == kOpenGL && m_openGLHost)
						m_openGLHost->DetachPad();
#endif
					if (m_padSurfacePublication)
						m_nativeSurfacePublisher->ClearCanvas(false, m_padSurfacePublication);
					if (m_padWindowPublication)
						m_nativeSurfacePublisher->ClearPadWindow(m_padWindowPublication);
					m_padSurfacePublication = {};
					m_padWindowPublication = {};
					throw;
				}
			}

			void PreparePadDestroy() override
			{
				if (!m_padSurfacePublication && !m_padWindowPublication)
					return;
				if (m_padRendererInitialized)
				{
					try
					{
						if (m_api == kMetal)
						{
#ifdef ENABLE_METAL
							LatteAsyncCommands_runWithRendererPaused([] {
								MetalRenderer::GetInstance()->ShutdownLayer(false);
							});
#endif
						}
						else
							LatteAsyncCommands_runOnRendererThread([this] {
								switch (m_api)
								{
#ifdef ENABLE_VULKAN
								case kVulkan:
									VulkanRenderer::GetInstance()->ShutdownPadSurface();
									break;
#endif
#ifdef ENABLE_OPENGL
								case kOpenGL:
									m_openGLHost->DeactivatePad();
									break;
#endif
								default:
									break;
								}
							});
					} catch (...)
					{
						if (!Latte_GetStopSignal())
							throw;
						LatteThread_WaitUntilStopped();
					}
				}
#ifdef ENABLE_OPENGL
				if (m_api == kOpenGL && m_openGLHost)
					m_openGLHost->DetachPad();
#endif
				if (m_padSurfacePublication)
					m_nativeSurfacePublisher->ClearCanvas(false, m_padSurfacePublication);
				if (m_padWindowPublication)
					m_nativeSurfacePublisher->ClearPadWindow(m_padWindowPublication);
				m_padSurfacePublication = {};
				m_padWindowPublication = {};
				m_padRendererInitialized = false;
			}

		  private:
			std::shared_ptr<Host::IWindowMetrics> m_windowMetrics;
			std::shared_ptr<Host::INativeSurfaceProvider> m_nativeSurfaces;
			std::shared_ptr<Host::INativeSurfacePublisher> m_nativeSurfacePublisher;
			std::function<void(bool)> m_framePresented;
			Host::NativeSurfacePublication m_mainPublication{};
			Host::NativeSurfacePublication m_padWindowPublication{};
			Host::NativeSurfacePublication m_padSurfacePublication{};
			bool m_padRendererInitialized{};
			GraphicAPI m_api{kVulkan};
#ifdef ENABLE_OPENGL
			std::unique_ptr<INativeOpenGLHost> m_openGLHost;
#endif
		};
	} // namespace

	std::unique_ptr<IRendererHost> CreateRendererHost(
		std::shared_ptr<Host::IWindowMetrics> windowMetrics,
		std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces,
		std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher,
		std::function<void(bool)> framePresented)
	{
		return std::make_unique<RendererHost>(std::move(windowMetrics),
											  std::move(nativeSurfaces), std::move(nativeSurfacePublisher),
											  std::move(framePresented));
	}
} // namespace WebFrontend
