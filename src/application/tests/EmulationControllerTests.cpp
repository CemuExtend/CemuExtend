#include "application/EmulationController.h"

#include <cassert>
#include <array>
#include <ranges>
#include <stdexcept>

namespace
{
	class FakeBackend final : public Application::IEmulationBackend
	{
	public:
		Application::LaunchError nextError{Application::LaunchError::None};
		int prepares{};
		int starts{};
		int aborts{};
		int stops{};
		bool throwOnPrepare{};
		bool throwOnStart{};
		bool throwOnStop{};
		bool failStop{};

		Application::LaunchResult Prepare(const Application::LaunchRequest& request) override
		{
			++prepares;
			if (throwOnPrepare) throw std::runtime_error("prepare failed");
			return {nextError, request.path, request.path, "Test title", {}};
		}
		void Start() override
		{
			++starts;
			if (throwOnStart) throw std::runtime_error("start failed");
		}
		bool AbortPrepared() override { ++aborts; return true; }
		bool Stop() override
		{
			++stops;
			if (throwOnStop) throw std::runtime_error("stop failed");
			return !failStop;
		}
		bool ShutdownApplication() override { return true; }
		bool IsTitleRunning() const override { return starts > stops; }
		std::optional<std::uint64_t> RunningTitleId() const override { return {}; }
		std::optional<std::int32_t> ForegroundProcessExitStatus() const override { return {}; }
		void SubmitKeyboard(std::uint16_t, bool, std::uint8_t) override {}
		void SubmitText(std::uint32_t, bool) override {}
		void KeyboardFocusLost() override {}
		void PointerFocusChanged(bool) override {}
		void SubmitMouse(const Application::MouseInput&) override {}
		Application::PointerPolicy GetPointerPolicy() override { return {}; }
		Application::TextInputState GetTextInputState() override { return {}; }
		void SubmitTextComposition(std::string_view, std::string_view,
			std::uint32_t, std::uint32_t) override {}
		void SetTextInputWakeCallback(void (*)()) override {}
		void SaveCemodPermissionDecisions(std::uint64_t,
			std::span<const Application::CemodPermissionDecision>) override {}
		std::vector<Application::CemodPackage> DiscoverCemodCatalog() override { return {}; }
		std::vector<Application::CemodPackage> DiscoverCemods(std::uint64_t) override { return {}; }
		Application::CemodGrant ResolveCemodGrant(std::uint64_t, std::string_view,
			std::string_view, std::uint32_t) override { return {}; }
		Application::CemuExtendServiceGrantDefaults ServiceGrantDefaults() const override
		{
			return {};
		}
		bool ImportLegacyCemodData(std::uint64_t, std::string_view,
			std::string&) override { return false; }
		std::vector<Application::TitleSummary> titles;
		std::vector<Application::GameSummary> games;
		std::vector<std::filesystem::path> scanPaths;
		int titleRefreshes{};
		int titleSubscriptions{};
		bool titleSubscriptionStopped{};
		std::filesystem::path addedTitle;
		std::vector<Application::TitleSummary> ListTitles() const override { return titles; }
		std::optional<Application::TitleSummary> ResolveBaseTitle(
			std::uint64_t titleId) const override
		{
			const auto found = std::ranges::find_if(titles,
				[titleId](const auto& title) { return title.titleId == titleId; });
			return found == titles.end() ? std::nullopt : std::optional{*found};
		}
		std::vector<Application::GameSummary> ListGames() const override { return games; }
		std::optional<Application::GameSummary> GetGame(std::uint64_t titleId) const override
		{
			const auto found = std::ranges::find_if(games,
				[titleId](const auto& game) { return game.titleId == titleId; });
			return found == games.end() ? std::nullopt : std::optional{*found};
		}
		bool IsTitleScanning() const override { return false; }
		std::optional<std::vector<std::uint8_t>> LoadTitleIcon(std::uint64_t) const override
		{
			return std::vector<std::uint8_t>{1, 2, 3};
		}
		Application::TitleCatalogSubscription SubscribeTitleCatalogEvents(
			Application::TitleCatalogHandler handler) override
		{
			struct State final : Application::Detail::TitleSubscriptionState
			{
				explicit State(bool& stopped) : stopped(stopped) {}
				void Stop() override { stopped = true; }
				bool& stopped;
			};
			++titleSubscriptions;
			for (const auto& game : games)
				handler({Application::TitleCatalogEventType::Discovered, game.titleId});
			return Application::TitleCatalogSubscription{
				std::make_shared<State>(titleSubscriptionStopped)};
		}
		void ReplaceScanPaths(std::span<const std::filesystem::path> paths) override
		{
			scanPaths.assign(paths.begin(), paths.end());
		}
		void RefreshTitles() override { ++titleRefreshes; }
		void AddTitleFromPath(const std::filesystem::path& path) override { addedTitle = path; }
		std::vector<Application::GraphicPackInfo> graphicPacks;
		int graphicPackRefreshes{};
		int graphicPackSaves{};
		std::vector<Application::GraphicPackInfo> ListGraphicPacks() const override
		{
			return graphicPacks;
		}
		Application::GraphicPackResult SetGraphicPackEnabled(
			std::string_view key, bool enabled) override
		{
			return {.changed = enabled, .info = Application::GraphicPackInfo{
				.key = std::string(key), .enabled = enabled}};
		}
		Application::GraphicPackResult SetGraphicPackPreset(std::string_view key,
			std::string_view category, std::string_view preset) override
		{
			return {.changed = true, .info = Application::GraphicPackInfo{
				.key = std::string(key), .presets = {{std::string(category),
					std::string(preset), true, true}}}};
		}
		Application::GraphicPackResult ReloadGraphicPack(std::string_view key) override
		{
			return {.reloaded = true,
				.info = Application::GraphicPackInfo{.key = std::string(key)}};
		}
		Application::GraphicPackRefreshResult RefreshGraphicPacks() override
		{
			++graphicPackRefreshes;
			return {};
		}
		void SaveGraphicPackState() override { ++graphicPackSaves; }
	};

	void VerifyFailure(Application::LaunchError error)
	{
		FakeBackend backend;
		backend.nextError = error;
		Application::EmulationController controller(backend);
		const auto result = controller.Launch({"test.rpx"});
		assert(result.error == error);
		assert(controller.State() == Application::EmulationState::Idle);
		assert(backend.starts == 0);
		assert(backend.aborts == 1);
	}
}

int main()
{
	FakeBackend backend;
	Application::EmulationController controller(backend);
	assert(controller.State() == Application::EmulationState::Idle);
	bool preparedBeforeStart{};
	const auto launch = controller.Launch({"test.rpx"}, [&](const auto&) {
		preparedBeforeStart = controller.State() == Application::EmulationState::Preparing;
	});
	assert(launch);
	assert(preparedBeforeStart);
	assert(controller.State() == Application::EmulationState::Running);
	assert(backend.prepares == 1 && backend.starts == 1);
	backend.titles.push_back({0x1234, "Test title", "test-title"});
	backend.games.push_back({.titleId = 0x1234, .name = "Test title",
		.basePath = "test-title", .version = 17});
	const auto titles = controller.ListTitles();
	assert(titles.size() == 1 && titles.front().titleId == 0x1234);
	assert(controller.ResolveBaseTitle(0x1234)->path == "test-title");
	assert(controller.ListGames().front().version == 17);
	assert(controller.GetGame(0x1234)->name == "Test title");
	assert(!controller.IsTitleScanning());
	assert(controller.LoadTitleIcon(0x1234)->size() == 3);
	std::vector<Application::TitleCatalogEvent> titleEvents;
	auto titleSubscription = controller.SubscribeTitleCatalog(
		[&](const auto& event) { titleEvents.push_back(event); });
	assert(backend.titleSubscriptions == 1);
	assert(titleEvents.size() == 1 && titleEvents.front().titleId == 0x1234);
	titleSubscription.Reset();
	assert(backend.titleSubscriptionStopped);
	const std::array<std::filesystem::path, 2> scanPaths{"games-a", "games-b"};
	controller.ReplaceTitleScanPaths(scanPaths);
	controller.RefreshTitles();
	controller.AddTitleFromPath("installed-title");
	assert(backend.scanPaths.size() == 2 && backend.titleRefreshes == 1 &&
		backend.addedTitle == "installed-title");
	backend.graphicPacks.push_back({.key = "pack-key", .name = "Pack"});
	assert(controller.ListGraphicPacks().front().key == "pack-key");
	assert(controller.SetGraphicPackEnabled("pack-key", true).changed);
	assert(controller.SetGraphicPackPreset("pack-key", "quality", "high").changed);
	assert(controller.ReloadGraphicPack("pack-key").reloaded);
	assert(controller.RefreshGraphicPacks());
	controller.SaveGraphicPackState();
	assert(backend.graphicPackRefreshes == 1 && backend.graphicPackSaves == 1);
	const auto stop = controller.Stop();
	assert(stop.stopped);
	assert(controller.State() == Application::EmulationState::Idle);
	assert(backend.stops == 1);

	VerifyFailure(Application::LaunchError::InvalidExecutable);
	VerifyFailure(Application::LaunchError::PermissionRequired);
	VerifyFailure(Application::LaunchError::PermissionDenied);
	VerifyFailure(Application::LaunchError::CemodRuntimeBusy);

	{
		FakeBackend throwing;
		throwing.throwOnPrepare = true;
		Application::EmulationController exceptionController(throwing);
		assert(exceptionController.Launch({"test.rpx"}).error ==
			Application::LaunchError::BackendFailure);
		assert(exceptionController.State() == Application::EmulationState::Idle);
		assert(throwing.aborts == 1);
		throwing.throwOnPrepare = false;
		throwing.throwOnStart = true;
		bool rolledBack{};
		assert(exceptionController.Launch({"test.rpx"}, {}, [&] { rolledBack = true; }).error ==
			Application::LaunchError::BackendFailure);
		assert(exceptionController.State() == Application::EmulationState::Idle);
		assert(throwing.aborts == 2);
		assert(rolledBack);
	}

	{
		FakeBackend throwing;
		Application::EmulationController exceptionController(throwing);
		assert(exceptionController.Launch({"test.rpx"}));
		throwing.throwOnStop = true;
		const auto failedStop = exceptionController.Stop();
		assert(!failedStop.stopped);
		assert(exceptionController.State() == Application::EmulationState::Running);
		throwing.throwOnStop = false;
		assert(exceptionController.Stop().stopped);
	}

	{
		FakeBackend retained;
		Application::EmulationController retainedController(retained);
		assert(retainedController.Launch({"test.rpx"}));
		retained.failStop = true;
		assert(!retainedController.Stop().stopped);
		assert(retainedController.State() == Application::EmulationState::Running);
		retained.failStop = false;
		assert(retainedController.Stop().stopped);
	}

	{
		FakeBackend ownedLifetime;
		{
			Application::EmulationController lifetimeController(ownedLifetime);
			assert(lifetimeController.Launch({"test.rpx"}));
		}
		assert(ownedLifetime.stops == 1);
	}
}
