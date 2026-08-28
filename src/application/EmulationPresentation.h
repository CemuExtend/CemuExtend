#pragma once

#include <cstdint>
#include <string>

namespace Application
{
	enum class TitleRegion : std::uint8_t
	{
		Unknown,
		Japan,
		UnitedStates,
		Europe,
	};

	enum class PresentationRenderer : std::uint8_t
	{
		Unknown,
		OpenGL,
		Vulkan,
		Metal,
	};

	enum class PresentationGpuVendor : std::uint8_t
	{
		Generic,
		Amd,
		Intel,
		Nvidia,
		Apple,
	};

	struct WindowTitlePresentation
	{
		std::uint64_t titleId{};
		std::string titleName;
		std::uint16_t version{};
		TitleRegion region{TitleRegion::Unknown};
		PresentationRenderer renderer{PresentationRenderer::Unknown};
		PresentationGpuVendor gpuVendor{PresentationGpuVendor::Generic};
	};
} // namespace Application
