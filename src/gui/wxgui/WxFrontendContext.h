#pragma once

#include "host/contracts/HostContracts.h"
#include "application/EmulationPresentation.h"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

class WxMainWindowRegistry;
class WxWindowState;

enum class WxFrontendErrorCategory
{
	KeysFileCreation,
	GraphicPacks,
};

class IWxUiDispatcher
{
public:
	virtual ~IWxUiDispatcher() = default;
	[[nodiscard]] virtual bool IsShuttingDown() const = 0;
	virtual void BeginShutdown() = 0;
	virtual void ResumeAfterFailedShutdown() = 0;
	[[nodiscard]] virtual bool Queue(std::function<void()> callback) = 0;
	[[nodiscard]] virtual bool Queue(std::function<void()> callback,
		std::function<void()> cancelled) = 0;
};

struct WxFrontendContext
{
	std::shared_ptr<Host::IWindowMetrics> windowMetrics;
	std::shared_ptr<Host::INativeSurfaceProvider> nativeSurfaces;
	std::shared_ptr<Host::INativeSurfacePublisher> nativeSurfacePublisher;
	std::shared_ptr<Host::IKeyboardState> keyboardState;
	std::shared_ptr<Host::IInputHostEvents> inputHostEvents;
	std::shared_ptr<WxWindowState> windowState;
	std::shared_ptr<WxMainWindowRegistry> mainWindowRegistry;
	std::shared_ptr<IWxUiDispatcher> uiDispatcher;
	std::function<void(std::string_view, std::string_view,
		std::optional<WxFrontendErrorCategory>)> showErrorDialog;
	std::function<void(bool, bool, double,
		std::optional<Application::WindowTitlePresentation>)> updateWindowTitles;
	std::function<void()> releaseHostServices;
};
