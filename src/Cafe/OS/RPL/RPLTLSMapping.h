#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace RPLLoaderInternal
{
	struct TLSSectionMapping
	{
		std::uint32_t virtualAddress{};
		std::uint32_t size{};
		std::uint32_t mappedAddress{};
	};

	struct TLSMapping
	{
		std::uint32_t virtualAddress{};
		std::uint32_t mappedAddress{};
		std::uint32_t size{};
	};

	// A TLS template may span adjacent .tdata/.tbss sections, but both the
	// virtual and mapped ranges must form one identical contiguous layout.
	[[nodiscard]] inline std::optional<TLSMapping> ResolveContiguousTLSMapping(
		std::span<const TLSSectionMapping> input)
	{
		if (input.empty())
			return std::nullopt;
		std::vector<TLSSectionMapping> sections(input.begin(), input.end());
		std::ranges::sort(sections, {}, &TLSSectionMapping::virtualAddress);
		if (sections.front().size == 0)
			return std::nullopt;

		const std::uint32_t virtualBase = sections.front().virtualAddress;
		const std::uint32_t mappedBase = sections.front().mappedAddress;
		std::uint64_t virtualCursor = virtualBase;
		std::uint64_t mappedCursor = mappedBase;
		for (const auto& section : sections)
		{
			if (section.size == 0 || section.virtualAddress != virtualCursor ||
				section.mappedAddress != mappedCursor)
				return std::nullopt;
			virtualCursor += section.size;
			mappedCursor += section.size;
			if (virtualCursor > std::numeric_limits<std::uint32_t>::max() ||
				mappedCursor > std::numeric_limits<std::uint32_t>::max())
				return std::nullopt;
		}
		const std::uint64_t size = virtualCursor - virtualBase;
		if (size == 0 || size > std::numeric_limits<std::uint32_t>::max())
			return std::nullopt;
		return TLSMapping{virtualBase, mappedBase, static_cast<std::uint32_t>(size)};
	}
} // namespace RPLLoaderInternal
