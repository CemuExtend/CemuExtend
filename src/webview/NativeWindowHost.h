#pragma once

#include "host/contracts/HostContracts.h"

#include <functional>
#include <memory>

namespace WebFrontend
{
	enum class MenuCommand
	{
		Load,
		EndEmulation,
		Exit,
		ToggleFullscreen,
		TogglePadView,
		GeneralSettings,
		InputSettings,
		GraphicPacks,
		TitleManager,
		Logging,
		About,
	};

	class INativeWindowHost
	{
	public:
		using CloseHandler = std::function<void()>;
		using MenuHandler = std::function<void(MenuCommand)>;
		using MetricsHandler = std::function<void(Host::WindowMetricsSnapshot)>;
		virtual ~INativeWindowHost() = default;

		[[nodiscard]] virtual void* GetNativeWindow() const = 0;
		[[nodiscard]] virtual Host::NativeWindowHandle GetMainWindowHandle() const = 0;
		[[nodiscard]] virtual Host::WindowMetricsSnapshot GetMetrics() const = 0;
		virtual void AttachWebView(void* widget) = 0;
		virtual void PrepareWebViewDestroy(void* widget) = 0;
		virtual void Show() = 0;
		virtual void ShowLibrary() = 0;
		[[nodiscard]] virtual Host::IRenderRegion& CreateMainRenderRegion() = 0;
		virtual void DestroyMainRenderRegion() = 0;
		virtual void ShowRenderRegion() = 0;
		virtual void SetFullscreen(bool fullscreen) = 0;
		virtual void SetCloseHandler(CloseHandler handler) = 0;
		virtual void SetMenuHandler(MenuHandler handler) = 0;
		virtual void SetMetricsHandler(MetricsHandler handler) = 0;
	};

	[[nodiscard]] std::unique_ptr<INativeWindowHost> CreateNativeWindowHost();
}
