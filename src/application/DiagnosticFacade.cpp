#include "Common/precompiled.h"

#include "application/DiagnosticFacade.h"

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Application
{
	namespace
	{
		std::string DimensionName(Latte::E_DIM dimension)
		{
			switch (dimension)
			{
			case Latte::E_DIM::DIM_1D:
				return "1D";
			case Latte::E_DIM::DIM_2D:
				return "2D";
			case Latte::E_DIM::DIM_3D:
				return "3D";
			case Latte::E_DIM::DIM_CUBEMAP:
				return "cubemap";
			case Latte::E_DIM::DIM_2D_ARRAY:
				return "2D array";
			case Latte::E_DIM::DIM_2D_MSAA:
				return "2D MSAA";
			case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
				return "2D array MSAA";
			default:
				return "unknown";
			}
		}

		std::string FormatName(Latte::E_GX2SURFFMT format)
		{
			return fmt::format("0x{:04x}", static_cast<std::uint32_t>(format));
		}

		std::uint32_t NonNegative(sint32 value)
		{
			return value <= 0 ? 0U : static_cast<std::uint32_t>(value);
		}

		template<typename Page, typename Row>
		void CopyPage(Page& page, const std::vector<Row>& source,
					  std::size_t offset, std::size_t limit)
		{
			page.offset = std::min(offset, source.size());
			page.total = source.size();
			const auto count = std::min(limit, source.size() - page.offset);
			page.rows.assign(source.begin() + page.offset,
							 source.begin() + page.offset + count);
		}
	} // namespace

	void DiagnosticFacade::RefreshTextures(bool activeOnly, bool includeViews)
	{
		m_textureRows.clear();
		m_textureDiagnostic.clear();
		m_textureAvailable = false;
		m_textureTruncated = false;
		++m_textureGeneration;
		if (!g_renderer)
		{
			m_textureDiagnostic = "Start a title to inspect the renderer texture cache.";
			return;
		}

		// LatteTexture_QueryCacheInfo requests a copied snapshot from the renderer thread.
		// The returned values own all nested view records; no renderer pointers escape here.
		auto textures = LatteTexture_QueryCacheInfo();
		m_textureAvailable = true;
		if (textures.empty())
			m_textureDiagnostic = "The renderer returned an empty texture-cache snapshot.";
		std::ranges::sort(textures, {}, &LatteTextureInformation::physAddress);
		const auto now = GetTickCount();
		for (std::size_t textureIndex = 0; textureIndex < textures.size(); ++textureIndex)
		{
			const auto& texture = textures[textureIndex];
			const auto age = now - texture.lastAccessTick;
			if (activeOnly && age > 3000)
				continue;
			if (m_textureRows.size() == MaximumTextureRows)
			{
				m_textureTruncated = true;
				break;
			}
			const auto id = fmt::format("texture:{}:{}", m_textureGeneration, textureIndex);
			m_textureRows.push_back({
				.id = id,
				.view = false,
				.active = age <= 3000,
				.updatedOnGpu = texture.isUpdatedOnGPU,
				.depthFormat = texture.isDepth,
				.dimension = DimensionName(texture.dim),
				.format = FormatName(texture.format),
				.width = NonNegative(texture.width),
				.height = NonNegative(texture.height),
				.depth = NonNegative(texture.depth),
				.pitch = NonNegative(texture.pitch),
				.tileMode = static_cast<std::uint32_t>(texture.tileMode),
				.mipCount = NonNegative(texture.mipLevels),
				.ageMilliseconds = age,
				.alternativeViewCount = texture.alternativeViewCount,
				.resolutionOverridden = texture.overwriteInfo.hasResolutionOverwrite,
				.effectiveWidth = texture.overwriteInfo.hasResolutionOverwrite ? NonNegative(texture.overwriteInfo.width) : NonNegative(texture.width),
				.effectiveHeight = texture.overwriteInfo.hasResolutionOverwrite ? NonNegative(texture.overwriteInfo.height) : NonNegative(texture.height),
				.effectiveDepth = texture.overwriteInfo.hasResolutionOverwrite ? NonNegative(texture.overwriteInfo.depth) : NonNegative(texture.depth),
			});
			if (!includeViews)
				continue;
			for (std::size_t viewIndex = 0; viewIndex < texture.views.size(); ++viewIndex)
			{
				if (m_textureRows.size() == MaximumTextureRows)
				{
					m_textureTruncated = true;
					break;
				}
				const auto& view = texture.views[viewIndex];
				m_textureRows.push_back({
					.id = fmt::format("view:{}:{}:{}", m_textureGeneration, textureIndex, viewIndex),
					.parentId = id,
					.view = true,
					.active = age <= 3000,
					.dimension = DimensionName(view.dim),
					.format = FormatName(view.format),
					.width = NonNegative(view.width),
					.height = NonNegative(view.height),
					.pitch = NonNegative(view.pitch),
					.firstSlice = NonNegative(view.firstSlice),
					.sliceCount = NonNegative(view.numSlice),
					.firstMip = NonNegative(view.firstMip),
					.mipCount = NonNegative(view.numMip),
				});
			}
		}
	}

	TextureDiagnosticPage DiagnosticFacade::GetTexturePage(std::uint64_t generation,
														   std::size_t offset, std::size_t limit, bool activeOnly, bool includeViews)
	{
		if (limit == 0 || limit > MaximumPageSize)
			throw std::invalid_argument("limit must be between 1 and 200");
		std::scoped_lock lock(m_mutex);
		if (generation == 0)
			RefreshTextures(activeOnly, includeViews);
		else if (generation != m_textureGeneration)
			throw std::invalid_argument("texture snapshot generation is stale");
		TextureDiagnosticPage page;
		page.generation = m_textureGeneration;
		page.available = m_textureAvailable;
		page.truncated = m_textureTruncated;
		page.diagnostic = m_textureDiagnostic;
		CopyPage(page, m_textureRows, offset, limit);
		return page;
	}

	void DiagnosticFacade::RefreshAudioVoices(bool activeOnly)
	{
		m_audioRows.clear();
		m_audioDiagnostic.clear();
		m_audioAvailable = false;
		++m_audioGeneration;
		std::lock_guard lock(snd_core::__AXVoiceListSpinlock);
		if (!snd_core::__AXVPBInternalVoiceArray || !snd_core::__AXVPBArrayPtr)
		{
			m_audioDiagnostic = "Start a title that initializes AX audio to inspect voices.";
			return;
		}
		m_audioAvailable = true;
		for (std::uint32_t index = 0; index < snd_core::AX_MAX_VOICES; ++index)
		{
			const auto& voice = snd_core::__AXVPBArrayPtr[index];
			const auto& internal = snd_core::__AXVPBInternalVoiceArray[index];
			const auto playing = _swapEndianU16(internal.playbackState) != 0;
			if (activeOnly && !playing)
				continue;
			const auto format = _swapEndianU16(voice.offsets.format);
			const char* formatName = format == snd_core::AX_FORMAT_ADPCM ? "ADPCM" : format == snd_core::AX_FORMAT_PCM16 ? "PCM16"
																				 : format == snd_core::AX_FORMAT_PCM8	 ? "PCM8"
																														 : "unknown";
			std::string mix;
			mix.reserve(snd_core::AX_TV_CHANNEL_COUNT * snd_core::AX_MAX_NUM_BUS);
			for (std::uint32_t channel = 0; channel < snd_core::AX_TV_CHANNEL_COUNT; ++channel)
				for (std::uint32_t bus = 0; bus < snd_core::AX_MAX_NUM_BUS; ++bus)
					mix += fmt::format("{:x}", std::min<std::uint32_t>(15,
																	   (static_cast<std::uint16_t>(internal.deviceMixTV[channel * 4 + bus].vol) + 0x0fff) >> 12));
			m_audioRows.push_back({
				.id = fmt::format("voice:{}:{}", m_audioGeneration, index),
				.index = index,
				.format = formatName,
				.currentOffset = _swapEndianU32(voice.offsets.currentOffset),
				.loopOffset = _swapEndianU32(voice.offsets.loopOffset),
				.endOffset = _swapEndianU32(voice.offsets.endOffset),
				.looping = static_cast<std::uint16_t>(voice.offsets.loopFlag) != 0,
				.volume = static_cast<std::uint16_t>(internal.veVolume),
				.volumeDelta = static_cast<std::int16_t>(internal.veDelta),
				.sourceRatio = (static_cast<std::uint32_t>(_swapEndianU16(internal.src.ratioHigh)) << 16) |
							   _swapEndianU16(internal.src.ratioLow),
				.lowPassEnabled = internal.lpf.on != 0,
				.biquadEnabled = internal.biquad.on != 0,
				.deviceMix = std::move(mix),
			});
		}
	}

	AudioVoiceDiagnosticPage DiagnosticFacade::GetAudioVoicePage(std::uint64_t generation,
																 std::size_t offset, std::size_t limit, bool activeOnly)
	{
		if (limit == 0 || limit > MaximumPageSize)
			throw std::invalid_argument("limit must be between 1 and 200");
		std::scoped_lock lock(m_mutex);
		if (generation == 0)
			RefreshAudioVoices(activeOnly);
		else if (generation != m_audioGeneration)
			throw std::invalid_argument("audio snapshot generation is stale");
		AudioVoiceDiagnosticPage page;
		page.generation = m_audioGeneration;
		page.available = m_audioAvailable;
		page.diagnostic = m_audioDiagnostic;
		CopyPage(page, m_audioRows, offset, limit);
		return page;
	}
} // namespace Application
