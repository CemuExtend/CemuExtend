#pragma once

#include <array>
#include <cstdint>

namespace WebFrontend::CefOverlay
{
	enum class OverlayLayer : std::uint8_t
	{
		Builtin,
		Cemod,
	};

	enum class CemodOverlayOrder : std::uint8_t
	{
		BelowBuiltin,
		AboveBuiltin,
	};

	[[nodiscard]] constexpr std::array<OverlayLayer, 2> OverlayLayersBottomToTop(
		CemodOverlayOrder order)
	{
		return order == CemodOverlayOrder::BelowBuiltin
				   ? std::array{OverlayLayer::Cemod, OverlayLayer::Builtin}
				   : std::array{OverlayLayer::Builtin, OverlayLayer::Cemod};
	}
} // namespace WebFrontend::CefOverlay
