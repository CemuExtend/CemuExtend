#include "frontend/RuntimeOverlay.h"

#include <cassert>
#include <chrono>
#include <thread>

int main()
{
	using namespace std::chrono_literals;
	auto& model = RuntimeOverlay::Model::Instance();
	model.Reset();

	int changes = 0;
	model.SetChangeHandler([&changes] { ++changes; });
	model.SetPresentation(
		{RuntimeOverlay::Position::TopLeft, 0xFF00FFFF, 125},
		{RuntimeOverlay::Position::BottomRight, 0xFFFFFFFF, 100},
		{.fps = true, .drawCalls = true},
		{.fps = 59.94, .drawCalls = 42, .fastDrawCalls = 40});
	auto snapshot = model.GetSnapshot();
	assert(snapshot.overlayStyle.position == RuntimeOverlay::Position::TopLeft);
	assert(snapshot.visibility.drawCalls);
	assert(snapshot.stats.drawCalls == 42);

	const auto noticeId = model.PushNotice(RuntimeOverlay::NoticeKind::Message, "hello", 1ms);
	assert(noticeId != 0);
	assert(model.GetSnapshot().notices.size() == 1);
	std::this_thread::sleep_for(2ms);
	assert(model.GetSnapshot().notices.empty());

	model.SetErrorDialog({.generation = 2, .active = true, .title = "Error"});
	assert(model.GetSnapshot().interaction == RuntimeOverlay::Interaction::ErrorDialog);
	model.SetSoftwareKeyboard({.generation = 3, .active = true, .maximumLength = 10, .text = "abc"});
	snapshot = model.GetSnapshot();
	assert(snapshot.interaction == RuntimeOverlay::Interaction::SoftwareKeyboard);
	assert(snapshot.keyboard.text == "abc");
	model.SetSoftwareKeyboard({.generation = 3});
	assert(model.GetSnapshot().interaction == RuntimeOverlay::Interaction::ErrorDialog);
	assert(changes >= 6);

	model.ClearChangeHandler();
	model.Reset();
	return 0;
}
