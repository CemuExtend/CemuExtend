#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Host
{
	using NativeSurfacePublication = std::uint64_t;
	enum class NativeWindowBackend : std::uint8_t
	{
		X11,
		Wayland,
		Cocoa,
		Windows,
	};

	struct NativeWindowHandle
	{
		using Backend = NativeWindowBackend;
		NativeWindowBackend backend{NativeWindowBackend::X11};
		void* display{};
		void* surface{};

		friend bool operator==(const NativeWindowHandle&, const NativeWindowHandle&) = default;
	};

	struct WindowMetricsSnapshot
	{
		bool appActive{};
		bool padOpen{};
		bool fullscreen{};
		bool debuggerFocused{};
		std::int32_t width{};
		std::int32_t height{};
		std::int32_t physicalWidth{};
		std::int32_t physicalHeight{};
		std::int32_t padWidth{};
		std::int32_t padHeight{};
		std::int32_t physicalPadWidth{};
		std::int32_t physicalPadHeight{};
		double dpiScale{1.0};
		double padDpiScale{1.0};
	};

	struct NativeSurfaceSnapshot
	{
		NativeWindowHandle mainWindow;
		NativeWindowHandle padWindow;
		NativeWindowHandle mainSurface;
		NativeWindowHandle padSurface;
		// Keeps toolkit-owned native objects alive while a consumer uses the raw
		// platform handle. The contract intentionally does not expose toolkit types.
		std::shared_ptr<const void> lifetime;
	};

	enum class Key : std::uint8_t
	{
		LeftControl,
		RightControl,
		Tab,
		Escape,
	};

	struct KeyState
	{
		std::uint32_t key{};
		bool pressed{};
	};

	enum class PointerSurface : std::uint8_t
	{
		Main,
		Pad,
	};

	enum class PointerButton : std::uint8_t
	{
		Left,
		Right,
	};

	struct PointerPosition
	{
		std::int32_t x{};
		std::int32_t y{};
	};

	class IWindowMetrics
	{
	public:
		virtual ~IWindowMetrics() = default;
		[[nodiscard]] virtual WindowMetricsSnapshot GetWindowMetrics() const = 0;
	};

	class INativeSurfaceProvider
	{
	public:
		virtual ~INativeSurfaceProvider() = default;
		[[nodiscard]] virtual NativeSurfaceSnapshot GetNativeSurfaces() const = 0;
	};

	class INativeSurfacePublisher
	{
	public:
		virtual ~INativeSurfacePublisher() = default;
		[[nodiscard]] virtual NativeSurfacePublication PublishMainWindow(
			NativeWindowHandle handle) = 0;
		virtual void ClearMainWindow(NativeSurfacePublication publication) = 0;
		[[nodiscard]] virtual NativeSurfacePublication PublishPadWindow(
			NativeWindowHandle handle) = 0;
		virtual void ClearPadWindow(NativeSurfacePublication publication) = 0;
		[[nodiscard]] virtual NativeSurfacePublication PublishCanvas(bool mainWindow,
			NativeWindowHandle handle) = 0;
		virtual void ClearCanvas(bool mainWindow,
			NativeSurfacePublication publication) = 0;
	};

	class IKeyboardState
	{
	public:
		virtual ~IKeyboardState() = default;
		[[nodiscard]] virtual bool IsKeyDown(Key key) const = 0;
		[[nodiscard]] virtual std::string GetKeyName(std::uint32_t key) const = 0;
		[[nodiscard]] virtual std::vector<KeyState> GetKeyStates() const = 0;
	};

	class IInputHostEvents
	{
	public:
		virtual ~IInputHostEvents() = default;
		virtual void UpdateMousePosition(PointerSurface surface,
			PointerPosition position) = 0;
		virtual void UpdateMouseButton(PointerSurface surface, PointerButton button,
			bool pressed, PointerPosition position) = 0;
		virtual void UpdateTouch(PointerSurface surface, PointerPosition position,
			bool pressed) = 0;
		virtual void UpdateMouseWheel(float value, std::int32_t cumulativeSteps) = 0;
		virtual void NotifyDeviceChanged() = 0;
	};

	class IClipboard
	{
	public:
		using GetTextCallback = std::function<void(bool, std::string)>;
		using SetTextCallback = std::function<void(bool)>;
		virtual ~IClipboard() = default;
		virtual void GetTextAsync(GetTextCallback callback) = 0;
		virtual void SetTextAsync(std::string text, SetTextCallback callback) = 0;
	};

	class IExternalLauncher
	{
	public:
		virtual ~IExternalLauncher() = default;
		[[nodiscard]] virtual bool OpenUrl(std::string url) = 0;
	};

	class IInputFocus
	{
	public:
		virtual ~IInputFocus() = default;
		[[nodiscard]] virtual bool InputConfigurationHasFocus() const = 0;
	};

	class ICanvasHost
	{
	public:
		virtual ~ICanvasHost() = default;
		[[nodiscard]] virtual bool RecreateCanvas() = 0;
	};

}
