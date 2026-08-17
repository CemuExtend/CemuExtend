#include "Common/precompiled.h"

#include "application/EmulationController.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/Filesystem/WUD/wud.h"
#include "Cafe/IOSU/PDM/iosu_pdm.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/SaveList.h"
#include "Cafe/OS/libs/cemuextend/Cex2Host.h"
#include "Cafe/OS/libs/cemuextend/cemuextend.h"
#include "Cafe/OS/libs/cemuextend/BridgeHost.h"
#include "Cafe/OS/libs/nfc/nfc.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "input/InputManager.h"
#include "Cemu/Tools/DownloadManager/DownloadManager.h"
#include "Common/FileStream.h"
#include "util/helpers/helpers.h"

#include <zarchive/zarchivereader.h>
#include <zarchive/zarchivewriter.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <condition_variable>
#include <deque>
#include <fstream>
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

		std::optional<GameSummary> TranslateGame(std::uint64_t titleId)
		{
			auto game = CafeTitleList::GetGameInfo(titleId);
			if (!game.IsValid())
				return std::nullopt;

			auto& base = game.GetBase();
			GameSummary result{
				.titleId = game.GetBaseTitleId(),
				.name = game.GetTitleName(),
				.basePath = base.GetPath(),
				.savePath = game.GetSaveFolder(),
				.version = game.GetVersion(),
				.aocVersion = game.GetAOCVersion(),
				.region = static_cast<std::uint32_t>(game.GetRegion()),
				.regionName = fmt::format("{}", game.GetRegion()),
				.systemData = game.IsSystemDataTitle(),
			};
			if (game.HasUpdate())
				result.updatePath = game.GetUpdate().GetPath();
			if (game.HasAOC())
				result.aocPath = game.GetAOC().front().GetPath();

			if (base.ParseXmlInfo())
			{
				if (const auto* meta = base.GetMetaInfo())
				{
					result.productCode = meta->GetProductCode();
					result.companyCode = meta->GetCompanyCode();
				}
			}

			iosu::pdm::GameListStat stats{};
			result.playStats.available = iosu::pdm::GetStatForGamelist(result.titleId, stats);
			if (result.playStats.available)
			{
				result.playStats.minutesPlayed = stats.numMinutesPlayed;
				result.playStats.lastPlayedYear = stats.last_played.year;
				result.playStats.lastPlayedMonth = stats.last_played.month;
				result.playStats.lastPlayedDay = stats.last_played.day;
			}
			return result;
		}

		std::uint64_t ToBaseTitleId(std::uint64_t titleId)
		{
			titleId = TitleIdParser::MakeBaseTitleId(titleId);
			if (((titleId >> 32) & 0xff) == 0x0c)
				titleId &= ~0xff00000000ULL;
			return titleId;
		}

		ManagedContentType TranslateManagedType(TitleInfo& title)
		{
			switch (title.GetTitleType())
			{
			case TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE:
				return ManagedContentType::Update;
			case TitleIdParser::TITLE_TYPE::AOC:
				return ManagedContentType::Dlc;
			case TitleIdParser::TITLE_TYPE::SYSTEM_DATA:
			case TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE:
			case TitleIdParser::TITLE_TYPE::SYSTEM_TITLE:
				return ManagedContentType::System;
			default:
				return ManagedContentType::Base;
			}
		}

		ManagedContentFormat TranslateManagedFormat(const TitleInfo& title)
		{
			switch (title.GetFormat())
			{
			case TitleInfo::TitleDataFormat::WUD:
				return ManagedContentFormat::Wud;
			case TitleInfo::TitleDataFormat::NUS:
				return ManagedContentFormat::Nus;
			case TitleInfo::TitleDataFormat::WIIU_ARCHIVE:
				return ManagedContentFormat::Wua;
			case TitleInfo::TitleDataFormat::WUHB:
				return ManagedContentFormat::Wuhb;
			case TitleInfo::TitleDataFormat::HOST_FS:
			default:
				return ManagedContentFormat::Folder;
			}
		}

		ManagedContentEntry TranslateManagedTitle(TitleInfo& title, bool presentation)
		{
			ManagedContentEntry result{
				.locationUid = title.GetUID(),
				.titleId = title.GetAppTitleId(),
				.path = title.GetPath(),
				.version = title.GetAppTitleVersion(),
				.region = static_cast<std::uint32_t>(title.GetMetaRegion()),
				.regionName = fmt::format("{}", title.GetMetaRegion()),
				.type = TranslateManagedType(title),
				.format = TranslateManagedFormat(title),
			};
			if (presentation)
			{
				result.name = title.GetMetaTitleName();
				const auto newline = result.name.find('\n');
				if (newline != std::string::npos)
					result.name.replace(newline, 1, " - ");
			}
			return result;
		}

		std::optional<ManagedContentEntry> TranslateManagedSave(SaveInfo& save)
		{
			auto* meta = save.GetMetaInfo();
			if (!meta)
				return std::nullopt;
			ManagedContentEntry result{
				.locationUid = std::hash<std::uint64_t>{}(meta->GetTitleId()),
				.titleId = meta->GetTitleId(),
				.path = save.GetPath(),
				.name = meta->GetLongName(GetConfig().console_language.GetValue()),
				.version = meta->GetTitleVersion(),
				.region = static_cast<std::uint32_t>(meta->GetRegion()),
				.regionName = fmt::format("{}", meta->GetRegion()),
				.type = ManagedContentType::Save,
				.format = ManagedContentFormat::Folder,
			};
			const auto newline = result.name.find('\n');
			if (newline != std::string::npos)
				result.name.replace(newline, 1, " - ");
			return result;
		}

		class TitleListLease final
		{
		public:
			TitleListLease() : titles(CafeTitleList::AcquireInternalList()) {}
			~TitleListLease() { CafeTitleList::ReleaseInternalList(); }
			std::span<TitleInfo*> titles;
		};

		std::optional<WuaConversionPlan> BuildWuaConversionPlan(
			std::uint64_t titleId, std::uint64_t preferredLocationUid)
		{
			titleId = TitleIdParser::MakeBaseTitleId(titleId);
			TitleIdParser parser(titleId);
			const bool hasBase = parser.GetType() != TitleIdParser::TITLE_TYPE::AOC;
			const bool hasUpdate = parser.CanHaveSeparateUpdateTitleId();
			const std::uint64_t updateId = hasUpdate ? parser.GetSeparateUpdateTitleId() : 0;
			const std::uint64_t aocId = hasBase ?
				(titleId & ~0xff00000000ULL) | 0x0c00000000ULL : titleId;

			TitleInfo* base{};
			TitleInfo* update{};
			TitleInfo* aoc{};
			TitleListLease lease;
			for (auto* candidate : lease.titles)
			{
				const auto candidateId = candidate->GetAppTitleId();
				const bool preferred = candidate->GetUID() == preferredLocationUid;
				if (hasBase && candidateId == titleId && (!base || preferred))
					base = candidate;
				else if (hasUpdate && candidateId == updateId &&
					(!update || candidate->GetAppTitleVersion() > update->GetAppTitleVersion() || preferred))
					update = candidate;
				else if (candidateId == aocId &&
					(!aoc || candidate->GetAppTitleVersion() > aoc->GetAppTitleVersion() || preferred))
					aoc = candidate;
			}

			WuaConversionPlan plan;
			auto add = [&plan](TitleInfo* title, ContentRole role) {
				if (!title)
					return;
				plan.items.push_back({
					.locationUid = title->GetUID(),
					.titleId = title->GetAppTitleId(),
					.version = title->GetAppTitleVersion(),
					.role = role,
					.displayPath = title->GetPrintPath(),
				});
			};
			add(base, ContentRole::Base);
			add(update, ContentRole::Update);
			add(aoc, ContentRole::Dlc);
			if (plan.items.empty())
				return std::nullopt;

			TitleInfo* namingTitle = base ? base : (update ? update : aoc);
			std::string name = namingTitle->GetMetaTitleName();
			boost::replace_all(name, ":", "");
			boost::replace_all(name, "/", "");
			boost::replace_all(name, "\\", "");
			const auto region = namingTitle->GetMetaRegion();
			if (region == CafeConsoleRegion::JPN)
				name.append(" (JP)");
			else if (region == CafeConsoleRegion::EUR)
				name.append(" (EU)");
			else if (region == CafeConsoleRegion::USA)
				name.append(" (US)");
			if (update)
				name.append(fmt::format(" (v{})", update->GetAppTitleVersion()));
			plan.suggestedFileName = std::move(name) + ".wua";
			return plan;
		}

		class WuaWriterContext final
		{
		public:
			WuaWriterContext(std::filesystem::path outputPath,
				ContentProgressHandler progress, ContentCancellationCheck cancelled)
				: outputPath(std::move(outputPath)), progress(std::move(progress)),
				  cancelled(std::move(cancelled)) {}

			~WuaWriterContext()
			{
				Close();
			}

			void Close()
			{
				delete writer;
				writer = nullptr;
				delete stream;
				stream = nullptr;
			}

			static void NewOutputFile(std::int32_t, void* context)
			{
				auto& self = *static_cast<WuaWriterContext*>(context);
				self.stream = FileStream::createFile2(self.outputPath);
				self.valid = self.stream != nullptr;
			}

			static void WriteOutputData(const void* data, std::size_t length, void* context)
			{
				auto& self = *static_cast<WuaWriterContext*>(context);
				if (!self.stream || length > static_cast<std::size_t>(std::numeric_limits<sint32>::max()) ||
					self.stream->writeData(data, static_cast<sint32>(length)) !=
						static_cast<sint32>(length))
					self.valid = false;
			}

			bool IsCancelled() const { return cancelled && cancelled(); }

			void Publish(ContentOperationPhase phase) const
			{
				if (progress)
					progress({phase, filesCompleted, filesTotal,
						bytesCompleted, bytesTotal});
			}

			bool CountFiles(const std::string& path)
			{
				if (IsCancelled())
					return false;
				sint32 status{};
				std::unique_ptr<FSCVirtualFile> directory(fsc_openDirIterator(path.c_str(), &status));
				if (!directory)
					return false;
				FSCDirEntry entry;
				while (fsc_nextDir(directory.get(), &entry))
				{
					if (entry.isFile)
					{
						bytesTotal += static_cast<std::uint64_t>(entry.fileSize);
						++filesTotal;
						Publish(ContentOperationPhase::Collecting);
					}
					else if (entry.isDirectory &&
						!CountFiles(fmt::format("{}{}/", path, entry.path)))
						return false;
				}
				return true;
			}

			bool AddFiles(std::string archivePath, const std::string& path)
			{
				if (IsCancelled())
					return false;
				sint32 status{};
				std::unique_ptr<FSCVirtualFile> directory(fsc_openDirIterator(path.c_str(), &status));
				if (!directory)
					return false;
				if (!writer->MakeDir(archivePath.c_str(), false))
					return false;
				FSCDirEntry entry;
				while (fsc_nextDir(directory.get(), &entry))
				{
					if (entry.isDirectory)
					{
						if (!AddFiles(fmt::format("{}{}/", archivePath, entry.path),
							fmt::format("{}{}/", path, entry.path)))
							return false;
						continue;
					}
					if (!entry.isFile)
						continue;
					if (!writer->StartNewFile((archivePath + entry.path).c_str()))
						return false;
					std::unique_ptr<FSCVirtualFile> file(fsc_open(
						(path + entry.path).c_str(), FSC_ACCESS_FLAG::OPEN_FILE |
						FSC_ACCESS_FLAG::READ_PERMISSION, &status));
					if (!file)
						return false;
					buffer.resize(32 * 1024);
					for (;;)
					{
						const auto read = file->fscReadData(buffer.data(), buffer.size());
						if (read == 0)
							break;
						writer->AppendData(buffer.data(), read);
						bytesCompleted += read;
						Publish(ContentOperationPhase::Converting);
						if (IsCancelled())
							return false;
					}
					++filesCompleted;
				}
				return true;
			}

			bool Process(TitleInfo& title, bool count)
			{
				const auto mountPath = TitleInfo::GetUniqueTempMountingPath();
				if (!title.Mount(mountPath, "", FSC_PRIORITY_BASE))
					return false;
				try
				{
					const bool result = count ? CountFiles(mountPath) :
						AddFiles(fmt::format("{:016x}_v{}/", title.GetAppTitleId(),
							title.GetAppTitleVersion()), mountPath);
					title.Unmount(mountPath);
					return result;
				}
				catch (...)
				{
					title.Unmount(mountPath);
					throw;
				}
			}

			std::filesystem::path outputPath;
			ContentProgressHandler progress;
			ContentCancellationCheck cancelled;
			FileStream* stream{};
			ZArchiveWriter* writer{};
			std::vector<std::uint8_t> buffer;
			std::uint32_t filesCompleted{};
			std::uint32_t filesTotal{};
			std::uint64_t bytesCompleted{};
			std::uint64_t bytesTotal{};
			bool valid{};
		};

		class CafeTitleEventSubscription final :
			public Detail::TitleSubscriptionState,
			public std::enable_shared_from_this<CafeTitleEventSubscription>
		{
		public:
			explicit CafeTitleEventSubscription(TitleCatalogHandler handler)
				: m_handler(std::move(handler)) {}

			static std::shared_ptr<CafeTitleEventSubscription> Create(
				TitleCatalogHandler handler)
			{
				auto state = std::make_shared<CafeTitleEventSubscription>(std::move(handler));
				try
				{
					state->Connect();
				}
				catch (...)
				{
					state->Stop();
					throw;
				}
				return state;
			}

			~CafeTitleEventSubscription() override
			{
				if (m_saveCallbackId != 0)
					CafeSaveList::UnregisterCallback(m_saveCallbackId);
				if (m_callbackId != 0)
					CafeTitleList::UnregisterCallback(m_callbackId);
				Stop();
				if (m_thread.joinable())
				{
					if (m_thread.get_id() == std::this_thread::get_id())
						m_thread.detach();
					else
						m_thread.join();
				}
			}

			void Stop() override
			{
				{
					std::scoped_lock lock(m_mutex);
					m_stopping = true;
					m_pending.clear();
				}
				m_condition.notify_all();
				if (m_thread.joinable() &&
					m_thread.get_id() != std::this_thread::get_id())
					m_thread.join();
			}

		private:
			void Connect()
			{
				auto self = shared_from_this();
				m_thread = std::thread([self = std::move(self)] { self->Run(); });
				m_callbackId = CafeTitleList::RegisterCallback(
					[](CafeTitleListCallbackEvent* event, void* context) {
						auto& self = *static_cast<CafeTitleEventSubscription*>(context);
						try
						{
							TitleCatalogEvent translated;
							switch (event->eventType)
							{
							case CafeTitleListCallbackEvent::TYPE::TITLE_DISCOVERED:
								translated.type = TitleCatalogEventType::Discovered;
								break;
							case CafeTitleListCallbackEvent::TYPE::TITLE_REMOVED:
								translated.type = TitleCatalogEventType::Removed;
								break;
							case CafeTitleListCallbackEvent::TYPE::SCAN_FINISHED:
								translated.type = TitleCatalogEventType::ScanFinished;
								break;
							default:
								cemuLog_log(LogType::Force,
									"Ignoring unknown title catalog event");
								return;
							}
							if (event->titleInfo)
							{
								translated.titleId = ToBaseTitleId(
									event->titleInfo->GetAppTitleId());
								const bool discovered = event->eventType ==
									CafeTitleListCallbackEvent::TYPE::TITLE_DISCOVERED;
								if (!discovered || (!event->titleInfo->IsCached() &&
									!event->titleInfo->IsSystemDataTitle()))
								{
									translated.managedEntry = TranslateManagedTitle(
										*event->titleInfo, discovered);
								}
							}
							self.Enqueue(std::move(translated));
						}
						catch (const std::exception& exception)
						{
							cemuLog_log(LogType::Force,
								"Unable to queue title catalog event: {}", exception.what());
						}
					}, this);

				m_saveCallbackId = CafeSaveList::RegisterCallback(
					[](CafeSaveListCallbackEvent* event, void* context) {
						auto& self = *static_cast<CafeTitleEventSubscription*>(context);
						try
						{
							TitleCatalogEvent translated;
							switch (event->eventType)
							{
							case CafeSaveListCallbackEvent::TYPE::SAVE_DISCOVERED:
								translated.type = TitleCatalogEventType::SaveDiscovered;
								break;
							case CafeSaveListCallbackEvent::TYPE::SAVE_REMOVED:
								translated.type = TitleCatalogEventType::SaveRemoved;
								break;
							case CafeSaveListCallbackEvent::TYPE::SCAN_FINISHED:
								translated.type = TitleCatalogEventType::SaveScanFinished;
								break;
							default:
								cemuLog_log(LogType::Force,
									"Ignoring unknown save catalog event");
								return;
							}
							if (event->saveInfo)
							{
								translated.titleId = event->saveInfo->GetTitleId();
								translated.managedEntry = TranslateManagedSave(*event->saveInfo);
							}
							self.Enqueue(std::move(translated));
						}
						catch (const std::exception& exception)
						{
							cemuLog_log(LogType::Force,
								"Unable to queue save catalog event: {}", exception.what());
						}
					}, this);
			}

			void Enqueue(TitleCatalogEvent event)
			{
				{
					std::scoped_lock lock(m_mutex);
					if (m_stopping)
						return;
					m_pending.push_back(std::move(event));
				}
				m_condition.notify_one();
			}

			void Run()
			{
				SetThreadName("TitleCatalogEvents");
				for (;;)
				{
					TitleCatalogEvent event;
					{
						std::unique_lock lock(m_mutex);
						m_condition.wait(lock, [this] {
							return m_stopping || !m_pending.empty();
						});
						if (m_stopping)
							return;
						event = std::move(m_pending.front());
						m_pending.pop_front();
					}
					try
					{
						m_handler(event);
					}
					catch (const std::exception& exception)
					{
						cemuLog_log(LogType::Force,
							"Title catalog subscriber failed: {}", exception.what());
					}
				}
			}

			TitleCatalogHandler m_handler;
			std::mutex m_mutex;
			std::condition_variable m_condition;
			std::deque<TitleCatalogEvent> m_pending;
			std::thread m_thread;
			std::uint64_t m_callbackId{};
			std::uint64_t m_saveCallbackId{};
			bool m_stopping{};
		};

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

		TitleInstallKind TranslateInstallKind(TitleIdParser::TITLE_TYPE type)
		{
			switch (type)
			{
			case TitleIdParser::TITLE_TYPE::BASE_TITLE: return TitleInstallKind::Base;
			case TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO: return TitleInstallKind::Demo;
			case TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE: return TitleInstallKind::Update;
			case TitleIdParser::TITLE_TYPE::AOC: return TitleInstallKind::Dlc;
			case TitleIdParser::TITLE_TYPE::SYSTEM_TITLE:
			case TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE:
				return TitleInstallKind::SystemTitle;
			case TitleIdParser::TITLE_TYPE::SYSTEM_DATA: return TitleInstallKind::SystemData;
			default: return TitleInstallKind::Unknown;
			}
		}

		std::uint64_t FingerprintInstallTarget(const fs::path& target)
		{
			std::uint64_t result = 1469598103934665603ULL;
			auto mix = [&result](std::uint64_t value) {
				result ^= value;
				result *= 1099511628211ULL;
			};
			const std::array<fs::path, 4> paths{
				target, target / "meta/meta.xml", target / "code/app.xml",
				target / "code/cos.xml"};
			for (const auto& path : paths)
			{
				std::error_code ec;
				const auto status = fs::symlink_status(path, ec);
				mix(ec ? std::numeric_limits<std::uint64_t>::max() :
					static_cast<std::uint64_t>(status.type()));
				if (!ec && fs::is_regular_file(status))
				{
					mix(fs::file_size(path, ec));
					if (ec)
						mix(std::numeric_limits<std::uint64_t>::max() - 1);
				}
				ec.clear();
				const auto modified = fs::last_write_time(path, ec);
				mix(ec ? std::numeric_limits<std::uint64_t>::max() - 2 :
					static_cast<std::uint64_t>(modified.time_since_epoch().count()));
			}
			return result;
		}

		TitleInstallPlanResult BuildTitleInstallPlan(const fs::path& requestedSource,
			bool checkAvailableSpace = true)
		{
			std::error_code ec;
			auto source = fs::weakly_canonical(requestedSource, ec);
			if (ec || source.empty())
				source = requestedSource;
			TitleInfo title(source);
			if (!title.IsValid())
				return {TitleInstallError::InvalidSource,
					"The selected folder is not a valid installable title", std::nullopt};

			const std::array<const char*, 3> requiredFolders{"content", "code", "meta"};
			std::uint64_t requiredBytes{};
			std::uint64_t sourceFingerprint = 1469598103934665603ULL;
			auto mixSource = [&sourceFingerprint](std::uint64_t value) {
				sourceFingerprint ^= value;
				sourceFingerprint *= 1099511628211ULL;
			};
			for (const auto* folder : requiredFolders)
			{
				const auto root = source / folder;
				const auto rootStatus = fs::symlink_status(root, ec);
				if (ec || fs::is_symlink(rootStatus))
					return {TitleInstallError::InvalidSource,
						fs::is_symlink(rootStatus) ?
							"Symbolic links are not supported in title installs" : ec.message(),
						std::nullopt};
				if (!fs::is_directory(rootStatus))
					return {TitleInstallError::MissingContent,
						fmt::format("Required '{}' folder is missing", folder), std::nullopt};
				fs::recursive_directory_iterator iterator(root, ec), end;
				if (ec)
					return {TitleInstallError::InvalidSource, ec.message(), std::nullopt};
				while (iterator != end)
				{
					if (iterator->is_symlink(ec))
						return {TitleInstallError::InvalidSource,
							"Symbolic links are not supported in title installs", std::nullopt};
					if (ec)
						return {TitleInstallError::InvalidSource, ec.message(), std::nullopt};
					if (iterator->is_regular_file(ec))
					{
						const auto size = iterator->file_size(ec);
						if (ec || size > std::numeric_limits<std::uint64_t>::max() - requiredBytes)
							return {TitleInstallError::InvalidSource,
								"Unable to measure title files", std::nullopt};
						requiredBytes += size;
						mixSource(std::hash<std::string>{}(
							_pathToUtf8(fs::relative(iterator->path(), source, ec))));
						if (ec)
							return {TitleInstallError::InvalidSource, ec.message(), std::nullopt};
						mixSource(size);
						const auto modified = iterator->last_write_time(ec);
						if (ec)
							return {TitleInstallError::InvalidSource, ec.message(), std::nullopt};
						mixSource(static_cast<std::uint64_t>(
							modified.time_since_epoch().count()));
					}
					else if (ec)
						return {TitleInstallError::InvalidSource, ec.message(), std::nullopt};
					else if (!iterator->is_directory(ec) || ec)
						return {TitleInstallError::InvalidSource,
							"Unsupported entry in title install source", std::nullopt};
					iterator.increment(ec);
					if (ec)
						return {TitleInstallError::InvalidSource, ec.message(), std::nullopt};
				}
			}

			TitleInstallPlan plan{
				.sourcePath = source,
				.targetPath = ActiveSettings::GetMlcPath(title.GetInstallPath()),
				.titleId = title.GetAppTitleId(),
				.version = title.GetAppTitleVersion(),
				.titleName = title.GetMetaTitleName(),
				.kind = TranslateInstallKind(title.GetTitleType()),
				.requiredBytes = requiredBytes,
				.sourceFingerprint = sourceFingerprint,
			};
			const auto canonicalTarget = fs::weakly_canonical(plan.targetPath, ec);
			if ((!ec && canonicalTarget == source) ||
				(fs::exists(plan.targetPath, ec) && !ec &&
					fs::equivalent(source, plan.targetPath, ec) && !ec))
				return {TitleInstallError::InvalidSource,
					"A title cannot be installed from its destination folder", std::nullopt};
			ec.clear();

			plan.installed.exists = fs::exists(plan.targetPath, ec) && !ec;
			if (ec)
				return {TitleInstallError::InvalidSource, ec.message(), std::nullopt};
			if (plan.installed.exists)
			{
				plan.installed.fingerprint = FingerprintInstallTarget(plan.targetPath);
				TitleInfo installed(plan.targetPath);
				plan.installed.valid = installed.IsValid();
				if (plan.installed.valid)
				{
					plan.installed.titleId = installed.GetAppTitleId();
					plan.installed.version = installed.GetAppTitleVersion();
					plan.installed.kind = TranslateInstallKind(installed.GetTitleType());
					if (title.GetTitleType() != installed.GetTitleType())
						plan.conflict = TitleInstallConflict::DifferentType;
					else if (plan.version == plan.installed.version)
						plan.conflict = TitleInstallConflict::SameVersion;
					else if (plan.installed.version > plan.version)
						plan.conflict = TitleInstallConflict::NewerVersionInstalled;
				}
			}

			const auto space = fs::space(ActiveSettings::GetMlcPath(), ec);
			if (ec)
				return {TitleInstallError::InvalidSource, ec.message(), std::nullopt};
			plan.availableBytes = space.available;
			if (checkAvailableSpace && plan.availableBytes <= plan.requiredBytes)
				return {TitleInstallError::NotEnoughSpace,
					fmt::format("Not enough space available. Required: {} MB, available: {} MB",
						plan.requiredBytes / 1024 / 1024,
						plan.availableBytes / 1024 / 1024), std::nullopt};
			return {TitleInstallError::None, {}, std::move(plan)};
		}

		TitleInstallPlanResult SafeBuildTitleInstallPlan(const fs::path& source,
			bool checkAvailableSpace = true)
		{
			try
			{
				return BuildTitleInstallPlan(source, checkAvailableSpace);
			}
			catch (const std::exception& exception)
			{
				return {TitleInstallError::InvalidSource, exception.what(), std::nullopt};
			}
			catch (...)
			{
				return {TitleInstallError::InvalidSource,
					"Unknown title install planning failure", std::nullopt};
			}
		}

		bool SameInstallSnapshot(const TitleInstallPlan& left, const TitleInstallPlan& right)
		{
			return left.sourcePath == right.sourcePath && left.targetPath == right.targetPath &&
				left.titleId == right.titleId && left.version == right.version &&
				left.kind == right.kind && left.requiredBytes == right.requiredBytes &&
				left.sourceFingerprint == right.sourceFingerprint &&
				left.conflict == right.conflict &&
				left.installed.exists == right.installed.exists &&
				left.installed.valid == right.installed.valid &&
				left.installed.titleId == right.installed.titleId &&
				left.installed.version == right.installed.version &&
				left.installed.kind == right.installed.kind &&
				left.installed.fingerprint == right.installed.fingerprint;
		}

		fs::path UniqueInstallSibling(const fs::path& target, std::string_view suffix)
		{
			static std::atomic_uint64_t sequence{};
			const auto seed = static_cast<std::uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			for (std::uint64_t attempt = 0; attempt < 1024; ++attempt)
			{
				auto candidate = target;
				candidate += _utf8ToPath(fmt::format(".{}.{}.{}", suffix, seed,
					sequence.fetch_add(1, std::memory_order_relaxed)));
				std::error_code ec;
				if (!fs::exists(candidate, ec) && !ec)
					return candidate;
			}
			return {};
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

			std::optional<WindowTitlePresentation>
			CurrentWindowTitlePresentation() const override
			{
				std::shared_lock lock(m_inputLifecycleMutex);
				if (!m_inputAvailable || !CafeSystem::IsTitleRunning())
					return std::nullopt;

				WindowTitlePresentation presentation{
					.titleId = CafeSystem::GetForegroundTitleId(),
					.titleName = CafeSystem::GetForegroundTitleName(),
					.version = CafeSystem::GetForegroundTitleVersion(),
				};
				switch (CafeSystem::GetForegroundTitleRegion())
				{
				case CafeConsoleRegion::JPN: presentation.region = TitleRegion::Japan; break;
				case CafeConsoleRegion::USA:
					presentation.region = TitleRegion::UnitedStates;
					break;
				case CafeConsoleRegion::EUR: presentation.region = TitleRegion::Europe; break;
				default: break;
				}

				if (g_renderer)
				{
					switch (g_renderer->GetType())
					{
					case RendererAPI::OpenGL:
						presentation.renderer = PresentationRenderer::OpenGL;
						break;
					case RendererAPI::Vulkan:
						presentation.renderer = PresentationRenderer::Vulkan;
						break;
					case RendererAPI::Metal:
						presentation.renderer = PresentationRenderer::Metal;
						break;
					default: break;
					}
				}

				switch (LatteGPUState.glVendor)
				{
				case GLVENDOR_AMD: presentation.gpuVendor = PresentationGpuVendor::Amd; break;
				case GLVENDOR_INTEL:
					presentation.gpuVendor = PresentationGpuVendor::Intel;
					break;
				case GLVENDOR_NVIDIA:
					presentation.gpuVendor = PresentationGpuVendor::Nvidia;
					break;
				case GLVENDOR_APPLE:
					presentation.gpuVendor = PresentationGpuVendor::Apple;
					break;
				default: break;
				}
				return presentation;
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

			bool SoftwareKeyboardActive() const override
			{
				return swkbd_hasKeyboardInputHook();
			}

			bool SubmitSoftwareKeyboardKey(std::uint32_t keyCode) override
			{
				if (!swkbd_hasKeyboardInputHook())
					return false;
				swkbd_keyInput(keyCode);
				return true;
			}

			NfcTouchResult TouchNfcTagFromFile(
				const std::filesystem::path& path) override
			{
				std::unique_lock lock(m_inputLifecycleMutex);
				if (!m_inputAvailable || !CafeSystem::IsTitleRunning())
					return NfcTouchResult::Inactive;

				uint32 error{};
				if (nfc::TouchTagFromFile(path, &error))
					return NfcTouchResult::Success;
				switch (error)
				{
				case NFC_TOUCH_TAG_ERROR_NO_ACCESS: return NfcTouchResult::NoAccess;
				case NFC_TOUCH_TAG_ERROR_INVALID_FILE_FORMAT:
					return NfcTouchResult::InvalidFileFormat;
				default: return NfcTouchResult::UnknownError;
				}
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

			std::vector<GameSummary> ListGames() const override
			{
				std::vector<GameSummary> result;
				std::set<std::uint64_t> seen;
				for (const auto titleId : CafeTitleList::GetAllTitleIds())
				{
					auto game = TranslateGame(titleId);
					if (game && seen.emplace(game->titleId).second)
						result.push_back(std::move(*game));
				}
				return result;
			}

			std::optional<GameSummary> GetGame(std::uint64_t titleId) const override
			{
				return TranslateGame(titleId);
			}

			bool IsTitleScanning() const override
			{
				return CafeTitleList::IsScanning();
			}

			std::optional<std::vector<std::uint8_t>> LoadTitleIcon(
				std::uint64_t titleId) const override
			{
				TitleInfo title;
				if (!CafeTitleList::GetFirstByTitleId(titleId, title))
					return std::nullopt;
				const auto mountPath = TitleInfo::GetUniqueTempMountingPath();
				if (!title.Mount(mountPath, "", FSC_PRIORITY_BASE))
					return std::nullopt;
				auto data = fsc_extractFile((mountPath + "/meta/iconTex.tga").c_str());
				if (!data)
				{
					data = fsc_extractFile((mountPath + "/meta/iconTex.tga.gz").c_str());
					if (data)
						data = zlibDecompress(*data, 70 * 1024);
				}
				title.Unmount(mountPath);
				return data;
			}

			TitleCatalogSubscription SubscribeTitleCatalogEvents(
				TitleCatalogHandler handler) override
			{
				return TitleCatalogSubscription{
					CafeTitleEventSubscription::Create(std::move(handler))};
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

			std::optional<WuaConversionPlan> PlanWuaConversion(
				std::uint64_t titleId, std::uint64_t preferredLocationUid) const override
			{
				return BuildWuaConversionPlan(titleId, preferredLocationUid);
			}

			ContentOperationResult ConvertToWua(
				std::span<const std::uint64_t> locationUids,
				const std::filesystem::path& outputPath,
				ContentProgressHandler progress,
				ContentCancellationCheck cancelled) override
			{
				if (locationUids.empty())
					return {ContentOperationError::NotFound, "No title content selected"};
				if (outputPath.empty())
					return {ContentOperationError::UnableToCreateOutput,
						"Archive output path is empty"};

				std::vector<TitleInfo> titles;
				titles.reserve(locationUids.size());
				for (const auto uid : locationUids)
				{
					auto title = CafeTitleList::GetTitleInfoByUID(uid);
					if (!title.IsValid())
						return {ContentOperationError::NotFound,
							fmt::format("Title content {:016x} is no longer available", uid)};
					titles.push_back(std::move(title));
				}

				auto temporaryPath = outputPath;
				temporaryPath += fmt::format(".tmp.{}",
					std::chrono::steady_clock::now().time_since_epoch().count());
				auto removeTemporary = [&temporaryPath] {
					std::error_code ignored;
					std::filesystem::remove(temporaryPath, ignored);
				};

				try
				{
					WuaWriterContext context(temporaryPath, std::move(progress),
						cancelled);
					context.writer = new ZArchiveWriter(&WuaWriterContext::NewOutputFile,
						&WuaWriterContext::WriteOutputData, &context);
					if (!context.valid)
					{
						context.Close();
						removeTemporary();
						return {ContentOperationError::UnableToCreateOutput,
							"Unable to create archive output"};
					}

					context.Publish(ContentOperationPhase::Counting);
					for (auto& title : titles)
					{
						if (!context.Process(title, true))
						{
							const bool wasCancelled = context.IsCancelled();
							context.Close();
							removeTemporary();
							return {wasCancelled ? ContentOperationError::Cancelled :
								ContentOperationError::ReadFailure,
								wasCancelled ? "Conversion cancelled" :
								"Unable to enumerate title files"};
						}
					}
					for (auto& title : titles)
					{
						if (!context.Process(title, false))
						{
							const bool wasCancelled = context.IsCancelled();
							context.Close();
							removeTemporary();
							return {wasCancelled ? ContentOperationError::Cancelled :
								ContentOperationError::ReadFailure,
								wasCancelled ? "Conversion cancelled" :
								"Unable to read title files"};
						}
					}
					if (!context.valid)
					{
						context.Close();
						removeTemporary();
						return {ContentOperationError::UnableToCreateOutput,
							"Unable to write archive output"};
					}

					if (context.IsCancelled())
					{
						context.Close();
						removeTemporary();
						return {ContentOperationError::Cancelled, "Conversion cancelled"};
					}
					context.Publish(ContentOperationPhase::Finalizing);
					context.writer->Finalize();
					delete context.stream;
					context.stream = nullptr;
					std::unique_ptr<ZArchiveReader> verification(
						ZArchiveReader::OpenFromFile(temporaryPath));
					if (!verification)
					{
						context.Close();
						removeTemporary();
						return {ContentOperationError::VerificationFailure,
							"Unable to reopen the generated archive"};
					}
				}
				catch (const std::exception& exception)
				{
					removeTemporary();
					return {ContentOperationError::ReadFailure, exception.what()};
				}
				catch (...)
				{
					removeTemporary();
					return {ContentOperationError::ReadFailure,
						"Unknown archive conversion failure"};
				}

				if (cancelled && cancelled())
				{
					removeTemporary();
					return {ContentOperationError::Cancelled, "Conversion cancelled"};
				}

				auto backupPath = outputPath;
				backupPath += fmt::format(".backup.{}",
					std::chrono::steady_clock::now().time_since_epoch().count());
				std::error_code renameError;
				const bool destinationExists = std::filesystem::exists(outputPath, renameError);
				if (renameError)
				{
					removeTemporary();
					return {ContentOperationError::RenameFailure, renameError.message()};
				}
				if (destinationExists)
				{
					std::filesystem::rename(outputPath, backupPath, renameError);
					if (renameError)
					{
						removeTemporary();
						return {ContentOperationError::RenameFailure, renameError.message()};
					}
				}
				std::filesystem::rename(temporaryPath, outputPath, renameError);
				if (renameError)
				{
					if (destinationExists)
					{
						std::error_code restoreError;
						std::filesystem::rename(backupPath, outputPath, restoreError);
						if (restoreError)
						{
							removeTemporary();
							return {ContentOperationError::RenameFailure,
								fmt::format("{}; original output remains at {} because restore failed: {}",
									renameError.message(), _pathToUtf8(backupPath),
									restoreError.message())};
						}
					}
					removeTemporary();
					return {ContentOperationError::RenameFailure, renameError.message()};
				}
				if (destinationExists)
				{
					std::error_code ignored;
					std::filesystem::remove(backupPath, ignored);
				}
				CafeTitleList::Refresh();
				return {};
			}

			ContentChecksumResult ComputeTitleChecksum(
				std::uint64_t locationUid, ContentProgressHandler progress,
				ContentCancellationCheck cancelled) override
			{
				auto title = CafeTitleList::GetTitleInfoByUID(locationUid);
				if (!title.IsValid())
					return {ContentOperationError::NotFound,
						"Title content is no longer available", std::nullopt};

				ContentChecksum checksum{
					.titleId = title.GetAppTitleId(),
					.version = title.GetAppTitleVersion(),
					.region = static_cast<std::uint32_t>(title.GetMetaRegion()),
				};
				auto isCancelled = [&cancelled] { return cancelled && cancelled(); };
				auto publish = [&progress](const ContentOperationProgress& value) {
					if (progress)
						progress(value);
				};
				auto toHex = [](const std::array<std::uint8_t, SHA256_DIGEST_LENGTH>& digest) {
					std::string value;
					value.reserve(SHA256_DIGEST_LENGTH * 2);
					for (const auto byte : digest)
						value.append(fmt::format("{:02X}", byte));
					return value;
				};

				try
				{
					std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
					if (title.GetFormat() == TitleInfo::TitleDataFormat::WUD)
					{
						using WudHandle = std::unique_ptr<wud_t, decltype(&wud_close)>;
						WudHandle wud(wud_open(title.GetPath()), &wud_close);
						if (!wud)
							return {ContentOperationError::ReadFailure,
								"Unable to open game image", std::nullopt};
						const auto total = wud_getWUDSize(wud.get());
						if (total <= 0)
							return {ContentOperationError::ReadFailure,
								"Game image is empty", std::nullopt};
						using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
						DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
						if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
							return {ContentOperationError::ReadFailure,
								"Unable to initialize SHA-256", std::nullopt};
						std::vector<std::uint8_t> buffer(8 * 1024 * 1024);
						std::uint64_t offset{};
						while (offset < total)
						{
							if (isCancelled())
								return {ContentOperationError::Cancelled, "Checksum cancelled",
									std::nullopt};
							const auto requested = static_cast<std::size_t>(
								std::min<std::uint64_t>(buffer.size(), total - offset));
							const auto read = wud_readData(wud.get(), buffer.data(), requested, offset);
							if (read != requested ||
								EVP_DigestUpdate(context.get(), buffer.data(), read) != 1)
								return {ContentOperationError::ReadFailure,
									"Game image ended before its declared size", std::nullopt};
							offset += read;
							publish({ContentOperationPhase::Hashing, 0, 0, offset, total});
						}
						unsigned int digestLength{};
						if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestLength) != 1 ||
							digestLength != digest.size())
							return {ContentOperationError::ReadFailure,
								"Unable to finalize SHA-256", std::nullopt};
						checksum.imageSha256 = toHex(digest);
					}
					else
					{
						const auto mountPath = TitleInfo::GetUniqueTempMountingPath();
						if (!title.Mount(mountPath, "", FSC_PRIORITY_BASE))
							return {ContentOperationError::ReadFailure,
								"Unable to mount title content", std::nullopt};
						struct UnmountGuard
						{
							TitleInfo& title;
							std::string path;
							~UnmountGuard() { title.Unmount(path); }
						} unmount{title, mountPath};

						std::vector<std::pair<std::string, std::uint64_t>> files;
						std::function<bool(const std::string&)> collect = [&](const std::string& relative) {
							if (isCancelled())
								return false;
							sint32 status{};
							std::unique_ptr<FSCVirtualFile, decltype(&fsc_close)> directory(
								fsc_openDirIterator((mountPath + relative).c_str(), &status),
								&fsc_close);
							if (!directory)
								return false;
							FSCDirEntry entry;
							while (fsc_nextDir(directory.get(), &entry))
							{
								const auto child = relative + entry.path;
								if (entry.isDirectory)
								{
									if (!collect(child + "/"))
										return false;
								}
								else if (entry.isFile)
								{
									files.emplace_back(child,
										static_cast<std::uint64_t>(entry.fileSize));
									publish({ContentOperationPhase::Collecting, 0,
										static_cast<std::uint32_t>(files.size()), 0, 0});
								}
							}
							return true;
						};
						if (!collect(""))
							return {isCancelled() ? ContentOperationError::Cancelled :
								ContentOperationError::ReadFailure,
								isCancelled() ? "Checksum cancelled" :
								"Unable to enumerate title files", std::nullopt};
						std::ranges::sort(files, {}, &decltype(files)::value_type::first);
						for (std::size_t index = 0; index < files.size(); ++index)
						{
							const auto& [filePath, expectedSize] = files[index];
							if (isCancelled())
								return {ContentOperationError::Cancelled, "Checksum cancelled",
									std::nullopt};
							sint32 status{};
							std::unique_ptr<FSCVirtualFile, decltype(&fsc_close)> file(
								fsc_open((mountPath + "/" + filePath).c_str(),
									FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION,
									&status), &fsc_close);
							if (!file)
								return {ContentOperationError::ReadFailure,
									fmt::format("Unable to read {}", filePath), std::nullopt};
							using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
							DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
							if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
								return {ContentOperationError::ReadFailure,
									"Unable to initialize SHA-256", std::nullopt};
							std::vector<std::uint8_t> buffer(1024 * 1024);
							std::uint64_t bytesRead{};
							for (;;)
							{
								if (isCancelled())
									return {ContentOperationError::Cancelled,
										"Checksum cancelled", std::nullopt};
								const auto read = file->fscReadData(buffer.data(), buffer.size());
								if (read == 0)
									break;
								if (EVP_DigestUpdate(context.get(), buffer.data(), read) != 1)
									return {ContentOperationError::ReadFailure,
										"Unable to update SHA-256", std::nullopt};
								bytesRead += read;
							}
							if (bytesRead != expectedSize)
								return {ContentOperationError::ReadFailure,
									fmt::format("{} ended before its declared size", filePath),
									std::nullopt};
							unsigned int digestLength{};
							if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestLength) != 1 ||
								digestLength != digest.size())
								return {ContentOperationError::ReadFailure,
									"Unable to finalize SHA-256", std::nullopt};
							checksum.files.push_back({filePath, toHex(digest)});
							publish({ContentOperationPhase::Hashing,
								static_cast<std::uint32_t>(index + 1),
								static_cast<std::uint32_t>(files.size()), 0, 0});
						}
					}
					return {ContentOperationError::None, {}, std::move(checksum)};
				}
				catch (const std::exception& exception)
				{
					return {ContentOperationError::ReadFailure, exception.what(), std::nullopt};
				}
				catch (...)
				{
					return {ContentOperationError::ReadFailure,
						"Unknown checksum failure", std::nullopt};
				}
			}

			GameProfileView LoadGameProfile(std::uint64_t titleId) const override
			{
				GameProfile profile;
				profile.Reset();
				profile.Load(titleId);

				GameProfileView result;
				result.gameName = profile.GetGameName();
				result.defaultProfile = profile.IsDefaultProfile();
				result.settings.loadSharedLibraries =
					profile.ShouldLoadSharedLibraries().value_or(true);
				result.settings.startWithPadView = profile.StartWithGamepadView();
				result.settings.threadQuantum = profile.GetThreadQuantum();
				result.settings.accurateShaderMultiplication =
					profile.GetAccurateShaderMul() != AccurateShaderMulOption::False;

				switch (profile.GetCPUMode().value_or(CPUMode::Auto))
				{
				case CPUMode::SinglecoreInterpreter:
					result.settings.cpuMode = GameProfileCpuMode::SingleCoreInterpreter;
					break;
				case CPUMode::SinglecoreRecompiler:
					result.settings.cpuMode = GameProfileCpuMode::SingleCoreRecompiler;
					break;
				case CPUMode::DualcoreRecompiler:
				case CPUMode::MulticoreRecompiler:
					result.settings.cpuMode = GameProfileCpuMode::MultiCoreRecompiler;
					break;
				default:
					result.settings.cpuMode = GameProfileCpuMode::Auto;
					break;
				}

				if (const auto api = profile.GetGraphicsAPI())
				{
					switch (*api)
					{
					case kOpenGL: result.settings.graphicsApi = GameProfileGraphicsApi::OpenGL; break;
					case kVulkan: result.settings.graphicsApi = GameProfileGraphicsApi::Vulkan; break;
#ifdef ENABLE_METAL
					case kMetal: result.settings.graphicsApi = GameProfileGraphicsApi::Metal; break;
#endif
					default: result.settings.graphicsApi = GameProfileGraphicsApi::Default; break;
					}
				}

#ifdef ENABLE_METAL
				result.settings.shaderFastMath = profile.GetShaderFastMath();
				result.settings.metalBufferCacheMode =
					static_cast<std::uint8_t>(profile.GetBufferCacheMode());
				result.settings.positionInvariance =
					static_cast<std::uint8_t>(profile.GetPositionInvariance());
#endif
				result.settings.controllerProfiles = profile.GetControllerProfile();
				return result;
			}

			GameProfileSaveResult SaveGameProfile(std::uint64_t titleId,
				const GameProfileUpdate& update) override
			{
				GameProfile profile;
				profile.Reset();
				profile.SetLoadSharedLibraries(update.loadSharedLibraries);
				profile.SetStartWithGamepadView(update.startWithPadView);
				profile.SetThreadQuantum(std::clamp<std::uint32_t>(
					update.threadQuantum, 5000, 536870912));
				profile.SetAccurateShaderMul(update.accurateShaderMultiplication ?
					AccurateShaderMulOption::True : AccurateShaderMulOption::False);

				switch (update.cpuMode)
				{
				case GameProfileCpuMode::SingleCoreInterpreter:
					profile.SetCPUMode(CPUMode::SinglecoreInterpreter);
					break;
				case GameProfileCpuMode::SingleCoreRecompiler:
					profile.SetCPUMode(CPUMode::SinglecoreRecompiler);
					break;
				case GameProfileCpuMode::MultiCoreRecompiler:
					profile.SetCPUMode(CPUMode::MulticoreRecompiler);
					break;
				default:
					profile.SetCPUMode(CPUMode::Auto);
					break;
				}

				switch (update.graphicsApi)
				{
				case GameProfileGraphicsApi::OpenGL: profile.SetGraphicsAPI(kOpenGL); break;
				case GameProfileGraphicsApi::Vulkan: profile.SetGraphicsAPI(kVulkan); break;
#ifdef ENABLE_METAL
				case GameProfileGraphicsApi::Metal: profile.SetGraphicsAPI(kMetal); break;
#endif
				default: profile.SetGraphicsAPI(std::nullopt); break;
				}

#ifdef ENABLE_METAL
				profile.SetShaderFastMath(update.shaderFastMath);
				profile.SetBufferCacheMode(static_cast<MetalBufferCacheMode>(
					std::min<std::uint8_t>(update.metalBufferCacheMode,
						static_cast<std::uint8_t>(MetalBufferCacheMode::Host))));
				profile.SetPositionInvariance(static_cast<PositionInvariance>(
					std::min<std::uint8_t>(update.positionInvariance,
						static_cast<std::uint8_t>(PositionInvariance::True))));
#endif
				for (std::size_t index = 0; index < update.controllerProfiles.size(); ++index)
					profile.SetControllerProfile(index, update.controllerProfiles[index]);

				if (!profile.Save(titleId))
					return {false, "Unable to write game profile"};
				return {true, {}};
			}

			TitleInstallPlanResult PlanTitleInstall(
				const std::filesystem::path& sourcePath) const override
			{
				return SafeBuildTitleInstallPlan(sourcePath);
			}

			TitleInstallResult InstallTitle(const TitleInstallPlan& plan,
				TitleInstallDecision decision, TitleInstallProgressHandler progress,
				TitleInstallCancellationCheck cancelled) override
			{
				if (plan.conflict != TitleInstallConflict::None &&
					decision != TitleInstallDecision::AcceptConflict)
					return {TitleInstallError::ConflictNotAccepted,
						"The existing title conflict was not accepted", {}};

				auto currentPlanResult = SafeBuildTitleInstallPlan(plan.sourcePath);
				if (!currentPlanResult)
					return {currentPlanResult.error, currentPlanResult.diagnostic, {}};
				if (!SameInstallSnapshot(plan, *currentPlanResult.plan))
					return {TitleInstallError::StalePlan,
						"The source or installed title changed before installation", {}};

				std::error_code ec;
				const auto canonicalSource = fs::weakly_canonical(plan.sourcePath, ec);
				ec.clear();
				const auto canonicalTarget = fs::weakly_canonical(plan.targetPath, ec);
				if ((!canonicalSource.empty() && canonicalSource == canonicalTarget) ||
					(fs::exists(plan.targetPath, ec) && !ec &&
						fs::equivalent(plan.sourcePath, plan.targetPath, ec) && !ec))
					return {TitleInstallError::InvalidSource,
						"A title cannot be installed from its destination folder", {}};

				if (cancelled && cancelled())
					return {TitleInstallError::Cancelled, "Title installation cancelled", {}};

				const auto stagingPath = UniqueInstallSibling(plan.targetPath, "installing");
				const auto backupPath = plan.installed.exists ?
					UniqueInstallSibling(plan.targetPath, "backup") : fs::path{};
				if (stagingPath.empty() || (plan.installed.exists && backupPath.empty()))
					return {TitleInstallError::CopyFailure,
						"Unable to reserve a temporary installation path", {}};

				auto removeStaging = [&] {
					std::error_code ignored;
					fs::remove_all(stagingPath, ignored);
				};
				try
				{
					fs::create_directories(stagingPath.parent_path(), ec);
					if (ec)
						return {TitleInstallError::CopyFailure, ec.message(), {}};
					if (!fs::create_directory(stagingPath, ec))
						return {TitleInstallError::CopyFailure,
							ec ? ec.message() : "Temporary installation path already exists", {}};

					std::uint64_t copiedBytes{};
					std::vector<std::uint8_t> buffer(1024 * 1024);
					const std::array<const char*, 3> folders{"content", "code", "meta"};
					for (const auto* folder : folders)
					{
						const auto root = plan.sourcePath / folder;
						fs::recursive_directory_iterator iterator(root, ec), end;
						if (ec)
							throw std::runtime_error(ec.message());
						fs::create_directories(stagingPath / folder, ec);
						if (ec)
							throw std::runtime_error(ec.message());
						while (iterator != end)
						{
							if (cancelled && cancelled())
							{
								removeStaging();
								return {TitleInstallError::Cancelled,
									"Title installation cancelled", {}};
							}
							const auto relative = fs::relative(iterator->path(), plan.sourcePath, ec);
							if (ec)
								throw std::runtime_error(ec.message());
							const auto destination = stagingPath / relative;
							if (iterator->is_directory(ec))
							{
								fs::create_directories(destination, ec);
								if (ec)
									throw std::runtime_error(ec.message());
							}
							else if (iterator->is_regular_file(ec))
							{
								fs::create_directories(destination.parent_path(), ec);
								if (ec)
									throw std::runtime_error(ec.message());
								std::ifstream input(iterator->path(), std::ios::binary);
								std::ofstream output(destination,
									std::ios::binary | std::ios::trunc);
								if (!input || !output)
									throw std::runtime_error(fmt::format("Unable to copy {}",
										_pathToUtf8(iterator->path())));
								while (input)
								{
									if (cancelled && cancelled())
									{
										input.close();
										output.close();
										removeStaging();
										return {TitleInstallError::Cancelled,
											"Title installation cancelled", {}};
									}
									input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
									const auto count = input.gcount();
									if (count <= 0)
										break;
									output.write(reinterpret_cast<const char*>(buffer.data()), count);
									if (!output)
										throw std::runtime_error(fmt::format("Unable to write {}",
											_pathToUtf8(destination)));
									copiedBytes += static_cast<std::uint64_t>(count);
									if (progress)
										progress({copiedBytes, plan.requiredBytes, relative});
								}
								if (!input.eof())
									throw std::runtime_error(fmt::format("Unable to read {}",
										_pathToUtf8(iterator->path())));
								output.flush();
								if (!output)
									throw std::runtime_error(fmt::format("Unable to flush {}",
										_pathToUtf8(destination)));
								const auto permissions = iterator->status(ec).permissions();
								if (!ec)
									fs::permissions(destination, permissions, ec);
								ec.clear();
							}
							else if (ec)
								throw std::runtime_error(ec.message());
							else if (iterator->is_symlink(ec))
							{
								fs::copy(iterator->path(), destination,
									fs::copy_options::copy_symlinks, ec);
								if (ec)
									throw std::runtime_error(ec.message());
							}
							iterator.increment(ec);
							if (ec)
								throw std::runtime_error(ec.message());
						}
					}

					if (copiedBytes != plan.requiredBytes)
						throw std::runtime_error("Copied title size does not match the install plan");
					const auto sourceAfterCopy = SafeBuildTitleInstallPlan(plan.sourcePath, false);
					if (!sourceAfterCopy ||
						sourceAfterCopy.plan->sourceFingerprint != plan.sourceFingerprint ||
						sourceAfterCopy.plan->requiredBytes != plan.requiredBytes ||
						sourceAfterCopy.plan->titleId != plan.titleId ||
						sourceAfterCopy.plan->version != plan.version)
					{
						removeStaging();
						return {TitleInstallError::StalePlan,
							"The title source changed while staging the installation", {}};
					}
					if (cancelled && cancelled())
					{
						removeStaging();
						return {TitleInstallError::Cancelled,
							"Title installation cancelled", {}};
					}

					const bool targetExists = fs::exists(plan.targetPath, ec) && !ec;
					if (ec || targetExists != plan.installed.exists ||
						(targetExists && FingerprintInstallTarget(plan.targetPath) !=
							plan.installed.fingerprint))
					{
						removeStaging();
						return {TitleInstallError::StalePlan,
							"The installed title changed while staging the update", {}};
					}

					bool originalMoved{};
					if (targetExists)
					{
						fs::rename(plan.targetPath, backupPath, ec);
						if (ec)
						{
							removeStaging();
							return {TitleInstallError::CommitFailure, ec.message(), {}};
						}
						originalMoved = true;
					}

					fs::rename(stagingPath, plan.targetPath, ec);
					if (ec)
					{
						const auto commitError = ec;
						if (originalMoved)
						{
							ec.clear();
							fs::rename(backupPath, plan.targetPath, ec);
							if (ec)
							{
								removeStaging();
								return {TitleInstallError::RestoreFailure,
									fmt::format("{}; original title remains at {} because restore failed: {}",
										commitError.message(), _pathToUtf8(backupPath), ec.message()), {}};
							}
						}
						removeStaging();
						return {TitleInstallError::CommitFailure, commitError.message(), {}};
					}

					std::string warning;
					if (originalMoved)
					{
						ec.clear();
						fs::remove_all(backupPath, ec);
						if (ec)
							warning = fmt::format("Installed title, but unable to remove backup {}: {}",
								_pathToUtf8(backupPath), ec.message());
					}
					try
					{
						if (!CafeTitleList::ReplaceTitleFromPath(plan.targetPath))
						{
							if (!warning.empty())
								warning.append("; ");
							warning.append("installed title could not be refreshed in the title catalog");
						}
					}
					catch (const std::exception& exception)
					{
						if (!warning.empty())
							warning.append("; ");
						warning.append(fmt::format(
							"installed title could not be refreshed in the title catalog: {}",
							exception.what()));
					}
					catch (...)
					{
						if (!warning.empty())
							warning.append("; ");
						warning.append(
							"installed title could not be refreshed in the title catalog");
					}
					return {TitleInstallError::None, std::move(warning), plan.targetPath};
				}
				catch (const std::exception& exception)
				{
					removeStaging();
					return {TitleInstallError::CopyFailure, exception.what(), {}};
				}
				catch (...)
				{
					removeStaging();
					return {TitleInstallError::CopyFailure,
						"Unknown title installation failure", {}};
				}
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
