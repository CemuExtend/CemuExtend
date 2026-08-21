#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Application
{
	enum class ContentRole : std::uint8_t
	{
		Base,
		Update,
		Dlc,
	};

	struct WuaContentItem
	{
		std::uint64_t locationUid{};
		std::uint64_t titleId{};
		std::uint32_t version{};
		std::uint64_t fingerprint{};
		ContentRole role{ContentRole::Base};
		std::string displayPath;
	};

	struct WuaConversionPlan
	{
		std::vector<WuaContentItem> items;
		std::string suggestedFileName;
	};

	enum class ContentOperationPhase : std::uint8_t
	{
		Counting,
		Collecting,
		Converting,
		Hashing,
		Finalizing,
	};

	struct ContentOperationProgress
	{
		ContentOperationPhase phase{ContentOperationPhase::Counting};
		std::uint32_t filesCompleted{};
		std::uint32_t filesTotal{};
		std::uint64_t bytesCompleted{};
		std::uint64_t bytesTotal{};
	};

	enum class ContentOperationError : std::uint8_t
	{
		None,
		NotFound,
		Cancelled,
		UnableToCreateOutput,
		ReadFailure,
		VerificationFailure,
		RenameFailure,
	};

	struct ContentOperationResult
	{
		ContentOperationError error{ContentOperationError::None};
		std::string diagnostic;

		[[nodiscard]] explicit operator bool() const
		{
			return error == ContentOperationError::None;
		}
	};

	struct ContentChecksumFile
	{
		std::string path;
		std::string sha256;
	};

	struct ContentChecksum
	{
		std::uint64_t titleId{};
		std::uint32_t version{};
		std::uint32_t region{};
		std::string imageSha256;
		std::vector<ContentChecksumFile> files;
	};

	struct ContentChecksumResult
	{
		ContentOperationError error{ContentOperationError::None};
		std::string diagnostic;
		std::optional<ContentChecksum> checksum;

		[[nodiscard]] explicit operator bool() const
		{
			return error == ContentOperationError::None && checksum.has_value();
		}
	};

	using ContentProgressHandler = std::function<void(const ContentOperationProgress&)>;
	using ContentCancellationCheck = std::function<bool()>;

	class IContentOperations
	{
	  public:
		virtual ~IContentOperations() = default;
		[[nodiscard]] virtual std::optional<WuaConversionPlan> PlanWuaConversion(
			std::uint64_t titleId, std::uint64_t preferredLocationUid) const = 0;
		[[nodiscard]] virtual ContentOperationResult ConvertToWua(
			const WuaConversionPlan& plan,
			const std::filesystem::path& outputPath,
			ContentProgressHandler progress,
			ContentCancellationCheck cancelled) = 0;
		[[nodiscard]] virtual ContentChecksumResult ComputeTitleChecksum(
			std::uint64_t locationUid, ContentProgressHandler progress,
			ContentCancellationCheck cancelled) = 0;
	};
} // namespace Application
