#include "CubebInputAPI.h"

#include <chrono>

#if BOOST_OS_WINDOWS
#include <combaseapi.h>
#include <mmreg.h>
#include <mmsystem.h>
#endif

static void state_cb(cubeb_stream* stream, void* user, cubeb_state state)
{
	if (!stream)
		return;

	auto* input = static_cast<CubebInputAPI*>(user);
	if (input && state == CUBEB_STATE_ERROR)
		input->MarkFailed();
	/*switch (state)
	{
	case CUBEB_STATE_STARTED:
		fprintf(stderr, "stream started\n");
		break;
	case CUBEB_STATE_STOPPED:
		fprintf(stderr, "stream stopped\n");
		break;
	case CUBEB_STATE_DRAINED:
		fprintf(stderr, "stream drained\n");
		break;
	default:
		fprintf(stderr, "unknown stream state %d\n", state);
	}*/
}

long CubebInputAPI::data_cb(cubeb_stream* stream, void* user, const void* inputbuffer, void* outputbuffer, long nframes)
{
	auto* thisptr = (CubebInputAPI*)user;
	const auto samples = static_cast<std::size_t>(nframes) * thisptr->m_channels;
	std::unique_lock lock(thisptr->m_mutex, std::try_to_lock);
	if (!lock.owns_lock() || thisptr->m_buffer.empty())
	{
		thisptr->m_droppedSamples += samples;
		return nframes;
	}

	const auto* input = static_cast<const sint16*>(inputbuffer);
	const auto callbackTimeNs = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count());
	const auto inputLatencyNs =
		static_cast<std::uint64_t>(thisptr->m_inputLatencyFrames.load(std::memory_order_acquire)) *
		1'000'000'000ULL / thisptr->m_samplerate;
	const auto lastCaptureTimeNs =
		callbackTimeNs > inputLatencyNs ? callbackTimeNs - inputLatencyNs : 0;
	const auto samplePeriodDenominator =
		static_cast<std::uint64_t>(thisptr->m_samplerate) * thisptr->m_channels;
	std::size_t first = samples > thisptr->m_buffer.size() ? samples - thisptr->m_buffer.size() : 0;
	if (first != 0)
		thisptr->m_droppedSamples += first;
	for (; first < samples; ++first)
	{
		if (thisptr->m_sampleCount == thisptr->m_buffer.size())
		{
			thisptr->m_readIndex = (thisptr->m_readIndex + 1) % thisptr->m_buffer.size();
			--thisptr->m_sampleCount;
			++thisptr->m_droppedSamples;
		}
		thisptr->m_buffer[thisptr->m_writeIndex] = input ? input[first] : 0;
		const auto remaining = samples - first - 1;
		const auto ageNs = static_cast<std::uint64_t>(remaining) * 1'000'000'000ULL /
						   samplePeriodDenominator;
		thisptr->m_captureTimes[thisptr->m_writeIndex] =
			lastCaptureTimeNs > ageNs ? lastCaptureTimeNs - ageNs : 0;
		thisptr->m_writeIndex = (thisptr->m_writeIndex + 1) % thisptr->m_buffer.size();
		++thisptr->m_sampleCount;
	}

	return nframes;
}

CubebInputAPI::CubebInputAPI(cubeb_devid devid, uint32 samplerate, uint32 channels, uint32 samples_per_block,
							 uint32 bits_per_sample, bool voice, uint32 target_latency_samples)
	: IAudioInputAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
	cubeb_stream_params input_params;

	input_params.format = CUBEB_SAMPLE_S16LE;
	input_params.rate = samplerate;
	input_params.channels = channels;
	input_params.prefs = voice ? CUBEB_STREAM_PREF_VOICE : CUBEB_STREAM_PREF_NONE;

	switch (channels)
	{
	case 8:
		input_params.layout = CUBEB_LAYOUT_3F4_LFE;
		break;
	case 6:
		input_params.layout = CUBEB_LAYOUT_QUAD_LFE | CHANNEL_FRONT_CENTER;
		break;
	case 4:
		input_params.layout = CUBEB_LAYOUT_QUAD;
		break;
	case 2:
		input_params.layout = CUBEB_LAYOUT_STEREO;
		break;
	default:
		input_params.layout = CUBEB_LAYOUT_MONO;
		break;
	}

	uint32 latency = 1;
	cubeb_get_min_latency(s_context, &input_params, &latency);
	if (target_latency_samples != 0)
		latency = std::max(latency, target_latency_samples);
	m_inputLatencyFrames.store(latency, std::memory_order_relaxed);

	m_buffer.resize(static_cast<std::size_t>(m_samplesPerBlock) * m_channels * kBlockCount);
	m_captureTimes.resize(m_buffer.size());

	if (cubeb_stream_init(s_context, &m_stream, "Cemu Cubeb input",
						  devid, &input_params,
						  nullptr, nullptr,
						  latency, data_cb, state_cb, this) != CUBEB_OK)
	{
		throw std::runtime_error("can't initialize cubeb device");
	}
}

CubebInputAPI::~CubebInputAPI()
{
	if (m_stream)
	{
		Stop();
		cubeb_stream_destroy(m_stream);
	}
}

bool CubebInputAPI::ConsumeBlock(sint16* data)
{
	const auto requested = static_cast<std::size_t>(m_samplesPerBlock) * m_channels;
	const auto copied = ConsumeAvailable({data, requested});
	if (copied < requested)
		std::fill(data + copied, data + requested, 0);
	return true;
}

std::size_t CubebInputAPI::ConsumeAvailable(std::span<sint16> data, std::uint64_t* captureTimeNs)
{
	RefreshInputLatency();
	std::unique_lock lock(m_mutex);
	const auto copied = std::min(data.size(), m_sampleCount);
	if (captureTimeNs)
		*captureTimeNs = copied != 0 ? m_captureTimes[m_readIndex] : 0;
	for (std::size_t index = 0; index < copied; ++index)
	{
		data[index] = m_buffer[m_readIndex];
		m_readIndex = (m_readIndex + 1) % m_buffer.size();
	}
	m_sampleCount -= copied;
	return copied;
}

void CubebInputAPI::RefreshInputLatency()
{
	if (m_inputLatencyKnown.load(std::memory_order_acquire) || !m_stream)
		return;
	std::uint32_t latencyFrames{};
	const int result = cubeb_stream_get_input_latency(m_stream, &latencyFrames);
	if (result == CUBEB_ERROR_NOT_SUPPORTED)
	{
		// PulseAudio and AudioUnit do not expose input latency through cubeb.
		// Retain the stream's requested/minimum latency as a fixed estimate and
		// avoid retrying an unsupported control query for every guest Read.
		m_inputLatencyKnown.store(true, std::memory_order_release);
		return;
	}
	if (result != CUBEB_OK)
		return;
	std::unique_lock lock(m_mutex);
	if (m_inputLatencyKnown.load(std::memory_order_relaxed))
		return;
	const auto previousFrames = m_inputLatencyFrames.load(std::memory_order_relaxed);
	const auto previousNs = static_cast<std::uint64_t>(previousFrames) * 1'000'000'000ULL /
							m_samplerate;
	const auto latencyNs = static_cast<std::uint64_t>(latencyFrames) * 1'000'000'000ULL /
						   m_samplerate;
	for (std::size_t index = 0, position = m_readIndex; index < m_sampleCount; ++index)
	{
		auto& timestamp = m_captureTimes[position];
		if (latencyNs >= previousNs)
		{
			const auto adjustment = latencyNs - previousNs;
			timestamp = timestamp > adjustment ? timestamp - adjustment : 0;
		}
		else
			timestamp += previousNs - latencyNs;
		position = (position + 1) % m_captureTimes.size();
	}
	m_inputLatencyFrames.store(latencyFrames, std::memory_order_release);
	m_inputLatencyKnown.store(true, std::memory_order_release);
}

std::uint64_t CubebInputAPI::ConsumeDroppedSamples()
{
	return m_droppedSamples.exchange(0, std::memory_order_relaxed);
}

bool CubebInputAPI::Play()
{
	if (m_is_playing)
		return true;

	if (cubeb_stream_start(m_stream) == CUBEB_OK)
	{
		m_is_playing = true;
		RefreshInputLatency();
		return true;
	}

	return false;
}

bool CubebInputAPI::Stop()
{
	if (!m_is_playing)
		return true;

	if (cubeb_stream_stop(m_stream) == CUBEB_OK)
	{
		m_is_playing = false;
		return true;
	}

	return false;
}

void CubebInputAPI::SetVolume(sint32 volume)
{
	IAudioInputAPI::SetVolume(volume);
	cubeb_stream_set_volume(m_stream, (float)volume / 100.0f);
}

bool CubebInputAPI::InitializeStatic()
{
	if (cubeb_init(&s_context, "Cemu Input Cubeb", nullptr))
	{
		cemuLog_log(LogType::Force, "can't create cubeb audio api");
		return false;
	}

	return true;
}

void CubebInputAPI::Destroy()
{
	if (s_context)
		cubeb_destroy(s_context);
}

std::vector<IAudioInputAPI::DeviceDescriptionPtr> CubebInputAPI::GetDevices()
{
	std::vector<DeviceDescriptionPtr> result;
	// Add the default device to the list
	auto defaultDevice = std::make_shared<CubebDeviceDescription>(nullptr, "default", L"Default Device");
	result.emplace_back(defaultDevice);

	cubeb_device_collection devices;
	if (cubeb_enumerate_devices(s_context, CUBEB_DEVICE_TYPE_INPUT, &devices) != CUBEB_OK)
		return result;

	result.reserve(devices.count + 1); // The default device already occupies one element

	for (size_t i = 0; i < devices.count; ++i)
	{
		// const auto& device = devices.device[i];
		if (devices.device[i].state == CUBEB_DEVICE_STATE_ENABLED)
		{
			auto device = std::make_shared<CubebDeviceDescription>(devices.device[i].devid, devices.device[i].device_id,
																   boost::nowide::widen(
																	   devices.device[i].friendly_name));
			result.emplace_back(device);
		}
	}

	cubeb_device_collection_destroy(s_context, &devices);

	return result;
}
