#include "application/EmulationController.h"

#include <cassert>
#include <array>
#include <chrono>
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
			if (throwOnPrepare)
				throw std::runtime_error("prepare failed");
			return {nextError, request.path, request.path, "Test title", {}};
		}
		void Start() override
		{
			++starts;
			if (throwOnStart)
				throw std::runtime_error("start failed");
		}
		bool AbortPrepared() override
		{
			++aborts;
			return true;
		}
		bool Stop() override
		{
			++stops;
			if (throwOnStop)
				throw std::runtime_error("stop failed");
			return !failStop;
		}
		bool ShutdownApplication() override
		{
			return true;
		}
		bool IsTitleRunning() const override
		{
			return starts > stops;
		}
		std::optional<std::uint64_t> RunningTitleId() const override
		{
			return {};
		}
		std::optional<std::int32_t> ForegroundProcessExitStatus() const override
		{
			return {};
		}
		std::optional<Application::WindowTitlePresentation> windowTitlePresentation;
		std::optional<Application::WindowTitlePresentation>
		CurrentWindowTitlePresentation() const override
		{
			return windowTitlePresentation;
		}
		void SubmitKeyboard(std::uint16_t, bool, std::uint8_t) override {}
		void SubmitText(std::uint32_t, bool) override {}
		void KeyboardFocusLost() override {}
		bool softwareKeyboardActive{};
		std::vector<std::uint32_t> softwareKeyboardKeys;
		bool SoftwareKeyboardActive() const override
		{
			return softwareKeyboardActive;
		}
		bool SubmitSoftwareKeyboardKey(std::uint32_t keyCode) override
		{
			if (!softwareKeyboardActive)
				return false;
			softwareKeyboardKeys.push_back(keyCode);
			return true;
		}
		Application::NfcTouchResult nfcTouchResult{Application::NfcTouchResult::Inactive};
		std::filesystem::path nfcTouchPath;
		Application::NfcTouchResult TouchNfcTagFromFile(
			const std::filesystem::path& path) override
		{
			nfcTouchPath = path;
			return nfcTouchResult;
		}
		void PointerFocusChanged(bool) override {}
		void SubmitMouse(const Application::MouseInput&) override {}
		Application::PointerPolicy GetPointerPolicy() override
		{
			return {};
		}
		Application::TextInputState GetTextInputState() override
		{
			return {};
		}
		void SubmitTextComposition(std::string_view, std::string_view,
								   std::uint32_t, std::uint32_t) override {}
		void SaveCemodPermissionDecisions(std::uint64_t,
										  std::span<const Application::CemodPermissionDecision>) override {}
		std::vector<Application::CemodPackage> DiscoverCemodCatalog() override
		{
			return {};
		}
		std::vector<Application::CemodPackage> DiscoverCemods(std::uint64_t) override
		{
			return {};
		}
		Application::CemodGrant ResolveCemodGrant(std::uint64_t, std::string_view,
												  std::string_view, std::uint32_t) override
		{
			return {};
		}
		Application::CemuExtendServiceGrantDefaults ServiceGrantDefaults() const override
		{
			return {};
		}
		bool ImportLegacyCemodData(std::uint64_t, std::string_view,
								   std::string&) override
		{
			return false;
		}
		Application::CemodManagerSnapshot cemodManagerSnapshot;
		Application::CemodManagerSnapshot GetCemodManagerSnapshot(
			std::optional<std::uint64_t> titleId,
			Application::CemodCancellationCheck = {}) override
		{
			auto result = cemodManagerSnapshot;
			result.selectedTitleId = titleId;
			return result;
		}
		Application::CemodManagerResult SaveCemodApproval(
			const Application::CemodApprovalUpdate&) override
		{
			return {Application::CemodManagerError::None, {}, cemodManagerSnapshot};
		}
		Application::CemodManagerResult ImportLegacyCemodPackageData(
			std::uint64_t, std::uint64_t, std::string_view) override
		{
			return {Application::CemodManagerError::None, {}, cemodManagerSnapshot};
		}
		Application::PpcThreadsSnapshot diagnosticsSnapshot;
		Application::PpcThreadCommandRequest lastDiagnosticCommand;
		Application::PpcThreadCommandResult diagnosticCommandResult{true, {}};
		Application::PpcThreadsSnapshot CapturePpcThreads() override
		{
			return diagnosticsSnapshot;
		}
		Application::PpcThreadCommandResult ExecutePpcThreadCommand(
			const Application::PpcThreadCommandRequest& request) override
		{
			lastDiagnosticCommand = request;
			return diagnosticCommandResult;
		}
		std::vector<Application::TitleSummary> titles;
		std::vector<Application::ManagedContentEntry> managedContent;
		std::vector<Application::GameSummary> games;
		std::vector<Application::TitleCatalogEvent> catalogEvents;
		std::vector<std::filesystem::path> scanPaths;
		int titleRefreshes{};
		int titleSubscriptions{};
		bool titleSubscriptionStopped{};
		std::filesystem::path addedTitle;
		std::vector<Application::TitleSummary> ListTitles() const override
		{
			return titles;
		}
		std::vector<Application::ManagedContentEntry> ListManagedContent() const override
		{
			return managedContent;
		}
		std::optional<Application::TitleSummary> ResolveBaseTitle(
			std::uint64_t titleId) const override
		{
			const auto found = std::ranges::find_if(titles,
													[titleId](const auto& title) { return title.titleId == titleId; });
			return found == titles.end() ? std::nullopt : std::optional{*found};
		}
		std::vector<Application::GameSummary> ListGames() const override
		{
			return games;
		}
		std::optional<Application::GameSummary> GetGame(std::uint64_t titleId) const override
		{
			const auto found = std::ranges::find_if(games,
													[titleId](const auto& game) { return game.titleId == titleId; });
			return found == games.end() ? std::nullopt : std::optional{*found};
		}
		bool IsTitleScanning() const override
		{
			return false;
		}
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
				void Stop() override
				{
					stopped = true;
				}
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
		void RefreshTitles() override
		{
			++titleRefreshes;
		}
		void AddTitleFromPath(const std::filesystem::path& path) override
		{
			addedTitle = path;
		}
		std::optional<Application::WuaConversionPlan> PlanWuaConversion(
			std::uint64_t titleId, std::uint64_t preferredLocationUid) const override
		{
			return Application::WuaConversionPlan{
				.items = {{preferredLocationUid, titleId, 1, 55,
						   Application::ContentRole::Base, "base-path"}},
				.suggestedFileName = "Test title.wua",
			};
		}
		Application::ContentOperationResult ConvertToWua(
			const Application::WuaConversionPlan&, const std::filesystem::path&,
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
			return {Application::ContentOperationError::None, {}, Application::ContentChecksum{.titleId = 0x1234, .version = 1, .region = 2, .imageSha256 = std::string(64, locationUid == 0 ? '0' : '1')}};
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
		Application::ManagedContentDeletePlan deletePlan{
			.locationUid = 42, .titleId = 0x1234, .fingerprint = 99, .name = "Test", .displayPath = "managed-title"};
		int managedContentDeletes{};
		Application::ManagedContentDeletePlanResult PlanManagedContentDelete(
			std::uint64_t) const override
		{
			return {Application::ManagedContentDeleteError::None, {}, deletePlan};
		}
		Application::ManagedContentDeleteResult DeleteManagedContent(
			const Application::ManagedContentDeletePlan&) override
		{
			++managedContentDeletes;
			return {};
		}
		std::vector<Application::AccountInfo> accounts{{
			.persistentId = Application::kMinimumPersistentId,
			.miiName = L"default",
		}};
		Application::DownloadAccountContext downloadAccountContext;
		std::vector<Application::AccountInfo> ListAccounts() const override
		{
			return accounts;
		}
		std::optional<Application::AccountInfo> GetAccount(
			std::uint32_t persistentId) const override
		{
			const auto found = std::ranges::find_if(accounts,
													[persistentId](const auto& account) {
														return account.persistentId == persistentId;
													});
			return found == accounts.end() ? std::nullopt : std::optional{*found};
		}
		std::uint32_t NextPersistentId() const override
		{
			return Application::kMinimumPersistentId +
				   static_cast<std::uint32_t>(accounts.size());
		}
		bool HasFreeAccountSlots() const override
		{
			return accounts.size() < Application::kMaximumAccountCount;
		}
		std::vector<Application::AccountCountry> ListAccountCountries() const override
		{
			return {{1, "US"}};
		}
		Application::OnlineEnvironmentStatus GetOnlineEnvironmentStatus() const override
		{
			return {.requiredFilesAvailable = true, .otpPresent = true, .seepromPresent = true, .consoleCertificateAvailable = true};
		}
		Application::AccountManagerSnapshot GetAccountManagerSnapshot() const override
		{
			return {.accounts = accounts, .countries = ListAccountCountries(), .networkSettings = {{accounts.front().persistentId, Application::AccountNetworkService::Offline, ValidateOnlineAccount(accounts.front().persistentId)}}, .onlineEnvironment = GetOnlineEnvironmentStatus(), .activePersistentId = accounts.front().persistentId, .nextPersistentId = NextPersistentId(), .hasFreeSlots = HasFreeAccountSlots()};
		}
		Application::FrontendSettingsSnapshot frontendSettings{.revision = 1};
		Application::FrontendSettingsSnapshot GetFrontendSettings() const override
		{
			return frontendSettings;
		}
		Application::FrontendSettingsResult ApplyFrontendSettings(
			const Application::FrontendSettingsUpdate& update) override
		{
			if (update.expectedRevision != frontendSettings.revision)
				return {Application::FrontendSettingsError::Conflict, frontendSettings,
						"revision conflict"};
			frontendSettings.gamePaths = update.gamePaths;
			frontendSettings.startFullscreen = update.startFullscreen;
			frontendSettings.openPad = update.openPad;
			frontendSettings.checkUpdates = update.checkUpdates;
			frontendSettings.saveScreenshots = update.saveScreenshots;
			frontendSettings.setupCompleted |= update.completeSetup;
			++frontendSettings.revision;
			return {Application::FrontendSettingsError::None, frontendSettings, {}};
		}
		Application::AccountOperationResult SetActiveAccount(
			std::uint32_t persistentId) override
		{
			const auto account = GetAccount(persistentId);
			return account ? Application::AccountOperationResult{.account = account} : Application::AccountOperationResult{Application::AccountOperationError::NotFound};
		}
		Application::AccountOperationResult SetAccountNetworkService(
			std::uint32_t persistentId, Application::AccountNetworkService) override
		{
			return SetActiveAccount(persistentId);
		}
		Application::DownloadAccountContext GetDownloadAccountContext(
			std::optional<std::uint32_t>) const override
		{
			return downloadAccountContext;
		}
		Application::AccountValidation ValidateOnlineAccount(
			std::uint32_t persistentId) const override
		{
			return {.validAccount = GetAccount(persistentId).has_value(),
					.otp = Application::AccountFileState::Ok,
					.seeprom = Application::AccountFileState::Ok};
		}
		Application::AccountOperationResult CreateAccount(
			std::uint32_t persistentId, std::wstring_view miiName) override
		{
			accounts.push_back({.persistentId = persistentId,
								.miiName = std::wstring(miiName)});
			return {.account = accounts.back()};
		}
		Application::AccountOperationResult UpdateAccount(
			std::uint32_t persistentId, const Application::AccountUpdate& update) override
		{
			auto found = std::ranges::find_if(accounts,
											  [persistentId](const auto& account) {
												  return account.persistentId == persistentId;
											  });
			if (found == accounts.end())
				return {Application::AccountOperationError::NotFound};
			found->miiName = update.miiName;
			return {.account = *found};
		}
		Application::AccountOperationResult DeleteAccount(
			std::uint32_t persistentId) override
		{
			std::erase_if(accounts, [persistentId](const auto& account) {
				return account.persistentId == persistentId;
			});
			return {};
		}
		std::vector<std::uint32_t> savePersistentIds{
			Application::kMinimumPersistentId};
		Application::SaveEntryLocation saveLocation{
			Application::SaveEntryState::Directory, "save-directory"};
		Application::SaveImportInspection saveInspection{
			Application::SaveOperationError::None, {}, 0x1234, saveLocation};
		int saveDeletes{};
		int saveTransfers{};
		int saveImports{};
		int saveExports{};
		std::vector<std::uint32_t> ListSavePersistentIds(std::uint64_t) const override
		{
			return savePersistentIds;
		}
		Application::SaveEntryLocation InspectSaveEntry(
			std::uint64_t, std::uint32_t) const override
		{
			return saveLocation;
		}
		Application::SaveImportInspection InspectSaveImport(
			const std::filesystem::path&, std::uint64_t,
			std::uint32_t) const override
		{
			return saveInspection;
		}
		Application::SaveOperationResult DeleteSave(
			std::uint64_t, std::uint32_t) override
		{
			++saveDeletes;
			return {};
		}
		Application::SaveOperationResult TransferSave(
			std::uint64_t, std::uint32_t, std::uint32_t, bool) override
		{
			++saveTransfers;
			return {};
		}
		Application::SaveOperationResult ImportSave(
			const std::filesystem::path&, std::uint64_t, std::uint32_t, bool,
			Application::SaveProgressHandler progress,
			Application::SaveCancellationCheck cancelled) override
		{
			++saveImports;
			if (cancelled && cancelled())
				return {Application::SaveOperationError::Cancelled, "cancelled"};
			if (progress)
				progress({1, 1, 4, 4, "save-file"});
			return {};
		}
		Application::SaveOperationResult ExportSave(
			std::uint64_t, std::uint32_t, const std::filesystem::path&, bool,
			Application::SaveProgressHandler progress,
			Application::SaveCancellationCheck cancelled) override
		{
			++saveExports;
			if (cancelled && cancelled())
				return {Application::SaveOperationError::Cancelled, "cancelled"};
			if (progress)
				progress({1, 1, 4, 4, "save-file"});
			return {};
		}
		std::vector<Application::GraphicPackInfo> graphicPacks;
		int graphicPackRefreshes{};
		int graphicPackSaves{};
		int graphicPackInstalls{};
		std::vector<Application::GraphicPackInfo> ListGraphicPacks() const override
		{
			return graphicPacks;
		}
		Application::GraphicPackResult SetGraphicPackEnabled(
			std::string_view key, bool enabled) override
		{
			return {.changed = enabled, .info = Application::GraphicPackInfo{.key = std::string(key), .enabled = enabled}};
		}
		Application::GraphicPackResult SetGraphicPackPreset(std::string_view key,
															std::string_view category, std::string_view preset) override
		{
			return {.changed = true, .info = Application::GraphicPackInfo{.key = std::string(key), .presets = {{std::string(category), std::string(preset), true, true}}}};
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
		Application::GraphicPackInstallResult InstallGraphicPacks(
			const Application::GraphicPackInstallRequest&,
			Application::GraphicPackInstallProgressHandler progress,
			Application::GraphicPackInstallCancellationCheck cancelled) override
		{
			++graphicPackInstalls;
			if (cancelled && cancelled())
				return {Application::GraphicPackInstallError::Cancelled, "cancelled"};
			if (progress)
				progress({Application::GraphicPackInstallPhase::Downloading, 5, 10, {}});
			return {};
		}
		void SaveGraphicPackState() override
		{
			++graphicPackSaves;
		}
		Application::InputSettingsModel GetInputSettings() const override
		{
			return {};
		}
		Application::InputDeviceEnumerationResult EnumerateInputDevices(
			std::string_view) override
		{
			return {};
		}
		Application::InputSettingsResult SetEmulatedController(std::uint32_t,
															   Application::EmulatedControllerType, bool) override
		{
			return {};
		}
		Application::InputSettingsResult AddInputDevice(std::uint32_t,
														std::uint64_t) override
		{
			return {};
		}
		Application::InputSettingsResult RemoveInputDevice(std::uint32_t,
														   std::uint64_t) override
		{
			return {};
		}
		Application::InputSettingsResult ConnectInputDevice(std::uint64_t) override
		{
			return {};
		}
		std::optional<Application::CapturedInputButton> CaptureInputButton(
			std::uint64_t) override
		{
			return {};
		}
		Application::InputSettingsResult SetInputMapping(std::uint32_t, std::uint64_t,
														 std::uint64_t, std::uint64_t) override
		{
			return {};
		}
		Application::InputSettingsResult ClearInputMapping(std::uint32_t,
														   std::optional<std::uint64_t>) override
		{
			return {};
		}
		Application::InputSettingsResult SetPhysicalControllerSettings(std::uint64_t,
																	   const Application::PhysicalControllerSettings&) override
		{
			return {};
		}
		Application::InputSettingsResult CalibrateInputDevice(std::uint64_t) override
		{
			return {};
		}
		Application::InputSettingsResult LoadInputProfile(std::uint32_t,
														  std::string_view) override
		{
			return {};
		}
		Application::InputSettingsResult SaveInputProfile(std::uint32_t,
														  std::string_view) override
		{
			return {};
		}
		Application::InputSettingsResult DeleteInputProfile(std::string_view) override
		{
			return {};
		}
		Application::HotkeySettingsModel hotkeySettings;
		Application::HotkeySettingsModel GetHotkeySettings() const override
		{
			return hotkeySettings;
		}
		Application::HotkeySettingsResult ApplyHotkeySettings(
			const Application::HotkeySettingsUpdate& update) override
		{
			hotkeySettings.revision = update.revision + 1;
			hotkeySettings.controllerModifier = update.controllerModifier;
			hotkeySettings.bindings = update.bindings;
			return {.snapshot = hotkeySettings};
		}
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

	Application::HotkeySettingsUpdate ValidHotkeyUpdate(bool includeEndEmulation)
	{
		Application::HotkeySettingsUpdate update;
		update.revision = 1;
		update.controllerModifier = 1;
		for (const auto action : {
				 Application::HotkeyAction::ToggleFullscreen,
				 Application::HotkeyAction::ToggleFullscreenAlternative,
				 Application::HotkeyAction::ExitFullscreen,
				 Application::HotkeyAction::TakeScreenshot,
				 Application::HotkeyAction::ToggleFastForward,
				 Application::HotkeyAction::ExitApplication})
			update.bindings.push_back({.action = action});
		if (includeEndEmulation)
			update.bindings.push_back({.action = Application::HotkeyAction::EndEmulation});
		return update;
	}

	void VerifyManagedContentPathValidation()
	{
		namespace fs = std::filesystem;
		const auto unique = std::to_string(std::chrono::steady_clock::now()
											   .time_since_epoch()
											   .count());
		const auto root = fs::temp_directory_path() / ("cemu-managed-content-test-" + unique);
		const auto managed = root / "games" / "title";
		const auto outside = root / "outside";
		std::error_code error;
		fs::create_directories(managed, error);
		assert(!error);
		fs::create_directories(outside, error);
		assert(!error);
		const std::array<fs::path, 1> roots{root / "games"};
		static_assert(Application::IsManagedContentDeletionSupported(
			Application::ManagedContentType::Base));
		static_assert(!Application::IsManagedContentDeletionSupported(
			Application::ManagedContentType::Save));
		static_assert(!Application::IsManagedContentDeletionSupported(
			Application::ManagedContentType::System));
		assert(Application::ValidateManagedContentPath(managed, roots));
		assert(!Application::ValidateManagedContentPath(roots.front(), roots));
		assert(!Application::ValidateManagedContentPath(outside, roots));
		const auto linked = root / "games" / "linked";
		fs::create_directory_symlink(outside, linked, error);
		if (!error)
			assert(!Application::ValidateManagedContentPath(linked, roots));
		error.clear();
		fs::remove_all(root, error);
	}
} // namespace

int main()
{
	VerifyManagedContentPathValidation();
	{
		std::string diagnostic;
		auto release = ValidHotkeyUpdate(false);
		assert(Application::ValidateHotkeySettingsUpdate(release, false, diagnostic) ==
			   Application::HotkeySettingsError::None);
		auto debug = ValidHotkeyUpdate(true);
		assert(Application::ValidateHotkeySettingsUpdate(debug, true, diagnostic) ==
			   Application::HotkeySettingsError::None);
		release.bindings[0].keyboardUsage = 0x44;
		release.bindings[1].keyboardUsage = 0x44;
		assert(Application::ValidateHotkeySettingsUpdate(release, false, diagnostic) ==
			   Application::HotkeySettingsError::DuplicateBinding);
		release = ValidHotkeyUpdate(false);
		release.bindings[0].controllerButton = release.controllerModifier;
		assert(Application::ValidateHotkeySettingsUpdate(release, false, diagnostic) ==
			   Application::HotkeySettingsError::DuplicateBinding);
		release = ValidHotkeyUpdate(false);
		release.bindings[0].keyboardUsage = 0xe0;
		assert(Application::ValidateHotkeySettingsUpdate(release, false, diagnostic) ==
			   Application::HotkeySettingsError::InvalidBinding);
		assert(Application::ValidateHotkeySettingsUpdate(debug, false, diagnostic) ==
			   Application::HotkeySettingsError::InvalidBinding);
	}
	FakeBackend backend;
	Application::EmulationController controller(backend);
	assert(controller.State() == Application::EmulationState::Idle);
	backend.cemodManagerSnapshot = {
		.generation = 77,
		.packages = {
			{.packageKey = "digest-a", .titleIds = {0x1234}, .modIdentity = "mod-a", .packageDigest = "digest-a", .requestedPermissions = 1ULL << 10, .trustedNative = true, .valid = true},
			{.packageKey = "approved", .titleIds = {0x1234}, .modIdentity = "mod-b", .packageDigest = "digest-b", .approved = true, .valid = true},
			{.packageKey = "wrong-title", .titleIds = {0x5678}, .modIdentity = "mod-c", .packageDigest = "digest-c", .valid = true},
			{.packageKey = "invalid", .titleIds = {0x1234}, .modIdentity = "mod-d", .packageDigest = "digest-d", .valid = false},
		},
	};
	const auto preflight = controller.GetCemodLaunchPreflight(0x1234);
	assert(preflight.generation == 77 && preflight.titleId == 0x1234);
	assert(preflight.pendingApprovals.size() == 1);
	assert(preflight.pendingApprovals.front().packageKey == "digest-a");
	assert(preflight.pendingApprovals.front().requestedPermissions == (1ULL << 10));
	backend.cemodManagerSnapshot = {};
	bool preparedBeforeStart{};
	const auto launch = controller.Launch({"test.rpx"}, [&](const auto&) {
		preparedBeforeStart = controller.State() == Application::EmulationState::Preparing;
	});
	assert(launch);
	assert(preparedBeforeStart);
	assert(controller.State() == Application::EmulationState::Running);
	assert(backend.prepares == 1 && backend.starts == 1);
	assert(!controller.CurrentWindowTitlePresentation());
	backend.windowTitlePresentation = {
		.titleId = 0x0005000012345678,
		.titleName = "Test title",
		.version = 17,
		.region = Application::TitleRegion::UnitedStates,
		.renderer = Application::PresentationRenderer::Vulkan,
		.gpuVendor = Application::PresentationGpuVendor::Amd,
	};
	const auto presentation = controller.CurrentWindowTitlePresentation();
	assert(presentation && presentation->titleId == 0x0005000012345678 &&
		   presentation->titleName == "Test title" && presentation->version == 17 &&
		   presentation->region == Application::TitleRegion::UnitedStates &&
		   presentation->renderer == Application::PresentationRenderer::Vulkan &&
		   presentation->gpuVendor == Application::PresentationGpuVendor::Amd);
	assert(!controller.SoftwareKeyboardActive());
	assert(!controller.SubmitSoftwareKeyboardKey('x'));
	backend.softwareKeyboardActive = true;
	assert(controller.SoftwareKeyboardActive());
	assert(controller.SubmitSoftwareKeyboardKey('x'));
	assert(backend.softwareKeyboardKeys == std::vector<std::uint32_t>{'x'});
	assert(controller.TouchNfcTagFromFile("missing.nfc") ==
		   Application::NfcTouchResult::Inactive);
	backend.nfcTouchResult = Application::NfcTouchResult::Success;
	assert(controller.TouchNfcTagFromFile("tag.nfc") ==
		   Application::NfcTouchResult::Success);
	assert(backend.nfcTouchPath == std::filesystem::path{"tag.nfc"});
	backend.titles.push_back({0x1234, "Test title", "test-title"});
	backend.managedContent.push_back({.locationUid = 42, .titleId = 0x1234, .path = "test-title", .name = "Test title", .version = 17, .type = Application::ManagedContentType::Base});
	backend.games.push_back({.titleId = 0x1234, .name = "Test title", .basePath = "test-title", .version = 17});
	const auto titles = controller.ListTitles();
	assert(titles.size() == 1 && titles.front().titleId == 0x1234);
	assert(controller.ListManagedContent().front().locationUid == 42);
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
	assert(controller.ConvertToWua(*conversionPlan, "test.wua", [&](const auto& progress) { conversionProgress = progress; }, [] { return false; }));
	assert(conversionProgress.phase == Application::ContentOperationPhase::Finalizing);
	const auto checksum = controller.ComputeTitleChecksum(17, [&](const auto& progress) { conversionProgress = progress; }, [] { return false; });
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
	const auto installed = controller.InstallTitle(*installPlan.plan, Application::TitleInstallDecision::AcceptConflict, [&](const auto& value) { installProgress = value; }, [] { return false; });
	assert(installed && installed.installedPath == "installed-title");
	assert(backend.titleInstalls == 1 &&
		   backend.installDecision == Application::TitleInstallDecision::AcceptConflict &&
		   installProgress.bytesCompleted == 10);
	const auto cancelledInstall = controller.InstallTitle(*installPlan.plan,
														  Application::TitleInstallDecision::Proceed, {}, [] { return true; });
	assert(!cancelledInstall &&
		   cancelledInstall.error == Application::TitleInstallError::Cancelled);
	const auto deletePlan = controller.PlanManagedContentDelete(42);
	assert(deletePlan && deletePlan.plan->locationUid == 42 &&
		   deletePlan.plan->fingerprint == 99);
	assert(controller.DeleteManagedContent(*deletePlan.plan));
	assert(backend.managedContentDeletes == 1);
	backend.graphicPacks.push_back({.key = "pack-key", .name = "Pack"});
	assert(controller.ListGraphicPacks().front().key == "pack-key");
	assert(controller.SetGraphicPackEnabled("pack-key", true).changed);
	assert(controller.SetGraphicPackPreset("pack-key", "quality", "high").changed);
	assert(controller.ReloadGraphicPack("pack-key").reloaded);
	assert(controller.RefreshGraphicPacks());
	controller.SaveGraphicPackState();
	assert(backend.graphicPackRefreshes == 1 && backend.graphicPackSaves == 1);
	Application::GraphicPackInstallProgress packInstallProgress;
	assert(controller.InstallGraphicPacks(
		{.kind = Application::GraphicPackInstallKind::Community},
		[&packInstallProgress](const auto& value) { packInstallProgress = value; },
		[] { return false; }));
	assert(backend.graphicPackInstalls == 1 && packInstallProgress.completed == 5);
	const auto accounts = controller.ListAccounts();
	assert(accounts.size() == 1 &&
		   accounts.front().persistentId == Application::kMinimumPersistentId &&
		   accounts.front().miiName == L"default");
	assert(controller.GetAccount(Application::kMinimumPersistentId));
	assert(!controller.GetAccount(0x8fffffff));
	assert(controller.NextPersistentId() == Application::kMinimumPersistentId + 1);
	assert(controller.HasFreeAccountSlots());
	assert(controller.ListAccountCountries().front().name == "US");
	const auto onlineEnvironment = controller.GetOnlineEnvironmentStatus();
	assert(onlineEnvironment.requiredFilesAvailable && onlineEnvironment.otpPresent &&
		   onlineEnvironment.seepromPresent &&
		   onlineEnvironment.consoleCertificateAvailable);
	const auto accountSnapshot = controller.GetAccountManagerSnapshot();
	assert(accountSnapshot.activePersistentId == Application::kMinimumPersistentId &&
		   accountSnapshot.accounts.size() == 1 &&
		   accountSnapshot.networkSettings.size() == 1);
	assert(controller.SetActiveAccount(Application::kMinimumPersistentId));
	assert(controller.SetAccountNetworkService(Application::kMinimumPersistentId,
											   Application::AccountNetworkService::Pretendo));
	const auto frontendSettings = controller.GetFrontendSettings();
	const auto frontendUpdate = controller.ApplyFrontendSettings({
		.expectedRevision = frontendSettings.revision,
		.gamePaths = {"/games"},
		.startFullscreen = true,
		.openPad = true,
		.checkUpdates = false,
		.saveScreenshots = false,
		.completeSetup = true,
	});
	assert(frontendUpdate && frontendUpdate.snapshot.revision == frontendSettings.revision + 1 && frontendUpdate.snapshot.setupCompleted &&
		   !frontendUpdate.snapshot.saveScreenshots &&
		   frontendUpdate.snapshot.gamePaths == std::vector<fs::path>{"/games"});
	const auto staleFrontendUpdate = controller.ApplyFrontendSettings({
		.expectedRevision = frontendSettings.revision,
	});
	assert(!staleFrontendUpdate && staleFrontendUpdate.error ==
									   Application::FrontendSettingsError::Conflict);
	backend.downloadAccountContext = {.accountName = "test-account", .region = 2};
	const auto downloadContext = controller.GetDownloadAccountContext(
		Application::kMinimumPersistentId);
	assert(downloadContext && downloadContext.accountName == "test-account" &&
		   downloadContext.region == 2);
	const auto validation = controller.ValidateOnlineAccount(
		Application::kMinimumPersistentId);
	assert(validation.validAccount &&
		   validation.otp == Application::AccountFileState::Ok &&
		   validation.seeprom == Application::AccountFileState::Ok);
	const auto createdAccount = controller.CreateAccount(
		Application::kMinimumPersistentId + 1, L"second");
	assert(createdAccount && createdAccount.account->miiName == L"second");
	Application::AccountUpdate accountUpdate{.miiName = L"updated"};
	const auto updatedAccount = controller.UpdateAccount(
		Application::kMinimumPersistentId + 1, accountUpdate);
	assert(updatedAccount && updatedAccount.account->miiName == L"updated");
	assert(controller.DeleteAccount(Application::kMinimumPersistentId + 1));
	assert(controller.ListAccounts().size() == 1);
	assert(controller.ListSavePersistentIds(0x1234) == backend.savePersistentIds);
	assert(controller.InspectSaveEntry(0x1234, Application::kMinimumPersistentId).path ==
		   "save-directory");
	const auto saveInspection = controller.InspectSaveImport("save.zip", 0x1234,
															 Application::kMinimumPersistentId);
	assert(saveInspection && saveInspection.sourceTitleId == 0x1234);
	assert(controller.DeleteSave(0x1234, Application::kMinimumPersistentId));
	assert(controller.TransferSave(0x1234, Application::kMinimumPersistentId,
								   Application::kMinimumPersistentId + 1, true));
	Application::SaveOperationProgress saveProgress;
	assert(controller.ImportSave("save.zip", 0x1234, Application::kMinimumPersistentId, false, [&](const auto& value) { saveProgress = value; }, [] { return false; }));
	assert(saveProgress.bytesCompleted == 4);
	assert(controller.ExportSave(0x1234, Application::kMinimumPersistentId,
								 "save-export.zip", true, {}, [] { return false; }));
	assert(!controller.ImportSave("save.zip", 0x1234,
								  Application::kMinimumPersistentId, false, {}, [] { return true; }));
	assert(backend.saveDeletes == 1 && backend.saveTransfers == 1 &&
		   backend.saveImports == 2 && backend.saveExports == 1);
	backend.diagnosticsSnapshot = {
		.generation = 9,
		.available = true,
		.threads = {{.address = 0x1000, .name = "test-thread"}},
	};
	const auto diagnostics = controller.CapturePpcThreads();
	assert(diagnostics.available && diagnostics.generation == 9 &&
		   diagnostics.threads.front().name == "test-thread");
	const Application::PpcThreadCommandRequest diagnosticCommand{
		.generation = diagnostics.generation,
		.threadAddress = diagnostics.threads.front().address,
		.threadIdentity = diagnostics.threads.front().identity,
		.command = Application::PpcThreadCommand::Suspend,
	};
	assert(controller.ExecutePpcThreadCommand(diagnosticCommand).applied);
	assert(backend.lastDiagnosticCommand.threadAddress == 0x1000);
	const auto stop = controller.Stop();
	assert(stop.stopped);
	assert(controller.State() == Application::EmulationState::Idle);
	assert(backend.stops == 1);
	assert(!controller.CapturePpcThreads().available);
	assert(!controller.ExecutePpcThreadCommand(diagnosticCommand).applied);

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
