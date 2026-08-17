#include "application/ApplicationEvents.h"

#include <cassert>

int main()
{
	Application::ApplicationEvents events;
	int first{};
	int second{};
	auto firstSubscription = events.Subscribe([&](const auto& event) {
		assert(event.type == Application::EventType::GameLoaded);
		++first;
	});
	{
		auto secondSubscription = events.Subscribe([&](const auto&) { ++second; });
		events.Publish({.type = Application::EventType::GameLoaded});
		assert(first == 1 && second == 1);
	}
	events.Publish({.type = Application::EventType::GameLoaded});
	assert(first == 2 && second == 1);
	firstSubscription.Reset();
	events.Publish({.type = Application::EventType::GameLoaded});
	assert(first == 2);
}
