#pragma once

#include "host/contracts/HostContracts.h"
#include "input/InputHost.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace WebFrontend
{
	class INativeWindowHost;
	class WebHostState;

	class WebHostServices final : public Host::IWindowMetrics,
								  public Host::IWindowControl,
								  public Host::IPathProvider,
								  public Host::INativeSurfaceProvider,
								  public Host::INativeSurfacePublisher,
								  public Host::IKeyboardState,
								  public Host::IInputHostEvents,
								  public Host::IClipboard,
								  public Host::IExternalLauncher,
								  public Host::IInputFocus,
								  public Host::ICanvasHost,
								  public Input::IControllerStateObserver
	{
	  public:
		using UiDispatch = std::function<bool(std::function<void()>)>;
		using CanvasRecreator = std::function<bool()>;
		using FullscreenSetter = std::function<void(bool)>;
		using ControllerObserver = std::function<void(const ControllerState&, const ControllerState&)>;

		WebHostServices(std::shared_ptr<WebHostState> state, INativeWindowHost& nativeWindow,
						UiDispatch dispatch, CanvasRecreator recreateCanvas,
						FullscreenSetter setFullscreen);

		void Deactivate();
		void UpdateKey(std::uint32_t key, bool pressed);
		void ReleaseKeys();
		void SetInputConfigurationFocused(bool focused);
		void SetControllerObserver(ControllerObserver observer);

		[[nodiscard]] Host::WindowMetricsSnapshot GetWindowMetrics() const override;
		[[nodiscard]] bool SetFullscreen(bool fullscreen) override;
		void SetFocusPaused(bool paused, std::uint64_t sequence) override;
		[[nodiscard]] std::filesystem::path GetUserDataPath(std::string_view relativePath) const override;
		[[nodiscard]] std::filesystem::path GetMlcPath() const override;
		[[nodiscard]] std::filesystem::path GetExecutablePath() const override;
		[[nodiscard]] std::filesystem::path GetCachePath(std::string_view relativePath) const override;
		[[nodiscard]] std::filesystem::path GetConfigPath(std::string_view relativePath) const override;
		[[nodiscard]] Host::NativeSurfaceSnapshot GetNativeSurfaces() const override;
		[[nodiscard]] Host::NativeSurfacePublication PublishMainWindow(Host::NativeWindowHandle handle) override;
		void ClearMainWindow(Host::NativeSurfacePublication publication) override;
		[[nodiscard]] Host::NativeSurfacePublication PublishPadWindow(Host::NativeWindowHandle handle) override;
		void ClearPadWindow(Host::NativeSurfacePublication publication) override;
		[[nodiscard]] Host::NativeSurfacePublication PublishCanvas(bool mainWindow,
																   Host::NativeWindowHandle handle) override;
		void ClearCanvas(bool mainWindow, Host::NativeSurfacePublication publication) override;
		[[nodiscard]] bool IsKeyDown(Host::Key key) const override;
		[[nodiscard]] std::string GetKeyName(std::uint32_t key) const override;
		[[nodiscard]] std::vector<Host::KeyState> GetKeyStates() const override;
		void UpdateMousePosition(Host::PointerSurface surface, Host::PointerPosition position) override;
		void UpdateMouseButton(Host::PointerSurface surface, Host::PointerButton button,
							   bool pressed, Host::PointerPosition position) override;
		void UpdateTouch(Host::PointerSurface surface, Host::PointerPosition position,
						 bool pressed) override;
		void UpdateMouseWheel(float value, std::int32_t cumulativeSteps) override;
		void NotifyDeviceChanged() override;
		void GetTextAsync(GetTextCallback callback) override;
		void SetTextAsync(std::string text, SetTextCallback callback) override;
		[[nodiscard]] bool OpenUrl(std::string url) override;
		[[nodiscard]] bool InputConfigurationHasFocus() const override;
		[[nodiscard]] bool RecreateCanvas() override;
		void OnControllerState(const ControllerState& current,
							   const ControllerState& previous) override;

	  private:
		struct PendingOperation
		{
			std::atomic_bool completed{};
			std::function<void()> cancelled;
		};
		[[nodiscard]] std::shared_ptr<PendingOperation> RegisterPending(
			std::function<void()> cancelled);
		[[nodiscard]] bool CompletePending(const std::shared_ptr<PendingOperation>& pending);
		void CancelPending(const std::shared_ptr<PendingOperation>& pending);
		[[nodiscard]] std::uint32_t PlatformKey(Host::Key key) const;
		[[nodiscard]] bool Queue(std::function<void()> action) const;

		std::shared_ptr<WebHostState> m_state;
		INativeWindowHost& m_nativeWindow;
		UiDispatch m_dispatch;
		CanvasRecreator m_recreateCanvas;
		FullscreenSetter m_setFullscreen;
		std::thread::id m_uiThread;
		std::atomic_bool m_active{true};
		std::atomic_bool m_inputConfigurationFocused{};
		mutable std::mutex m_keyMutex;
		std::unordered_map<std::uint32_t, bool> m_keys;
		mutable std::mutex m_observerMutex;
		ControllerObserver m_controllerObserver;
		mutable std::mutex m_pendingMutex;
		std::vector<std::shared_ptr<PendingOperation>> m_pendingOperations;
	};
} // namespace WebFrontend
