#pragma once

#include <span>
#include <cstdint>

class IAudioInputAPI
{
	friend class GeneralSettings2;

  public:
	class DeviceDescription
	{
	  public:
		explicit DeviceDescription(std::wstring name)
			: m_name(std::move(name)) {}

		virtual ~DeviceDescription() = default;
		virtual std::wstring GetIdentifier() const = 0;
		const std::wstring& GetName() const
		{
			return m_name;
		}

		bool operator==(const DeviceDescription& o) const
		{
			return GetIdentifier() == o.GetIdentifier();
		}

	  private:
		std::wstring m_name;
	};

	using DeviceDescriptionPtr = std::shared_ptr<DeviceDescription>;

	enum AudioInputAPI
	{
		Cubeb,

		AudioInputAPIEnd,
	};
	static constexpr uint32 kBlockCount = 24;

	IAudioInputAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample);
	virtual ~IAudioInputAPI() = default;
	virtual AudioInputAPI GetType() const = 0;

	sint32 GetChannels() const
	{
		return m_channels;
	}

	virtual sint32 GetVolume() const
	{
		return m_volume;
	}
	virtual void SetVolume(sint32 volume)
	{
		m_volume = volume;
	}

	virtual bool ConsumeBlock(sint16* data) = 0;
	// Returns only captured samples and never pads with silence. CEX2 uses this
	// to keep its guest-side jitter buffer from treating underflow as real data.
	virtual std::size_t ConsumeAvailable(std::span<sint16> data,
										 std::uint64_t* captureTimeNs = nullptr) = 0;
	virtual std::uint64_t ConsumeDroppedSamples() = 0;
	virtual bool HasFailed() const
	{
		return false;
	}
	virtual bool Play() = 0;
	virtual bool Stop() = 0;
	virtual bool IsPlaying() const = 0;

	static void PrintLogging();
	static void InitializeStatic();
	static bool IsAudioInputAPIAvailable(AudioInputAPI api);

	static std::unique_ptr<IAudioInputAPI> CreateDevice(AudioInputAPI api, const DeviceDescriptionPtr& device, sint32 samplerate, sint32 channels, sint32 samples_per_block, sint32 bits_per_sample, bool voice = false, uint32 target_latency_samples = 0);
	static std::vector<DeviceDescriptionPtr> GetDevices(AudioInputAPI api);

  protected:
	uint32 m_samplerate, m_channels, m_samplesPerBlock, m_bitsPerSample;
	uint32 m_bytesPerBlock;

	sint32 m_volume = 0;

	static std::array<bool, AudioInputAPIEnd> s_availableApis;

  private:
};

using AudioInputAPIPtr = std::unique_ptr<IAudioInputAPI>;
extern std::shared_mutex g_audioInputMutex;
extern AudioInputAPIPtr g_inputAudio;
