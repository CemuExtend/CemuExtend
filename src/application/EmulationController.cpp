#include "application/EmulationController.h"

#include <algorithm>
#include <exception>
#include <ranges>
#include <utility>

namespace Application
{
	std::unique_ptr<IEmulationBackend> CreateCafeEmulationBackend(ApplicationEvents& events);

	EmulationController::EmulationController()
		: m_ownedBackend(CreateCafeEmulationBackend(m_events)), m_backend(m_ownedBackend.get())
	{
	}

	EmulationController::EmulationController(IEmulationBackend& backend)
		: m_backend(&backend)
	{
	}

	EmulationController::~EmulationController()
	{
		if (State() == EmulationState::Running)
			(void)Stop();
	}

	std::optional<std::uint64_t> EmulationController::ResolveLaunchTitleId(
		const std::filesystem::path& path) const
	{
		return m_backend->ResolveLaunchTitleId(path);
	}

	LaunchResult EmulationController::Launch(const LaunchRequest& request,
											 BeforeStart beforeStart, StartFailure startFailure)
	{
		std::scoped_lock operationLock(m_operationMutex);
		{
			std::scoped_lock lock(m_mutex);
			if (m_state != EmulationState::Idle)
				return {LaunchError::InvalidState, request.path, {}, {}, "emulation is already preparing, running, or stopping"};
			m_state = EmulationState::Preparing;
		}

		LaunchResult result;
		try
		{
			result = m_backend->Prepare(request);
		} catch (const std::exception& ex)
		{
			bool aborted{};
			try
			{
				aborted = m_backend->AbortPrepared();
			} catch (...)
			{}
			std::scoped_lock lock(m_mutex);
			m_state = aborted ? EmulationState::Idle : EmulationState::Running;
			return {LaunchError::BackendFailure, request.path, {}, {}, ex.what()};
		} catch (...)
		{
			bool aborted{};
			try
			{
				aborted = m_backend->AbortPrepared();
			} catch (...)
			{}
			std::scoped_lock lock(m_mutex);
			m_state = aborted ? EmulationState::Idle : EmulationState::Running;
			return {LaunchError::BackendFailure, request.path, {}, {}, "unknown backend exception while preparing emulation"};
		}
		if (!result)
		{
			bool aborted{};
			try
			{
				aborted = m_backend->AbortPrepared();
			} catch (...)
			{}
			std::scoped_lock lock(m_mutex);
			m_state = aborted ? EmulationState::Idle : EmulationState::Running;
			if (!aborted)
			{
				if (!result.diagnostic.empty())
					result.diagnostic.append("; ");
				result.diagnostic.append("backend retained resources after failed preparation");
			}
			return result;
		}

		try
		{
			if (beforeStart)
				beforeStart(result);
			m_backend->Start();
		} catch (const std::exception& ex)
		{
			bool aborted{};
			try
			{
				aborted = m_backend->AbortPrepared();
			} catch (...)
			{}
			if (aborted && startFailure)
				try
				{
					startFailure();
				} catch (...)
				{}
			std::scoped_lock lock(m_mutex);
			m_state = aborted ? EmulationState::Idle : EmulationState::Running;
			result.error = LaunchError::BackendFailure;
			result.diagnostic = ex.what();
			return result;
		} catch (...)
		{
			bool aborted{};
			try
			{
				aborted = m_backend->AbortPrepared();
			} catch (...)
			{}
			if (aborted && startFailure)
				try
				{
					startFailure();
				} catch (...)
				{}
			std::scoped_lock lock(m_mutex);
			m_state = aborted ? EmulationState::Idle : EmulationState::Running;
			result.error = LaunchError::BackendFailure;
			result.diagnostic = "unknown exception while starting emulation";
			return result;
		}
		{
			std::scoped_lock lock(m_mutex);
			m_state = EmulationState::Running;
		}
		return result;
	}

	CemodLaunchPreflight EmulationController::GetCemodLaunchPreflight(
		std::uint64_t titleId)
	{
		const auto snapshot = m_backend->GetCemodManagerSnapshot(titleId);
		CemodLaunchPreflight result{snapshot.generation, titleId, {}};
		for (const auto& package : snapshot.packages)
		{
			const bool nativePermissionsDenied =
				(package.trustedNative || package.wups) &&
				(package.requestedPermissions & ~package.grantedPermissions) != 0;
			// Disabled packages never load, and native/WUPS packages only count as
			// approved when every requested permission is granted.
			if (!package.valid || !package.enabled ||
				(package.approved && !nativePermissionsDenied) ||
				package.packageDigest.empty() ||
				package.modIdentity.empty() ||
				std::ranges::find(package.titleIds, titleId) == package.titleIds.end())
				continue;
			result.pendingApprovals.push_back({snapshot.generation, titleId,
											   package.packageKey, package.packageDigest, package.modIdentity,
											   package.requestedPermissions});
		}
		return result;
	}

	StopResult EmulationController::Stop()
	{
		std::scoped_lock operationLock(m_operationMutex);
		{
			std::scoped_lock lock(m_mutex);
			if (m_state == EmulationState::Idle)
				return {false, "emulation is already idle"};
			if (m_state != EmulationState::Running)
				return {false, "emulation cannot stop while preparing or already stopping"};
			m_state = EmulationState::Stopping;
		}

		try
		{
			if (!m_backend->Stop())
			{
				std::scoped_lock lock(m_mutex);
				m_state = EmulationState::Running;
				return {false, "backend retained title resources during shutdown"};
			}
		} catch (const std::exception& ex)
		{
			std::scoped_lock lock(m_mutex);
			m_state = EmulationState::Running;
			return {false, ex.what()};
		} catch (...)
		{
			std::scoped_lock lock(m_mutex);
			m_state = EmulationState::Running;
			return {false, "unknown backend exception while stopping emulation"};
		}
		{
			std::scoped_lock lock(m_mutex);
			m_state = EmulationState::Idle;
		}
		return {true, {}};
	}

	StopResult EmulationController::ShutdownApplication()
	{
		std::scoped_lock operationLock(m_operationMutex);
		if (State() == EmulationState::Running)
		{
			const auto stopped = Stop();
			if (!stopped.stopped)
				return stopped;
		}
		{
			std::scoped_lock lock(m_mutex);
			if (m_state != EmulationState::Idle)
				return {false, "emulation lifecycle transition is still in progress"};
			m_state = EmulationState::Stopping;
		}
		try
		{
			if (!m_backend->ShutdownApplication())
			{
				std::scoped_lock lock(m_mutex);
				m_state = EmulationState::Idle;
				return {false, "backend retained resources during application shutdown"};
			}
		} catch (const std::exception& ex)
		{
			std::scoped_lock lock(m_mutex);
			m_state = EmulationState::Idle;
			return {false, ex.what()};
		} catch (...)
		{
			std::scoped_lock lock(m_mutex);
			m_state = EmulationState::Idle;
			return {false, "unknown backend exception during application shutdown"};
		}
		{
			std::scoped_lock lock(m_mutex);
			m_state = EmulationState::Idle;
		}
		return {true, {}};
	}

	EmulationState EmulationController::State() const
	{
		std::scoped_lock lock(m_mutex);
		return m_state;
	}

	bool EmulationController::IsTitleRunning() const
	{
		return m_backend->IsTitleRunning();
	}

	PpcThreadsSnapshot EmulationController::CapturePpcThreads()
	{
		std::scoped_lock operationLock(m_operationMutex);
		{
			std::scoped_lock lock(m_mutex);
			if (m_state != EmulationState::Running)
				return {.diagnostic = "PPC threads are available only while emulation is running"};
		}
		if (!m_backend->IsTitleRunning())
			return {.diagnostic = "the emulated title is not running"};
		return m_backend->CapturePpcThreads();
	}

	PpcThreadCommandResult EmulationController::ExecutePpcThreadCommand(
		const PpcThreadCommandRequest& request)
	{
		std::scoped_lock operationLock(m_operationMutex);
		{
			std::scoped_lock lock(m_mutex);
			if (m_state != EmulationState::Running)
				return {false, "PPC thread commands require running emulation"};
		}
		if (!m_backend->IsTitleRunning())
			return {false, "the emulated title is not running"};
		return m_backend->ExecutePpcThreadCommand(request);
	}

	std::optional<std::uint64_t> EmulationController::RunningTitleId() const
	{
		return m_backend->RunningTitleId();
	}

	std::optional<std::int32_t> EmulationController::ForegroundProcessExitStatus() const
	{
		return m_backend->ForegroundProcessExitStatus();
	}

	std::optional<WindowTitlePresentation>
	EmulationController::CurrentWindowTitlePresentation() const
	{
		return m_backend->CurrentWindowTitlePresentation();
	}

	void EmulationController::SubmitKeyboard(std::uint16_t usagePage, std::uint16_t usage, bool pressed,
										 std::uint8_t modifiers, std::uint16_t deviceId)
	{
		m_backend->SubmitKeyboard(usagePage, usage, pressed, modifiers, deviceId);
	}

	void EmulationController::UpdatePhysicalInputOwnership(
		const PhysicalInputOwnership& ownership)
	{
		m_backend->UpdatePhysicalInputOwnership(ownership);
	}

	void EmulationController::SubmitText(std::uint32_t codepoint, bool repeat)
	{
		m_backend->SubmitText(codepoint, repeat);
	}

	void EmulationController::KeyboardFocusLost()
	{
		m_backend->KeyboardFocusLost();
	}

	bool EmulationController::SoftwareKeyboardActive() const
	{
		return m_backend->SoftwareKeyboardActive();
	}

	bool EmulationController::SubmitSoftwareKeyboardKey(std::uint32_t keyCode)
	{
		return m_backend->SubmitSoftwareKeyboardKey(keyCode);
	}

	RuntimeOverlay::Snapshot EmulationController::GetRuntimeOverlaySnapshot()
	{
		return m_backend->GetRuntimeOverlaySnapshot();
	}

	bool EmulationController::SubmitRuntimeOverlayKeyboardKey(
		std::uint64_t generation, std::uint32_t keyCode)
	{
		return m_backend->SubmitRuntimeOverlayKeyboardKey(generation, keyCode);
	}

	bool EmulationController::SelectRuntimeOverlayErrorButton(
		std::uint64_t generation, bool rightButton)
	{
		return m_backend->SelectRuntimeOverlayErrorButton(generation, rightButton);
	}

	NfcTouchResult EmulationController::TouchNfcTagFromFile(
		const std::filesystem::path& path)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->TouchNfcTagFromFile(path);
	}

	void EmulationController::PointerFocusChanged(bool focused)
	{
		m_backend->PointerFocusChanged(focused);
	}

	void EmulationController::SubmitMouse(const MouseInput& input)
	{
		m_backend->SubmitMouse(input);
	}

	PointerPolicy EmulationController::GetPointerPolicy()
	{
		return m_backend->GetPointerPolicy();
	}

	TextInputState EmulationController::GetTextInputState()
	{
		return m_backend->GetTextInputState();
	}

	void EmulationController::SubmitTextComposition(std::string_view text,
													std::string_view preedit, std::uint32_t cursor, std::uint32_t selectionLength)
	{
		m_backend->SubmitTextComposition(text, preedit, cursor, selectionLength);
	}

	void EmulationController::SaveCemodPermissionDecisions(std::uint64_t titleId,
														   std::span<const CemodPermissionDecision> decisions)
	{
		m_backend->SaveCemodPermissionDecisions(titleId, decisions);
	}

	std::vector<CemodPackage> EmulationController::DiscoverCemodCatalog()
	{
		return m_backend->DiscoverCemodCatalog();
	}

	std::vector<CemodPackage> EmulationController::DiscoverCemods(std::uint64_t titleId)
	{
		return m_backend->DiscoverCemods(titleId);
	}

	CemodGrant EmulationController::ResolveCemodGrant(std::uint64_t titleId,
													  std::string_view modId, std::string_view principal,
													  std::uint32_t requestedPermissions)
	{
		return m_backend->ResolveCemodGrant(
			titleId, modId, principal, requestedPermissions);
	}

	CemuExtendServiceGrantDefaults EmulationController::ServiceGrantDefaults() const
	{
		return m_backend->ServiceGrantDefaults();
	}

	bool EmulationController::ImportLegacyCemodData(std::uint64_t titleId,
													std::string_view principal, std::string& error)
	{
		return m_backend->ImportLegacyCemodData(titleId, principal, error);
	}

	CemodManagerSnapshot EmulationController::GetCemodManagerSnapshot(
		std::optional<std::uint64_t> titleId, CemodCancellationCheck cancelled)
	{
		std::scoped_lock lock(m_operationMutex);
		return m_backend->GetCemodManagerSnapshot(titleId, std::move(cancelled));
	}

	CemodManagerResult EmulationController::SaveCemodApproval(
		const CemodApprovalUpdate& update)
	{
		std::scoped_lock lock(m_operationMutex);
		return m_backend->SaveCemodApproval(update);
	}

	CemodManagerResult EmulationController::ImportLegacyCemodPackageData(
		std::uint64_t generation, std::uint64_t titleId, std::string_view packageKey)
	{
		std::scoped_lock lock(m_operationMutex);
		return m_backend->ImportLegacyCemodPackageData(generation, titleId, packageKey);
	}

	CemodManagerResult EmulationController::SetCemodEnabled(const CemodEnableUpdate& update)
	{
		return m_backend->SetCemodEnabled(update);
	}

	FrontendSettingsSnapshot EmulationController::GetFrontendSettings() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->GetFrontendSettings();
	}

	FrontendSettingsResult EmulationController::ApplyFrontendSettings(
		const FrontendSettingsUpdate& update)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ApplyFrontendSettings(update);
	}

	InputSettingsModel EmulationController::GetInputSettings() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->GetInputSettings();
	}

	InputDeviceEnumerationResult EmulationController::EnumerateInputDevices(
		std::string_view api)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->EnumerateInputDevices(api);
	}

	InputSettingsResult EmulationController::SetEmulatedController(std::uint32_t player,
																   EmulatedControllerType type, bool preserveDevices)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->SetEmulatedController(player, type, preserveDevices);
	}

	InputSettingsResult EmulationController::AddInputDevice(
		std::uint32_t player, std::uint64_t token)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->AddInputDevice(player, token);
	}

	InputSettingsResult EmulationController::RemoveInputDevice(
		std::uint32_t player, std::uint64_t token)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->RemoveInputDevice(player, token);
	}

	InputSettingsResult EmulationController::ConnectInputDevice(std::uint64_t token)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ConnectInputDevice(token);
	}

	std::optional<CapturedInputButton> EmulationController::CaptureInputButton(std::uint64_t token)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->CaptureInputButton(token);
	}

	InputSettingsResult EmulationController::SetInputMapping(std::uint32_t player,
															 std::uint64_t mappingId, std::uint64_t controllerToken, std::uint64_t buttonId)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->SetInputMapping(player, mappingId, controllerToken, buttonId);
	}

	InputSettingsResult EmulationController::ClearInputMapping(std::uint32_t player,
															   std::optional<std::uint64_t> mappingId)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ClearInputMapping(player, mappingId);
	}

	InputSettingsResult EmulationController::SetPhysicalControllerSettings(
		std::uint64_t token, const PhysicalControllerSettings& settings)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->SetPhysicalControllerSettings(token, settings);
	}

	InputSettingsResult EmulationController::CalibrateInputDevice(std::uint64_t token)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->CalibrateInputDevice(token);
	}

	InputSettingsResult EmulationController::LoadInputProfile(
		std::uint32_t player, std::string_view profile)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->LoadInputProfile(player, profile);
	}

	InputSettingsResult EmulationController::SaveInputProfile(
		std::uint32_t player, std::string_view profile)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->SaveInputProfile(player, profile);
	}

	InputSettingsResult EmulationController::DeleteInputProfile(std::string_view profile)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->DeleteInputProfile(profile);
	}

	HotkeySettingsModel EmulationController::GetHotkeySettings() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->GetHotkeySettings();
	}

	HotkeySettingsResult EmulationController::ApplyHotkeySettings(
		const HotkeySettingsUpdate& update)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ApplyHotkeySettings(update);
	}

	std::vector<TitleSummary> EmulationController::ListTitles() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ListTitles();
	}

	std::vector<ManagedContentEntry> EmulationController::ListManagedContent() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ListManagedContent();
	}

	std::optional<TitleSummary> EmulationController::ResolveBaseTitle(
		std::uint64_t titleId) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ResolveBaseTitle(titleId);
	}

	std::vector<GameSummary> EmulationController::ListGames() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ListGames();
	}

	std::optional<GameSummary> EmulationController::GetGame(std::uint64_t titleId) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->GetGame(titleId);
	}

	bool EmulationController::IsTitleScanning() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->IsTitleScanning();
	}

	std::optional<std::vector<std::uint8_t>> EmulationController::LoadTitleIcon(
		std::uint64_t titleId) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->LoadTitleIcon(titleId);
	}

	TitleCatalogSubscription EmulationController::SubscribeTitleCatalog(
		TitleCatalogHandler handler)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->SubscribeTitleCatalogEvents(std::move(handler));
	}

	void EmulationController::ReplaceTitleScanPaths(
		std::span<const std::filesystem::path> paths)
	{
		std::scoped_lock operationLock(m_operationMutex);
		m_backend->ReplaceScanPaths(paths);
	}

	void EmulationController::RefreshTitles()
	{
		std::scoped_lock operationLock(m_operationMutex);
		m_backend->RefreshTitles();
	}

	void EmulationController::AddTitleFromPath(const std::filesystem::path& path)
	{
		std::scoped_lock operationLock(m_operationMutex);
		m_backend->AddTitleFromPath(path);
	}

	std::optional<WuaConversionPlan> EmulationController::PlanWuaConversion(
		std::uint64_t titleId, std::uint64_t preferredLocationUid) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->PlanWuaConversion(titleId, preferredLocationUid);
	}

	ContentOperationResult EmulationController::ConvertToWua(
		const WuaConversionPlan& plan,
		const std::filesystem::path& outputPath, ContentProgressHandler progress,
		ContentCancellationCheck cancelled)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ConvertToWua(plan, outputPath,
									   std::move(progress), std::move(cancelled));
	}

	ContentChecksumResult EmulationController::ComputeTitleChecksum(
		std::uint64_t locationUid, ContentProgressHandler progress,
		ContentCancellationCheck cancelled)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ComputeTitleChecksum(locationUid,
											   std::move(progress), std::move(cancelled));
	}

	GameProfileView EmulationController::LoadGameProfile(std::uint64_t titleId) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->LoadGameProfile(titleId);
	}

	GameProfileSaveResult EmulationController::SaveGameProfile(
		std::uint64_t titleId, const GameProfileUpdate& update)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->SaveGameProfile(titleId, update);
	}

	TitleInstallPlanResult EmulationController::PlanTitleInstall(
		const std::filesystem::path& sourcePath) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->PlanTitleInstall(sourcePath);
	}

	TitleInstallResult EmulationController::InstallTitle(const TitleInstallPlan& plan,
														 TitleInstallDecision decision, TitleInstallProgressHandler progress,
														 TitleInstallCancellationCheck cancelled)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->InstallTitle(plan, decision, std::move(progress),
									   std::move(cancelled));
	}

	ManagedContentDeletePlanResult EmulationController::PlanManagedContentDelete(
		std::uint64_t locationUid) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->PlanManagedContentDelete(locationUid);
	}

	ManagedContentDeleteResult EmulationController::DeleteManagedContent(
		const ManagedContentDeletePlan& plan)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->DeleteManagedContent(plan);
	}

	std::vector<GraphicPackInfo> EmulationController::ListGraphicPacks() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ListGraphicPacks();
	}

	GraphicPackResult EmulationController::SetGraphicPackEnabled(
		std::string_view key, bool enabled)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->SetGraphicPackEnabled(key, enabled);
	}

	GraphicPackResult EmulationController::SetGraphicPackPreset(
		std::string_view key, std::string_view category, std::string_view preset)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->SetGraphicPackPreset(key, category, preset);
	}

	GraphicPackResult EmulationController::ReloadGraphicPack(std::string_view key)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ReloadGraphicPack(key);
	}

	GraphicPackRefreshResult EmulationController::RefreshGraphicPacks()
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->RefreshGraphicPacks();
	}

	GraphicPackInstallResult EmulationController::InstallGraphicPacks(
		const GraphicPackInstallRequest& request,
		GraphicPackInstallProgressHandler progress,
		GraphicPackInstallCancellationCheck cancelled)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->InstallGraphicPacks(request, std::move(progress),
											  std::move(cancelled));
	}

	void EmulationController::SaveGraphicPackState()
	{
		std::scoped_lock operationLock(m_operationMutex);
		m_backend->SaveGraphicPackState();
	}

	std::vector<AccountInfo> EmulationController::ListAccounts() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ListAccounts();
	}

	std::optional<AccountInfo> EmulationController::GetAccount(
		std::uint32_t persistentId) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->GetAccount(persistentId);
	}

	std::uint32_t EmulationController::NextPersistentId() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->NextPersistentId();
	}

	bool EmulationController::HasFreeAccountSlots() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->HasFreeAccountSlots();
	}

	std::vector<AccountCountry> EmulationController::ListAccountCountries() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ListAccountCountries();
	}

	OnlineEnvironmentStatus EmulationController::GetOnlineEnvironmentStatus() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->GetOnlineEnvironmentStatus();
	}

	AccountManagerSnapshot EmulationController::GetAccountManagerSnapshot() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->GetAccountManagerSnapshot();
	}

	AccountOperationResult EmulationController::SetActiveAccount(
		std::uint32_t persistentId)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->SetActiveAccount(persistentId);
	}

	AccountOperationResult EmulationController::SetAccountNetworkService(
		std::uint32_t persistentId, AccountNetworkService service)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->SetAccountNetworkService(persistentId, service);
	}

	DownloadAccountContext EmulationController::GetDownloadAccountContext(
		std::optional<std::uint32_t> persistentId) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->GetDownloadAccountContext(persistentId);
	}

	AccountValidation EmulationController::ValidateOnlineAccount(
		std::uint32_t persistentId) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ValidateOnlineAccount(persistentId);
	}

	AccountOperationResult EmulationController::CreateAccount(
		std::uint32_t persistentId, std::wstring_view miiName)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->CreateAccount(persistentId, miiName);
	}

	AccountOperationResult EmulationController::UpdateAccount(
		std::uint32_t persistentId, const AccountUpdate& update)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->UpdateAccount(persistentId, update);
	}

	AccountOperationResult EmulationController::DeleteAccount(
		std::uint32_t persistentId)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->DeleteAccount(persistentId);
	}

	std::vector<std::uint32_t> EmulationController::ListSavePersistentIds(
		std::uint64_t titleId) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ListSavePersistentIds(titleId);
	}

	SaveEntryLocation EmulationController::InspectSaveEntry(
		std::uint64_t titleId, std::uint32_t persistentId) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->InspectSaveEntry(titleId, persistentId);
	}

	SaveImportInspection EmulationController::InspectSaveImport(
		const std::filesystem::path& archivePath, std::uint64_t titleId,
		std::uint32_t persistentId) const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->InspectSaveImport(archivePath, titleId, persistentId);
	}

	SaveOperationResult EmulationController::DeleteSave(
		std::uint64_t titleId, std::uint32_t persistentId)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->DeleteSave(titleId, persistentId);
	}

	SaveOperationResult EmulationController::TransferSave(
		std::uint64_t titleId, std::uint32_t sourcePersistentId,
		std::uint32_t targetPersistentId, bool overwrite)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->TransferSave(titleId, sourcePersistentId,
									   targetPersistentId, overwrite);
	}

	SaveOperationResult EmulationController::ImportSave(
		const std::filesystem::path& archivePath, std::uint64_t titleId,
		std::uint32_t persistentId, bool overwrite,
		SaveProgressHandler progress, SaveCancellationCheck cancelled)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ImportSave(archivePath, titleId, persistentId, overwrite,
									 std::move(progress), std::move(cancelled));
	}

	SaveOperationResult EmulationController::ExportSave(
		std::uint64_t titleId, std::uint32_t persistentId,
		const std::filesystem::path& archivePath, bool overwrite,
		SaveProgressHandler progress, SaveCancellationCheck cancelled)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ExportSave(titleId, persistentId, archivePath, overwrite,
									 std::move(progress), std::move(cancelled));
	}
} // namespace Application
