#pragma once

#include "host/contracts/HostContracts.h"

#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace WebFrontend
{
	class IToolWindowSupport
	{
	  public:
		virtual ~IToolWindowSupport() = default;
		[[nodiscard]] virtual void* GetWindow() const = 0;
		[[nodiscard]] virtual void* GetBrowserParentWindow() const = 0;
		[[nodiscard]] virtual Host::RenderRegionBounds GetBrowserBounds() const = 0;
		[[nodiscard]] virtual double GetBrowserDpiScale() const = 0;
		virtual void AttachBrowser(void* browserWindow) = 0;
		virtual void ResizeBrowser() = 0;
		virtual void FocusBrowser() = 0;
		virtual void DetachBrowser(void* browserWindow) = 0;
		virtual void SetSize(std::int32_t width, std::int32_t height) = 0;
		virtual void SetBounds(std::int32_t x, std::int32_t y,
							   std::int32_t width, std::int32_t height) = 0;
		virtual void SetMinimumSize(std::int32_t width, std::int32_t height) = 0;
		virtual void SetResizable(bool resizable) = 0;
		virtual void SetStateCallbacks(
			std::function<void(Host::RenderRegionBounds)> boundsChanged,
			std::function<void(bool)> focusChanged) = 0;
		virtual void SetTitle(std::string_view title) = 0;
		virtual void Show() = 0;
		virtual void Hide() = 0;
		virtual void Focus() = 0;
		[[nodiscard]] virtual std::optional<std::filesystem::path> PickDirectory(
			std::string_view title) = 0;
	};

	std::unique_ptr<IToolWindowSupport> CreateToolWindowSupport(
		void* parent, bool modal, std::function<void()> closeHandler);
} // namespace WebFrontend
