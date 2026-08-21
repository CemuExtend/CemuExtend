#include "wxgui/WxWindowState.h"

#include <wx/thread.h>

namespace
{
	struct NativeHandleLease
	{
		NativeHandleLease(std::shared_ptr<const WxWindowState> state,
						  std::shared_mutex& mutex)
			: state(std::move(state)), lock(mutex) {}
		std::shared_ptr<const WxWindowState> state;
		std::shared_lock<std::shared_mutex> lock;
	};
} // namespace

void WxWindowState::SetKeyState(std::uint32_t keycode, bool state)
{
	std::scoped_lock lock(m_keyMutex);
	m_keyDown[keycode] = state;
}

void WxWindowState::ReleaseKeyStates()
{
	std::scoped_lock lock(m_keyMutex);
	for (auto& [_, pressed] : m_keyDown)
		pressed = false;
}

bool WxWindowState::IsKeyDown(std::uint32_t keycode) const
{
	std::scoped_lock lock(m_keyMutex);
	const auto found = m_keyDown.find(keycode);
	return found != m_keyDown.end() && found->second;
}

std::vector<Host::KeyState> WxWindowState::KeyStates() const
{
	std::scoped_lock lock(m_keyMutex);
	std::vector<Host::KeyState> result;
	result.reserve(m_keyDown.size());
	for (const auto& [key, pressed] : m_keyDown)
		result.push_back({key, pressed});
	return result;
}

Host::WindowMetricsSnapshot WxWindowState::Metrics() const
{
	return {
		.appActive = app_active,
		.padOpen = pad_open,
		.fullscreen = is_fullscreen,
		.debuggerFocused = debugger_focused,
		.width = width,
		.height = height,
		.physicalWidth = phys_width,
		.physicalHeight = phys_height,
		.padWidth = pad_width,
		.padHeight = pad_height,
		.physicalPadWidth = phys_pad_width,
		.physicalPadHeight = phys_pad_height,
		.dpiScale = dpi_scale,
		.padDpiScale = pad_dpi_scale,
	};
}

Host::NativeSurfaceSnapshot WxWindowState::NativeSurfaces() const
{
	auto lease = std::make_shared<NativeHandleLease>(shared_from_this(), m_nativeMutex);
	return {
		.mainWindow = m_mainWindow,
		.padWindow = m_padWindow,
		.mainSurface = m_mainCanvas,
		.padSurface = m_padCanvas,
		.lifetime = std::move(lease),
	};
}

Host::NativeSurfacePublication WxWindowState::PublishMainWindow(
	Host::NativeWindowHandle handle)
{
	std::unique_lock lock(m_nativeMutex);
	m_mainWindow = handle;
	return m_mainWindowPublication = m_nextPublication.fetch_add(1);
}

void WxWindowState::ClearMainWindow(Host::NativeSurfacePublication publication)
{
	std::unique_lock lock(m_nativeMutex);
	if (m_mainWindowPublication == publication)
		m_mainWindow = {};
}

Host::NativeSurfacePublication WxWindowState::PublishPadWindow(
	Host::NativeWindowHandle handle)
{
	std::unique_lock lock(m_nativeMutex);
	m_padWindow = handle;
	return m_padWindowPublication = m_nextPublication.fetch_add(1);
}

void WxWindowState::ClearPadWindow(Host::NativeSurfacePublication publication)
{
	std::unique_lock lock(m_nativeMutex);
	if (m_padWindowPublication == publication)
		m_padWindow = {};
}

Host::NativeSurfacePublication WxWindowState::PublishCanvas(bool mainWindow,
															Host::NativeWindowHandle handle)
{
	std::unique_lock lock(m_nativeMutex);
	auto& current = mainWindow ? m_mainCanvas : m_padCanvas;
	auto& publication = mainWindow ? m_mainCanvasPublication : m_padCanvasPublication;
	current = handle;
	return publication = m_nextPublication.fetch_add(1);
}

void WxWindowState::ClearCanvas(bool mainWindow,
								Host::NativeSurfacePublication publication)
{
	std::unique_lock lock(m_nativeMutex);
	auto& current = mainWindow ? m_mainCanvas : m_padCanvas;
	const auto currentPublication = mainWindow ? m_mainCanvasPublication : m_padCanvasPublication;
	if (currentPublication == publication)
		current = {};
}

void WxMainWindowRegistry::Register(MainWindow& window,
									std::weak_ptr<std::atomic_bool> lifetime)
{
	std::scoped_lock lock(m_mutex);
	m_window = &window;
	m_lifetime = std::move(lifetime);
}

void WxMainWindowRegistry::Unregister(MainWindow& window)
{
	std::scoped_lock lock(m_mutex);
	if (m_window == &window)
	{
		m_window = nullptr;
		m_lifetime.reset();
	}
}

bool WxMainWindowRegistry::InvokeForUi(
	const std::function<void(MainWindow&)>& callback) const
{
	cemu_assert_debug(wxIsMainThread());
	MainWindow* window{};
	std::shared_ptr<std::atomic_bool> lifetime;
	{
		std::scoped_lock lock(m_mutex);
		lifetime = m_lifetime.lock();
		if (lifetime && lifetime->load(std::memory_order_acquire))
			window = m_window;
	}
	if (!window)
		return false;
	callback(*window);
	return true;
}
