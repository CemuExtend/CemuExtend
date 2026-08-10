#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Deciding which module sections must stay constant after linking, and finding
// the bytes that stopped matching, is pure logic over the section header and two
// byte buffers. It lives here instead of inside rpl.cpp so it can be unit tested
// without constructing a loader, a module or a guest address space.
namespace rpl_sections
{
inline constexpr std::uint32_t kFlagWrite = 0x00000001;
inline constexpr std::uint32_t kFlagAlloc = 0x00000002;
inline constexpr std::uint32_t kFlagExecute = 0x00000004;
inline constexpr std::uint32_t kTypeNoBits = 0x00000008;

// A section may only be guarded when the module itself declares it constant.
// The name alone does not: RPX images built by the Cafe SDK routinely emit a
// section called ".rodata" with SHF_WRITE set (Minecraft: Wii U Edition ships
// flags 0x08000003), and the loader then maps it into the read-write data
// region right next to .data - the same region that holds the module's
// r2-relative small-data window. Guarding such a section would make the loader
// silently revert writes the title is entitled to make.
[[nodiscard]] constexpr bool IsGuardableReadOnlySection(std::uint32_t sectionType,
	std::uint32_t sectionFlags, std::uint32_t sectionSize, std::string_view sectionName)
{
	if (sectionSize == 0 || sectionType == kTypeNoBits)
		return false;
	if (sectionName != ".rodata")
		return false;
	if ((sectionFlags & kFlagAlloc) == 0)
		return false;
	return (sectionFlags & (kFlagWrite | kFlagExecute)) == 0;
}

// True for the case worth reporting once per module: the module has a section
// named .rodata that is skipped only because the module marks it writable.
[[nodiscard]] constexpr bool IsWritableRodataSection(std::uint32_t sectionType,
	std::uint32_t sectionFlags, std::uint32_t sectionSize, std::string_view sectionName)
{
	if (sectionSize == 0 || sectionType == kTypeNoBits)
		return false;
	if (sectionName != ".rodata")
		return false;
	if ((sectionFlags & kFlagAlloc) == 0)
		return false;
	return (sectionFlags & kFlagWrite) != 0;
}

// Calls handler(offset, length) for every maximal run of bytes where live and
// reference disagree, and returns how many bytes diverged in total.
template<typename Handler>
std::size_t ForEachDivergentRange(const std::uint8_t* live, const std::uint8_t* reference,
	std::size_t size, Handler&& handler)
{
	std::size_t divergentBytes = 0;
	std::size_t index = 0;
	while (index < size)
	{
		if (live[index] == reference[index])
		{
			++index;
			continue;
		}
		const std::size_t rangeStart = index;
		while (index < size && live[index] != reference[index])
			++index;
		const std::size_t rangeSize = index - rangeStart;
		divergentBytes += rangeSize;
		handler(rangeStart, rangeSize);
	}
	return divergentBytes;
}
} // namespace rpl_sections
