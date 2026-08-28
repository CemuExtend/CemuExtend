#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Application
{
	enum class EmulatedControllerType : std::uint8_t
	{
		Disabled,
		GamePad,
		ProController,
		ClassicController,
		Wiimote,
	};

	struct ControllerAxisSettings
	{
		float deadzone{0.25F};
		float range{1.0F};
	};

	struct PhysicalControllerSettings
	{
		ControllerAxisSettings axis;
		ControllerAxisSettings rotation;
		ControllerAxisSettings trigger;
		float rumble{};
		bool motion{};
		std::optional<std::uint32_t> packetDelay;
	};

	struct PhysicalControllerInfo
	{
		std::uint64_t token{};
		std::string api;
		std::string displayName;
		bool connected{};
		bool hasBattery{};
		bool lowBattery{};
		bool hasMotion{};
		bool hasRumble{};
		std::optional<std::string> wiimoteExtension;
		PhysicalControllerSettings settings;
	};
	struct CapturedInputButton
	{
		std::uint64_t id{};
		std::string label;
	};

	struct InputMappingInfo
	{
		std::uint64_t mappingId{};
		std::string label;
		std::string binding;
		std::optional<std::uint64_t> controllerToken;
	};

	struct InputPlayerInfo
	{
		std::uint32_t player{};
		EmulatedControllerType type{EmulatedControllerType::Disabled};
		bool gameProfileLocked{};
		std::string profileName;
		std::vector<PhysicalControllerInfo> controllers;
		std::vector<InputMappingInfo> mappings;
	};

	struct InputSettingsModel
	{
		std::uint64_t generation{};
		std::vector<InputPlayerInfo> players;
		std::vector<std::string> profiles;
		std::vector<std::string> availableApis;
	};

	struct InputDeviceCandidate
	{
		std::uint64_t token{};
		std::string api;
		std::string displayName;
		bool connected{};
	};

	enum class InputSettingsError : std::uint8_t
	{
		None,
		InvalidPlayer,
		UnsupportedType,
		UnsupportedApi,
		DeviceNotFound,
		InvalidMapping,
		InvalidSettings,
		ProfileLocked,
		InvalidProfile,
		PersistenceFailed,
		OperationFailed,
	};
	struct InputDeviceEnumerationResult
	{
		InputSettingsError error{InputSettingsError::None};
		std::string diagnostic;
		std::vector<InputDeviceCandidate> devices;
		[[nodiscard]] explicit operator bool() const
		{
			return error == InputSettingsError::None;
		}
	};

	struct InputSettingsResult
	{
		InputSettingsError error{InputSettingsError::None};
		std::string diagnostic;
		[[nodiscard]] explicit operator bool() const
		{
			return error == InputSettingsError::None;
		}
	};

	class IInputSettingsService
	{
	  public:
		virtual ~IInputSettingsService() = default;
		[[nodiscard]] virtual InputSettingsModel GetInputSettings() const = 0;
		[[nodiscard]] virtual InputDeviceEnumerationResult EnumerateInputDevices(
			std::string_view api) = 0;
		[[nodiscard]] virtual InputSettingsResult SetEmulatedController(
			std::uint32_t player, EmulatedControllerType type, bool preserveDevices) = 0;
		[[nodiscard]] virtual InputSettingsResult AddInputDevice(
			std::uint32_t player, std::uint64_t token) = 0;
		[[nodiscard]] virtual InputSettingsResult RemoveInputDevice(
			std::uint32_t player, std::uint64_t token) = 0;
		[[nodiscard]] virtual InputSettingsResult ConnectInputDevice(
			std::uint64_t token) = 0;
		[[nodiscard]] virtual std::optional<CapturedInputButton> CaptureInputButton(
			std::uint64_t token) = 0;
		[[nodiscard]] virtual InputSettingsResult SetInputMapping(
			std::uint32_t player, std::uint64_t mappingId,
			std::uint64_t controllerToken, std::uint64_t buttonId) = 0;
		[[nodiscard]] virtual InputSettingsResult ClearInputMapping(
			std::uint32_t player, std::optional<std::uint64_t> mappingId) = 0;
		[[nodiscard]] virtual InputSettingsResult SetPhysicalControllerSettings(
			std::uint64_t token, const PhysicalControllerSettings& settings) = 0;
		[[nodiscard]] virtual InputSettingsResult CalibrateInputDevice(
			std::uint64_t token) = 0;
		[[nodiscard]] virtual InputSettingsResult LoadInputProfile(
			std::uint32_t player, std::string_view profile) = 0;
		[[nodiscard]] virtual InputSettingsResult SaveInputProfile(
			std::uint32_t player, std::string_view profile) = 0;
		[[nodiscard]] virtual InputSettingsResult DeleteInputProfile(
			std::string_view profile) = 0;
	};
} // namespace Application
