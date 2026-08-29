#pragma once

#include "host/contracts/HostContracts.h"
#include "webview/NativeWindowHost.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace WebFrontend::CefOverlay
{
	int ExecuteSubprocess(int argc, char* argv[]);
	// macOS helper app entry points must use this so the versioned CEF framework
	// is loaded with LoadInHelper. It aliases ExecuteSubprocess elsewhere.
	int ExecuteHelperSubprocess(int argc, char* argv[]);
	bool InitializeProcessRuntime();
	void DoProcessMessageLoopWork();
	void ShutdownProcessRuntime();
	[[nodiscard]] bool IsProcessRuntimeInitialized();

	enum class BrowserPresentation : std::uint8_t
	{
		OverlayOsr,
		NativeChild,
	};

	struct BrowserAssetBundle
	{
		std::string originId;
		std::map<std::string, std::string> viewEntries;
		std::map<std::string, std::vector<std::byte>> assets;
		std::vector<std::string> connectOrigins;
		std::vector<std::string> resourceOrigins;
		std::string networkPolicyKey;
		bool credentials{};
		bool persistentStorage{};
		bool allowPrivateNetwork{};
	};

	struct BrowserDescriptor
	{
		std::uint64_t windowId{};
		std::string role{"main-library"};
		// Optional complete Bootstrap JSON object. When empty, the runtime builds a
		// minimal object from windowId, role, contextJson, and overlaySurface.
		std::string bootstrapJson;
		// Must contain a JSON object. It is exposed as
		// window.__CEMU_BOOTSTRAP__.context before document scripts run.
		std::string contextJson{"{}"};
		std::string initialUrl{"cemu://ui/index.html"};
		BrowserPresentation presentation{BrowserPresentation::NativeChild};
		std::optional<Host::PointerSurface> overlaySurface;
		void* nativeParent{};
		Host::RenderRegionBounds bounds{0, 0, 1, 1};
		double dpiScale{1.0};
		// NativeChild lifecycle hooks. The opaque value is CEF's child window
		// handle and is valid only between these two callbacks.
		std::function<void(void*)> nativeBrowserCreated;
		std::function<void(void*)> nativeBrowserClosing;
		std::shared_ptr<const BrowserAssetBundle> cemodAssets;
		std::function<void(std::int64_t, std::string,
			std::function<void(bool, std::string)>)> cemodQuery;
		std::function<void(std::int64_t)> cemodQueryCancelled;
		std::function<void()> loadReady;
		std::function<void()> closed;
	};

	class BrowserRuntime : public Host::IOverlayFrameSource
	{
	  public:
		using RpcHandler = std::function<std::string(std::uint64_t, std::string_view)>;
		using ClosedHandler = std::function<void(Host::PointerSurface)>;
		using WindowClosedHandler = std::function<void(std::uint64_t)>;

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

		// Registry API used by launcher/tool windows. The legacy surface API above
		// remains available for the Vulkan Main/Pad overlay integration.
		virtual bool CreateBrowser(const BrowserDescriptor& descriptor) = 0;
		virtual bool CloseWindow(std::uint64_t windowId) = 0;
		virtual void ResizeWindow(std::uint64_t windowId, int width, int height,
			double dpiScale) = 0;
		virtual void SetWindowFocus(std::uint64_t windowId, bool focused) = 0;
		virtual void ExecuteWindowEvent(std::uint64_t windowId, std::string_view name,
			std::string_view jsonPayload, std::uint64_t sequence) = 0;
		virtual void ExecuteCemodEvent(std::uint64_t windowId, std::string_view name,
			std::string_view jsonPayload) = 0;
		virtual void ExecuteWindowScript(std::uint64_t windowId, std::string_view script) = 0;
		[[nodiscard]] virtual bool HasWindow(std::uint64_t windowId) const = 0;
	};

	[[nodiscard]] std::shared_ptr<BrowserRuntime> CreateBrowserRuntime(
		BrowserRuntime::RpcHandler rpc, std::function<void(Host::PointerSurface)> redraw,
		BrowserRuntime::ClosedHandler closed = {},
		BrowserRuntime::WindowClosedHandler windowClosed = {});
} // namespace WebFrontend::CefOverlay
