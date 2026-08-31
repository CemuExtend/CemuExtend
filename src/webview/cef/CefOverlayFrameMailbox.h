#pragma once

#include "host/contracts/HostContracts.h"

#include <array>
#include <functional>
#include <mutex>
#include <span>

namespace WebFrontend::CefOverlay
{
	class FrameMailbox final : public Host::IOverlayFrameSource
	{
	  public:
		explicit FrameMailbox(std::function<void(Host::PointerSurface)> redraw = {});

		bool PublishView(Host::PointerSurface surface, int width, int height,
						 const void* bgra, int sourceStride,
						 std::span<const Host::OverlayDirtyRect> dirtyRects);
		void SetPopupVisible(Host::PointerSurface surface, bool visible);
		void SetPopupRect(Host::PointerSurface surface, Host::OverlayDirtyRect rect);
		bool PublishPopup(Host::PointerSurface surface, int width, int height,
						  const void* bgra, int sourceStride,
						  std::span<const Host::OverlayDirtyRect> dirtyRects);
		void BeginClose(Host::PointerSurface surface);
		void Reopen(Host::PointerSurface surface);

		[[nodiscard]] std::optional<Host::OverlayFrameSnapshot> AcquireLatestOverlayFrame(
			Host::PointerSurface surface, std::uint64_t afterSequence) override;

	  private:
		struct SurfaceState
		{
			int width{};
			int height{};
			std::uint64_t sequence{};
			std::uint64_t resizeGeneration{};
			bool closing{};
			bool forceFullDamage{true};
			bool popupVisible{};
			Host::OverlayDirtyRect popupRect{};
			std::shared_ptr<std::vector<std::uint8_t>> view;
			std::vector<std::uint8_t> popup;
			int popupWidth{};
			int popupHeight{};
			std::vector<Host::OverlayDirtyRect> pendingDamage;
			std::optional<Host::OverlayFrameSnapshot> latest;
		};

		static std::size_t Index(Host::PointerSurface surface);
		static std::optional<Host::OverlayDirtyRect> Clamp(
			Host::OverlayDirtyRect rect, int width, int height);
		static void CopyDirty(std::vector<std::uint8_t>& destination, int destinationWidth,
							  int destinationHeight, const std::uint8_t* source, int sourceWidth,
							  int sourceStride,
							  std::span<const Host::OverlayDirtyRect> dirtyRects);
		void PublishLocked(Host::PointerSurface surface, SurfaceState& state);

		std::mutex m_mutex;
		std::array<SurfaceState, 2> m_surfaces;
		std::function<void(Host::PointerSurface)> m_redraw;
	};
} // namespace WebFrontend::CefOverlay
