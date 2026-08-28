#include "application/EmulatedUsbFacade.h"

#include <cassert>
#include <stdexcept>
#include <unordered_map>

namespace
{
	class FakeBackend final : public Application::IEmulatedUsbBackend
	{
	  public:
		std::vector<Application::UsbDeviceDescriptor> devices;
		std::unordered_map<std::uint32_t, bool> enabled;
		Observer observer;
		bool saveSucceeds{true};

		std::vector<Application::UsbDeviceDescriptor> Enumerate() override
		{
			return devices;
		}
		bool IsEnabled(std::uint16_t vendorId, std::uint16_t productId) const override
		{
			const auto found = enabled.find((std::uint32_t(vendorId) << 16) | productId);
			return found != enabled.end() && found->second;
		}
		bool SetEnabled(std::uint16_t vendorId, std::uint16_t productId, bool value) override
		{
			if (!saveSucceeds)
				return false;
			enabled[(std::uint32_t(vendorId) << 16) | productId] = value;
			return true;
		}
		std::uint64_t Subscribe(Observer value) override
		{
			observer = std::move(value);
			return 7;
		}
		void Unsubscribe(std::uint64_t token) override
		{
			assert(token == 7);
			observer = {};
		}
	};
} // namespace

int main()
{
	auto backend = std::make_unique<FakeBackend>();
	auto* fake = backend.get();
	fake->devices.push_back({"1430:0150:00", 0x1430, 0x0150, 0, 0, 0, 64, 64, false});
	Application::EmulatedUsbFacade facade(std::move(backend));
	auto model = facade.GetModel();
	assert(model.generation == 1);
	assert(model.emulatedDevices.size() == 3);
	assert(model.emulatedDevices[0].connected);
	assert(!model.emulatedDevices[0].enabled);

	model = facade.SetEnabled("skylanders", 0x1430, 0x0150, true);
	assert(model.generation == 2 && model.emulatedDevices[0].enabled);
	bool rejected{};
	try
	{
		(void)facade.SetEnabled("skylanders", 0x0e6f, 0x0129, true);
	} catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	assert(rejected);

	std::uint64_t observedGeneration{};
	facade.SetObserver([&](const Application::UsbDeviceChange& change) {
		observedGeneration = change.generation;
	});
	assert(fake->observer);
	fake->observer({0, true, fake->devices.front()});
	assert(observedGeneration == 3);
	auto late = fake->observer;
	facade.Close();
	late({0, false, fake->devices.front()});
	assert(observedGeneration == 3);
}
