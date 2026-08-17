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
		std::vector<Application::TitleCatalogEvent> catalogEvents;
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
			if (catalogEvents.empty())
			{
				for (const auto& game : games)
					handler({Application::TitleCatalogEventType::Discovered, game.titleId});
			}
			else
			{
				for (const auto& event : catalogEvents)
					handler(event);
			}
			return Application::TitleCatalogSubscription{
				std::make_shared<State>(titleSubscriptionStopped)};
		}
		void ReplaceScanPaths(std::span<const std::filesystem::path> paths) override
		{
			scanPaths.assign(paths.begin(), paths.end());
		}
		void RefreshTitles() override { ++titleRefreshes; }
		void AddTitleFromPath(const std::filesystem::path& path) override { addedTitle = path; }
		std::optional<Application::WuaConversionPlan> PlanWuaConversion(
			std::uint64_t titleId, std::uint64_t preferredLocationUid) const override
		{
			return Application::WuaConversionPlan{
				.items = {{preferredLocationUid, titleId, 1,
					Application::ContentRole::Base, "base-path"}},
				.suggestedFileName = "Test title.wua",
			};
		}
		Application::ContentOperationResult ConvertToWua(
			std::span<const std::uint64_t>, const std::filesystem::path&,
			Application::ContentProgressHandler progress,
			Application::ContentCancellationCheck cancelled) override
		{
			if (cancelled && cancelled())
				return {Application::ContentOperationError::Cancelled, "cancelled"};
			if (progress)
				progress({Application::ContentOperationPhase::Finalizing, 1, 1, 4, 4});
			return {};
		}
		Application::ContentChecksumResult ComputeTitleChecksum(
			std::uint64_t locationUid, Application::ContentProgressHandler progress,
			Application::ContentCancellationCheck cancelled) override
		{
			if (cancelled && cancelled())
				return {Application::ContentOperationError::Cancelled, "cancelled", std::nullopt};
			if (progress)
				progress({Application::ContentOperationPhase::Hashing, 1, 1, 4, 4});
			return {Application::ContentOperationError::None, {}, Application::ContentChecksum{
				.titleId = 0x1234, .version = 1, .region = 2,
				.imageSha256 = std::string(64, locationUid == 0 ? '0' : '1')}};
		}
		Application::GameProfileView gameProfile;
		int gameProfileSaves{};
		Application::GameProfileView LoadGameProfile(std::uint64_t) const override
		{
			return gameProfile;
		}
		Application::GameProfileSaveResult SaveGameProfile(std::uint64_t,
			const Application::GameProfileUpdate& update) override
		{
			++gameProfileSaves;
			gameProfile.settings = update;
			return {true, {}};
		}
		Application::TitleInstallPlan installPlan{
			.sourcePath = "source-title",
			.targetPath = "installed-title",
			.titleId = 0x1234,
			.version = 7,
			.kind = Application::TitleInstallKind::Update,
			.requiredBytes = 10,
			.availableBytes = 100,
		};
		int titleInstalls{};
		Application::TitleInstallDecision installDecision{
			Application::TitleInstallDecision::Proceed};
		Application::TitleInstallPlanResult PlanTitleInstall(
			const std::filesystem::path&) const override
		{
			return {Application::TitleInstallError::None, {}, installPlan};
		}
		Application::TitleInstallResult InstallTitle(
			const Application::TitleInstallPlan& plan,
			Application::TitleInstallDecision decision,
			Application::TitleInstallProgressHandler progress,
			Application::TitleInstallCancellationCheck cancelled) override
		{
			++titleInstalls;
			installDecision = decision;
			if (cancelled && cancelled())
				return {Application::TitleInstallError::Cancelled, "cancelled", {}};
			if (progress)
				progress({plan.requiredBytes, plan.requiredBytes, "meta/meta.xml"});
			return {Application::TitleInstallError::None, {}, plan.targetPath};
		}
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
	backend.catalogEvents.push_back({
		.type = Application::TitleCatalogEventType::SaveDiscovered,
		.titleId = 0x1234,
		.managedEntry = Application::ManagedContentEntry{
			.locationUid = 17,
			.titleId = 0x1234,
			.path = "save-path",
			.name = "Test save",
			.version = 9,
			.region = 2,
			.regionName = "USA",
			.type = Application::ManagedContentType::Save,
			.format = Application::ManagedContentFormat::Folder,
		},
	});
	std::vector<Application::TitleCatalogEvent> titleEvents;
	auto titleSubscription = controller.SubscribeTitleCatalog(
		[&](const auto& event) { titleEvents.push_back(event); });
	assert(backend.titleSubscriptions == 1);
	assert(titleEvents.size() == 1 && titleEvents.front().titleId == 0x1234);
	assert(titleEvents.front().managedEntry->locationUid == 17);
	assert(titleEvents.front().managedEntry->name == "Test save");
	titleSubscription.Reset();
	assert(backend.titleSubscriptionStopped);
	const std::array<std::filesystem::path, 2> scanPaths{"games-a", "games-b"};
	controller.ReplaceTitleScanPaths(scanPaths);
	controller.RefreshTitles();
	controller.AddTitleFromPath("installed-title");
	assert(backend.scanPaths.size() == 2 && backend.titleRefreshes == 1 &&
		backend.addedTitle == "installed-title");
	const auto conversionPlan = controller.PlanWuaConversion(0x1234, 17);
	assert(conversionPlan && conversionPlan->items.front().locationUid == 17);
	Application::ContentOperationProgress conversionProgress;
	const std::array<std::uint64_t, 1> conversionItems{17};
	assert(controller.ConvertToWua(conversionItems, "test.wua",
		[&](const auto& progress) { conversionProgress = progress; }, [] { return false; }));
	assert(conversionProgress.phase == Application::ContentOperationPhase::Finalizing);
	const auto checksum = controller.ComputeTitleChecksum(17,
		[&](const auto& progress) { conversionProgress = progress; }, [] { return false; });
	assert(checksum && checksum.checksum->titleId == 0x1234);
	assert(conversionProgress.phase == Application::ContentOperationPhase::Hashing);
	const auto cancelledChecksum = controller.ComputeTitleChecksum(17, {},
		[] { return true; });
	assert(!cancelledChecksum &&
		cancelledChecksum.error == Application::ContentOperationError::Cancelled);
	backend.gameProfile = {
		.settings = {
			.loadSharedLibraries = false,
			.startWithPadView = true,
			.cpuMode = Application::GameProfileCpuMode::MultiCoreRecompiler,
			.threadQuantum = 60000,
			.graphicsApi = Application::GameProfileGraphicsApi::Vulkan,
			.accurateShaderMultiplication = true,
		},
		.gameName = "Test game",
		.defaultProfile = false,
	};
	const auto profile = controller.LoadGameProfile(0x1234);
	assert(profile.gameName == "Test game" && !profile.defaultProfile);
	assert(profile.settings.cpuMode == Application::GameProfileCpuMode::MultiCoreRecompiler);
	auto updatedProfile = profile.settings;
	updatedProfile.threadQuantum = 80000;
	updatedProfile.controllerProfiles[0] = "Controller 1";
	assert(controller.SaveGameProfile(0x1234, updatedProfile));
	assert(backend.gameProfileSaves == 1 &&
		backend.gameProfile.settings.threadQuantum == 80000 &&
		backend.gameProfile.settings.controllerProfiles[0] == "Controller 1");
	const auto installPlan = controller.PlanTitleInstall("source-title");
	assert(installPlan && installPlan.plan->targetPath == "installed-title");
	Application::TitleInstallProgress installProgress;
	const auto installed = controller.InstallTitle(*installPlan.plan,
		Application::TitleInstallDecision::AcceptConflict,
		[&](const auto& value) { installProgress = value; }, [] { return false; });
	assert(installed && installed.installedPath == "installed-title");
	assert(backend.titleInstalls == 1 &&
		backend.installDecision == Application::TitleInstallDecision::AcceptConflict &&
		installProgress.bytesCompleted == 10);
	const auto cancelledInstall = controller.InstallTitle(*installPlan.plan,
		Application::TitleInstallDecision::Proceed, {}, [] { return true; });
	assert(!cancelledInstall &&
		cancelledInstall.error == Application::TitleInstallError::Cancelled);
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
