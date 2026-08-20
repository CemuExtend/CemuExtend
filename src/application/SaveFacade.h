#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Application
{
	enum class SaveEntryState : std::uint8_t
	{
		Missing,
		Directory,
		NonDirectory,
	};

	enum class SaveOperationError : std::uint8_t
	{
		None,
		InvalidPersistentId,
		TitleRunning,
		Scanning,
		NotFound,
		TargetExists,
		InvalidTarget,
		ArchiveInvalid,
		PathUnsafe,
		Cancelled,
		IoFailure,
		MetadataFailure,
		BackendFailure,
	};

	struct SaveEntryLocation
	{
		SaveEntryState state{SaveEntryState::Missing};
		std::filesystem::path path;
	};

	struct SaveImportInspection
	{
		SaveOperationError error{SaveOperationError::None};
		std::string diagnostic;
		std::optional<std::uint64_t> sourceTitleId;
		SaveEntryLocation target;

		[[nodiscard]] explicit operator bool() const
		{
			return error == SaveOperationError::None;
		}
	};

	struct SaveOperationProgress
	{
		std::uint64_t filesCompleted{};
		std::uint64_t filesTotal{};
		std::uint64_t bytesCompleted{};
		std::uint64_t bytesTotal{};
		std::filesystem::path currentPath;
	};

	// Operations invoke these callbacks synchronously while holding their
	// serialization locks. Callbacks must not call back into EmulationController.
	using SaveProgressHandler = std::function<void(const SaveOperationProgress&)>;
	using SaveCancellationCheck = std::function<bool()>;

	struct SaveOperationResult
	{
		SaveOperationError error{SaveOperationError::None};
		std::string diagnostic;

		[[nodiscard]] explicit operator bool() const
		{
			return error == SaveOperationError::None;
		}
	};

	class ISaveService
	{
	public:
		virtual ~ISaveService() = default;
		[[nodiscard]] virtual std::vector<std::uint32_t> ListSavePersistentIds(
			std::uint64_t titleId) const = 0;
		[[nodiscard]] virtual SaveEntryLocation InspectSaveEntry(
			std::uint64_t titleId, std::uint32_t persistentId) const = 0;
		[[nodiscard]] virtual SaveImportInspection InspectSaveImport(
			const std::filesystem::path& archivePath, std::uint64_t titleId,
			std::uint32_t persistentId) const = 0;
		[[nodiscard]] virtual SaveOperationResult DeleteSave(
			std::uint64_t titleId, std::uint32_t persistentId) = 0;
		[[nodiscard]] virtual SaveOperationResult TransferSave(
			std::uint64_t titleId, std::uint32_t sourcePersistentId,
			std::uint32_t targetPersistentId, bool overwrite) = 0;
		[[nodiscard]] virtual SaveOperationResult ImportSave(
			const std::filesystem::path& archivePath, std::uint64_t titleId,
			std::uint32_t persistentId, bool overwrite,
			SaveProgressHandler progress = {},
			SaveCancellationCheck cancelled = {}) = 0;
		[[nodiscard]] virtual SaveOperationResult ExportSave(
			std::uint64_t titleId, std::uint32_t persistentId,
			const std::filesystem::path& archivePath, bool overwrite,
			SaveProgressHandler progress = {},
			SaveCancellationCheck cancelled = {}) = 0;
	};
}
