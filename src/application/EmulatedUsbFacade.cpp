#include "Common/precompiled.h"

#include "application/EmulatedUsbFacade.h"
#include "Cafe/OS/libs/nsyshid/nsyshid.h"
#include "config/CemuConfig.h"

#include <array>
#include <mutex>
#include <stdexcept>

namespace Application
{
	namespace
	{
		struct SupportedDevice { std::string_view id; std::string_view name; std::uint16_t vendorId; std::uint16_t productId; };
		constexpr std::array SupportedDevices{
			SupportedDevice{"skylanders", "Skylanders Portal", 0x1430, 0x0150},
			SupportedDevice{"infinity", "Infinity Base", 0x0e6f, 0x0129},
			SupportedDevice{"dimensions", "Dimensions Toypad", 0x0e6f, 0x0241},
		};

		const SupportedDevice& FindSupported(std::string_view id,
			std::uint16_t vendorId, std::uint16_t productId)
		{
			const auto found = std::ranges::find_if(SupportedDevices, [&](const auto& value) {
				return value.id == id && value.vendorId == vendorId && value.productId == productId;
			});
			if (found == SupportedDevices.end())
				throw std::invalid_argument("device selection does not match a supported VID/PID");
			return *found;
		}

		class NativeEmulatedUsbBackend final : public IEmulatedUsbBackend
		{
		public:
			std::vector<UsbDeviceDescriptor> Enumerate() override
			{
				std::vector<UsbDeviceDescriptor> result;
				for (const auto& device : nsyshid::EnumerateDeviceDescriptors())
					result.push_back({device.id, device.vendorId, device.productId,
						device.interfaceIndex, device.interfaceSubClass, device.protocol,
						device.maxPacketSizeRx, device.maxPacketSizeTx, device.opened});
				return result;
			}

			bool IsEnabled(std::uint16_t vendorId, std::uint16_t productId) const override
			{
				const auto& settings = GetConfig().emulated_usb_devices;
				if (vendorId == 0x1430 && productId == 0x0150) return settings.emulate_skylander_portal;
				if (vendorId == 0x0e6f && productId == 0x0129) return settings.emulate_infinity_base;
				if (vendorId == 0x0e6f && productId == 0x0241) return settings.emulate_dimensions_toypad;
				return false;
			}

			bool SetEnabled(std::uint16_t vendorId, std::uint16_t productId, bool enabled) override
			{
				auto& settings = GetConfig().emulated_usb_devices;
				if (vendorId == 0x1430 && productId == 0x0150) settings.emulate_skylander_portal = enabled;
				else if (vendorId == 0x0e6f && productId == 0x0129) settings.emulate_infinity_base = enabled;
				else if (vendorId == 0x0e6f && productId == 0x0241) settings.emulate_dimensions_toypad = enabled;
				else return false;
				return GetConfigHandle().Save();
			}

			std::uint64_t Subscribe(Observer observer) override
			{
				return nsyshid::SubscribeDeviceChanges([observer = std::move(observer)](const nsyshid::DeviceChange& change) {
					const auto& device = change.device;
					observer({0, change.kind == nsyshid::DeviceChangeKind::Attached,
						{device.id, device.vendorId, device.productId, device.interfaceIndex,
						 device.interfaceSubClass, device.protocol, device.maxPacketSizeRx,
						 device.maxPacketSizeTx, device.opened}});
				});
			}

			void Unsubscribe(std::uint64_t token) override { nsyshid::UnsubscribeDeviceChanges(token); }
		};
	}

	EmulatedUsbFacade::EmulatedUsbFacade(std::unique_ptr<IEmulatedUsbBackend> backend)
		: m_backend(std::move(backend)), m_gate(std::make_shared<CallbackGate>())
	{
		if (!m_backend) throw std::invalid_argument("emulated USB backend is required");
		m_subscription = m_backend->Subscribe([gate = m_gate](const UsbDeviceChange& change) {
			std::scoped_lock lock(gate->mutex);
			if (gate->active.load(std::memory_order_acquire) && gate->observer)
			{
				auto versioned = change;
				versioned.generation = gate->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
				gate->observer(versioned);
			}
		});
	}

	EmulatedUsbFacade::~EmulatedUsbFacade() { Close(); }

	EmulatedUsbModel EmulatedUsbFacade::GetModel()
	{
		auto attached = m_backend->Enumerate();
		EmulatedUsbModel model;
		model.generation = m_gate->generation.load(std::memory_order_acquire);
		model.attachedDevices = attached;
		for (const auto& supported : SupportedDevices)
		{
			const bool connected = std::ranges::any_of(attached, [&](const auto& device) {
				return device.vendorId == supported.vendorId && device.productId == supported.productId;
			});
			model.emulatedDevices.push_back({std::string(supported.id), std::string(supported.name),
				supported.vendorId, supported.productId,
				m_backend->IsEnabled(supported.vendorId, supported.productId), connected});
		}
		return model;
	}

	EmulatedUsbModel EmulatedUsbFacade::SetEnabled(std::string_view deviceId,
		std::uint16_t vendorId, std::uint16_t productId, bool enabled)
	{
		(void)FindSupported(deviceId, vendorId, productId);
		if (!m_backend->SetEnabled(vendorId, productId, enabled))
			throw std::runtime_error("failed to save emulated USB settings");
		m_gate->generation.fetch_add(1, std::memory_order_acq_rel);
		return GetModel();
	}

	void EmulatedUsbFacade::SetObserver(Observer observer)
	{
		std::scoped_lock lock(m_gate->mutex);
		if (m_gate->active.load(std::memory_order_acquire)) m_gate->observer = std::move(observer);
	}

	void EmulatedUsbFacade::Close()
	{
		{
			std::scoped_lock lock(m_gate->mutex);
			if (!m_gate->active.exchange(false, std::memory_order_acq_rel)) return;
			m_gate->observer = {};
		}
		if (m_subscription) m_backend->Unsubscribe(std::exchange(m_subscription, 0));
	}

	std::unique_ptr<IEmulatedUsbBackend> CreateEmulatedUsbBackend()
	{
		return std::make_unique<NativeEmulatedUsbBackend>();
	}
}
