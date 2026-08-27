#pragma once

#include "host/contracts/HostContracts.h"
#include "webview/NativeWindowHost.h"

#include <functional>
#include <memory>
#include <string_view>

namespace WebFrontend::CefOverlay
{
	int ExecuteSubprocess(int argc, char* argv[]);
	bool InitializeProcessRuntime();
	void DoProcessMessageLoopWork();
	void ShutdownProcessRuntime();

	class BrowserRuntime : public Host::IOverlayFrameSource
	{
	  public:
		using RpcHandler = std::function<std::string(std::uint64_t, std::string_view)>;
		using ClosedHandler = std::function<void(Host::PointerSurface)>;

		virtual ~BrowserRuntime() = default;
		virtual bool Create(Host::PointerSurface surface, std::uint64_t windowId,
			int physicalWidth, int physicalHeight, double dpiScale) = 0;
		virtual void Close(Host::PointerSurface surface) = 0;
		virtual void CloseAll() = 0;
		virtual void Resize(Host::PointerSurface surface, int physicalWidth,
			int physicalHeight, double dpiScale) = 0;
		virtual void SetInteractive(Host::PointerSurface surface, bool interactive) = 0;
		virtual bool SendInput(const NativeInputEvent& event) = 0;
		virtual void ExecuteEvent(Host::PointerSurface surface, std::string_view name,
			std::string_view jsonPayload, std::uint64_t sequence) = 0;
		virtual void ExecuteScript(Host::PointerSurface surface, std::string_view script) = 0;
	};

	[[nodiscard]] std::shared_ptr<BrowserRuntime> CreateBrowserRuntime(
		BrowserRuntime::RpcHandler rpc, std::function<void(Host::PointerSurface)> redraw,
		BrowserRuntime::ClosedHandler closed = {});
} // namespace WebFrontend::CefOverlay
