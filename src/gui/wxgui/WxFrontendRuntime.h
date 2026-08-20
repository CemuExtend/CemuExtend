#pragma once

#include "application/EmulationPresentation.h"
#include "host/contracts/HostContracts.h"
#include "input/api/ControllerState.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class WxWindowState;
class WxMainWindowRegistry;

namespace WxFrontendRuntime
{
	enum struct PlatformKeyCodes : std::uint32_t
	{
		LCONTROL,
		RCONTROL,
		TAB,
		ESCAPE,
	};

	enum class ErrorCategory
	{
		KEYS_TXT_CREATION,
		GRAPHIC_PACKS,
	};

	std::shared_ptr<WxWindowState> GetWindowState();
	std::shared_ptr<WxMainWindowRegistry> GetMainWindowRegistry();
	std::shared_ptr<Host::IWindowMetrics> GetWindowMetricsHost();
	std::shared_ptr<Host::INativeSurfaceProvider> GetNativeSurfaceHost();
	std::shared_ptr<Host::INativeSurfacePublisher> GetNativeSurfacePublisher();

	[[nodiscard]] bool IsShuttingDown();
	void BeginShutdown();
	void ResumeAfterFailedShutdown();
	[[nodiscard]] bool QueueUi(std::function<void()> callback);
	[[nodiscard]] bool QueueUi(std::function<void()> callback,
		std::function<void()> cancelled);
	void ReleaseHostServices();

	void ShowErrorDialog(std::string_view message, std::string_view title,
		std::optional<ErrorCategory> errorCategory = {});
	inline void ShowErrorDialog(std::string_view message,
		std::optional<ErrorCategory> errorCategory = {})
	{
		ShowErrorDialog(message, "", errorCategory);
	}

	void UpdateWindowTitles(bool isIdle, bool isLoading, double fps,
		std::optional<Application::WindowTitlePresentation> presentation = std::nullopt);
	void GetWindowSize(int& width, int& height);
	void GetPadWindowSize(int& width, int& height);
	void GetWindowPhysSize(int& width, int& height);
	void GetPadWindowPhysSize(int& width, int& height);
	double GetWindowDPIScale();
	double GetPadDPIScale();
	bool IsPadWindowOpen();
	bool IsKeyDown(std::uint32_t key);
	bool IsKeyDown(PlatformKeyCodes key);
	std::string GetKeyCodeName(std::uint32_t key);
	bool InputConfigWindowHasFocus();
	void NotifyGameLoaded();
	void NotifyGameExited();
	void RefreshGameList();
	bool IsFullScreen();
	void GetClipboardTextAsync(std::function<void(bool, std::string)> callback);
	void SetClipboardTextAsync(std::string text, std::function<void(bool)> callback);
	void CaptureInput(const ControllerState& currentState,
		const ControllerState& lastState);
}
