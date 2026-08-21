#pragma once

#include <cstdint>

namespace AudioSpec
{
	inline constexpr std::uint32_t kOutputSampleRate = 48'000;
	inline constexpr std::uint32_t kInputSampleRate = 32'000;
	inline constexpr std::uint32_t kFrameDurationMilliseconds = 3;
	inline constexpr std::uint32_t kOutputFramesPerBlock = 4;

	[[nodiscard]] constexpr std::uint32_t SamplesPerFrame(std::uint32_t sampleRate)
	{
		return sampleRate * kFrameDurationMilliseconds / 1'000;
	}

	inline constexpr std::uint32_t kOutputSamplesPerBlock =
		SamplesPerFrame(kOutputSampleRate) * kOutputFramesPerBlock;
	inline constexpr std::uint32_t kInputSamplesPerBlock = SamplesPerFrame(kInputSampleRate);

	static_assert(kOutputSamplesPerBlock == 576);
	static_assert(kInputSamplesPerBlock == 96);
} // namespace AudioSpec
