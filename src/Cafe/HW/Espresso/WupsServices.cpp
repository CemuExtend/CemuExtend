#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/WupsRuntime.h"
#include "Cafe/HW/Espresso/WupsBackendAbi.h"
#include "Cafe/HW/Espresso/WupsBackendManagement.h"
#include "Cafe/OS/libs/cemuextend/Cex2Owner.h"
#include "Cemu/Logging/CemuLogging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#if BOOST_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
	constexpr std::size_t kMaximumGuestString = 4096;
	constexpr std::size_t kMaximumStorageKey = 127;
	constexpr std::size_t kMaximumStorageDepth = 16;
	constexpr std::size_t kMaximumStorageValue = 256U * 1024U;
	constexpr std::size_t kMaximumConfigName = 256;
	constexpr std::size_t kMaximumLogMessage = 4096;
	constexpr std::size_t kMaximumContentPath = 1024;
	constexpr std::uint32_t kMaximumComboHoldMilliseconds = 60U * 60U * 1000U;
	constexpr std::uint32_t kStorageContainerType = 0xffffffffU;
	constexpr std::array<std::byte, 8> kStorageMagic{
		std::byte{'C'}, std::byte{'W'}, std::byte{'U'}, std::byte{'P'},
		std::byte{'S'}, std::byte{'S'}, std::byte{'T'}, std::byte{'1'}};

	thread_local const void* s_activeServiceOwner{};

	[[nodiscard]] std::string Lower(std::string_view value)
	{
		std::string result(value);
		std::ranges::transform(result, result.begin(), [](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
		return result;
	}

	[[nodiscard]] bool ValidAbiName(std::string_view value)
	{
		return !value.empty() && value.size() <= 128 &&
			   std::ranges::all_of(value, [](unsigned char character) {
				   return std::isalnum(character) || character == '_' ||
						  character == '.' || character == '-';
			   });
	}

	[[nodiscard]] bool AddU32(std::uint32_t left, std::uint32_t right,
							  std::uint32_t& result)
	{
		const auto sum = static_cast<std::uint64_t>(left) + right;
		if (sum > std::numeric_limits<std::uint32_t>::max())
			return false;
		result = static_cast<std::uint32_t>(sum);
		return true;
	}

	[[nodiscard]] bool IsPowerOfTwo(std::uint32_t value)
	{
		return value != 0 && (value & (value - 1U)) == 0;
	}

	[[nodiscard]] bool ValidStorageKey(std::string_view key)
	{
		if (key.empty() || key.size() > kMaximumStorageKey ||
			key == "." || key == "..")
			return false;
		for (const unsigned char character : key)
			if (character == 0 || character == '/' || character == '\\' ||
				character < 0x20 || character == 0x7f)
				return false;
		return true;
	}

	[[nodiscard]] bool ValidStorageId(std::string_view id)
	{
		return ValidStorageKey(id) && id.size() <= 128;
	}

	[[nodiscard]] bool ValidDisplayName(std::string_view name)
	{
		if (name.empty() || name.size() > kMaximumConfigName)
			return false;
		return std::ranges::none_of(name, [](unsigned char character) {
			return character == 0 || character < 0x20;
		});
	}

	[[nodiscard]] std::string HexEncode(std::string_view value)
	{
		static constexpr char digits[] = "0123456789abcdef";
		std::string encoded;
		encoded.reserve(value.size() * 2);
		for (const unsigned char character : value)
		{
			encoded.push_back(digits[character >> 4]);
			encoded.push_back(digits[character & 0xf]);
		}
		return encoded;
	}

	[[nodiscard]] std::string HexU64(std::uint64_t value)
	{
		return fmt::format("{:016x}", value);
	}

	void AppendU16(std::vector<std::byte>& output, std::uint16_t value)
	{
		output.push_back(static_cast<std::byte>(value >> 8));
		output.push_back(static_cast<std::byte>(value));
	}

	void AppendU32(std::vector<std::byte>& output, std::uint32_t value)
	{
		output.push_back(static_cast<std::byte>(value >> 24));
		output.push_back(static_cast<std::byte>(value >> 16));
		output.push_back(static_cast<std::byte>(value >> 8));
		output.push_back(static_cast<std::byte>(value));
	}

	[[nodiscard]] std::uint16_t ReadU16(std::span<const std::byte> input,
										std::size_t offset)
	{
		return static_cast<std::uint16_t>(
			(std::to_integer<std::uint16_t>(input[offset]) << 8) |
			std::to_integer<std::uint16_t>(input[offset + 1]));
	}

	[[nodiscard]] std::uint32_t ReadU32(std::span<const std::byte> input,
										std::size_t offset)
	{
		return (std::to_integer<std::uint32_t>(input[offset]) << 24) |
			   (std::to_integer<std::uint32_t>(input[offset + 1]) << 16) |
			   (std::to_integer<std::uint32_t>(input[offset + 2]) << 8) |
			   std::to_integer<std::uint32_t>(input[offset + 3]);
	}

	[[nodiscard]] std::uint32_t Crc32(std::span<const std::byte> input)
	{
		std::uint32_t crc = 0xffffffffU;
		for (const auto raw : input)
		{
			crc ^= std::to_integer<std::uint8_t>(raw);
			for (unsigned bit = 0; bit < 8; ++bit)
				crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
		}
		return ~crc;
	}

	[[nodiscard]] bool PathStartsWith(const std::filesystem::path& path,
									  const std::filesystem::path& root)
	{
		auto pathIterator = path.begin();
		for (auto rootIterator = root.begin(); rootIterator != root.end();
			 ++rootIterator, ++pathIterator)
			if (pathIterator == path.end() || *pathIterator != *rootIterator)
				return false;
		return true;
	}

	[[nodiscard]] std::optional<std::string> NormalizeVirtualPath(
		std::string_view raw)
	{
		if (raw.empty() || raw.size() > kMaximumContentPath || raw.front() != '/' ||
			raw.find('\\') != std::string_view::npos ||
			raw.find('\0') != std::string_view::npos)
			return std::nullopt;
		std::vector<std::string_view> components;
		std::size_t begin = 1;
		while (begin <= raw.size())
		{
			const auto end = raw.find('/', begin);
			const auto component = raw.substr(begin,
											  end == std::string_view::npos ? raw.size() - begin : end - begin);
			if (!component.empty() && component != ".")
			{
				if (component == ".." ||
					std::ranges::any_of(component, [](unsigned char character) {
						return character < 0x20 || character == 0x7f;
					}))
					return std::nullopt;
				components.push_back(component);
			}
			if (end == std::string_view::npos)
				break;
			begin = end + 1;
		}
		std::string normalized{"/"};
		for (std::size_t index = 0; index < components.size(); ++index)
		{
			if (index != 0)
				normalized.push_back('/');
			normalized.append(components[index]);
		}
		return normalized;
	}

	[[nodiscard]] bool ValidateStorageValue(const WupsStorageValue& value)
	{
		if (value.bytes.size() > kMaximumStorageValue)
			return false;
		switch (value.type)
		{
		case WupsStorageValueType::Signed32:
		case WupsStorageValueType::Unsigned32:
		case WupsStorageValueType::Float:
			return value.bytes.size() == 4;
		case WupsStorageValueType::Signed64:
		case WupsStorageValueType::Unsigned64:
		case WupsStorageValueType::Double:
			return value.bytes.size() == 8;
		case WupsStorageValueType::Boolean:
			return value.bytes.size() == 1;
		case WupsStorageValueType::String:
			return value.bytes.size() <= kMaximumGuestString &&
				   std::ranges::none_of(value.bytes, [](std::byte value) {
					   return value == std::byte{};
				   });
		case WupsStorageValueType::Binary:
			return true;
		}
		return false;
	}

	[[nodiscard]] std::int32_t StorageAbiResult(WupsServiceStatus status)
	{
		switch (status)
		{
		case WupsServiceStatus::Success:
			return 0;
		case WupsServiceStatus::InvalidArgument:
		case WupsServiceStatus::OwnerMismatch:
		case WupsServiceStatus::StaleGeneration:
		case WupsServiceStatus::PermissionDenied:
			return -0x01;
		case WupsServiceStatus::BufferTooSmall:
			return -0x04;
		case WupsServiceStatus::AlreadyExists:
			return -0x05;
		case WupsServiceStatus::NotFound:
			return -0x10;
		case WupsServiceStatus::IoError:
		case WupsServiceStatus::CorruptData:
			return -0x06;
		default:
			return -0x100;
		}
	}

	[[nodiscard]] std::int32_t ConfigAbiResult(WupsServiceStatus status)
	{
		switch (status)
		{
		case WupsServiceStatus::Success:
			return 0;
		case WupsServiceStatus::InvalidArgument:
		case WupsServiceStatus::OwnerMismatch:
		case WupsServiceStatus::PermissionDenied:
			return -0x01;
		case WupsServiceStatus::NotFound:
		case WupsServiceStatus::StaleGeneration:
			return -0x06;
		case WupsServiceStatus::Unsupported:
			return -0x83;
		case WupsServiceStatus::LimitExceeded:
			return -0x03;
		default:
			return -0x100;
		}
	}

	[[nodiscard]] std::int32_t ComboAbiResult(WupsServiceStatus status)
	{
		switch (status)
		{
		case WupsServiceStatus::Success:
		case WupsServiceStatus::Conflict:
			return 0;
		case WupsServiceStatus::InvalidArgument:
		case WupsServiceStatus::OwnerMismatch:
		case WupsServiceStatus::PermissionDenied:
			return -0x01;
		case WupsServiceStatus::NotFound:
		case WupsServiceStatus::StaleGeneration:
			return -0x03;
		case WupsServiceStatus::Busy:
			return -0x04;
		default:
			return -0x100;
		}
	}

	[[nodiscard]] std::int32_t NotificationAbiResult(WupsServiceStatus status)
	{
		switch (status)
		{
		case WupsServiceStatus::Success:
			return 0;
		case WupsServiceStatus::InvalidArgument:
		case WupsServiceStatus::OwnerMismatch:
		case WupsServiceStatus::PermissionDenied:
			return -0x04;
		case WupsServiceStatus::Unsupported:
			return -0x06;
		case WupsServiceStatus::LimitExceeded:
			return -0x12;
		case WupsServiceStatus::NotFound:
		case WupsServiceStatus::StaleGeneration:
			return -0x13;
		default:
			return -0x1000;
		}
	}

	[[nodiscard]] std::int32_t ContentAbiResult(WupsServiceStatus status)
	{
		switch (status)
		{
		case WupsServiceStatus::Success:
			return 0;
		case WupsServiceStatus::InvalidArgument:
		case WupsServiceStatus::OwnerMismatch:
		case WupsServiceStatus::PermissionDenied:
			return -0x10;
		case WupsServiceStatus::LimitExceeded:
			return -0x11;
		case WupsServiceStatus::NotFound:
		case WupsServiceStatus::StaleGeneration:
			return -0x13;
		case WupsServiceStatus::Unsupported:
			return -0x21;
		default:
			return -0x1000;
		}
	}

	[[nodiscard]] std::int32_t FunctionPatcherAbiResult(WupsServiceStatus status)
	{
		switch (status)
		{
		case WupsServiceStatus::Success:
			return 0;
		case WupsServiceStatus::InvalidArgument:
		case WupsServiceStatus::OwnerMismatch:
		case WupsServiceStatus::PermissionDenied:
			return -0x10;
		case WupsServiceStatus::NotFound:
		case WupsServiceStatus::StaleGeneration:
			return -0x11;
		case WupsServiceStatus::UnsupportedVersion:
			return -0x12;
		case WupsServiceStatus::Unsupported:
			return -0x21;
		default:
			return -0x1000;
		}
	}

	class UnsupportedFunctionPatcher final : public IWupsFunctionPatcherFacade
	{
	  public:
		std::uint32_t ApiVersion() const override
		{
			return 2;
		}

		WupsServiceStatus AddPatch(WupsOwnerToken, std::uint32_t, bool,
								   std::uint32_t&, bool&, std::string& error) override
		{
			error = "FunctionPatcher provider is not connected";
			return WupsServiceStatus::Unsupported;
		}

		WupsServiceStatus RemovePatch(WupsOwnerToken, std::uint32_t,
									  std::string& error) override
		{
			error = "FunctionPatcher provider is not connected";
			return WupsServiceStatus::Unsupported;
		}

		WupsServiceStatus IsPatchApplied(WupsOwnerToken, std::uint32_t,
										 bool&, std::string& error) const override
		{
			error = "FunctionPatcher provider is not connected";
			return WupsServiceStatus::Unsupported;
		}

		void ReleaseOwner(WupsOwnerToken) override
		{
		}
	};
} // namespace

struct AromaCompatibilityRuntime::Impl
{
	struct StorageNode
	{
		std::uint32_t handle{};
		std::uint32_t parent{};
		std::string key;
		bool container{};
		std::map<std::string, std::uint32_t, std::less<>> children;
		WupsStorageValue value;
	};

	struct StorageState
	{
		std::mutex mutex;
		std::unordered_map<std::uint32_t, StorageNode> nodes;
		std::uint32_t root{};
		std::filesystem::path path;
		bool loaded{};
		bool dirty{};
		std::uint64_t revision{};
	};

	struct ConfigCategory
	{
		std::uint32_t handle{};
		std::string name;
		bool attached{};
		std::vector<std::uint32_t> categories;
		std::vector<std::uint32_t> items;
	};

	struct ConfigItem
	{
		WupsConfigItemModel model;
		bool attached{};
	};

	struct ConfigState
	{
		std::mutex mutex;
		std::string name;
		std::uint32_t openCallback{};
		std::uint32_t closeCallback{};
		std::uint32_t root{};
		bool registered{};
		bool menuOpen{};
		std::unordered_map<std::uint32_t, ConfigCategory> categories;
		std::unordered_map<std::uint32_t, ConfigItem> items;
	};

	struct ComboState
	{
		std::uint32_t handle{};
		WupsButtonComboDefinition definition;
		WupsButtonComboStatus status{WupsButtonComboStatus::Invalid};
		bool holdFired{};
	};

	struct ReentState
	{
		std::uint64_t thread{};
		std::uint32_t pluginId{};
		std::uint32_t context{};
		std::uint32_t cleanup{};
	};

	struct Owner final : cemuextend_hle::Cex2Owner
	{
		WupsOwnerToken token;
		CemodPackage package;
		CemodNativePermissions permissions;
		std::string packageId;
		std::string principal;
		std::uint64_t titleId{};
		std::uint32_t pluginIdentifier{};
		WupsMetadata metadata;
		std::mutex mutex;
		std::mutex storageLoadMutex;
		std::recursive_mutex exportRegistrationMutex;
		std::condition_variable pinsChanged;
		std::size_t pins{};
		bool closing{};
		bool finalized{};
		bool deferredFinalize{};
		WupsGuestInvoker invoker;
		std::shared_ptr<StorageState> storage{std::make_shared<StorageState>()};
		ConfigState config;
		std::unordered_map<std::uint32_t, ComboState> combos;
		std::map<std::pair<std::uint64_t, std::uint32_t>, ReentState> reent;
		std::unordered_map<std::uint32_t, WupsMappedMemoryInfo> mappings;
		std::unordered_map<std::uint32_t, WupsMappedMemoryInfo> mappingsBeingFreed;
		std::unordered_map<std::uint32_t, WupsNotificationModel> notifications;
		std::unordered_map<std::uint32_t, WupsContentRedirectRule> redirects;
		std::map<std::pair<std::string, std::string>, std::uint32_t> functionExports;
		std::set<std::pair<std::string, std::string>> functionExportsRegistering;
		std::map<std::pair<std::string, std::string>, std::uint32_t> dataExports;
		std::vector<ModuleExportLease> importLeases;
		std::vector<std::uint32_t> guestData;
		std::deque<std::chrono::steady_clock::time_point> recentLogs;
		std::size_t mappedBytes{};
		std::size_t mappedBytesReserved{};
		std::size_t pendingCallbacks{};
		std::atomic_bool stopped{};
		std::atomic_uint32_t grantedPermissions{};

		std::uint64_t AddressSpaceId() const override
		{
			return 0x4000000000000000ULL | token.owner;
		}
		std::uint32_t Generation() const override
		{
			return token.generation;
		}
		const std::string& Principal() const override
		{
			return principal;
		}
		std::uint64_t TitleId() const override
		{
			return titleId;
		}
		bool IsStopped() const override
		{
			return stopped.load(std::memory_order_acquire);
		}
		const CemodPackage* Package() const override
		{
			return &package;
		}
		std::uint32_t GrantedPermissions() const override
		{
			return grantedPermissions.load(std::memory_order_acquire);
		}
		void SetGrantedPermissions(std::uint32_t value) override
		{
			grantedPermissions.store(value, std::memory_order_release);
		}
		bool IsServiceAllowed(std::uint16_t service, std::uint32_t permission,
							  std::uint16_t operation) const override
		{
			if (service == 1 || permission == 0)
				return true;
			if (service < 2 || service > 12)
				return false;
			const auto bit = 1U << (service - 1U);
			if (permission == 1)
				return (package.serviceReadMask & bit) != 0;
			if (permission == 2)
				return (package.serviceWriteMask & bit) != 0;
			if (permission == 4)
				return (package.serviceInjectMask & bit) != 0;
			if (permission == 8)
				return (((operation == 1 ? package.serviceReadMask : package.serviceWriteMask) & bit) != 0);
			if (permission == 16)
				return (package.serviceReadMask & bit) != 0;
			return false;
		}
	};

	struct PendingCallback
	{
		explicit PendingCallback(std::shared_ptr<Owner> owner_) : owner(std::move(owner_))
		{
			std::lock_guard lock(owner->mutex);
			++owner->pendingCallbacks;
		}

		~PendingCallback()
		{
			std::lock_guard lock(owner->mutex);
			cemu_assert_debug(owner->pendingCallbacks != 0);
			if (owner->pendingCallbacks != 0)
				--owner->pendingCallbacks;
		}

		std::shared_ptr<Owner> owner;
	};

	struct AsyncGate
	{
		std::mutex mutex;
		std::condition_variable idle;
		Impl* impl{};
		std::size_t active{};
	};

	struct AsyncLease
	{
		AsyncLease() = default;
		AsyncLease(std::shared_ptr<AsyncGate> gate_, Impl* impl_) : gate(std::move(gate_)), impl(impl_)
		{
		}
		AsyncLease(AsyncLease&& other) noexcept : gate(std::move(other.gate)), impl(std::exchange(other.impl, nullptr))
		{
		}
		AsyncLease& operator=(AsyncLease&&) = delete;
		AsyncLease(const AsyncLease&) = delete;
		AsyncLease& operator=(const AsyncLease&) = delete;
		std::shared_ptr<AsyncGate> gate;
		Impl* impl{};
		~AsyncLease()
		{
			if (!gate || !impl)
				return;
			std::lock_guard lock(gate->mutex);
			if (gate->active != 0)
				--gate->active;
			gate->idle.notify_all();
		}
	};

	struct Lease
	{
		Lease() = default;
		Lease(Impl* impl_, std::shared_ptr<Owner> owner_) : impl(impl_), owner(std::move(owner_)), previous(s_activeServiceOwner)
		{
			s_activeServiceOwner = owner.get();
		}
		Lease(Lease&& other) noexcept : impl(std::exchange(other.impl, nullptr)),
										owner(std::move(other.owner)), previous(other.previous)
		{
		}
		Lease& operator=(Lease&&) = delete;
		Lease(const Lease&) = delete;
		Lease& operator=(const Lease&) = delete;
		~Lease()
		{
			if (impl && owner)
			{
				s_activeServiceOwner = previous;
				impl->Unpin(owner);
			}
		}
		explicit operator bool() const
		{
			return owner != nullptr;
		}
		Owner* operator->() const
		{
			return owner.get();
		}
		Impl* impl{};
		std::shared_ptr<Owner> owner;
		const void* previous{};
	};

	explicit Impl(AromaRuntimeOptions options_, WupsProcessKind initialProcess) : options(std::move(options_)), process(initialProcess),
																				  registry(options.exportRegistry ? options.exportRegistry : std::make_shared<ModuleExportRegistry>()),
																				  patchManager(options.patchManager),
																				  patchPlatform(options.patchPlatform),
																				  asyncGate(std::make_shared<AsyncGate>())
	{
		asyncGate->impl = this;
		if (!options.functionPatcher)
			options.functionPatcher = CreateUnsupportedFunctionPatcherFacade();
		if (patchPlatform)
			patchPlatform->SetCurrentProcess(
				initialProcess == WupsProcessKind::RootRpx ? WupsPatchProcess::RootRpx : initialProcess == WupsProcessKind::WiiUMenu ? WupsPatchProcess::WiiUMenu
																																	 : WupsPatchProcess::Game);
	}

	~Impl()
	{
		std::unique_lock lock(asyncGate->mutex);
		asyncGate->impl = nullptr;
		asyncGate->idle.wait(lock, [&] { return asyncGate->active == 0; });
	}

	[[nodiscard]] static AsyncLease PinAsync(
		const std::shared_ptr<AsyncGate>& gate)
	{
		std::lock_guard lock(gate->mutex);
		if (!gate->impl)
			return {};
		++gate->active;
		return AsyncLease(gate, gate->impl);
	}

	AromaRuntimeOptions options;
	std::atomic<WupsProcessKind> process;
	std::shared_ptr<ModuleExportRegistry> registry;
	std::shared_ptr<WupsFunctionPatchManager> patchManager;
	std::shared_ptr<IWupsPatchPlatform> patchPlatform;
	std::mutex compatibilityMutex;
	std::function<void()> detachModuleEvents;
	std::shared_ptr<AsyncGate> asyncGate;
	mutable std::mutex registryMutex;
	std::unordered_map<std::uint64_t, std::shared_ptr<Owner>> owners;
	std::atomic_uint32_t nextHandle{1};

	[[nodiscard]] std::uint32_t NewHandle()
	{
		auto handle = nextHandle.fetch_add(1);
		if (handle == 0)
			handle = nextHandle.fetch_add(1);
		return handle;
	}

	[[nodiscard]] Lease Pin(WupsOwnerToken token, WupsServiceStatus& status)
	{
		std::lock_guard lock(registryMutex);
		const auto found = owners.find(token.owner);
		if (found == owners.end())
		{
			status = WupsServiceStatus::StaleGeneration;
			return {};
		}
		const auto& owner = found->second;
		std::lock_guard ownerLock(owner->mutex);
		if (owner->token.generation != token.generation || owner->closing)
		{
			status = WupsServiceStatus::StaleGeneration;
			return {};
		}
		++owner->pins;
		status = WupsServiceStatus::Success;
		return Lease(this, owner);
	}

	void Unpin(const std::shared_ptr<Owner>& owner)
	{
		bool finalize{};
		{
			std::lock_guard lock(owner->mutex);
			cemu_assert_debug(owner->pins != 0);
			if (owner->pins != 0)
				--owner->pins;
			owner->pinsChanged.notify_all();
			finalize = owner->pins == 0 && owner->closing &&
					   owner->deferredFinalize && !owner->finalized;
		}
		if (finalize)
			FinalizeOwner(owner);
	}

	[[nodiscard]] bool ModuleAllowed(const Owner& owner,
									 std::string_view moduleName) const
	{
		return std::ranges::find(owner.permissions.modules, moduleName) !=
			   owner.permissions.modules.end();
	}

	[[nodiscard]] bool ReadGuest(const Owner& owner, std::uint32_t address,
								 std::span<std::byte> output, WupsGuestAccess access,
								 std::string& error) const
	{
		if (!options.platform || address == 0 || output.empty() ||
			output.size() > std::numeric_limits<std::uint32_t>::max())
		{
			error = !options.platform ? "guest platform adapter is unavailable" : "guest range is null or empty";
			return false;
		}
		std::uint32_t end{};
		if (!AddU32(address, static_cast<std::uint32_t>(output.size()), end) ||
			!options.platform->ValidateGuestRangeForOwner(owner.token, address,
														  static_cast<std::uint32_t>(output.size()), access) ||
			!options.platform->ReadGuest(address, output))
		{
			error = fmt::format(
				"owner {} generation {} supplied an invalid guest {} range "
				"0x{:08x}+0x{:x}",
				owner.token.owner, owner.token.generation,
				access == WupsGuestAccess::Execute ? "executable" : "readable",
				address, output.size());
			return false;
		}
		return true;
	}

	[[nodiscard]] bool WriteGuest(const Owner& owner, std::uint32_t address,
								  std::span<const std::byte> input, std::string& error)
	{
		if (!options.platform || address == 0 || input.empty() ||
			input.size() > std::numeric_limits<std::uint32_t>::max())
		{
			error = !options.platform ? "guest platform adapter is unavailable" : "guest output range is null or empty";
			return false;
		}
		std::uint32_t end{};
		if (!AddU32(address, static_cast<std::uint32_t>(input.size()), end) ||
			!options.platform->ValidateGuestRangeForOwner(owner.token, address,
														  static_cast<std::uint32_t>(input.size()), WupsGuestAccess::Write) ||
			!options.platform->WriteGuest(address, input))
		{
			error = fmt::format(
				"owner {} generation {} supplied an invalid writable guest range "
				"0x{:08x}+0x{:x}",
				owner.token.owner, owner.token.generation,
				address, input.size());
			return false;
		}
		return true;
	}

	[[nodiscard]] bool ReadGuestString(const Owner& owner, std::uint32_t address,
									   std::size_t maximum, std::string& value, std::string& error) const
	{
		value.clear();
		if (!options.platform || address == 0 || maximum == 0 ||
			maximum > kMaximumGuestString)
		{
			error = !options.platform ? "guest platform adapter is unavailable" : "guest string pointer or limit is invalid";
			return false;
		}
		value.reserve(std::min<std::size_t>(maximum, 128));
		for (std::size_t index = 0; index < maximum; ++index)
		{
			std::uint32_t current{};
			if (!AddU32(address, static_cast<std::uint32_t>(index), current) ||
				!options.platform->ValidateGuestRangeForOwner(owner.token,
															  current, 1, WupsGuestAccess::Read))
			{
				error = fmt::format(
					"guest string at 0x{:08x} leaves readable memory", address);
				return false;
			}
			std::array<std::byte, 1> byte{};
			if (!options.platform->ReadGuest(current, byte))
			{
				error = fmt::format("guest string at 0x{:08x} could not be read",
									address);
				return false;
			}
			const auto character = std::to_integer<unsigned char>(byte[0]);
			if (character == 0)
				return true;
			if (character < 0x20 && character != '\t')
			{
				error = "guest string contains a control character";
				return false;
			}
			value.push_back(static_cast<char>(character));
		}
		error = fmt::format(
			"guest string at 0x{:08x} is not NUL-terminated within {} bytes",
			address, maximum);
		return false;
	}

	[[nodiscard]] bool ReadGuestU32(const Owner& owner, std::uint32_t address,
									std::uint32_t& value, std::string& error) const
	{
		if ((address & 3U) != 0)
		{
			error = "guest uint32 pointer is not 4-byte aligned";
			return false;
		}
		std::array<std::byte, 4> bytes{};
		if (!ReadGuest(owner, address, bytes, WupsGuestAccess::Read, error))
			return false;
		value = ReadU32(bytes, 0);
		return true;
	}

	[[nodiscard]] bool WriteGuestU32(const Owner& owner, std::uint32_t address,
									 std::uint32_t value, std::string& error)
	{
		if ((address & 3U) != 0)
		{
			error = "guest uint32 output is not 4-byte aligned";
			return false;
		}
		std::array<std::byte, 4> bytes{
			static_cast<std::byte>(value >> 24),
			static_cast<std::byte>(value >> 16),
			static_cast<std::byte>(value >> 8),
			static_cast<std::byte>(value)};
		return WriteGuest(owner, address, bytes, error);
	}

	[[nodiscard]] bool ValidateGuestOutput(const Owner& owner,
										   std::uint32_t address, std::uint32_t size, std::uint32_t alignment,
										   std::string& error) const
	{
		if (!options.platform || address == 0 || size == 0 ||
			(alignment > 1 && (address & (alignment - 1)) != 0) ||
			!options.platform->ValidateGuestRangeForOwner(owner.token, address,
														  size, WupsGuestAccess::Write))
		{
			error = fmt::format(
				"owner {} generation {} supplied an invalid writable guest output "
				"0x{:08x}+0x{:x}",
				owner.token.owner, owner.token.generation,
				address, size);
			return false;
		}
		return true;
	}

	[[nodiscard]] bool WriteGuestBool(const Owner& owner,
									  std::uint32_t address, bool value, std::string& error)
	{
		const std::array bytes{value ? std::byte{1} : std::byte{0}};
		return WriteGuest(owner, address, bytes, error);
	}

	[[nodiscard]] bool ValidateGuestCallback(const Owner& owner,
											 std::uint32_t address, std::string& error) const
	{
		if (address == 0)
			return true;
		if (!options.platform || (address & 3U) != 0 ||
			!options.platform->ValidateGuestRangeForOwner(owner.token,
														  address, 4, WupsGuestAccess::Execute))
		{
			error = fmt::format(
				"owner {} generation {} supplied non-executable callback 0x{:08x}",
				owner.token.owner, owner.token.generation, address);
			return false;
		}
		return true;
	}

	[[nodiscard]] bool Invoke(const std::shared_ptr<Owner>& owner,
							  std::uint32_t callback, std::span<const std::uint32_t> arguments,
							  std::uint32_t& result, std::string& error) const
	{
		if (callback == 0)
			return true;
		WupsGuestInvoker invoker;
		{
			std::lock_guard lock(owner->mutex);
			invoker = owner->invoker;
		}
		if (!invoker)
		{
			error = "guest callback invoker is not bound to the plugin module";
			return false;
		}
		return invoker(callback, arguments, result, error);
	}

	[[nodiscard]] std::filesystem::path StoragePath(const Owner& owner) const
	{
		const auto principal = owner.principal.empty() ? HexEncode(owner.packageId) : HexEncode(owner.principal);
		return options.storageRoot / principal / HexU64(owner.titleId) /
			   (HexEncode(owner.metadata.storageId) + ".wups-storage");
	}

	[[nodiscard]] bool EnsureSafeStorageParent(const std::filesystem::path& path,
											   std::string& error) const
	{
		std::error_code code;
		if (options.storageRoot.empty())
		{
			error = "WUPS storage root is not configured";
			return false;
		}
		std::filesystem::create_directories(options.storageRoot, code);
		if (code)
		{
			error = fmt::format("could not create WUPS storage root: {}",
								code.message());
			return false;
		}
		auto current = options.storageRoot;
		const auto relative = path.parent_path().lexically_relative(
			options.storageRoot);
		if (relative.empty() || *relative.begin() == "..")
		{
			error = "storage namespace escaped the configured root";
			return false;
		}
		for (const auto& component : relative)
		{
			current /= component;
			const auto status = std::filesystem::symlink_status(current, code);
			if (!code && std::filesystem::is_symlink(status))
			{
				error = "storage namespace contains a symlink";
				return false;
			}
			code.clear();
			std::filesystem::create_directory(current, code);
			if (code && code != std::errc::file_exists)
			{
				error = fmt::format("could not create storage namespace: {}",
									code.message());
				return false;
			}
			code.clear();
		}
		return true;
	}

	[[nodiscard]] bool WriteAtomic(const std::filesystem::path& path,
								   std::span<const std::byte> bytes, WupsOwnerToken token,
								   std::string& error) const
	{
		if (!EnsureSafeStorageParent(path, error))
			return false;
		const auto temporary = path.parent_path() /
							   fmt::format(".{}.tmp-{}-{}", path.filename().string(),
										   token.owner, token.generation);
		std::error_code code;
		if (std::filesystem::is_symlink(
				std::filesystem::symlink_status(temporary, code)))
		{
			error = "storage temporary path is a symlink";
			return false;
		}
		std::filesystem::remove(temporary, code);
#if BOOST_OS_WINDOWS
		const int descriptor = _wopen(temporary.c_str(),
									  _O_BINARY | _O_CREAT | _O_EXCL | _O_WRONLY,
									  _S_IREAD | _S_IWRITE);
#else
		const int descriptor = open(temporary.c_str(),
									O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
#endif
		if (descriptor < 0)
		{
			error = "could not create the storage temporary file";
			return false;
		}
		std::size_t offset{};
		bool success = true;
		while (offset < bytes.size())
		{
#if BOOST_OS_WINDOWS
			const auto written = _write(descriptor, bytes.data() + offset,
										static_cast<unsigned>(std::min<std::size_t>(
											bytes.size() - offset, std::numeric_limits<unsigned>::max())));
#else
			const auto written = write(descriptor, bytes.data() + offset,
									   bytes.size() - offset);
#endif
			if (written <= 0)
			{
				success = false;
				break;
			}
			offset += static_cast<std::size_t>(written);
		}
#if BOOST_OS_WINDOWS
		success = success && _commit(descriptor) == 0;
		_close(descriptor);
#else
		success = success && fsync(descriptor) == 0;
		close(descriptor);
#endif
		if (!success)
		{
			std::filesystem::remove(temporary, code);
			error = "storage temporary file could not be written atomically";
			return false;
		}
		std::filesystem::rename(temporary, path, code);
		if (code)
		{
			const auto renameError = code;
			std::error_code cleanupError;
			std::filesystem::remove(temporary, cleanupError);
			error = fmt::format(
				"storage atomic rename failed: {}", renameError.message());
			return false;
		}
#if !BOOST_OS_WINDOWS
		const int directory = open(path.parent_path().c_str(),
								   O_RDONLY | O_CLOEXEC | O_DIRECTORY);
		if (directory >= 0)
		{
			(void)fsync(directory);
			close(directory);
		}
#endif
		return true;
	}

	[[nodiscard]] bool SerializeStorage(const Owner& owner,
										std::vector<std::byte>& output, std::uint64_t& revision,
										std::string& error) const
	{
		struct Entry
		{
			std::string path;
			const StorageNode* node{};
		};
		std::vector<Entry> entries;
		std::function<bool(std::uint32_t, std::string, std::size_t)> walk;
		const auto storage = owner.storage;
		std::lock_guard lock(storage->mutex);
		walk = [&](std::uint32_t handle, std::string prefix,
				   std::size_t depth) {
			if (depth > kMaximumStorageDepth)
				return false;
			const auto found = storage->nodes.find(handle);
			if (found == storage->nodes.end())
				return false;
			for (const auto& [key, childHandle] : found->second.children)
			{
				const auto child = storage->nodes.find(childHandle);
				if (child == storage->nodes.end())
					return false;
				const auto path = prefix.empty() ? key : prefix + "/" + key;
				entries.push_back({path, &child->second});
				if (child->second.container &&
					!walk(childHandle, path, depth + 1))
					return false;
			}
			return true;
		};
		if (storage->root == 0 ||
			!walk(storage->root, {}, 0) ||
			entries.size() > options.maximumStorageItems)
		{
			error = "storage tree is internally inconsistent";
			return false;
		}
		output.assign(kStorageMagic.begin(), kStorageMagic.end());
		AppendU32(output, static_cast<std::uint32_t>(entries.size()));
		for (const auto& entry : entries)
		{
			if (entry.path.size() > std::numeric_limits<std::uint16_t>::max() ||
				entry.node->value.bytes.size() >
					std::numeric_limits<std::uint32_t>::max())
			{
				error = "storage entry exceeds serialization limits";
				return false;
			}
			AppendU16(output, static_cast<std::uint16_t>(entry.path.size()));
			output.insert(output.end(),
						  reinterpret_cast<const std::byte*>(entry.path.data()),
						  reinterpret_cast<const std::byte*>(entry.path.data() +
															 entry.path.size()));
			AppendU32(output, entry.node->container ? kStorageContainerType : static_cast<std::uint32_t>(entry.node->value.type));
			AppendU32(output, static_cast<std::uint32_t>(
								  entry.node->value.bytes.size()));
			output.insert(output.end(), entry.node->value.bytes.begin(),
						  entry.node->value.bytes.end());
		}
		const auto crc = Crc32(output);
		AppendU32(output, crc);
		if (output.size() > options.maximumStorageBytes)
		{
			error = "serialized storage exceeds the per-plugin size limit";
			return false;
		}
		revision = storage->revision;
		return true;
	}

	[[nodiscard]] bool DeserializeStorage(Owner& owner,
										  std::span<const std::byte> input, std::string& error)
	{
		if (input.size() < kStorageMagic.size() + 8 ||
			!std::equal(kStorageMagic.begin(), kStorageMagic.end(), input.begin()) ||
			ReadU32(input, input.size() - 4) !=
				Crc32(input.first(input.size() - 4)))
		{
			error = "WUPS storage file has an invalid header or checksum";
			return false;
		}
		const auto count = ReadU32(input, 8);
		if (count > options.maximumStorageItems)
		{
			error = "WUPS storage item count exceeds the configured limit";
			return false;
		}
		std::unordered_map<std::uint32_t, StorageNode> replacement;
		const auto replacementRoot = NewHandle();
		replacement.emplace(replacementRoot,
							StorageNode{replacementRoot, 0, {}, true});
		std::size_t offset = 12;
		for (std::uint32_t index = 0; index < count; ++index)
		{
			if (offset > input.size() - 4 || input.size() - 4 - offset < 2)
			{
				error = "WUPS storage path length is truncated";
				return false;
			}
			const auto pathSize = ReadU16(input, offset);
			offset += 2;
			if (pathSize == 0 || pathSize > input.size() - 4 - offset)
			{
				error = "WUPS storage path is invalid";
				return false;
			}
			const std::string path(
				reinterpret_cast<const char*>(input.data() + offset), pathSize);
			offset += pathSize;
			if (input.size() - 4 - offset < 8)
			{
				error = "WUPS storage entry header is truncated";
				return false;
			}
			const auto type = ReadU32(input, offset);
			const auto size = ReadU32(input, offset + 4);
			offset += 8;
			if (size > kMaximumStorageValue || size > input.size() - 4 - offset)
			{
				error = "WUPS storage value is truncated or too large";
				return false;
			}
			std::uint32_t parent = replacementRoot;
			std::size_t componentBegin{};
			std::size_t depth{};
			while (componentBegin <= path.size())
			{
				const auto slash = path.find('/', componentBegin);
				const auto key = std::string_view(path).substr(componentBegin,
															   slash == std::string::npos ? path.size() - componentBegin : slash - componentBegin);
				if (!ValidStorageKey(key) || ++depth > kMaximumStorageDepth)
				{
					error = "WUPS storage contains an unsafe item path";
					return false;
				}
				const bool final = slash == std::string::npos;
				auto& parentNode = replacement.at(parent);
				const auto existing = parentNode.children.find(key);
				if (!final)
				{
					if (existing == parentNode.children.end())
					{
						error = "WUPS storage parent appears after its child";
						return false;
					}
					const auto parentFound = replacement.find(existing->second);
					if (parentFound == replacement.end() ||
						!parentFound->second.container)
					{
						error = "WUPS storage path traverses a value";
						return false;
					}
					parent = existing->second;
					componentBegin = slash + 1;
					continue;
				}
				if (existing != parentNode.children.end())
				{
					error = "WUPS storage contains duplicate paths";
					return false;
				}
				const auto handle = NewHandle();
				StorageNode node{handle, parent, std::string(key),
								 type == kStorageContainerType};
				if (!node.container)
				{
					if (type > static_cast<std::uint32_t>(
								   WupsStorageValueType::Double))
					{
						error = "WUPS storage contains an invalid item type";
						return false;
					}
					node.value.type = static_cast<WupsStorageValueType>(type);
					node.value.bytes.assign(input.begin() + offset,
											input.begin() + offset + size);
					if (!ValidateStorageValue(node.value))
					{
						error = "WUPS storage contains an invalid typed value";
						return false;
					}
				}
				else if (size != 0)
				{
					error = "WUPS storage container has value bytes";
					return false;
				}
				parentNode.children.emplace(node.key, handle);
				replacement.emplace(handle, std::move(node));
				parent = handle;
				break;
			}
			offset += size;
		}
		if (offset != input.size() - 4)
		{
			error = "WUPS storage contains trailing data";
			return false;
		}
		{
			std::lock_guard lock(owner.storage->mutex);
			owner.storage->nodes = std::move(replacement);
			owner.storage->root = replacementRoot;
			owner.storage->loaded = true;
			owner.storage->dirty = false;
			owner.storage->revision = 0;
		}
		return true;
	}

	[[nodiscard]] WupsServiceStatus EnsureStorageLoadedLocked(Owner& owner,
															  std::string& error)
	{
		if (!ValidStorageId(owner.metadata.storageId))
		{
			error = "plugin storage_id is missing or unsafe";
			return WupsServiceStatus::InvalidArgument;
		}
		{
			std::lock_guard lock(owner.storage->mutex);
			if (owner.storage->loaded)
				return WupsServiceStatus::Success;
			owner.storage->path = StoragePath(owner);
		}
		const auto path = owner.storage->path;
		std::error_code code;
		if (!std::filesystem::exists(path, code))
		{
			if (code)
			{
				error = fmt::format("could not inspect WUPS storage: {}",
									code.message());
				return WupsServiceStatus::IoError;
			}
			std::lock_guard lock(owner.storage->mutex);
			owner.storage->root = NewHandle();
			owner.storage->nodes.emplace(owner.storage->root,
										 StorageNode{owner.storage->root, 0, {}, true});
			owner.storage->loaded = true;
			return WupsServiceStatus::Success;
		}
		if (std::filesystem::is_symlink(
				std::filesystem::symlink_status(path, code)))
		{
			error = "WUPS storage file is a symlink";
			return WupsServiceStatus::IoError;
		}
		const auto size = std::filesystem::file_size(path, code);
		if (code || size > options.maximumStorageBytes)
		{
			error = "WUPS storage file exceeds its size limit or cannot be read";
			return WupsServiceStatus::CorruptData;
		}
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
		{
			error = "WUPS storage file could not be opened";
			return WupsServiceStatus::IoError;
		}
		std::vector<std::byte> bytes(static_cast<std::size_t>(size));
		if (!bytes.empty() &&
			!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
		{
			error = "WUPS storage file could not be read completely";
			return WupsServiceStatus::IoError;
		}
		return DeserializeStorage(owner, bytes, error) ? WupsServiceStatus::Success : WupsServiceStatus::CorruptData;
	}

	[[nodiscard]] WupsServiceStatus EnsureStorageLoaded(Owner& owner,
														std::string& error)
	{
		std::lock_guard loadLock(owner.storageLoadMutex);
		return EnsureStorageLoadedLocked(owner, error);
	}

	[[nodiscard]] WupsServiceStatus SaveStorage(Owner& owner, bool force,
												std::string& error)
	{
		auto status = EnsureStorageLoaded(owner, error);
		if (status != WupsServiceStatus::Success)
			return status;
		{
			std::lock_guard lock(owner.storage->mutex);
			if (!force && !owner.storage->dirty)
				return WupsServiceStatus::Success;
		}
		std::vector<std::byte> bytes;
		std::uint64_t revision{};
		if (!SerializeStorage(owner, bytes, revision, error))
			return WupsServiceStatus::LimitExceeded;
		if (!WriteAtomic(owner.storage->path, bytes, owner.token, error))
			return WupsServiceStatus::IoError;
		std::lock_guard lock(owner.storage->mutex);
		if (owner.storage->revision == revision)
			owner.storage->dirty = false;
		return WupsServiceStatus::Success;
	}

	void FinalizeOwner(const std::shared_ptr<Owner>& owner)
	{
		{
			std::lock_guard lock(owner->mutex);
			if (owner->finalized)
				return;
			owner->finalized = true;
		}
		if (options.closeCex2Owner)
			options.closeCex2Owner(*owner);

		bool storageWasLoaded{};
		{
			std::lock_guard lock(owner->storage->mutex);
			storageWasLoaded = owner->storage->loaded;
		}
		if (storageWasLoaded)
		{
			std::string storageError;
			if (const auto status = SaveStorage(*owner, false, storageError);
				status != WupsServiceStatus::Success && options.platform)
				options.platform->Log(owner->token, WupsLogLevel::Error,
									  "homebrew_wupsbackend", "storage cleanup", storageError);
		}

		{
			std::lock_guard lock(owner->config.mutex);
			owner->config.categories.clear();
			owner->config.items.clear();
			owner->config.menuOpen = false;
		}
		std::vector<WupsMappedMemoryInfo> mappings;
		std::vector<std::uint32_t> guestData;
		{
			std::lock_guard lock(owner->mutex);
			for (const auto& [address, mapping] : owner->mappings)
				mappings.push_back(mapping);
			guestData.swap(owner->guestData);
			owner->mappings.clear();
			owner->mappingsBeingFreed.clear();
			owner->combos.clear();
			owner->reent.clear();
			owner->notifications.clear();
			owner->redirects.clear();
			owner->functionExports.clear();
			owner->dataExports.clear();
			owner->importLeases.clear();
			owner->mappedBytes = 0;
			owner->mappedBytesReserved = 0;
		}
		if (options.functionPatcher)
			options.functionPatcher->ReleaseOwner(owner->token);
		if (options.platform)
		{
			for (const auto& mapping : mappings)
			{
				std::string mappingError;
				if (!options.platform->FreeMappedMemory(
						owner->token, mapping, mappingError))
					options.platform->Log(owner->token, WupsLogLevel::Warning,
										  "homebrew_memorymapping", "owner cleanup", mappingError);
			}
			for (const auto address : guestData)
				options.platform->FreeGuestData(owner->token, address);
			options.platform->ReleaseOwnerExports(owner->token);
		}
	}

	void Release(WupsOwnerToken token)
	{
		std::shared_ptr<Owner> owner;
		{
			std::lock_guard lock(registryMutex);
			const auto found = owners.find(token.owner);
			if (found == owners.end() ||
				found->second->token.generation != token.generation)
				return;
			owner = found->second;
			{
				std::lock_guard ownerLock(owner->mutex);
				owner->closing = true;
				owner->stopped.store(true, std::memory_order_release);
			}
			owners.erase(found);
		}
		if (options.platform)
		{
			options.platform->CancelCpuTasks(token);
		}
		{
			std::unique_lock lock(owner->mutex);
			if (s_activeServiceOwner == owner.get())
			{
				owner->deferredFinalize = true;
				return;
			}
			owner->pinsChanged.wait(lock, [&] { return owner->pins == 0; });
		}
		FinalizeOwner(owner);
	}

	[[nodiscard]] bool EnsureFunctionExport(const std::shared_ptr<Owner>& owner,
											std::string_view moduleName, std::string_view symbolName,
											std::uint32_t& address, std::string& error);
	[[nodiscard]] std::int32_t Dispatch(const std::shared_ptr<Owner>& owner,
										std::string_view moduleName, std::string_view symbolName,
										std::span<const std::uint32_t> arguments, std::string& error);
	[[nodiscard]] std::int32_t DispatchBackend(
		const std::shared_ptr<Owner>& owner,
		const WupsBackendExportDescriptor& export_,
		std::span<const std::uint32_t> arguments, std::string& error);
	[[nodiscard]] bool QueueCallback(WupsOwnerToken token,
									 std::uint32_t callback, std::vector<std::uint32_t> arguments,
									 std::string& error);
	[[nodiscard]] WupsServiceStatus AllocateMapping(
		const std::shared_ptr<Owner>& owner, WupsOwnerToken token,
		std::uint32_t size, std::uint32_t alignment, bool writable,
		WupsMappedMemoryPurpose purpose, WupsMappedMemoryInfo& allocation,
		std::string& error);
	[[nodiscard]] WupsServiceStatus FreeMapping(
		const std::shared_ptr<Owner>& owner, WupsOwnerToken token,
		std::uint32_t address, std::string& error);
};

std::shared_ptr<IWupsFunctionPatcherFacade>
CreateUnsupportedFunctionPatcherFacade()
{
	return std::make_shared<UnsupportedFunctionPatcher>();
}

AromaCompatibilityRuntime::AromaCompatibilityRuntime(WupsProcessKind process) : AromaCompatibilityRuntime(AromaRuntimeOptions{}, process)
{
}

AromaCompatibilityRuntime::AromaCompatibilityRuntime(AromaRuntimeOptions options,
													 WupsProcessKind process) : m_impl(std::make_unique<Impl>(std::move(options), process))
{
}

AromaCompatibilityRuntime::~AromaCompatibilityRuntime()
{
	std::function<void()> detach;
	{
		std::lock_guard lock(m_impl->compatibilityMutex);
		detach = std::move(m_impl->detachModuleEvents);
	}
	if (detach)
		detach();

	std::vector<WupsOwnerToken> owners;
	{
		std::lock_guard lock(m_impl->registryMutex);
		owners.reserve(m_impl->owners.size());
		for (const auto& [handle, owner] : m_impl->owners)
			owners.push_back(owner->token);
	}
	for (const auto owner : owners)
		m_impl->Release(owner);
}

void AromaCompatibilityRuntime::SetCurrentProcess(WupsProcessKind process)
{
	m_impl->process.store(process);
	if (m_impl->patchPlatform)
		m_impl->patchPlatform->SetCurrentProcess(
			process == WupsProcessKind::RootRpx ? WupsPatchProcess::RootRpx : process == WupsProcessKind::WiiUMenu ? WupsPatchProcess::WiiUMenu
																												   : WupsPatchProcess::Game);
}

WupsProcessKind AromaCompatibilityRuntime::CurrentProcess() const
{
	return m_impl->process.load();
}

bool AromaCompatibilityRuntime::RegisterOwner(const CemodPackage& package,
											  const WupsMetadata& metadata, WupsOwnerToken token, std::string& error)
{
	error.clear();
	if (token.owner == 0 || token.generation == 0)
	{
		error = "WUPS owner and generation must be non-zero";
		return false;
	}
	if (!package.IsTrustedNative() ||
		package.manifest.payload.format != CemodPayloadFormat::Wups)
	{
		error = "WUPS services require a trusted_native WUPS package";
		return false;
	}
	std::lock_guard lock(m_impl->registryMutex);
	if (const auto found = m_impl->owners.find(token.owner);
		found != m_impl->owners.end())
	{
		if (found->second->token == token)
			return true;
		error = fmt::format(
			"WUPS owner {} is still registered with generation {}",
			token.owner, found->second->token.generation);
		return false;
	}
	auto owner = std::make_shared<Impl::Owner>();
	owner->token = token;
	owner->package = package;
	owner->grantedPermissions.store(package.grantedPermissions,
									std::memory_order_release);
	owner->permissions = package.manifest.nativePermissions;
	owner->packageId = package.manifest.modId;
	owner->principal = package.principal;
	owner->titleId = package.targetTitleId;
	owner->pluginIdentifier = m_impl->NewHandle();
	owner->metadata = metadata;
	m_impl->owners.emplace(token.owner, std::move(owner));
	return true;
}

bool AromaCompatibilityRuntime::IsOwnerActive(WupsOwnerToken token) const
{
	std::lock_guard lock(m_impl->registryMutex);
	const auto found = m_impl->owners.find(token.owner);
	if (found == m_impl->owners.end())
		return false;
	std::lock_guard ownerLock(found->second->mutex);
	return found->second->token == token && !found->second->closing;
}

std::shared_ptr<cemuextend_hle::Cex2Owner>
AromaCompatibilityRuntime::Cex2OwnerFor(WupsOwnerToken token) const
{
	std::lock_guard lock(m_impl->registryMutex);
	const auto found = m_impl->owners.find(token.owner);
	if (found == m_impl->owners.end())
		return {};
	const auto& owner = found->second;
	std::lock_guard ownerLock(owner->mutex);
	if (owner->token != token || owner->closing || owner->IsStopped())
		return {};
	return std::static_pointer_cast<cemuextend_hle::Cex2Owner>(owner);
}

bool AromaCompatibilityRuntime::BindGuestInvoker(std::uint64_t owner,
												 std::uint32_t generation, WupsGuestInvoker invoker, std::string& error)
{
	error.clear();
	WupsServiceStatus status;
	auto lease = m_impl->Pin({owner, generation}, status);
	if (!lease)
	{
		error = "guest callback invoker rejected a stale owner generation";
		return false;
	}
	if (!invoker)
	{
		error = "guest callback invoker is empty";
		return false;
	}
	std::lock_guard lock(lease->mutex);
	lease->invoker = std::move(invoker);
	return true;
}

bool AromaCompatibilityRuntime::BeginOwner(const CemodPackage& package,
										   const WupsMetadata& metadata, std::uint64_t owner,
										   std::uint32_t generation, std::string& error)
{
	return RegisterOwner(package, metadata, {owner, generation}, error);
}

WupsServiceStatus AromaCompatibilityRuntime::StorageCreateSubItem(
	WupsOwnerToken token, std::uint32_t parent, std::string_view key,
	std::uint32_t& handle)
{
	handle = 0;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::string error;
	status = m_impl->EnsureStorageLoaded(*owner.owner, error);
	if (status != WupsServiceStatus::Success || !ValidStorageKey(key))
		return status == WupsServiceStatus::Success ? WupsServiceStatus::InvalidArgument : status;
	auto storage = owner->storage;
	std::lock_guard lock(storage->mutex);
	parent = parent == 0 ? storage->root : parent;
	const auto found = storage->nodes.find(parent);
	if (found == storage->nodes.end() || !found->second.container)
		return WupsServiceStatus::NotFound;
	if (found->second.children.contains(key))
		return WupsServiceStatus::AlreadyExists;
	if (storage->nodes.size() - 1 >= m_impl->options.maximumStorageItems)
		return WupsServiceStatus::LimitExceeded;
	handle = m_impl->NewHandle();
	found->second.children.emplace(std::string(key), handle);
	storage->nodes.emplace(handle, Impl::StorageNode{
									   handle, parent, std::string(key), true});
	storage->dirty = true;
	++storage->revision;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::StorageGetSubItem(
	WupsOwnerToken token, std::uint32_t parent, std::string_view key,
	std::uint32_t& handle) const
{
	handle = 0;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::string error;
	status = m_impl->EnsureStorageLoaded(*owner.owner, error);
	if (status != WupsServiceStatus::Success || !ValidStorageKey(key))
		return status == WupsServiceStatus::Success ? WupsServiceStatus::InvalidArgument : status;
	const auto storage = owner->storage;
	std::lock_guard lock(storage->mutex);
	parent = parent == 0 ? storage->root : parent;
	const auto found = storage->nodes.find(parent);
	if (found == storage->nodes.end() || !found->second.container)
		return WupsServiceStatus::NotFound;
	const auto child = found->second.children.find(key);
	if (child == found->second.children.end())
		return WupsServiceStatus::NotFound;
	const auto node = storage->nodes.find(child->second);
	if (node == storage->nodes.end() || !node->second.container)
		return WupsServiceStatus::NotFound;
	handle = node->first;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::StorageStore(WupsOwnerToken token,
														  std::uint32_t parent, std::string_view key, const WupsStorageValue& value)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::string error;
	status = m_impl->EnsureStorageLoaded(*owner.owner, error);
	if (status != WupsServiceStatus::Success || !ValidStorageKey(key) ||
		!ValidateStorageValue(value))
		return status == WupsServiceStatus::Success ? WupsServiceStatus::InvalidArgument : status;
	const auto storage = owner->storage;
	std::lock_guard lock(storage->mutex);
	parent = parent == 0 ? storage->root : parent;
	const auto found = storage->nodes.find(parent);
	if (found == storage->nodes.end() || !found->second.container)
		return WupsServiceStatus::NotFound;
	const auto child = found->second.children.find(key);
	if (child != found->second.children.end())
	{
		auto node = storage->nodes.find(child->second);
		if (node == storage->nodes.end())
			return WupsServiceStatus::InternalError;
		if (node->second.container)
			return WupsServiceStatus::AlreadyExists;
		node->second.value = value;
	}
	else
	{
		if (storage->nodes.size() - 1 >= m_impl->options.maximumStorageItems)
			return WupsServiceStatus::LimitExceeded;
		const auto handle = m_impl->NewHandle();
		found->second.children.emplace(std::string(key), handle);
		Impl::StorageNode node{
			handle, parent, std::string(key), false, {}, value};
		storage->nodes.emplace(handle, std::move(node));
	}
	storage->dirty = true;
	++storage->revision;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::StorageGet(WupsOwnerToken token,
														std::uint32_t parent, std::string_view key, WupsStorageValueType type,
														WupsStorageValue& value) const
{
	value = {};
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::string error;
	status = m_impl->EnsureStorageLoaded(*owner.owner, error);
	if (status != WupsServiceStatus::Success || !ValidStorageKey(key))
		return status == WupsServiceStatus::Success ? WupsServiceStatus::InvalidArgument : status;
	const auto storage = owner->storage;
	std::lock_guard lock(storage->mutex);
	parent = parent == 0 ? storage->root : parent;
	const auto found = storage->nodes.find(parent);
	if (found == storage->nodes.end() || !found->second.container)
		return WupsServiceStatus::NotFound;
	const auto child = found->second.children.find(key);
	if (child == found->second.children.end())
		return WupsServiceStatus::NotFound;
	const auto node = storage->nodes.find(child->second);
	if (node == storage->nodes.end() || node->second.container)
		return WupsServiceStatus::NotFound;
	if (node->second.value.type != type)
		return WupsServiceStatus::InvalidArgument;
	value = node->second.value;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::StorageDelete(WupsOwnerToken token,
														   std::uint32_t parent, std::string_view key)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::string error;
	status = m_impl->EnsureStorageLoaded(*owner.owner, error);
	if (status != WupsServiceStatus::Success || !ValidStorageKey(key))
		return status == WupsServiceStatus::Success ? WupsServiceStatus::InvalidArgument : status;
	const auto storage = owner->storage;
	std::lock_guard lock(storage->mutex);
	parent = parent == 0 ? storage->root : parent;
	const auto found = storage->nodes.find(parent);
	if (found == storage->nodes.end() || !found->second.container)
		return WupsServiceStatus::NotFound;
	const auto child = found->second.children.find(key);
	if (child == found->second.children.end())
		return WupsServiceStatus::NotFound;
	std::vector<std::uint32_t> pending{child->second};
	while (!pending.empty())
	{
		const auto current = pending.back();
		pending.pop_back();
		const auto node = storage->nodes.find(current);
		if (node == storage->nodes.end())
			continue;
		for (const auto& [childKey, childHandle] : node->second.children)
			pending.push_back(childHandle);
		storage->nodes.erase(node);
	}
	found->second.children.erase(child);
	storage->dirty = true;
	++storage->revision;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::StorageSave(WupsOwnerToken token,
														 bool force, std::string& error)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
	{
		error = "storage save rejected a stale owner generation";
		return status;
	}
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	return m_impl->SaveStorage(*owner.owner, force, error);
}

WupsServiceStatus AromaCompatibilityRuntime::StorageForceReload(
	WupsOwnerToken token, std::string& error)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
	{
		error = "storage reload rejected a stale owner generation";
		return status;
	}
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::lock_guard loadLock(owner->storageLoadMutex);
	{
		std::lock_guard lock(owner->storage->mutex);
		owner->storage->nodes.clear();
		owner->storage->root = 0;
		owner->storage->path = m_impl->StoragePath(*owner.owner);
		owner->storage->loaded = false;
		owner->storage->dirty = false;
		owner->storage->revision = 0;
	}
	status = m_impl->EnsureStorageLoadedLocked(*owner.owner, error);
	return status;
}

WupsServiceStatus AromaCompatibilityRuntime::StorageWipe(WupsOwnerToken token,
														 std::string& error)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
	{
		error = "storage wipe rejected a stale owner generation";
		return status;
	}
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	status = m_impl->EnsureStorageLoaded(*owner.owner, error);
	if (status != WupsServiceStatus::Success)
		return status;
	const auto storage = owner->storage;
	{
		std::lock_guard lock(storage->mutex);
		const auto root = storage->root;
		storage->nodes.clear();
		storage->nodes.emplace(root,
							   Impl::StorageNode{root, 0, {}, true});
		storage->dirty = true;
		++storage->revision;
	}
	return m_impl->SaveStorage(*owner.owner, true, error);
}

WupsServiceStatus AromaCompatibilityRuntime::ConfigRegisterCallbacks(
	WupsOwnerToken token, std::string_view name, std::uint32_t openCallback,
	std::uint32_t closeCallback)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::string error;
	if (!ValidDisplayName(name) || openCallback == 0 || closeCallback == 0 ||
		!m_impl->ValidateGuestCallback(*owner.owner, openCallback, error) ||
		!m_impl->ValidateGuestCallback(*owner.owner, closeCallback, error))
		return WupsServiceStatus::InvalidArgument;
	std::lock_guard lock(owner->config.mutex);
	if (owner->config.registered)
		return WupsServiceStatus::AlreadyExists;
	owner->config.name = std::string(name);
	owner->config.openCallback = openCallback;
	owner->config.closeCallback = closeCallback;
	owner->config.registered = true;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ConfigCreateCategory(
	WupsOwnerToken token, std::string_view name, std::uint32_t& handle)
{
	handle = 0;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	if (!ValidDisplayName(name))
		return WupsServiceStatus::InvalidArgument;
	std::lock_guard lock(owner->config.mutex);
	if (owner->config.categories.size() >= 256)
		return WupsServiceStatus::LimitExceeded;
	handle = m_impl->NewHandle();
	owner->config.categories.emplace(handle,
									 Impl::ConfigCategory{handle, std::string(name)});
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ConfigCreateItem(
	WupsOwnerToken token, WupsConfigItemModel item, std::uint32_t& handle)
{
	handle = 0;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	if (!ValidDisplayName(item.displayName) ||
		item.choices.size() > 128 ||
		(item.kind == WupsConfigItemKind::IntegerRange &&
		 item.minimum > item.maximum))
		return WupsServiceStatus::InvalidArgument;
	std::set<std::int32_t> choiceValues;
	for (const auto& choice : item.choices)
		if (!ValidDisplayName(choice.label) ||
			!choiceValues.insert(choice.value).second)
			return WupsServiceStatus::InvalidArgument;
	const std::array callbacks{
		item.callbacks.valueDisplay,
		item.callbacks.selectedValueDisplay,
		item.callbacks.selected,
		item.callbacks.restoreDefault,
		item.callbacks.movementAllowed,
		item.callbacks.close,
		item.callbacks.input,
		item.callbacks.inputEx,
		item.callbacks.destroy,
	};
	std::string error;
	for (const auto callback : callbacks)
		if (!m_impl->ValidateGuestCallback(*owner.owner, callback, error))
			return WupsServiceStatus::InvalidArgument;
	std::lock_guard lock(owner->config.mutex);
	if (owner->config.items.size() >= 512)
		return WupsServiceStatus::LimitExceeded;
	handle = m_impl->NewHandle();
	item.handle = handle;
	owner->config.items.emplace(handle,
								Impl::ConfigItem{std::move(item)});
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ConfigAddCategory(
	WupsOwnerToken token, std::uint32_t parent, std::uint32_t child)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	if (parent == 0 || child == 0 || parent == child)
		return WupsServiceStatus::InvalidArgument;
	std::lock_guard lock(owner->config.mutex);
	auto parentFound = owner->config.categories.find(parent);
	auto childFound = owner->config.categories.find(child);
	if (parentFound == owner->config.categories.end() ||
		childFound == owner->config.categories.end())
		return WupsServiceStatus::NotFound;
	if (childFound->second.attached)
		return WupsServiceStatus::OwnerMismatch;
	std::set<std::uint32_t> visited;
	std::vector<std::uint32_t> pending{child};
	while (!pending.empty())
	{
		const auto current = pending.back();
		pending.pop_back();
		if (!visited.insert(current).second)
			continue;
		if (current == parent)
			return WupsServiceStatus::Conflict;
		const auto category = owner->config.categories.find(current);
		if (category != owner->config.categories.end())
			pending.insert(pending.end(), category->second.categories.begin(),
						   category->second.categories.end());
	}
	parentFound->second.categories.push_back(child);
	childFound->second.attached = true;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ConfigAddItem(
	WupsOwnerToken token, std::uint32_t category, std::uint32_t item)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	if (category == 0 || item == 0)
		return WupsServiceStatus::InvalidArgument;
	std::lock_guard lock(owner->config.mutex);
	auto categoryFound = owner->config.categories.find(category);
	auto itemFound = owner->config.items.find(item);
	if (categoryFound == owner->config.categories.end() ||
		itemFound == owner->config.items.end())
		return WupsServiceStatus::NotFound;
	if (itemFound->second.attached)
		return WupsServiceStatus::OwnerMismatch;
	categoryFound->second.items.push_back(item);
	itemFound->second.attached = true;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ConfigDestroyCategory(
	WupsOwnerToken token, std::uint32_t handle)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::lock_guard lock(owner->config.mutex);
	const auto found = owner->config.categories.find(handle);
	if (found == owner->config.categories.end())
		return WupsServiceStatus::NotFound;
	if (found->second.attached || !found->second.categories.empty() ||
		!found->second.items.empty())
		return WupsServiceStatus::Busy;
	owner->config.categories.erase(found);
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ConfigDestroyItem(
	WupsOwnerToken token, std::uint32_t handle)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::uint32_t callback{};
	std::uint32_t context{};
	{
		std::lock_guard lock(owner->config.mutex);
		const auto found = owner->config.items.find(handle);
		if (found == owner->config.items.end())
			return WupsServiceStatus::NotFound;
		if (found->second.attached)
			return WupsServiceStatus::Busy;
		callback = found->second.model.callbacks.destroy;
		context = found->second.model.callbacks.context;
		owner->config.items.erase(found);
	}
	if (callback != 0)
	{
		std::string error;
		if (!m_impl->QueueCallback(token, callback, {context}, error))
			return WupsServiceStatus::InternalError;
	}
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ConfigOpen(WupsOwnerToken token,
														std::string& error)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
	{
		error = "config open rejected a stale owner generation";
		return status;
	}
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::uint32_t callback{};
	std::uint32_t root{};
	{
		std::lock_guard lock(owner->config.mutex);
		if (!owner->config.registered)
			return WupsServiceStatus::NotFound;
		if (owner->config.menuOpen)
			return WupsServiceStatus::Busy;
		owner->config.categories.clear();
		owner->config.items.clear();
		root = m_impl->NewHandle();
		owner->config.root = root;
		owner->config.categories.emplace(root,
										 Impl::ConfigCategory{root, owner->config.name, true});
		owner->config.menuOpen = true;
		callback = owner->config.openCallback;
	}
	if (!m_impl->QueueCallback(token, callback, {root}, error))
	{
		std::lock_guard lock(owner->config.mutex);
		owner->config.menuOpen = false;
		owner->config.categories.clear();
		owner->config.items.clear();
		return WupsServiceStatus::InternalError;
	}
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ConfigClose(WupsOwnerToken token,
														 std::string& error)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
	{
		error = "config close rejected a stale owner generation";
		return status;
	}
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::vector<std::pair<std::uint32_t, std::uint32_t>> itemCallbacks;
	std::uint32_t closeCallback{};
	{
		std::lock_guard lock(owner->config.mutex);
		if (!owner->config.menuOpen)
			return WupsServiceStatus::NotFound;
		owner->config.menuOpen = false;
		for (const auto& [handle, item] : owner->config.items)
			if (item.model.callbacks.close)
				itemCallbacks.emplace_back(
					item.model.callbacks.close, item.model.callbacks.context);
		closeCallback = owner->config.closeCallback;
	}
	for (const auto& [callback, context] : itemCallbacks)
	{
		if (!m_impl->QueueCallback(token, callback, {context}, error))
			return WupsServiceStatus::InternalError;
	}
	if (closeCallback != 0)
	{
		if (!m_impl->QueueCallback(token, closeCallback, {}, error))
			return WupsServiceStatus::InternalError;
	}
	return WupsServiceStatus::Success;
}

std::optional<WupsConfigModel> AromaCompatibilityRuntime::ConfigSnapshot(
	WupsOwnerToken token) const
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return std::nullopt;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return std::nullopt;
	WupsConfigModel model;
	model.pluginName = owner->metadata.name;
	model.pluginVersion = owner->metadata.version;
	model.abiVersion = owner->metadata.abiVersion.ToString();
	if (owner->metadata.abiVersion < WupsVersion{0, 9, 1})
		model.compatibilityWarning = "Legacy WUPS ABI compatibility mode";
	std::lock_guard lock(owner->config.mutex);
	model.menuOpen = owner->config.menuOpen;
	if (owner->config.root == 0 ||
		!owner->config.categories.contains(owner->config.root))
		return model;
	std::set<std::uint32_t> visited;
	std::function<std::optional<WupsConfigCategoryModel>(std::uint32_t)> copy;
	copy = [&](std::uint32_t handle)
		-> std::optional<WupsConfigCategoryModel> {
		if (!visited.insert(handle).second)
			return std::nullopt;
		const auto found = owner->config.categories.find(handle);
		if (found == owner->config.categories.end())
			return std::nullopt;
		WupsConfigCategoryModel category;
		category.handle = handle;
		category.name = found->second.name;
		for (const auto child : found->second.categories)
		{
			auto childCopy = copy(child);
			if (!childCopy)
				return std::nullopt;
			category.categories.push_back(std::move(*childCopy));
		}
		for (const auto item : found->second.items)
		{
			const auto itemFound = owner->config.items.find(item);
			if (itemFound == owner->config.items.end())
				return std::nullopt;
			category.items.push_back(itemFound->second.model);
		}
		return category;
	};
	model.root = copy(owner->config.root);
	return model;
}

namespace
{
	[[nodiscard]] bool IsObserverCombo(WupsButtonComboType type)
	{
		return type == WupsButtonComboType::HoldObserver ||
			   type == WupsButtonComboType::PressDownObserver ||
			   type == WupsButtonComboType::PressReleaseObserver;
	}

	[[nodiscard]] bool ValidComboDefinition(
		const WupsButtonComboDefinition& definition)
	{
		const auto type = static_cast<std::uint32_t>(definition.type);
		if (!ValidDisplayName(definition.label) ||
			definition.controllerMask == 0 || definition.buttons == 0 ||
			definition.callback == 0 ||
			type < static_cast<std::uint32_t>(WupsButtonComboType::Hold) ||
			type > static_cast<std::uint32_t>(
					   WupsButtonComboType::PressReleaseObserver))
			return false;
		const bool hold = definition.type == WupsButtonComboType::Hold ||
						  definition.type == WupsButtonComboType::HoldObserver;
		return hold ? definition.holdDurationMilliseconds != 0 &&
						  definition.holdDurationMilliseconds <=
							  kMaximumComboHoldMilliseconds
					: definition.holdDurationMilliseconds == 0;
	}

	template<typename Combos>
	[[nodiscard]] WupsButtonComboStatus ComboAvailability(
		const Combos& combos,
		const WupsButtonComboDefinition& candidate,
		std::uint32_t ignoredHandle = 0)
	{
		if (IsObserverCombo(candidate.type))
			return WupsButtonComboStatus::Valid;
		for (const auto& [handle, combo] : combos)
		{
			if (handle == ignoredHandle ||
				IsObserverCombo(combo.definition.type))
				continue;
			if ((candidate.controllerMask &
				 combo.definition.controllerMask) != 0 &&
				(candidate.buttons & combo.definition.buttons) != 0)
				return WupsButtonComboStatus::Conflict;
		}
		return WupsButtonComboStatus::Valid;
	}
} // namespace

WupsServiceStatus AromaCompatibilityRuntime::ButtonComboCreate(
	WupsOwnerToken token, const WupsButtonComboDefinition& definition,
	std::uint32_t& handle, WupsButtonComboStatus& comboStatus)
{
	handle = 0;
	comboStatus = WupsButtonComboStatus::Invalid;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::string error;
	if (!ValidComboDefinition(definition) ||
		!m_impl->ValidateGuestCallback(*owner.owner, definition.callback, error))
		return WupsServiceStatus::InvalidArgument;
	std::lock_guard lock(owner->mutex);
	if (owner->combos.size() >= 128)
		return WupsServiceStatus::LimitExceeded;
	comboStatus = ComboAvailability(owner->combos, definition);
	handle = m_impl->NewHandle();
	owner->combos.emplace(handle, Impl::ComboState{
									  handle, definition, comboStatus});
	return comboStatus == WupsButtonComboStatus::Conflict ? WupsServiceStatus::Conflict : WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ButtonComboUpdate(
	WupsOwnerToken token, std::uint32_t handle,
	const WupsButtonComboDefinition& definition,
	WupsButtonComboStatus& comboStatus)
{
	comboStatus = WupsButtonComboStatus::Invalid;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::string error;
	if (handle == 0 || !ValidComboDefinition(definition) ||
		!m_impl->ValidateGuestCallback(*owner.owner, definition.callback, error))
		return WupsServiceStatus::InvalidArgument;
	std::lock_guard lock(owner->mutex);
	const auto found = owner->combos.find(handle);
	if (found == owner->combos.end())
		return WupsServiceStatus::NotFound;
	comboStatus = ComboAvailability(owner->combos, definition, handle);
	found->second.definition = definition;
	found->second.status = comboStatus;
	found->second.holdFired = false;
	return comboStatus == WupsButtonComboStatus::Conflict ? WupsServiceStatus::Conflict : WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ButtonComboRemove(
	WupsOwnerToken token, std::uint32_t handle)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::lock_guard lock(owner->mutex);
	return owner->combos.erase(handle) != 0 ? WupsServiceStatus::Success : WupsServiceStatus::NotFound;
}

WupsServiceStatus AromaCompatibilityRuntime::ButtonComboGet(
	WupsOwnerToken token, std::uint32_t handle,
	WupsButtonComboDefinition& definition,
	WupsButtonComboStatus& comboStatus) const
{
	definition = {};
	comboStatus = WupsButtonComboStatus::Invalid;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	std::lock_guard lock(owner->mutex);
	const auto found = owner->combos.find(handle);
	if (found == owner->combos.end())
		return WupsServiceStatus::NotFound;
	definition = found->second.definition;
	comboStatus = found->second.status;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ButtonComboCheckAvailable(
	WupsOwnerToken token, const WupsButtonComboDefinition& definition,
	WupsButtonComboStatus& comboStatus) const
{
	comboStatus = WupsButtonComboStatus::Invalid;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	if (!ValidComboDefinition(definition))
		return WupsServiceStatus::InvalidArgument;
	std::lock_guard lock(owner->mutex);
	comboStatus = ComboAvailability(owner->combos, definition);
	return comboStatus == WupsButtonComboStatus::Conflict ? WupsServiceStatus::Conflict : WupsServiceStatus::Success;
}

void AromaCompatibilityRuntime::SubmitButtonSample(
	const WupsButtonSample& sample)
{
	struct Pending
	{
		WupsOwnerToken owner;
		std::uint32_t callback{};
		std::uint32_t context{};
		std::uint32_t controller{};
		std::uint32_t handle{};
	};
	std::vector<Pending> callbacks;
	std::vector<WupsOwnerToken> tokens;
	{
		std::lock_guard lock(m_impl->registryMutex);
		tokens.reserve(m_impl->owners.size());
		for (const auto& [id, owner] : m_impl->owners)
			tokens.push_back(owner->token);
	}
	for (const auto token : tokens)
	{
		WupsServiceStatus status;
		auto owner = m_impl->Pin(token, status);
		if (!owner ||
			!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
			continue;
		std::lock_guard lock(owner->mutex);
		for (auto& [handle, combo] : owner->combos)
		{
			const auto& definition = combo.definition;
			if ((sample.controllerMask & definition.controllerMask) == 0)
				continue;
			bool fired{};
			switch (definition.type)
			{
			case WupsButtonComboType::PressDown:
			case WupsButtonComboType::PressDownObserver:
				fired = (sample.pressed & definition.buttons) ==
						definition.buttons;
				break;
			case WupsButtonComboType::PressRelease:
			case WupsButtonComboType::PressReleaseObserver:
				fired = (sample.released & definition.buttons) ==
						definition.buttons;
				break;
			case WupsButtonComboType::Hold:
			case WupsButtonComboType::HoldObserver:
				if ((sample.held & definition.buttons) != definition.buttons)
					combo.holdFired = false;
				else if (!combo.holdFired &&
						 sample.heldFor >= std::chrono::milliseconds(
											   definition.holdDurationMilliseconds))
				{
					combo.holdFired = true;
					fired = true;
				}
				break;
			default:
				break;
			}
			if (fired)
				callbacks.push_back({token, definition.callback,
									 definition.context,
									 sample.controllerMask & definition.controllerMask,
									 handle});
		}
	}
	for (const auto& callback : callbacks)
	{
		std::string error;
		(void)m_impl->QueueCallback(callback.owner, callback.callback,
									{callback.controller, callback.handle, callback.context}, error);
	}
}

WupsServiceStatus AromaCompatibilityRuntime::ReentGet(WupsOwnerToken token,
													  std::uint32_t pluginId, std::uint32_t& context) const
{
	context = 0;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		return WupsServiceStatus::PermissionDenied;
	if (!m_impl->options.platform || pluginId == 0)
		return WupsServiceStatus::InvalidArgument;
	const auto thread = m_impl->options.platform->CurrentGuestThreadId();
	if (thread == 0)
		return WupsServiceStatus::InvalidArgument;
	std::lock_guard lock(owner->mutex);
	const auto found = owner->reent.find({thread, pluginId});
	if (found != owner->reent.end())
		context = found->second.context;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ReentRegister(
	WupsOwnerToken token, std::uint32_t pluginId, std::uint32_t context,
	std::uint32_t cleanupCallback)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
	{
		cemuLog_log(LogType::Force,
					"WUPS ReentRegister: owner {}:{} pin failed (status {})", token.owner,
					token.generation, static_cast<int>(status));
		return status;
	}
	if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
	{
		cemuLog_log(LogType::Force,
					"WUPS ReentRegister: owner {}:{} lacks homebrew_wupsbackend",
					token.owner, token.generation);
		return WupsServiceStatus::PermissionDenied;
	}
	if (!m_impl->options.platform || pluginId == 0 || context == 0)
	{
		cemuLog_log(LogType::Force,
					"WUPS ReentRegister: invalid args pluginId=0x{:08x} context=0x{:08x}",
					pluginId, context);
		return WupsServiceStatus::InvalidArgument;
	}
	std::string error;
	if (!m_impl->ValidateGuestCallback(
			*owner.owner, cleanupCallback, error))
	{
		cemuLog_log(LogType::Force,
					"WUPS ReentRegister: cleanup callback 0x{:08x} rejected: {}",
					cleanupCallback, error);
		return WupsServiceStatus::InvalidArgument;
	}
	const auto thread = m_impl->options.platform->CurrentGuestThreadId();
	if (thread == 0)
	{
		cemuLog_log(LogType::Force,
					"WUPS ReentRegister: no current guest thread");
		return WupsServiceStatus::InvalidArgument;
	}
	std::lock_guard lock(owner->mutex);
	const auto key = std::pair{thread, pluginId};
	if (owner->reent.contains(key))
		return WupsServiceStatus::AlreadyExists;
	owner->reent.emplace(key, Impl::ReentState{
								  thread, pluginId, context, cleanupCallback});
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ReentUnregisterThread(
	WupsOwnerToken token, std::uint64_t threadId, std::string& error)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (threadId == 0)
		return WupsServiceStatus::InvalidArgument;
	std::vector<Impl::ReentState> removed;
	{
		std::lock_guard lock(owner->mutex);
		for (auto iterator = owner->reent.begin();
			 iterator != owner->reent.end();)
		{
			if (iterator->first.first != threadId)
			{
				++iterator;
				continue;
			}
			removed.push_back(iterator->second);
			iterator = owner->reent.erase(iterator);
		}
	}
	for (const auto& reent : removed)
		if (reent.cleanup != 0 &&
			!m_impl->QueueCallback(token, reent.cleanup,
								   {reent.context}, error))
			return WupsServiceStatus::InternalError;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::Impl::AllocateMapping(
	const std::shared_ptr<Owner>& owner, WupsOwnerToken token,
	std::uint32_t size, std::uint32_t alignment, bool writable,
	WupsMappedMemoryPurpose purpose, WupsMappedMemoryInfo& allocation,
	std::string& error)
{
	allocation = {};
	{
		std::lock_guard lock(owner->mutex);
		const auto maximum = options.maximumMappedBytes;
		if (owner->mappedBytes > maximum ||
			owner->mappedBytesReserved > maximum - owner->mappedBytes ||
			size > maximum - owner->mappedBytes - owner->mappedBytesReserved)
			return WupsServiceStatus::LimitExceeded;
		owner->mappedBytesReserved += size;
	}
	bool reserved = true;
	auto releaseReservation = [&] {
		if (!reserved)
			return;
		std::lock_guard lock(owner->mutex);
		cemu_assert_debug(owner->mappedBytesReserved >= size);
		owner->mappedBytesReserved -= size;
		reserved = false;
	};
	auto result = options.platform->AllocateMappedMemory(
		token, size, alignment, writable, purpose, error);
	if (!result)
	{
		releaseReservation();
		return WupsServiceStatus::IoError;
	}
	std::uint32_t end{};
	std::uint32_t physicalEnd{};
	if (result->address == 0 || result->physicalAddress == 0 ||
		result->size < size || !AddU32(result->address, result->size, end) ||
		!AddU32(result->physicalAddress, result->size, physicalEnd) ||
		(result->address & (alignment - 1U)) != 0)
	{
		std::string ignored;
		(void)options.platform->FreeMappedMemory(token, *result, ignored);
		releaseReservation();
		error = "platform returned an invalid or wrapping mapped allocation";
		return WupsServiceStatus::InternalError;
	}
	WupsServiceStatus publishStatus = WupsServiceStatus::Success;
	{
		std::lock_guard lock(owner->mutex);
		cemu_assert_debug(owner->mappedBytesReserved >= size);
		owner->mappedBytesReserved -= size;
		reserved = false;
		const auto maximum = options.maximumMappedBytes;
		if (owner->mappedBytes > maximum ||
			owner->mappedBytesReserved > maximum - owner->mappedBytes ||
			result->size > maximum - owner->mappedBytes -
							   owner->mappedBytesReserved)
			publishStatus = WupsServiceStatus::LimitExceeded;
		auto overlaps = [&](const auto& mappings) {
			for (const auto& [address, existing] : mappings)
			{
				const auto existingEnd = static_cast<std::uint64_t>(
											 existing.address) +
										 existing.size;
				const auto existingPhysicalEnd = static_cast<std::uint64_t>(
													 existing.physicalAddress) +
												 existing.size;
				if (!(end <= existing.address || result->address >= existingEnd) ||
					!(physicalEnd <= existing.physicalAddress ||
					  result->physicalAddress >= existingPhysicalEnd))
					return true;
			}
			return false;
		};
		if (publishStatus == WupsServiceStatus::Success &&
			(overlaps(owner->mappings) || overlaps(owner->mappingsBeingFreed)))
			publishStatus = WupsServiceStatus::Conflict;
		if (publishStatus == WupsServiceStatus::Success)
		{
			const bool inserted = owner->mappings.emplace(
													 result->address, *result)
									  .second;
			cemu_assert_debug(inserted);
			owner->mappedBytes += result->size;
		}
	}
	if (publishStatus != WupsServiceStatus::Success)
	{
		std::string ignored;
		(void)options.platform->FreeMappedMemory(token, *result, ignored);
		error = publishStatus == WupsServiceStatus::Conflict ? "platform returned an overlapping mapped allocation" : "platform returned an allocation that exceeds the mapped-memory quota";
		return publishStatus;
	}
	allocation = *result;
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::Impl::FreeMapping(
	const std::shared_ptr<Owner>& owner, WupsOwnerToken token,
	std::uint32_t address, std::string& error)
{
	WupsMappedMemoryInfo allocation;
	{
		std::lock_guard lock(owner->mutex);
		const auto found = owner->mappings.find(address);
		if (found == owner->mappings.end())
			return WupsServiceStatus::NotFound;
		allocation = found->second;
		const bool inserted = owner->mappingsBeingFreed.emplace(
														   address, allocation)
								  .second;
		if (!inserted)
			return WupsServiceStatus::Busy;
		owner->mappings.erase(found);
	}
	const auto freed = options.platform &&
					   options.platform->FreeMappedMemory(token, allocation, error);
	{
		std::lock_guard lock(owner->mutex);
		owner->mappingsBeingFreed.erase(address);
		if (freed)
		{
			cemu_assert_debug(owner->mappedBytes >= allocation.size);
			owner->mappedBytes -= allocation.size;
		}
		else
		{
			const bool inserted = owner->mappings.emplace(
													 address, allocation)
									  .second;
			cemu_assert_debug(inserted);
		}
	}
	if (!freed)
	{
		return WupsServiceStatus::IoError;
	}
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::MappedMemoryAllocate(
	WupsOwnerToken token, std::uint32_t size, std::uint32_t alignment,
	bool writable, WupsMappedMemoryPurpose purpose,
	WupsMappedMemoryInfo& allocation, std::string& error)
{
	allocation = {};
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!owner->permissions.mappedMemory ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_memorymapping"))
		return WupsServiceStatus::PermissionDenied;
	if (!m_impl->options.platform || size == 0 ||
		alignment < 4 || alignment > 1024U * 1024U ||
		!IsPowerOfTwo(alignment) ||
		static_cast<std::uint64_t>(size) + alignment - 1 >
			std::numeric_limits<std::uint32_t>::max())
		return WupsServiceStatus::InvalidArgument;
	return m_impl->AllocateMapping(owner.owner, token, size, alignment,
								   writable, purpose, allocation, error);
}

WupsServiceStatus AromaCompatibilityRuntime::MappedMemoryFree(
	WupsOwnerToken token, std::uint32_t address, std::string& error)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!owner->permissions.mappedMemory ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_memorymapping"))
		return WupsServiceStatus::PermissionDenied;
	return m_impl->FreeMapping(owner.owner, token, address, error);
}

WupsServiceStatus AromaCompatibilityRuntime::MappedMemoryEffectiveToPhysical(
	WupsOwnerToken token, std::uint32_t effective,
	std::uint32_t& physical) const
{
	physical = 0;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!owner->permissions.mappedMemory ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_memorymapping"))
		return WupsServiceStatus::PermissionDenied;
	std::lock_guard lock(owner->mutex);
	for (const auto& [address, mapping] : owner->mappings)
		if (effective >= address &&
			static_cast<std::uint64_t>(effective) <
				static_cast<std::uint64_t>(address) + mapping.size)
		{
			const auto translated = static_cast<std::uint64_t>(
										mapping.physicalAddress) +
									(effective - address);
			if (translated > std::numeric_limits<std::uint32_t>::max())
				return WupsServiceStatus::InternalError;
			physical = static_cast<std::uint32_t>(translated);
			return WupsServiceStatus::Success;
		}
	return WupsServiceStatus::NotFound;
}

WupsServiceStatus AromaCompatibilityRuntime::MappedMemoryPhysicalToEffective(
	WupsOwnerToken token, std::uint32_t physical,
	std::uint32_t& effective) const
{
	effective = 0;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!owner->permissions.mappedMemory ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_memorymapping"))
		return WupsServiceStatus::PermissionDenied;
	std::lock_guard lock(owner->mutex);
	for (const auto& [address, mapping] : owner->mappings)
		if (physical >= mapping.physicalAddress &&
			static_cast<std::uint64_t>(physical) <
				static_cast<std::uint64_t>(mapping.physicalAddress) + mapping.size)
		{
			const auto translated = static_cast<std::uint64_t>(address) +
									(physical - mapping.physicalAddress);
			if (translated > std::numeric_limits<std::uint32_t>::max())
				return WupsServiceStatus::InternalError;
			effective = static_cast<std::uint32_t>(translated);
			return WupsServiceStatus::Success;
		}
	return WupsServiceStatus::NotFound;
}

WupsServiceStatus AromaCompatibilityRuntime::NotificationAdd(
	WupsOwnerToken token, WupsNotificationModel notification,
	std::uint32_t& handle, std::string& error)
{
	handle = 0;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!owner->permissions.notifications ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_notifications"))
		return WupsServiceStatus::PermissionDenied;
	if (!m_impl->options.platform || notification.text.empty() ||
		notification.text.size() > 1024 ||
		std::ranges::any_of(notification.text, [](unsigned char character) {
			return character == 0 ||
				   (character < 0x20 && character != '\n' && character != '\t');
		}) ||
		notification.duration < std::chrono::milliseconds::zero() || notification.duration > std::chrono::hours(1) || (!notification.dynamic && notification.duration == std::chrono::milliseconds::zero()) || !m_impl->ValidateGuestCallback(*owner.owner, notification.callback, error))
		return WupsServiceStatus::InvalidArgument;
	{
		std::lock_guard lock(owner->mutex);
		if (owner->notifications.size() >=
			m_impl->options.maximumNotifications)
			return WupsServiceStatus::LimitExceeded;
		handle = m_impl->NewHandle();
		notification.handle = handle;
		notification.finished = false;
		owner->notifications.emplace(handle, notification);
	}
	m_impl->options.platform->ShowNotification(token, notification);
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::NotificationUpdate(
	WupsOwnerToken token, std::uint32_t handle, std::string_view text,
	std::string& error)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!owner->permissions.notifications ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_notifications"))
		return WupsServiceStatus::PermissionDenied;
	if (text.empty() || text.size() > 1024 ||
		std::ranges::any_of(text, [](unsigned char character) {
			return character == 0 ||
				   (character < 0x20 && character != '\n' && character != '\t');
		}))
		return WupsServiceStatus::InvalidArgument;
	WupsNotificationModel copy;
	{
		std::lock_guard lock(owner->mutex);
		const auto found = owner->notifications.find(handle);
		if (found == owner->notifications.end() || found->second.finished)
			return WupsServiceStatus::NotFound;
		if (!found->second.dynamic)
		{
			error = "only dynamic notifications can be updated";
			return WupsServiceStatus::Unsupported;
		}
		found->second.text = std::string(text);
		copy = found->second;
	}
	m_impl->options.platform->ShowNotification(token, copy);
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::NotificationFinish(
	WupsOwnerToken token, std::uint32_t handle, std::string& error)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!owner->permissions.notifications ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_notifications"))
		return WupsServiceStatus::PermissionDenied;
	WupsNotificationModel notification;
	{
		std::lock_guard lock(owner->mutex);
		const auto found = owner->notifications.find(handle);
		if (found == owner->notifications.end() || found->second.finished)
			return WupsServiceStatus::NotFound;
		found->second.finished = true;
		notification = found->second;
		owner->notifications.erase(found);
	}
	if (notification.callback != 0 &&
		!m_impl->QueueCallback(token, notification.callback,
							   {notification.handle, notification.callbackContext}, error))
		return WupsServiceStatus::InternalError;
	return WupsServiceStatus::Success;
}

std::vector<WupsNotificationModel>
AromaCompatibilityRuntime::NotificationSnapshot(WupsOwnerToken token) const
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return {};
	std::vector<WupsNotificationModel> result;
	std::lock_guard lock(owner->mutex);
	result.reserve(owner->notifications.size());
	for (const auto& [handle, notification] : owner->notifications)
		result.push_back(notification);
	std::ranges::sort(result, {}, &WupsNotificationModel::handle);
	return result;
}

WupsServiceStatus AromaCompatibilityRuntime::Log(WupsOwnerToken token,
												 WupsLogLevel level, std::string_view moduleName, std::string_view source,
												 std::string_view message)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!ValidAbiName(moduleName) ||
		!m_impl->ModuleAllowed(*owner.owner, moduleName))
		return WupsServiceStatus::PermissionDenied;
	if (!m_impl->options.platform || source.empty() || source.size() > 128 ||
		message.empty() || message.size() > kMaximumLogMessage ||
		std::ranges::any_of(source, [](unsigned char character) {
			return character == 0 || character < 0x20 || character == 0x7f;
		}) ||
		std::ranges::any_of(message, [](unsigned char character) {
			return character == 0 ||
				   (character < 0x20 && character != '\n' && character != '\t');
		}) ||
		static_cast<unsigned>(level) > static_cast<unsigned>(WupsLogLevel::Verbose))
		return WupsServiceStatus::InvalidArgument;
	const auto now = std::chrono::steady_clock::now();
	{
		std::lock_guard lock(owner->mutex);
		while (!owner->recentLogs.empty() &&
			   now - owner->recentLogs.front() >= std::chrono::seconds(1))
			owner->recentLogs.pop_front();
		if (owner->recentLogs.size() >= 100)
			return WupsServiceStatus::LimitExceeded;
		owner->recentLogs.push_back(now);
	}
	m_impl->options.platform->Log(token, level, moduleName, source, message);
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ContentRedirectAdd(
	WupsOwnerToken token, std::string_view virtualPath,
	const std::filesystem::path& sourcePath, std::int32_t priority,
	bool writable, std::uint32_t& handle, std::string& error)
{
	handle = 0;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!owner->permissions.contentRedirection ||
		!owner->permissions.filesystemRead ||
		(writable && !owner->permissions.filesystemWrite) ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_content_redirection"))
		return WupsServiceStatus::PermissionDenied;
	const auto normalized = NormalizeVirtualPath(virtualPath);
	if (!normalized || sourcePath.empty() || !sourcePath.is_absolute())
		return WupsServiceStatus::InvalidArgument;
	std::error_code code;
	const auto source = std::filesystem::weakly_canonical(sourcePath, code);
	if (code || !std::filesystem::exists(source, code) ||
		std::filesystem::is_symlink(
			std::filesystem::symlink_status(source, code)))
	{
		error = "content source is missing, inaccessible, or a symlink";
		return WupsServiceStatus::InvalidArgument;
	}
	bool withinRoot{};
	for (const auto& configured : m_impl->options.contentRoots)
	{
		const auto root = std::filesystem::weakly_canonical(configured, code);
		if (!code && PathStartsWith(source, root))
		{
			withinRoot = true;
			break;
		}
		code.clear();
	}
	if (!withinRoot)
	{
		error = "content source is outside every configured content root";
		return WupsServiceStatus::PermissionDenied;
	}
	std::lock_guard lock(owner->mutex);
	if (owner->redirects.size() >= 256)
		return WupsServiceStatus::LimitExceeded;
	handle = m_impl->NewHandle();
	owner->redirects.emplace(handle, WupsContentRedirectRule{
										 handle, *normalized, source, priority, writable, true});
	return WupsServiceStatus::Success;
}

WupsServiceStatus AromaCompatibilityRuntime::ContentRedirectRemove(
	WupsOwnerToken token, std::uint32_t handle)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!owner->permissions.contentRedirection ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_content_redirection"))
		return WupsServiceStatus::PermissionDenied;
	std::lock_guard lock(owner->mutex);
	return owner->redirects.erase(handle) != 0 ? WupsServiceStatus::Success : WupsServiceStatus::NotFound;
}

WupsServiceStatus AromaCompatibilityRuntime::ContentRedirectSetActive(
	WupsOwnerToken token, std::uint32_t handle, bool active)
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return status;
	if (!owner->permissions.contentRedirection ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_content_redirection"))
		return WupsServiceStatus::PermissionDenied;
	std::lock_guard lock(owner->mutex);
	const auto found = owner->redirects.find(handle);
	if (found == owner->redirects.end())
		return WupsServiceStatus::NotFound;
	found->second.active = active;
	return WupsServiceStatus::Success;
}

std::optional<std::filesystem::path>
AromaCompatibilityRuntime::ResolveContentPath(WupsOwnerToken token,
											  std::string_view virtualPath, bool write, std::string& error) const
{
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
	{
		error = "content lookup rejected a stale owner generation";
		return std::nullopt;
	}
	if (!owner->permissions.contentRedirection ||
		!owner->permissions.filesystemRead ||
		(write && !owner->permissions.filesystemWrite) ||
		!m_impl->ModuleAllowed(*owner.owner, "homebrew_content_redirection"))
	{
		error = "content lookup denied by package permissions";
		return std::nullopt;
	}
	const auto normalized = NormalizeVirtualPath(virtualPath);
	if (!normalized)
	{
		error = "content lookup contains an unsafe virtual path";
		return std::nullopt;
	}
	std::optional<WupsContentRedirectRule> selected;
	{
		std::lock_guard lock(owner->mutex);
		for (const auto& [handle, rule] : owner->redirects)
		{
			if (!rule.active || (write && !rule.writable) ||
				normalized->size() < rule.virtualPath.size() ||
				normalized->compare(0, rule.virtualPath.size(),
									rule.virtualPath) != 0 ||
				(normalized->size() != rule.virtualPath.size() &&
				 rule.virtualPath != "/" &&
				 (*normalized)[rule.virtualPath.size()] != '/'))
				continue;
			if (!selected || rule.virtualPath.size() > selected->virtualPath.size() ||
				(rule.virtualPath.size() == selected->virtualPath.size() &&
				 (rule.priority > selected->priority ||
				  (rule.priority == selected->priority &&
				   rule.handle < selected->handle))))
				selected = rule;
		}
	}
	if (!selected)
	{
		error = "no active content redirection matches the virtual path";
		return std::nullopt;
	}
	auto suffix = std::string_view(*normalized).substr(selected->virtualPath == "/" ? 1 : selected->virtualPath.size());
	if (!suffix.empty() && suffix.front() == '/')
		suffix.remove_prefix(1);
	std::error_code code;
	const auto resolved = std::filesystem::weakly_canonical(
		selected->sourcePath / std::filesystem::path(suffix), code);
	if (code || !PathStartsWith(resolved, selected->sourcePath))
	{
		error = "resolved content path escaped its registered source";
		return std::nullopt;
	}
	return resolved;
}

WupsOwnerResourceCounts AromaCompatibilityRuntime::ResourceCounts(
	WupsOwnerToken token) const
{
	WupsOwnerResourceCounts counts;
	WupsServiceStatus status;
	auto owner = m_impl->Pin(token, status);
	if (!owner)
		return counts;
	{
		std::lock_guard lock(owner->storage->mutex);
		counts.storageItems = owner->storage->nodes.empty() ? 0 : owner->storage->nodes.size() - 1;
	}
	{
		std::lock_guard lock(owner->config.mutex);
		counts.configCategories = owner->config.categories.size();
		counts.configItems = owner->config.items.size();
	}
	{
		std::lock_guard lock(owner->mutex);
		counts.buttonCombos = owner->combos.size();
		counts.reentContexts = owner->reent.size();
		counts.mappedAllocations = owner->mappings.size() +
								   owner->mappingsBeingFreed.size();
		counts.notifications = owner->notifications.size();
		counts.contentRedirects = owner->redirects.size();
		counts.pendingCallbacks = owner->pendingCallbacks;
	}
	return counts;
}

bool AromaCompatibilityRuntime::Impl::QueueCallback(WupsOwnerToken token,
													std::uint32_t callback, std::vector<std::uint32_t> arguments,
													std::string& error)
{
	if (!options.platform || callback == 0)
	{
		error = !options.platform ? "guest platform adapter is unavailable" : "guest callback pointer is null";
		return false;
	}
	WupsServiceStatus status;
	auto owner = Pin(token, status);
	if (!owner)
	{
		error = "guest callback rejected a stale owner generation";
		return false;
	}
	if (!ValidateGuestCallback(*owner.owner, callback, error))
		return false;
	const auto pending = std::make_shared<PendingCallback>(owner.owner);
	const std::weak_ptr gateWeak(asyncGate);
	if (!options.platform->QueueCpuTask(token, [gateWeak, pending, token, callback, arguments = std::move(arguments)] {
			(void)pending;
			const auto gate = gateWeak.lock();
			if (!gate)
				return;
			auto async = PinAsync(gate);
			if (!async.impl)
				return;
			auto* impl = async.impl;
			WupsServiceStatus pinStatus;
			auto callbackOwner = impl->Pin(token, pinStatus);
			if (!callbackOwner)
				return;
			std::uint32_t result{};
			std::string callbackError;
			if (!impl->Invoke(callbackOwner.owner, callback, arguments,
				result, callbackError) && impl->options.platform)
				impl->options.platform->Log(token, WupsLogLevel::Warning,
					"homebrew_wupsbackend", "queued callback", callbackError); }, error))
	{
		return false;
	}
	return true;
}

bool AromaCompatibilityRuntime::Impl::EnsureFunctionExport(
	const std::shared_ptr<Owner>& owner, std::string_view moduleName,
	std::string_view symbolName, std::uint32_t& address, std::string& error)
{
	address = 0;
	if (!options.platform)
	{
		error = "guest platform adapter is unavailable";
		return false;
	}
	const auto key = std::pair{std::string(moduleName), std::string(symbolName)};
	{
		std::lock_guard lock(owner->mutex);
		if (const auto found = owner->functionExports.find(key);
			found != owner->functionExports.end())
		{
			address = found->second;
			return true;
		}
	}
	// Serializes registration without holding the owner state mutex across the
	// platform call. The second check avoids duplicate publication.
	std::lock_guard registrationLock(owner->exportRegistrationMutex);
	{
		std::lock_guard lock(owner->mutex);
		if (const auto found = owner->functionExports.find(key);
			found != owner->functionExports.end())
		{
			address = found->second;
			return true;
		}
	}
	if (!owner->functionExportsRegistering.emplace(key).second)
	{
		error = fmt::format(
			"recursive registration of WUPS export {}.{}", moduleName, symbolName);
		return false;
	}
	const auto gateWeak = std::weak_ptr(asyncGate);
	const auto token = owner->token;
	const auto registered = options.platform->RegisterFunction(
		token, moduleName, symbolName,
		[gateWeak, token, module = key.first, symbol = key.second](
			std::span<const std::uint32_t> arguments,
			std::string& dispatchError) -> std::int32_t {
			const auto gate = gateWeak.lock();
			if (!gate)
			{
				dispatchError = "WUPS service runtime has been destroyed";
				return -0x1000;
			}
			auto async = PinAsync(gate);
			if (!async.impl)
			{
				dispatchError = "WUPS service runtime has been destroyed";
				return -0x1000;
			}
			WupsServiceStatus pinStatus;
			auto dispatchOwner = async.impl->Pin(token, pinStatus);
			if (!dispatchOwner)
			{
				dispatchError = "WUPS export rejected a stale owner generation";
				return -0x1000;
			}
			return async.impl->Dispatch(
				dispatchOwner.owner, module, symbol, arguments, dispatchError);
		},
		error);
	owner->functionExportsRegistering.erase(key);
	if (!registered || *registered == 0)
		return false;
	address = *registered;
	{
		std::lock_guard lock(owner->mutex);
		owner->functionExports.emplace(key, address);
	}
	return true;
}

namespace
{
	[[nodiscard]] bool Contains(std::span<const std::string_view> values,
								std::string_view value)
	{
		return std::ranges::find(values, value) != values.end();
	}

	constexpr std::array kWupsConfigExports{
		std::string_view{"WUPSConfigAPI_GetVersion"},
		std::string_view{"WUPSConfigAPI_InitEx"},
		std::string_view{"WUPSConfigAPI_Category_CreateEx"},
		std::string_view{"WUPSConfigAPI_Category_Destroy"},
		std::string_view{"WUPSConfigAPI_Category_AddCategory"},
		std::string_view{"WUPSConfigAPI_Category_AddItem"},
		std::string_view{"WUPSConfigAPI_Item_CreateEx"},
		std::string_view{"WUPSConfigAPI_Item_Destroy"},
		std::string_view{"WUPSConfigAPI_Menu_GetStatus"},
	};
	constexpr std::array kMemoryFunctions{
		std::string_view{"MemoryMappingEffectiveToPhysical"},
		std::string_view{"MemoryMappingPhysicalToEffective"},
	};
	constexpr std::array kMemoryData{
		std::string_view{"MEMAllocFromMappedMemory"},
		std::string_view{"MEMAllocFromMappedMemoryEx"},
		std::string_view{"MEMAllocFromMappedMemoryForGX2Ex"},
		std::string_view{"MEMFreeToMappedMemory"},
	};
	constexpr std::array kNotificationExports{
		std::string_view{"NMGetVersion"},
		std::string_view{"NMAddDynamicNotificationV2"},
		std::string_view{"NMAddStaticNotificationV2"},
		std::string_view{"NMAddDynamicNotification"},
		std::string_view{"NMAddStaticNotification"},
		std::string_view{"NMUpdateDynamicNotificationText"},
		std::string_view{"NMUpdateDynamicNotificationBackgroundColor"},
		std::string_view{"NMUpdateDynamicNotificationTextColor"},
		std::string_view{"NMFinishDynamicNotification"},
		std::string_view{"NMIsOverlayReady"},
	};
	constexpr std::array kContentExports{
		std::string_view{"CRGetVersion"},
		std::string_view{"CRAddFSLayer"},
		std::string_view{"CRRemoveFSLayer"},
		std::string_view{"CRSetActive"},
		std::string_view{"CRAddDevice"},
		std::string_view{"CRRemoveDevice"},
		std::string_view{"CRAddFSLayerEx"},
		std::string_view{"CRAddDeviceABI"},
		std::string_view{"CRRemoveDeviceABI"},
	};
	constexpr std::array kFunctionPatcherExports{
		std::string_view{"FPGetVersion"},
		std::string_view{"FPAddFunctionPatch"},
		std::string_view{"FPRemoveFunctionPatch"},
		std::string_view{"FPIsFunctionPatched"},
		std::string_view{"FunctionPatcher_AddFunctionPatch"},
		std::string_view{"FunctionPatcher_RemoveFunctionPatch"},
	};

	template<typename Owner>
	[[nodiscard]] bool PermissionForModule(
		const Owner& owner,
		std::string_view module, std::string& error)
	{
		if (module == "homebrew_memorymapping" &&
			!owner.permissions.mappedMemory)
			error = "mapped_memory permission is required";
		else if (module == "homebrew_notifications" &&
				 !owner.permissions.notifications)
			error = "notifications permission is required";
		else if (module == "homebrew_content_redirection" &&
				 (!owner.permissions.contentRedirection ||
				  !owner.permissions.filesystemRead))
			error = "content_redirection and filesystem.read permissions are required";
		else if (module == "homebrew_functionpatcher" &&
				 !owner.permissions.functionPatching)
			error = "function_patching permission is required";
		return error.empty();
	}
} // namespace

std::optional<std::uint32_t> AromaCompatibilityRuntime::ResolveImport(
	const CemodPackage& package, const WupsMetadata& metadata,
	std::uint64_t ownerId, std::uint32_t generation,
	std::string_view moduleName, std::string_view symbolName,
	WupsSymbolKind kind, std::string& error)
{
	error.clear();
	const WupsOwnerToken token{ownerId, generation};
	if (!RegisterOwner(package, metadata, token, error))
		return std::nullopt;
	WupsServiceStatus pinStatus;
	auto owner = m_impl->Pin(token, pinStatus);
	if (!owner)
	{
		error = "WUPS import resolution rejected a stale owner generation";
		return std::nullopt;
	}
	if (!ValidAbiName(moduleName) || !ValidAbiName(symbolName))
	{
		error = "WUPS import module or symbol name is invalid";
		return std::nullopt;
	}
	// Only WUMS/homebrew modules are gated by the manifest's module-permission
	// list. Base OS modules (coreinit, gx2, vpad, nn_*, ...) are resolved through
	// the normal Cafe RPL/HLE path and must never require an explicit homebrew
	// permission - every WUT plugin imports coreinit and gx2 unconditionally.
	if (moduleName.starts_with("homebrew_") &&
		!m_impl->ModuleAllowed(*owner.owner, moduleName))
	{
		error = fmt::format(
			"package '{}' has no permission for WUPS module '{}'",
			package.manifest.modId, moduleName);
		return std::nullopt;
	}

	// A WUT plugin loaded as an external RPL imports coreinit's default-heap
	// data exports the same way any Cafe RPL does, which would otherwise
	// resolve to the *game*'s own gCoreinitData->MEMAllocFromDefaultHeap*
	// function-pointer slots (see WupsPluginHeap.h for the full story on why
	// that is unsafe). Redirect these three data exports to a dedicated,
	// isolated plugin heap before any other resolution path sees them. This
	// takes priority even over normal Cafe RPL/HLE resolution; in practice
	// normal resolution never reaches here for these symbols anyway (coreinit
	// registers them as ordinary virtual-pointer data exports), but the
	// redirection must win unconditionally regardless of how it got here.
	if (kind == WupsSymbolKind::Data && moduleName == "coreinit" &&
		(symbolName == "MEMAllocFromDefaultHeap" ||
		 symbolName == "MEMAllocFromDefaultHeapEx" ||
		 symbolName == "MEMFreeToDefaultHeap"))
	{
		if (!m_impl->options.platform ||
			!m_impl->options.platform->SupportsPluginHeap())
		{
			error = fmt::format(
				"package '{}' plugin '{}' cannot resolve 'coreinit.{}': this "
				"platform has no isolated WUPS plugin heap to redirect libc "
				"allocations into",
				package.manifest.modId, metadata.name, symbolName);
			return std::nullopt;
		}
		const auto key = std::pair{std::string(moduleName), std::string(symbolName)};
		{
			std::lock_guard lock(owner->mutex);
			if (const auto found = owner->dataExports.find(key);
				found != owner->dataExports.end())
				return found->second;
		}
		std::uint32_t target{};
		if (!m_impl->EnsureFunctionExport(owner.owner, "__cemu_wups_data",
										  symbolName, target, error))
			return std::nullopt;
		const auto cell = m_impl->options.platform->AllocateGuestData(
			token, 4, 4, error);
		if (!cell)
			return std::nullopt;
		if (!m_impl->WriteGuestU32(*owner.owner, *cell, target, error))
		{
			m_impl->options.platform->FreeGuestData(token, *cell);
			return std::nullopt;
		}
		{
			std::lock_guard lock(owner->mutex);
			owner->dataExports.emplace(key, *cell);
			owner->guestData.push_back(*cell);
		}
		cemuLog_log(LogType::Force,
					"WUPS: redirected coreinit.{} -> cell 0x{:08x} (thunk 0x{:08x})",
					symbolName, *cell, target);
		return *cell;
	}

	bool backendKnown{};
	bool standardKnown{};
	if (kind == WupsSymbolKind::Function)
	{
		backendKnown = moduleName == "homebrew_wupsbackend" &&
					   (Contains(kWupsConfigExports, symbolName) ||
						FindWupsBackendExport(symbolName, kind) != nullptr);
		standardKnown =
			(moduleName == "homebrew_memorymapping" &&
			 Contains(kMemoryFunctions, symbolName)) ||
			(moduleName == "homebrew_notifications" &&
			 Contains(kNotificationExports, symbolName)) ||
			(moduleName == "homebrew_logging" &&
			 symbolName == "WUMSLogWrite") ||
			(moduleName == "homebrew_content_redirection" &&
			 Contains(kContentExports, symbolName)) ||
			(moduleName == "homebrew_functionpatcher" &&
			 Contains(kFunctionPatcherExports, symbolName));
	}
	else
		standardKnown = moduleName == "homebrew_memorymapping" &&
						Contains(kMemoryData, symbolName);

	std::string registryError;
	if (!backendKnown)
	{
		const ModuleProviderOwner requester{ownerId, generation, 1};
		auto resolved = m_impl->registry->Resolve(
			moduleName, symbolName, kind, requester, registryError);
		if (resolved)
		{
			const auto address = resolved->Address();
			{
				std::lock_guard lock(owner->mutex);
				owner->importLeases.push_back(std::move(*resolved));
			}
			return address;
		}
	}
	if (!backendKnown && !standardKnown)
	{
		error = fmt::format(
			"package '{}' plugin '{}' (WUPS {}) has unresolved mandatory {} "
			"import '{}.{}' for owner {} generation {}; normal Cafe RPL/HLE, "
			"WUPS backend, and registry resolution failed: {}",
			package.manifest.modId, metadata.name,
			metadata.abiVersion.ToString(),
			kind == WupsSymbolKind::Function ? "function" : "data",
			moduleName, symbolName, ownerId, generation, registryError);
		return std::nullopt;
	}
	if (moduleName == "homebrew_wupsbackend" &&
		FindWupsBackendExport(symbolName, kind) &&
		!owner->permissions.pluginManagement)
	{
		error = "plugin_management permission is required for the WUPS management API";
		return std::nullopt;
	}
	if ((moduleName == "homebrew_wupsbackend" ||
		 moduleName == "homebrew_logging") &&
		(!m_impl->options.platform ||
		 !m_impl->options.platform->SupportsOwnerScopedHeapPointers()))
	{
		error = fmt::format(
			"package '{}' plugin '{}' cannot resolve '{}.{}': this platform "
			"cannot attribute arbitrary WUT heap pointers to an owner generation; "
			"the heap-taking ABI is explicitly unsupported",
			package.manifest.modId, metadata.name, moduleName, symbolName);
		return std::nullopt;
	}
	if (moduleName == "homebrew_memorymapping" &&
		(!m_impl->options.platform ||
		 !m_impl->options.platform->SupportsMappedMemory()))
	{
		error = fmt::format(
			"package '{}' plugin '{}' cannot resolve '{}.{}': Cemu has no "
			"safe guest effective/physical mapped-memory allocator",
			package.manifest.modId, metadata.name, moduleName, symbolName);
		return std::nullopt;
	}
	if (!PermissionForModule(*owner.owner, moduleName, error))
		return std::nullopt;

	if (kind == WupsSymbolKind::Function)
	{
		std::uint32_t address{};
		if (!m_impl->EnsureFunctionExport(
				owner.owner, moduleName, symbolName, address, error))
			return std::nullopt;
		return address;
	}

	const auto key = std::pair{
		std::string(moduleName), std::string(symbolName)};
	{
		std::lock_guard lock(owner->mutex);
		if (const auto found = owner->dataExports.find(key);
			found != owner->dataExports.end())
			return found->second;
	}
	std::uint32_t target{};
	if (!m_impl->EnsureFunctionExport(owner.owner, "__cemu_wups_data",
									  symbolName, target, error))
		return std::nullopt;
	const auto cell = m_impl->options.platform->AllocateGuestData(
		token, 4, 4, error);
	if (!cell)
		return std::nullopt;
	if (!m_impl->WriteGuestU32(*owner.owner, *cell, target, error))
	{
		m_impl->options.platform->FreeGuestData(token, *cell);
		return std::nullopt;
	}
	{
		std::lock_guard lock(owner->mutex);
		owner->dataExports.emplace(key, *cell);
		owner->guestData.push_back(*cell);
	}
	return *cell;
}

std::optional<std::uint32_t> AromaCompatibilityRuntime::ResolveRuntimeModuleExport(
	WupsOwnerToken token, std::string_view moduleName, std::string_view symbolName,
	WupsSymbolKind kind, std::string& error)
{
	// Mirrors the tail of ResolveImport() for symbols a plugin resolves
	// dynamically via OSDynLoad_Acquire/OSDynLoad_FindExport instead of a
	// static RPL import - libwups' config API does this for
	// "homebrew_wupsbackend" rather than importing WUPSConfigAPI_* directly.
	// Unlike ResolveImport this never registers a brand-new owner: the plugin
	// is already running, so the owner must already be pinned.
	error.clear();
	WupsServiceStatus pinStatus;
	auto owner = m_impl->Pin(token, pinStatus);
	if (!owner)
	{
		error = "WUPS runtime export resolution rejected a stale owner generation";
		return std::nullopt;
	}
	if (!ValidAbiName(moduleName) || !ValidAbiName(symbolName))
	{
		error = "WUPS import module or symbol name is invalid";
		return std::nullopt;
	}
	if (moduleName.starts_with("homebrew_") &&
		!m_impl->ModuleAllowed(*owner.owner, moduleName))
	{
		error = fmt::format(
			"owner {} generation {} has no permission for WUPS module '{}'",
			token.owner, token.generation, moduleName);
		return std::nullopt;
	}

	bool backendKnown{};
	bool standardKnown{};
	if (kind == WupsSymbolKind::Function)
	{
		backendKnown = moduleName == "homebrew_wupsbackend" &&
					   (Contains(kWupsConfigExports, symbolName) ||
						FindWupsBackendExport(symbolName, kind) != nullptr);
		standardKnown =
			(moduleName == "homebrew_memorymapping" &&
			 Contains(kMemoryFunctions, symbolName)) ||
			(moduleName == "homebrew_notifications" &&
			 Contains(kNotificationExports, symbolName)) ||
			(moduleName == "homebrew_logging" &&
			 symbolName == "WUMSLogWrite") ||
			(moduleName == "homebrew_content_redirection" &&
			 Contains(kContentExports, symbolName)) ||
			(moduleName == "homebrew_functionpatcher" &&
			 Contains(kFunctionPatcherExports, symbolName));
	}
	else
		standardKnown = moduleName == "homebrew_memorymapping" &&
						Contains(kMemoryData, symbolName);

	if (!backendKnown && !standardKnown)
	{
		error = fmt::format(
			"owner {} generation {} requested unknown WUPS runtime export "
			"'{}.{}'",
			token.owner, token.generation, moduleName, symbolName);
		return std::nullopt;
	}
	if (moduleName == "homebrew_wupsbackend" &&
		FindWupsBackendExport(symbolName, kind) &&
		!owner->permissions.pluginManagement)
	{
		error = "plugin_management permission is required for the WUPS management API";
		return std::nullopt;
	}
	if ((moduleName == "homebrew_wupsbackend" ||
		 moduleName == "homebrew_logging") &&
		(!m_impl->options.platform ||
		 !m_impl->options.platform->SupportsOwnerScopedHeapPointers()))
	{
		error = fmt::format(
			"cannot resolve '{}.{}': this platform cannot attribute arbitrary "
			"WUT heap pointers to an owner generation; the heap-taking ABI is "
			"explicitly unsupported",
			moduleName, symbolName);
		return std::nullopt;
	}
	if (moduleName == "homebrew_memorymapping" &&
		(!m_impl->options.platform ||
		 !m_impl->options.platform->SupportsMappedMemory()))
	{
		error = fmt::format(
			"cannot resolve '{}.{}': Cemu has no safe guest effective/physical "
			"mapped-memory allocator",
			moduleName, symbolName);
		return std::nullopt;
	}
	if (!PermissionForModule(*owner.owner, moduleName, error))
		return std::nullopt;

	if (kind == WupsSymbolKind::Function)
	{
		std::uint32_t address{};
		if (!m_impl->EnsureFunctionExport(
				owner.owner, moduleName, symbolName, address, error))
			return std::nullopt;
		return address;
	}

	const auto key = std::pair{std::string(moduleName), std::string(symbolName)};
	{
		std::lock_guard lock(owner->mutex);
		if (const auto found = owner->dataExports.find(key);
			found != owner->dataExports.end())
			return found->second;
	}
	std::uint32_t target{};
	if (!m_impl->EnsureFunctionExport(owner.owner, "__cemu_wups_data",
									  symbolName, target, error))
		return std::nullopt;
	const auto cell = m_impl->options.platform->AllocateGuestData(token, 4, 4, error);
	if (!cell)
		return std::nullopt;
	if (!m_impl->WriteGuestU32(*owner.owner, *cell, target, error))
	{
		m_impl->options.platform->FreeGuestData(token, *cell);
		return std::nullopt;
	}
	{
		std::lock_guard lock(owner->mutex);
		owner->dataExports.emplace(key, *cell);
		owner->guestData.push_back(*cell);
	}
	return *cell;
}

bool AromaCompatibilityRuntime::PrepareHookInvocation(
	const CemodPackage& package, const WupsMetadata& metadata,
	std::uint64_t ownerId, std::uint32_t generation, WupsHookType type,
	WupsHookInvocation& invocation, std::string& error)
{
	invocation = {};
	error.clear();
	const WupsOwnerToken token{ownerId, generation};
	if (!RegisterOwner(package, metadata, token, error))
		return false;
	WupsServiceStatus pinStatus;
	auto owner = m_impl->Pin(token, pinStatus);
	if (!owner)
	{
		error = "WUPS hook preparation rejected a stale owner generation";
		return false;
	}
	if (type == WupsHookType::InitWutSockets &&
		!owner->permissions.network)
	{
		error = "INIT_WUT_SOCKETS requires the network permission";
		return false;
	}
	if (type == WupsHookType::InitReentFunctions &&
		metadata.abiVersion <= WupsVersion{0, 9, 0})
	{
		invocation.skip = true;
		return true;
	}
	if (type == WupsHookType::InitButtonCombo &&
		metadata.abiVersion <= WupsVersion{0, 8, 1})
	{
		invocation.skip = true;
		return true;
	}
	if (type == WupsHookType::InitStorageDeprecated)
	{
		error = fmt::format(
			"plugin '{}' uses the deprecated pre-0.10 storage hook; this "
			"backend rejects that ABI instead of guessing its layout",
			metadata.name);
		return false;
	}
	const auto requireHeapProvenance = [&] {
		if (m_impl->options.platform &&
			m_impl->options.platform->SupportsOwnerScopedHeapPointers())
			return true;
		error = fmt::format(
			"WUPS backend initialization hook {} is unsupported because this "
			"platform cannot attribute arbitrary WUT heap pointers to owner {} "
			"generation {}",
			static_cast<unsigned>(type), ownerId, generation);
		return false;
	};

	auto addFunctions = [&](std::span<const std::string_view> names) {
		for (const auto name : names)
		{
			std::uint32_t address{};
			if (!m_impl->EnsureFunctionExport(
					owner.owner, "__cemu_wups_hook", name, address, error))
				return false;
			invocation.argumentWords.push_back(address);
		}
		return true;
	};

	switch (type)
	{
	case WupsHookType::InitStorage:
	{
		if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		{
			error = "INIT_STORAGE requires homebrew_wupsbackend permission";
			return false;
		}
		if (!requireHeapProvenance())
			return false;
		const auto status = m_impl->EnsureStorageLoaded(*owner.owner, error);
		if (status != WupsServiceStatus::Success)
			return false;
		std::uint32_t root{};
		{
			std::lock_guard lock(owner->storage->mutex);
			root = owner->storage->root;
		}
		invocation.argumentWords = {2, root};
		constexpr std::array functions{
			std::string_view{"StorageSave"},
			std::string_view{"StorageReload"},
			std::string_view{"StorageWipe"},
			std::string_view{"StorageDelete"},
			std::string_view{"StorageCreate"},
			std::string_view{"StorageGetSub"},
			std::string_view{"StorageStore"},
			std::string_view{"StorageGet"},
			std::string_view{"StorageGetSize"},
		};
		if (!addFunctions(functions))
			return false;
		invocation.requireZeroResult = true;
		return true;
	}
	case WupsHookType::InitConfig:
		if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		{
			error = "INIT_CONFIG requires homebrew_wupsbackend permission";
			return false;
		}
		if (!requireHeapProvenance())
			return false;
		invocation.argumentWords = {1, owner->pluginIdentifier};
		// libwups' config library resolves its backend dynamically through
		// OSDynLoad_Acquire("homebrew_wupsbackend")/OSDynLoad_FindExport rather
		// than a static RPL import (see WupsDynLoadInterception.h), so a
		// well-formed plugin should now see WUPSCONFIG_API_RESULT_SUCCESS here.
		// The configuration menu is still an optional surface though - record a
		// non-zero status but let the plugin start rather than failing the whole
		// load over it.
		invocation.reportNonZeroResult = true;
		return true;
	case WupsHookType::InitButtonCombo:
	{
		if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		{
			error = "INIT_BUTTON_COMBO requires homebrew_wupsbackend permission";
			return false;
		}
		if (!requireHeapProvenance())
			return false;
		invocation.argumentWords = {1, owner->pluginIdentifier};
		constexpr std::array functions{
			std::string_view{"ButtonAdd"},
			std::string_view{"ButtonRemove"},
			std::string_view{"ButtonGetStatus"},
			std::string_view{"ButtonUpdateMeta"},
			std::string_view{"ButtonUpdateCallback"},
			std::string_view{"ButtonUpdateController"},
			std::string_view{"ButtonUpdateButtons"},
			std::string_view{"ButtonUpdateHold"},
			std::string_view{"ButtonGetMeta"},
			std::string_view{"ButtonGetCallback"},
			std::string_view{"ButtonGetInfo"},
			std::string_view{"ButtonCheck"},
			std::string_view{"ButtonDetect"},
		};
		if (!addFunctions(functions))
			return false;
		invocation.requireZeroResult = true;
		return true;
	}
	case WupsHookType::InitReentFunctions:
	{
		if (!m_impl->ModuleAllowed(*owner.owner, "homebrew_wupsbackend"))
		{
			error = "INIT_REENT_FUNCTIONS requires homebrew_wupsbackend permission";
			return false;
		}
		if (!requireHeapProvenance())
			return false;
		invocation.argumentWords = {2};
		constexpr std::array functions{
			std::string_view{"ReentGet"},
			std::string_view{"ReentAdd"},
		};
		if (!addFunctions(functions))
			return false;
		return true;
	}
	default:
		return true;
	}
}

bool AromaCompatibilityRuntime::ActivatePlugin(
	const CemodPackage& package, const WupsMetadata& metadata,
	std::uint64_t owner, std::uint32_t generation,
	std::span<const WupsPatchRequest> patches, std::string& error)
{
	error.clear();
	if (patches.empty())
		return true;
	if (!m_impl->patchManager)
	{
		error = fmt::format(
			"package '{}' plugin '{}' requested {} function patch(es), but the "
			"Cemu FunctionPatcher provider is unavailable",
			package.manifest.modId, metadata.name, patches.size());
		return false;
	}
	if (!m_impl->patchManager->Apply(patches, error))
	{
		error = fmt::format(
			"package '{}' plugin '{}' owner {} generation {} patch transaction "
			"failed: {}",
			package.manifest.modId, metadata.name, owner, generation, error);
		return false;
	}
	return true;
}

bool AromaCompatibilityRuntime::DeactivatePlugin(
	std::uint64_t owner, std::uint32_t generation, std::string& error)
{
	error.clear();
	if (m_impl->patchManager)
	{
		if (!m_impl->patchManager->RemoveOwner(
				WupsPatchOwner{owner, generation}, error))
		{
			error = fmt::format(
				"failed to remove patches for owner {} generation {}: {}",
				owner, generation, error);
			return false;
		}
	}
	return true;
}

bool AromaCompatibilityRuntime::ReleaseOwnerResources(
	std::uint64_t owner, std::uint32_t generation, std::string& error)
{
	error.clear();
	m_impl->Release({owner, generation});
	return true;
}

bool AromaCompatibilityRuntime::IsProcessInScope(
	const CemodPackage& package, std::string& reason) const
{
	reason.clear();
	if (package.manifest.scope.type != CemodScopeType::Process)
		return true;
	const auto process = CurrentProcess();
	for (const auto& rawTarget : package.manifest.scope.targets)
	{
		const auto target = Lower(rawTarget);
		if (target == "all" ||
			(target == "root_rpx" && process == WupsProcessKind::RootRpx) ||
			(target == "game" && process == WupsProcessKind::Game) ||
			(target == "wii_u_menu" && process == WupsProcessKind::WiiUMenu) ||
			(target == "game_and_menu" &&
			 (process == WupsProcessKind::Game ||
			  process == WupsProcessKind::WiiUMenu)))
			return true;
	}
	reason =
		"plugin process scope does not include the process currently emulated";
	return false;
}

std::shared_ptr<ModuleExportRegistry>
AromaCompatibilityRuntime::ExportRegistry() const
{
	return m_impl->registry;
}

std::shared_ptr<WupsFunctionPatchManager>
AromaCompatibilityRuntime::PatchManager() const
{
	return m_impl->patchManager;
}

void AromaCompatibilityRuntime::OnModuleLoaded(
	std::string_view moduleName, std::uint64_t lifetimeId)
{
	if (!m_impl->patchManager)
		return;
	std::string error;
	if (!m_impl->patchManager->OnModuleLoaded(
			{std::string(moduleName), lifetimeId}, error))
		cemuLog_log(LogType::Force,
					"WUPS: dynamic module '{}' patch transaction failed: {}",
					moduleName, error);
}

void AromaCompatibilityRuntime::OnModuleUnloading(
	std::string_view moduleName, std::uint64_t lifetimeId)
{
	if (!m_impl->patchManager)
		return;
	std::string error;
	if (!m_impl->patchManager->OnModuleUnloading(
			{std::string(moduleName), lifetimeId}, error))
		cemuLog_log(LogType::Force,
					"WUPS: dynamic module '{}' pre-unload patch restoration failed: {}",
					moduleName, error);
}

void AromaCompatibilityRuntime::SetModuleEventDetach(
	std::function<void()> detach)
{
	std::lock_guard lock(m_impl->compatibilityMutex);
	m_impl->detachModuleEvents = std::move(detach);
}

std::int32_t AromaCompatibilityRuntime::Impl::DispatchBackend(
	const std::shared_ptr<Owner>& owner,
	const WupsBackendExportDescriptor& export_,
	std::span<const std::uint32_t> arguments, std::string& error)
{
	const auto result = [](WupsBackendApiError value) {
		return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
	};
	const auto require = [&](std::size_t count) {
		if (arguments.size() >= count)
			return true;
		error = fmt::format("{}.{} requires {} ABI words, received {}",
							"homebrew_wupsbackend", export_.name, count, arguments.size());
		return false;
	};
	if (!owner->permissions.pluginManagement || !options.backendManagement)
		return result(WupsBackendApiError::UnsupportedCommand);

	auto readHandles = [&](std::uint32_t address, std::uint32_t count,
						   std::vector<std::uint32_t>& handles) {
		handles.clear();
		if (count == 0)
			return true;
		if (address == 0 || count > WupsBackendManagementRuntime::kMaximumDataHandles ||
			static_cast<std::uint64_t>(count) * 4 >
				std::numeric_limits<std::uint32_t>::max())
			return false;
		std::vector<std::byte> bytes(static_cast<std::size_t>(count) * 4);
		if (!ReadGuest(*owner, address, bytes, WupsGuestAccess::Read, error))
			return false;
		handles.reserve(count);
		for (std::uint32_t index = 0; index < count; ++index)
			handles.push_back(ReadU32(bytes, index * 4));
		return true;
	};
	auto writeHandles = [&](std::uint32_t address,
							std::span<const std::uint32_t> handles) {
		if (handles.empty())
			return true;
		if (address == 0 || handles.size() >
								std::numeric_limits<std::uint32_t>::max() / 4)
			return false;
		std::vector<std::byte> bytes(handles.size() * 4);
		for (std::size_t index = 0; index < handles.size(); ++index)
		{
			const auto value = handles[index];
			bytes[index * 4] = static_cast<std::byte>(value >> 24);
			bytes[index * 4 + 1] = static_cast<std::byte>(value >> 16);
			bytes[index * 4 + 2] = static_cast<std::byte>(value >> 8);
			bytes[index * 4 + 3] = static_cast<std::byte>(value);
		}
		return WriteGuest(*owner, address, bytes, error);
	};
	auto copyString = [](std::span<std::byte> output, std::string_view input) {
		std::ranges::fill(output, std::byte{});
		const auto count = std::min(input.size(), output.size() - 1);
		std::memcpy(output.data(), input.data(), count);
	};
	auto putU32 = [](std::span<std::byte> output, std::uint32_t value) {
		output[0] = static_cast<std::byte>(value >> 24);
		output[1] = static_cast<std::byte>(value >> 16);
		output[2] = static_cast<std::byte>(value >> 8);
		output[3] = static_cast<std::byte>(value);
	};
	auto writeInformation = [&](std::uint32_t address,
								const CemodPackage& package) {
		GuestWupsPluginInformationV2 wire{};
		putU32(wire.informationVersion,
			   kWupsBackendPluginInformationVersion);
		const auto& metadata = package.wups->metadata;
		copyString(wire.name, metadata.name);
		copyString(wire.author, metadata.author);
		copyString(wire.buildTimestamp, metadata.buildTimestamp);
		copyString(wire.description, metadata.description);
		copyString(wire.license, metadata.license);
		copyString(wire.version, metadata.version);
		copyString(wire.storageId, metadata.storageId);
		putU32(wire.size, static_cast<std::uint32_t>(package.PayloadBytes().size()));
		return WriteGuest(*owner, address,
						  std::as_bytes(std::span{&wire, 1}), error);
	};
	auto loadBytes = [&](WupsBackendInputType inputType,
						 std::uint32_t pathAddress, std::uint32_t bufferAddress,
						 std::uint32_t size, std::vector<std::byte>& bytes) {
		bytes.clear();
		if (inputType == WupsBackendInputType::Buffer)
		{
			if (bufferAddress == 0 || size == 0 ||
				size > CemodPackage::kMaximumPayloadBytes)
				return WupsBackendApiError::InvalidArgument;
			bytes.resize(size);
			if (!ReadGuest(*owner, bufferAddress, bytes,
						   WupsGuestAccess::Read, error))
				return WupsBackendApiError::InvalidArgument;
			return WupsBackendApiError::None;
		}
		if (inputType != WupsBackendInputType::Path || pathAddress == 0)
			return WupsBackendApiError::InvalidArgument;
		if (!owner->permissions.filesystemRead)
			return WupsBackendApiError::UnsupportedCommand;
		std::string guestPath;
		if (!ReadGuestString(*owner, pathAddress, kMaximumContentPath,
							 guestPath, error))
			return WupsBackendApiError::InvalidArgument;
		std::string relative = guestPath;
		if (const auto marker = relative.find("/vol/external01/");
			marker != std::string::npos)
			relative.erase(0, marker + std::string_view("/vol/external01/").size());
		else if (const auto scheme = relative.find(":/");
				 scheme != std::string::npos)
			relative.erase(0, scheme + 2);
		while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\'))
			relative.erase(relative.begin());
		const std::filesystem::path requested(relative);
		if (requested.empty() || requested.is_absolute() ||
			std::ranges::find(requested, "..") != requested.end())
			return WupsBackendApiError::InvalidArgument;
		for (const auto& configuredRoot : options.contentRoots)
		{
			std::error_code code;
			const auto root = std::filesystem::weakly_canonical(configuredRoot, code);
			if (code)
				continue;
			const auto path = std::filesystem::weakly_canonical(root / requested, code);
			if (code || !PathStartsWith(path, root) ||
				!std::filesystem::is_regular_file(path, code) || code)
				continue;
			const auto fileSize = std::filesystem::file_size(path, code);
			if (code || fileSize == 0 || fileSize > CemodPackage::kMaximumPayloadBytes)
				return WupsBackendApiError::InvalidSize;
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				continue;
			bytes.resize(static_cast<std::size_t>(fileSize));
			stream.read(reinterpret_cast<char*>(bytes.data()),
						static_cast<std::streamsize>(bytes.size()));
			if (!stream)
			{
				bytes.clear();
				return WupsBackendApiError::FileNotFound;
			}
			return WupsBackendApiError::None;
		}
		return WupsBackendApiError::FileNotFound;
	};
	auto inspectInput = [&](WupsBackendInputType inputType,
							std::uint32_t pathAddress, std::uint32_t bufferAddress,
							std::uint32_t size, CemodPackage& package,
							WupsBackendParseError& parseError) {
		parseError = WupsBackendParseError::Unknown;
		std::vector<std::byte> bytes;
		const auto loadResult = loadBytes(inputType, pathAddress, bufferAddress,
										  size, bytes);
		if (loadResult != WupsBackendApiError::None)
			return loadResult;
		std::string inspectError;
		auto inspection = WupsBinaryInspector::Inspect(bytes, inspectError);
		if (!inspection)
		{
			if (inspectError.find("version") != std::string::npos ||
				inspectError.find("ABI") != std::string::npos)
				parseError = WupsBackendParseError::IncompatibleVersion;
			return WupsBackendApiError::FileNotFound;
		}
		parseError = WupsBackendParseError::None;
		package = MakeDynamicWupsPackage(std::move(bytes),
										 std::move(*inspection), owner->package);
		return WupsBackendApiError::None;
	};
	const WupsProcessKey processKey{
		static_cast<std::uint8_t>(process.load()), owner->titleId};

	switch (export_.id)
	{
	case WupsBackendExportId::GetApiVersion:
		if (!require(1) || arguments[0] == 0 ||
			!WriteGuestU32(*owner, arguments[0], kWupsBackendApiVersion, error))
			return result(WupsBackendApiError::InvalidArgument);
		return result(WupsBackendApiError::None);
	case WupsBackendExportId::GetNumberOfLoadedPlugins:
		if (!require(1) || arguments[0] == 0 ||
			!WriteGuestU32(*owner, arguments[0], static_cast<std::uint32_t>(options.backendManagement->LoadedContainers().size()), error))
			return result(WupsBackendApiError::InvalidArgument);
		return result(WupsBackendApiError::None);
	case WupsBackendExportId::WillReloadPluginsOnNextLaunch:
		if (!require(1) || arguments[0] == 0 ||
			!WriteGuestBool(*owner, arguments[0],
							options.backendManagement->HasPendingPlan(processKey), error))
			return result(WupsBackendApiError::InvalidArgument);
		return result(WupsBackendApiError::None);
	case WupsBackendExportId::GetLoadedPlugins:
	{
		if (!require(4) || arguments[3] == 0 ||
			(arguments[1] != 0 && arguments[0] == 0))
			return result(WupsBackendApiError::InvalidArgument);
		if (!WriteGuestU32(*owner, arguments[3],
						   kWupsBackendPluginInformationVersion, error))
			return result(WupsBackendApiError::InvalidArgument);
		const auto loaded = options.backendManagement->LoadedContainers();
		const auto count = std::min<std::size_t>(loaded.size(), arguments[1]);
		std::vector<std::uint32_t> handles;
		handles.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
			handles.push_back(loaded[index].publicHandle);
		if (!writeHandles(arguments[0], handles) ||
			(arguments[2] != 0 && !WriteGuestU32(*owner, arguments[2],
												 static_cast<std::uint32_t>(count), error)))
			return result(WupsBackendApiError::InvalidArgument);
		return result(WupsBackendApiError::None);
	}
	case WupsBackendExportId::LoadAndLinkByDataHandle:
	{
		if (!require(2) || arguments[0] == 0 || arguments[1] == 0)
			return result(WupsBackendApiError::InvalidArgument);
		std::vector<std::uint32_t> handles;
		if (!readHandles(arguments[0], arguments[1], handles))
			return result(WupsBackendApiError::InvalidArgument);
		return result(options.backendManagement->ScheduleNextLaunch(
						  processKey, handles)
						  ? WupsBackendApiError::None
						  : WupsBackendApiError::InvalidSize);
	}
	case WupsBackendExportId::DeletePluginData:
	{
		if (!require(2))
			return result(WupsBackendApiError::InvalidArgument);
		if (arguments[0] == 0 || arguments[1] == 0)
			return result(WupsBackendApiError::None);
		std::vector<std::uint32_t> handles;
		if (!readHandles(arguments[0], arguments[1], handles))
			return result(WupsBackendApiError::InvalidArgument);
		options.backendManagement->DeletePluginData(handles);
		return result(WupsBackendApiError::None);
	}
	case WupsBackendExportId::LoadPluginAsDataByPath:
	case WupsBackendExportId::LoadPluginAsDataByBuffer:
	case WupsBackendExportId::LoadPluginAsData:
	{
		WupsBackendInputType inputType{};
		std::uint32_t path{}, buffer{}, size{}, output{};
		if (export_.id == WupsBackendExportId::LoadPluginAsData)
		{
			if (!require(5) || arguments[0] > 1)
				return result(WupsBackendApiError::InvalidArgument);
			inputType = static_cast<WupsBackendInputType>(arguments[0]);
			path = arguments[1];
			buffer = arguments[2];
			size = arguments[3];
			output = arguments[4];
		}
		else if (export_.id == WupsBackendExportId::LoadPluginAsDataByPath)
		{
			if (!require(2))
				return result(WupsBackendApiError::InvalidArgument);
			inputType = WupsBackendInputType::Path;
			output = arguments[0];
			path = arguments[1];
		}
		else
		{
			if (!require(3))
				return result(WupsBackendApiError::InvalidArgument);
			inputType = WupsBackendInputType::Buffer;
			output = arguments[0];
			buffer = arguments[1];
			size = arguments[2];
		}
		if (output == 0)
			return result(WupsBackendApiError::InvalidArgument);
		CemodPackage package;
		WupsBackendParseError parseError;
		const auto status = inspectInput(inputType, path, buffer, size,
										 package, parseError);
		if (status != WupsBackendApiError::None)
			return result(status);
		const auto handle = options.backendManagement->CreatePluginData(
			std::move(package));
		if (!handle)
			return result(WupsBackendApiError::FailedAllocation);
		if (!WriteGuestU32(*owner, output, *handle, error))
		{
			const std::array handles{*handle};
			options.backendManagement->DeletePluginData(handles);
			return result(WupsBackendApiError::InvalidArgument);
		}
		return result(WupsBackendApiError::None);
	}
	case WupsBackendExportId::GetPluginMetaInformation:
	case WupsBackendExportId::GetPluginMetaInformationByPath:
	case WupsBackendExportId::GetPluginMetaInformationByBuffer:
	case WupsBackendExportId::GetPluginMetaInformationByPathEx:
	case WupsBackendExportId::GetPluginMetaInformationByBufferEx:
	{
		WupsBackendInputType inputType{};
		std::uint32_t path{}, buffer{}, size{}, output{}, parseOutput{};
		if (export_.id == WupsBackendExportId::GetPluginMetaInformation)
		{
			if (!require(5) || arguments[0] > 1)
				return result(WupsBackendApiError::InvalidArgument);
			inputType = static_cast<WupsBackendInputType>(arguments[0]);
			path = arguments[1];
			buffer = arguments[2];
			size = arguments[3];
			output = arguments[4];
		}
		else if (export_.id == WupsBackendExportId::GetPluginMetaInformationByPath ||
				 export_.id == WupsBackendExportId::GetPluginMetaInformationByPathEx)
		{
			if (!require(export_.id == WupsBackendExportId::GetPluginMetaInformationByPathEx ? 3 : 2))
				return result(WupsBackendApiError::InvalidArgument);
			inputType = WupsBackendInputType::Path;
			output = arguments[0];
			path = arguments[1];
			if (export_.id == WupsBackendExportId::GetPluginMetaInformationByPathEx)
				parseOutput = arguments[2];
		}
		else
		{
			const bool extended = export_.id == WupsBackendExportId::GetPluginMetaInformationByBufferEx;
			if (!require(extended ? 4 : 3))
				return result(WupsBackendApiError::InvalidArgument);
			inputType = WupsBackendInputType::Buffer;
			output = arguments[0];
			buffer = arguments[1];
			size = arguments[2];
			if (extended)
				parseOutput = arguments[3];
		}
		if (output == 0)
			return result(WupsBackendApiError::InvalidArgument);
		CemodPackage package;
		WupsBackendParseError parseError;
		const auto status = inspectInput(inputType, path, buffer, size,
										 package, parseError);
		if (parseOutput != 0 && !WriteGuestU32(*owner, parseOutput,
											   static_cast<std::uint32_t>(static_cast<std::int32_t>(parseError)), error))
			return result(WupsBackendApiError::InvalidArgument);
		if (status != WupsBackendApiError::None)
			return result(status);
		return result(writeInformation(output, package) ? WupsBackendApiError::None : WupsBackendApiError::InvalidArgument);
	}
	case WupsBackendExportId::GetMetaInformation:
	{
		if (!require(3) || arguments[0] == 0 || arguments[1] == 0 ||
			arguments[2] == 0 || arguments[2] > WupsBackendManagementRuntime::kMaximumDataHandles)
			return result(WupsBackendApiError::InvalidArgument);
		std::vector<std::uint32_t> handles;
		if (!readHandles(arguments[0], arguments[2], handles))
			return result(WupsBackendApiError::InvalidArgument);
		const std::uint64_t total = static_cast<std::uint64_t>(arguments[2]) *
									kWupsBackendPluginInformationSize;
		if (total > std::numeric_limits<std::uint32_t>::max())
			return result(WupsBackendApiError::InvalidSize);
		std::vector<std::byte> zeros(static_cast<std::size_t>(total));
		if (!WriteGuest(*owner, arguments[1], zeros, error))
			return result(WupsBackendApiError::InvalidArgument);
		for (std::size_t index = 0; index < handles.size(); ++index)
		{
			const auto container = options.backendManagement->FindContainer(handles[index]);
			if (!container)
				continue;
			if (!writeInformation(arguments[1] + static_cast<std::uint32_t>(index) *
													 kWupsBackendPluginInformationSize,
								  *container->package))
				return result(WupsBackendApiError::InvalidArgument);
		}
		return result(WupsBackendApiError::None);
	}
	case WupsBackendExportId::GetPluginDataForContainerHandles:
	{
		if (!require(3) || arguments[0] == 0 || arguments[1] == 0 ||
			arguments[2] == 0)
			return result(WupsBackendApiError::InvalidArgument);
		std::vector<std::uint32_t> containers;
		if (!readHandles(arguments[0], arguments[2], containers))
			return result(WupsBackendApiError::InvalidArgument);
		for (std::size_t index = 0; index < containers.size(); ++index)
		{
			const auto container = options.backendManagement->FindContainer(containers[index]);
			if (!container)
				return result(WupsBackendApiError::InvalidHandle);
			const auto data = options.backendManagement->CreatePluginData(*container->package);
			if (!data)
				return result(WupsBackendApiError::FailedAllocation);
			if (!WriteGuestU32(*owner, arguments[1] + static_cast<std::uint32_t>(index * 4),
							   *data, error))
				return result(WupsBackendApiError::InvalidArgument);
		}
		return result(WupsBackendApiError::None);
	}
	case WupsBackendExportId::GetSectionInformationForPlugin:
	{
		if (!require(4))
			return result(WupsBackendApiError::InvalidArgument);
		if (arguments[3] != 0 && !WriteGuestU32(*owner, arguments[3], 0, error))
			return result(WupsBackendApiError::InvalidArgument);
		if (!owner->permissions.nativeMemory)
			return result(WupsBackendApiError::UnsupportedCommand);
		if (arguments[0] == 0 || arguments[1] == 0 || arguments[2] == 0)
			return result(WupsBackendApiError::InvalidArgument);
		const auto container = options.backendManagement->FindContainer(arguments[0]);
		if (!container)
			return result(WupsBackendApiError::InvalidHandle);
		WupsMappedLayout layout;
		if (!container->runtime->QueryMappedLayout(layout, error))
			return result(WupsBackendApiError::InvalidHandle);
		const auto count = std::min<std::size_t>(layout.sections.size(), arguments[2]);
		for (std::size_t index = 0; index < count; ++index)
		{
			GuestWupsPluginSectionInfoV1 wire{};
			putU32(wire.informationVersion, kWupsBackendSectionInformationVersion);
			copyString(wire.name, layout.sections[index].name);
			putU32(wire.address, layout.sections[index].address);
			putU32(wire.size, layout.sections[index].size);
			if (!WriteGuest(*owner, arguments[1] + static_cast<std::uint32_t>(index) * kWupsBackendSectionInformationSize,
							std::as_bytes(std::span{&wire, 1}), error))
				return result(WupsBackendApiError::InvalidArgument);
		}
		if (arguments[3] != 0 && !WriteGuestU32(*owner, arguments[3],
												static_cast<std::uint32_t>(count), error))
			return result(WupsBackendApiError::InvalidArgument);
		return result(WupsBackendApiError::None);
	}
	case WupsBackendExportId::GetSectionMemoryAddresses:
	{
		if (!require(3) || arguments[0] == 0 || arguments[1] == 0 || arguments[2] == 0)
			return result(WupsBackendApiError::InvalidArgument);
		if (!owner->permissions.nativeMemory)
			return result(WupsBackendApiError::UnsupportedCommand);
		const auto container = options.backendManagement->FindContainer(arguments[0]);
		if (!container)
			return result(WupsBackendApiError::InvalidHandle);
		WupsMappedLayout layout;
		if (!container->runtime->QueryMappedLayout(layout, error))
			return result(WupsBackendApiError::InvalidHandle);
		if (!WriteGuestU32(*owner, arguments[1], layout.textBase, error) ||
			!WriteGuestU32(*owner, arguments[2], layout.dataBase, error))
			return result(WupsBackendApiError::InvalidArgument);
		return result(WupsBackendApiError::None);
	}
	}
	return result(WupsBackendApiError::UnsupportedCommand);
}

std::int32_t AromaCompatibilityRuntime::Impl::Dispatch(
	const std::shared_ptr<Owner>& owner, std::string_view moduleName,
	std::string_view symbolName, std::span<const std::uint32_t> arguments,
	std::string& error)
{
	auto require = [&](std::size_t count) {
		if (arguments.size() >= count)
			return true;
		error = fmt::format("{}.{} requires {} ABI words, received {}",
							moduleName, symbolName, count, arguments.size());
		return false;
	};
	auto readString = [&](std::uint32_t address, std::size_t maximum,
						  std::string& output) {
		return ReadGuestString(*owner, address, maximum, output, error);
	};
	const auto token = owner->token;
	if (moduleName == "homebrew_wupsbackend")
		if (const auto* export_ = FindWupsBackendExport(symbolName))
			return DispatchBackend(owner, *export_, arguments, error);

	if (moduleName == "__cemu_wups_hook")
	{
		std::uint32_t root{};
		{
			std::lock_guard lock(owner->storage->mutex);
			root = owner->storage->root;
		}
		if (symbolName.starts_with("Storage"))
		{
			if (!require(1) || arguments[0] != root)
				return StorageAbiResult(WupsServiceStatus::InvalidArgument);
			if (symbolName == "StorageSave")
			{
				if (!require(2))
					return StorageAbiResult(WupsServiceStatus::InvalidArgument);
				return StorageAbiResult(SaveStorage(
					*owner, arguments[1] != 0, error));
			}
			if (symbolName == "StorageReload")
			{
				if (!require(1))
					return StorageAbiResult(WupsServiceStatus::InvalidArgument);
				{
					std::lock_guard loadLock(owner->storageLoadMutex);
					std::lock_guard lock(owner->storage->mutex);
					owner->storage->nodes.clear();
					owner->storage->root = 0;
					owner->storage->loaded = false;
					owner->storage->dirty = false;
					owner->storage->revision = 0;
				}
				return StorageAbiResult(EnsureStorageLoaded(*owner, error));
			}
			if (symbolName == "StorageWipe")
			{
				std::lock_guard lock(owner->storage->mutex);
				owner->storage->nodes.clear();
				owner->storage->nodes.emplace(root,
											  StorageNode{root, 0, {}, true});
				owner->storage->dirty = true;
				++owner->storage->revision;
				return 0;
			}
			std::string key;
			if (!require(symbolName == "StorageDelete" ? 3 : 4) ||
				!readString(arguments[2], kMaximumStorageKey + 1, key))
				return StorageAbiResult(WupsServiceStatus::InvalidArgument);
			const auto parent = arguments[1];
			if (symbolName == "StorageDelete")
			{
				// Keep the root argument in the ABI but route through the public
				// owner-checked operation.
				AromaCompatibilityRuntime runtimeShim;
				(void)runtimeShim;
				const auto storage = owner->storage;
				std::lock_guard lock(storage->mutex);
				const auto actualParent = parent == 0 ? storage->root : parent;
				const auto parentFound = storage->nodes.find(actualParent);
				if (parentFound == storage->nodes.end() ||
					!parentFound->second.container)
					return StorageAbiResult(WupsServiceStatus::NotFound);
				const auto child = parentFound->second.children.find(key);
				if (child == parentFound->second.children.end())
					return StorageAbiResult(WupsServiceStatus::NotFound);
				std::vector<std::uint32_t> pending{child->second};
				while (!pending.empty())
				{
					const auto current = pending.back();
					pending.pop_back();
					const auto node = storage->nodes.find(current);
					if (node == storage->nodes.end())
						continue;
					for (const auto& [childKey, childHandle] :
						 node->second.children)
						pending.push_back(childHandle);
					storage->nodes.erase(node);
				}
				parentFound->second.children.erase(child);
				storage->dirty = true;
				++storage->revision;
				return 0;
			}
			if (symbolName == "StorageCreate" ||
				symbolName == "StorageGetSub")
			{
				if (arguments[3] == 0)
					return StorageAbiResult(WupsServiceStatus::InvalidArgument);
				std::uint32_t handle{};
				const auto storage = owner->storage;
				WupsServiceStatus status{WupsServiceStatus::NotFound};
				{
					std::lock_guard lock(storage->mutex);
					const auto actualParent =
						parent == 0 ? storage->root : parent;
					const auto parentFound = storage->nodes.find(actualParent);
					if (parentFound != storage->nodes.end() &&
						parentFound->second.container)
					{
						const auto child = parentFound->second.children.find(key);
						if (symbolName == "StorageGetSub")
						{
							if (child != parentFound->second.children.end())
							{
								const auto node =
									storage->nodes.find(child->second);
								if (node != storage->nodes.end() &&
									node->second.container)
								{
									handle = node->first;
									status = WupsServiceStatus::Success;
								}
							}
						}
						else if (child != parentFound->second.children.end())
							status = WupsServiceStatus::AlreadyExists;
						else if (storage->nodes.size() - 1 >=
								 options.maximumStorageItems)
							status = WupsServiceStatus::LimitExceeded;
						else
						{
							handle = NewHandle();
							parentFound->second.children.emplace(key, handle);
							storage->nodes.emplace(handle, StorageNode{
															   handle, actualParent, key, true});
							storage->dirty = true;
							++storage->revision;
							status = WupsServiceStatus::Success;
						}
					}
				}
				if (status == WupsServiceStatus::Success &&
					!WriteGuestU32(*owner, arguments[3], handle, error))
					status = WupsServiceStatus::InvalidArgument;
				return StorageAbiResult(status);
			}
			if (symbolName == "StorageStore")
			{
				if (!require(6) || arguments[3] > static_cast<std::uint32_t>(WupsStorageValueType::Double) ||
					arguments[5] > kMaximumStorageValue ||
					(arguments[5] != 0 && arguments[4] == 0))
					return StorageAbiResult(WupsServiceStatus::InvalidArgument);
				WupsStorageValue value;
				value.type = static_cast<WupsStorageValueType>(arguments[3]);
				value.bytes.resize(arguments[5]);
				if (!value.bytes.empty() &&
					!ReadGuest(*owner, arguments[4], value.bytes,
							   WupsGuestAccess::Read, error))
					return StorageAbiResult(WupsServiceStatus::InvalidArgument);
				if (!ValidateStorageValue(value))
					return StorageAbiResult(WupsServiceStatus::InvalidArgument);
				const auto storage = owner->storage;
				std::lock_guard lock(storage->mutex);
				const auto actualParent =
					parent == 0 ? storage->root : parent;
				const auto parentFound = storage->nodes.find(actualParent);
				if (parentFound == storage->nodes.end() ||
					!parentFound->second.container)
					return StorageAbiResult(WupsServiceStatus::NotFound);
				const auto child = parentFound->second.children.find(key);
				if (child != parentFound->second.children.end())
				{
					auto node = storage->nodes.find(child->second);
					if (node == storage->nodes.end())
						return StorageAbiResult(
							WupsServiceStatus::InternalError);
					if (node->second.container)
						return StorageAbiResult(
							WupsServiceStatus::AlreadyExists);
					node->second.value = std::move(value);
				}
				else
				{
					if (storage->nodes.size() - 1 >=
						options.maximumStorageItems)
						return StorageAbiResult(
							WupsServiceStatus::LimitExceeded);
					const auto handle = NewHandle();
					parentFound->second.children.emplace(key, handle);
					storage->nodes.emplace(handle, StorageNode{
													   handle, actualParent, key, false, {}, std::move(value)});
				}
				storage->dirty = true;
				++storage->revision;
				return 0;
			}
			if (symbolName == "StorageGet" ||
				symbolName == "StorageGetSize")
			{
				const auto needed = symbolName == "StorageGet" ? 8U : 5U;
				if (!require(needed) ||
					arguments[3] > static_cast<std::uint32_t>(
									   WupsStorageValueType::Double))
					return StorageAbiResult(WupsServiceStatus::InvalidArgument);
				WupsStorageValue value;
				{
					const auto storage = owner->storage;
					std::lock_guard lock(storage->mutex);
					const auto actualParent =
						parent == 0 ? storage->root : parent;
					const auto parentFound = storage->nodes.find(actualParent);
					if (parentFound == storage->nodes.end() ||
						!parentFound->second.container)
						return StorageAbiResult(WupsServiceStatus::NotFound);
					const auto child = parentFound->second.children.find(key);
					if (child == parentFound->second.children.end())
						return StorageAbiResult(WupsServiceStatus::NotFound);
					const auto node = storage->nodes.find(child->second);
					if (node == storage->nodes.end() ||
						node->second.container)
						return StorageAbiResult(WupsServiceStatus::NotFound);
					if (node->second.value.type !=
						static_cast<WupsStorageValueType>(arguments[3]))
						return -0x03;
					value = node->second.value;
				}
				const auto outSize =
					symbolName == "StorageGet" ? arguments[7] : arguments[4];
				if (!WriteGuestU32(*owner, outSize,
								   static_cast<std::uint32_t>(value.bytes.size()), error))
					return StorageAbiResult(
						WupsServiceStatus::InvalidArgument);
				if (symbolName == "StorageGetSize")
					return 0;
				if (arguments[5] < value.bytes.size())
					return StorageAbiResult(
						WupsServiceStatus::BufferTooSmall);
				if (!value.bytes.empty() &&
					!WriteGuest(*owner, arguments[4], value.bytes, error))
					return StorageAbiResult(
						WupsServiceStatus::InvalidArgument);
				return 0;
			}
			error = fmt::format("unsupported internal storage command '{}'",
								symbolName);
			return StorageAbiResult(WupsServiceStatus::Unsupported);
		}

		if (symbolName == "ReentGet")
		{
			// arguments[0] is an opaque, plugin-chosen reentrancy key (in
			// practice the guest address of one of the plugin's own static
			// variables), not this owner's WUPS pluginIdentifier handle - it
			// only needs to be non-zero and is scoped per {thread, key} below,
			// which already prevents cross-owner collisions.
			if (!require(2) || arguments[0] == 0 ||
				arguments[1] == 0 || !options.platform)
				return 0;
			const auto thread = options.platform->CurrentGuestThreadId();
			std::lock_guard lock(owner->mutex);
			const auto found =
				owner->reent.find({thread, arguments[0]});
			// "No context registered for this thread yet" is a successful query
			// that yields a null context, not a backend failure: libwups only
			// allocates and registers a fresh _reent when this returns true with
			// a null out-pointer. Returning false makes it fall back to
			// _GLOBAL_REENT forever, which then has no devoptab device data.
			if (found == owner->reent.end())
				return WriteGuestU32(*owner, arguments[1], 0, error) ? 1 : 0;
			if (!WriteGuestU32(*owner, arguments[1],
							   found->second.context, error))
				return 0;
			return 1;
		}
		if (symbolName == "ReentAdd")
		{
			// See ReentGet above: arguments[0] is the plugin's own opaque
			// reentrancy key, not owner->pluginIdentifier.
			if (!require(3) || arguments[0] == 0 ||
				arguments[1] == 0 || !options.platform ||
				!ValidateGuestCallback(*owner, arguments[2], error))
				return 0;
			const auto thread = options.platform->CurrentGuestThreadId();
			std::lock_guard lock(owner->mutex);
			const auto [iterator, added] = owner->reent.emplace(
				std::pair{thread, arguments[0]}, ReentState{
													 thread, arguments[0], arguments[1], arguments[2]});
			return added ? 1 : 0;
		}

		if (symbolName.starts_with("Button"))
		{
			if (!require(1) || arguments[0] != owner->pluginIdentifier)
				return ComboAbiResult(WupsServiceStatus::InvalidArgument);
			if (symbolName == "ButtonAdd" || symbolName == "ButtonCheck")
			{
				if (!require(symbolName == "ButtonAdd" ? 4 : 3))
					return ComboAbiResult(WupsServiceStatus::InvalidArgument);
				std::array<std::byte, 28> raw{};
				if (!ReadGuest(*owner, arguments[1], raw,
							   WupsGuestAccess::Read, error))
					return ComboAbiResult(WupsServiceStatus::InvalidArgument);
				WupsButtonComboDefinition definition;
				if (!ReadGuestString(*owner, ReadU32(raw, 0),
									 kMaximumConfigName, definition.label, error))
					return ComboAbiResult(WupsServiceStatus::InvalidArgument);
				definition.callback = ReadU32(raw, 4);
				definition.context = ReadU32(raw, 8);
				definition.type =
					static_cast<WupsButtonComboType>(ReadU32(raw, 12));
				definition.controllerMask = ReadU32(raw, 16);
				definition.buttons = ReadU32(raw, 20);
				definition.holdDurationMilliseconds = ReadU32(raw, 24);
				if (!ValidComboDefinition(definition) ||
					!ValidateGuestCallback(
						*owner, definition.callback, error))
					return ComboAbiResult(WupsServiceStatus::InvalidArgument);
				WupsButtonComboStatus comboStatus;
				std::uint32_t handle{};
				{
					std::lock_guard lock(owner->mutex);
					comboStatus = ComboAvailability(
						owner->combos, definition);
					if (symbolName == "ButtonAdd")
					{
						if (owner->combos.size() >= 128)
							return ComboAbiResult(
								WupsServiceStatus::LimitExceeded);
						handle = NewHandle();
						owner->combos.emplace(handle, ComboState{
														  handle, definition, comboStatus});
					}
				}
				const auto outStatus =
					arguments[symbolName == "ButtonAdd" ? 3 : 2];
				if (!WriteGuestU32(*owner, outStatus,
								   static_cast<std::uint32_t>(comboStatus), error) ||
					(symbolName == "ButtonAdd" &&
					 !WriteGuestU32(*owner, arguments[2], handle, error)))
					return ComboAbiResult(WupsServiceStatus::InvalidArgument);
				return 0;
			}
			if (symbolName == "ButtonRemove")
			{
				if (!require(2))
					return ComboAbiResult(WupsServiceStatus::InvalidArgument);
				std::lock_guard lock(owner->mutex);
				return ComboAbiResult(owner->combos.erase(arguments[1]) ? WupsServiceStatus::Success : WupsServiceStatus::NotFound);
			}
			if (symbolName == "ButtonGetStatus")
			{
				if (!require(3))
					return ComboAbiResult(WupsServiceStatus::InvalidArgument);
				WupsButtonComboStatus status;
				{
					std::lock_guard lock(owner->mutex);
					const auto found = owner->combos.find(arguments[1]);
					if (found == owner->combos.end())
						return ComboAbiResult(WupsServiceStatus::NotFound);
					status = found->second.status;
				}
				return WriteGuestU32(*owner, arguments[2],
									 static_cast<std::uint32_t>(status), error)
						   ? 0
						   : ComboAbiResult(WupsServiceStatus::InvalidArgument);
			}
			error = fmt::format(
				"button combo command '{}' is explicitly unsupported",
				symbolName);
			return ComboAbiResult(WupsServiceStatus::Unsupported);
		}
		error = fmt::format("unknown internal WUPS hook export '{}'",
							symbolName);
		return -0x1000;
	}

	if (moduleName == "homebrew_wupsbackend")
	{
		if (symbolName == "WUPSConfigAPI_GetVersion")
		{
			if (!require(1) ||
				!WriteGuestU32(*owner, arguments[0], 2, error))
				return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
			return 0;
		}
		if (symbolName == "WUPSConfigAPI_InitEx")
		{
			if (!require(5) || arguments[0] != owner->pluginIdentifier ||
				arguments[1] != 1)
				return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
			std::string name;
			if (!readString(arguments[2], kMaximumConfigName, name) ||
				!ValidateGuestCallback(*owner, arguments[3], error) ||
				!ValidateGuestCallback(*owner, arguments[4], error))
				return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
			std::lock_guard lock(owner->config.mutex);
			if (owner->config.registered)
				return ConfigAbiResult(WupsServiceStatus::AlreadyExists);
			owner->config.name = std::move(name);
			owner->config.openCallback = arguments[3];
			owner->config.closeCallback = arguments[4];
			owner->config.registered = true;
			return 0;
		}
		if (symbolName == "WUPSConfigAPI_Category_CreateEx")
		{
			if (!require(3) || arguments[0] != 1)
				return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
			std::string name;
			if (!readString(arguments[1], kMaximumConfigName, name))
				return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
			std::uint32_t handle;
			{
				std::lock_guard lock(owner->config.mutex);
				if (owner->config.categories.size() >= 256)
					return ConfigAbiResult(
						WupsServiceStatus::LimitExceeded);
				handle = NewHandle();
				owner->config.categories.emplace(handle,
												 ConfigCategory{handle, std::move(name)});
			}
			return WriteGuestU32(*owner, arguments[2], handle, error) ? 0 : ConfigAbiResult(WupsServiceStatus::InvalidArgument);
		}
		if (symbolName == "WUPSConfigAPI_Category_Destroy" ||
			symbolName == "WUPSConfigAPI_Item_Destroy")
		{
			if (!require(1) || arguments[0] == 0)
				return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
			if (symbolName == "WUPSConfigAPI_Category_Destroy")
			{
				std::lock_guard lock(owner->config.mutex);
				const auto found =
					owner->config.categories.find(arguments[0]);
				if (found == owner->config.categories.end())
					return ConfigAbiResult(WupsServiceStatus::NotFound);
				if (found->second.attached ||
					!found->second.categories.empty() ||
					!found->second.items.empty())
					return ConfigAbiResult(WupsServiceStatus::Busy);
				owner->config.categories.erase(found);
				return 0;
			}
			std::uint32_t callback{};
			std::uint32_t context{};
			{
				std::lock_guard lock(owner->config.mutex);
				const auto found = owner->config.items.find(arguments[0]);
				if (found == owner->config.items.end())
					return ConfigAbiResult(WupsServiceStatus::NotFound);
				if (found->second.attached)
					return ConfigAbiResult(WupsServiceStatus::Busy);
				callback = found->second.model.callbacks.destroy;
				context = found->second.model.callbacks.context;
				owner->config.items.erase(found);
			}
			if (callback)
				(void)QueueCallback(token, callback, {context}, error);
			return 0;
		}
		if (symbolName == "WUPSConfigAPI_Category_AddCategory" ||
			symbolName == "WUPSConfigAPI_Category_AddItem")
		{
			if (!require(2))
				return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
			std::lock_guard lock(owner->config.mutex);
			auto parent = owner->config.categories.find(arguments[0]);
			if (parent == owner->config.categories.end())
				return ConfigAbiResult(WupsServiceStatus::NotFound);
			if (symbolName == "WUPSConfigAPI_Category_AddCategory")
			{
				auto child = owner->config.categories.find(arguments[1]);
				if (child == owner->config.categories.end())
					return ConfigAbiResult(WupsServiceStatus::NotFound);
				if (arguments[0] == arguments[1] || child->second.attached)
					return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
				parent->second.categories.push_back(arguments[1]);
				child->second.attached = true;
			}
			else
			{
				auto child = owner->config.items.find(arguments[1]);
				if (child == owner->config.items.end())
					return ConfigAbiResult(WupsServiceStatus::NotFound);
				if (child->second.attached)
					return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
				parent->second.items.push_back(arguments[1]);
				child->second.attached = true;
			}
			return 0;
		}
		if (symbolName == "WUPSConfigAPI_Item_CreateEx")
		{
			if (!require(13) ||
				(arguments[0] != 1 && arguments[0] != 2))
				return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
			const bool v2 = arguments[0] == 2;
			const auto displayIndex = v2 ? 1U : 2U;
			const auto contextIndex = v2 ? 2U : 3U;
			const auto callbacksIndex = v2 ? 3U : 4U;
			std::string display;
			if (!readString(arguments[displayIndex],
							kMaximumConfigName, display))
				return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
			WupsConfigItemModel item;
			item.displayName = std::move(display);
			item.kind = WupsConfigItemKind::Custom;
			item.callbacks.context = arguments[contextIndex];
			item.callbacks.valueDisplay = arguments[callbacksIndex];
			item.callbacks.selectedValueDisplay =
				arguments[callbacksIndex + 1];
			item.callbacks.selected = arguments[callbacksIndex + 2];
			item.callbacks.restoreDefault = arguments[callbacksIndex + 3];
			item.callbacks.movementAllowed = arguments[callbacksIndex + 4];
			item.callbacks.close = arguments[callbacksIndex + 5];
			item.callbacks.input = arguments[callbacksIndex + 6];
			item.callbacks.inputEx =
				v2 ? arguments[callbacksIndex + 7] : 0;
			item.callbacks.destroy =
				arguments[callbacksIndex + (v2 ? 8 : 7)];
			const std::array callbacks{
				item.callbacks.valueDisplay,
				item.callbacks.selectedValueDisplay,
				item.callbacks.selected,
				item.callbacks.restoreDefault,
				item.callbacks.movementAllowed,
				item.callbacks.close,
				item.callbacks.input,
				item.callbacks.inputEx,
				item.callbacks.destroy};
			for (const auto callback : callbacks)
				if (!ValidateGuestCallback(*owner, callback, error))
					return ConfigAbiResult(
						WupsServiceStatus::InvalidArgument);
			std::uint32_t handle;
			{
				std::lock_guard lock(owner->config.mutex);
				if (owner->config.items.size() >= 512)
					return ConfigAbiResult(
						WupsServiceStatus::LimitExceeded);
				handle = NewHandle();
				item.handle = handle;
				owner->config.items.emplace(
					handle, ConfigItem{std::move(item)});
			}
			return WriteGuestU32(*owner, arguments[12], handle, error) ? 0 : ConfigAbiResult(WupsServiceStatus::InvalidArgument);
		}
		if (symbolName == "WUPSConfigAPI_Menu_GetStatus")
		{
			if (!require(1))
				return ConfigAbiResult(WupsServiceStatus::InvalidArgument);
			bool open;
			{
				std::lock_guard lock(owner->config.mutex);
				open = owner->config.menuOpen;
			}
			return WriteGuestU32(*owner, arguments[0], open ? 1 : 0, error) ? 0 : ConfigAbiResult(WupsServiceStatus::InvalidArgument);
		}
		error = fmt::format("unsupported WUPS config export '{}'", symbolName);
		return ConfigAbiResult(WupsServiceStatus::Unsupported);
	}

	if (moduleName == "__cemu_wups_data")
	{
		if (symbolName == "MEMAllocFromDefaultHeap" ||
			symbolName == "MEMAllocFromDefaultHeapEx")
		{
			// Serviced entirely from the platform's isolated WUPS plugin heap
			// (see WupsPluginHeap.h): this never touches the game's own
			// default heap or re-enters guest code.
			const bool ex = symbolName == "MEMAllocFromDefaultHeapEx";
			if (!require(ex ? 2 : 1) || !options.platform || arguments[0] == 0)
				return 0;
			// MEMAllocFromDefaultHeapEx's alignment is a signed word (a
			// negative value requests a tail allocation); the plain variant
			// matches the real default heap's implicit 0x40 alignment.
			const auto alignment = ex ? static_cast<std::int32_t>(arguments[1]) : std::int32_t{0x40};
			const auto allocated = options.platform->AllocatePluginHeapMemory(
				token, arguments[0], alignment);
			return static_cast<std::int32_t>(allocated);
		}
		if (symbolName == "MEMFreeToDefaultHeap")
		{
			if (!require(1))
				return 0;
			if (options.platform && arguments[0] != 0)
				options.platform->FreePluginHeapMemory(token, arguments[0]);
			return 0;
		}
		if (symbolName == "MEMFreeToMappedMemory")
		{
			if (!require(1))
				return 0;
			(void)FreeMapping(owner, token, arguments[0], error);
			return 0;
		}
		const bool gx2 =
			symbolName == "MEMAllocFromMappedMemoryForGX2Ex";
		if (!require(symbolName == "MEMAllocFromMappedMemory" ? 1 : 2))
			return 0;
		const auto alignment =
			symbolName == "MEMAllocFromMappedMemory" ? 4U : arguments[1];
		if (!options.platform || arguments[0] == 0 ||
			!IsPowerOfTwo(alignment) || alignment < 4 ||
			alignment > 1024U * 1024U ||
			static_cast<std::uint64_t>(arguments[0]) + alignment - 1 >
				std::numeric_limits<std::uint32_t>::max())
			return 0;
		WupsMappedMemoryInfo allocation;
		if (AllocateMapping(owner, token, arguments[0], alignment, true,
							gx2 ? WupsMappedMemoryPurpose::Gx2 : WupsMappedMemoryPurpose::Cpu, allocation, error) !=
			WupsServiceStatus::Success)
			return 0;
		return static_cast<std::int32_t>(allocation.address);
	}

	if (moduleName == "homebrew_memorymapping")
	{
		if (!require(1))
			return 0;
		std::lock_guard lock(owner->mutex);
		for (const auto& [address, mapping] : owner->mappings)
		{
			if (symbolName == "MemoryMappingEffectiveToPhysical" &&
				arguments[0] >= address &&
				static_cast<std::uint64_t>(arguments[0]) <
					static_cast<std::uint64_t>(address) + mapping.size)
				return static_cast<std::int32_t>(
					mapping.physicalAddress + (arguments[0] - address));
			if (symbolName == "MemoryMappingPhysicalToEffective" &&
				arguments[0] >= mapping.physicalAddress &&
				static_cast<std::uint64_t>(arguments[0]) <
					static_cast<std::uint64_t>(
						mapping.physicalAddress) +
						mapping.size)
				return static_cast<std::int32_t>(
					address + (arguments[0] - mapping.physicalAddress));
		}
		return 0;
	}

	if (moduleName == "homebrew_logging")
	{
		if (!require(2) || arguments[1] == 0 ||
			arguments[1] > kMaximumLogMessage)
			return 0;
		std::vector<std::byte> bytes(arguments[1]);
		if (!ReadGuest(*owner, arguments[0], bytes,
					   WupsGuestAccess::Read, error) ||
			std::ranges::any_of(bytes, [](std::byte byte) {
				return byte == std::byte{};
			}))
			return 0;
		std::string message(reinterpret_cast<const char*>(bytes.data()),
							bytes.size());
		const auto now = std::chrono::steady_clock::now();
		{
			std::lock_guard lock(owner->mutex);
			while (!owner->recentLogs.empty() &&
				   now - owner->recentLogs.front() >=
					   std::chrono::seconds(1))
				owner->recentLogs.pop_front();
			if (owner->recentLogs.size() >= 100)
				return 0;
			owner->recentLogs.push_back(now);
		}
		options.platform->Log(token, WupsLogLevel::Info,
							  moduleName, "WUMSLogWrite", message);
		return 0;
	}

	if (moduleName == "homebrew_notifications")
	{
		if (symbolName == "NMGetVersion")
			return 2;
		if (symbolName == "NMIsOverlayReady")
			return 0;
		error = fmt::format(
			"notification ABI command '{}' is not implemented by the Cemu "
			"overlay adapter",
			symbolName);
		return NotificationAbiResult(WupsServiceStatus::Unsupported);
	}

	if (moduleName == "homebrew_content_redirection")
	{
		if (symbolName == "CRGetVersion")
			return 2;
		error = fmt::format(
			"content-redirection guest ABI command '{}' requires the Task 3 "
			"filesystem registry adapter",
			symbolName);
		return ContentAbiResult(WupsServiceStatus::Unsupported);
	}

	if (moduleName == "homebrew_functionpatcher")
	{
		if (symbolName == "FPGetVersion")
		{
			if (!require(1) || !WriteGuestU32(*owner, arguments[0],
											  options.functionPatcher->ApiVersion(), error))
				return FunctionPatcherAbiResult(
					WupsServiceStatus::InvalidArgument);
			return 0;
		}
		if (symbolName == "FPAddFunctionPatch")
		{
			if (!require(3))
				return FunctionPatcherAbiResult(
					WupsServiceStatus::InvalidArgument);
			if ((arguments[1] != 0 &&
				 !ValidateGuestOutput(*owner, arguments[1], 4, 4, error)) ||
				(arguments[2] != 0 &&
				 !ValidateGuestOutput(*owner, arguments[2], 1, 1, error)))
				return FunctionPatcherAbiResult(
					WupsServiceStatus::InvalidArgument);
			std::uint32_t handle{};
			bool applied{};
			const auto status = options.functionPatcher->AddPatch(
				token, arguments[0],
				owner->permissions.physicalAddressPatching,
				handle, applied, error);
			if (status == WupsServiceStatus::Success)
			{
				const bool outputsWritten =
					(arguments[1] == 0 ||
					 WriteGuestU32(*owner, arguments[1], handle, error)) &&
					(arguments[2] == 0 ||
					 WriteGuestBool(*owner, arguments[2], applied, error));
				if (!outputsWritten)
				{
					const auto outputError = error;
					std::string rollbackError;
					const auto rollbackStatus = options.functionPatcher->RemovePatch(
						token, handle, rollbackError);
					error = outputError;
					if (rollbackStatus != WupsServiceStatus::Success)
						error.append("; patch rollback failed: ").append(rollbackError.empty() ? "unknown error" : rollbackError);
					return FunctionPatcherAbiResult(
						WupsServiceStatus::InvalidArgument);
				}
			}
			return FunctionPatcherAbiResult(status);
		}
		if (symbolName == "FPRemoveFunctionPatch")
		{
			if (!require(1))
				return FunctionPatcherAbiResult(
					WupsServiceStatus::InvalidArgument);
			return FunctionPatcherAbiResult(
				options.functionPatcher->RemovePatch(
					token, arguments[0], error));
		}
		if (symbolName == "FPIsFunctionPatched")
		{
			if (!require(2))
				return FunctionPatcherAbiResult(
					WupsServiceStatus::InvalidArgument);
			bool applied{};
			const auto status = options.functionPatcher->IsPatchApplied(
				token, arguments[0], applied, error);
			if (status == WupsServiceStatus::Success &&
				!WriteGuestBool(*owner, arguments[1], applied, error))
				return FunctionPatcherAbiResult(
					WupsServiceStatus::InvalidArgument);
			return FunctionPatcherAbiResult(status);
		}
		error = fmt::format(
			"legacy FunctionPatcher export '{}' is explicitly unsupported",
			symbolName);
		return FunctionPatcherAbiResult(WupsServiceStatus::Unsupported);
	}

	error = fmt::format("unhandled WUPS export {}.{}",
						moduleName, symbolName);
	return -0x1000;
}
