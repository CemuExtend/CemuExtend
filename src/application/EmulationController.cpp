#include "application/EmulationController.h"

#include <exception>
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

	LaunchResult EmulationController::Launch(const LaunchRequest& request,
		BeforeStart beforeStart, StartFailure startFailure)
	{
		std::scoped_lock operationLock(m_operationMutex);
		{
			std::scoped_lock lock(m_mutex);
			if (m_state != EmulationState::Idle)
				return {LaunchError::InvalidState, request.path, {}, {},
					"emulation is already preparing, running, or stopping"};
			m_state = EmulationState::Preparing;
		}

		LaunchResult result;
		try
		{
			result = m_backend->Prepare(request);
		}
		catch (const std::exception& ex)
		{
			bool aborted{};
			try { aborted = m_backend->AbortPrepared(); } catch (...) {}
			std::scoped_lock lock(m_mutex);
			m_state = aborted ? EmulationState::Idle : EmulationState::Running;
			return {LaunchError::BackendFailure, request.path, {}, {}, ex.what()};
		}
		catch (...)
		{
			bool aborted{};
			try { aborted = m_backend->AbortPrepared(); } catch (...) {}
			std::scoped_lock lock(m_mutex);
			m_state = aborted ? EmulationState::Idle : EmulationState::Running;
			return {LaunchError::BackendFailure, request.path, {}, {},
				"unknown backend exception while preparing emulation"};
		}
		if (!result)
		{
			bool aborted{};
			try { aborted = m_backend->AbortPrepared(); } catch (...) {}
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
		}
		catch (const std::exception& ex)
		{
			bool aborted{};
			try { aborted = m_backend->AbortPrepared(); } catch (...) {}
			if (aborted && startFailure)
				try { startFailure(); } catch (...) {}
			std::scoped_lock lock(m_mutex);
			m_state = aborted ? EmulationState::Idle : EmulationState::Running;
			result.error = LaunchError::BackendFailure;
			result.diagnostic = ex.what();
			return result;
		}
		catch (...)
		{
			bool aborted{};
			try { aborted = m_backend->AbortPrepared(); } catch (...) {}
			if (aborted && startFailure)
				try { startFailure(); } catch (...) {}
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
		}
		catch (const std::exception& ex)
		{
			std::scoped_lock lock(m_mutex);
			m_state = EmulationState::Running;
			return {false, ex.what()};
		}
		catch (...)
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
		}
		catch (const std::exception& ex)
		{
			std::scoped_lock lock(m_mutex);
			m_state = EmulationState::Idle;
			return {false, ex.what()};
		}
		catch (...)
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

	std::optional<std::uint64_t> EmulationController::RunningTitleId() const
	{
		return m_backend->RunningTitleId();
	}

	std::optional<std::int32_t> EmulationController::ForegroundProcessExitStatus() const
	{
		return m_backend->ForegroundProcessExitStatus();
	}

	void EmulationController::SubmitKeyboard(std::uint16_t usage, bool pressed,
		std::uint8_t modifiers)
	{
		m_backend->SubmitKeyboard(usage, pressed, modifiers);
	}

	void EmulationController::SubmitText(std::uint32_t codepoint, bool repeat)
	{
		m_backend->SubmitText(codepoint, repeat);
	}

	void EmulationController::KeyboardFocusLost()
	{
		m_backend->KeyboardFocusLost();
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

	void EmulationController::SetTextInputWakeCallback(void (*callback)())
	{
		m_backend->SetTextInputWakeCallback(callback);
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

	std::vector<TitleSummary> EmulationController::ListTitles() const
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ListTitles();
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
		std::span<const std::uint64_t> locationUids,
		const std::filesystem::path& outputPath, ContentProgressHandler progress,
		ContentCancellationCheck cancelled)
	{
		std::scoped_lock operationLock(m_operationMutex);
		return m_backend->ConvertToWua(locationUids, outputPath,
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

	void EmulationController::SaveGraphicPackState()
	{
		std::scoped_lock operationLock(m_operationMutex);
		m_backend->SaveGraphicPackState();
	}
}
