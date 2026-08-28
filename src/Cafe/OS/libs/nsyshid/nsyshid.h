#pragma once
#include "Cafe/OS/RPL/COSModule.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nsyshid
{
	class Backend;

	// A value-only view of a HID device. This is safe to copy across frontend
	// boundaries and deliberately excludes HID/libusb pointers and native handles.
	struct DeviceDescriptor
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

	enum class DeviceChangeKind
	{
		Attached,
		Detached
	};
	struct DeviceChange
	{
		DeviceChangeKind kind{};
		DeviceDescriptor device;
	};
	using DeviceObserver = std::function<void(const DeviceChange&)>;
	using DeviceObserverToken = std::uint64_t;

	[[nodiscard]] std::vector<DeviceDescriptor> EnumerateDeviceDescriptors();
	[[nodiscard]] DeviceObserverToken SubscribeDeviceChanges(DeviceObserver observer);
	void UnsubscribeDeviceChanges(DeviceObserverToken token);

	void AttachBackend(const std::shared_ptr<Backend>& backend);
	void DetachBackend(const std::shared_ptr<Backend>& backend);

	COSModule* GetModule();
} // namespace nsyshid
