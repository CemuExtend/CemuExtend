#pragma once

#include "application/EmulationPresentation.h"
#include "host/contracts/HostContracts.h"
#include "input/api/ControllerState.h"

namespace WindowSystem
{
	using WindowHandleInfo = Host::NativeWindowHandle;

	enum struct PlatformKeyCodes : uint32
	{
		LCONTROL,
		RCONTROL,
		TAB,
		ESCAPE,
	};

	enum class ErrorCategory
	{
		KEYS_TXT_CREATION = 0,
		GRAPHIC_PACKS = 1,
	};

	void ShowErrorDialog(std::string_view message, std::string_view title, std::optional<ErrorCategory> errorCategory = {});
	inline void ShowErrorDialog(std::string_view message, std::optional<ErrorCategory> errorCategory = {})
	{
		ShowErrorDialog(message, "", errorCategory);
	}

	std::shared_ptr<Host::IWindowMetrics> GetWindowMetricsHost();
	std::shared_ptr<Host::INativeSurfaceProvider> GetNativeSurfaceHost();
	std::shared_ptr<Host::INativeSurfacePublisher> GetNativeSurfacePublisher();
	// Prevents new asynchronous host work from being queued while wx tears down.
	[[nodiscard]] bool IsShuttingDown();
	void BeginShutdown();
	void ResumeAfterFailedShutdown();
	// Thread-safe temporary wx dispatch boundary. New work is rejected once
	// shutdown begins, atomically with respect to queue submission.
	[[nodiscard]] bool QueueUi(std::function<void()> callback);
	[[nodiscard]] bool QueueUi(std::function<void()> callback,
		std::function<void()> cancelled);
	void ReleaseHostServices();

	void UpdateWindowTitles(bool isIdle, bool isLoading, double fps,
		std::optional<Application::WindowTitlePresentation> presentation = std::nullopt);
	void GetWindowSize(int& w, int& h);
	void GetPadWindowSize(int& w, int& h);
	void GetWindowPhysSize(int& w, int& h);
	void GetPadWindowPhysSize(int& w, int& h);
	double GetWindowDPIScale();
	double GetPadDPIScale();
	bool IsPadWindowOpen();
	bool IsKeyDown(uint32 key);
	bool IsKeyDown(PlatformKeyCodes key);
	std::string GetKeyCodeName(uint32 key);

	bool InputConfigWindowHasFocus();

	void NotifyGameLoaded();
	void NotifyGameExited();

	void RefreshGameList();

	bool IsFullScreen();

	void GetClipboardTextAsync(std::function<void(bool, std::string)> callback);
	void SetClipboardTextAsync(std::string text, std::function<void(bool)> callback);

	void CaptureInput(const ControllerState& currentState, const ControllerState& lastState);
}; // namespace WindowSystem
