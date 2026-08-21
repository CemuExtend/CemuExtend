#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Application
{
	inline constexpr std::uint32_t kMinimumPersistentId = 0x80000001U;
	inline constexpr std::size_t kMaximumAccountCount = 12;

	enum class AccountOnlineError : std::uint8_t
	{
		None,
		NoAccountId,
		NoPasswordCached,
		PasswordCacheEmpty,
		NoPrincipalId,
	};

	enum class AccountFileState : std::uint8_t
	{
		Missing,
		Corrupted,
		Ok,
	};

	struct AccountInfo
	{
		std::uint32_t persistentId{};
		std::wstring miiName;
		std::uint16_t birthYear{};
		std::uint8_t birthMonth{};
		std::uint8_t birthDay{};
		std::uint8_t gender{};
		std::string email;
		std::uint32_t country{};
		bool validOnlineAccount{};
	};

	struct AccountUpdate
	{
		std::wstring miiName;
		std::uint16_t birthYear{};
		std::uint8_t birthMonth{};
		std::uint8_t birthDay{};
		std::uint8_t gender{};
		std::string email;
		std::uint32_t country{};
	};

	struct AccountCountry
	{
		std::uint32_t code{};
		std::string name;
	};

	struct AccountValidation
	{
		bool validAccount{};
		AccountFileState otp{AccountFileState::Missing};
		AccountFileState seeprom{AccountFileState::Missing};
		std::vector<std::wstring> missingFiles;
		AccountOnlineError accountError{AccountOnlineError::None};

		[[nodiscard]] explicit operator bool() const
		{
			return validAccount && otp == AccountFileState::Ok &&
				seeprom == AccountFileState::Ok && missingFiles.empty();
		}
	};

	struct OnlineEnvironmentStatus
	{
		bool requiredFilesAvailable{};
		bool otpPresent{};
		bool seepromPresent{};
		bool consoleCertificateAvailable{};
	};

	enum class AccountNetworkService : std::uint8_t
	{
		Offline,
		Nintendo,
		Pretendo,
		Custom,
		Plasma,
	};

	struct AccountNetworkSetting
	{
		std::uint32_t persistentId{};
		AccountNetworkService service{AccountNetworkService::Offline};
		AccountValidation validation;
	};

	struct AccountManagerSnapshot
	{
		std::vector<AccountInfo> accounts;
		std::vector<AccountCountry> countries;
		std::vector<AccountNetworkSetting> networkSettings;
		OnlineEnvironmentStatus onlineEnvironment;
		std::uint32_t activePersistentId{};
		std::uint32_t nextPersistentId{};
		bool hasFreeSlots{};
		bool titleRunning{};
	};

	enum class AccountOperationError : std::uint8_t
	{
		None,
		InvalidPersistentId,
		DuplicatePersistentId,
		NoFreeSlots,
		CannotDeleteOnlyAccount,
		TitleRunning,
		NotFound,
		InvalidMiiName,
		IoFailure,
		BackendFailure,
	};

	enum class DownloadAccountError : std::uint8_t
	{
		None,
		AccountNotFound,
		InvalidCredentials,
		OnlineFilesMissing,
	};

	struct DownloadAccountContext
	{
		DownloadAccountError error{DownloadAccountError::None};
		std::string accountName;
		std::array<std::uint8_t, 32> passwordHash{};
		std::uint32_t region{};
		std::string country;
		std::uint32_t deviceId{};
		std::string serial;
		std::string deviceCertificateBase64;

		[[nodiscard]] explicit operator bool() const
		{
			return error == DownloadAccountError::None;
		}
	};

	struct AccountOperationResult
	{
		AccountOperationError error{AccountOperationError::None};
		std::string diagnostic;
		std::optional<AccountInfo> account;

		[[nodiscard]] explicit operator bool() const
		{
			return error == AccountOperationError::None;
		}
	};

	class IAccountService
	{
	public:
		virtual ~IAccountService() = default;
		[[nodiscard]] virtual std::vector<AccountInfo> ListAccounts() const = 0;
		[[nodiscard]] virtual std::optional<AccountInfo> GetAccount(
			std::uint32_t persistentId) const = 0;
		[[nodiscard]] virtual std::uint32_t NextPersistentId() const = 0;
		[[nodiscard]] virtual bool HasFreeAccountSlots() const = 0;
		[[nodiscard]] virtual std::vector<AccountCountry> ListAccountCountries() const = 0;
		[[nodiscard]] virtual OnlineEnvironmentStatus GetOnlineEnvironmentStatus() const = 0;
		[[nodiscard]] virtual AccountManagerSnapshot GetAccountManagerSnapshot() const = 0;
		[[nodiscard]] virtual AccountOperationResult SetActiveAccount(
			std::uint32_t persistentId) = 0;
		[[nodiscard]] virtual AccountOperationResult SetAccountNetworkService(
			std::uint32_t persistentId, AccountNetworkService service) = 0;
		[[nodiscard]] virtual DownloadAccountContext GetDownloadAccountContext(
			std::optional<std::uint32_t> persistentId) const = 0;
		[[nodiscard]] virtual AccountValidation ValidateOnlineAccount(
			std::uint32_t persistentId) const = 0;
		[[nodiscard]] virtual AccountOperationResult CreateAccount(
			std::uint32_t persistentId, std::wstring_view miiName) = 0;
		[[nodiscard]] virtual AccountOperationResult UpdateAccount(
			std::uint32_t persistentId, const AccountUpdate& update) = 0;
		[[nodiscard]] virtual AccountOperationResult DeleteAccount(
			std::uint32_t persistentId) = 0;
	};
}
