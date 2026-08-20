#pragma once

#if BOOST_OS_WINDOWS
#include "input/api/DirectInput/DirectInputControllerProvider.h"
#include "input/api/XInput/XInputControllerProvider.h"
#endif

#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/WiimoteControllerProvider.h"
#endif

#ifdef HAS_SDL
#include "input/api/SDL/SDLControllerProvider.h"
#endif

#include "input/api/Keyboard/KeyboardControllerProvider.h"
#include "input/api/DSU/DSUControllerProvider.h"
#include "input/api/GameCube/GameCubeControllerProvider.h"

#include "input/emulated/VPADController.h"
#include "input/emulated/WPADController.h"
#include "input/InputHost.h"
#include "host/contracts/HostContracts.h"

#include "util/helpers/Singleton.h"

class InputManager : public Singleton<InputManager>
{
	friend class Singleton<InputManager>;
	InputManager();
	~InputManager();

public:
	constexpr static size_t kMaxController = 8;
	constexpr static size_t kMaxVPADControllers = 2;
	constexpr static size_t kMaxWPADControllers = 7;
	
	void load() noexcept;
	bool load(size_t player_index, std::string_view filename = {});

	bool migrate_config(const fs::path& file_path);

	void save() noexcept;
	bool save(size_t player_index, std::string_view filename = {});

	bool is_gameprofile_set(size_t player_index) const;

	void Shutdown(); 
	void Start();
	void ConfigureHost(Host::IKeyboardState& keyboard, Host::IWindowMetrics& windowMetrics,
		Host::INativeSurfaceProvider& nativeSurfaces, Input::IControllerStateObserver& observer);
	void ClearHost();
	void ConfigureEmulationContext(Input::IEmulationInputContext& context);
	void ClearEmulationContext(Input::IEmulationInputContext& context);
	void NotifyControllerState(const ControllerState& current, const ControllerState& previous);
	[[nodiscard]] std::string GetHostKeyName(std::uint32_t key) const;
	[[nodiscard]] std::vector<Host::KeyState> GetHostKeyStates() const;
	[[nodiscard]] std::vector<std::uint32_t> GetPressedHostKeys() const;
	[[nodiscard]] bool IsHostDebuggerFocused() const;
	[[nodiscard]] bool IsHostKeyDown(Host::Key key) const;
	[[nodiscard]] Host::WindowMetricsSnapshot GetHostWindowMetrics() const;
	[[nodiscard]] Host::NativeSurfaceSnapshot GetHostNativeSurfaces() const;
	[[nodiscard]] bool IsTitleRunningForInput() const;
	[[nodiscard]] Input::ScreenImageArea GetScreenImageArea(bool padView) const;
	
	EmulatedControllerPtr set_controller(EmulatedControllerPtr controller);
	EmulatedControllerPtr set_controller(size_t player_index, EmulatedController::Type type);
	EmulatedControllerPtr set_controller(size_t player_index, EmulatedController::Type type, const std::shared_ptr<ControllerBase>& controller);

	EmulatedControllerPtr delete_controller(size_t player_index, bool delete_profile = false);
	
	EmulatedControllerPtr get_controller(size_t player_index) const;
	std::shared_ptr<VPADController> get_vpad_controller(size_t index) const;
	std::shared_ptr<WPADController> get_wpad_controller(size_t index) const;
	std::pair<size_t, size_t> get_controller_count() const;

	bool is_api_available(InputAPI::Type api) const { return !m_api_available[api].empty(); }
	
	ControllerProviderPtr get_api_provider(std::string_view api_name) const;
	ControllerProviderPtr get_api_provider(InputAPI::Type api) const;
	// will create the provider with the given settings if it doesn't exist yet
	ControllerProviderPtr get_api_provider(InputAPI::Type api, const ControllerProviderSettings& settings);

	const auto& get_api_providers() const
	{
		return m_api_available;
	}

	void apply_game_profile();
	void on_device_changed();
	void UpdateHostMousePosition(Host::PointerSurface surface,
		Host::PointerPosition position);
	void UpdateHostMouseButton(Host::PointerSurface surface, Host::PointerButton button,
		bool pressed, Host::PointerPosition position);
	void UpdateHostTouch(Host::PointerSurface surface, Host::PointerPosition position,
		bool pressed);
	void UpdateHostMouseWheel(float value, std::int32_t cumulativeSteps);
	[[nodiscard]] float ConsumeHostMouseWheel();


	static std::vector<std::string> get_profiles();
	static bool is_valid_profilename(const std::string& name);

	glm::ivec2 get_mouse_position(bool pad_window) const;
	std::optional<glm::ivec2> get_left_down_mouse_info(bool* is_pad);
	std::optional<glm::ivec2> get_right_down_mouse_info(bool* is_pad);

private:
	struct MouseInfo
	{
		mutable std::shared_mutex m_mutex;
		glm::ivec2 position{};
		bool left_down = false;
		bool right_down = false;

		bool left_down_toggle = false;
		bool right_down_toggle = false;
	} m_main_mouse{}, m_pad_mouse{}, m_main_touch{}, m_pad_touch{};

	// m_mouse_wheel is the transient value consumed by VPADController. The
	// bridge publishes the cumulative counter so a reset to zero cannot create
	// a synthetic reverse-wheel event or hide a pulse between state samples.
	std::atomic<float> m_mouse_wheel{};
	std::atomic<sint32> m_mouse_wheel_cumulative{};

	void update_thread();

	std::thread m_update_thread;
	std::atomic<bool> m_update_thread_shutdown{false};

	std::array<std::vector<ControllerProviderPtr>, InputAPI::MAX> m_api_available{ };

	mutable std::shared_mutex m_mutex;
	std::array<EmulatedControllerPtr, kMaxVPADControllers> m_vpad;
	std::array<EmulatedControllerPtr, kMaxWPADControllers> m_wpad;

	std::array<bool, kMaxController> m_is_gameprofile_set{};

	mutable std::shared_mutex m_host_mutex;
	Host::IKeyboardState* m_keyboard_host{};
	Host::IWindowMetrics* m_window_metrics_host{};
	Host::INativeSurfaceProvider* m_native_surfaces_host{};
	Input::IControllerStateObserver* m_controller_observer{};
	Input::IEmulationInputContext* m_emulation_context{};

	template<std::derived_from<ControllerProviderBase> TProvider>
	void create_provider()
	{
		try
		{
			auto controller = std::make_shared<TProvider>();
			m_api_available[controller->api()] = std::vector<ControllerProviderPtr>{controller};
		} catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, ex.what());
		}
	}
};
