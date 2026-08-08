#include "Cafe/OS/libs/cemuextend/Cex2Http.h"

// The test binaries compile this file for its dispatch and validation logic but
// must never reach the network, so the transport half is compiled out for them
// the same way the audit sink is.
#ifndef CEMU_CEX2_TESTING
#include "curl/curl.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace cemuextend_hle {
namespace {

using cemuextend::wire::Status;

constexpr std::uint32_t kMaximumBodyBytes = 8U * 1024U * 1024U;
constexpr std::uint32_t kDefaultTimeoutMs = 15000;
constexpr std::uint32_t kMaximumTimeoutMs = 60000;
constexpr std::uint32_t kMaximumUrlBytes = 2048;
// A guest that never polls must not be able to keep host threads alive for
// free, so a session is capped at a handful of transfers at once.
constexpr std::size_t kMaximumTransfersPerSession = 4;

// One transfer. The worker thread and the dispatch thread both hold a shared
// pointer, so a session torn down mid-flight drops its reference and the worker
// frees the last one when it finishes.
struct Transfer
{
	std::mutex mutex;
	std::vector<std::byte> body;
	std::uint32_t handle{};
	std::uint32_t maximumBodyBytes{kMaximumBodyBytes};
	std::uint64_t session{};
	std::uint16_t httpStatus{};
	Status error{Status::Ok};
	cemuextend::wire::HttpState state{cemuextend::wire::HttpState::Pending};
	std::atomic<bool> abandoned{false};
};

struct Registry
{
	std::mutex mutex;
	std::unordered_map<std::uint32_t, std::shared_ptr<Transfer>> transfers;
	std::uint32_t nextHandle{1};
};

Registry& Transfers()
{
	static Registry registry;
	return registry;
}

#ifndef CEMU_CEX2_TESTING
std::size_t WriteCallback(char* data, std::size_t size, std::size_t count, void* context)
{
	auto* transfer = static_cast<Transfer*>(context);
	if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
		return 0;
	const std::size_t bytes = size * count;
	if (transfer->abandoned.load(std::memory_order_relaxed))
		return 0;
	std::scoped_lock lock(transfer->mutex);
	if (bytes > transfer->maximumBodyBytes - transfer->body.size())
		return 0;
	const auto* begin = reinterpret_cast<const std::byte*>(data);
	transfer->body.insert(transfer->body.end(), begin, begin + bytes);
	return bytes;
}

Status TranslateCurl(CURLcode code)
{
	switch (code)
	{
	case CURLE_OK:
		return Status::Ok;
	case CURLE_OPERATION_TIMEDOUT:
		return Status::TimedOut;
	case CURLE_URL_MALFORMAT:
	case CURLE_UNSUPPORTED_PROTOCOL:
		return Status::InvalidArgument;
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY:
		return Status::NotFound;
	case CURLE_FILESIZE_EXCEEDED:
	case CURLE_WRITE_ERROR:
		return Status::TooLarge;
	default:
		return Status::IoError;
	}
}

void RunTransfer(std::shared_ptr<Transfer> transfer, std::string url, std::uint32_t timeoutMs,
	std::uint32_t maximumBodyBytes)
{
	CURL* curl = curl_easy_init();
	Status error = Status::IoError;
	long httpStatus = 0;
	if (curl)
	{
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutMs));
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
		curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
			static_cast<curl_off_t>(maximumBodyBytes));
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, transfer.get());
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "CemuExtend/2");
		const CURLcode code = curl_easy_perform(curl);
		error = TranslateCurl(code);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
		curl_easy_cleanup(curl);
	}
	std::scoped_lock lock(transfer->mutex);
	transfer->httpStatus = static_cast<std::uint16_t>(std::clamp<long>(httpStatus, 0, 0xffff));
	transfer->error = error;
	transfer->state = error == Status::Ok ? cemuextend::wire::HttpState::Complete
										  : cemuextend::wire::HttpState::Failed;
	if (error != Status::Ok)
		transfer->body.clear();
}
#else
void RunTransfer(std::shared_ptr<Transfer> transfer, std::string, std::uint32_t, std::uint32_t)
{
	std::scoped_lock lock(transfer->mutex);
	transfer->error = Status::NotSupported;
	transfer->state = cemuextend::wire::HttpState::Failed;
}
#endif

Cex2HttpResult Fail(Status status)
{
	return Cex2HttpResult{status, {}};
}

Cex2HttpResult Start(std::uint64_t session, std::span<const std::byte> payload)
{
	using cemuextend::wire::HttpStartRequest;
	using cemuextend::wire::HttpStartResponse;
	if (payload.size() < sizeof(HttpStartRequest))
		return Fail(Status::InvalidArgument);
	HttpStartRequest header{};
	std::memcpy(&header, payload.data(), sizeof(header));
	const std::uint32_t urlBytes = header.urlBytes.get();
	if (payload.size() != sizeof(header) + urlBytes || urlBytes == 0 ||
		urlBytes > kMaximumUrlBytes || header.reserved.get() != 0)
		return Fail(Status::InvalidArgument);
	const std::string url(reinterpret_cast<const char*>(payload.data() + sizeof(header)), urlBytes);
	// Only the two schemes curl is being asked for here. Anything else -- file:,
	// scp:, gopher: -- would turn this into a host filesystem reader.
	if (!url.starts_with("http://") && !url.starts_with("https://"))
		return Fail(Status::InvalidArgument);
	if (url.find_first_of("\r\n") != std::string::npos)
		return Fail(Status::InvalidArgument);

	std::uint32_t timeoutMs = header.timeoutMs.get();
	timeoutMs = timeoutMs == 0 ? kDefaultTimeoutMs : std::min(timeoutMs, kMaximumTimeoutMs);
	std::uint32_t maximumBodyBytes = header.maximumBodyBytes.get();
	maximumBodyBytes = maximumBodyBytes == 0 ? kMaximumBodyBytes
											 : std::min(maximumBodyBytes, kMaximumBodyBytes);

	auto transfer = std::make_shared<Transfer>();
	transfer->maximumBodyBytes = maximumBodyBytes;
	{
		Registry& registry = Transfers();
		std::scoped_lock lock(registry.mutex);
		std::size_t owned = 0;
		for (const auto& [handle, entry] : registry.transfers)
			owned += entry->session == session ? 1 : 0;
		if (owned >= kMaximumTransfersPerSession)
			return Fail(Status::Busy);
		transfer->handle = registry.nextHandle++;
		if (registry.nextHandle == 0)
			registry.nextHandle = 1;
		transfer->session = session;
		registry.transfers.emplace(transfer->handle, transfer);
	}
#ifndef CEMU_CEX2_TESTING
	std::thread(RunTransfer, transfer, url, timeoutMs, maximumBodyBytes).detach();
#else
	RunTransfer(transfer, url, timeoutMs, maximumBodyBytes);
#endif

	HttpStartResponse response{};
	response.handle = transfer->handle;
	Cex2HttpResult result{Status::Ok, {}};
	result.payload.resize(sizeof(response));
	std::memcpy(result.payload.data(), &response, sizeof(response));
	return result;
}

std::shared_ptr<Transfer> Find(std::uint64_t session, std::uint32_t handle)
{
	Registry& registry = Transfers();
	std::scoped_lock lock(registry.mutex);
	const auto found = registry.transfers.find(handle);
	if (found == registry.transfers.end() || found->second->session != session)
		return {};
	return found->second;
}

Cex2HttpResult Poll(std::uint64_t session, std::span<const std::byte> payload)
{
	using cemuextend::wire::HttpPollRequest;
	using cemuextend::wire::HttpPollResponse;
	if (payload.size() != sizeof(HttpPollRequest))
		return Fail(Status::InvalidArgument);
	HttpPollRequest request{};
	std::memcpy(&request, payload.data(), sizeof(request));
	if (request.reserved.get() != 0)
		return Fail(Status::InvalidArgument);
	const auto transfer = Find(session, request.handle.get());
	if (!transfer)
		return Fail(Status::NotFound);

	HttpPollResponse response{};
	std::vector<std::byte> chunk;
	{
		std::scoped_lock lock(transfer->mutex);
		response.handle = transfer->handle;
		response.state = static_cast<std::uint8_t>(transfer->state);
		response.status = transfer->httpStatus;
		response.error = static_cast<std::int32_t>(transfer->error);
		if (transfer->state == cemuextend::wire::HttpState::Complete)
		{
			response.bodyBytes = static_cast<std::uint32_t>(transfer->body.size());
			const std::uint32_t offset =
				std::min<std::uint32_t>(request.offset.get(),
					static_cast<std::uint32_t>(transfer->body.size()));
			const std::uint32_t length = std::min<std::uint32_t>(request.length.get(),
				static_cast<std::uint32_t>(transfer->body.size()) - offset);
			chunk.assign(transfer->body.begin() + offset, transfer->body.begin() + offset + length);
			response.chunkBytes = length;
		}
	}
	Cex2HttpResult result{Status::Ok, {}};
	result.payload.resize(sizeof(response) + chunk.size());
	std::memcpy(result.payload.data(), &response, sizeof(response));
	if (!chunk.empty())
		std::memcpy(result.payload.data() + sizeof(response), chunk.data(), chunk.size());
	return result;
}

Cex2HttpResult Release(std::uint64_t session, std::span<const std::byte> payload)
{
	if (payload.size() != 4)
		return Fail(Status::InvalidArgument);
	cemuextend::wire::Be32 handle{};
	std::memcpy(&handle, payload.data(), sizeof(handle));
	const auto transfer = Find(session, handle.get());
	if (!transfer)
		return Fail(Status::NotFound);
	transfer->abandoned.store(true, std::memory_order_relaxed);
	Registry& registry = Transfers();
	std::scoped_lock lock(registry.mutex);
	registry.transfers.erase(handle.get());
	return Cex2HttpResult{Status::Ok, {}};
}

} // namespace

Cex2HttpResult Cex2Http::Dispatch(std::uint64_t session, std::string_view,
	std::uint16_t operation, std::span<const std::byte> payload)
{
	switch (static_cast<cemuextend::wire::HttpOperation>(operation))
	{
	case cemuextend::wire::HttpOperation::Start:
		return Start(session, payload);
	case cemuextend::wire::HttpOperation::Poll:
		return Poll(session, payload);
	case cemuextend::wire::HttpOperation::Release:
		return Release(session, payload);
	}
	return Fail(Status::NotSupported);
}

void Cex2Http::ReleaseSession(std::uint64_t session)
{
	Registry& registry = Transfers();
	std::scoped_lock lock(registry.mutex);
	for (auto entry = registry.transfers.begin(); entry != registry.transfers.end();)
	{
		if (entry->second->session != session)
		{
			++entry;
			continue;
		}
		entry->second->abandoned.store(true, std::memory_order_relaxed);
		entry = registry.transfers.erase(entry);
	}
}

std::size_t Cex2Http::ActiveTransfers()
{
	Registry& registry = Transfers();
	std::scoped_lock lock(registry.mutex);
	return registry.transfers.size();
}

} // namespace cemuextend_hle
