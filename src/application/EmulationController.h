#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <vector>

#include "application/ApplicationEvents.h"
#include "application/ContentOperations.h"
#include "application/EmulationPresentation.h"
#include "application/GameProfileFacade.h"
#include "application/GraphicPackFacade.h"
#include "application/TitleCatalog.h"
#include "application/TitleInstallFacade.h"

namespace Application
{
	enum class EmulationState : std::uint8_t
	{
		Idle,
		Preparing,
		Running,
		Stopping,
	};

	enum class LaunchError : std::uint8_t
	{
		None,
		InvalidState,
		BaseTitleMissing,
		InvalidExecutable,
		UnableToMount,
		CemodRuntimeBusy,
		PermissionRequired,
		PermissionDenied,
		MissingDiscKey,
		MissingTitleTicket,
		InvalidTitle,
		BackendFailure,
	};

	enum class CemodExecutionMode : std::uint8_t
	{
		Isolated,
		TrustedNative,
	};

	struct CemodPermissionRequest
	{
		std::string modId;
		std::string principal;
		std::uint32_t requestedPermissions{};
		std::uint32_t grantedPermissions{};
		CemodExecutionMode executionMode{CemodExecutionMode::Isolated};
		bool signedPackage{};
	};

	struct CemodPermissionDecision
	{
		std::string principal;
		std::uint32_t requestedPermissions{};
		std::uint32_t grantedPermissions{};
	};

	struct CemodPackage
	{
		std::filesystem::path path;
		std::string modId;
		std::string principal;
		std::uint32_t requestedPermissions{};
		CemodExecutionMode executionMode{CemodExecutionMode::Isolated};
		bool signedPackage{};
		std::vector<std::uint64_t> titleIds;
		std::string error;
	};

	struct CemodGrant
	{
		std::uint32_t permissions{};
		std::uint32_t approvedRequestMask{};
		bool approved{};
	};

	struct CemuExtendServiceGrantDefaults
	{
		std::uint32_t readMask{};
		std::uint32_t writeMask{};
		std::uint32_t injectMask{};
	};

	struct LaunchRequest
	{
		std::filesystem::path path;
	};

	struct LaunchResult
	{
		LaunchError error{LaunchError::None};
		std::filesystem::path requestedPath;
		std::filesystem::path recentPath;
		std::string titleName;
		std::string diagnostic;
		std::uint64_t titleId{};
		std::vector<CemodPermissionRequest> permissionRequests;

		[[nodiscard]] explicit operator bool() const { return error == LaunchError::None; }
	};

	struct StopResult
	{
		bool stopped{};
		std::string diagnostic;
	};

	enum class PointerSurface : std::uint8_t
	{
		Tv,
		Drc,
	};

	struct PointerPolicy
	{
		std::uint8_t mode{};
		std::uint8_t cursor{};
		std::uint32_t flags{};
	};

	struct TextInputState
	{
		bool active{};
		std::uint64_t sequence{};
		std::uint32_t requestId{};
		std::uint32_t maximumLength{};
		std::int32_t caretX{};
		std::int32_t caretY{};
		std::int32_t lineHeight{};
		std::string initialText;
	};

	struct MouseInput
	{
		PointerSurface surface{};
		std::int32_t x{};
		std::int32_t y{};
		std::int32_t deltaX{};
		std::int32_t deltaY{};
		std::int32_t wheelX{};
		std::int32_t wheelY{};
		std::uint32_t buttons{};
		std::uint32_t changedButtons{};
		std::int32_t contentWidth{};
		std::int32_t contentHeight{};
		bool insideContent{};
		bool focused{};
		std::uint8_t flags{};
	};

	class IEmulationBackend : public ITitleCatalog, public IGraphicPackService,
		public IContentOperations, public IGameProfileService,
		public ITitleInstallService
	{
	public:
		virtual ~IEmulationBackend() = default;
		[[nodiscard]] virtual LaunchResult Prepare(const LaunchRequest& request) = 0;
		virtual void Start() = 0;
		[[nodiscard]] virtual bool AbortPrepared() = 0;
		[[nodiscard]] virtual bool Stop() = 0;
		[[nodiscard]] virtual bool ShutdownApplication() = 0;
		[[nodiscard]] virtual bool IsTitleRunning() const = 0;
		[[nodiscard]] virtual std::optional<std::uint64_t> RunningTitleId() const = 0;
		[[nodiscard]] virtual std::optional<std::int32_t> ForegroundProcessExitStatus() const = 0;
		[[nodiscard]] virtual std::optional<WindowTitlePresentation>
			CurrentWindowTitlePresentation() const = 0;
		virtual void SubmitKeyboard(std::uint16_t usage, bool pressed,
			std::uint8_t modifiers) = 0;
		virtual void SubmitText(std::uint32_t codepoint, bool repeat) = 0;
		virtual void KeyboardFocusLost() = 0;
		virtual void PointerFocusChanged(bool focused) = 0;
		virtual void SubmitMouse(const MouseInput& input) = 0;
		[[nodiscard]] virtual PointerPolicy GetPointerPolicy() = 0;
		[[nodiscard]] virtual TextInputState GetTextInputState() = 0;
		virtual void SubmitTextComposition(std::string_view text,
			std::string_view preedit, std::uint32_t cursor,
			std::uint32_t selectionLength) = 0;
		virtual void SetTextInputWakeCallback(void (*callback)()) = 0;
		virtual void SaveCemodPermissionDecisions(std::uint64_t titleId,
			std::span<const CemodPermissionDecision> decisions) = 0;
		[[nodiscard]] virtual std::vector<CemodPackage> DiscoverCemodCatalog() = 0;
		[[nodiscard]] virtual std::vector<CemodPackage> DiscoverCemods(
			std::uint64_t titleId) = 0;
		[[nodiscard]] virtual CemodGrant ResolveCemodGrant(std::uint64_t titleId,
			std::string_view modId, std::string_view principal,
			std::uint32_t requestedPermissions) = 0;
		[[nodiscard]] virtual CemuExtendServiceGrantDefaults ServiceGrantDefaults() const = 0;
		[[nodiscard]] virtual bool ImportLegacyCemodData(std::uint64_t titleId,
			std::string_view principal, std::string& error) = 0;
	};

	class EmulationController final
	{
	public:
		using BeforeStart = std::function<void(const LaunchResult&)>;
		using StartFailure = std::function<void()>;

		EmulationController();
		explicit EmulationController(IEmulationBackend& backend);
		~EmulationController();

		EmulationController(const EmulationController&) = delete;
		EmulationController& operator=(const EmulationController&) = delete;

		[[nodiscard]] LaunchResult Launch(const LaunchRequest& request,
			BeforeStart beforeStart = {}, StartFailure startFailure = {});
		[[nodiscard]] StopResult Stop();
		[[nodiscard]] StopResult ShutdownApplication();
		[[nodiscard]] EmulationState State() const;
		[[nodiscard]] bool IsTitleRunning() const;
		[[nodiscard]] std::optional<std::uint64_t> RunningTitleId() const;
		[[nodiscard]] std::optional<std::int32_t> ForegroundProcessExitStatus() const;
		[[nodiscard]] std::optional<WindowTitlePresentation>
			CurrentWindowTitlePresentation() const;
		void SubmitKeyboard(std::uint16_t usage, bool pressed, std::uint8_t modifiers);
		void SubmitText(std::uint32_t codepoint, bool repeat);
		void KeyboardFocusLost();
		void PointerFocusChanged(bool focused);
		void SubmitMouse(const MouseInput& input);
		[[nodiscard]] PointerPolicy GetPointerPolicy();
		[[nodiscard]] TextInputState GetTextInputState();
		void SubmitTextComposition(std::string_view text, std::string_view preedit,
			std::uint32_t cursor, std::uint32_t selectionLength);
		void SetTextInputWakeCallback(void (*callback)());
		void SaveCemodPermissionDecisions(std::uint64_t titleId,
			std::span<const CemodPermissionDecision> decisions);
		[[nodiscard]] std::vector<CemodPackage> DiscoverCemodCatalog();
		[[nodiscard]] std::vector<CemodPackage> DiscoverCemods(std::uint64_t titleId);
		[[nodiscard]] CemodGrant ResolveCemodGrant(std::uint64_t titleId,
			std::string_view modId, std::string_view principal,
			std::uint32_t requestedPermissions);
		[[nodiscard]] CemuExtendServiceGrantDefaults ServiceGrantDefaults() const;
		[[nodiscard]] bool ImportLegacyCemodData(std::uint64_t titleId,
			std::string_view principal, std::string& error);
		[[nodiscard]] std::vector<TitleSummary> ListTitles() const;
		[[nodiscard]] std::optional<TitleSummary> ResolveBaseTitle(
			std::uint64_t titleId) const;
		[[nodiscard]] std::vector<GameSummary> ListGames() const;
		[[nodiscard]] std::optional<GameSummary> GetGame(std::uint64_t titleId) const;
		[[nodiscard]] bool IsTitleScanning() const;
		[[nodiscard]] std::optional<std::vector<std::uint8_t>> LoadTitleIcon(
			std::uint64_t titleId) const;
		[[nodiscard]] TitleCatalogSubscription SubscribeTitleCatalog(
			TitleCatalogHandler handler);
		void ReplaceTitleScanPaths(std::span<const std::filesystem::path> paths);
		void RefreshTitles();
		void AddTitleFromPath(const std::filesystem::path& path);
		[[nodiscard]] std::optional<WuaConversionPlan> PlanWuaConversion(
			std::uint64_t titleId, std::uint64_t preferredLocationUid) const;
		[[nodiscard]] ContentOperationResult ConvertToWua(
			std::span<const std::uint64_t> locationUids,
			const std::filesystem::path& outputPath,
			ContentProgressHandler progress,
			ContentCancellationCheck cancelled);
		[[nodiscard]] ContentChecksumResult ComputeTitleChecksum(
			std::uint64_t locationUid, ContentProgressHandler progress,
			ContentCancellationCheck cancelled);
		[[nodiscard]] GameProfileView LoadGameProfile(std::uint64_t titleId) const;
		[[nodiscard]] GameProfileSaveResult SaveGameProfile(
			std::uint64_t titleId, const GameProfileUpdate& update);
		[[nodiscard]] TitleInstallPlanResult PlanTitleInstall(
			const std::filesystem::path& sourcePath) const;
		[[nodiscard]] TitleInstallResult InstallTitle(const TitleInstallPlan& plan,
			TitleInstallDecision decision, TitleInstallProgressHandler progress,
			TitleInstallCancellationCheck cancelled);
		[[nodiscard]] std::vector<GraphicPackInfo> ListGraphicPacks() const;
		[[nodiscard]] GraphicPackResult SetGraphicPackEnabled(
			std::string_view key, bool enabled);
		[[nodiscard]] GraphicPackResult SetGraphicPackPreset(
			std::string_view key, std::string_view category, std::string_view preset);
		[[nodiscard]] GraphicPackResult ReloadGraphicPack(std::string_view key);
		[[nodiscard]] GraphicPackRefreshResult RefreshGraphicPacks();
		void SaveGraphicPackState();
		[[nodiscard]] ApplicationEvents& Events() { return m_events; }
		[[nodiscard]] const ApplicationEvents& Events() const { return m_events; }

	private:
		mutable std::recursive_mutex m_operationMutex;
		mutable std::mutex m_mutex;
		ApplicationEvents m_events;
		std::unique_ptr<IEmulationBackend> m_ownedBackend;
		IEmulationBackend* m_backend{};
		EmulationState m_state{EmulationState::Idle};
	};
}
