#pragma once

#include "host/contracts/HostContracts.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

class MainWindow;

class WxWindowState final : public std::enable_shared_from_this<WxWindowState>
{
  public:
	std::atomic_bool app_active{};
	std::atomic_int32_t width{}, height{};
	std::atomic_int32_t phys_width{}, phys_height{};
	std::atomic<double> dpi_scale{1.0};
	std::atomic_bool pad_open{};
	std::atomic_int32_t pad_width{}, pad_height{};
	std::atomic_int32_t phys_pad_width{}, phys_pad_height{};
	std::atomic<double> pad_dpi_scale{1.0};
	std::atomic_bool pad_maximized{};
	std::atomic_int32_t restored_pad_x{-1}, restored_pad_y{-1};
	std::atomic_int32_t restored_pad_width{-1}, restored_pad_height{-1};
	std::atomic_bool is_fullscreen{};
	std::atomic_bool debugger_focused{};

	void SetKeyState(std::uint32_t keycode, bool state);
	void ReleaseKeyStates();
	[[nodiscard]] bool IsKeyDown(std::uint32_t keycode) const;
	[[nodiscard]] std::vector<Host::KeyState> KeyStates() const;
	[[nodiscard]] Host::WindowMetricsSnapshot Metrics() const;
	[[nodiscard]] Host::NativeSurfaceSnapshot NativeSurfaces() const;

	[[nodiscard]] Host::NativeSurfacePublication PublishMainWindow(
		Host::NativeWindowHandle handle);
	void ClearMainWindow(Host::NativeSurfacePublication publication);
	[[nodiscard]] Host::NativeSurfacePublication PublishPadWindow(
		Host::NativeWindowHandle handle);
	void ClearPadWindow(Host::NativeSurfacePublication publication);
	[[nodiscard]] Host::NativeSurfacePublication PublishCanvas(bool mainWindow,
															   Host::NativeWindowHandle handle);
	void ClearCanvas(bool mainWindow, Host::NativeSurfacePublication publication);

  private:
	mutable std::mutex m_keyMutex;
	std::unordered_map<std::uint32_t, bool> m_keyDown;
	mutable std::shared_mutex m_nativeMutex;
	Host::NativeWindowHandle m_mainWindow;
	Host::NativeWindowHandle m_padWindow;
	Host::NativeWindowHandle m_mainCanvas;
	Host::NativeWindowHandle m_padCanvas;
	std::atomic_uint64_t m_nextPublication{1};
	Host::NativeSurfacePublication m_mainWindowPublication{};
	Host::NativeSurfacePublication m_padWindowPublication{};
	Host::NativeSurfacePublication m_mainCanvasPublication{};
	Host::NativeSurfacePublication m_padCanvasPublication{};
};

class WxMainWindowRegistry final
{
  public:
	void Register(MainWindow& window, std::weak_ptr<std::atomic_bool> lifetime);
	void Unregister(MainWindow& window);
	[[nodiscard]] bool InvokeForUi(const std::function<void(MainWindow&)>& callback) const;

  private:
	mutable std::mutex m_mutex;
	MainWindow* m_window{};
	std::weak_ptr<std::atomic_bool> m_lifetime;
};
