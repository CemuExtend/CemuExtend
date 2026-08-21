#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Application
{
	struct UsbDeviceDescriptor
	{
		std::string id;
		std::uint16_t vendorId{};
		std::uint16_t productId{};
		std::uint8_t interfaceIndex{};
		std::uint8_t interfaceSubClass{};
		std::uint8_t protocol{};
		std::uint16_t maxPacketSizeRx{};
		std::uint16_t maxPacketSizeTx{};
		bool opened{};
	};

	struct EmulatedUsbDevice
	{
		std::string id;
		std::string name;
		std::uint16_t vendorId{};
		std::uint16_t productId{};
		bool enabled{};
		bool connected{};
	};

	struct EmulatedUsbModel
	{
		std::uint64_t generation{};
		std::vector<EmulatedUsbDevice> emulatedDevices;
		std::vector<UsbDeviceDescriptor> attachedDevices;
	};

	struct UsbDeviceChange
	{
		std::uint64_t generation{};
		bool attached{};
		UsbDeviceDescriptor device;
	};

	class IEmulatedUsbBackend
	{
	public:
		using Observer = std::function<void(const UsbDeviceChange&)>;
		virtual ~IEmulatedUsbBackend() = default;
		[[nodiscard]] virtual std::vector<UsbDeviceDescriptor> Enumerate() = 0;
		[[nodiscard]] virtual bool IsEnabled(std::uint16_t vendorId, std::uint16_t productId) const = 0;
		virtual bool SetEnabled(std::uint16_t vendorId, std::uint16_t productId, bool enabled) = 0;
		[[nodiscard]] virtual std::uint64_t Subscribe(Observer observer) = 0;
		virtual void Unsubscribe(std::uint64_t token) = 0;
	};

	class EmulatedUsbFacade final
	{
	public:
		using Observer = IEmulatedUsbBackend::Observer;
		explicit EmulatedUsbFacade(std::unique_ptr<IEmulatedUsbBackend> backend);
		~EmulatedUsbFacade();
		EmulatedUsbFacade(const EmulatedUsbFacade&) = delete;
		EmulatedUsbFacade& operator=(const EmulatedUsbFacade&) = delete;

		[[nodiscard]] EmulatedUsbModel GetModel();
		[[nodiscard]] EmulatedUsbModel SetEnabled(std::string_view deviceId,
			std::uint16_t vendorId, std::uint16_t productId, bool enabled);
		void SetObserver(Observer observer);
		void Close();

	private:
		struct CallbackGate
		{
			std::atomic_bool active{true};
			std::atomic_uint64_t generation{1};
			std::mutex mutex;
			Observer observer;
		};
		std::unique_ptr<IEmulatedUsbBackend> m_backend;
		std::shared_ptr<CallbackGate> m_gate;
		std::uint64_t m_subscription{};
	};

	[[nodiscard]] std::unique_ptr<IEmulatedUsbBackend> CreateEmulatedUsbBackend();
}
