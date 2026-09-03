#include "webview/cef/CefOverlayFrameMailbox.h"

#include <algorithm>
#include <cstring>

namespace WebFrontend::CefOverlay
{
	namespace
	{
		constexpr std::size_t kBytesPerPixel = 4;

		void BlendPremultiplied(std::uint8_t* destination, const std::uint8_t* source)
		{
			const unsigned inverseAlpha = 255U - source[3];
			for (unsigned channel = 0; channel != 4; ++channel)
				destination[channel] = static_cast<std::uint8_t>(std::min(
					255U, static_cast<unsigned>(source[channel]) +
							  (static_cast<unsigned>(destination[channel]) * inverseAlpha + 127U) / 255U));
		}

		void MergeDamage(std::vector<Host::OverlayDirtyRect>& damage,
						 const Host::OverlayDirtyRect& rect)
		{
			if (damage.empty())
			{
				damage.push_back(rect);
				return;
			}
			auto& merged = damage.front();
			const int right = std::max(merged.x + merged.width, rect.x + rect.width);
			const int bottom = std::max(merged.y + merged.height, rect.y + rect.height);
			merged.x = std::min(merged.x, rect.x);
			merged.y = std::min(merged.y, rect.y);
			merged.width = right - merged.x;
			merged.height = bottom - merged.y;
		}
	} // namespace

	FrameMailbox::FrameMailbox(std::function<void(Host::PointerSurface)> redraw)
		: m_redraw(std::move(redraw))
	{}

	std::size_t FrameMailbox::Index(Host::PointerSurface surface)
	{
		return surface == Host::PointerSurface::Main ? 0 : 1;
	}

	std::optional<Host::OverlayDirtyRect> FrameMailbox::Clamp(
		Host::OverlayDirtyRect rect, int width, int height)
	{
		if (rect.width <= 0 || rect.height <= 0 || width <= 0 || height <= 0)
			return std::nullopt;
		const std::int64_t right = std::min<std::int64_t>(width,
														  static_cast<std::int64_t>(rect.x) + rect.width);
		const std::int64_t bottom = std::min<std::int64_t>(height,
														   static_cast<std::int64_t>(rect.y) + rect.height);
		rect.x = std::max(rect.x, 0);
		rect.y = std::max(rect.y, 0);
		rect.width = static_cast<int>(right - rect.x);
		rect.height = static_cast<int>(bottom - rect.y);
		if (rect.width <= 0 || rect.height <= 0)
			return std::nullopt;
		return rect;
	}

	void FrameMailbox::CopyDirty(std::vector<std::uint8_t>& destination,
								 int destinationWidth, int destinationHeight, const std::uint8_t* source,
								 int sourceWidth, int sourceStride,
								 std::span<const Host::OverlayDirtyRect> dirtyRects)
	{
		for (const auto candidate : dirtyRects)
		{
			const auto rect = Clamp(candidate, destinationWidth, destinationHeight);
			if (!rect)
				continue;
			for (int row = 0; row < rect->height; ++row)
			{
				const auto offset = (static_cast<std::size_t>(rect->y + row) * destinationWidth + rect->x) * kBytesPerPixel;
				const auto sourceOffset = static_cast<std::size_t>(rect->y + row) * sourceStride +
										  static_cast<std::size_t>(rect->x) * kBytesPerPixel;
				std::memcpy(destination.data() + offset, source + sourceOffset,
							static_cast<std::size_t>(rect->width) * kBytesPerPixel);
			}
		}
	}

	bool FrameMailbox::PublishView(Host::PointerSurface surface, int width, int height,
								   const void* pixels, int sourceStride,
								   std::span<const Host::OverlayDirtyRect> dirtyRects)
	{
		if (!pixels || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
			sourceStride < width * static_cast<int>(kBytesPerPixel))
			return false;
		bool redraw{};
		{
			std::scoped_lock lock(m_mutex);
			auto& state = m_surfaces[Index(surface)];
			if (state.closing)
				return false;
			const bool resized = state.width != width || state.height != height;
			if (resized)
			{
				state.width = width;
				state.height = height;
				state.view = std::make_shared<std::vector<std::uint8_t>>(
					static_cast<std::size_t>(width) * height * kBytesPerPixel, 0);
				state.pendingDamage.clear();
				state.forceFullDamage = true;
				++state.resizeGeneration;
			}
			std::vector<Host::OverlayDirtyRect> valid;
			if (dirtyRects.empty() || resized)
				valid.push_back({0, 0, width, height});
			else
				for (const auto rect : dirtyRects)
					if (const auto clamped = Clamp(rect, width, height))
						valid.push_back(*clamped);
			if (valid.empty())
				return false;
			// Drop an unconsumed snapshot only after the incoming paint was
			// validated. The mailbox's own latest snapshot must not force a full
			// buffer clone for every CEF paint; only a snapshot already acquired by
			// the renderer needs copy-on-write protection.
			state.latest.reset();
			if (!resized && !state.view.unique())
				state.view = std::make_shared<std::vector<std::uint8_t>>(*state.view);
			CopyDirty(*state.view, width, height, static_cast<const std::uint8_t*>(pixels), width,
					  sourceStride, valid);
			for (const auto& rect : valid)
				MergeDamage(state.pendingDamage, rect);
			PublishLocked(surface, state);
			redraw = true;
		}
		if (redraw && m_redraw)
			m_redraw(surface);
		return true;
	}

	void FrameMailbox::SetPopupVisible(Host::PointerSurface surface, bool visible)
	{
		{
			std::scoped_lock lock(m_mutex);
			auto& state = m_surfaces[Index(surface)];
			if (state.popupVisible == visible)
				return;
			state.popupVisible = visible;
			state.forceFullDamage = true;
			if (!visible)
			{
				state.popup.clear();
				state.popupWidth = state.popupHeight = 0;
			}
			if (state.view && !state.view->empty() && !state.closing)
				PublishLocked(surface, state);
		}
		if (m_redraw)
			m_redraw(surface);
	}

	void FrameMailbox::SetPopupRect(Host::PointerSurface surface, Host::OverlayDirtyRect rect)
	{
		{
			std::scoped_lock lock(m_mutex);
			auto& state = m_surfaces[Index(surface)];
			state.popupRect = rect;
			state.forceFullDamage = true;
			if (state.view && !state.view->empty() && !state.closing)
				PublishLocked(surface, state);
		}
		if (m_redraw)
			m_redraw(surface);
	}

	bool FrameMailbox::PublishPopup(Host::PointerSurface surface, int width, int height,
									const void* pixels, int sourceStride,
									std::span<const Host::OverlayDirtyRect> dirtyRects)
	{
		if (!pixels || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
			sourceStride < width * static_cast<int>(kBytesPerPixel))
			return false;
		{
			std::scoped_lock lock(m_mutex);
			auto& state = m_surfaces[Index(surface)];
			if (state.closing || !state.popupVisible || !state.view || state.view->empty())
				return false;
			const bool resized = state.popupWidth != width || state.popupHeight != height;
			if (resized)
			{
				state.popup.assign(static_cast<std::size_t>(width) * height * kBytesPerPixel, 0);
				state.popupWidth = width;
				state.popupHeight = height;
			}
			std::vector<Host::OverlayDirtyRect> valid;
			if (dirtyRects.empty() || resized)
				valid.push_back({0, 0, width, height});
			else
				for (const auto rect : dirtyRects)
					if (const auto clamped = Clamp(rect, width, height))
						valid.push_back(*clamped);
			if (valid.empty())
				return false;
			CopyDirty(state.popup, width, height, static_cast<const std::uint8_t*>(pixels), width,
					  sourceStride, valid);
			state.forceFullDamage = true;
			PublishLocked(surface, state);
		}
		if (m_redraw)
			m_redraw(surface);
		return true;
	}

	void FrameMailbox::PublishLocked(Host::PointerSurface surface, SurfaceState& state)
	{
		std::shared_ptr<std::vector<std::uint8_t>> composed;
		if (state.popupVisible && !state.popup.empty())
			composed = std::make_shared<std::vector<std::uint8_t>>(*state.view);
		else
			composed = state.view;
		if (state.popupVisible && !state.popup.empty())
		{
			for (int popupY = 0; popupY < state.popupHeight; ++popupY)
				for (int popupX = 0; popupX < state.popupWidth; ++popupX)
				{
					const int x = state.popupRect.x + popupX;
					const int y = state.popupRect.y + popupY;
					if (x < 0 || y < 0 || x >= state.width || y >= state.height)
						continue;
					auto* destination = composed->data() +
										(static_cast<std::size_t>(y) * state.width + x) * kBytesPerPixel;
					const auto* source = state.popup.data() +
										 (static_cast<std::size_t>(popupY) * state.popupWidth + popupX) * kBytesPerPixel;
					BlendPremultiplied(destination, source);
				}
		}
		Host::OverlayFrameSnapshot snapshot;
		snapshot.surface = surface;
		snapshot.sequence = ++state.sequence;
		snapshot.resizeGeneration = state.resizeGeneration;
		snapshot.width = state.width;
		snapshot.height = state.height;
		snapshot.stride = state.width * static_cast<int>(kBytesPerPixel);
		snapshot.fullDamage = state.forceFullDamage;
		snapshot.bgra = std::move(composed);
		snapshot.dirtyRects = snapshot.fullDamage
								  ? std::vector<Host::OverlayDirtyRect>{{0, 0, state.width, state.height}}
								  : state.pendingDamage;
		state.latest = std::move(snapshot);
	}

	void FrameMailbox::BeginClose(Host::PointerSurface surface)
	{
		std::scoped_lock lock(m_mutex);
		auto& state = m_surfaces[Index(surface)];
		state.closing = true;
		state.latest.reset();
		state.pendingDamage.clear();
	}

	void FrameMailbox::Reopen(Host::PointerSurface surface)
	{
		std::scoped_lock lock(m_mutex);
		auto& state = m_surfaces[Index(surface)];
		state.closing = false;
		state.forceFullDamage = true;
	}

	std::optional<Host::OverlayFrameSnapshot> FrameMailbox::AcquireLatestOverlayFrame(
		Host::PointerSurface surface, std::uint64_t afterSequence)
	{
		std::scoped_lock lock(m_mutex);
		auto& state = m_surfaces[Index(surface)];
		if (state.closing || !state.latest || state.latest->sequence <= afterSequence)
			return std::nullopt;
		auto result = state.latest;
		state.latest.reset();
		state.pendingDamage.clear();
		state.forceFullDamage = false;
		return result;
	}
} // namespace WebFrontend::CefOverlay
