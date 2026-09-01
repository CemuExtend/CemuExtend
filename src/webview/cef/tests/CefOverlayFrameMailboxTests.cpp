#include "webview/cef/CefOverlayFrameMailbox.h"
#include "webview/cef/CefOverlayInput.h"
#include "webview/cef/CefOverlayOrder.h"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using Host::OverlayDirtyRect;
using Host::PointerSurface;
using WebFrontend::CefOverlay::FrameMailbox;

namespace
{
	std::vector<std::uint8_t> Pixels(int width, int height, std::uint8_t value)
	{
		return std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4, value);
	}
} // namespace

int main()
{
	using WebFrontend::CefOverlay::IsPointerInput;
	using WebFrontend::CefOverlay::ResolveNativeInputRoute;
	using WebFrontend::CefOverlay::WindowsKeyCodeFromUsbHid;
	static_assert(IsPointerInput(WebFrontend::NativeInputKind::PointerMove));
	static_assert(IsPointerInput(WebFrontend::NativeInputKind::RawMouse));
	static_assert(!IsPointerInput(WebFrontend::NativeInputKind::Key));
	static_assert(WindowsKeyCodeFromUsbHid(0x04) == 'A');
	static_assert(WindowsKeyCodeFromUsbHid(0x1d) == 'Z');
	static_assert(WindowsKeyCodeFromUsbHid(0x29) == 0x1b);
	static_assert(WindowsKeyCodeFromUsbHid(0xe5) == 0xa1);
	constexpr auto overlayKey =
		ResolveNativeInputRoute(WebFrontend::NativeInputKind::Key, true);
	static_assert(overlayKey.publishGuestPhysicalInput && overlayKey.sendOverlayInput &&
				  !overlayKey.processFrontendInput);
	constexpr auto overlayButton =
		ResolveNativeInputRoute(WebFrontend::NativeInputKind::PointerButton, true);
	static_assert(overlayButton.publishGuestPhysicalInput && overlayButton.sendOverlayInput &&
				  !overlayButton.processFrontendInput);
	constexpr auto overlayWheel =
		ResolveNativeInputRoute(WebFrontend::NativeInputKind::PointerWheel, true);
	static_assert(overlayWheel.publishGuestPhysicalInput && overlayWheel.sendOverlayInput &&
				  !overlayWheel.processFrontendInput);
	constexpr auto overlayCharacter =
		ResolveNativeInputRoute(WebFrontend::NativeInputKind::Character, true);
	static_assert(!overlayCharacter.publishGuestPhysicalInput &&
				  !overlayCharacter.processFrontendInput);
	constexpr auto overlayFocusLost =
		ResolveNativeInputRoute(WebFrontend::NativeInputKind::FocusLost, true);
	static_assert(overlayFocusLost.publishGuestPhysicalInput &&
				  !overlayFocusLost.processFrontendInput);
	constexpr auto overlayDeviceChanged =
		ResolveNativeInputRoute(WebFrontend::NativeInputKind::DeviceChanged, true);
	static_assert(!overlayDeviceChanged.publishGuestPhysicalInput &&
				  overlayDeviceChanged.processFrontendInput);
	constexpr auto ordinaryKey =
		ResolveNativeInputRoute(WebFrontend::NativeInputKind::Key, false);
	static_assert(ordinaryKey.publishGuestPhysicalInput && !ordinaryKey.sendOverlayInput &&
				  ordinaryKey.processFrontendInput);

	using WebFrontend::CefOverlay::CemodOverlayOrder;
	using WebFrontend::CefOverlay::OverlayLayer;
	using WebFrontend::CefOverlay::OverlayLayersBottomToTop;
	constexpr auto below = OverlayLayersBottomToTop(CemodOverlayOrder::BelowBuiltin);
	constexpr auto above = OverlayLayersBottomToTop(CemodOverlayOrder::AboveBuiltin);
	static_assert(below[0] == OverlayLayer::Cemod && below[1] == OverlayLayer::Builtin);
	static_assert(above[0] == OverlayLayer::Builtin && above[1] == OverlayLayer::Cemod);

	int redraws{};
	FrameMailbox mailbox([&](PointerSurface) { ++redraws; });
	auto firstPixels = Pixels(4, 3, 7);
	assert(mailbox.PublishView(PointerSurface::Main, 4, 3, firstPixels.data(), 16, {}));
	auto first = mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, 0);
	assert(first && first->fullDamage && first->sequence == 1 && first->resizeGeneration == 1);
	assert(first->dirtyRects.size() == 1 && first->dirtyRects[0].width == 4);
	assert(first->bgra->at(0) == 7 && redraws == 1);
	assert(!mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, first->sequence));

	auto secondPixels = Pixels(4, 3, 9);
	const std::array dirty{OverlayDirtyRect{1, 1, 2, 1}};
	assert(mailbox.PublishView(PointerSurface::Main, 4, 3, secondPixels.data(), 16, dirty));
	auto second = mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, first->sequence);
	assert(second && !second->fullDamage && second->sequence == 2);
	assert(second->bgra->at((1 * 4 + 1) * 4) == 9);
	assert(second->bgra->at(0) == 7);

	// The CEF buffer may have row padding. Only the dirty row is copied and
	// padding must not leak into the tightly packed renderer snapshot.
	std::vector<std::uint8_t> padded(3 * 20, 0xee);
	for (int x = 0; x < 4; ++x)
		for (int channel = 0; channel < 4; ++channel)
			padded[20 + x * 4 + channel] = static_cast<std::uint8_t>(20 + x);
	const std::array paddedDirty{OverlayDirtyRect{1, 1, 2, 1}};
	assert(mailbox.PublishView(PointerSurface::Main, 4, 3, padded.data(), 20, paddedDirty));
	auto paddedFrame = mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, second->sequence);
	assert(paddedFrame && paddedFrame->bgra->at((1 * 4 + 1) * 4) == 21);
	assert(paddedFrame->bgra->at((1 * 4 + 2) * 4) == 22);
	assert(paddedFrame->bgra->at((1 * 4 + 3) * 4) == 7);

	const std::array clamped{OverlayDirtyRect{-2, -1, 4, 3},
							 OverlayDirtyRect{100, 100, 2, 2}, OverlayDirtyRect{0, 0, -1, 2}};
	assert(mailbox.PublishView(PointerSurface::Main, 4, 3, secondPixels.data(), 16, clamped));
	auto clampedFrame = mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, paddedFrame->sequence);
	assert(clampedFrame && clampedFrame->dirtyRects.size() == 1);
	assert(clampedFrame->dirtyRects[0].x == 0 && clampedFrame->dirtyRects[0].width == 2);
	assert(clampedFrame->bgra->at(0) == 9);

	const std::array accumulatedA{OverlayDirtyRect{0, 0, 1, 1}};
	const std::array accumulatedB{OverlayDirtyRect{3, 2, 1, 1}};
	assert(mailbox.PublishView(PointerSurface::Main, 4, 3, firstPixels.data(), 16, accumulatedA));
	assert(mailbox.PublishView(PointerSurface::Main, 4, 3, secondPixels.data(), 16, accumulatedB));
	auto latest = mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, clampedFrame->sequence);
	assert(latest && latest->sequence == clampedFrame->sequence + 2);
	assert(latest->dirtyRects.size() == 1);
	assert(latest->dirtyRects[0].x == 0 && latest->dirtyRects[0].y == 0);
	assert(latest->dirtyRects[0].width == 4 && latest->dirtyRects[0].height == 3);
	assert(latest->bgra->at(0) == 7);
	assert(latest->bgra->at((2 * 4 + 3) * 4) == 9);
	const auto latestSequence = latest->sequence;
	assert(!mailbox.PublishView(PointerSurface::Main, 4, 3, secondPixels.data(), 15, {}));
	const std::array invalidOnly{OverlayDirtyRect{20, 20, 2, 2}};
	assert(!mailbox.PublishView(PointerSurface::Main, 4, 3, secondPixels.data(), 16, invalidOnly));
	assert(!mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, latestSequence));
	assert(mailbox.PublishView(PointerSurface::Main, 4, 3, firstPixels.data(), 16, accumulatedA));
	assert(!mailbox.PublishView(PointerSurface::Main, 4, 3, secondPixels.data(), 16, invalidOnly));
	auto preserved = mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, latestSequence);
	assert(preserved && preserved->bgra->at(0) == 7);

	auto resizedPixels = Pixels(2, 2, 3);
	assert(mailbox.PublishView(PointerSurface::Main, 2, 2, resizedPixels.data(), 8, {}));
	auto resized = mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, latest->sequence);
	assert(resized && resized->fullDamage && resized->resizeGeneration == 2 && resized->width == 2);

	mailbox.SetPopupRect(PointerSurface::Main, {0, 0, 1, 1});
	mailbox.SetPopupVisible(PointerSurface::Main, true);
	std::vector<std::uint8_t> popup{10, 20, 30, 128};
	assert(mailbox.PublishPopup(PointerSurface::Main, 1, 1, popup.data(), 4, {}));
	auto popupFrame = mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, resized->sequence);
	assert(popupFrame && popupFrame->fullDamage);
	const auto& composed = *popupFrame->bgra;
	assert(composed[0] == 11 && composed[1] == 21 && composed[2] == 31 && composed[3] == 129);

	mailbox.BeginClose(PointerSurface::Main);
	assert(!mailbox.PublishView(PointerSurface::Main, 2, 2, resizedPixels.data(), 8, {}));
	assert(!mailbox.AcquireLatestOverlayFrame(PointerSurface::Main, 0));
	mailbox.Reopen(PointerSurface::Main);
	assert(mailbox.PublishView(PointerSurface::Main, 2, 2, resizedPixels.data(), 8, {}));

	return 0;
}
