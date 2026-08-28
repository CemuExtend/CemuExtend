#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace RPLLoaderInternal
{
	constexpr std::uint32_t kSectionTypeExports = 0x80000001U;
	constexpr std::uint32_t kSectionTypeImports = 0x80000002U;
	constexpr std::uint32_t kSectionTypeCrcs = 0x80000003U;
	constexpr std::uint32_t kSectionTypeFileInfo = 0x80000004U;
	constexpr std::uint32_t kSectionFlagWrite = 1U;
	constexpr std::uint32_t kSectionFlagAlloc = 2U;
	constexpr std::uint32_t kSectionFlagExecute = 4U;

	// Modules hidden from title dependency registration are never candidates for
	// ordinary name-based import resolution. External registries apply their own
	// owner/generation policy after the ordinary scan fails.
	[[nodiscard]] constexpr bool IsVisibleThroughOrdinaryModuleScan(
		bool externalModule, bool registerDependency)
	{
		return !externalModule || registerDependency;
	}

	enum class ExternalMappingRegion : std::uint8_t
	{
		None,
		Text,
		Data,
		Loader,
	};

	struct ExternalSectionMapping
	{
		std::uint32_t type{};
		std::uint32_t flags{};
		std::uint32_t virtualAddress{};
		std::uint32_t expandedSize{};
	};

	struct ExternalFileInfoMapping
	{
		std::uint32_t textRegionSize{};
		std::uint32_t dataRegionSize{};
		std::uint32_t loaderRegionSize{};
		std::uint32_t trampolineAdjustment{};
		std::uint32_t loaderAdjustment{};
	};

	struct ExternalMappingViolation
	{
		enum class Reason : std::uint8_t
		{
			InvalidAdjustment,
			RegionAddressOverflow,
			SectionOutsideRegion,
		};

		std::size_t sectionIndex{};
		ExternalMappingRegion region{};
		Reason reason{Reason::SectionOutsideRegion};
		std::uint64_t sectionBegin{};
		std::uint64_t sectionEnd{};
		std::uint64_t regionBegin{};
		std::uint64_t regionEnd{};
	};

	// This deliberately mirrors RPLLoader_LoadSections classification: imports
	// and exports do not enter the executable region merely because SHF_EXECUTE
	// is present, and SHF_WRITE takes precedence over the loader-info region.
	[[nodiscard]] constexpr ExternalMappingRegion ClassifyExternalSectionMapping(
		const ExternalSectionMapping& section)
	{
		if (section.expandedSize == 0 ||
			(section.flags & kSectionFlagAlloc) == 0 ||
			section.type == kSectionTypeCrcs ||
			section.type == kSectionTypeFileInfo)
			return ExternalMappingRegion::None;
		if ((section.flags & kSectionFlagExecute) != 0 &&
			section.type != kSectionTypeExports &&
			section.type != kSectionTypeImports)
			return ExternalMappingRegion::Text;
		if ((section.flags & kSectionFlagWrite) != 0)
			return ExternalMappingRegion::Data;
		return ExternalMappingRegion::Loader;
	}

	[[nodiscard]] inline std::optional<ExternalMappingViolation>
	FindExternalMappingViolation(
		std::span<const ExternalSectionMapping> sections,
		const ExternalFileInfoMapping& fileInfo)
	{
		if (fileInfo.trampolineAdjustment > fileInfo.textRegionSize ||
			fileInfo.loaderAdjustment > fileInfo.loaderRegionSize)
			return ExternalMappingViolation{
				0, ExternalMappingRegion::None,
				ExternalMappingViolation::Reason::InvalidAdjustment, 0, 0, 0, 0};

		constexpr std::uint64_t unset = std::numeric_limits<std::uint64_t>::max();
		std::uint64_t textBegin = unset;
		std::uint64_t dataBegin = unset;
		std::uint64_t loaderBegin = unset;
		for (const auto& section : sections)
		{
			const auto region = ClassifyExternalSectionMapping(section);
			const std::uint64_t begin = section.virtualAddress;
			switch (region)
			{
			case ExternalMappingRegion::Text:
				textBegin = textBegin == unset ? begin : (begin < textBegin ? begin : textBegin);
				break;
			case ExternalMappingRegion::Data:
				dataBegin = dataBegin == unset ? begin : (begin < dataBegin ? begin : dataBegin);
				break;
			case ExternalMappingRegion::Loader:
				loaderBegin = loaderBegin == unset ? begin : (begin < loaderBegin ? begin : loaderBegin);
				break;
			case ExternalMappingRegion::None:
				break;
			}
		}

		for (std::size_t index = 0; index < sections.size(); ++index)
		{
			const auto& section = sections[index];
			const auto region = ClassifyExternalSectionMapping(section);
			if (region == ExternalMappingRegion::None)
				continue;
			const std::uint64_t sectionBegin = section.virtualAddress;
			const std::uint64_t sectionEnd = sectionBegin + section.expandedSize;
			std::uint64_t regionBegin{};
			std::uint64_t declaredRegionSize{};
			std::uint64_t regionSize{};
			switch (region)
			{
			case ExternalMappingRegion::Text:
				regionBegin = textBegin;
				declaredRegionSize = fileInfo.textRegionSize;
				regionSize = fileInfo.textRegionSize - fileInfo.trampolineAdjustment;
				break;
			case ExternalMappingRegion::Data:
				regionBegin = dataBegin;
				declaredRegionSize = fileInfo.dataRegionSize;
				regionSize = fileInfo.dataRegionSize;
				break;
			case ExternalMappingRegion::Loader:
				regionBegin = loaderBegin;
				declaredRegionSize = fileInfo.loaderRegionSize;
				regionSize = fileInfo.loaderRegionSize - fileInfo.loaderAdjustment;
				break;
			case ExternalMappingRegion::None:
				continue;
			}
			const std::uint64_t regionEnd = regionBegin + regionSize;
			if (regionBegin + declaredRegionSize >
				std::numeric_limits<std::uint32_t>::max())
				return ExternalMappingViolation{
					index, region,
					ExternalMappingViolation::Reason::RegionAddressOverflow,
					sectionBegin, sectionEnd, regionBegin,
					regionBegin + declaredRegionSize};
			if (sectionBegin < regionBegin || sectionEnd < sectionBegin ||
				sectionEnd > regionEnd)
				return ExternalMappingViolation{
					index, region,
					ExternalMappingViolation::Reason::SectionOutsideRegion,
					sectionBegin, sectionEnd, regionBegin, regionEnd};
		}
		return std::nullopt;
	}
} // namespace RPLLoaderInternal
