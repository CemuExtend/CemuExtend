#pragma once

#include "host/contracts/HostContracts.h"

#include <atomic>
#include <memory>
#include <shared_mutex>

namespace WebFrontend
{
	class WebHostState final : public Host::IWindowMetrics,
		public Host::INativeSurfaceProvider,
		public Host::INativeSurfacePublisher,
		public std::enable_shared_from_this<WebHostState>
	{
	public:
		void UpdateMetrics(Host::WindowMetricsSnapshot metrics);
		[[nodiscard]] Host::WindowMetricsSnapshot GetWindowMetrics() const override;
		[[nodiscard]] Host::NativeSurfaceSnapshot GetNativeSurfaces() const override;

		[[nodiscard]] Host::NativeSurfacePublication PublishMainWindow(
			Host::NativeWindowHandle handle) override;
		void ClearMainWindow(Host::NativeSurfacePublication publication) override;
		[[nodiscard]] Host::NativeSurfacePublication PublishPadWindow(
			Host::NativeWindowHandle handle) override;
		void ClearPadWindow(Host::NativeSurfacePublication publication) override;
		[[nodiscard]] Host::NativeSurfacePublication PublishCanvas(bool mainWindow,
			Host::NativeWindowHandle handle) override;
		void ClearCanvas(bool mainWindow,
			Host::NativeSurfacePublication publication) override;

	private:
		[[nodiscard]] Host::NativeSurfacePublication NextPublication();

		mutable std::shared_mutex m_mutex;
		Host::WindowMetricsSnapshot m_metrics;
		Host::NativeWindowHandle m_mainWindow;
		Host::NativeWindowHandle m_padWindow;
		Host::NativeWindowHandle m_mainSurface;
		Host::NativeWindowHandle m_padSurface;
		std::atomic_uint64_t m_nextPublication{1};
		Host::NativeSurfacePublication m_mainWindowPublication{};
		Host::NativeSurfacePublication m_padWindowPublication{};
		Host::NativeSurfacePublication m_mainSurfacePublication{};
		Host::NativeSurfacePublication m_padSurfacePublication{};
	};
}
