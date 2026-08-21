#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Application
{
	struct TextureDiagnosticRow
	{
		std::string id;
		std::string parentId;
		bool view{};
		bool active{};
		bool updatedOnGpu{};
		bool depthFormat{};
		std::string dimension;
		std::string format;
		std::uint32_t width{};
		std::uint32_t height{};
		std::uint32_t depth{};
		std::uint32_t pitch{};
		std::uint32_t tileMode{};
		std::uint32_t firstSlice{};
		std::uint32_t sliceCount{};
		std::uint32_t firstMip{};
		std::uint32_t mipCount{};
		std::uint32_t ageMilliseconds{};
		std::uint32_t alternativeViewCount{};
		bool resolutionOverridden{};
		std::uint32_t effectiveWidth{};
		std::uint32_t effectiveHeight{};
		std::uint32_t effectiveDepth{};
	};

	struct TextureDiagnosticPage
	{
		std::uint64_t generation{};
		std::size_t offset{};
		std::size_t total{};
		bool truncated{};
		bool available{};
		std::string diagnostic;
		std::vector<TextureDiagnosticRow> rows;
	};

	struct AudioVoiceDiagnosticRow
	{
		std::string id;
		std::uint32_t index{};
		std::string format;
		std::uint32_t currentOffset{};
		std::uint32_t loopOffset{};
		std::uint32_t endOffset{};
		bool looping{};
		std::uint16_t volume{};
		std::int16_t volumeDelta{};
		std::uint32_t sourceRatio{};
		bool lowPassEnabled{};
		bool biquadEnabled{};
		std::string deviceMix;
	};

	struct AudioVoiceDiagnosticPage
	{
		std::uint64_t generation{};
		std::size_t offset{};
		std::size_t total{};
		bool available{};
		std::string diagnostic;
		std::vector<AudioVoiceDiagnosticRow> rows;
	};

	class DiagnosticFacade final
	{
	public:
		static constexpr std::size_t MaximumPageSize = 200;
		static constexpr std::size_t MaximumTextureRows = 16384;

		[[nodiscard]] TextureDiagnosticPage GetTexturePage(std::uint64_t generation,
			std::size_t offset, std::size_t limit, bool activeOnly, bool includeViews);
		[[nodiscard]] AudioVoiceDiagnosticPage GetAudioVoicePage(std::uint64_t generation,
			std::size_t offset, std::size_t limit, bool activeOnly);

	private:
		void RefreshTextures(bool activeOnly, bool includeViews);
		void RefreshAudioVoices(bool activeOnly);

		std::mutex m_mutex;
		std::uint64_t m_textureGeneration{};
		std::uint64_t m_audioGeneration{};
		std::vector<TextureDiagnosticRow> m_textureRows;
		std::vector<AudioVoiceDiagnosticRow> m_audioRows;
		bool m_textureAvailable{};
		bool m_audioAvailable{};
		bool m_textureTruncated{};
		std::string m_textureDiagnostic;
		std::string m_audioDiagnostic;
	};
}
