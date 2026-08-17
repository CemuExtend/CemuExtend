#include "Common/precompiled.h"

#include "application/EmulationController.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/OS/libs/cemuextend/Cex2Host.h"
#include "Cafe/OS/libs/cemuextend/cemuextend.h"
#include "Cafe/OS/libs/cemuextend/BridgeHost.h"
#include "config/CemuConfig.h"
#include "input/InputManager.h"
#include "Cemu/Tools/DownloadManager/DownloadManager.h"

#include <shared_mutex>

namespace Application
{
	namespace
	{
		struct DownloadRefreshForwarder
		{
			std::mutex mutex;
			ApplicationEvents* events{};
		};

		Event TranslateCafeEvent(const CafeSystem::Event& event)
		{
			Event translated;
			translated.processStatus = event.processStatus;
			translated.framesPerSecond = event.framesPerSecond;
			translated.diagnostic = event.diagnostic;
			translated.type = static_cast<EventType>(event.type);
			translated.diagnosticCode = static_cast<DiagnosticCode>(event.diagnosticCode);
			return translated;
		}

		std::vector<CemodPermissionRequest> GetPermissionRequests(TitleId titleId)
		{
			std::vector<CemodPermissionRequest> result;
			for (auto& request : cemuextend_hle::PendingCemodPermissionRequests(titleId))
			{
				result.push_back({
					.modId = std::move(request.modId),
					.principal = std::move(request.principal),
					.requestedPermissions = request.requestedPermissions,
					.grantedPermissions = request.grantedPermissions,
					.executionMode = request.executionMode == ::CemodExecutionMode::TrustedNative ?
						CemodExecutionMode::TrustedNative : CemodExecutionMode::Isolated,
					.signedPackage = request.signedPackage,
				});
			}
			return result;
		}

		CemodPackage TranslatePackage(CemodPackageInfo package)
		{
			return {
				.path = std::move(package.path),
				.modId = std::move(package.modId),
				.principal = std::move(package.principal),
				.requestedPermissions = package.requestedPermissions,
				.executionMode = package.executionMode == ::CemodExecutionMode::TrustedNative ?
					CemodExecutionMode::TrustedNative : CemodExecutionMode::Isolated,
				.signedPackage = package.signedPackage,
				.titleIds = std::move(package.titleIds),
				.error = std::move(package.error),
			};
		}

		GraphicPackPtr FindGraphicPack(std::string_view key)
		{
			const auto& packs = GraphicPack2::GetGraphicPacks();
			const auto found = std::ranges::find_if(packs, [key](const auto& pack) {
				return pack->GetNormalizedPathString() == key;
			});
			return found == packs.end() ? nullptr : *found;
		}

		GraphicPackInfo TranslateGraphicPack(const GraphicPackPtr& pack)
		{
			GraphicPackInfo result{
				.key = pack->GetNormalizedPathString(),
				.virtualPath = pack->GetVirtualPath(),
				.name = pack->GetName(),
				.description = pack->GetDescription(),
				.version = pack->GetVersion(),
				.universal = pack->IsUniversal(),
				.enabled = pack->IsEnabled(),
				.activated = pack->IsActivated(),
				.defaultEnabled = pack->IsDefaultEnabled(),
				.hasShaders = pack->HasShaders(),
				.hasPatches = pack->HasPatches(),
				.hasCustomVsync = pack->HasCustomVSyncFrequency(),
				.supportedVersion = pack->GetVersion() >= 3 &&
					pack->GetVersion() <= GraphicPack2::GFXPACK_VERSION_8,
				.titleIds = pack->GetTitleIds(),
			};
			auto categorized = pack->GetCategorizedPresets(result.presetOrder);
			for (const auto& category : result.presetOrder)
			{
				const auto found = categorized.find(category);
				if (found == categorized.end())
					continue;
				for (const auto& preset : found->second)
					result.presets.push_back({preset->category, preset->name,
						preset->active, preset->visible});
			}
			return result;
		}

		void DeleteGraphicPackShaders(const GraphicPackPtr& pack)
		{
			for (const auto& shader : pack->GetCustomShaders())
			{
				std::optional<LatteConst::ShaderType> shaderType;
				switch (shader.type)
				{
				case GraphicPack2::GP_SHADER_TYPE::VERTEX:
					shaderType = LatteConst::ShaderType::Vertex;
					break;
				case GraphicPack2::GP_SHADER_TYPE::GEOMETRY:
					shaderType = LatteConst::ShaderType::Geometry;
					break;
				case GraphicPack2::GP_SHADER_TYPE::PIXEL:
					shaderType = LatteConst::ShaderType::Pixel;
					break;
				}
				if (shaderType)
					LatteAsyncCommands_queueDeleteShader(shader.shader_base_hash,
						shader.shader_aux_hash, *shaderType);
			}
		}

		bool ReloadGraphicPackInternal(const GraphicPackPtr& pack)
		{
			if (!pack->HasShaders() && !pack->HasPatches() &&
				!pack->HasCustomVSyncFrequency())
				return false;
			if (!pack->Reload())
				return false;
			DeleteGraphicPackShaders(pack);
			return true;
		}

		LaunchError MapPrepareStatus(CafeSystem::PREPARE_STATUS_CODE status)
		{
			switch (status)
			{
			case CafeSystem::PREPARE_STATUS_CODE::SUCCESS: return LaunchError::None;
			case CafeSystem::PREPARE_STATUS_CODE::CANCELLED: return LaunchError::PermissionDenied;
			case CafeSystem::PREPARE_STATUS_CODE::INVALID_RPX: return LaunchError::InvalidExecutable;
			case CafeSystem::PREPARE_STATUS_CODE::UNABLE_TO_MOUNT: return LaunchError::UnableToMount;
			case CafeSystem::PREPARE_STATUS_CODE::CEMOD_RUNTIME_BUSY: return LaunchError::CemodRuntimeBusy;
			}
			return LaunchError::InvalidExecutable;
		}

		class CafeEmulationBackend final : public IEmulationBackend,
			public CafeSystem::IEventSink,
			public Input::IEmulationInputContext
		{
		public:
			explicit CafeEmulationBackend(ApplicationEvents& events) : m_events(events)
			{
				CafeSystem::SetEventSink(this);
				InputManager::instance().ConfigureEmulationContext(*this);
				m_downloadRefreshForwarder = std::make_shared<DownloadRefreshForwarder>();
				m_downloadRefreshForwarder->events = &m_events;
				DownloadManager::SetGameListRefreshCallback([forwarder = m_downloadRefreshForwarder] {
					std::scoped_lock lock(forwarder->mutex);
					if (forwarder->events)
						forwarder->events->Publish({.type = EventType::GameListRefreshRequested});
				});
			}

			~CafeEmulationBackend() override
			{
				DownloadManager::SetGameListRefreshCallback({});
				{
					std::scoped_lock lock(m_downloadRefreshForwarder->mutex);
					m_downloadRefreshForwarder->events = nullptr;
				}
				InputManager::instance().ClearEmulationContext(*this);
				CafeSystem::SetEventSink(nullptr);
			}

			bool IsTitleRunning() const override
			{
				std::shared_lock lock(m_inputLifecycleMutex);
				return m_inputAvailable && CafeSystem::IsTitleRunning();
			}

			std::optional<std::uint64_t> RunningTitleId() const override
			{
				std::shared_lock lock(m_inputLifecycleMutex);
				if (!m_inputAvailable || !CafeSystem::IsTitleRunning())
					return std::nullopt;
				return CafeSystem::GetForegroundTitleId();
			}

			std::optional<std::int32_t> ForegroundProcessExitStatus() const override
			{
				std::shared_lock lock(m_inputLifecycleMutex);
				return CafeSystem::GetForegroundTitleReturnStatus();
			}

			Input::ScreenImageArea GetScreenImageArea(bool padView) const override
			{
				std::shared_lock lock(m_inputLifecycleMutex);
				if (!m_inputAvailable)
					return {};
				Input::ScreenImageArea area;
				LatteRenderTarget_getScreenImageArea(&area.x, &area.y, &area.width, &area.height,
					nullptr, nullptr, padView);
				return area;
			}

			void OnCafeEvent(const CafeSystem::Event& event) override
			{
				m_events.Publish(TranslateCafeEvent(event));
			}

			LaunchResult Prepare(const LaunchRequest& request) override
			{
				LaunchResult result;
				result.requestedPath = request.path;
				TitleInfo title{request.path};
				CafeSystem::PREPARE_STATUS_CODE status{};
				if (title.IsValid())
				{
					CafeTitleList::AddTitleFromPath(request.path);
					TitleId baseTitleId;
					if (!CafeTitleList::FindBaseTitleId(title.GetAppTitleId(), baseTitleId))
					{
						result.error = LaunchError::BaseTitleMissing;
						result.diagnostic = "base title files were not found";
						return result;
					}
					result.titleId = baseTitleId;
					result.permissionRequests = GetPermissionRequests(baseTitleId);
					if (!result.permissionRequests.empty())
					{
						TitleInfo baseTitle;
						if (CafeTitleList::GetFirstByTitleId(baseTitleId, baseTitle))
							result.titleName = baseTitle.GetMetaTitleName();
						result.error = LaunchError::PermissionRequired;
						result.recentPath = title.GetPath();
						return result;
					}
					status = CafeSystem::PrepareForegroundTitle(baseTitleId);
					result.recentPath = title.GetPath();
				}
				else
				{
					const auto fileType = DetermineCafeSystemFileType(request.path);
					if (fileType != CafeTitleFileType::RPX && fileType != CafeTitleFileType::ELF)
					{
						switch (title.GetInvalidReason())
						{
						case TitleInfo::InvalidReason::NO_DISC_KEY:
							result.error = LaunchError::MissingDiscKey;
							break;
						case TitleInfo::InvalidReason::NO_TITLE_TIK:
							result.error = LaunchError::MissingTitleTicket;
							break;
						default:
							result.error = LaunchError::InvalidTitle;
							break;
						}
						result.diagnostic = "path is not a valid title or standalone executable";
						return result;
					}
					const auto standaloneTitleId = CafeSystem::GetStandaloneTitleId(request.path);
					if (!standaloneTitleId)
					{
						result.error = LaunchError::InvalidExecutable;
						result.diagnostic = "standalone executable could not be hashed";
						return result;
					}
					result.titleId = *standaloneTitleId;
					result.permissionRequests = GetPermissionRequests(*standaloneTitleId);
					if (!result.permissionRequests.empty())
					{
						result.error = LaunchError::PermissionRequired;
						result.recentPath = request.path;
						return result;
					}
					status = CafeSystem::PrepareForegroundTitleFromStandaloneRPX(request.path);
					result.recentPath = request.path;
				}

				result.error = MapPrepareStatus(status);
				if (result.error != LaunchError::None)
				{
					result.diagnostic = "Cafe title preparation failed";
					return result;
				}
				result.titleName = CafeSystem::GetForegroundTitleName();
				return result;
			}

			void Start() override
			{
				std::unique_lock lock(m_inputLifecycleMutex);
				CafeSystem::LaunchForegroundTitle();
				m_inputAvailable = true;
			}
			bool AbortPrepared() override
			{
				std::unique_lock lock(m_inputLifecycleMutex);
				m_inputAvailable = false;
				if (CafeSystem::IsTitleRunning())
					return CafeSystem::ShutdownTitle();
				else
					CafeSystem::AbortPreparedTitle();
				return true;
			}
			bool Stop() override
			{
				std::unique_lock lock(m_inputLifecycleMutex);
				m_inputAvailable = false;
				return CafeSystem::ShutdownTitle();
			}
			bool ShutdownApplication() override
			{
				std::unique_lock lock(m_inputLifecycleMutex);
				m_inputAvailable = false;
				return CafeSystem::Shutdown();
			}

			void SubmitKeyboard(std::uint16_t usage, bool pressed,
				std::uint8_t modifiers) override
			{
				cemuextend_hle::Cex2Host::Instance().KeyboardEvent(usage, pressed, modifiers);
			}

			void SubmitText(std::uint32_t codepoint, bool repeat) override
			{
				cemuextend_hle::Cex2Host::Instance().TextEvent(codepoint, repeat);
			}

			void KeyboardFocusLost() override
			{
				cemuextend_hle::Cex2Host::Instance().KeyboardFocusLost();
			}

			void PointerFocusChanged(bool focused) override
			{
				cemuextend_hle::Cex2Host::Instance().PointerFocusChanged(focused);
			}

			void SubmitMouse(const MouseInput& input) override
			{
				cemuextend_hle::Cex2Host::Instance().MouseEvent(
					static_cast<cemuextend::wire::PointerSurface>(input.surface),
					input.x, input.y, input.deltaX, input.deltaY, input.wheelX, input.wheelY,
					input.buttons, input.changedButtons, input.contentWidth, input.contentHeight,
					input.insideContent, input.focused, input.flags);
			}

			PointerPolicy GetPointerPolicy() override
			{
				const auto policy = cemuextend_hle::Cex2Host::Instance().EffectivePointerPolicy();
				return {policy.mode, policy.cursor, policy.flags.get()};
			}

			TextInputState GetTextInputState() override
			{
				const auto state = cemuextend_hle::Cex2Host::Instance().EffectiveTextInput();
				return {
					.active = state.active,
					.sequence = state.sequence,
					.requestId = state.requestId,
					.maximumLength = state.maximumLength,
					.caretX = state.caretX,
					.caretY = state.caretY,
					.lineHeight = state.lineHeight,
					.initialText = state.initialText,
				};
			}

			void SubmitTextComposition(std::string_view text, std::string_view preedit,
				std::uint32_t cursor, std::uint32_t selectionLength) override
			{
				cemuextend_hle::Cex2Host::Instance().TextCompositionEvent(
					text, preedit, cursor, selectionLength);
			}

			void SetTextInputWakeCallback(void (*callback)()) override
			{
				cemuextend_hle::Cex2Host::Instance().SetTextInputWakeCallback(callback);
			}

			void SaveCemodPermissionDecisions(std::uint64_t titleId,
				std::span<const CemodPermissionDecision> decisions) override
			{
				for (const auto& decision : decisions)
					GetConfig().SetCemuExtendModGrant(titleId, decision.principal,
						{decision.grantedPermissions, decision.requestedPermissions, true});
				GetConfigHandle().Save();
			}

			std::vector<CemodPackage> DiscoverCemodCatalog() override
			{
				std::vector<CemodPackage> result;
				for (auto& package : cemuextend_hle::DiscoverCemodCatalog())
					result.push_back(TranslatePackage(std::move(package)));
				return result;
			}

			std::vector<CemodPackage> DiscoverCemods(std::uint64_t titleId) override
			{
				std::vector<CemodPackage> result;
				for (auto& package : cemuextend_hle::DiscoverCemods(titleId))
					result.push_back(TranslatePackage(std::move(package)));
				return result;
			}

			CemodGrant ResolveCemodGrant(std::uint64_t titleId, std::string_view modId,
				std::string_view principal, std::uint32_t requestedPermissions) override
			{
				const auto grant = cemuextend_hle::ResolveCemodGrant(titleId,
					std::string(modId), std::string(principal), requestedPermissions);
				return {grant.permissions, grant.approved_request_mask, grant.approved};
			}

			CemuExtendServiceGrantDefaults ServiceGrantDefaults() const override
			{
				return {cemuextend_hle::kDefaultReadMask,
					cemuextend_hle::kDefaultWriteMask, cemuextend_hle::kDefaultInjectMask};
			}

			bool ImportLegacyCemodData(std::uint64_t titleId,
				std::string_view principal, std::string& error) override
			{
				return cemuextend_hle::ImportLegacyData(titleId, principal, error);
			}

			std::vector<TitleSummary> ListTitles() const override
			{
				std::vector<TitleSummary> result;
				for (const auto titleId : CafeTitleList::GetAllTitleIds())
				{
					TitleInfo title;
					if (!CafeTitleList::GetFirstByTitleId(titleId, title))
						continue;
					result.push_back({titleId, title.GetMetaTitleName(), title.GetPath()});
				}
				return result;
			}

			std::optional<TitleSummary> ResolveBaseTitle(std::uint64_t titleId) const override
			{
				TitleId baseTitleId{};
				TitleInfo title;
				if (!CafeTitleList::FindBaseTitleId(titleId, baseTitleId) ||
					!CafeTitleList::GetFirstByTitleId(baseTitleId, title))
					return std::nullopt;
				return TitleSummary{baseTitleId, title.GetMetaTitleName(), title.GetPath()};
			}

			void ReplaceScanPaths(std::span<const std::filesystem::path> paths) override
			{
				CafeTitleList::ClearScanPaths();
				for (const auto& path : paths)
					CafeTitleList::AddScanPath(path);
			}

			void RefreshTitles() override
			{
				CafeTitleList::Refresh();
			}

			void AddTitleFromPath(const std::filesystem::path& path) override
			{
				CafeTitleList::AddTitleFromPath(path);
			}

			std::vector<GraphicPackInfo> ListGraphicPacks() const override
			{
				std::vector<GraphicPackInfo> result;
				result.reserve(GraphicPack2::GetGraphicPacks().size());
				for (const auto& pack : GraphicPack2::GetGraphicPacks())
					result.push_back(TranslateGraphicPack(pack));
				return result;
			}

			GraphicPackResult SetGraphicPackEnabled(
				std::string_view key, bool enabled) override
			{
				auto pack = FindGraphicPack(key);
				if (!pack)
					return {.error = GraphicPackError::NotFound,
						.diagnostic = "graphic pack no longer exists"};

				GraphicPackResult result;
				result.changed = pack->IsEnabled() != enabled;
				pack->SetEnabled(enabled);
				result.requiresRestart = pack->RequiresRestart(true, false);
				const auto runningTitle = RunningTitleId();
				result.titleRunning = runningTitle && pack->ContainsTitleId(*runningTitle);
				if (result.titleRunning)
				{
					if (enabled)
					{
						result.applied = GraphicPack2::ActivateGraphicPack(pack);
						if (!result.applied)
						{
							result.error = GraphicPackError::BackendFailure;
							result.diagnostic = "graphic pack activation failed";
						}
						if (!result.requiresRestart)
							result.reloaded = ReloadGraphicPackInternal(pack);
					}
					else
					{
						if (!result.requiresRestart)
							DeleteGraphicPackShaders(pack);
						// A disabled pack can legitimately be inactive already. Preserve
						// the old UI behavior: only a failed real deactivation is an error.
						if (pack->IsActivated())
						{
							result.applied = GraphicPack2::DeactivateGraphicPack(pack);
							if (!result.applied)
							{
								result.error = GraphicPackError::BackendFailure;
								result.diagnostic = "graphic pack deactivation failed";
							}
						}
					}
				}
				result.info = TranslateGraphicPack(pack);
				return result;
			}

			GraphicPackResult SetGraphicPackPreset(std::string_view key,
				std::string_view category, std::string_view preset) override
			{
				auto pack = FindGraphicPack(key);
				if (!pack)
					return {.error = GraphicPackError::NotFound,
						.diagnostic = "graphic pack no longer exists"};

				GraphicPackResult result;
				if (!preset.empty())
				{
					std::vector<std::string> categoryOrder;
					const auto categorized = pack->GetCategorizedPresets(categoryOrder);
					const auto categoryIt = categorized.find(std::string(category));
					const bool presetExists = categoryIt != categorized.end() &&
						std::ranges::any_of(categoryIt->second, [preset](const auto& candidate) {
							return candidate->name == preset;
						});
					if (!presetExists)
					{
						result.error = GraphicPackError::InvalidPreset;
						result.diagnostic = "graphic pack preset is unavailable";
						result.info = TranslateGraphicPack(pack);
						return result;
					}
				}
				result.changed = pack->SetActivePreset(category, preset);
				if (!result.changed)
				{
					result.error = GraphicPackError::InvalidPreset;
					result.diagnostic = "graphic pack preset is unavailable";
					result.info = TranslateGraphicPack(pack);
					return result;
				}
				result.requiresRestart = pack->RequiresRestart(false, true);
				if (!result.requiresRestart)
					result.reloaded = ReloadGraphicPackInternal(pack);
				result.info = TranslateGraphicPack(pack);
				return result;
			}

			GraphicPackResult ReloadGraphicPack(std::string_view key) override
			{
				auto pack = FindGraphicPack(key);
				if (!pack)
					return {.error = GraphicPackError::NotFound,
						.diagnostic = "graphic pack no longer exists"};
				GraphicPackResult result;
				result.reloaded = ReloadGraphicPackInternal(pack);
				result.info = TranslateGraphicPack(pack);
				return result;
			}

			GraphicPackRefreshResult RefreshGraphicPacks() override
			{
				if (IsTitleRunning())
					return {.error = GraphicPackError::TitleRunning,
						.diagnostic = "graphic packs cannot be refreshed while a title is running"};

				std::map<std::string, std::string> previouslyEnabled;
				for (const auto& pack : GraphicPack2::GetGraphicPacks())
					if (pack->IsEnabled())
						previouslyEnabled.emplace(pack->GetNormalizedPathString(),
							pack->GetVirtualPath());

				GraphicPack2::ClearGraphicPacks();
				GraphicPack2::LoadAll();
				for (const auto& pack : GraphicPack2::GetGraphicPacks())
					previouslyEnabled.erase(pack->GetNormalizedPathString());

				GraphicPackRefreshResult result;
				for (auto& [_, path] : previouslyEnabled)
					result.removedEnabledPaths.push_back(std::move(path));
				return result;
			}

			void SaveGraphicPackState() override
			{
				auto& entries = GetConfigHandle().data().graphic_pack_entries;
				entries.clear();
				for (const auto& pack : GraphicPack2::GetGraphicPacks())
				{
					const auto filename = _utf8ToPath(pack->GetNormalizedPathString());
					if (pack->IsEnabled())
					{
						auto& presets = entries.try_emplace(filename).first->second;
						for (const auto& preset : pack->GetActivePresets())
							presets.try_emplace(preset->category, preset->name);
					}
					else if (pack->IsDefaultEnabled())
					{
						auto& presets = entries.try_emplace(filename).first->second;
						presets.try_emplace("_disabled", "false");
					}
				}
				GetConfigHandle().Save();
			}

		private:
			ApplicationEvents& m_events;
			std::shared_ptr<DownloadRefreshForwarder> m_downloadRefreshForwarder;
			mutable std::shared_mutex m_inputLifecycleMutex;
			bool m_inputAvailable{};
		};
	}

	std::unique_ptr<IEmulationBackend> CreateCafeEmulationBackend(ApplicationEvents& events)
	{
		return std::make_unique<CafeEmulationBackend>(events);
	}
}
