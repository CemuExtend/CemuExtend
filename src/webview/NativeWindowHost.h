#pragma once

#include "host/contracts/HostContracts.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

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

	enum class NativeInputKind : std::uint8_t
	{
		PointerMove,
		PointerButton,
		PointerWheel,
		Touch,
		Key,
		Character,
		FocusLost,
		DeviceChanged,
		RawMouse,
		TextComposition,
	};

	struct NativeInputEvent
	{
		NativeInputKind kind{};
		Host::PointerSurface surface{Host::PointerSurface::Main};
		std::int32_t x{};
		std::int32_t y{};
		std::int32_t deltaX{};
		std::int32_t deltaY{};
		std::int32_t wheelX{};
		std::int32_t wheelY{};
		std::int32_t contentWidth{};
		std::int32_t contentHeight{};
		std::uint32_t key{};
		std::uint16_t usage{};
		std::uint8_t modifiers{};
		std::uint32_t button{};
		std::uint32_t touchId{};
		bool pressed{};
		bool repeat{};
		bool insideContent{};
		std::string text;
		std::string preedit;
		std::uint32_t textCursor{};
		std::uint32_t selectionLength{};
		std::uint64_t textSequence{};
	};

	struct NativePointerPresentation
	{
		Host::PointerSurface surface{Host::PointerSurface::Main};
		bool ownsPointer{};
		bool showCursor{true};
		bool confine{};
		bool enteringCapture{};
		bool leavingPolicy{};
		bool requestRawMouse{};
		bool rawMouseEnabled{};
		std::uint8_t cursor{};
	};

	struct NativeTextInputRequest
	{
		bool active{};
		std::uint64_t sequence{};
		std::string initialText;
		std::uint32_t maximumLength{};
		std::int32_t caretX{};
		std::int32_t caretY{};
		std::int32_t lineHeight{};
	};

	class INativeWindowHost
	{
	public:
		using CloseHandler = std::function<void()>;
		using MenuHandler = std::function<void(MenuCommand)>;
		using MetricsHandler = std::function<void(Host::WindowMetricsSnapshot)>;
		using PadCloseHandler = std::function<void()>;
		using InputHandler = std::function<void(const NativeInputEvent&)>;
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
		[[nodiscard]] virtual Host::IRenderRegion& CreatePadRenderRegion() = 0;
		virtual void DestroyPadRenderRegion() = 0;
		[[nodiscard]] virtual bool IsPadRenderRegionOpen() const = 0;
		virtual void SetFullscreen(bool fullscreen) = 0;
		virtual void SetCloseHandler(CloseHandler handler) = 0;
		virtual void SetMenuHandler(MenuHandler handler) = 0;
		virtual void SetMetricsHandler(MetricsHandler handler) = 0;
		virtual void SetPadCloseHandler(PadCloseHandler handler) = 0;
		virtual void SetPadMetricsEnabled(bool enabled) = 0;
		virtual void SetInputHandler(InputHandler handler) = 0;
		virtual void ApplyPointerPresentation(const NativePointerPresentation& presentation) = 0;
		virtual void UpdateTextInput(const NativeTextInputRequest& request) = 0;
		[[nodiscard]] virtual std::string GetKeyName(std::uint32_t key) const = 0;
		[[nodiscard]] virtual std::pair<bool, std::string> GetClipboardText() = 0;
		[[nodiscard]] virtual bool SetClipboardText(std::string text) = 0;
		[[nodiscard]] virtual bool OpenExternalUrl(std::string url) = 0;
	};

	[[nodiscard]] std::unique_ptr<INativeWindowHost> CreateNativeWindowHost();
}
