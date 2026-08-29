#include "Common/precompiled.h"

#include "application/EmulationController.h"

#include "Cafe/Account/Account.h"
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
#include "Cafe/OS/libs/cemuextend/CemodPermission.h"
#include "Cafe/OS/libs/cemuextend/BridgeHost.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/nfc/nfc.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "Cafe/OS/libs/erreula/erreula.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/WiimoteController.h"
#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif
#include "Cemu/Tools/DownloadManager/DownloadManager.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Common/FileStream.h"
#include "Common/socket.h"
#include "util/helpers/helpers.h"

#include <zarchive/zarchivereader.h>
#include <zarchive/zarchivewriter.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <pugixml.hpp>
#include <curl/curl.h>
#include <rapidjson/document.h>
#include <zip.h>

#include <condition_variable>
#include <charconv>
#include <cctype>
#include <deque>
#include <fstream>
#include <set>
#include <shared_mutex>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#endif

namespace Application
{
	namespace
	{
		struct ApplicationEventForwarder
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
			switch (event.type)
			{
			case CafeSystem::EventType::LoadingStarted:
				translated.type = EventType::LoadingStarted;
				break;
			case CafeSystem::EventType::GameLoaded:
				translated.type = EventType::GameLoaded;
				break;
			case CafeSystem::EventType::GameExited:
				translated.type = EventType::GameExited;
				break;
			case CafeSystem::EventType::PpcProcessExited:
				translated.type = EventType::PpcProcessExited;
				break;
			case CafeSystem::EventType::PerformanceUpdated:
				translated.type = EventType::PerformanceUpdated;
				break;
			case CafeSystem::EventType::Diagnostic:
				translated.type = EventType::Diagnostic;
				break;
			}
			switch (event.diagnosticCode)
			{
			case CafeSystem::DiagnosticCode::DamagedExecutable:
				translated.diagnosticCode = DiagnosticCode::DamagedExecutable;
				break;
			case CafeSystem::DiagnosticCode::KeyFileCreateFailed:
				translated.diagnosticCode = DiagnosticCode::KeyFileCreateFailed;
				break;
			case CafeSystem::DiagnosticCode::KeyFileInvalidLine:
				translated.diagnosticCode = DiagnosticCode::KeyFileInvalidLine;
				break;
			case CafeSystem::DiagnosticCode::GraphicPackInvalid:
				translated.diagnosticCode = DiagnosticCode::GraphicPackInvalid;
				break;
			case CafeSystem::DiagnosticCode::MemoryAllocationFailed:
				translated.diagnosticCode = DiagnosticCode::MemoryAllocationFailed;
				break;
			case CafeSystem::DiagnosticCode::MemoryReservationFailed:
				translated.diagnosticCode = DiagnosticCode::MemoryReservationFailed;
				break;
			}
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
					.executionMode = request.executionMode == ::CemodExecutionMode::TrustedNative ? CemodExecutionMode::TrustedNative : CemodExecutionMode::Isolated,
					.signedPackage = request.signedPackage,
					.headless = request.headless,
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
				.executionMode = package.executionMode == ::CemodExecutionMode::TrustedNative ? CemodExecutionMode::TrustedNative : CemodExecutionMode::Isolated,
				.signedPackage = package.signedPackage,
				.titleIds = std::move(package.titleIds),
				.error = std::move(package.error),
			};
		}

		std::uint64_t CemodCatalogFingerprint(std::span<const CemodManagerPackage> packages)
		{
			std::uint64_t value = 1469598103934665603ULL;
			auto append = [&value](std::string_view text) {
				for (const auto byte : text)
				{
					value ^= static_cast<unsigned char>(byte);
					value *= 1099511628211ULL;
				}
			};
			for (const auto& package : packages)
			{
				append(package.packageKey);
				append(package.modIdentity);
				append(package.status);
				auto appendInteger = [&value](std::uint64_t integer) {
					for (unsigned shift = 0; shift < 64; shift += 8)
					{
						value ^= static_cast<std::uint8_t>(integer >> shift);
						value *= 1099511628211ULL;
					}
				};
				appendInteger(package.requestedPermissions);
				appendInteger(package.grantedPermissions);
				appendInteger(package.approved);
				appendInteger(package.headless);
				appendInteger(package.valid);
				for (const auto titleId : package.titleIds)
					appendInteger(titleId);
			}
			return value == 0 ? 1 : value;
		}

		AccountOnlineError TranslateAccountOnlineError(::OnlineAccountError error)
		{
			switch (error)
			{
			case ::OnlineAccountError::kNoAccountId:
				return AccountOnlineError::NoAccountId;
			case ::OnlineAccountError::kNoPasswordCached:
				return AccountOnlineError::NoPasswordCached;
			case ::OnlineAccountError::kPasswordCacheEmpty:
				return AccountOnlineError::PasswordCacheEmpty;
			case ::OnlineAccountError::kNoPrincipalId:
				return AccountOnlineError::NoPrincipalId;
			case ::OnlineAccountError::kNone:
			default:
				return AccountOnlineError::None;
			}
		}

		AccountFileState TranslateAccountFileState(::OnlineValidator::FileState state)
		{
			switch (state)
			{
			case ::OnlineValidator::FileState::Corrupted:
				return AccountFileState::Corrupted;
			case ::OnlineValidator::FileState::Ok:
				return AccountFileState::Ok;
			case ::OnlineValidator::FileState::Missing:
			default:
				return AccountFileState::Missing;
			}
		}

		AccountInfo TranslateAccount(const ::Account& account)
		{
			return {
				.persistentId = account.GetPersistentId(),
				.miiName = std::wstring(account.GetMiiName()),
				.birthYear = account.GetBirthYear(),
				.birthMonth = account.GetBirthMonth(),
				.birthDay = account.GetBirthDay(),
				.gender = account.GetGender(),
				.email = std::string(account.GetEmail()),
				.country = account.GetCountry(),
				.validOnlineAccount = account.IsValidOnlineAccount(),
			};
		}

		AccountNetworkService TranslateNetworkService(NetworkService service)
		{
			switch (service)
			{
			case NetworkService::Nintendo:
				return AccountNetworkService::Nintendo;
			case NetworkService::Pretendo:
				return AccountNetworkService::Pretendo;
			case NetworkService::Custom:
				return AccountNetworkService::Custom;
			case NetworkService::Plasma:
				return AccountNetworkService::Plasma;
			default:
				return AccountNetworkService::Offline;
			}
		}

		NetworkService TranslateNetworkService(AccountNetworkService service)
		{
			switch (service)
			{
			case AccountNetworkService::Nintendo:
				return NetworkService::Nintendo;
			case AccountNetworkService::Pretendo:
				return NetworkService::Pretendo;
			case AccountNetworkService::Custom:
				return NetworkService::Custom;
			case AccountNetworkService::Plasma:
				return NetworkService::Plasma;
			default:
				return NetworkService::Offline;
			}
		}

		const ::Account* FindAccount(std::uint32_t persistentId)
		{
			const auto& accounts = ::Account::GetAccounts();
			const auto found = std::ranges::find_if(accounts,
													[persistentId](const auto& account) {
														return account.GetPersistentId() == persistentId;
													});
			return found == accounts.end() ? nullptr : &*found;
		}

		constexpr std::uint64_t kMaximumSaveArchiveEntrySize = 128ULL * 1024 * 1024;
		constexpr std::uint64_t kMaximumSaveArchiveTotalSize = 2ULL * 1024 * 1024 * 1024;
		constexpr std::uint64_t kMaximumSaveArchiveEntries = 100000;

		struct SaveArchiveEntry
		{
			zip_uint64_t index{};
			fs::path relativePath;
			std::uint64_t size{};
			bool directory{};
		};

		struct SaveArchivePlan
		{
			SaveOperationError error{SaveOperationError::None};
			std::string diagnostic;
			std::optional<std::uint64_t> sourceTitleId;
			std::vector<SaveArchiveEntry> entries;
			std::uint64_t bytesTotal{};

			[[nodiscard]] explicit operator bool() const
			{
				return error == SaveOperationError::None;
			}
		};

		fs::path SaveUserRoot(std::uint64_t titleId)
		{
			return ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/user",
											  static_cast<std::uint32_t>(titleId >> 32),
											  static_cast<std::uint32_t>(titleId));
		}

		fs::path SaveAccountPath(std::uint64_t titleId, std::uint32_t persistentId)
		{
			return SaveUserRoot(titleId) / fmt::format("{:08x}", persistentId);
		}

		fs::path SaveInfoPath(std::uint64_t titleId)
		{
			return ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/meta/saveinfo.xml",
											  static_cast<std::uint32_t>(titleId >> 32),
											  static_cast<std::uint32_t>(titleId));
		}

		SaveEntryLocation InspectSaveEntryPath(const fs::path& path)
		{
			std::error_code ec;
			const auto status = fs::symlink_status(path, ec);
			if (ec == std::errc::no_such_file_or_directory ||
				status.type() == fs::file_type::not_found)
				return {SaveEntryState::Missing, path};
			if (ec)
				return {SaveEntryState::NonDirectory, path};
			return {status.type() == fs::file_type::directory ? SaveEntryState::Directory : SaveEntryState::NonDirectory, path};
		}

		std::optional<fs::path> NormalizeSaveArchivePath(std::string_view rawName)
		{
			if (rawName.empty() || rawName.front() == '/' || rawName.front() == '\\')
				return std::nullopt;

			std::string portableName(rawName);
			std::ranges::replace(portableName, '\\', '/');
			if (portableName.size() >= 2 &&
				std::isalpha(static_cast<unsigned char>(portableName[0])) &&
				portableName[1] == ':')
				return std::nullopt;

			const fs::path original = fs::u8path(portableName);
			if (original.empty() || original.is_absolute() || original.has_root_path())
				return std::nullopt;
			for (const auto& component : original)
			{
				if (component == "..")
					return std::nullopt;
			}

			const auto normalized = original.lexically_normal();
			if (normalized.empty() || normalized == "." || normalized.is_absolute() ||
				normalized.has_root_path())
				return std::nullopt;
			for (const auto& component : normalized)
			{
				if (component == "..")
					return std::nullopt;
			}
			return normalized;
		}

		bool IsZipSymlink(zip_t* archive, zip_uint64_t index)
		{
			zip_uint8_t operatingSystem{};
			zip_uint32_t attributes{};
			if (zip_file_get_external_attributes(archive, index, 0, &operatingSystem,
												 &attributes) != 0)
				return false;
			if (operatingSystem != ZIP_OPSYS_UNIX)
				return false;
			const auto unixMode = attributes >> 16;
			return (unixMode & S_IFMT) == S_IFLNK;
		}

		bool IsSafeGraphicPackZipEntry(zip_t* archive, zip_uint64_t index)
		{
			zip_uint8_t operatingSystem{};
			zip_uint32_t attributes{};
			if (zip_file_get_external_attributes(archive, index, 0, &operatingSystem,
												 &attributes) != 0)
				return false;
			if (operatingSystem != ZIP_OPSYS_UNIX)
				return true;
			const auto type = (attributes >> 16) & S_IFMT;
			return type == 0 || type == S_IFREG || type == S_IFDIR;
		}

		SaveArchivePlan BuildSaveArchivePlan(const fs::path& archivePath)
		{
			SaveArchivePlan plan;
			int zipError{};
			zip_t* archive = zip_open(_pathToUtf8(archivePath).c_str(), ZIP_RDONLY,
									  &zipError);
			if (!archive)
			{
				plan.error = SaveOperationError::ArchiveInvalid;
				plan.diagnostic = "unable to open save archive";
				return plan;
			}
			const std::unique_ptr<zip_t, decltype(&zip_discard)> closeArchive(
				archive, &zip_discard);

			const auto entryCount = zip_get_num_entries(archive, 0);
			if (entryCount < 0 || static_cast<zip_uint64_t>(entryCount) >
									  kMaximumSaveArchiveEntries)
			{
				plan.error = SaveOperationError::ArchiveInvalid;
				plan.diagnostic = "save archive contains too many entries";
				return plan;
			}

			std::set<std::string> normalizedNames;
			bool metadataFound{};
			for (zip_int64_t index = 0; index < entryCount; ++index)
			{
				zip_stat_t stat{};
				zip_stat_init(&stat);
				if (zip_stat_index(archive, static_cast<zip_uint64_t>(index), 0, &stat) != 0 ||
					!stat.name)
				{
					plan.error = SaveOperationError::ArchiveInvalid;
					plan.diagnostic = "unable to inspect save archive entry";
					return plan;
				}

				const std::string_view rawName(stat.name);
				if (rawName == "cemu_meta")
				{
					if (metadataFound)
					{
						plan.error = SaveOperationError::ArchiveInvalid;
						plan.diagnostic = "save archive contains duplicate metadata";
						return plan;
					}
					metadataFound = true;
					if (stat.size > 256)
					{
						plan.error = SaveOperationError::ArchiveInvalid;
						plan.diagnostic = "save archive metadata is too large";
						return plan;
					}
					zip_file_t* file = zip_fopen_index(archive,
													   static_cast<zip_uint64_t>(index), 0);
					if (!file)
					{
						plan.error = SaveOperationError::ArchiveInvalid;
						plan.diagnostic = "unable to read save archive metadata";
						return plan;
					}
					const std::unique_ptr<zip_file_t, void (*)(zip_file_t*)> closeFile(
						file, [](zip_file_t* value) { (void)zip_fclose(value); });
					std::string metadata(static_cast<std::size_t>(stat.size), '\0');
					if (stat.size && zip_fread(file, metadata.data(), stat.size) !=
										 static_cast<zip_int64_t>(stat.size))
					{
						plan.error = SaveOperationError::ArchiveInvalid;
						plan.diagnostic = "save archive metadata is truncated";
						return plan;
					}
					constexpr std::string_view prefix = "titleId = ";
					if (metadata.starts_with(prefix))
					{
						std::string_view value(metadata);
						value.remove_prefix(prefix.size());
						if (value.starts_with("0x") || value.starts_with("0X"))
							value.remove_prefix(2);
						std::uint64_t titleId{};
						const auto parsed = std::from_chars(value.data(),
															value.data() + value.size(), titleId, 16);
						if (parsed.ec == std::errc())
							plan.sourceTitleId = titleId;
					}
					continue;
				}

				if (IsZipSymlink(archive, static_cast<zip_uint64_t>(index)))
				{
					plan.error = SaveOperationError::PathUnsafe;
					plan.diagnostic = "save archive contains a symbolic link";
					return plan;
				}
				const auto relativePath = NormalizeSaveArchivePath(rawName);
				if (!relativePath)
				{
					plan.error = SaveOperationError::PathUnsafe;
					plan.diagnostic = "save archive contains an unsafe path";
					return plan;
				}
				const auto normalizedName = relativePath->generic_string();
				if (!normalizedNames.insert(normalizedName).second)
				{
					plan.error = SaveOperationError::ArchiveInvalid;
					plan.diagnostic = "save archive contains duplicate paths";
					return plan;
				}

				const bool directory = rawName.ends_with('/') || rawName.ends_with('\\');
				if (!directory)
				{
					if (stat.size > kMaximumSaveArchiveEntrySize ||
						plan.bytesTotal > kMaximumSaveArchiveTotalSize - stat.size)
					{
						plan.error = SaveOperationError::ArchiveInvalid;
						plan.diagnostic = "save archive exceeds extraction limits";
						return plan;
					}
					plan.bytesTotal += stat.size;
				}
				plan.entries.push_back({static_cast<zip_uint64_t>(index),
										*relativePath, stat.size, directory});
			}
			return plan;
		}

		fs::path UniqueSiblingPath(const fs::path& target, std::string_view purpose)
		{
			static std::atomic_uint64_t sequence{};
			const auto nonce = static_cast<std::uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			for (std::uint64_t attempt = 0; attempt != 1024; ++attempt)
			{
				const auto candidate = target.parent_path() /
									   fmt::format("{}.{}.{}", _pathToUtf8(target.filename()), purpose,
												   nonce + sequence.fetch_add(1) + attempt);
				std::error_code ec;
				if (!fs::exists(candidate, ec) && !ec)
					return candidate;
			}
			return {};
		}

		void RemovePathQuietly(const fs::path& path)
		{
			if (path.empty())
				return;
			std::error_code ec;
			fs::remove_all(path, ec);
		}

		FrontendSettingsResult EnsureDefaultMlcFiles(const fs::path& mlc,
													 std::span<const AccountCountry> countries, FrontendSettingsSnapshot snapshot)
		{
			const std::array directories{
				mlc,
				mlc / "sys",
				mlc / "usr",
				mlc / "usr/title/00050000",
				mlc / "usr/title/0005000c",
				mlc / "usr/title/0005000e",
				mlc / "usr/save/00050010/1004a000/user/common/db",
				mlc / "usr/save/00050010/1004a100/user/common/db",
				mlc / "usr/save/00050010/1004a200/user/common/db",
				mlc / "sys/title/0005001b/1005c000/content",
			};
			std::error_code ec;
			for (const auto& directory : directories)
			{
				fs::create_directories(directory, ec);
				if (ec || !fs::is_directory(directory, ec) || ec)
					return {FrontendSettingsError::StorageFailed, std::move(snapshot),
							fmt::format("unable to create MLC directory {}: {}", _pathToUtf8(directory),
										ec ? ec.message() : "path is not a directory")};
			}
			const auto content = mlc / "sys/title/0005001b/1005c000/content";
			auto writeIfMissing = [&](const fs::path& path, std::string_view data) -> bool {
				if (fs::exists(path, ec))
					return !ec && fs::is_regular_file(path, ec) && !ec;
				if (ec)
					return false;
				auto temporary = path;
				temporary += fmt::format(".{}.tmp",
										 std::chrono::steady_clock::now().time_since_epoch().count());
				{
					std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
					output.write(data.data(), static_cast<std::streamsize>(data.size()));
					output.flush();
					if (!output)
					{
						RemovePathQuietly(temporary);
						return false;
					}
				}
				fs::rename(temporary, path, ec);
				if (ec)
				{
					RemovePathQuietly(temporary);
					return false;
				}
				return true;
			};
			constexpr std::string_view languages =
				R"("ja",
"en",
"fr",
"de",
"it",
"es",
"zh",
"ko",
"nl",
"pt",
"ru",
"zh",
)";
			if (!writeIfMissing(content / "language.txt", languages))
				return {FrontendSettingsError::StorageFailed, std::move(snapshot),
						"unable to initialize the MLC language table"};
			std::string countryTable;
			for (const auto& country : countries)
				countryTable += boost::iequals(country.name, "NN") ? "NULL,\n" : fmt::format("\"{}\",\n", country.name);
			if (!writeIfMissing(content / "country.txt", countryTable))
				return {FrontendSettingsError::StorageFailed, std::move(snapshot),
						"unable to initialize the MLC country table"};
			const auto writeTest = mlc / fmt::format(".cemu-write-test-{}",
													 std::chrono::steady_clock::now().time_since_epoch().count());
			{
				std::ofstream output(writeTest, std::ios::binary | std::ios::trunc);
				output << "ok";
				if (!output)
				{
					RemovePathQuietly(writeTest);
					return {
						FrontendSettingsError::StorageFailed, std::move(snapshot),
						"the MLC directory is not writable"};
				}
			}
			fs::remove(writeTest, ec);
			if (ec)
				return {FrontendSettingsError::StorageFailed, std::move(snapshot),
						"unable to remove the MLC write test"};
			return {FrontendSettingsError::None, std::move(snapshot), {}};
		}

		EmulatedControllerType TranslateEmulatedControllerType(EmulatedController::Type type)
		{
			switch (type)
			{
			case EmulatedController::VPAD:
				return EmulatedControllerType::GamePad;
			case EmulatedController::Pro:
				return EmulatedControllerType::ProController;
			case EmulatedController::Classic:
				return EmulatedControllerType::ClassicController;
			case EmulatedController::Wiimote:
				return EmulatedControllerType::Wiimote;
			default:
				return EmulatedControllerType::Disabled;
			}
		}

		std::optional<EmulatedController::Type> TranslateEmulatedControllerType(
			EmulatedControllerType type)
		{
			switch (type)
			{
			case EmulatedControllerType::GamePad:
				return EmulatedController::VPAD;
			case EmulatedControllerType::ProController:
				return EmulatedController::Pro;
			case EmulatedControllerType::ClassicController:
				return EmulatedController::Classic;
			case EmulatedControllerType::Wiimote:
				return EmulatedController::Wiimote;
			default:
				return std::nullopt;
			}
		}

		std::string InputMappingLabel(const EmulatedController& controller,
									  std::uint64_t mapping)
		{
			switch (controller.type())
			{
			case EmulatedController::VPAD:
				if (mapping == VPADController::kButtonId_Mic)
					return "Blow microphone";
				if (mapping == VPADController::kButtonId_Screen)
					return "Show screen";
				return std::string(VPADController::get_button_name(
					static_cast<VPADController::ButtonId>(mapping)));
			case EmulatedController::Pro:
				return std::string(ProController::get_button_name(
					static_cast<ProController::ButtonId>(mapping)));
			case EmulatedController::Classic:
				return std::string(ClassicController::get_button_name(
					static_cast<ClassicController::ButtonId>(mapping)));
			case EmulatedController::Wiimote:
				return std::string(WiimoteController::get_button_name(
					static_cast<WiimoteController::ButtonId>(mapping)));
			default:
				return fmt::format("Mapping {}", mapping);
			}
		}

		constexpr std::size_t kMaximumGraphicPackDownloadSize = 512ULL * 1024ULL * 1024ULL;
		constexpr zip_uint64_t kMaximumGraphicPackEntries = 20000;
		constexpr zip_uint64_t kMaximumGraphicPackFileSize = 128ULL * 1024ULL * 1024ULL;
		constexpr zip_uint64_t kMaximumGraphicPackTotalSize = 2ULL * 1024ULL * 1024ULL * 1024ULL;
		constexpr zip_uint64_t kMaximumGraphicPackCompressionRatio = 1000;

		struct GraphicPackInstallTransaction
		{
			fs::path target;
			fs::path backup;
			bool replacedExisting{};

			void Commit() const
			{
				RemovePathQuietly(backup);
			}

			GraphicPackInstallResult Rollback() const
			{
				RemovePathQuietly(target);
				if (!replacedExisting)
					return {};
				std::error_code ec;
				fs::rename(backup, target, ec);
				return ec ? GraphicPackInstallResult{GraphicPackInstallError::IoFailure,
													 fmt::format("graphic-pack rollback failed: {}", ec.message())}
						  : GraphicPackInstallResult{};
			}
		};

		struct GraphicPackDownloadBuffer
		{
			std::vector<std::uint8_t> bytes;
			GraphicPackInstallProgressHandler progress;
			GraphicPackInstallCancellationCheck cancelled;
			GraphicPackInstallPhase phase{GraphicPackInstallPhase::Checking};
		};

		bool IsPublicGraphicPackAddress(const sockaddr& address)
		{
			if (address.sa_family == AF_INET)
			{
				const auto& ipv4 = reinterpret_cast<const sockaddr_in&>(address);
				const auto value = ntohl(ipv4.sin_addr.s_addr);
				const auto first = value >> 24;
				const auto second = (value >> 16) & 0xff;
				if (first == 0 || first == 10 || first == 127 || first >= 224)
					return false;
				if (first == 100 && second >= 64 && second <= 127)
					return false;
				if (first == 169 && second == 254)
					return false;
				if (first == 172 && second >= 16 && second <= 31)
					return false;
				if (first == 192 && (second == 0 || second == 168))
					return false;
				if (first == 198 && (second == 18 || second == 19))
					return false;
				// Documentation networks are never valid download origins.
				if ((first == 192 && second == 0 && ((value >> 8) & 0xff) == 2) ||
					(first == 198 && second == 51 && ((value >> 8) & 0xff) == 100) ||
					(first == 203 && second == 0 && ((value >> 8) & 0xff) == 113))
					return false;
				return true;
			}
			if (address.sa_family == AF_INET6)
			{
				const auto& ipv6 = reinterpret_cast<const sockaddr_in6&>(address);
				const auto* bytes = ipv6.sin6_addr.s6_addr;
				const bool allZeroPrefix = std::all_of(bytes, bytes + 12,
													   [](std::uint8_t byte) { return byte == 0; });
				if (allZeroPrefix)
					return false;
				const bool mappedIpv4 = std::all_of(bytes, bytes + 10,
													[](std::uint8_t byte) { return byte == 0; }) &&
										bytes[10] == 0xff && bytes[11] == 0xff;
				if (mappedIpv4)
				{
					sockaddr_in mapped{};
					mapped.sin_family = AF_INET;
					std::memcpy(&mapped.sin_addr.s_addr, bytes + 12, sizeof(mapped.sin_addr.s_addr));
					return IsPublicGraphicPackAddress(reinterpret_cast<const sockaddr&>(mapped));
				}
				if ((bytes[0] & 0xfe) == 0xfc ||
					(bytes[0] == 0xfe && ((bytes[1] & 0xc0) == 0x80 ||
										  (bytes[1] & 0xc0) == 0xc0)) ||
					bytes[0] == 0xff)
					return false;
				if (bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x0d && bytes[3] == 0xb8)
					return false;
				// Reject transition mechanisms that can hide an IPv4 destination from
				// the socket address policy (Teredo, NAT64, and 6to4).
				if ((bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0 && bytes[3] == 0) ||
					(bytes[0] == 0 && bytes[1] == 0x64 && bytes[2] == 0xff && bytes[3] == 0x9b) ||
					(bytes[0] == 0x20 && bytes[1] == 0x02))
					return false;
				return true;
			}
			return false;
		}

		curl_socket_t OpenPublicGraphicPackSocket(void*, curlsocktype purpose,
												  curl_sockaddr* address)
		{
			if (purpose != CURLSOCKTYPE_IPCXN || !address ||
				!IsPublicGraphicPackAddress(address->addr))
				return CURL_SOCKET_BAD;
			return ::socket(address->family, address->socktype, address->protocol);
		}

		bool IsValidGraphicPackHttpsUrl(std::string_view url)
		{
			if (url.empty() || url.size() > 4096)
				return false;
			std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), &curl_url_cleanup);
			if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, std::string(url).c_str(), 0) != CURLUE_OK)
				return false;
			auto readPart = [&](CURLUPart part) -> std::optional<std::string> {
				char* raw{};
				if (curl_url_get(parsed.get(), part, &raw, 0) != CURLUE_OK)
					return std::nullopt;
				std::unique_ptr<char, decltype(&curl_free)> value(raw, &curl_free);
				return std::string(value.get());
			};
			const auto scheme = readPart(CURLUPART_SCHEME);
			const auto host = readPart(CURLUPART_HOST);
			if (!scheme || !boost::iequals(*scheme, "https") || !host || host->empty())
				return false;
			if (boost::iequals(*host, "localhost") || boost::iends_with(*host, ".localhost"))
				return false;
			return !readPart(CURLUPART_USER) && !readPart(CURLUPART_PASSWORD);
		}

		size_t WriteGraphicPackDownload(void* data, size_t size, size_t count, void* context)
		{
			auto& buffer = *static_cast<GraphicPackDownloadBuffer*>(context);
			if (size != 0 && count > std::numeric_limits<size_t>::max() / size)
				return 0;
			const auto byteCount = size * count;
			if (buffer.cancelled && buffer.cancelled())
				return 0;
			if (byteCount > kMaximumGraphicPackDownloadSize -
								std::min(buffer.bytes.size(), kMaximumGraphicPackDownloadSize))
				return 0;
			const auto* source = static_cast<const std::uint8_t*>(data);
			buffer.bytes.insert(buffer.bytes.end(), source, source + byteCount);
			return byteCount;
		}

		int ReportGraphicPackDownload(void* context, curl_off_t total, curl_off_t current,
									  curl_off_t, curl_off_t)
		{
			auto& buffer = *static_cast<GraphicPackDownloadBuffer*>(context);
			if (buffer.cancelled && buffer.cancelled())
				return 1;
			if (buffer.progress)
				buffer.progress({buffer.phase, static_cast<std::uint64_t>(std::max<curl_off_t>(0, current)), static_cast<std::uint64_t>(std::max<curl_off_t>(0, total)), {}});
			return 0;
		}

		GraphicPackInstallResult DownloadGraphicPackUrl(std::string_view url,
														GraphicPackDownloadBuffer& buffer, GraphicPackInstallPhase phase)
		{
			if (!IsValidGraphicPackHttpsUrl(url))
				return {GraphicPackInstallError::InvalidUrl, "graphic-pack URL must use HTTPS"};
			if (buffer.cancelled && buffer.cancelled())
				return {GraphicPackInstallError::Cancelled, "graphic-pack installation was cancelled"};
			std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
																	 &curl_easy_cleanup);
			if (!curl)
				return {GraphicPackInstallError::ConnectionFailed, "unable to initialize HTTPS client"};
			buffer.bytes.clear();
			buffer.phase = phase;
			const std::string ownedUrl(url);
			curl_easy_setopt(curl.get(), CURLOPT_URL, ownedUrl.c_str());
			// Environment proxies would make the socket callback inspect the proxy
			// rather than the ultimate URL target, bypassing the private-address ban.
			curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
			curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "*");
			curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &WriteGraphicPackDownload);
			curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buffer);
			curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &ReportGraphicPackDownload);
			curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &buffer);
			curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
			curl_easy_setopt(curl.get(), CURLOPT_OPENSOCKETFUNCTION, &OpenPublicGraphicPackSocket);
			curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
			curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 900L);
			curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 30L);
			curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 15L);
			curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
			curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
			curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
			curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
			curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, BUILD_VERSION_WITH_NAME_STRING);
			const auto code = curl_easy_perform(curl.get());
			if (buffer.cancelled && buffer.cancelled())
				return {GraphicPackInstallError::Cancelled, "graphic-pack installation was cancelled"};
			if (code != CURLE_OK)
				return {GraphicPackInstallError::ConnectionFailed, curl_easy_strerror(code)};
			long status{};
			curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
			if (status < 200 || status >= 300)
				return {GraphicPackInstallError::ConnectionFailed,
						fmt::format("graphic-pack server returned HTTP {}", status)};
			return {};
		}

		GraphicPackInstallResult ExtractGraphicPackArchive(
			const std::vector<std::uint8_t>& bytes, const fs::path& target,
			bool replaceExisting, bool requireRootRules, std::string_view version,
			GraphicPackInstallProgressHandler progress,
			GraphicPackInstallCancellationCheck cancelled,
			GraphicPackInstallTransaction* transaction)
		{
			zip_error_t zipError;
			zip_error_init(&zipError);
			zip_source_t* source = zip_source_buffer_create(bytes.data(), bytes.size(), 0, &zipError);
			if (!source)
			{
				zip_error_fini(&zipError);
				return {GraphicPackInstallError::InvalidArchive, "unable to read graphic-pack archive"};
			}
			zip_t* rawArchive = zip_open_from_source(source, ZIP_RDONLY, &zipError);
			if (!rawArchive)
			{
				zip_source_free(source);
				zip_error_fini(&zipError);
				return {GraphicPackInstallError::InvalidArchive, "download is not a valid ZIP archive"};
			}
			std::unique_ptr<zip_t, decltype(&zip_discard)> archive(rawArchive, &zip_discard);
			zip_error_fini(&zipError);
			if (requireRootRules)
			{
				zip_stat_t rules{};
				if (zip_stat(archive.get(), "rules.txt", 0, &rules) != 0)
					return {GraphicPackInstallError::InvalidArchive,
							"custom graphic-pack archive must contain rules.txt at its root"};
			}

			struct Entry
			{
				zip_uint64_t index;
				fs::path path;
				zip_uint64_t size;
				bool directory;
			};
			std::vector<Entry> entries;
			std::set<std::string> names;
			zip_uint64_t totalBytes{};
			const auto entryCount = zip_get_num_entries(archive.get(), 0);
			if (entryCount < 0 || static_cast<zip_uint64_t>(entryCount) > kMaximumGraphicPackEntries)
				return {GraphicPackInstallError::InvalidArchive, "graphic-pack archive has too many entries"};
			for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entryCount); ++index)
			{
				zip_stat_t stat{};
				if (zip_stat_index(archive.get(), index, 0, &stat) != 0 || !stat.name ||
					!IsSafeGraphicPackZipEntry(archive.get(), index))
					return {GraphicPackInstallError::InvalidArchive, "graphic-pack archive contains an invalid entry"};
				auto relative = NormalizeSaveArchivePath(stat.name);
				if (!relative || !names.emplace(relative->generic_string()).second)
					return {GraphicPackInstallError::InvalidArchive, "graphic-pack archive contains an unsafe or duplicate path"};
				const std::string_view rawName(stat.name);
				const bool directory = rawName.ends_with('/') || rawName.ends_with('\\');
				if (!directory)
				{
					if (stat.size > kMaximumGraphicPackFileSize ||
						totalBytes > kMaximumGraphicPackTotalSize - stat.size ||
						(stat.comp_size > 0 && stat.size / stat.comp_size > kMaximumGraphicPackCompressionRatio))
						return {GraphicPackInstallError::InvalidArchive, "graphic-pack archive exceeds extraction limits"};
					totalBytes += stat.size;
				}
				entries.push_back({index, std::move(*relative), stat.size, directory});
			}

			std::error_code ec;
			const bool targetExists = fs::exists(target, ec);
			if (ec)
				return {GraphicPackInstallError::IoFailure, ec.message()};
			if (targetExists && !replaceExisting)
				return {GraphicPackInstallError::ConfirmationRequired,
						"existing graphic packs must be replaced to continue"};
			fs::create_directories(target.parent_path(), ec);
			if (ec)
				return {GraphicPackInstallError::IoFailure, ec.message()};
			const auto staging = UniqueSiblingPath(target, "graphic-pack-staging");
			const auto backup = targetExists ? UniqueSiblingPath(target, "graphic-pack-backup") : fs::path{};
			if (staging.empty() || (targetExists && backup.empty()))
				return {GraphicPackInstallError::IoFailure, "unable to reserve graphic-pack transaction paths"};
			fs::create_directories(staging, ec);
			if (ec)
				return {GraphicPackInstallError::IoFailure, ec.message()};

			std::array<char, 1024 * 1024> copyBuffer{};
			std::uint64_t completed{};
			for (const auto& entry : entries)
			{
				if (cancelled && cancelled())
				{
					RemovePathQuietly(staging);
					return {GraphicPackInstallError::Cancelled, "graphic-pack installation was cancelled"};
				}
				const auto destination = staging / entry.path;
				if (entry.directory)
				{
					fs::create_directories(destination, ec);
					if (ec)
					{
						RemovePathQuietly(staging);
						return {GraphicPackInstallError::IoFailure, ec.message()};
					}
					continue;
				}
				fs::create_directories(destination.parent_path(), ec);
				if (ec)
				{
					RemovePathQuietly(staging);
					return {GraphicPackInstallError::IoFailure, ec.message()};
				}
				std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(
					zip_fopen_index(archive.get(), entry.index, 0), &zip_fclose);
				std::ofstream output(destination, std::ios::binary | std::ios::trunc);
				if (!file || !output)
				{
					RemovePathQuietly(staging);
					return {GraphicPackInstallError::IoFailure, "unable to create extracted graphic-pack file"};
				}
				zip_uint64_t remaining = entry.size;
				while (remaining > 0)
				{
					if (cancelled && cancelled())
					{
						RemovePathQuietly(staging);
						return {GraphicPackInstallError::Cancelled, "graphic-pack installation was cancelled"};
					}
					const auto request = std::min<zip_uint64_t>(copyBuffer.size(), remaining);
					const auto read = zip_fread(file.get(), copyBuffer.data(), request);
					if (read <= 0 || static_cast<zip_uint64_t>(read) > remaining)
					{
						RemovePathQuietly(staging);
						return {GraphicPackInstallError::InvalidArchive, "unable to extract graphic-pack archive"};
					}
					output.write(copyBuffer.data(), static_cast<std::streamsize>(read));
					if (!output)
					{
						RemovePathQuietly(staging);
						return {GraphicPackInstallError::IoFailure, "unable to write extracted graphic-pack file"};
					}
					remaining -= static_cast<zip_uint64_t>(read);
					completed += static_cast<std::uint64_t>(read);
					if (progress)
						progress({GraphicPackInstallPhase::Extracting, completed,
								  totalBytes, _pathToUtf8(entry.path)});
				}
			}
			if (!version.empty())
			{
				std::ofstream versionFile(staging / "version.txt", std::ios::binary | std::ios::trunc);
				versionFile << version;
				if (!versionFile)
				{
					RemovePathQuietly(staging);
					return {GraphicPackInstallError::IoFailure, "unable to write graphic-pack version"};
				}
			}
			if (targetExists)
			{
				fs::rename(target, backup, ec);
				if (ec)
				{
					RemovePathQuietly(staging);
					return {GraphicPackInstallError::IoFailure, ec.message()};
				}
			}
			fs::rename(staging, target, ec);
			if (ec)
			{
				if (targetExists)
				{
					std::error_code restoreError;
					fs::rename(backup, target, restoreError);
					if (restoreError)
						return {GraphicPackInstallError::IoFailure,
								fmt::format("{}; rollback failed: {}", ec.message(), restoreError.message())};
				}
				RemovePathQuietly(staging);
				return {GraphicPackInstallError::IoFailure, ec.message()};
			}
			if (transaction)
				*transaction = {target, backup, targetExists};
			else
				RemovePathQuietly(backup);
			return {};
		}

		GraphicPackInstallResult InstallGraphicPackFiles(
			const GraphicPackInstallRequest& request,
			GraphicPackInstallProgressHandler progress,
			GraphicPackInstallCancellationCheck cancelled,
			GraphicPackInstallTransaction* transaction)
		{
			GraphicPackDownloadBuffer download{{}, progress, cancelled};
			if (progress)
				progress({GraphicPackInstallPhase::Checking, 0, 0, {}});
			std::string archiveUrl = request.url;
			std::string releaseName;
			fs::path target;
			bool rootRules{};
			if (request.kind == GraphicPackInstallKind::Community)
			{
				auto result = DownloadGraphicPackUrl(
					"https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest",
					download, GraphicPackInstallPhase::Checking);
				if (!result)
					return result;
				rapidjson::Document document;
				document.Parse(reinterpret_cast<const char*>(download.bytes.data()), download.bytes.size());
				if (document.HasParseError() || !document.IsObject() ||
					!document.HasMember("name") || !document["name"].IsString() ||
					!document.HasMember("assets") || !document["assets"].IsArray())
					return {GraphicPackInstallError::ConnectionFailed, "community graphic-pack metadata is invalid"};
				releaseName = document["name"].GetString();
				for (const auto& asset : document["assets"].GetArray())
				{
					if (asset.IsObject() && asset.HasMember("browser_download_url") &&
						asset["browser_download_url"].IsString())
					{
						archiveUrl = asset["browser_download_url"].GetString();
						if (archiveUrl.starts_with("https://"))
							break;
						archiveUrl.clear();
					}
				}
				if (archiveUrl.empty())
					return {GraphicPackInstallError::ConnectionFailed, "community release has no HTTPS archive"};
				target = ActiveSettings::GetUserDataPath("graphicPacks/downloadedGraphicPacks");
				std::ifstream versionFile(target / "version.txt", std::ios::binary);
				std::string installedVersion;
				std::getline(versionFile, installedVersion);
				if (boost::iequals(installedVersion, releaseName))
					return {GraphicPackInstallError::None, {}, true};
				if (!installedVersion.empty() && !request.replaceExisting)
					return {GraphicPackInstallError::ConfirmationRequired,
							fmt::format("replace installed community packs {} with {}?", installedVersion, releaseName)};
			}
			else
			{
				if (!archiveUrl.starts_with("https://") || archiveUrl.size() > 4096)
					return {GraphicPackInstallError::InvalidUrl, "custom graphic-pack URL must be a valid HTTPS URL"};
				auto fileName = archiveUrl.substr(archiveUrl.find_last_of('/') + 1);
				if (const auto query = fileName.find_first_of("?#"); query != std::string::npos)
					fileName.resize(query);
				if (const auto extension = fileName.find_last_of('.'); extension != std::string::npos)
					fileName.resize(extension);
				if (fileName.empty())
					fileName = "NewCustomPack";
				auto folder = NormalizeSaveArchivePath(fileName);
				if (!folder || folder->has_parent_path())
					return {GraphicPackInstallError::InvalidUrl, "custom graphic-pack URL has an unsafe file name"};
				target = ActiveSettings::GetUserDataPath("graphicPacks/customGraphicPacks") / *folder;
				rootRules = true;
			}

			auto result = DownloadGraphicPackUrl(archiveUrl, download,
												 GraphicPackInstallPhase::Downloading);
			if (!result)
				return result;
			return ExtractGraphicPackArchive(download.bytes, target, request.replaceExisting,
											 rootRules, releaseName, std::move(progress), std::move(cancelled), transaction);
		}

		struct SaveMetadataStage
		{
			fs::path original;
			fs::path temporary;
			bool changed{};
		};

		template<typename Modifier>
		SaveOperationResult StageSaveMetadata(std::uint64_t titleId,
											  Modifier&& modifier, SaveMetadataStage& stage)
		{
			stage.original = SaveInfoPath(titleId);
			std::error_code ec;
			if (!fs::exists(stage.original, ec))
				return ec ? SaveOperationResult{SaveOperationError::MetadataFailure,
												ec.message()}
						  : SaveOperationResult{};
			if (ec || !fs::is_regular_file(stage.original, ec) || ec)
				return {SaveOperationError::MetadataFailure,
						"saveinfo.xml is not a regular file"};

			pugi::xml_document document;
			if (!document.load_file(stage.original.c_str()))
				return {SaveOperationError::MetadataFailure,
						"unable to parse saveinfo.xml"};
			auto info = document.child("info");
			if (!info)
				return {SaveOperationError::MetadataFailure,
						"saveinfo.xml is missing its info node"};
			stage.changed = std::forward<Modifier>(modifier)(info);
			if (!stage.changed)
				return {};

			stage.temporary = UniqueSiblingPath(stage.original, "staging");
			if (stage.temporary.empty() || !document.save_file(stage.temporary.c_str()))
			{
				RemovePathQuietly(stage.temporary);
				return {SaveOperationError::MetadataFailure,
						"unable to stage saveinfo.xml"};
			}
			return {};
		}

		SaveOperationResult CommitSaveMetadata(SaveMetadataStage& stage)
		{
			if (!stage.changed)
				return {};
			const auto backup = UniqueSiblingPath(stage.original, "backup");
			if (backup.empty())
				return {SaveOperationError::MetadataFailure,
						"unable to reserve saveinfo.xml backup path"};

			std::error_code ec;
			fs::rename(stage.original, backup, ec);
			if (ec)
				return {SaveOperationError::MetadataFailure, ec.message()};
			fs::rename(stage.temporary, stage.original, ec);
			if (ec)
			{
				std::error_code restoreError;
				fs::rename(backup, stage.original, restoreError);
				return {SaveOperationError::MetadataFailure,
						restoreError ? fmt::format("{}; restore failed: {}", ec.message(),
												   restoreError.message())
									 : ec.message()};
			}
			RemovePathQuietly(backup);
			stage.temporary.clear();
			return {};
		}

		pugi::xml_node FindSaveAccountNode(pugi::xml_node info,
										   std::uint32_t persistentId)
		{
			const auto id = fmt::format("{:08x}", persistentId);
			return info.find_child([&id](const pugi::xml_node& node) {
				return boost::iequals(node.attribute("persistentId").as_string(), id);
			});
		}

		SaveOperationResult BeginSaveMutation(std::unique_lock<std::mutex>& lock)
		{
			lock = CafeSaveList::AcquireOperationLock();
			if (CafeSaveList::IsScanning())
				return {SaveOperationError::Scanning,
						"save catalog scan is still running"};
			if (CafeSystem::IsTitleRunning())
				return {SaveOperationError::TitleRunning,
						"save data cannot be changed while a title is running"};
			return {};
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

		std::uint64_t ManagedContentFingerprint(const ManagedContentEntry& entry,
												const fs::path* identityPath = nullptr)
		{
			auto mix = [](std::uint64_t seed, std::uint64_t value) {
				return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
			};
			std::uint64_t result = std::hash<std::string>{}(
				_pathToUtf8(identityPath ? *identityPath : entry.path));
			result = mix(result, entry.locationUid);
			result = mix(result, entry.titleId);
			result = mix(result, entry.version);
			result = mix(result, static_cast<std::uint64_t>(entry.type));
			result = mix(result, static_cast<std::uint64_t>(entry.format));
			std::error_code error;
			const auto status = fs::symlink_status(entry.path, error);
			if (!error)
				result = mix(result, static_cast<std::uint64_t>(status.type()));
			error.clear();
			const auto timestamp = fs::last_write_time(entry.path, error);
			if (!error)
				result = mix(result, static_cast<std::uint64_t>(timestamp.time_since_epoch().count()));
			error.clear();
			if (fs::is_regular_file(entry.path, error) && !error)
			{
				const auto size = fs::file_size(entry.path, error);
				if (!error)
					result = mix(result, size);
			}
			error.clear();
			if (fs::is_directory(entry.path, error) && !error)
			{
				std::vector<std::string> identities;
				fs::recursive_directory_iterator iterator(entry.path,
														  fs::directory_options::skip_permission_denied, error),
					end;
				while (!error && iterator != end)
				{
					const auto relative = fs::relative(iterator->path(), entry.path, error);
					if (error)
						break;
					const auto status = iterator->symlink_status(error);
					if (error)
						break;
					std::string identity = _pathToUtf8(relative);
					identity.append("|").append(std::to_string(static_cast<unsigned>(status.type())));
					const auto modified = fs::last_write_time(iterator->path(), error);
					if (error)
						break;
					identity.append("|").append(std::to_string(modified.time_since_epoch().count()));
					if (status.type() == fs::file_type::regular)
					{
						const auto size = fs::file_size(iterator->path(), error);
						if (error)
							break;
						identity.append("|").append(std::to_string(size));
					}
					identities.push_back(std::move(identity));
					iterator.increment(error);
				}
				if (error)
					result = mix(result, std::numeric_limits<std::uint64_t>::max());
				else
				{
					std::ranges::sort(identities);
					for (const auto& identity : identities)
						result = mix(result, std::hash<std::string>{}(identity));
				}
			}
			return result;
		}

		ManagedContentPathValidation ValidateCatalogDeletePath(const fs::path& path)
		{
			std::vector<fs::path> roots{ActiveSettings::GetMlcPath()};
			roots.reserve(GetConfig().game_paths.size() + 1);
			for (const auto& root : GetConfig().game_paths)
				roots.push_back(_utf8ToPath(root));
			return ValidateManagedContentPath(path, roots);
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
			~TitleListLease()
			{
				CafeTitleList::ReleaseInternalList();
			}
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
			const std::uint64_t aocId = hasBase ? (titleId & ~0xff00000000ULL) | 0x0c00000000ULL : titleId;

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
					.fingerprint = ManagedContentFingerprint(TranslateManagedTitle(*title, false)),
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

			bool IsCancelled() const
			{
				return cancelled && cancelled();
			}

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
						(path + entry.path).c_str(), FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status));
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
					const bool result = count ? CountFiles(mountPath) : AddFiles(fmt::format("{:016x}_v{}/", title.GetAppTitleId(), title.GetAppTitleVersion()), mountPath);
					title.Unmount(mountPath);
					return result;
				} catch (...)
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

		class CafeTitleEventSubscription final : public Detail::TitleSubscriptionState,
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
				} catch (...)
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
						} catch (const std::exception& exception)
						{
							cemuLog_log(LogType::Force,
										"Unable to queue title catalog event: {}", exception.what());
						}
					},
					this);

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
						} catch (const std::exception& exception)
						{
							cemuLog_log(LogType::Force,
										"Unable to queue save catalog event: {}", exception.what());
						}
					},
					this);
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
					} catch (const std::exception& exception)
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
			case CafeSystem::PREPARE_STATUS_CODE::SUCCESS:
				return LaunchError::None;
			case CafeSystem::PREPARE_STATUS_CODE::CANCELLED:
				return LaunchError::PermissionDenied;
			case CafeSystem::PREPARE_STATUS_CODE::INVALID_RPX:
				return LaunchError::InvalidExecutable;
			case CafeSystem::PREPARE_STATUS_CODE::UNABLE_TO_MOUNT:
				return LaunchError::UnableToMount;
			case CafeSystem::PREPARE_STATUS_CODE::CEMOD_RUNTIME_BUSY:
				return LaunchError::CemodRuntimeBusy;
			}
			return LaunchError::InvalidExecutable;
		}

		TitleInstallKind TranslateInstallKind(TitleIdParser::TITLE_TYPE type)
		{
			switch (type)
			{
			case TitleIdParser::TITLE_TYPE::BASE_TITLE:
				return TitleInstallKind::Base;
			case TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO:
				return TitleInstallKind::Demo;
			case TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE:
				return TitleInstallKind::Update;
			case TitleIdParser::TITLE_TYPE::AOC:
				return TitleInstallKind::Dlc;
			case TitleIdParser::TITLE_TYPE::SYSTEM_TITLE:
			case TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE:
				return TitleInstallKind::SystemTitle;
			case TitleIdParser::TITLE_TYPE::SYSTEM_DATA:
				return TitleInstallKind::SystemData;
			default:
				return TitleInstallKind::Unknown;
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
				mix(ec ? std::numeric_limits<std::uint64_t>::max() : static_cast<std::uint64_t>(status.type()));
				if (!ec && fs::is_regular_file(status))
				{
					mix(fs::file_size(path, ec));
					if (ec)
						mix(std::numeric_limits<std::uint64_t>::max() - 1);
				}
				ec.clear();
				const auto modified = fs::last_write_time(path, ec);
				mix(ec ? std::numeric_limits<std::uint64_t>::max() - 2 : static_cast<std::uint64_t>(modified.time_since_epoch().count()));
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
							fs::is_symlink(rootStatus) ? "Symbolic links are not supported in title installs" : ec.message(),
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
									plan.availableBytes / 1024 / 1024),
						std::nullopt};
			return {TitleInstallError::None, {}, std::move(plan)};
		}

		TitleInstallPlanResult SafeBuildTitleInstallPlan(const fs::path& source,
														 bool checkAvailableSpace = true)
		{
			try
			{
				return BuildTitleInstallPlan(source, checkAvailableSpace);
			} catch (const std::exception& exception)
			{
				return {TitleInstallError::InvalidSource, exception.what(), std::nullopt};
			} catch (...)
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
				m_eventForwarder = std::make_shared<ApplicationEventForwarder>();
				m_eventForwarder->events = &m_events;
				DownloadManager::SetGameListRefreshCallback([forwarder = m_eventForwarder] {
					std::scoped_lock lock(forwarder->mutex);
					if (forwarder->events)
						forwarder->events->Publish({.type = EventType::GameListRefreshRequested});
				});
				cemuextend_hle::Cex2Host::Instance().SetTextInputWakeCallback(
					[forwarder = m_eventForwarder] {
						std::scoped_lock lock(forwarder->mutex);
						if (forwarder->events)
							forwarder->events->Publish(
								{.type = EventType::TextInputWakeRequested});
					});
			}

			~CafeEmulationBackend() override
			{
				cemuextend_hle::Cex2Host::Instance().SetTextInputWakeCallback({});
				DownloadManager::SetGameListRefreshCallback({});
				{
					std::scoped_lock lock(m_eventForwarder->mutex);
					m_eventForwarder->events = nullptr;
				}
				InputManager::instance().ClearEmulationContext(*this);
				CafeSystem::SetEventSink(nullptr);
			}

			bool IsTitleRunning() const override
			{
				std::shared_lock lock(m_inputLifecycleMutex);
				return m_inputAvailable && CafeSystem::IsTitleRunning();
			}

			PpcThreadsSnapshot CapturePpcThreads() override
			{
				struct SchedulerLock
				{
					SchedulerLock()
					{
						__OSLockScheduler();
					}
					~SchedulerLock()
					{
						__OSUnlockScheduler();
					}
				} schedulerLock;
				struct ActiveListLock
				{
					ActiveListLock()
					{
						srwlock_activeThreadList.LockWrite();
					}
					~ActiveListLock()
					{
						srwlock_activeThreadList.UnlockWrite();
					}
				} activeListLock;

				PpcThreadsSnapshot result;
				result.available = true;
				std::vector<std::pair<std::uint32_t, std::uint64_t>> currentThreads;
				currentThreads.reserve(static_cast<std::size_t>(
					std::max<sint32>(activeThreadCount, 0)));
				for (sint32 index = 0; index < activeThreadCount; ++index)
				{
					const auto address = activeThread[index];
					if (!coreinit::OSThreadQueue_IsThreadPointerValid(address))
						continue;
					const auto* thread = MEMPTR<OSThread_t>{address}.GetPtr();
					if (thread)
						currentThreads.emplace_back(address, PpcThreadIdentity(*thread));
				}
				if (currentThreads != m_diagnosticsThreads)
				{
					m_diagnosticsThreads = currentThreads;
					m_diagnosticsGeneration.fetch_add(1, std::memory_order_acq_rel);
				}
				std::erase_if(m_diagnosticsSuspensions, [&currentThreads](const auto& entry) {
					return std::ranges::none_of(currentThreads, [&entry](const auto& active) {
						return active.first == entry.first && active.second == entry.second.identity;
					});
				});
				result.generation = m_diagnosticsGeneration.load(std::memory_order_acquire);
				result.threads.reserve(static_cast<std::size_t>(
					std::max<sint32>(activeThreadCount, 0)));
				for (sint32 index = 0; index < activeThreadCount; ++index)
				{
					const auto address = activeThread[index];
					if (!coreinit::OSThreadQueue_IsThreadPointerValid(address))
						continue;
					auto* thread = MEMPTR<OSThread_t>{address}.GetPtr();
					if (!thread)
						continue;

					PpcThreadSnapshot item;
					item.address = address;
					item.identity = PpcThreadIdentity(*thread);
					item.entryPoint = thread->entrypoint.GetMPTR();
					item.stackLow = thread->stackEnd.GetMPTR();
					item.stackHigh = thread->stackBase.GetMPTR();
					item.instructionPointer = thread->context.srr0;
					item.linkRegister = _swapEndianU32(thread->context.lr);
					if (thread->suspendCounter != 0)
						item.state = PpcThreadState::Suspended;
					else
					{
						switch (thread->state)
						{
						case OSThread_t::THREAD_STATE::STATE_NONE:
							item.state = PpcThreadState::None;
							break;
						case OSThread_t::THREAD_STATE::STATE_READY:
							item.state = PpcThreadState::Ready;
							break;
						case OSThread_t::THREAD_STATE::STATE_RUNNING:
							item.state = PpcThreadState::Running;
							break;
						case OSThread_t::THREAD_STATE::STATE_WAITING:
							item.state = PpcThreadState::Waiting;
							break;
						case OSThread_t::THREAD_STATE::STATE_MORIBUND:
							item.state = PpcThreadState::Moribund;
							break;
						default:
							item.state = PpcThreadState::Unknown;
							break;
						}
					}
					item.requestedAffinity = thread->attr & 7;
					item.effectiveAffinity = thread->context.affinity;
					item.basePriority = thread->basePriority;
					item.effectivePriority = thread->effectivePriority;
					item.wakeUpTime = thread->wakeUpTime;
					item.totalCycles = thread->totalCycles;
					if (!thread->threadName.IsNull())
					{
						const char* name = thread->threadName.GetPtr();
						if (name)
						{
							std::size_t length{};
							while (length < 128 && name[length] != '\0')
								++length;
							item.name.assign(name, length);
						}
					}
					item.gpr3 = _swapEndianU32(thread->context.gpr[3]);
					item.gpr4 = _swapEndianU32(thread->context.gpr[4]);
					item.gpr5 = _swapEndianU32(thread->context.gpr[5]);
					item.gpr6 = _swapEndianU32(thread->context.gpr[6]);
					item.gpr7 = _swapEndianU32(thread->context.gpr[7]);
					item.cancelRequested = thread->requestFlags & OSThread_t::REQUEST_FLAG_CANCEL;
					if (const auto owned = m_diagnosticsSuspensions.find(address);
						owned != m_diagnosticsSuspensions.end() && owned->second.identity == item.identity &&
						owned->second.expectedCounter == static_cast<std::int32_t>(thread->suspendCounter))
						item.suspensionOwnedByFacade = true;
					if (coreinit::OSMutex* mutex = thread->waitingForMutex)
					{
						item.waitingMutex = memory_getVirtualOffsetFromPointer(mutex);
						item.mutexOwner = mutex->owner.GetMPTR();
						item.mutexLockCount = mutex->lockCount;
					}
					result.threads.emplace_back(std::move(item));
				}
				return result;
			}

			PpcThreadCommandResult ExecutePpcThreadCommand(
				const PpcThreadCommandRequest& request) override
			{
				if (request.generation == 0 ||
					request.generation != m_diagnosticsGeneration.load(std::memory_order_acquire))
					return {false, "the PPC thread snapshot is stale; refresh before retrying"};
				if (std::ranges::none_of(m_diagnosticsThreads, [&request](const auto& active) {
						return active.first == request.threadAddress && active.second == request.threadIdentity;
					}))
					return {false, "the selected PPC thread was not part of this diagnostic snapshot"};
				if (request.command == PpcThreadCommand::AdjustPriority &&
					(request.priorityDelta < -5 || request.priorityDelta > 5 ||
					 request.priorityDelta == 0))
					return {false, "priorityDelta must be between -5 and 5 and cannot be zero"};

				struct SchedulerLock
				{
					SchedulerLock()
					{
						__OSLockScheduler();
					}
					~SchedulerLock()
					{
						__OSUnlockScheduler();
					}
				} schedulerLock;
				if (!coreinit::OSThreadQueue_IsThreadPointerValid(request.threadAddress))
					return {false, "the selected PPC thread address is no longer valid"};
				auto* thread = MEMPTR<OSThread_t>{request.threadAddress}.GetPtr();
				if (!thread || !coreinit::__OSIsThreadActive(thread) ||
					PpcThreadIdentity(*thread) != request.threadIdentity ||
					thread->state == OSThread_t::THREAD_STATE::STATE_NONE ||
					thread->state == OSThread_t::THREAD_STATE::STATE_MORIBUND)
					return {false, "the selected PPC thread is no longer active"};

				switch (request.command)
				{
				case PpcThreadCommand::Suspend:
					if (thread->suspendCounter > 0)
						return {false, "the selected PPC thread is already suspended"};
					coreinit::__OSSuspendThreadNolock(thread);
					m_diagnosticsSuspensions[request.threadAddress] = {
						request.threadIdentity, static_cast<std::int32_t>(thread->suspendCounter)};
					break;
				case PpcThreadCommand::Resume:
				{
					const auto owned = m_diagnosticsSuspensions.find(request.threadAddress);
					if (owned == m_diagnosticsSuspensions.end() ||
						owned->second.identity != request.threadIdentity ||
						owned->second.expectedCounter != static_cast<std::int32_t>(thread->suspendCounter))
						return {false, "this window does not own the selected thread suspension"};
					coreinit::__OSResumeThreadInternal(thread, 1);
					m_diagnosticsSuspensions.erase(owned);
					break;
				}
				case PpcThreadCommand::AdjustPriority:
					thread->basePriority = std::clamp<std::int32_t>(
						static_cast<std::int32_t>(thread->basePriority) + request.priorityDelta, 0, 31);
					coreinit::__OSUpdateThreadEffectivePriority(thread);
					break;
				}
				return {true, {}};
			}

			FrontendSettingsSnapshot GetFrontendSettings() const override
			{
				std::scoped_lock transactionLock(m_frontendSettingsTransactionMutex);
				FrontendSettingsSnapshot snapshot;
				std::string fingerprint;
				for (const auto& path : GetConfig().game_paths)
				{
					auto displayPath = _utf8ToPath(path);
					std::error_code pathError;
					if (displayPath.is_relative())
						displayPath = fs::absolute(displayPath, pathError);
					if (!pathError && fs::exists(displayPath, pathError) && !pathError)
					{
						auto canonical = fs::canonical(displayPath, pathError);
						if (!pathError)
							displayPath = std::move(canonical);
					}
					snapshot.gamePaths.emplace_back(std::move(displayPath));
					fingerprint.append(path).push_back('\0');
				}
				snapshot.startFullscreen = GetConfig().frontend.start_fullscreen.GetValue();
				snapshot.openPad = GetConfig().frontend.open_pad.GetValue();
				snapshot.checkUpdates = GetConfig().frontend.check_updates.GetValue();
				snapshot.saveScreenshots = GetConfig().frontend.save_screenshots.GetValue();
#if BOOST_OS_LINUX
				snapshot.updateChecksSupported = std::getenv("APPIMAGE") != nullptr;
#else
				snapshot.updateChecksSupported = true;
#endif
				snapshot.portableMode = ActiveSettings::IsPortableMode();
				snapshot.titleRunning = IsTitleRunning();
				snapshot.setupCompleted = GetConfig().frontend.setup_completed.GetValue();
				snapshot.fullscreenOverride = LaunchSettings::FullscreenEnabled();
				fingerprint.append(snapshot.startFullscreen ? "1" : "0");
				fingerprint.append(snapshot.openPad ? "1" : "0");
				fingerprint.append(snapshot.checkUpdates ? "1" : "0");
				fingerprint.append(snapshot.saveScreenshots ? "1" : "0");
				fingerprint.append(snapshot.setupCompleted ? "1" : "0");
				{
					std::scoped_lock stateLock(m_frontendSettingsStateMutex);
					if (m_frontendSettingsFingerprint.empty())
						m_frontendSettingsFingerprint = fingerprint;
					else if (m_frontendSettingsFingerprint != fingerprint)
					{
						m_frontendSettingsFingerprint = fingerprint;
						++m_frontendSettingsRevision;
					}
					snapshot.revision = m_frontendSettingsRevision;
				}
				return snapshot;
			}

			FrontendSettingsResult ApplyFrontendSettings(
				const FrontendSettingsUpdate& update) override
			{
				std::scoped_lock transactionLock(m_frontendSettingsTransactionMutex);
				auto current = GetFrontendSettings();
				if (current.titleRunning)
					return {FrontendSettingsError::TitleRunning, std::move(current),
							"frontend settings cannot be changed while a title is running"};
				if (update.expectedRevision != current.revision)
					return {FrontendSettingsError::Conflict, std::move(current),
							"frontend settings changed in another window"};
				if (current.fullscreenOverride && update.startFullscreen != current.startFullscreen)
					return {FrontendSettingsError::FullscreenOverride, std::move(current),
							"fullscreen startup is controlled by the command line"};
				if (!current.updateChecksSupported && update.checkUpdates != current.checkUpdates)
					return {FrontendSettingsError::UpdateUnsupported, std::move(current),
							"automatic update checks are unavailable in this package"};
				if (update.gamePaths.size() > 64)
					return {FrontendSettingsError::InvalidPath, std::move(current),
							"at most 64 game paths are supported"};

				std::vector<fs::path> normalizedPaths;
				std::set<std::string> normalizedKeys;
				std::set<std::string> existingPathKeys;
				for (const auto& path : current.gamePaths)
				{
					auto key = _pathToUtf8(path.lexically_normal());
#if BOOST_OS_WINDOWS
					boost::to_lower(key);
#endif
					existingPathKeys.emplace(std::move(key));
				}
				for (const auto& path : update.gamePaths)
				{
					if (!path.is_absolute() || _pathToUtf8(path).find('\0') != std::string::npos)
						return {FrontendSettingsError::InvalidPath, std::move(current),
								"game paths must be absolute paths"};
					std::error_code ec;
					auto requestedKey = _pathToUtf8(path.lexically_normal());
#if BOOST_OS_WINDOWS
					boost::to_lower(requestedKey);
#endif
					auto normalized = fs::canonical(path, ec);
					if (ec || !fs::is_directory(normalized, ec) || ec)
					{
						if (existingPathKeys.contains(requestedKey))
						{
							if (normalizedKeys.emplace(std::move(requestedKey)).second)
								normalizedPaths.emplace_back(path.lexically_normal());
							continue;
						}
						return {FrontendSettingsError::InvalidPath, std::move(current),
								fmt::format("game path is not an accessible directory: {}", _pathToUtf8(path))};
					}
					fs::directory_iterator readable(normalized, ec);
					if (ec)
						return {FrontendSettingsError::InvalidPath, std::move(current),
								fmt::format("game path cannot be read: {}", _pathToUtf8(path))};
					auto key = _pathToUtf8(normalized.lexically_normal());
#if BOOST_OS_WINDOWS
					boost::to_lower(key);
#endif
					if (normalizedKeys.emplace(std::move(key)).second)
						normalizedPaths.emplace_back(std::move(normalized));
				}
				if (update.completeSetup && !current.setupCompleted)
				{
					auto storage = EnsureDefaultMlcFiles(ActiveSettings::GetMlcPath(),
														 ListAccountCountries(), current);
					if (!storage)
						return storage;
				}

				auto& config = GetConfig();
				const auto oldPaths = config.game_paths;
				const bool oldFullscreen = config.frontend.start_fullscreen.GetValue();
				const bool oldPad = config.frontend.open_pad.GetValue();
				const bool oldUpdates = config.frontend.check_updates.GetValue();
				const bool oldSaveScreenshots = config.frontend.save_screenshots.GetValue();
				const bool oldSetup = config.frontend.setup_completed.GetValue();
				auto restore = [&] {
					config.game_paths = oldPaths;
					config.frontend.start_fullscreen = oldFullscreen;
					config.frontend.open_pad = oldPad;
					config.frontend.check_updates = oldUpdates;
					config.frontend.save_screenshots = oldSaveScreenshots;
					config.frontend.setup_completed = oldSetup;
					CafeTitleList::ClearScanPaths();
					for (const auto& path : oldPaths)
						CafeTitleList::AddScanPath(_utf8ToPath(path));
					(void)GetConfigHandle().Save();
				};

				try
				{
					config.game_paths.clear();
					for (const auto& path : normalizedPaths)
						config.game_paths.emplace_back(_pathToUtf8(path));
					config.frontend.start_fullscreen = update.startFullscreen;
					config.frontend.open_pad = update.openPad;
					config.frontend.check_updates = update.checkUpdates;
					config.frontend.save_screenshots = update.saveScreenshots;
					if (update.completeSetup)
						config.frontend.setup_completed = true;
					CafeTitleList::ClearScanPaths();
					for (const auto& path : normalizedPaths)
						CafeTitleList::AddScanPath(path);
					if (!GetConfigHandle().Save())
					{
						restore();
						return {FrontendSettingsError::SaveFailed, GetFrontendSettings(),
								"unable to save frontend settings"};
					}
					CafeTitleList::Refresh();
				} catch (const std::exception& error)
				{
					restore();
					return {FrontendSettingsError::SaveFailed, GetFrontendSettings(), error.what()};
				} catch (...)
				{
					restore();
					return {FrontendSettingsError::SaveFailed, GetFrontendSettings(),
							"unknown error while applying frontend settings"};
				}
				return {FrontendSettingsError::None, GetFrontendSettings(), {}};
			}

			HotkeySettingsModel GetHotkeySettings() const override
			{
				std::scoped_lock hotkeyLock(m_hotkeySettingsMutex);
				HotkeySettingsModel model;
				model.revision = m_hotkeySettingsRevision;
				const auto& settings = GetConfig().frontend.hotkeys;
				if (settings.controller_modifier >= 0)
					model.controllerModifier = static_cast<std::uint32_t>(
						settings.controller_modifier);
				auto append = [&model](HotkeyAction action,
									   const FrontendHotkeyBindingConfig& binding) {
					model.bindings.push_back({
						.action = action,
						.keyboardUsage = binding.keyboard_usage,
						.keyboardModifiers = binding.keyboard_modifiers,
						.controllerButton = binding.controller_button >= 0 ? std::optional<std::uint32_t>(binding.controller_button) : std::nullopt,
					});
				};
				append(HotkeyAction::ToggleFullscreen, settings.toggle_fullscreen);
				append(HotkeyAction::ToggleFullscreenAlternative,
					   settings.toggle_fullscreen_alternative);
				append(HotkeyAction::ExitFullscreen, settings.exit_fullscreen);
				append(HotkeyAction::TakeScreenshot, settings.take_screenshot);
				append(HotkeyAction::ToggleFastForward, settings.toggle_fast_forward);
#ifdef CEMU_DEBUG_ASSERT
				append(HotkeyAction::EndEmulation, settings.end_emulation);
#endif
				append(HotkeyAction::ExitApplication, settings.exit_application);
				std::scoped_lock inputLock(m_inputSettingsMutex);
				InputManager::instance().RunOnInputThread([this, &model] {
					const auto emulated = InputManager::instance().get_controller(0);
					if (!emulated)
						return;
					const auto controllers = emulated->copy_controllers();
					if (controllers.empty())
						return;
					model.controller = HotkeyController{
						.token = TokenForInputController(controllers.front()),
						.displayName = controllers.front()->display_name(),
					};
					if (model.controllerModifier)
						model.controllerModifierLabel = controllers.front()->get_button_name(
							*model.controllerModifier);
					for (auto& binding : model.bindings)
						if (binding.controllerButton)
							binding.controllerLabel = controllers.front()->get_button_name(
								*binding.controllerButton);
				});
				return model;
			}

			HotkeySettingsResult ApplyHotkeySettings(
				const HotkeySettingsUpdate& update) override
			{
				static_assert(FrontendHotkeyBindingConfig::kControllerButtonCount == kButtonMAX);
				std::scoped_lock hotkeyLock(m_hotkeySettingsMutex);
				if (update.revision != m_hotkeySettingsRevision)
					return {HotkeySettingsError::Conflict, GetHotkeySettings(),
							"hotkey settings changed in another window"};
#ifdef CEMU_DEBUG_ASSERT
				constexpr bool endEmulationAvailable = true;
#else
				constexpr bool endEmulationAvailable = false;
#endif
				std::string validationDiagnostic;
				if (const auto validation = ValidateHotkeySettingsUpdate(update,
																		 endEmulationAvailable, validationDiagnostic);
					validation != HotkeySettingsError::None)
					return {validation, GetHotkeySettings(), std::move(validationDiagnostic)};

				auto configLock = GetConfigHandle().Lock();
				auto& config = GetConfig().frontend.hotkeys;
				const auto previous = config;
				config.controller_modifier = update.controllerModifier ? static_cast<std::int16_t>(*update.controllerModifier) : -1;
				auto assign = [&config](const HotkeyBinding& binding) {
					FrontendHotkeyBindingConfig* target{};
					switch (binding.action)
					{
					case HotkeyAction::ToggleFullscreen:
						target = &config.toggle_fullscreen;
						break;
					case HotkeyAction::ToggleFullscreenAlternative:
						target = &config.toggle_fullscreen_alternative;
						break;
					case HotkeyAction::ExitFullscreen:
						target = &config.exit_fullscreen;
						break;
					case HotkeyAction::TakeScreenshot:
						target = &config.take_screenshot;
						break;
					case HotkeyAction::ToggleFastForward:
						target = &config.toggle_fast_forward;
						break;
					case HotkeyAction::EndEmulation:
						target = &config.end_emulation;
						break;
					case HotkeyAction::ExitApplication:
						target = &config.exit_application;
						break;
					}
					*target = {binding.keyboardUsage, binding.keyboardModifiers,
							   binding.controllerButton ? static_cast<std::int16_t>(
															  *binding.controllerButton)
														: static_cast<std::int16_t>(-1)};
				};
				for (const auto& binding : update.bindings)
					assign(binding);
				if (!GetConfigHandle().Save())
				{
					config = previous;
					return {HotkeySettingsError::SaveFailed, GetHotkeySettings(),
							"hotkey settings could not be saved"};
				}
				++m_hotkeySettingsRevision;
				return {HotkeySettingsError::None, GetHotkeySettings(), {}};
			}

			InputSettingsModel GetInputSettings() const override
			{
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsModel model;
				InputManager::instance().RunOnInputThread([this, &model] {
					model.profiles = InputManager::instance().get_profiles();
					for (const auto api : {
							 InputAPI::Keyboard,
							 InputAPI::SDLController,
#if BOOST_OS_WINDOWS
							 InputAPI::XInput,
							 InputAPI::DirectInput,
#endif
							 InputAPI::DSUClient,
							 InputAPI::GameCube,
#ifdef SUPPORTS_WIIMOTE
							 InputAPI::Wiimote,
#endif
						 })
						if (InputManager::instance().is_api_available(api))
							model.availableApis.emplace_back(InputAPI::to_string(api));
					for (std::uint32_t player = 0; player < InputManager::kMaxController; ++player)
					{
						InputPlayerInfo info;
						info.player = player;
						info.gameProfileLocked = InputManager::instance().is_gameprofile_set(player);
						auto emulated = InputManager::instance().get_controller(player);
						if (!emulated)
						{
							model.players.emplace_back(std::move(info));
							continue;
						}
						info.type = TranslateEmulatedControllerType(emulated->type());
						info.profileName = emulated->get_profile_name();
						for (const auto& controller : emulated->copy_controllers())
						{
							const auto token = TokenForInputController(controller);
							const auto settings = controller->get_settings();
							PhysicalControllerInfo physical{
								.token = token,
								.api = std::string(controller->api_name()),
								.displayName = controller->display_name(),
								.connected = controller->is_connected(),
								.hasBattery = controller->has_battery(),
								.lowBattery = controller->has_low_battery(),
								.hasMotion = controller->has_motion(),
								.hasRumble = controller->has_rumble(),
								.settings = {
									.axis = {settings.axis.deadzone, settings.axis.range},
									.rotation = {settings.rotation.deadzone, settings.rotation.range},
									.trigger = {settings.trigger.deadzone, settings.trigger.range},
									.rumble = settings.rumble,
									.motion = settings.motion,
								},
							};
#ifdef SUPPORTS_WIIMOTE
							if (const auto wiimote = std::dynamic_pointer_cast<NativeWiimoteController>(controller))
							{
								physical.settings.packetDelay = wiimote->get_packet_delay();
								switch (wiimote->get_extension())
								{
								case NativeWiimoteController::Nunchuck:
									physical.wiimoteExtension = "nunchuck";
									break;
								case NativeWiimoteController::Classic:
									physical.wiimoteExtension = "classic";
									break;
								case NativeWiimoteController::MotionPlus:
									physical.wiimoteExtension = "motionPlus";
									break;
								default:
									physical.wiimoteExtension = "none";
									break;
								}
							}
#endif
							info.controllers.emplace_back(std::move(physical));
						}
						for (std::uint64_t mapping = 1;
							 mapping < emulated->get_highest_mapping_id(); ++mapping)
						{
							auto controller = emulated->get_mapping_controller(mapping);
							info.mappings.push_back({
								.mappingId = mapping,
								.label = InputMappingLabel(*emulated, mapping),
								.binding = emulated->get_mapping_name(mapping),
								.controllerToken = controller ? std::optional<std::uint64_t>(TokenForInputController(controller)) : std::nullopt,
							});
						}
						model.players.emplace_back(std::move(info));
					}
				});
				std::string fingerprint;
				for (const auto& player : model.players)
				{
					fingerprint += fmt::format("{}:{}:{}:{}|", player.player,
											   static_cast<unsigned>(player.type), player.gameProfileLocked,
											   player.profileName);
					for (const auto& controller : player.controllers)
						fingerprint += fmt::format("{}:{}:{}:{}:{}:{}|", controller.token,
												   controller.connected, controller.settings.axis.deadzone,
												   controller.settings.axis.range, controller.settings.rumble,
												   controller.settings.motion);
					for (const auto& mapping : player.mappings)
						fingerprint += fmt::format("{}:{}:{}|", mapping.mappingId,
												   mapping.controllerToken.value_or(0), mapping.binding);
				}
				for (const auto& profile : model.profiles)
					fingerprint += "p:" + profile + "|";
				if (m_inputSettingsFingerprint.empty())
				{
					m_inputSettingsFingerprint = fingerprint;
					m_inputSettingsFingerprintGeneration = m_inputSettingsGeneration;
				}
				else if (m_inputSettingsFingerprint != fingerprint)
				{
					if (m_inputSettingsFingerprintGeneration == m_inputSettingsGeneration)
						++m_inputSettingsGeneration;
					m_inputSettingsFingerprint = std::move(fingerprint);
					m_inputSettingsFingerprintGeneration = m_inputSettingsGeneration;
				}
				model.generation = m_inputSettingsGeneration;
				return model;
			}

			InputDeviceEnumerationResult EnumerateInputDevices(
				std::string_view apiName) override
			{
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputDeviceEnumerationResult result;
				InputManager::instance().RunOnInputThread([this, &result, apiName = std::string(apiName)] {
					PruneUnassignedInputTokens();
					InputAPI::Type api;
					try
					{
						api = InputAPI::from_string(apiName);
					} catch (...)
					{
						result.error = InputSettingsError::UnsupportedApi;
						result.diagnostic = "input API is unknown";
						return;
					}
					if (api == InputAPI::WGIGamepad || api == InputAPI::WGIRawController ||
						!InputManager::instance().is_api_available(api))
					{
						result.error = InputSettingsError::UnsupportedApi;
						result.diagnostic = "input API is unavailable";
						return;
					}
					auto provider = InputManager::instance().get_api_provider(api);
					if (!provider)
					{
						result.error = InputSettingsError::OperationFailed;
						result.diagnostic = "input provider is unavailable";
						return;
					}
					for (auto& controller : provider->get_controllers())
						if (controller)
							result.devices.push_back({TokenForInputController(controller),
													  std::string(controller->api_name()), controller->display_name(),
													  controller->is_connected()});
				});
				return result;
			}

			InputSettingsResult SetEmulatedController(std::uint32_t player,
													  EmulatedControllerType type, bool preserveDevices) override
			{
				if (player >= InputManager::kMaxController)
					return {InputSettingsError::InvalidPlayer, "player index is out of range"};
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					if (InputManager::instance().is_gameprofile_set(player))
					{
						result = {InputSettingsError::ProfileLocked,
								  "this player is controlled by the running game's profile"};
						return;
					}
					if (type == EmulatedControllerType::Disabled)
					{
						InputManager::instance().delete_controller(player, false);
						++m_inputSettingsGeneration;
						return;
					}
					const auto nativeType = TranslateEmulatedControllerType(type);
					if (!nativeType)
					{
						result = {InputSettingsError::UnsupportedType,
								  "emulated controller type is unsupported"};
						return;
					}
					const auto existing = InputManager::instance().get_controller(player);
					const auto [vpadCount, wpadCount] = InputManager::instance().get_controller_count();
					if ((*nativeType == EmulatedController::VPAD &&
						 (!existing || existing->type() != EmulatedController::VPAD) &&
						 vpadCount >= InputManager::kMaxVPADControllers) ||
						(*nativeType != EmulatedController::VPAD &&
						 (!existing || existing->type() == EmulatedController::VPAD) &&
						 wpadCount >= InputManager::kMaxWPADControllers))
					{
						result = {InputSettingsError::UnsupportedType,
								  "all slots for this emulated controller type are already in use"};
						return;
					}
					if (!preserveDevices)
						InputManager::instance().delete_controller(player, false);
					if (!InputManager::instance().set_controller(player, *nativeType))
						result = {InputSettingsError::UnsupportedType,
								  "the requested emulated controller slot is unavailable"};
					else
						++m_inputSettingsGeneration;
				});
				return result;
			}

			InputSettingsResult AddInputDevice(std::uint32_t player,
											   std::uint64_t token) override
			{
				if (player >= InputManager::kMaxController)
					return {InputSettingsError::InvalidPlayer, "player index is out of range"};
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					auto emulated = InputManager::instance().get_controller(player);
					auto controller = InputControllerForToken(token);
					if (!emulated || !controller)
					{
						result = {InputSettingsError::DeviceNotFound,
								  "emulated controller or physical device was not found"};
						return;
					}
					const auto controllers = emulated->copy_controllers();
					if (std::ranges::find(controllers, controller) == controllers.end())
					{
						emulated->add_controller(controller);
						emulated->set_default_mapping(controller);
						++m_inputSettingsGeneration;
					}
				});
				return result;
			}

			InputSettingsResult RemoveInputDevice(std::uint32_t player,
												  std::uint64_t token) override
			{
				if (player >= InputManager::kMaxController)
					return {InputSettingsError::InvalidPlayer, "player index is out of range"};
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					auto emulated = InputManager::instance().get_controller(player);
					auto controller = InputControllerForToken(token);
					const auto controllers = emulated ? emulated->copy_controllers() : std::vector<std::shared_ptr<ControllerBase>>{};
					if (!emulated || !controller ||
						std::ranges::find(controllers, controller) == controllers.end())
					{
						result = {InputSettingsError::DeviceNotFound,
								  "physical device is not assigned to this player"};
						return;
					}
					emulated->remove_controller(controller);
					++m_inputSettingsGeneration;
				});
				return result;
			}

			InputSettingsResult ConnectInputDevice(std::uint64_t token) override
			{
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					auto controller = InputControllerForToken(token);
					if (!controller)
						result = {InputSettingsError::DeviceNotFound,
								  "physical device was not found"};
					else if (!controller->connect())
						result = {InputSettingsError::DeviceNotFound,
								  "physical device could not be connected"};
					else
						++m_inputSettingsGeneration;
				});
				return result;
			}

			std::optional<CapturedInputButton> CaptureInputButton(std::uint64_t token) override
			{
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				std::optional<CapturedInputButton> captured;
				InputManager::instance().RunOnInputThread([&] {
					auto controller = InputControllerForToken(token);
					if (!controller)
						return;
					const auto state = controller->raw_state();
					const auto buttons = state.buttons.GetButtonList();
					std::optional<CapturedInputButton> active;
					if (!buttons.empty())
					{
						active = CapturedInputButton{buttons.front(),
													 controller->get_button_name(buttons.front())};
					}
					auto axisValue = [&state](std::uint64_t button) {
						switch (button)
						{
						case kAxisXP:
							return state.axis.x;
						case kAxisXN:
							return -state.axis.x;
						case kAxisYP:
							return state.axis.y;
						case kAxisYN:
							return -state.axis.y;
						case kRotationXP:
							return state.rotation.x;
						case kRotationXN:
							return -state.rotation.x;
						case kRotationYP:
							return state.rotation.y;
						case kRotationYN:
							return -state.rotation.y;
						case kTriggerXP:
							return state.trigger.x;
						case kTriggerXN:
							return -state.trigger.x;
						case kTriggerYP:
							return state.trigger.y;
						case kTriggerYN:
							return -state.trigger.y;
						default:
							return 0.0F;
						}
					};
					for (std::uint64_t button = kButtonAxisStart;
						 !active && button < kButtonMAX; ++button)
						if (axisValue(button) >= 0.33F)
						{
							active = CapturedInputButton{button,
														 controller->get_button_name(button)};
						}
					if (!active)
					{
						m_armedInputCaptures.emplace(token);
						return;
					}
					if (m_armedInputCaptures.erase(token))
						captured = std::move(active);
				});
				return captured;
			}

			InputSettingsResult SetInputMapping(std::uint32_t player,
												std::uint64_t mappingId, std::uint64_t controllerToken,
												std::uint64_t buttonId) override
			{
				if (player >= InputManager::kMaxController)
					return {InputSettingsError::InvalidPlayer, "player index is out of range"};
				if (buttonId >= kButtonMAX)
					return {InputSettingsError::InvalidMapping, "physical button is out of range"};
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					auto emulated = InputManager::instance().get_controller(player);
					auto controller = InputControllerForToken(controllerToken);
					const auto controllers = emulated ? emulated->copy_controllers() : std::vector<std::shared_ptr<ControllerBase>>{};
					if (!emulated || !controller ||
						std::ranges::find(controllers, controller) == controllers.end())
					{
						result = {InputSettingsError::DeviceNotFound,
								  "physical device is not assigned to this player"};
						return;
					}
					if (mappingId == 0 || mappingId >= emulated->get_highest_mapping_id())
					{
						result = {InputSettingsError::InvalidMapping,
								  "emulated mapping is out of range"};
						return;
					}
					emulated->set_mapping(mappingId, controller, buttonId);
					++m_inputSettingsGeneration;
				});
				return result;
			}

			InputSettingsResult ClearInputMapping(std::uint32_t player,
												  std::optional<std::uint64_t> mappingId) override
			{
				if (player >= InputManager::kMaxController)
					return {InputSettingsError::InvalidPlayer, "player index is out of range"};
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					auto emulated = InputManager::instance().get_controller(player);
					if (!emulated)
					{
						result = {InputSettingsError::InvalidPlayer,
								  "player has no emulated controller"};
						return;
					}
					if (mappingId)
					{
						if (*mappingId == 0 || *mappingId >= emulated->get_highest_mapping_id())
						{
							result = {InputSettingsError::InvalidMapping,
									  "emulated mapping is out of range"};
							return;
						}
						emulated->delete_mapping(*mappingId);
					}
					else
						emulated->clear_mappings();
					++m_inputSettingsGeneration;
				});
				return result;
			}

			InputSettingsResult SetPhysicalControllerSettings(std::uint64_t token,
															  const PhysicalControllerSettings& requested) override
			{
				auto validAxis = [](const ControllerAxisSettings& value) {
					return std::isfinite(value.deadzone) && std::isfinite(value.range) &&
						   value.deadzone >= 0.0F && value.deadzone <= 1.0F &&
						   value.range >= 0.5F && value.range <= 2.0F;
				};
				if (!validAxis(requested.axis) || !validAxis(requested.rotation) ||
					!validAxis(requested.trigger) || !std::isfinite(requested.rumble) ||
					requested.rumble < 0.0F || requested.rumble > 1.0F ||
					(requested.packetDelay && (*requested.packetDelay < 1 || *requested.packetDelay > 100)))
					return {InputSettingsError::InvalidSettings,
							"controller settings are outside their supported range"};
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					auto controller = InputControllerForToken(token);
					if (!controller)
					{
						result = {InputSettingsError::DeviceNotFound,
								  "physical device was not found"};
						return;
					}
					if ((requested.motion && !controller->has_motion()) ||
						(requested.rumble > 0.0F && !controller->has_rumble()))
					{
						result = {InputSettingsError::InvalidSettings,
								  "the physical device does not support the requested feature"};
						return;
					}
#ifdef SUPPORTS_WIIMOTE
					auto wiimote = std::dynamic_pointer_cast<NativeWiimoteController>(controller);
					if (requested.packetDelay && !wiimote)
					{
						result = {InputSettingsError::InvalidSettings,
								  "packet delay is only supported by native Wiimotes"};
						return;
					}
#else
					if (requested.packetDelay)
					{
						result = {InputSettingsError::InvalidSettings,
								  "native Wiimotes are unavailable in this build"};
						return;
					}
#endif
					ControllerBase::Settings settings;
					settings.axis.deadzone = requested.axis.deadzone;
					settings.axis.range = requested.axis.range;
					settings.rotation.deadzone = requested.rotation.deadzone;
					settings.rotation.range = requested.rotation.range;
					settings.trigger.deadzone = requested.trigger.deadzone;
					settings.trigger.range = requested.trigger.range;
					settings.rumble = requested.rumble;
					settings.motion = requested.motion;
					controller->set_settings(settings);
#ifdef SUPPORTS_WIIMOTE
					if (requested.packetDelay)
						wiimote->set_packet_delay(*requested.packetDelay);
#endif
					++m_inputSettingsGeneration;
				});
				return result;
			}

			InputSettingsResult CalibrateInputDevice(std::uint64_t token) override
			{
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					auto controller = InputControllerForToken(token);
					if (!controller)
						result = {InputSettingsError::DeviceNotFound,
								  "physical device was not found"};
					else
						controller->calibrate();
				});
				return result;
			}

			InputSettingsResult LoadInputProfile(std::uint32_t player,
												 std::string_view profile) override
			{
				if (player >= InputManager::kMaxController)
					return {InputSettingsError::InvalidPlayer, "player index is out of range"};
				const std::string name(profile);
				if (!InputManager::is_valid_profilename(name))
					return {InputSettingsError::InvalidProfile, "profile name is invalid"};
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					if (InputManager::instance().is_gameprofile_set(player))
						result = {InputSettingsError::ProfileLocked,
								  "this player is controlled by the running game's profile"};
					else if (!InputManager::instance().load(player, name))
						result = {InputSettingsError::PersistenceFailed,
								  "input profile could not be loaded"};
					else
						++m_inputSettingsGeneration;
				});
				return result;
			}

			InputSettingsResult SaveInputProfile(std::uint32_t player,
												 std::string_view profile) override
			{
				if (player >= InputManager::kMaxController)
					return {InputSettingsError::InvalidPlayer, "player index is out of range"};
				const std::string name(profile);
				if (!InputManager::is_valid_profilename(name))
					return {InputSettingsError::InvalidProfile, "profile name is invalid"};
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					if (InputManager::instance().is_gameprofile_set(player))
						result = {InputSettingsError::ProfileLocked,
								  "this player is controlled by the running game's profile"};
					else if (!InputManager::instance().save(player, name))
						result = {InputSettingsError::PersistenceFailed,
								  "input profile could not be saved"};
					else
						++m_inputSettingsGeneration;
				});
				return result;
			}

			InputSettingsResult DeleteInputProfile(std::string_view profile) override
			{
				const std::string name(profile);
				if (!InputManager::is_valid_profilename(name))
					return {InputSettingsError::InvalidProfile, "profile name is invalid"};
				std::scoped_lock settingsLock(m_inputSettingsMutex);
				InputSettingsResult result;
				InputManager::instance().RunOnInputThread([&] {
					if (!InputManager::instance().delete_profile(name))
						result = {InputSettingsError::PersistenceFailed,
								  "input profile could not be deleted"};
					else
						++m_inputSettingsGeneration;
				});
				return result;
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
				case CafeConsoleRegion::JPN:
					presentation.region = TitleRegion::Japan;
					break;
				case CafeConsoleRegion::USA:
					presentation.region = TitleRegion::UnitedStates;
					break;
				case CafeConsoleRegion::EUR:
					presentation.region = TitleRegion::Europe;
					break;
				default:
					break;
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
					default:
						break;
					}
				}

				switch (LatteGPUState.glVendor)
				{
				case GLVENDOR_AMD:
					presentation.gpuVendor = PresentationGpuVendor::Amd;
					break;
				case GLVENDOR_INTEL:
					presentation.gpuVendor = PresentationGpuVendor::Intel;
					break;
				case GLVENDOR_NVIDIA:
					presentation.gpuVendor = PresentationGpuVendor::Nvidia;
					break;
				case GLVENDOR_APPLE:
					presentation.gpuVendor = PresentationGpuVendor::Apple;
					break;
				default:
					break;
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

			std::optional<std::uint64_t> ResolveLaunchTitleId(
				const std::filesystem::path& path) const override
			{
				TitleInfo title{path};
				if (title.IsValid())
				{
					TitleId baseTitleId;
					if (CafeTitleList::FindBaseTitleId(title.GetAppTitleId(), baseTitleId))
						return baseTitleId;
					return std::nullopt;
				}
				const auto fileType = DetermineCafeSystemFileType(path);
				if (fileType != CafeTitleFileType::RPX && fileType != CafeTitleFileType::ELF)
					return std::nullopt;
				return CafeSystem::GetStandaloneTitleId(path);
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
				m_diagnosticsThreads.clear();
				m_diagnosticsSuspensions.clear();
				m_diagnosticsGeneration.fetch_add(1, std::memory_order_acq_rel);
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

			RuntimeOverlay::Snapshot GetRuntimeOverlaySnapshot() override
			{
				return RuntimeOverlay::Model::Instance().GetSnapshot();
			}

			bool SubmitRuntimeOverlayKeyboardKey(std::uint64_t generation,
												 std::uint32_t keyCode) override
			{
				if (generation != swkbd_overlayGeneration())
					return false;
				return SubmitSoftwareKeyboardKey(keyCode);
			}

			bool SelectRuntimeOverlayErrorButton(std::uint64_t generation,
												 bool rightButton) override
			{
				return nn::erreula::SelectRuntimeOverlayButton(generation, rightButton);
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
				case NFC_TOUCH_TAG_ERROR_NO_ACCESS:
					return NfcTouchResult::NoAccess;
				case NFC_TOUCH_TAG_ERROR_INVALID_FILE_FORMAT:
					return NfcTouchResult::InvalidFileFormat;
				default:
					return NfcTouchResult::UnknownError;
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

			void SaveCemodPermissionDecisions(std::uint64_t titleId,
											  std::span<const CemodPermissionDecision> decisions) override
			{
				for (const auto& decision : decisions)
				{
					GetConfig().SetCemuExtendModGrant(titleId, decision.principal,
													  {decision.grantedPermissions, decision.requestedPermissions, true});
					// Bind the old launch dialog's decision to every currently installed
					// isolated package for this principal.  Native packages use a different
					// permission namespace and must be approved in the package manager.
					for (const auto& package : cemuextend_hle::DiscoverCemods(titleId))
					{
						if (package.principal != decision.principal || !package.error.empty() ||
							package.executionMode != ::CemodExecutionMode::Isolated)
							continue;
						const auto inspected = cemuextend_hle::InspectCemodPackage(package);
						if (!inspected.valid || inspected.packageDigest.empty() ||
							inspected.modIdentity.empty() ||
							(inspected.requestedPermissions & ~cemuextend_hle::kCemodPermissionMask) != 0)
							continue;
						const auto key = cemuextend_hle::MakeCemodApprovalKey(
							inspected.modIdentity, inspected.packageDigest);
						GetConfig().SetCemuExtendPermissionApproval(titleId, key,
																	{inspected.packageDigest, inspected.modIdentity,
																	 inspected.requestedPermissions,
																	 decision.grantedPermissions & inspected.requestedPermissions,
																	 true, false});
					}
				}
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

			CemodManagerSnapshot GetCemodManagerSnapshot(
				std::optional<std::uint64_t> titleId, CemodCancellationCheck cancelled = {}) override
			{
				CemodManagerSnapshot snapshot;
				snapshot.selectedTitleId = titleId;
				auto discovered = titleId ? cemuextend_hle::DiscoverCemods(*titleId) : cemuextend_hle::DiscoverCemodCatalog();
				snapshot.packages.reserve(discovered.size());
				for (const auto& package : discovered)
				{
					if (cancelled && cancelled())
					{
						snapshot.packages.clear();
						snapshot.generation = 0;
						snapshot.cancelled = true;
						return snapshot;
					}
					auto inspection = titleId ? cemuextend_hle::InspectConfiguredCemodPackage(*titleId, package)
													.value_or(cemuextend_hle::InspectCemodPackage(package))
											  : cemuextend_hle::InspectCemodPackage(package);
					CemodManagerPackage model;
					model.packageKey = inspection.packageDigest;
					if (model.packageKey.empty())
					{
						const auto path = _pathToUtf8(package.path);
						std::uint64_t hash = 1469598103934665603ULL;
						for (const auto byte : path)
						{
							hash ^= static_cast<unsigned char>(byte);
							hash *= 1099511628211ULL;
						}
						model.packageKey = fmt::format("invalid-{:016x}", hash);
					}
					model.titleIds = package.titleIds;
					model.modId = inspection.modId;
					model.principal = inspection.principal;
					model.modIdentity = inspection.modIdentity;
					model.packageDigest = inspection.packageDigest;
					model.pluginName = inspection.pluginName;
					model.author = inspection.author;
					model.version = inspection.version;
					model.description = inspection.description;
					model.scope = inspection.scope;
					model.approvalReason = inspection.approvalReason;
					model.requestedPermissions = inspection.requestedPermissions;
					model.grantedPermissions = inspection.grantedPermissions;
					model.approved = inspection.approved;
					model.signedPackage = inspection.signedPackage;
					model.trustedNative = package.executionMode == ::CemodExecutionMode::TrustedNative;
					model.wups = inspection.wups;
					model.headless = inspection.headless;
					model.runtimeAvailable = true;
					model.valid = inspection.valid;
					model.status = !model.valid ? "rejected" : model.approved ? "ready"
																			  : "disabled";
					model.warnings = inspection.warnings;
					for (const auto& permission : inspection.permissions)
						model.permissions.push_back({permission.name,
													 permission.bit, permission.requested, permission.granted,
													 permission.dangerous, permission.manifestMismatch});
					snapshot.packages.push_back(std::move(model));
				}
				if (cancelled && cancelled())
				{
					snapshot.packages.clear();
					snapshot.generation = 0;
					snapshot.cancelled = true;
					return snapshot;
				}
				std::ranges::sort(snapshot.packages, {}, &CemodManagerPackage::packageKey);
				snapshot.generation = CemodCatalogFingerprint(snapshot.packages);
				return snapshot;
			}

			CemodManagerResult SaveCemodApproval(const CemodApprovalUpdate& update) override
			{
				auto snapshot = GetCemodManagerSnapshot(update.titleId);
				if (snapshot.generation != update.generation)
					return {CemodManagerError::Conflict, "The installed package catalog changed", std::move(snapshot)};
				const auto found = std::ranges::find(snapshot.packages, update.packageKey,
													 &CemodManagerPackage::packageKey);
				if (found == snapshot.packages.end() || !found->valid ||
					std::ranges::find(found->titleIds, update.titleId) == found->titleIds.end())
					return {CemodManagerError::NotFound, "The selected package is no longer installed", std::move(snapshot)};
				constexpr std::uint64_t kKnownPermissions = (1ULL << 11) - 1;
				if ((found->requestedPermissions & ~kKnownPermissions) != 0 ||
					(update.grantedPermissions & ~found->requestedPermissions) != 0)
					return {CemodManagerError::InvalidPermissions, "The permission selection is invalid", std::move(snapshot)};
				const auto approvalKey = cemuextend_hle::MakeCemodApprovalKey(
					found->modIdentity, found->packageDigest);
				auto configLock = GetConfigHandle().Lock();
				const auto previous = GetConfig().GetCemuExtendPermissionApproval(update.titleId, approvalKey);
				GetConfig().SetCemuExtendPermissionApproval(update.titleId, approvalKey,
															{found->packageDigest, found->modIdentity, found->requestedPermissions,
															 update.approved ? update.grantedPermissions : 0, update.approved, false});
				if (!GetConfigHandle().Save())
				{
					if (previous)
						GetConfig().SetCemuExtendPermissionApproval(update.titleId, approvalKey, *previous);
					else
						GetConfig().RemoveCemuExtendPermissionApproval(update.titleId, approvalKey);
					return {CemodManagerError::SaveFailed, "The approval could not be persisted", std::move(snapshot)};
				}
				configLock.unlock();
				cemuextend_hle::ReloadCemodPermissions(update.titleId, found->principal);
				return {CemodManagerError::None, {}, GetCemodManagerSnapshot(update.titleId)};
			}

			CemodManagerResult ImportLegacyCemodPackageData(std::uint64_t generation,
															std::uint64_t titleId, std::string_view packageKey) override
			{
				auto snapshot = GetCemodManagerSnapshot(titleId);
				if (snapshot.generation != generation)
					return {CemodManagerError::Conflict, "The installed package catalog changed", std::move(snapshot)};
				const auto found = std::ranges::find(snapshot.packages, packageKey,
													 &CemodManagerPackage::packageKey);
				if (found == snapshot.packages.end() || !found->valid ||
					std::ranges::find(found->titleIds, titleId) == found->titleIds.end())
					return {CemodManagerError::NotFound, "The selected package is no longer installed", std::move(snapshot)};
				std::string error;
				if (!cemuextend_hle::ImportLegacyData(titleId, found->principal, error))
					return {CemodManagerError::ImportFailed, std::move(error), std::move(snapshot)};
				return {CemodManagerError::None, {}, GetCemodManagerSnapshot(titleId)};
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

			std::vector<ManagedContentEntry> ListManagedContent() const override
			{
				struct ListLease final
				{
					std::span<TitleInfo*> titles{CafeTitleList::AcquireInternalList()};
					~ListLease()
					{
						CafeTitleList::ReleaseInternalList();
					}
				} lease;
				std::vector<ManagedContentEntry> result;
				result.reserve(lease.titles.size());
				for (auto* title : lease.titles)
				{
					if (title)
						result.push_back(TranslateManagedTitle(*title, true));
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
				const WuaConversionPlan& plan,
				const std::filesystem::path& outputPath,
				ContentProgressHandler progress,
				ContentCancellationCheck cancelled) override
			{
				if (plan.items.empty())
					return {ContentOperationError::NotFound, "No title content selected"};
				if (outputPath.empty())
					return {ContentOperationError::UnableToCreateOutput,
							"Archive output path is empty"};

				const auto current = BuildWuaConversionPlan(plan.items.front().titleId,
															plan.items.front().locationUid);
				auto samePlan = [](const WuaConversionPlan& left, const WuaConversionPlan& right) {
					if (left.items.size() != right.items.size())
						return false;
					for (std::size_t index = 0; index < left.items.size(); ++index)
					{
						const auto& a = left.items[index];
						const auto& b = right.items[index];
						if (a.locationUid != b.locationUid || a.titleId != b.titleId ||
							a.version != b.version || a.fingerprint != b.fingerprint || a.role != b.role)
							return false;
					}
					return true;
				};
				if (!current || !samePlan(plan, *current))
					return {ContentOperationError::VerificationFailure,
							"Installed content changed after the WUA conversion was confirmed"};

				std::vector<TitleInfo> titles;
				titles.reserve(plan.items.size());
				for (const auto& item : plan.items)
				{
					auto title = CafeTitleList::GetTitleInfoByUID(item.locationUid);
					if (!title.IsValid())
						return {ContentOperationError::NotFound,
								fmt::format("Title content {:016x} is no longer available", item.locationUid)};
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
							return {wasCancelled ? ContentOperationError::Cancelled : ContentOperationError::ReadFailure,
									wasCancelled ? "Conversion cancelled" : "Unable to enumerate title files"};
						}
					}
					for (auto& title : titles)
					{
						if (!context.Process(title, false))
						{
							const bool wasCancelled = context.IsCancelled();
							context.Close();
							removeTemporary();
							return {wasCancelled ? ContentOperationError::Cancelled : ContentOperationError::ReadFailure,
									wasCancelled ? "Conversion cancelled" : "Unable to read title files"};
						}
					}
					const auto afterRead = BuildWuaConversionPlan(plan.items.front().titleId,
																  plan.items.front().locationUid);
					if (!afterRead || !samePlan(plan, *afterRead))
					{
						context.Close();
						removeTemporary();
						return {ContentOperationError::VerificationFailure,
								"Installed content changed while the WUA archive was being created"};
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
				} catch (const std::exception& exception)
				{
					removeTemporary();
					return {ContentOperationError::ReadFailure, exception.what()};
				} catch (...)
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
				auto isCancelled = [&cancelled] {
					return cancelled && cancelled();
				};
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
							~UnmountGuard()
							{
								title.Unmount(path);
							}
						} unmount{title, mountPath};

						std::vector<std::pair<std::string, std::uint64_t>> files;
						constexpr std::size_t kMaximumChecksumFiles = 20000;
						constexpr std::size_t kMaximumChecksumPath = 4096;
						constexpr std::uint64_t kMaximumChecksumBytes = 2ULL * 1024 * 1024 * 1024 * 1024;
						std::uint64_t collectedBytes{};
						std::size_t collectedPathBytes{};
						std::size_t collectedDirectories{};
						std::string collectionLimitError;
						std::function<bool(const std::string&, std::size_t)> collect =
							[&](const std::string& relative, std::size_t depth) {
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
									const std::string_view component(entry.path);
									if (component.empty() || component == "." || component == ".." ||
										component.find_first_of("/\\\0") != std::string_view::npos)
									{
										collectionLimitError = "Title contains an invalid checksum path component";
										return false;
									}
									const auto child = relative + entry.path;
									if (entry.isDirectory)
									{
										if (depth >= 256 || collectedDirectories >= 20000 ||
											child.size() > kMaximumChecksumPath ||
											child.size() > 4 * 1024 * 1024 - collectedPathBytes)
										{
											collectionLimitError = "Title exceeds the safe checksum directory depth or size limit";
											return false;
										}
										++collectedDirectories;
										collectedPathBytes += child.size();
										if (!collect(child + "/", depth + 1))
											return false;
									}
									else if (entry.isFile)
									{
										if (child.size() > kMaximumChecksumPath || files.size() >= kMaximumChecksumFiles ||
											child.size() > 4 * 1024 * 1024 - collectedPathBytes ||
											entry.fileSize > kMaximumChecksumBytes - collectedBytes)
										{
											collectionLimitError = "Title exceeds the safe checksum file, path, or size limit";
											return false;
										}
										collectedBytes += static_cast<std::uint64_t>(entry.fileSize);
										collectedPathBytes += child.size();
										files.emplace_back(child,
														   static_cast<std::uint64_t>(entry.fileSize));
										publish({ContentOperationPhase::Collecting, 0,
												 static_cast<std::uint32_t>(files.size()), 0, 0});
									}
								}
								return true;
							};
						if (!collect("", 0))
							return {isCancelled() ? ContentOperationError::Cancelled : collectionLimitError.empty() ? ContentOperationError::ReadFailure
																													: ContentOperationError::VerificationFailure,
									isCancelled() ? "Checksum cancelled" : !collectionLimitError.empty() ? collectionLimitError
																										 : "Unable to enumerate title files",
									std::nullopt};
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
										 &status),
								&fsc_close);
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
				} catch (const std::exception& exception)
				{
					return {ContentOperationError::ReadFailure, exception.what(), std::nullopt};
				} catch (...)
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
					case kOpenGL:
						result.settings.graphicsApi = GameProfileGraphicsApi::OpenGL;
						break;
					case kVulkan:
						result.settings.graphicsApi = GameProfileGraphicsApi::Vulkan;
						break;
#ifdef ENABLE_METAL
					case kMetal:
						result.settings.graphicsApi = GameProfileGraphicsApi::Metal;
						break;
#endif
					default:
						result.settings.graphicsApi = GameProfileGraphicsApi::Default;
						break;
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
				profile.SetAccurateShaderMul(update.accurateShaderMultiplication ? AccurateShaderMulOption::True : AccurateShaderMulOption::False);

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
				case GameProfileGraphicsApi::OpenGL:
					profile.SetGraphicsAPI(kOpenGL);
					break;
				case GameProfileGraphicsApi::Vulkan:
					profile.SetGraphicsAPI(kVulkan);
					break;
#ifdef ENABLE_METAL
				case GameProfileGraphicsApi::Metal:
					profile.SetGraphicsAPI(kMetal);
					break;
#endif
				default:
					profile.SetGraphicsAPI(std::nullopt);
					break;
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
							"The existing title conflict was not accepted",
							{}};

				auto currentPlanResult = SafeBuildTitleInstallPlan(plan.sourcePath);
				if (!currentPlanResult)
					return {currentPlanResult.error, currentPlanResult.diagnostic, {}};
				if (!SameInstallSnapshot(plan, *currentPlanResult.plan))
					return {TitleInstallError::StalePlan,
							"The source or installed title changed before installation",
							{}};

				std::error_code ec;
				const auto canonicalSource = fs::weakly_canonical(plan.sourcePath, ec);
				ec.clear();
				const auto canonicalTarget = fs::weakly_canonical(plan.targetPath, ec);
				if ((!canonicalSource.empty() && canonicalSource == canonicalTarget) ||
					(fs::exists(plan.targetPath, ec) && !ec &&
					 fs::equivalent(plan.sourcePath, plan.targetPath, ec) && !ec))
					return {TitleInstallError::InvalidSource,
							"A title cannot be installed from its destination folder",
							{}};

				if (cancelled && cancelled())
					return {TitleInstallError::Cancelled, "Title installation cancelled", {}};

				const auto stagingPath = UniqueInstallSibling(plan.targetPath, "installing");
				const auto backupPath = plan.installed.exists ? UniqueInstallSibling(plan.targetPath, "backup") : fs::path{};
				if (stagingPath.empty() || (plan.installed.exists && backupPath.empty()))
					return {TitleInstallError::CopyFailure,
							"Unable to reserve a temporary installation path",
							{}};

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
								ec ? ec.message() : "Temporary installation path already exists",
								{}};

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
										"Title installation cancelled",
										{}};
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
												"Title installation cancelled",
												{}};
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
								"The title source changed while staging the installation",
								{}};
					}
					if (cancelled && cancelled())
					{
						removeStaging();
						return {TitleInstallError::Cancelled,
								"Title installation cancelled",
								{}};
					}

					const bool targetExists = fs::exists(plan.targetPath, ec) && !ec;
					if (ec || targetExists != plan.installed.exists ||
						(targetExists && FingerprintInstallTarget(plan.targetPath) !=
											 plan.installed.fingerprint))
					{
						removeStaging();
						return {TitleInstallError::StalePlan,
								"The installed title changed while staging the update",
								{}};
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
													commitError.message(), _pathToUtf8(backupPath), ec.message()),
										{}};
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
					} catch (const std::exception& exception)
					{
						if (!warning.empty())
							warning.append("; ");
						warning.append(fmt::format(
							"installed title could not be refreshed in the title catalog: {}",
							exception.what()));
					} catch (...)
					{
						if (!warning.empty())
							warning.append("; ");
						warning.append(
							"installed title could not be refreshed in the title catalog");
					}
					return {TitleInstallError::None, std::move(warning), plan.targetPath};
				} catch (const std::exception& exception)
				{
					removeStaging();
					return {TitleInstallError::CopyFailure, exception.what(), {}};
				} catch (...)
				{
					removeStaging();
					return {TitleInstallError::CopyFailure,
							"Unknown title installation failure",
							{}};
				}
			}

			ManagedContentDeletePlanResult PlanManagedContentDelete(
				std::uint64_t locationUid) const override
			{
				if (IsTitleRunning())
					return {ManagedContentDeleteError::TitleRunning,
							"Managed content cannot be deleted while a title is running", std::nullopt};
				if (CafeTitleList::IsScanning())
					return {ManagedContentDeleteError::Scanning,
							"Managed content cannot be deleted while the title catalog is scanning", std::nullopt};
				const auto title = CafeTitleList::GetTitleInfoByUID(locationUid);
				if (!title.IsValid())
					return {ManagedContentDeleteError::NotFound,
							"The selected managed-content installation is no longer available", std::nullopt};
				auto entry = TranslateManagedTitle(const_cast<TitleInfo&>(title), true);
				if (!IsManagedContentDeletionSupported(entry.type))
					return {ManagedContentDeleteError::Unsupported,
							"System titles cannot be deleted from Title Manager", std::nullopt};
				if (entry.path.empty())
					return {ManagedContentDeleteError::Unsupported,
							"The selected installation does not have a deletable catalog location", std::nullopt};
				const auto safePath = ValidateCatalogDeletePath(entry.path);
				if (!safePath)
					return {ManagedContentDeleteError::Unsupported, safePath.diagnostic, std::nullopt};
				entry.path = safePath.canonicalPath;
				return {ManagedContentDeleteError::None, {}, ManagedContentDeletePlan{
																 .locationUid = entry.locationUid,
																 .titleId = entry.titleId,
																 .fingerprint = ManagedContentFingerprint(entry),
																 .name = entry.name,
																 .displayPath = _pathToUtf8(entry.path),
																 .type = entry.type,
																 .format = entry.format,
															 }};
			}

			ManagedContentDeleteResult DeleteManagedContent(
				const ManagedContentDeletePlan& plan) override
			{
				if (IsTitleRunning())
					return {ManagedContentDeleteError::TitleRunning,
							"Managed content cannot be deleted while a title is running"};
				if (CafeTitleList::IsScanning())
					return {ManagedContentDeleteError::Scanning,
							"Managed content cannot be deleted while the title catalog is scanning"};
				const auto title = CafeTitleList::GetTitleInfoByUID(plan.locationUid);
				if (!title.IsValid())
					return {ManagedContentDeleteError::NotFound,
							"The selected managed-content installation is no longer available"};
				auto entry = TranslateManagedTitle(const_cast<TitleInfo&>(title), false);
				if (!IsManagedContentDeletionSupported(entry.type))
					return {ManagedContentDeleteError::Unsupported,
							"System titles cannot be deleted from Title Manager"};
				const auto safePath = ValidateCatalogDeletePath(entry.path);
				if (!safePath)
					return {ManagedContentDeleteError::Unsupported, safePath.diagnostic};
				entry.path = safePath.canonicalPath;
				if (entry.titleId != plan.titleId || entry.type != plan.type ||
					entry.format != plan.format || ManagedContentFingerprint(entry) != plan.fingerprint)
					return {ManagedContentDeleteError::StalePlan,
							"The selected installation changed after deletion was confirmed"};
				std::error_code error;
				const auto status = fs::symlink_status(entry.path, error);
				if (error || status.type() == fs::file_type::not_found)
					return {ManagedContentDeleteError::NotFound,
							error ? error.message() : "The selected installation no longer exists"};
				if (status.type() == fs::file_type::symlink)
					return {ManagedContentDeleteError::Unsupported,
							"Managed-content catalog roots may not be symbolic links"};
				const auto quarantine = UniqueInstallSibling(entry.path, "deleting");
				if (quarantine.empty())
					return {ManagedContentDeleteError::DeleteFailure,
							"Unable to reserve a deletion quarantine path"};
				fs::rename(entry.path, quarantine, error);
				if (error)
					return {ManagedContentDeleteError::DeleteFailure, error.message()};
				auto moved = entry;
				moved.path = quarantine;
				if (ManagedContentFingerprint(moved, &entry.path) != plan.fingerprint)
				{
					std::error_code restoreError;
					fs::rename(quarantine, entry.path, restoreError);
					return {ManagedContentDeleteError::StalePlan, restoreError ? fmt::format("The selected installation changed; it was quarantined at {} because restore failed: {}",
																							 _pathToUtf8(quarantine), restoreError.message())
																			   : "The selected installation changed while deletion was starting"};
				}
				fs::remove_all(quarantine, error);
				if (error)
					return {ManagedContentDeleteError::DeleteFailure,
							fmt::format("Content was quarantined at {}, but deletion failed: {}",
										_pathToUtf8(quarantine), error.message())};
				CafeTitleList::Refresh();
				return {};
			}

			std::vector<AccountInfo> ListAccounts() const override
			{
				std::vector<AccountInfo> result;
				const auto& accounts = ::Account::GetAccounts();
				result.reserve(accounts.size());
				for (const auto& account : accounts)
					result.push_back(TranslateAccount(account));
				return result;
			}

			std::optional<AccountInfo> GetAccount(
				std::uint32_t persistentId) const override
			{
				const auto* account = FindAccount(persistentId);
				return account ? std::optional{TranslateAccount(*account)} : std::nullopt;
			}

			std::uint32_t NextPersistentId() const override
			{
				return ::Account::GetNextPersistentId();
			}

			bool HasFreeAccountSlots() const override
			{
				return ::Account::HasFreeAccountSlots();
			}

			std::vector<AccountCountry> ListAccountCountries() const override
			{
				std::vector<AccountCountry> result;
				for (std::uint32_t index = 0; index <= std::numeric_limits<std::uint8_t>::max(); ++index)
				{
					const char* country = NCrypto::GetCountryAsString(index);
					if (country && (index == 0 || !boost::equals(country, "NN")))
						result.push_back({index, country});
				}
				return result;
			}

			OnlineEnvironmentStatus GetOnlineEnvironmentStatus() const override
			{
				return {
					.requiredFilesAvailable = ActiveSettings::HasRequiredOnlineFiles(),
					.otpPresent = NCrypto::OTP_IsPresent(),
					.seepromPresent = NCrypto::SEEPROM_IsPresent(),
					.consoleCertificateAvailable = NCrypto::HasDataForConsoleCert(),
				};
			}

			AccountManagerSnapshot GetAccountManagerSnapshot() const override
			{
				AccountManagerSnapshot snapshot;
				snapshot.accounts = ListAccounts();
				snapshot.countries = ListAccountCountries();
				snapshot.onlineEnvironment = GetOnlineEnvironmentStatus();
				snapshot.activePersistentId = ActiveSettings::GetPersistentId();
				snapshot.nextPersistentId = NextPersistentId();
				snapshot.hasFreeSlots = HasFreeAccountSlots();
				snapshot.titleRunning = IsTitleRunning();
				snapshot.networkSettings.reserve(snapshot.accounts.size());
				for (const auto& account : snapshot.accounts)
				{
					snapshot.networkSettings.push_back({account.persistentId,
														TranslateNetworkService(
															GetConfig().GetAccountNetworkService(account.persistentId)),
														ValidateOnlineAccount(account.persistentId)});
				}
				return snapshot;
			}

			AccountOperationResult SetActiveAccount(std::uint32_t persistentId) override
			{
				if (IsTitleRunning())
					return {AccountOperationError::TitleRunning,
							"the active account cannot be changed while a title is running"};
				const auto* account = FindAccount(persistentId);
				if (!account)
					return {AccountOperationError::NotFound, "account no longer exists"};
				GetConfig().account.m_persistent_id = persistentId;
				GetConfigHandle().Save();
				return {AccountOperationError::None, {}, TranslateAccount(*account)};
			}

			AccountOperationResult SetAccountNetworkService(std::uint32_t persistentId,
															AccountNetworkService service) override
			{
				if (IsTitleRunning())
					return {AccountOperationError::TitleRunning,
							"network service cannot be changed while a title is running"};
				const auto* account = FindAccount(persistentId);
				if (!account)
					return {AccountOperationError::NotFound, "account no longer exists"};
				if (service != AccountNetworkService::Offline && !account->IsValidOnlineAccount())
					return {AccountOperationError::BackendFailure,
							"online account files must be valid before enabling a network service"};
				GetConfig().SetAccountSelectedService(persistentId,
													  TranslateNetworkService(service));
				GetConfigHandle().Save();
				return {AccountOperationError::None, {}, TranslateAccount(*account)};
			}

			DownloadAccountContext GetDownloadAccountContext(
				std::optional<std::uint32_t> persistentId) const override
			{
				DownloadAccountContext result;
				if (!ActiveSettings::HasRequiredOnlineFiles() || !NCrypto::OTP_IsPresent() ||
					!NCrypto::SEEPROM_IsPresent() || !NCrypto::HasDataForConsoleCert())
				{
					result.error = DownloadAccountError::OnlineFilesMissing;
					return result;
				}

				if (persistentId)
				{
					const auto* account = FindAccount(*persistentId);
					if (!account)
					{
						result.error = DownloadAccountError::AccountNotFound;
						return result;
					}
					const auto validation = account->ValidateOnlineFiles();
					if (!validation.valid_account)
					{
						result.error = DownloadAccountError::InvalidCredentials;
						return result;
					}
					result.accountName = account->GetAccountId();
					result.passwordHash = account->GetAccountPasswordCache();
					result.country = NCrypto::GetCountryAsString(account->GetCountry());
				}

				result.deviceCertificateBase64 =
					NCrypto::CertECC::GetDeviceCertificate().encodeToBase64();
				result.region = static_cast<std::uint32_t>(NCrypto::SEEPROM_GetRegion());
				result.deviceId = NCrypto::GetDeviceId();
				result.serial = NCrypto::GetSerial();
				return result;
			}

			AccountValidation ValidateOnlineAccount(
				std::uint32_t persistentId) const override
			{
				const auto* account = FindAccount(persistentId);
				if (!account)
					return {.accountError = AccountOnlineError::NoAccountId};
				const auto validation = account->ValidateOnlineFiles();
				return {
					.validAccount = validation.valid_account,
					.otp = TranslateAccountFileState(validation.otp),
					.seeprom = TranslateAccountFileState(validation.seeprom),
					.missingFiles = validation.missing_files,
					.accountError = TranslateAccountOnlineError(validation.account_error),
				};
			}

			AccountOperationResult CreateAccount(std::uint32_t persistentId,
												 std::wstring_view miiName) override
			{
				if (IsTitleRunning())
					return {AccountOperationError::TitleRunning,
							"accounts cannot be changed while a title is running"};
				if (persistentId < kMinimumPersistentId)
					return {AccountOperationError::InvalidPersistentId,
							"persistent id is below the supported range"};
				if (miiName.empty() || miiName.size() > 10)
					return {AccountOperationError::InvalidMiiName,
							"account name must contain between 1 and 10 characters"};
				if (!::Account::HasFreeAccountSlots())
					return {AccountOperationError::NoFreeSlots,
							"all account slots are occupied"};
				if (FindAccount(persistentId))
					return {AccountOperationError::DuplicatePersistentId,
							"persistent id is already in use"};
				try
				{
					::Account account(persistentId, miiName);
					const auto error = account.Save();
					if (error)
						return {AccountOperationError::IoFailure, error.message()};
					::Account::RefreshAccounts();
					return {AccountOperationError::None, {}, TranslateAccount(account)};
				} catch (const std::exception& exception)
				{
					return {AccountOperationError::BackendFailure, exception.what()};
				}
			}

			AccountOperationResult UpdateAccount(std::uint32_t persistentId,
												 const AccountUpdate& update) override
			{
				if (IsTitleRunning())
					return {AccountOperationError::TitleRunning,
							"accounts cannot be changed while a title is running"};
				const auto* existing = FindAccount(persistentId);
				if (!existing)
					return {AccountOperationError::NotFound, "account no longer exists"};
				if (update.miiName.empty() || update.miiName.size() > 10)
					return {AccountOperationError::InvalidMiiName,
							"account name must contain between 1 and 10 characters"};
				if (update.birthYear > 2100 || update.birthMonth > 12 ||
					update.birthDay > 31 || update.gender > 2 || update.email.size() > 320)
					return {AccountOperationError::BackendFailure,
							"account profile fields are outside the supported range"};
				const char* country = update.country <= std::numeric_limits<std::uint8_t>::max() ? NCrypto::GetCountryAsString(update.country) : nullptr;
				if (!country || (update.country != 0 && boost::equals(country, "NN")))
					return {AccountOperationError::BackendFailure,
							"account country is not supported"};
				try
				{
					::Account account = *existing;
					account.SetMiiName(update.miiName);
					account.SetBirthYear(update.birthYear);
					account.SetBirthMonth(update.birthMonth);
					account.SetBirthDay(update.birthDay);
					account.SetGender(update.gender);
					account.SetEmail(update.email);
					account.SetCountry(update.country);
					const auto error = account.Save();
					if (error)
						return {AccountOperationError::IoFailure, error.message()};
					::Account::RefreshAccounts();
					return {AccountOperationError::None, {}, TranslateAccount(account)};
				} catch (const std::exception& exception)
				{
					return {AccountOperationError::BackendFailure, exception.what()};
				}
			}

			AccountOperationResult DeleteAccount(
				std::uint32_t persistentId) override
			{
				if (IsTitleRunning())
					return {AccountOperationError::TitleRunning,
							"accounts cannot be changed while a title is running"};
				const auto& accounts = ::Account::GetAccounts();
				if (accounts.size() <= 1)
					return {AccountOperationError::CannotDeleteOnlyAccount,
							"the only account cannot be deleted"};
				const auto* account = FindAccount(persistentId);
				if (!account)
					return {AccountOperationError::NotFound, "account no longer exists"};
				const bool deletingActive =
					GetConfig().account.m_persistent_id.GetValue() == persistentId;
				std::error_code error;
				fs::remove_all(account->GetFileName().parent_path(), error);
				if (error)
					return {AccountOperationError::IoFailure, error.message()};
				const auto& remainingAccounts = ::Account::RefreshAccounts();
				if (deletingActive && !remainingAccounts.empty())
				{
					GetConfig().account.m_persistent_id =
						remainingAccounts.front().GetPersistentId();
					GetConfigHandle().Save();
				}
				return {};
			}

			std::vector<std::uint32_t> ListSavePersistentIds(
				std::uint64_t titleId) const override
			{
				auto saveListLock = CafeSaveList::AcquireOperationLock();
				std::vector<std::uint32_t> result;
				std::error_code ec;
				for (fs::directory_iterator iterator(SaveUserRoot(titleId), ec), end;
					 !ec && iterator != end; iterator.increment(ec))
				{
					const auto status = iterator->symlink_status(ec);
					if (ec || status.type() != fs::file_type::directory ||
						fs::is_empty(iterator->path(), ec) || ec)
					{
						ec.clear();
						continue;
					}
					const auto name = iterator->path().filename().string();
					if (name.size() != 8)
						continue;
					std::uint32_t persistentId{};
					const auto parsed = std::from_chars(name.data(), name.data() + name.size(),
														persistentId, 16);
					if (parsed.ec == std::errc() && parsed.ptr == name.data() + name.size())
						result.push_back(persistentId);
				}
				std::ranges::sort(result);
				return result;
			}

			SaveEntryLocation InspectSaveEntry(std::uint64_t titleId,
											   std::uint32_t persistentId) const override
			{
				auto saveListLock = CafeSaveList::AcquireOperationLock();
				return InspectSaveEntryPath(SaveAccountPath(titleId, persistentId));
			}

			SaveImportInspection InspectSaveImport(const fs::path& archivePath,
												   std::uint64_t titleId, std::uint32_t persistentId) const override
			{
				if (persistentId < kMinimumPersistentId)
					return {SaveOperationError::InvalidPersistentId,
							"persistent id is below the supported range"};
				const auto plan = BuildSaveArchivePlan(archivePath);
				if (!plan)
					return {plan.error, plan.diagnostic};
				auto saveListLock = CafeSaveList::AcquireOperationLock();
				return {SaveOperationError::None, {}, plan.sourceTitleId, InspectSaveEntryPath(SaveAccountPath(titleId, persistentId))};
			}

			SaveOperationResult DeleteSave(std::uint64_t titleId,
										   std::uint32_t persistentId) override
			{
				if (persistentId < kMinimumPersistentId)
					return {SaveOperationError::InvalidPersistentId,
							"persistent id is below the supported range"};
				std::unique_lock<std::mutex> saveListLock;
				if (auto status = BeginSaveMutation(saveListLock); !status)
					return status;

				const auto target = SaveAccountPath(titleId, persistentId);
				if (InspectSaveEntryPath(target).state != SaveEntryState::Directory)
					return {SaveOperationError::NotFound, "save directory no longer exists"};

				SaveMetadataStage metadata;
				auto metadataResult = StageSaveMetadata(titleId, [persistentId](pugi::xml_node info) {
						auto node = FindSaveAccountNode(info, persistentId);
						return node ? info.remove_child(node) : false; }, metadata);
				if (!metadataResult)
					return metadataResult;

				const auto quarantine = UniqueSiblingPath(target, "deleted");
				if (quarantine.empty())
				{
					RemovePathQuietly(metadata.temporary);
					return {SaveOperationError::IoFailure,
							"unable to reserve save deletion staging path"};
				}
				std::error_code ec;
				fs::rename(target, quarantine, ec);
				if (ec)
				{
					RemovePathQuietly(metadata.temporary);
					return {SaveOperationError::IoFailure, ec.message()};
				}
				metadataResult = CommitSaveMetadata(metadata);
				if (!metadataResult)
				{
					std::error_code restoreError;
					fs::rename(quarantine, target, restoreError);
					RemovePathQuietly(metadata.temporary);
					if (restoreError)
						metadataResult.diagnostic.append(fmt::format(
							"; save restore failed: {}", restoreError.message()));
					return metadataResult;
				}
				RemovePathQuietly(quarantine);
				return {};
			}

			SaveOperationResult TransferSave(std::uint64_t titleId,
											 std::uint32_t sourcePersistentId, std::uint32_t targetPersistentId,
											 bool overwrite) override
			{
				if (sourcePersistentId < kMinimumPersistentId ||
					targetPersistentId < kMinimumPersistentId ||
					sourcePersistentId == targetPersistentId)
					return {SaveOperationError::InvalidPersistentId,
							"source and target persistent ids must be distinct and valid"};
				std::unique_lock<std::mutex> saveListLock;
				if (auto status = BeginSaveMutation(saveListLock); !status)
					return status;

				const auto source = SaveAccountPath(titleId, sourcePersistentId);
				const auto target = SaveAccountPath(titleId, targetPersistentId);
				if (InspectSaveEntryPath(source).state != SaveEntryState::Directory)
					return {SaveOperationError::NotFound, "source save directory no longer exists"};
				const auto targetState = InspectSaveEntryPath(target).state;
				if (targetState == SaveEntryState::NonDirectory)
					return {SaveOperationError::InvalidTarget,
							"target save path is not a directory"};
				if (targetState == SaveEntryState::Directory && !overwrite)
					return {SaveOperationError::TargetExists,
							"target account already has save data"};

				SaveMetadataStage metadata;
				auto metadataResult = StageSaveMetadata(titleId, [sourcePersistentId, targetPersistentId](pugi::xml_node info) {
						if (auto targetNode = FindSaveAccountNode(info, targetPersistentId))
							info.remove_child(targetNode);
						const auto targetId = fmt::format("{:08x}", targetPersistentId);
						if (auto sourceNode = FindSaveAccountNode(info, sourcePersistentId))
						{
							sourceNode.attribute("persistentId").set_value(targetId.c_str());
							return true;
						}
						auto account = info.append_child("account");
						account.append_attribute("persistentId").set_value(targetId.c_str());
						auto timestamp = account.append_child("timestamp");
						const auto unixSeconds = std::chrono::duration_cast<std::chrono::seconds>(
							std::chrono::system_clock::now().time_since_epoch()).count();
						constexpr std::int64_t unixToWiiEpoch = 946684800;
						timestamp.text().set(fmt::format("{:016x}",
							std::max<std::int64_t>(0, unixSeconds - unixToWiiEpoch)).c_str());
						return true; }, metadata);
				if (!metadataResult)
					return metadataResult;

				const auto staging = UniqueSiblingPath(source, "moving");
				const auto backup = targetState == SaveEntryState::Directory ? UniqueSiblingPath(target, "backup") : fs::path{};
				if (staging.empty() || (targetState == SaveEntryState::Directory && backup.empty()))
				{
					RemovePathQuietly(metadata.temporary);
					return {SaveOperationError::IoFailure,
							"unable to reserve save transfer staging paths"};
				}

				std::error_code ec;
				fs::rename(source, staging, ec);
				if (ec)
				{
					RemovePathQuietly(metadata.temporary);
					return {SaveOperationError::IoFailure, ec.message()};
				}
				if (!backup.empty())
				{
					fs::rename(target, backup, ec);
					if (ec)
					{
						std::error_code ignored;
						fs::rename(staging, source, ignored);
						RemovePathQuietly(metadata.temporary);
						return {SaveOperationError::IoFailure, ec.message()};
					}
				}
				fs::rename(staging, target, ec);
				if (ec)
				{
					std::error_code ignored;
					if (!backup.empty())
						fs::rename(backup, target, ignored);
					fs::rename(staging, source, ignored);
					RemovePathQuietly(metadata.temporary);
					return {SaveOperationError::IoFailure, ec.message()};
				}

				metadataResult = CommitSaveMetadata(metadata);
				if (!metadataResult)
				{
					std::error_code rollbackError;
					fs::rename(target, staging, rollbackError);
					if (!backup.empty())
					{
						std::error_code restoreTargetError;
						fs::rename(backup, target, restoreTargetError);
						if (restoreTargetError)
							metadataResult.diagnostic.append(fmt::format(
								"; target restore failed: {}", restoreTargetError.message()));
					}
					std::error_code restoreSourceError;
					fs::rename(staging, source, restoreSourceError);
					if (rollbackError || restoreSourceError)
						metadataResult.diagnostic.append("; source restore failed");
					RemovePathQuietly(metadata.temporary);
					return metadataResult;
				}

				RemovePathQuietly(backup);
				return {};
			}

			SaveOperationResult ImportSave(const fs::path& archivePath,
										   std::uint64_t titleId, std::uint32_t persistentId, bool overwrite,
										   SaveProgressHandler progress,
										   SaveCancellationCheck cancelled) override
			{
				if (persistentId < kMinimumPersistentId)
					return {SaveOperationError::InvalidPersistentId,
							"persistent id is below the supported range"};
				std::unique_lock<std::mutex> saveListLock;
				if (auto status = BeginSaveMutation(saveListLock); !status)
					return status;

				const auto plan = BuildSaveArchivePlan(archivePath);
				if (!plan)
					return {plan.error, plan.diagnostic};
				const auto target = SaveAccountPath(titleId, persistentId);
				const auto targetState = InspectSaveEntryPath(target).state;
				if (targetState == SaveEntryState::NonDirectory)
					return {SaveOperationError::InvalidTarget,
							"target save path is not a directory"};
				if (targetState == SaveEntryState::Directory && !overwrite)
					return {SaveOperationError::TargetExists,
							"target account already has save data"};

				std::error_code ec;
				fs::create_directories(target.parent_path(), ec);
				if (ec)
					return {SaveOperationError::IoFailure, ec.message()};
				const auto staging = UniqueSiblingPath(target, "importing");
				if (staging.empty() || !fs::create_directory(staging, ec) || ec)
					return {SaveOperationError::IoFailure,
							ec ? ec.message() : "unable to create save import staging directory"};

				SaveMetadataStage metadata;
				auto cleanup = [&] {
					RemovePathQuietly(staging);
					RemovePathQuietly(metadata.temporary);
				};
				try
				{
					int zipError{};
					zip_t* archive = zip_open(_pathToUtf8(archivePath).c_str(), ZIP_RDONLY,
											  &zipError);
					if (!archive)
					{
						cleanup();
						return {SaveOperationError::ArchiveInvalid,
								"unable to reopen save archive"};
					}
					const std::unique_ptr<zip_t, decltype(&zip_discard)> closeArchive(
						archive, &zip_discard);
					std::array<char, 1024 * 1024> buffer{};
					SaveOperationProgress progressValue{
						.filesTotal = plan.entries.size(), .bytesTotal = plan.bytesTotal};
					for (const auto& entry : plan.entries)
					{
						if (cancelled && cancelled())
						{
							cleanup();
							return {SaveOperationError::Cancelled, "save import was cancelled"};
						}
						progressValue.currentPath = entry.relativePath;
						const auto destination = staging / entry.relativePath;
						if (entry.directory)
						{
							fs::create_directories(destination, ec);
							if (ec)
							{
								cleanup();
								return {SaveOperationError::IoFailure, ec.message()};
							}
						}
						else
						{
							fs::create_directories(destination.parent_path(), ec);
							if (ec)
							{
								cleanup();
								return {SaveOperationError::IoFailure, ec.message()};
							}
							zip_file_t* file = zip_fopen_index(archive, entry.index, 0);
							if (!file)
							{
								cleanup();
								return {SaveOperationError::ArchiveInvalid,
										"unable to read save archive entry"};
							}
							std::ofstream output(destination, std::ios::binary | std::ios::trunc);
							if (!output)
							{
								zip_fclose(file);
								cleanup();
								return {SaveOperationError::IoFailure,
										"unable to create staged save file"};
							}
							std::uint64_t remaining = entry.size;
							while (remaining)
							{
								if (cancelled && cancelled())
								{
									zip_fclose(file);
									output.close();
									cleanup();
									return {SaveOperationError::Cancelled,
											"save import was cancelled"};
								}
								const auto requested = static_cast<zip_uint64_t>(
									std::min<std::uint64_t>(remaining, buffer.size()));
								const auto read = zip_fread(file, buffer.data(), requested);
								if (read != static_cast<zip_int64_t>(requested))
								{
									zip_fclose(file);
									output.close();
									cleanup();
									return {SaveOperationError::ArchiveInvalid,
											"save archive entry is truncated"};
								}
								output.write(buffer.data(), static_cast<std::streamsize>(read));
								if (!output)
								{
									zip_fclose(file);
									output.close();
									cleanup();
									return {SaveOperationError::IoFailure,
											"unable to write staged save file"};
								}
								remaining -= static_cast<std::uint64_t>(read);
								progressValue.bytesCompleted += static_cast<std::uint64_t>(read);
								if (progress)
									progress(progressValue);
							}
							output.close();
							if (!output || zip_fclose(file) != 0)
							{
								cleanup();
								return {SaveOperationError::ArchiveInvalid,
										"unable to finalize staged save file"};
							}
						}
						++progressValue.filesCompleted;
						if (progress)
							progress(progressValue);
					}

					auto metadataResult = StageSaveMetadata(titleId, [persistentId](pugi::xml_node info) {
							if (FindSaveAccountNode(info, persistentId))
								return false;
							const auto id = fmt::format("{:08x}", persistentId);
							auto account = info.append_child("account");
							account.append_attribute("persistentId").set_value(id.c_str());
							auto timestamp = account.append_child("timestamp");
							const auto unixSeconds = std::chrono::duration_cast<std::chrono::seconds>(
								std::chrono::system_clock::now().time_since_epoch()).count();
							constexpr std::int64_t unixToWiiEpoch = 946684800;
							timestamp.text().set(fmt::format("{:016x}",
								std::max<std::int64_t>(0, unixSeconds - unixToWiiEpoch)).c_str());
							return true; }, metadata);
					if (!metadataResult)
					{
						cleanup();
						return metadataResult;
					}

					const auto backup = targetState == SaveEntryState::Directory ? UniqueSiblingPath(target, "backup") : fs::path{};
					if (targetState == SaveEntryState::Directory && backup.empty())
					{
						cleanup();
						return {SaveOperationError::IoFailure,
								"unable to reserve save backup path"};
					}
					if (!backup.empty())
					{
						fs::rename(target, backup, ec);
						if (ec)
						{
							cleanup();
							return {SaveOperationError::IoFailure, ec.message()};
						}
					}
					fs::rename(staging, target, ec);
					if (ec)
					{
						std::error_code ignored;
						if (!backup.empty())
							fs::rename(backup, target, ignored);
						cleanup();
						return {SaveOperationError::IoFailure, ec.message()};
					}
					metadataResult = CommitSaveMetadata(metadata);
					if (!metadataResult)
					{
						RemovePathQuietly(target);
						std::error_code restoreError;
						if (!backup.empty())
							fs::rename(backup, target, restoreError);
						if (restoreError)
							metadataResult.diagnostic.append(fmt::format(
								"; save restore failed: {}", restoreError.message()));
						RemovePathQuietly(metadata.temporary);
						return metadataResult;
					}
					RemovePathQuietly(backup);
					return {};
				} catch (const std::exception& exception)
				{
					cleanup();
					return {SaveOperationError::BackendFailure, exception.what()};
				}
			}

			SaveOperationResult ExportSave(std::uint64_t titleId,
										   std::uint32_t persistentId, const fs::path& archivePath,
										   bool overwrite, SaveProgressHandler progress,
										   SaveCancellationCheck cancelled) override
			{
				if (persistentId < kMinimumPersistentId || archivePath.empty())
					return {SaveOperationError::InvalidPersistentId,
							"persistent id or archive path is invalid"};
				std::unique_lock<std::mutex> saveListLock;
				if (auto status = BeginSaveMutation(saveListLock); !status)
					return status;
				const auto source = SaveAccountPath(titleId, persistentId);
				if (InspectSaveEntryPath(source).state != SaveEntryState::Directory)
					return {SaveOperationError::NotFound, "save directory no longer exists"};

				std::error_code ec;
				const auto outputState = InspectSaveEntryPath(archivePath).state;
				if (outputState == SaveEntryState::Directory)
					return {SaveOperationError::InvalidTarget,
							"archive target is a directory"};
				if (outputState != SaveEntryState::Missing && !overwrite)
					return {SaveOperationError::TargetExists,
							"archive target already exists"};

				std::vector<std::pair<fs::path, bool>> entries;
				std::uint64_t bytesTotal{};
				for (fs::recursive_directory_iterator iterator(source, ec), end;
					 !ec && iterator != end; iterator.increment(ec))
				{
					const auto status = iterator->symlink_status(ec);
					if (ec || status.type() == fs::file_type::symlink ||
						(status.type() != fs::file_type::directory &&
						 status.type() != fs::file_type::regular))
						return {SaveOperationError::PathUnsafe,
								"save directory contains an unsupported entry"};
					const bool directory = status.type() == fs::file_type::directory;
					if (!directory)
					{
						const auto size = iterator->file_size(ec);
						if (ec || bytesTotal > kMaximumSaveArchiveTotalSize - size)
							return {SaveOperationError::IoFailure,
									"save data exceeds export limits"};
						bytesTotal += size;
					}
					entries.emplace_back(iterator->path(), directory);
				}
				if (ec)
					return {SaveOperationError::IoFailure, ec.message()};

				const auto staging = UniqueSiblingPath(archivePath, "exporting");
				if (staging.empty())
					return {SaveOperationError::IoFailure,
							"unable to reserve save export staging path"};
				int zipError{};
				zip_t* archive = zip_open(_pathToUtf8(staging).c_str(),
										  ZIP_CREATE | ZIP_TRUNCATE, &zipError);
				if (!archive)
					return {SaveOperationError::IoFailure,
							"unable to create save archive"};
				auto failArchive = [&](SaveOperationError error, std::string diagnostic) {
					zip_discard(archive);
					RemovePathQuietly(staging);
					return SaveOperationResult{error, std::move(diagnostic)};
				};

				SaveOperationProgress progressValue{
					.filesTotal = entries.size(), .bytesTotal = bytesTotal};
				for (const auto& [entryPath, directory] : entries)
				{
					if (cancelled && cancelled())
						return failArchive(SaveOperationError::Cancelled,
										   "save export was cancelled");
					const auto relative = fs::relative(entryPath, source, ec);
					if (ec || relative.empty() || relative.has_root_path())
						return failArchive(SaveOperationError::PathUnsafe,
										   "unable to derive safe archive path");
					const auto archiveName = relative.generic_string();
					progressValue.currentPath = relative;
					if (directory)
					{
						if (zip_dir_add(archive, archiveName.c_str(), ZIP_FL_ENC_UTF_8) < 0)
							return failArchive(SaveOperationError::IoFailure,
											   zip_strerror(archive));
					}
					else
					{
						zip_source_t* zipSource = zip_source_file(archive,
																  _pathToUtf8(entryPath).c_str(), 0, 0);
						if (!zipSource)
							return failArchive(SaveOperationError::IoFailure,
											   zip_strerror(archive));
						if (zip_file_add(archive, archiveName.c_str(), zipSource,
										 ZIP_FL_ENC_UTF_8) < 0)
						{
							zip_source_free(zipSource);
							return failArchive(SaveOperationError::IoFailure,
											   zip_strerror(archive));
						}
						progressValue.bytesCompleted += fs::file_size(entryPath, ec);
						if (ec)
							return failArchive(SaveOperationError::IoFailure, ec.message());
					}
					++progressValue.filesCompleted;
					if (progress)
						progress(progressValue);
				}

				const std::string metadata = fmt::format("titleId = {:#016x}", titleId);
				zip_source_t* metadataSource = zip_source_buffer(archive, metadata.data(),
																 metadata.size(), 0);
				if (!metadataSource || zip_file_add(archive, "cemu_meta", metadataSource,
													ZIP_FL_ENC_UTF_8) < 0)
				{
					if (metadataSource)
						zip_source_free(metadataSource);
					return failArchive(SaveOperationError::IoFailure,
									   zip_strerror(archive));
				}
				if (zip_close(archive) != 0)
					return failArchive(SaveOperationError::IoFailure,
									   "unable to finalize save archive");
				archive = nullptr;

				const auto backup = outputState == SaveEntryState::Missing ? fs::path{} : UniqueSiblingPath(archivePath, "backup");
				if (outputState != SaveEntryState::Missing && backup.empty())
				{
					RemovePathQuietly(staging);
					return {SaveOperationError::IoFailure,
							"unable to reserve archive backup path"};
				}
				if (!backup.empty())
				{
					fs::rename(archivePath, backup, ec);
					if (ec)
					{
						RemovePathQuietly(staging);
						return {SaveOperationError::IoFailure, ec.message()};
					}
				}
				fs::rename(staging, archivePath, ec);
				if (ec)
				{
					std::error_code restoreError;
					if (!backup.empty())
						fs::rename(backup, archivePath, restoreError);
					RemovePathQuietly(staging);
					return {SaveOperationError::IoFailure,
							restoreError ? fmt::format("{}; restore failed: {}", ec.message(),
													   restoreError.message())
										 : ec.message()};
				}
				RemovePathQuietly(backup);
				return {};
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

			GraphicPackInstallResult InstallGraphicPacks(
				const GraphicPackInstallRequest& request,
				GraphicPackInstallProgressHandler progress,
				GraphicPackInstallCancellationCheck cancelled) override
			{
				if (IsTitleRunning())
					return {GraphicPackInstallError::Conflict,
							"graphic packs cannot be installed while a title is running"};
				GraphicPackInstallTransaction transaction;
				auto result = InstallGraphicPackFiles(request, progress, cancelled, &transaction);
				if (!result || result.upToDate)
					return result;
				try
				{
					if (cancelled && cancelled())
					{
						const auto rollback = transaction.Rollback();
						return rollback ? GraphicPackInstallResult{GraphicPackInstallError::Cancelled,
																   "graphic-pack installation was cancelled"}
										: rollback;
					}
					if (progress)
						progress({GraphicPackInstallPhase::Refreshing, 0, 0, {}});
					const auto refresh = RefreshGraphicPacks();
					if (!refresh)
					{
						const auto rollback = transaction.Rollback();
						if (!rollback)
							return rollback;
						return {GraphicPackInstallError::IoFailure,
								refresh.diagnostic.empty() ? "installed packs could not be refreshed" : refresh.diagnostic};
					}
					transaction.Commit();
					result.removedEnabledPaths = refresh.removedEnabledPaths;
					return result;
				} catch (const std::exception& error)
				{
					const auto rollback = transaction.Rollback();
					if (!rollback)
						return rollback;
					return {GraphicPackInstallError::IoFailure,
							fmt::format("graphic-pack refresh failed and was rolled back: {}", error.what())};
				} catch (...)
				{
					const auto rollback = transaction.Rollback();
					return rollback ? GraphicPackInstallResult{GraphicPackInstallError::IoFailure,
															   "graphic-pack refresh failed and was rolled back"}
									: rollback;
				}
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
			struct DiagnosticSuspension
			{
				std::uint64_t identity{};
				std::int32_t expectedCounter{};
			};

			static std::uint64_t PpcThreadIdentity(const OSThread_t& thread)
			{
				std::uint64_t value = thread.entrypoint.GetMPTR();
				value = (value * 0x9e3779b185ebca87ULL) ^ thread.stackEnd.GetMPTR();
				value = (value * 0x9e3779b185ebca87ULL) ^ thread.stackBase.GetMPTR();
				value = (value * 0x9e3779b185ebca87ULL) ^ thread.threadName.GetMPTR();
				return value == 0 ? 1 : value;
			}

			std::uint64_t TokenForInputController(
				const std::shared_ptr<ControllerBase>& controller) const
			{
				if (!controller)
					return 0;
				if (const auto found = m_inputTokensByPointer.find(controller.get());
					found != m_inputTokensByPointer.end())
					return found->second;
				const auto token = ++m_nextInputDeviceToken;
				m_inputDeviceTokens.emplace(token, controller);
				m_inputTokensByPointer.emplace(controller.get(), token);
				return token;
			}

			std::shared_ptr<ControllerBase> InputControllerForToken(
				std::uint64_t token) const
			{
				const auto found = m_inputDeviceTokens.find(token);
				if (found == m_inputDeviceTokens.end())
					return {};
				return found->second;
			}

			void PruneUnassignedInputTokens() const
			{
				for (auto it = m_inputDeviceTokens.begin(); it != m_inputDeviceTokens.end();)
				{
					if (it->second.use_count() == 1)
					{
						m_inputTokensByPointer.erase(it->second.get());
						m_armedInputCaptures.erase(it->first);
						it = m_inputDeviceTokens.erase(it);
					}
					else
						++it;
				}
			}

			ApplicationEvents& m_events;
			std::shared_ptr<ApplicationEventForwarder> m_eventForwarder;
			mutable std::shared_mutex m_inputLifecycleMutex;
			mutable std::mutex m_frontendSettingsStateMutex;
			mutable std::recursive_mutex m_frontendSettingsTransactionMutex;
			mutable std::string m_frontendSettingsFingerprint;
			mutable std::uint64_t m_frontendSettingsRevision{1};
			mutable std::recursive_mutex m_inputSettingsMutex;
			mutable std::recursive_mutex m_hotkeySettingsMutex;
			mutable std::uint64_t m_hotkeySettingsRevision{1};
			mutable std::unordered_map<std::uint64_t, std::shared_ptr<ControllerBase>>
				m_inputDeviceTokens;
			mutable std::unordered_map<const ControllerBase*, std::uint64_t>
				m_inputTokensByPointer;
			mutable std::uint64_t m_nextInputDeviceToken{};
			mutable std::uint64_t m_inputSettingsGeneration{1};
			mutable std::uint64_t m_inputSettingsFingerprintGeneration{};
			std::atomic_uint64_t m_diagnosticsGeneration{};
			std::vector<std::pair<std::uint32_t, std::uint64_t>> m_diagnosticsThreads;
			std::unordered_map<std::uint32_t, DiagnosticSuspension> m_diagnosticsSuspensions;
			mutable std::string m_inputSettingsFingerprint;
			mutable std::unordered_set<std::uint64_t> m_armedInputCaptures;
			bool m_inputAvailable{};
		};
	} // namespace

	std::unique_ptr<IEmulationBackend> CreateCafeEmulationBackend(ApplicationEvents& events)
	{
		return std::make_unique<CafeEmulationBackend>(events);
	}
} // namespace Application
