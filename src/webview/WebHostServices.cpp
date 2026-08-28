#include "Common/precompiled.h"

#include "webview/WebHostServices.h"

#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "webview/NativeWindowHost.h"
#include "webview/WebHostState.h"

#include <future>

namespace WebFrontend
{
	WebHostServices::WebHostServices(std::shared_ptr<WebHostState> state,
									 INativeWindowHost& nativeWindow, UiDispatch dispatch, CanvasRecreator recreateCanvas)
		: m_state(std::move(state)), m_nativeWindow(nativeWindow),
		  m_dispatch(std::move(dispatch)), m_recreateCanvas(std::move(recreateCanvas)),
		  m_uiThread(std::this_thread::get_id()) {}

	void WebHostServices::Deactivate()
	{
		m_active.store(false, std::memory_order_release);
		ReleaseKeys();
		{
			std::scoped_lock lock(m_observerMutex);
			m_controllerObserver = {};
		}
		std::vector<std::shared_ptr<PendingOperation>> pending;
		{
			std::scoped_lock lock(m_pendingMutex);
			pending.swap(m_pendingOperations);
		}
		for (const auto& operation : pending)
			CancelPending(operation);
	}

	void WebHostServices::UpdateKey(std::uint32_t key, bool pressed)
	{
		if (!m_active.load(std::memory_order_acquire))
			return;
		std::scoped_lock lock(m_keyMutex);
		if (pressed)
			m_keys[key] = true;
		else
			m_keys.erase(key);
	}

	void WebHostServices::ReleaseKeys()
	{
		std::scoped_lock lock(m_keyMutex);
		m_keys.clear();
	}

	void WebHostServices::SetInputConfigurationFocused(bool focused)
	{
		m_inputConfigurationFocused.store(focused, std::memory_order_release);
	}

	void WebHostServices::SetControllerObserver(ControllerObserver observer)
	{
		std::scoped_lock lock(m_observerMutex);
		m_controllerObserver = std::move(observer);
	}

	Host::WindowMetricsSnapshot WebHostServices::GetWindowMetrics() const
	{
		return m_state->GetWindowMetrics();
	}

	std::filesystem::path WebHostServices::GetUserDataPath(std::string_view path) const
	{
		return ActiveSettings::GetUserDataPath(path);
	}
	std::filesystem::path WebHostServices::GetMlcPath() const
	{
		return ActiveSettings::GetMlcPath();
	}
	std::filesystem::path WebHostServices::GetExecutablePath() const
	{
		return ActiveSettings::GetExecutablePath();
	}
	std::filesystem::path WebHostServices::GetCachePath(std::string_view path) const
	{
		return ActiveSettings::GetCachePath(path);
	}
	std::filesystem::path WebHostServices::GetConfigPath(std::string_view path) const
	{
		return ActiveSettings::GetConfigPath(path);
	}
	Host::NativeSurfaceSnapshot WebHostServices::GetNativeSurfaces() const
	{
		return m_state->GetNativeSurfaces();
	}

	Host::NativeSurfacePublication WebHostServices::PublishMainWindow(Host::NativeWindowHandle handle)
	{
		return m_state->PublishMainWindow(handle);
	}
	void WebHostServices::ClearMainWindow(Host::NativeSurfacePublication publication)
	{
		m_state->ClearMainWindow(publication);
	}
	Host::NativeSurfacePublication WebHostServices::PublishPadWindow(Host::NativeWindowHandle handle)
	{
		return m_state->PublishPadWindow(handle);
	}
	void WebHostServices::ClearPadWindow(Host::NativeSurfacePublication publication)
	{
		m_state->ClearPadWindow(publication);
	}
	Host::NativeSurfacePublication WebHostServices::PublishCanvas(bool mainWindow, Host::NativeWindowHandle handle)
	{
		return m_state->PublishCanvas(mainWindow, handle);
	}
	void WebHostServices::ClearCanvas(bool mainWindow, Host::NativeSurfacePublication publication)
	{
		m_state->ClearCanvas(mainWindow, publication);
	}

	std::uint32_t WebHostServices::PlatformKey(Host::Key key) const
	{
#if BOOST_OS_WINDOWS
		switch (key)
		{
		case Host::Key::LeftControl:
			return VK_LCONTROL;
		case Host::Key::RightControl:
			return VK_RCONTROL;
		case Host::Key::Tab:
			return VK_TAB;
		case Host::Key::Escape:
			return VK_ESCAPE;
		}
#elif BOOST_OS_MACOS
		switch (key)
		{
		case Host::Key::LeftControl:
			return 0x3b;
		case Host::Key::RightControl:
			return 0x3e;
		case Host::Key::Tab:
			return 0x30;
		case Host::Key::Escape:
			return 0x35;
		}
#else
		switch (key)
		{
		case Host::Key::LeftControl:
			return 0xffe3;
		case Host::Key::RightControl:
			return 0xffe4;
		case Host::Key::Tab:
			return 0xff09;
		case Host::Key::Escape:
			return 0xff1b;
		}
#endif
		return 0;
	}

	bool WebHostServices::IsKeyDown(Host::Key key) const
	{
		const auto native = PlatformKey(key);
		std::scoped_lock lock(m_keyMutex);
		return native != 0 && m_keys.contains(native);
	}

	std::string WebHostServices::GetKeyName(std::uint32_t key) const
	{
		return m_nativeWindow.GetKeyName(key);
	}

	std::vector<Host::KeyState> WebHostServices::GetKeyStates() const
	{
		std::scoped_lock lock(m_keyMutex);
		std::vector<Host::KeyState> states;
		states.reserve(m_keys.size());
		for (const auto& [key, pressed] : m_keys)
			states.push_back({key, pressed});
		return states;
	}

	void WebHostServices::UpdateMousePosition(Host::PointerSurface surface, Host::PointerPosition position)
	{
		if (m_active.load(std::memory_order_acquire))
			InputManager::instance().UpdateHostMousePosition(surface, position);
	}
	void WebHostServices::UpdateMouseButton(Host::PointerSurface surface, Host::PointerButton button,
											bool pressed, Host::PointerPosition position)
	{
		if (m_active.load(std::memory_order_acquire))
			InputManager::instance().UpdateHostMouseButton(surface, button, pressed, position);
	}
	void WebHostServices::UpdateTouch(Host::PointerSurface surface, Host::PointerPosition position, bool pressed)
	{
		if (m_active.load(std::memory_order_acquire))
			InputManager::instance().UpdateHostTouch(surface, position, pressed);
	}
	void WebHostServices::UpdateMouseWheel(float value, std::int32_t cumulativeSteps)
	{
		if (m_active.load(std::memory_order_acquire))
			InputManager::instance().UpdateHostMouseWheel(value, cumulativeSteps);
	}
	void WebHostServices::NotifyDeviceChanged()
	{
		if (m_active.load(std::memory_order_acquire))
			InputManager::instance().on_device_changed();
	}

	bool WebHostServices::Queue(std::function<void()> action) const
	{
		return m_active.load(std::memory_order_acquire) && m_dispatch && m_dispatch(std::move(action));
	}

	std::shared_ptr<WebHostServices::PendingOperation> WebHostServices::RegisterPending(
		std::function<void()> cancelled)
	{
		auto pending = std::make_shared<PendingOperation>();
		pending->cancelled = std::move(cancelled);
		std::scoped_lock lock(m_pendingMutex);
		if (!m_active.load(std::memory_order_acquire))
		{
			pending->completed.store(true, std::memory_order_release);
			return pending;
		}
		m_pendingOperations.push_back(pending);
		return pending;
	}

	void WebHostServices::CancelPending(const std::shared_ptr<PendingOperation>& pending)
	{
		if (CompletePending(pending) && pending->cancelled)
			pending->cancelled();
	}

	bool WebHostServices::CompletePending(const std::shared_ptr<PendingOperation>& pending)
	{
		if (!pending || pending->completed.exchange(true, std::memory_order_acq_rel))
			return false;
		std::scoped_lock lock(m_pendingMutex);
		std::erase(m_pendingOperations, pending);
		return true;
	}

	void WebHostServices::GetTextAsync(GetTextCallback callback)
	{
		auto shared = std::make_shared<GetTextCallback>(std::move(callback));
		auto pending = RegisterPending([shared] { (*shared)(false, {}); });
		if (pending->completed.load(std::memory_order_acquire))
		{
			(*shared)(false, {});
			return;
		}
		if (!Queue([this, pending, shared] {
				if (CompletePending(pending))
				{
					auto [success, text] = m_nativeWindow.GetClipboardText();
					(*shared)(success, std::move(text));
				}
			}))
			CancelPending(pending);
	}

	void WebHostServices::SetTextAsync(std::string text, SetTextCallback callback)
	{
		auto shared = std::make_shared<SetTextCallback>(std::move(callback));
		auto pending = RegisterPending([shared] { (*shared)(false); });
		if (pending->completed.load(std::memory_order_acquire))
		{
			(*shared)(false);
			return;
		}
		if (!Queue([this, text = std::move(text), pending, shared]() mutable {
				if (CompletePending(pending))
					(*shared)(m_nativeWindow.SetClipboardText(std::move(text)));
			}))
			CancelPending(pending);
	}

	bool WebHostServices::OpenUrl(std::string url)
	{
		if (!url.starts_with("https://") && !url.starts_with("http://") &&
			!url.starts_with("mailto:"))
			return false;
		return Queue([this, url = std::move(url)]() mutable {
			(void)m_nativeWindow.OpenExternalUrl(std::move(url));
		});
	}

	bool WebHostServices::InputConfigurationHasFocus() const
	{
		return m_inputConfigurationFocused.load(std::memory_order_acquire);
	}

	bool WebHostServices::RecreateCanvas()
	{
		if (!m_active.load(std::memory_order_acquire) || !m_recreateCanvas)
			return false;
		if (std::this_thread::get_id() == m_uiThread)
			return m_recreateCanvas();
		auto result = std::make_shared<std::promise<bool>>();
		auto future = result->get_future();
		auto pending = RegisterPending([result] { result->set_value(false); });
		if (pending->completed.load(std::memory_order_acquire))
			return false;
		if (!Queue([this, result, pending] {
				if (!CompletePending(pending))
					return;
				try
				{
					result->set_value(m_recreateCanvas());
				} catch (...)
				{
					result->set_value(false);
				}
			}))
		{
			CancelPending(pending);
			return false;
		}
		return future.get();
	}

	void WebHostServices::OnControllerState(const ControllerState& current,
											const ControllerState& previous)
	{
		std::scoped_lock lock(m_observerMutex);
		if (m_active.load(std::memory_order_acquire) && m_controllerObserver)
			m_controllerObserver(current, previous);
	}
} // namespace WebFrontend
