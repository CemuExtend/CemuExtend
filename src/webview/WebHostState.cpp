#include "webview/WebHostState.h"

#include <mutex>

namespace WebFrontend
{
	namespace
	{
		struct NativeSurfaceLease
		{
			NativeSurfaceLease(std::shared_ptr<const WebHostState> owner,
				std::shared_mutex& mutex)
				: owner(std::move(owner)), lock(mutex) {}
			std::shared_ptr<const WebHostState> owner;
			std::shared_lock<std::shared_mutex> lock;
		};
	}

	void WebHostState::UpdateMetrics(Host::WindowMetricsSnapshot metrics)
	{
		std::unique_lock lock(m_mutex);
		m_metrics = metrics;
	}

	Host::WindowMetricsSnapshot WebHostState::GetWindowMetrics() const
	{
		std::shared_lock lock(m_mutex);
		return m_metrics;
	}

	Host::NativeSurfaceSnapshot WebHostState::GetNativeSurfaces() const
	{
		auto lease = std::make_shared<NativeSurfaceLease>(shared_from_this(), m_mutex);
		return {
			.mainWindow = m_mainWindow,
			.padWindow = m_padWindow,
			.mainSurface = m_mainSurface,
			.padSurface = m_padSurface,
			.lifetime = std::move(lease),
		};
	}

	Host::NativeSurfacePublication WebHostState::NextPublication()
	{
		return m_nextPublication.fetch_add(1, std::memory_order_relaxed);
	}

	Host::NativeSurfacePublication WebHostState::PublishMainWindow(
		Host::NativeWindowHandle handle)
	{
		std::unique_lock lock(m_mutex);
		m_mainWindow = handle;
		return m_mainWindowPublication = NextPublication();
	}

	void WebHostState::ClearMainWindow(Host::NativeSurfacePublication publication)
	{
		std::unique_lock lock(m_mutex);
		if (m_mainWindowPublication == publication)
			m_mainWindow = {};
	}

	Host::NativeSurfacePublication WebHostState::PublishPadWindow(
		Host::NativeWindowHandle handle)
	{
		std::unique_lock lock(m_mutex);
		m_padWindow = handle;
		return m_padWindowPublication = NextPublication();
	}

	void WebHostState::ClearPadWindow(Host::NativeSurfacePublication publication)
	{
		std::unique_lock lock(m_mutex);
		if (m_padWindowPublication == publication)
			m_padWindow = {};
	}

	Host::NativeSurfacePublication WebHostState::PublishCanvas(bool mainWindow,
		Host::NativeWindowHandle handle)
	{
		std::unique_lock lock(m_mutex);
		auto& surface = mainWindow ? m_mainSurface : m_padSurface;
		auto& publication = mainWindow ? m_mainSurfacePublication : m_padSurfacePublication;
		surface = handle;
		return publication = NextPublication();
	}

	void WebHostState::ClearCanvas(bool mainWindow,
		Host::NativeSurfacePublication publication)
	{
		std::unique_lock lock(m_mutex);
		auto& surface = mainWindow ? m_mainSurface : m_padSurface;
		const auto current = mainWindow ? m_mainSurfacePublication : m_padSurfacePublication;
		if (current == publication)
			surface = {};
	}
}
