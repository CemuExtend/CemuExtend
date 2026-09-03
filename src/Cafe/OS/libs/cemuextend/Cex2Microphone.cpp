#include "Cafe/OS/libs/cemuextend/Cex2Microphone.h"

#include "audio/AudioSpec.h"
#include "audio/IAudioInputAPI.h"
#include "config/CemuConfig.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>

namespace cemuextend_hle
{
	namespace
	{
		using namespace cemuextend::wire;
		constexpr std::uint16_t MaximumReadSamples = 4096;

		struct ActiveMicrophone
		{
			Cex2MicrophoneOwner owner{};
			std::uint32_t handle{};
			AudioInputAPIPtr device;
		};

		std::mutex g_mutex;
		ActiveMicrophone g_microphone;
		std::uint32_t g_nextHandle{1};

		std::uint64_t MonotonicTimeNs()
		{
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
												  std::chrono::steady_clock::now().time_since_epoch())
												  .count());
		}

		IAudioInputAPI::DeviceDescriptionPtr FindConfiguredDevice(std::wstring_view identifier)
		{
			if (identifier.empty() || !IAudioInputAPI::IsAudioInputAPIAvailable(IAudioInputAPI::Cubeb))
				return {};
			const auto devices = IAudioInputAPI::GetDevices(IAudioInputAPI::Cubeb);
			const auto found = std::ranges::find_if(devices, [&](const auto& device) {
				return device->GetIdentifier() == identifier;
			});
			return found == devices.end() ? IAudioInputAPI::DeviceDescriptionPtr{} : *found;
		}

		Cex2MicrophoneResult Probe(std::wstring_view identifier)
		{
			MicrophoneProbeResponse response{};
			response.sampleRate = AudioSpec::kInputSampleRate;
			response.channels = 1;
			response.maximumReadSamples = MaximumReadSamples;
			response.format = static_cast<std::uint8_t>(MicrophoneSampleFormat::PcmS16BigEndian);
			Cex2MicrophoneResult result{FindConfiguredDevice(identifier) ? Status::Ok : Status::NotFound};
			result.payload.resize(sizeof(response));
			std::memcpy(result.payload.data(), &response, sizeof(response));
			return result;
		}
	} // namespace

	Cex2MicrophoneResult Cex2Microphone::Dispatch(Cex2MicrophoneOwner owner, std::string_view,
												  std::uint16_t operation, std::span<const std::byte> payload)
	{
		using namespace cemuextend::wire;
		std::scoped_lock lock(g_mutex);
		const auto configuration = GetConfig().GetModInputConfiguration();
		switch (static_cast<MicrophoneOperation>(operation))
		{
		case MicrophoneOperation::Probe:
			return payload.empty() ? Probe(configuration.device) : Cex2MicrophoneResult{Status::InvalidArgument};
		case MicrophoneOperation::Open:
		{
			if (payload.size() != sizeof(MicrophoneOpenRequest))
				return {Status::InvalidArgument};
			MicrophoneOpenRequest request{};
			std::memcpy(&request, payload.data(), sizeof(request));
			if (request.sampleRate.get() != AudioSpec::kInputSampleRate || request.channels.get() != 1 ||
				request.format != static_cast<std::uint8_t>(MicrophoneSampleFormat::PcmS16BigEndian) ||
				request.reserved != std::array<std::byte, 3>{} ||
				request.targetLatencySamples.get() > MaximumReadSamples)
				return {Status::InvalidArgument};
			if (g_microphone.device)
				return {g_microphone.owner == owner ? Status::Busy : Status::PermissionDenied};
			const auto description = FindConfiguredDevice(configuration.device);
			if (!description)
				return {Status::NotFound};
			try
			{
				auto device = IAudioInputAPI::CreateDevice(IAudioInputAPI::Cubeb, description,
														   AudioSpec::kInputSampleRate, 1, AudioSpec::kInputSamplesPerBlock, 16, true,
														   request.targetLatencySamples.get());
				if (!device)
					return {Status::NotSupported};
				if (!device->Play())
					return {Status::IoError};
				g_microphone.owner = owner;
				g_microphone.handle = g_nextHandle++;
				if (g_nextHandle == 0)
					g_nextHandle = 1;
				g_microphone.device = std::move(device);
			} catch (const std::exception&)
			{
				return {Status::IoError};
			}
			MicrophoneOpenResponse response{};
			response.handle = g_microphone.handle;
			response.sampleRate = AudioSpec::kInputSampleRate;
			response.channels = 1;
			response.maximumReadSamples = MaximumReadSamples;
			response.format = static_cast<std::uint8_t>(MicrophoneSampleFormat::PcmS16BigEndian);
			Cex2MicrophoneResult result{Status::Ok};
			result.payload.resize(sizeof(response));
			std::memcpy(result.payload.data(), &response, sizeof(response));
			return result;
		}
		case MicrophoneOperation::Read:
		{
			if (payload.size() != sizeof(MicrophoneReadRequest))
				return {Status::InvalidArgument};
			MicrophoneReadRequest request{};
			std::memcpy(&request, payload.data(), sizeof(request));
			const auto maximum = request.maximumSamples.get();
			if (request.reserved.get() != 0 || maximum == 0 || maximum > MaximumReadSamples)
				return {Status::InvalidArgument};
			if (!g_microphone.device || g_microphone.owner != owner ||
				request.handle.get() != g_microphone.handle)
				return {Status::NotFound};
			if (g_microphone.device->HasFailed())
			{
				g_microphone = {};
				return {Status::IoError};
			}
			std::vector<sint16> samples(maximum);
			std::uint64_t captureTimeNs{};
			const auto count = g_microphone.device->ConsumeAvailable(samples, &captureTimeNs);
			const auto dropped = g_microphone.device->ConsumeDroppedSamples();
			const auto gain = std::clamp(configuration.volume, 0, 200);
			for (std::size_t index = 0; index < count; ++index)
			{
				const auto amplified = static_cast<std::int64_t>(samples[index]) * gain / 100;
				samples[index] = static_cast<sint16>(std::clamp<std::int64_t>(
					amplified, std::numeric_limits<sint16>::min(), std::numeric_limits<sint16>::max()));
			}
			MicrophoneReadResponse response{};
			response.handle = g_microphone.handle;
			response.sampleCount = static_cast<std::uint32_t>(count);
			response.droppedSamples = static_cast<std::uint32_t>(
				std::min<std::uint64_t>(dropped, std::numeric_limits<std::uint32_t>::max()));
			response.captureTimeNs = captureTimeNs != 0 ? captureTimeNs : MonotonicTimeNs();
			if (dropped != 0)
				response.flags = static_cast<std::uint8_t>(MicrophoneReadFlag::Discontinuity);
			Cex2MicrophoneResult result{Status::Ok};
			result.payload.resize(sizeof(response) + count * sizeof(BeI16));
			std::memcpy(result.payload.data(), &response, sizeof(response));
			auto* output = reinterpret_cast<BeI16*>(result.payload.data() + sizeof(response));
			for (std::size_t index = 0; index < count; ++index)
				output[index] = samples[index];
			return result;
		}
		case MicrophoneOperation::Close:
		{
			if (payload.size() != sizeof(Be32))
				return {Status::InvalidArgument};
			Be32 handle{};
			std::memcpy(&handle, payload.data(), sizeof(handle));
			if (!g_microphone.device || g_microphone.owner != owner || handle.get() != g_microphone.handle)
				return {Status::NotFound};
			g_microphone = {};
			return {Status::Ok};
		}
		}
		return {Status::NotSupported};
	}

	void Cex2Microphone::ReleaseSession(Cex2MicrophoneOwner owner)
	{
		std::scoped_lock lock(g_mutex);
		if (g_microphone.owner == owner)
			g_microphone = {};
	}

	void Cex2Microphone::ReleaseOwner(std::uint64_t addressSpaceId, std::uint32_t generation)
	{
		std::scoped_lock lock(g_mutex);
		if (g_microphone.owner.addressSpaceId == addressSpaceId &&
			g_microphone.owner.generation == generation)
			g_microphone = {};
	}

	void Cex2Microphone::ReleaseAll()
	{
		std::scoped_lock lock(g_mutex);
		g_microphone = {};
	}
} // namespace cemuextend_hle
