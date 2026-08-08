#include "Common/precompiled.h"

#include "Cafe/OS/libs/cemuextend/Cex2Host.h"
#include "Cafe/OS/libs/cemuextend/CemodPermission.h"
#include "Cafe/OS/libs/cemuextend/Cex2Owner.h"
#include "Cafe/OS/libs/cemuextend/Cex2Http.h"
#include "Cafe/OS/libs/cemuextend/Cex2Storage.h"

#include "Cafe/HW/Espresso/ModExecutionContext.h"
#ifndef CEMU_CEX2_TESTING
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cemu/Logging/CemuLogging.h"
#include "gui/interface/WindowSystem.h"
#endif
#include "Cafe/OS/libs/cemuextend/BuildId.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "cemuextend/services.hpp"
#include "cemuextend/transport.hpp"

#include <openssl/crypto.h>

#include <deque>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <thread>

namespace cemuextend_hle {
namespace {

std::uint64_t CurrentFrameNumber()
{
#ifdef CEMU_CEX2_TESTING
	return 0;
#else
	return LatteGPUState.frameCounter;
#endif
}

void LogGuestRecord(std::string_view principal, std::uint8_t level, std::string_view message)
{
#ifndef CEMU_CEX2_TESTING
	cemuLog_log(LogType::Force, "[CemuExtend Mod/{}/{}] {}", principal, level, message);
#endif
}

void AuditSensitiveUse(std::string_view principal, std::string_view action,
	bool showNotification = true)
{
#ifndef CEMU_CEX2_TESTING
	const auto message = fmt::format("CemuExtend Mod [{}]: {}", principal, action);
	cemuLog_log(LogType::Force, "AUDIT {}", message);
	if (showNotification)
		LatteOverlay_pushNotification(message, 3000);
#endif
}

using cemuextend::transport::RequestHeader;
using cemuextend::transport::ResponseHeader;
using cemuextend::wire::Error;
using cemuextend::wire::ServiceId;
using cemuextend::wire::Status;

struct ServiceDefinition
{
	std::uint16_t id;
	std::uint16_t version;
	std::uint32_t requiredPermission;
	std::uint32_t maximumRequest;
	std::uint32_t maximumResponse;
};

enum class Handler : std::uint8_t
{
	Core,
	Input,
	Logging,
	Configuration,
	File,
	Clipboard,
	Window,
	Capture,
	Diagnostics,
	Http,
};

struct OperationDefinition
{
	std::uint16_t service;
	std::uint16_t operation;
	std::uint16_t version;
	std::uint32_t permission;
	std::uint32_t maximumRequest;
	std::uint32_t maximumResponse;
	std::uint16_t ratePerSecond;
	std::uint16_t burst;
	Handler handler;
};

constexpr std::array kOperations{
	OperationDefinition{1,1,1,0,0,4096,0,0,Handler::Core},
	OperationDefinition{1,2,1,0,8,8,0,0,Handler::Core},
	OperationDefinition{1,3,1,0,0,32,0,0,Handler::Core},
	OperationDefinition{1,4,1,0,2,0,0,0,Handler::Core},
	OperationDefinition{1,5,1,0,2,0,0,0,Handler::Core},
	OperationDefinition{1,6,1,0,0,128,0,0,Handler::Core},
	OperationDefinition{2,1,1,4,sizeof(cemuextend::wire::ControllerEventPayload),0,0,0,Handler::Input},
	OperationDefinition{2,2,1,4,1+sizeof(cemuextend::wire::ObservedVpadState),0,0,0,Handler::Input},
	OperationDefinition{2,3,1,1,1,sizeof(cemuextend::wire::ObservedVpadState),0,0,Handler::Input},
	OperationDefinition{2,4,1,1,0,sizeof(cemuextend::wire::MouseEventPayloadV2),0,0,Handler::Input},
	OperationDefinition{2,5,1,1,sizeof(cemuextend::wire::TextInputRequestHeader)+4096,0,0,0,Handler::Input},
	OperationDefinition{3,1,1,2,4096+5,0,20,50,Handler::Logging},
	OperationDefinition{4,1,1,1,260,65520,0,0,Handler::Configuration},
	OperationDefinition{4,2,1,2,65520,0,0,0,Handler::Configuration},
	OperationDefinition{4,3,1,2,260,0,0,0,Handler::Configuration},
	OperationDefinition{4,4,1,1,65520,65520,0,0,Handler::Configuration},
	OperationDefinition{5,1,1,1,4096,64,0,0,Handler::File},
	OperationDefinition{5,2,1,1,4096,65520,0,0,Handler::File},
	OperationDefinition{5,3,1,1,4096,65520,0,0,Handler::File},
	OperationDefinition{5,4,1,2,65520,0,0,0,Handler::File},
	OperationDefinition{5,5,1,2,4096,0,0,0,Handler::File},
	OperationDefinition{5,6,1,2,4096,0,0,0,Handler::File},
	OperationDefinition{5,7,1,2,8192,0,0,0,Handler::File},
	OperationDefinition{6,1,1,8,0,65520,0,0,Handler::Clipboard},
	OperationDefinition{6,2,1,8,65520,0,0,0,Handler::Clipboard},
	OperationDefinition{7,1,1,1,0,sizeof(cemuextend::wire::WindowStatePayload),0,0,Handler::Window},
	OperationDefinition{7,2,1,4,sizeof(cemuextend::wire::PointerPolicyPayload),sizeof(cemuextend::wire::PointerPolicyPayload),0,0,Handler::Window},
	OperationDefinition{7,3,1,1,0,sizeof(cemuextend::wire::PointerPolicyPayload),0,0,Handler::Window},
	OperationDefinition{8,1,1,16,1,64,0,0,Handler::Capture},
	OperationDefinition{8,2,1,16,8,65520,0,0,Handler::Capture},
	OperationDefinition{8,3,1,16,4,0,0,0,Handler::Capture},
	OperationDefinition{9,1,1,1,0,sizeof(cemuextend::wire::DiagnosticsPayload),0,0,Handler::Diagnostics},
	OperationDefinition{10,1,1,kCemodNetworkPermission,sizeof(cemuextend::wire::HttpStartRequest)+2048,sizeof(cemuextend::wire::HttpStartResponse),0,0,Handler::Http},
	OperationDefinition{10,2,1,kCemodNetworkPermission,sizeof(cemuextend::wire::HttpPollRequest),65520,0,0,Handler::Http},
	OperationDefinition{10,3,1,kCemodNetworkPermission,4,0,0,0,Handler::Http},
};

const OperationDefinition* FindOperation(std::uint16_t service, std::uint16_t operation)
{
	const auto found = std::ranges::find_if(kOperations, [=](const OperationDefinition& definition) {
		return definition.service == service && definition.operation == operation;
	});
	return found == kOperations.end() ? nullptr : &*found;
}

constexpr std::array kServices{
	ServiceDefinition{1, 1, 0, 64U * 1024U, 64U * 1024U},
	ServiceDefinition{2, 1, 5, 64U * 1024U, 64U * 1024U},
	ServiceDefinition{3, 1, 2, 4U * 1024U, 64},
	ServiceDefinition{4, 1, 3, 64U * 1024U, 64U * 1024U},
	ServiceDefinition{5, 1, 3, 64U * 1024U, 64U * 1024U},
	ServiceDefinition{6, 1, 8, 64U * 1024U, 64U * 1024U},
	ServiceDefinition{7, 1, 1, 64, 256},
	ServiceDefinition{8, 1, 16, 64, 64U * 1024U},
	ServiceDefinition{9, 1, 1, 64, 4U * 1024U},
	ServiceDefinition{10, 1, 32, 64U * 1024U, 64U * 1024U},
};

struct WireServiceDefinition
{
	cemuextend::wire::Be16 id;
	cemuextend::wire::Be16 version;
	cemuextend::wire::Be32 requiredPermission;
	cemuextend::wire::Be32 maximumRequest;
	cemuextend::wire::Be32 maximumResponse;
};
static_assert(sizeof(WireServiceDefinition) == 16);

std::vector<std::byte> MakeResponse(const RequestHeader& request, Status status,
	std::span<const std::byte> payload = {})
{
	std::vector<std::byte> result(sizeof(ResponseHeader) + payload.size());
	ResponseHeader header{};
	header.totalSize = static_cast<std::uint32_t>(result.size());
	header.correlationId = request.correlationId.get();
	header.serviceId = request.serviceId.get();
	header.operation = request.operation.get();
	header.status = static_cast<std::uint16_t>(status);
	header.flags = 0;
	std::memcpy(result.data(), &header, sizeof(header));
	if (!payload.empty())
		std::memcpy(result.data() + sizeof(ResponseHeader), payload.data(), payload.size());
	return result;
}

} // namespace

struct Cex2Host::Impl
{
	struct Session
	{
		struct Pending
		{
			std::uint32_t permission{};
			RequestHeader header{};
			std::chrono::steady_clock::time_point deadline{};
		};
		Cex2Owner* owner{};
		std::uint64_t addressSpaceId{};
		std::uint32_t generation{};
		std::deque<std::vector<std::byte>> responses;
		std::unordered_set<std::uint16_t> subscriptions;
		std::uint64_t acceptedRequests{};
		std::uint64_t completedResponses{};
		std::uint64_t protocolErrors{};
		std::uint64_t droppedEvents{};
		std::uint64_t bytesCopied{};
		std::uint64_t nextInputEventId{1};
		std::size_t reservedResponses{};
		std::unordered_map<std::uint32_t, Pending> pending;
		// Compact exact-once admission history. Sequential IDs occupy one range;
		// pathological sparse IDs are bounded and reap the session.
		std::map<std::uint32_t, std::uint32_t> admittedRanges;
		double loggingTokens{50.0};
		std::chrono::steady_clock::time_point loggingLastRefill{std::chrono::steady_clock::now()};
		std::set<std::uint16_t> pressedKeyboardUsages;
		cemuextend::wire::PointerPolicyPayload pointerPolicy{};
		std::uint64_t pointerPolicySequence{};
		Cex2HostTextInputState textInput{};
		std::array<cemuextend::wire::ObservedVpadState, 2> observedVpad{};
		std::array<bool, 2> hasObservedVpad{};
		std::array<cemuextend::wire::ObservedVpadState, 2> mappedInjection{};
		std::array<bool, 2> hasMappedInjection{};
		std::array<std::chrono::steady_clock::time_point, 2> mappedInjectionTime{};
		bool clipboardPending{};
		struct Capture
		{
			std::uint32_t handle{};
			std::uint32_t width{};
			std::uint32_t height{};
			std::vector<std::byte> rgb;
			std::chrono::steady_clock::time_point expires{};
			bool pending{};
			bool mainWindow{};
		} capture;
	};

	std::mutex mutex;
	std::uint64_t nextTextInputSequence{1};
	void (*textInputWakeCallback)(){};
	std::unordered_map<std::uint32_t, Session> sessions;
	std::uint32_t nextSession{1};
	std::uint64_t nextPointerPolicySequence{1};
	cemuextend::wire::MouseEventPayloadV2 hostMouse{};
	std::mutex workMutex;
	std::condition_variable workReady;
	std::deque<std::function<void()>> work;
	bool stopping{};
	std::array<std::thread, 2> workers;

	Impl()
	{
		hostMouse.focused = 1;
		for (auto& worker : workers)
			worker = std::thread([this] {
				for (;;)
				{
					std::function<void()> task;
					{
						std::unique_lock lock(workMutex);
						workReady.wait(lock, [this] { return stopping || !work.empty(); });
						if (stopping && work.empty()) break;
						task = std::move(work.front()); work.pop_front();
					}
					task();
				}
				// Storage hashing initializes OpenSSL's per-thread RCU state. Release
				// it while the worker still owns the thread-local allocation.
				OPENSSL_thread_stop();
			});
	}

	~Impl()
	{
		{ std::lock_guard lock(workMutex); stopping = true; }
		workReady.notify_all();
		for (auto& worker : workers) if (worker.joinable()) worker.join();
	}

	void Enqueue(std::function<void()> task)
	{
		{ std::lock_guard lock(workMutex); work.push_back(std::move(task)); }
		workReady.notify_one();
	}

	void Complete(std::uint32_t sessionId, std::uint64_t addressSpaceId, std::uint32_t generation,
		std::uint32_t correlationId, Status status, std::span<const std::byte> payload = {})
	{
		std::lock_guard lock(mutex);
		const auto found = sessions.find(sessionId);
		if (found == sessions.end() || found->second.addressSpaceId != addressSpaceId ||
			found->second.generation != generation) return;
		auto& session = found->second;
		const auto pending = session.pending.find(correlationId);
		if (pending == session.pending.end()) return;
		if (!HasPermission(session, pending->second.permission,
			pending->second.header.serviceId.get(), pending->second.header.operation.get()))
			status = Status::PermissionDenied;
		const auto service = pending->second.header.serviceId.get();
		if (service == static_cast<std::uint16_t>(ServiceId::Clipboard)) session.clipboardPending = false;
		if (service == static_cast<std::uint16_t>(ServiceId::Capture) && status != Status::Ok)
			session.capture = {};
		auto response = MakeResponse(pending->second.header, status, payload);
		session.bytesCopied += response.size();
		session.responses.push_back(std::move(response));
		session.pending.erase(pending);
		--session.reservedResponses;
	}

	static bool Owns(const Session& session, Cex2Owner& owner)
	{
		return session.owner == &owner && session.addressSpaceId == owner.AddressSpaceId() &&
			session.generation == owner.Generation() && !owner.IsStopped();
	}

	static bool HasPermission(const Session& session, std::uint32_t permission,
		std::uint16_t service = 0, std::uint16_t operation = 0)
	{
		const bool granted = permission == 0 ||
			(session.owner->GrantedPermissions() & permission) == permission;
		// HTTP is authorized by the dedicated per-Mod Network grant. It is not
		// part of the legacy title-wide read/write/inject service matrix.
		const bool networkService =
			service == static_cast<std::uint16_t>(cemuextend::wire::ServiceId::Http) &&
			permission == kCemodNetworkPermission;
		return granted && (service == 0 || networkService ||
			session.owner->IsServiceAllowed(service, permission, operation));
	}

	static bool IsValidPointerPolicy(const cemuextend::wire::PointerPolicyPayload& policy)
	{
		using namespace cemuextend::wire;
		const auto flags = policy.flags.get();
		const auto rawModeFlags =
			static_cast<std::uint32_t>(PointerPolicyFlag::PreferRawMouse) |
			static_cast<std::uint32_t>(PointerPolicyFlag::DisableRawMouse);
		const auto allowedFlags = rawModeFlags |
			static_cast<std::uint32_t>(PointerPolicyFlag::ConfineToContent);
		return policy.mode <= static_cast<std::uint8_t>(PointerMode::CapturedRelative) &&
			policy.cursor <= static_cast<std::uint8_t>(PointerCursor::NotAllowed) &&
			policy.surface <= static_cast<std::uint8_t>(PointerSurface::Drc) &&
			policy.reserved == 0 &&
			(flags & ~allowedFlags) == 0 &&
			(flags & rawModeFlags) != rawModeFlags;
	}

	[[nodiscard]] cemuextend::wire::PointerPolicyPayload EffectivePointerPolicyLocked() const
	{
		using namespace cemuextend::wire;
		PointerPolicyPayload result{};
		if (!hostMouse.focused)
			return result;
		std::uint64_t newest{};
		for (const auto& [id, session] : sessions)
		{
			if (session.pointerPolicy.mode == static_cast<std::uint8_t>(PointerMode::Default) ||
				session.pointerPolicySequence <= newest ||
				!HasPermission(session, 4, static_cast<std::uint16_t>(ServiceId::Window),
					static_cast<std::uint16_t>(WindowOperation::SetPointerPolicy)))
				continue;
			newest = session.pointerPolicySequence;
			result = session.pointerPolicy;
		}
		return result;
	}

	static bool AdmitCorrelation(Session& session, std::uint32_t correlation)
	{
		auto next = session.admittedRanges.upper_bound(correlation);
		auto previous = next == session.admittedRanges.begin() ? session.admittedRanges.end() : std::prev(next);
		if (previous != session.admittedRanges.end() && previous->second >= correlation)
			return false;
		const bool joinsPrevious = previous != session.admittedRanges.end() &&
			previous->second != std::numeric_limits<std::uint32_t>::max() &&
			previous->second + 1 == correlation;
		const bool joinsNext = next != session.admittedRanges.end() &&
			correlation != std::numeric_limits<std::uint32_t>::max() &&
			correlation + 1 == next->first;
		if (joinsPrevious)
		{
			previous->second = joinsNext ? next->second : correlation;
			if (joinsNext) session.admittedRanges.erase(next);
			return true;
		}
		if (joinsNext)
		{
			const auto end = next->second;
			session.admittedRanges.erase(next);
			session.admittedRanges.emplace(correlation, end);
			return true;
		}
		if (session.admittedRanges.size() >= 4096)
			return false;
		session.admittedRanges.emplace(correlation, correlation);
		return true;
	}

	static bool IsValidUtf8(std::string_view text)
	{
		for (std::size_t index = 0; index < text.size();)
		{
			const auto lead = static_cast<std::uint8_t>(text[index]);
			if (lead < 0x80) { ++index; continue; }
			std::size_t count = (lead & 0xe0) == 0xc0 ? 1 : (lead & 0xf0) == 0xe0 ? 2 :
				(lead & 0xf8) == 0xf0 ? 3 : 0;
			if (!count || index + count >= text.size()) return false;
			std::uint32_t codepoint = lead & (0x7fU >> count);
			for (std::size_t offset = 1; offset <= count; ++offset)
			{
				const auto next = static_cast<std::uint8_t>(text[index + offset]);
				if ((next & 0xc0) != 0x80) return false;
				codepoint = (codepoint << 6) | (next & 0x3f);
			}
			if ((count == 1 && codepoint < 0x80) || (count == 2 && codepoint < 0x800) ||
				(count == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
				(codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
			index += count + 1;
		}
		return true;
	}

	void EmitEvent(Session& session, ServiceId service, std::uint16_t operation,
		std::span<const std::byte> payload)
	{
		const auto serviceId = static_cast<std::uint16_t>(service);
		if ((service != ServiceId::Core && !HasPermission(session, 1, serviceId, operation)) ||
			!session.subscriptions.contains(serviceId) ||
			session.responses.size() + session.reservedResponses >=
				cemuextend::transport::kMaximumResponseQueue)
		{
			if (session.subscriptions.contains(static_cast<std::uint16_t>(service))) ++session.droppedEvents;
			return;
		}
		std::vector<std::byte> result(sizeof(ResponseHeader) + payload.size());
		ResponseHeader header{};
		header.totalSize = static_cast<std::uint32_t>(result.size());
		header.correlationId = 0;
		header.serviceId = static_cast<std::uint16_t>(service);
		header.operation = operation;
		header.status = static_cast<std::uint16_t>(Status::Ok);
		header.flags = static_cast<std::uint16_t>(cemuextend::transport::ResponseFlag::Event);
		std::memcpy(result.data(), &header, sizeof(header));
		if (!payload.empty()) std::memcpy(result.data() + sizeof(ResponseHeader), payload.data(), payload.size());
		session.responses.push_back(std::move(result));
	}

	void EmitMouseEventLocked(const cemuextend::wire::MouseEventPayloadV2& state)
	{
		using namespace cemuextend::wire;
		hostMouse = state;
		for (auto& [id, session] : sessions)
		{
			if (!HasPermission(session, 1, static_cast<std::uint16_t>(ServiceId::Input),
				static_cast<std::uint16_t>(InputEvent::MouseV2)))
				continue;
			auto event = state;
			event.identity.eventId = session.nextInputEventId++;
			event.identity.parentEventId = 0;
			event.identity.origin = static_cast<std::uint8_t>(InputOrigin::Physical);
			event.identity.channel = static_cast<std::uint8_t>(InputChannel::Mouse);
			event.identity.deviceId = static_cast<std::uint16_t>(event.surface);
			event.identity.frameNumber = static_cast<std::uint32_t>(CurrentFrameNumber());
			EmitEvent(session, ServiceId::Input,
				static_cast<std::uint16_t>(InputEvent::MouseV2),
				{reinterpret_cast<const std::byte*>(&event), sizeof(event)});
		}
	}

	std::vector<std::byte> Dispatch(Session& session, const RequestHeader& request,
		std::span<const std::byte> payload)
	{
		using namespace cemuextend::wire;
		const auto* definition = FindOperation(request.serviceId.get(), request.operation.get());
		if (!definition)
			return MakeResponse(request, Status::NotSupported);
		if (request.operationVersion.get() != definition->version)
			return MakeResponse(request, Status::NotSupported);
		if (!HasPermission(session, definition->permission,
			request.serviceId.get(), request.operation.get()))
			return MakeResponse(request, Status::PermissionDenied);
		if (payload.size() > definition->maximumRequest)
			return MakeResponse(request, Status::TooLarge);

		if (definition->handler == Handler::Input)
		{
			if (request.operation.get() == static_cast<std::uint16_t>(InputOperation::SetTextInput))
			{
				if (payload.size() < sizeof(TextInputRequestHeader))
					return MakeResponse(request, Status::InvalidArgument);
				TextInputRequestHeader header{};
				std::memcpy(&header, payload.data(), sizeof(header));
				const auto textBytes = header.textBytes.get();
				const auto allowedFlags = static_cast<std::uint8_t>(TextInputFlag::Active) |
					static_cast<std::uint8_t>(TextInputFlag::Multiline);
				if (payload.size() != sizeof(header) + textBytes ||
					(header.flags & ~allowedFlags) != 0 ||
					header.reserved != std::array<std::byte, 3>{} ||
					header.maximumLength.get() > 4096 || header.lineHeight.get() < 0)
					return MakeResponse(request, Status::InvalidArgument);
				const std::string_view text{
					reinterpret_cast<const char*>(payload.data() + sizeof(header)), textBytes};
				if (!IsValidUtf8(text)) return MakeResponse(request, Status::InvalidArgument);
				const bool active = (header.flags &
					static_cast<std::uint8_t>(TextInputFlag::Active)) != 0;
				const auto requestId = header.requestId.get();
				if (active && (!session.textInput.active ||
					session.textInput.requestId != requestId))
					session.textInput.sequence = nextTextInputSequence++;
				session.textInput.active = active;
				session.textInput.requestId = header.requestId.get();
				session.textInput.maximumLength = header.maximumLength.get();
				session.textInput.caretX = header.caretX.get();
				session.textInput.caretY = header.caretY.get();
				session.textInput.lineHeight = header.lineHeight.get();
				session.textInput.initialText.assign(text);
				if (textInputWakeCallback) textInputWakeCallback();
				return MakeResponse(request, Status::Ok);
			}
			if (request.operation.get() == static_cast<std::uint16_t>(InputOperation::GetHostMouse))
			{
				if (!payload.empty())
					return MakeResponse(request, Status::InvalidArgument);
				return MakeResponse(request, Status::Ok,
					{reinterpret_cast<const std::byte*>(&hostMouse), sizeof(hostMouse)});
			}
			if (request.operation.get() == static_cast<std::uint16_t>(InputOperation::GetObserved))
			{
				Decoder decoder(payload);
				std::uint8_t channel{};
				if (!decoder.U8(channel) || decoder.remaining() || channel >= session.observedVpad.size())
					return MakeResponse(request, Status::InvalidArgument);
				if (!session.hasObservedVpad[channel])
					return MakeResponse(request, Status::NotFound);
				return MakeResponse(request, Status::Ok,
					{reinterpret_cast<const std::byte*>(&session.observedVpad[channel]),
						sizeof(session.observedVpad[channel])});
			}
			if (request.operation.get() == static_cast<std::uint16_t>(InputOperation::InjectGuest))
			{
				if (payload.size() != sizeof(ControllerEventPayload))
					return MakeResponse(request, Status::InvalidArgument);
				ControllerEventPayload event{};
				std::memcpy(&event, payload.data(), sizeof(event));
				const std::array values{event.leftX.get(), event.leftY.get(), event.rightX.get(),
					event.rightY.get(), event.leftTrigger.get(), event.rightTrigger.get()};
				if (!std::ranges::all_of(values, [](float value) { return std::isfinite(value); }))
					return MakeResponse(request, Status::InvalidArgument);
				event.identity.eventId = session.nextInputEventId++;
				event.identity.parentEventId = 0;
				event.identity.origin = static_cast<std::uint8_t>(InputOrigin::ClientInjected);
				event.identity.channel = static_cast<std::uint8_t>(InputChannel::Controller);
				event.identity.deviceId = 0;
				event.identity.frameNumber = CurrentFrameNumber();
				event.leftX = std::clamp(event.leftX.get(), -1.0f, 1.0f);
				event.leftY = std::clamp(event.leftY.get(), -1.0f, 1.0f);
				event.rightX = std::clamp(event.rightX.get(), -1.0f, 1.0f);
				event.rightY = std::clamp(event.rightY.get(), -1.0f, 1.0f);
				event.leftTrigger = std::clamp(event.leftTrigger.get(), 0.0f, 1.0f);
				event.rightTrigger = std::clamp(event.rightTrigger.get(), 0.0f, 1.0f);
				AuditSensitiveUse(session.owner->Principal(), "Input Inject");
				EmitEvent(session, ServiceId::Input, static_cast<std::uint16_t>(InputEvent::Controller),
					{reinterpret_cast<const std::byte*>(&event), sizeof(event)});
				return MakeResponse(request, Status::Ok);
			}
			if (payload.size() != 1 + sizeof(ObservedVpadState))
				return MakeResponse(request, Status::InvalidArgument);
			const auto channel = std::to_integer<std::uint8_t>(payload[0]);
			if (channel >= 2) return MakeResponse(request, Status::InvalidArgument);
			ObservedVpadState injected{};
			std::memcpy(&injected, payload.data() + 1, sizeof(injected));
			const auto allowedFlags = static_cast<std::uint8_t>(MappedInputFlag::ReplacePhysical);
			if ((injected.flags & ~allowedFlags) != 0 ||
				injected.reserved[0] != std::byte{} || injected.reserved[1] != std::byte{})
				return MakeResponse(request, Status::InvalidArgument);
			const std::array sticks{injected.leftX.get(), injected.leftY.get(),
				injected.rightX.get(), injected.rightY.get()};
			if (!std::ranges::all_of(sticks, [](float value) { return std::isfinite(value); }))
				return MakeResponse(request, Status::InvalidArgument);
			injected.leftX = std::clamp(injected.leftX.get(), -1.0f, 1.0f);
			injected.leftY = std::clamp(injected.leftY.get(), -1.0f, 1.0f);
			injected.rightX = std::clamp(injected.rightX.get(), -1.0f, 1.0f);
			injected.rightY = std::clamp(injected.rightY.get(), -1.0f, 1.0f);
			const auto now = std::chrono::steady_clock::now();
			const bool startsLease =
				!session.hasMappedInjection[channel] ||
				now - session.mappedInjectionTime[channel] >
					std::chrono::milliseconds(250);
			session.mappedInjection[channel] = injected;
			session.hasMappedInjection[channel] = true;
			session.mappedInjectionTime[channel] = now;
			if (startsLease)
				AuditSensitiveUse(session.owner->Principal(), "Mapped Input Inject", false);
			return MakeResponse(request, Status::Ok);
		}
		if (definition->handler == Handler::Logging)
		{
			Decoder decoder(payload);
			std::uint8_t level{};
			std::string message;
			if (!decoder.U8(level) || !decoder.String(message) || decoder.remaining() ||
				message.size() > 4096 || !IsValidUtf8(message) ||
				level > static_cast<std::uint8_t>(LogLevel::Critical))
				return MakeResponse(request, Status::InvalidArgument);
			const auto now = std::chrono::steady_clock::now();
			const auto elapsed = std::chrono::duration<double>(now - session.loggingLastRefill).count();
			session.loggingLastRefill = now;
			session.loggingTokens = std::min(50.0, session.loggingTokens + elapsed * 20.0);
			if (session.loggingTokens < 1.0) return MakeResponse(request, Status::Busy);
			--session.loggingTokens;
			std::string escaped;
			constexpr char hex[] = "0123456789abcdef";
			for (std::size_t index = 0; index < message.size(); ++index)
			{
				const auto character = static_cast<unsigned char>(message[index]);
				if (character < 0x20 || character == 0x7f)
				{
					escaped.append("\\x"); escaped.push_back(hex[character >> 4]); escaped.push_back(hex[character & 15]);
				}
				else if (character == 0xc2 && index + 1 < message.size() &&
					static_cast<unsigned char>(message[index + 1]) >= 0x80 &&
					static_cast<unsigned char>(message[index + 1]) <= 0x9f)
				{
					const auto control = static_cast<unsigned char>(message[++index]);
					escaped.append("\\u00");
					escaped.push_back(hex[control >> 4]); escaped.push_back(hex[control & 15]);
				}
				else escaped.push_back(static_cast<char>(character));
			}
			LogGuestRecord(session.owner->Principal(), level, escaped);
			return MakeResponse(request, Status::Ok);
		}
		if (definition->handler == Handler::Window)
		{
			if (request.operation.get() == static_cast<std::uint16_t>(WindowOperation::SetPointerPolicy))
			{
				if (payload.size() != sizeof(PointerPolicyPayload))
					return MakeResponse(request, Status::InvalidArgument);
				PointerPolicyPayload policy{};
				std::memcpy(&policy, payload.data(), sizeof(policy));
				if (!IsValidPointerPolicy(policy))
					return MakeResponse(request, Status::InvalidArgument);
				session.pointerPolicy = policy;
				session.pointerPolicySequence = nextPointerPolicySequence++;
				AuditSensitiveUse(session.owner->Principal(), "Pointer Policy", false);
				return MakeResponse(request, Status::Ok,
					{reinterpret_cast<const std::byte*>(&session.pointerPolicy),
						sizeof(session.pointerPolicy)});
			}
			if (request.operation.get() == static_cast<std::uint16_t>(WindowOperation::GetPointerPolicy))
			{
				if (!payload.empty()) return MakeResponse(request, Status::InvalidArgument);
				return MakeResponse(request, Status::Ok,
					{reinterpret_cast<const std::byte*>(&session.pointerPolicy),
						sizeof(session.pointerPolicy)});
			}
			if (request.operation.get() != static_cast<std::uint16_t>(WindowOperation::Get) ||
				!payload.empty())
				return MakeResponse(request, Status::InvalidArgument);
			WindowStatePayload state{};
			state.frameNumber = CurrentFrameNumber();
#ifndef CEMU_CEX2_TESTING
			const auto& window = WindowSystem::GetWindowInfo();
			state.tvWidth = std::max(0, window.phys_width.load());
			state.tvHeight = std::max(0, window.phys_height.load());
			state.drcWidth = std::max(0, window.phys_pad_width.load());
			state.drcHeight = std::max(0, window.phys_pad_height.load());
			state.dpiScale = static_cast<float>(window.dpi_scale.load());
			state.focused = window.app_active.load();
			state.fullscreen = window.is_fullscreen.load();
#endif
			return MakeResponse(request, Status::Ok,
				{reinterpret_cast<const std::byte*>(&state), sizeof(state)});
		}
		if (definition->handler == Handler::Diagnostics)
		{
			if (!payload.empty()) return MakeResponse(request, Status::InvalidArgument);
			DiagnosticsPayload diagnostics{};
			diagnostics.hostHeartbeat = static_cast<std::uint32_t>(CurrentFrameNumber());
			diagnostics.sessionState = 1;
			diagnostics.queuedResponses = static_cast<std::uint32_t>(session.responses.size());
			diagnostics.reservedResponses = static_cast<std::uint32_t>(session.reservedResponses);
			diagnostics.pendingRequests = static_cast<std::uint32_t>(session.pending.size());
			diagnostics.activeSubscriptions = static_cast<std::uint32_t>(session.subscriptions.size());
			diagnostics.droppedEvents = session.droppedEvents;
			diagnostics.protocolErrors = session.protocolErrors;
			diagnostics.requests = session.acceptedRequests;
			diagnostics.responses = session.completedResponses;
			diagnostics.bytesCopied = session.bytesCopied;
			return MakeResponse(request, Status::Ok,
				{reinterpret_cast<const std::byte*>(&diagnostics), sizeof(diagnostics)});
		}
		if (definition->handler == Handler::Http)
		{
			if (request.operation.get() == static_cast<std::uint16_t>(HttpOperation::Start))
				AuditSensitiveUse(session.owner->Principal(), "Network Fetch", false);
			auto result = Cex2Http::Dispatch(session.addressSpaceId,
				session.owner->Principal(), request.operation.get(), payload);
			if (result.status != Status::Ok)
				return MakeResponse(request, result.status);
			return MakeResponse(request, Status::Ok, result.payload);
		}
		if (definition->handler == Handler::Configuration || definition->handler == Handler::File)
		{
			auto result = Cex2Storage::Dispatch(session.owner->TitleId(), session.owner->Principal(),
				static_cast<ServiceId>(request.serviceId.get()), request.operation.get(), payload);
			if (result.payload.size() > definition->maximumResponse)
				return MakeResponse(request, Status::TooLarge);
			return MakeResponse(request, result.status, result.payload);
		}
		if (definition->handler == Handler::Capture)
		{
			if (session.capture.handle && std::chrono::steady_clock::now() >= session.capture.expires)
				session.capture = {};
			Decoder decoder(payload); std::uint32_t handle{};
			if (request.operation.get() == static_cast<std::uint16_t>(CaptureOperation::Read))
			{
				std::uint32_t offset{};
				if (!decoder.U32(handle) || !decoder.U32(offset) || decoder.remaining() ||
					handle == 0 || handle != session.capture.handle || offset > session.capture.rgb.size())
					return MakeResponse(request, Status::NotFound);
				const auto size = std::min<std::size_t>(64U * 1024U - sizeof(ResponseHeader),
					session.capture.rgb.size() - offset);
				return MakeResponse(request, Status::Ok,
					std::span<const std::byte>(session.capture.rgb).subspan(offset, size));
			}
			if (request.operation.get() == static_cast<std::uint16_t>(CaptureOperation::Close))
			{
				if (!decoder.U32(handle) || decoder.remaining() || handle == 0 || handle != session.capture.handle)
					return MakeResponse(request, Status::NotFound);
				session.capture = {};
				return MakeResponse(request, Status::Ok);
			}
			return MakeResponse(request, Status::NotSupported);
		}
		if (definition->handler != Handler::Core)
			return MakeResponse(request, Status::NotSupported);

		Encoder encoder;
		switch (static_cast<CoreOperation>(request.operation.get()))
		{
		case CoreOperation::GetServices:
			if (!payload.empty())
				return MakeResponse(request, Status::InvalidArgument);
			encoder.U16(static_cast<std::uint16_t>(kServices.size()));
			for (const auto& service : kServices)
			{
				WireServiceDefinition descriptor{};
				descriptor.id = service.id;
				descriptor.version = service.version;
				descriptor.requiredPermission = service.requiredPermission;
				descriptor.maximumRequest = service.maximumRequest;
				descriptor.maximumResponse = service.maximumResponse;
				const auto* bytes = reinterpret_cast<const std::byte*>(&descriptor);
				encoder.Bytes({bytes, sizeof(descriptor)});
			}
			return MakeResponse(request, Status::Ok, encoder.data());
		case CoreOperation::Ping:
			return payload.size() == sizeof(std::uint64_t) ? MakeResponse(request, Status::Ok, payload)
				: MakeResponse(request, Status::InvalidArgument);
		case CoreOperation::GetVersion:
			if (!payload.empty())
				return MakeResponse(request, Status::InvalidArgument);
			encoder.U16(cemuextend::transport::kAbiMajor);
			encoder.U16(cemuextend::transport::kAbiMinor);
			encoder.U16(1); // core service version
			encoder.U16(cemuextend::transport::kOperationVersion);
			encoder.U64(kCemuExtendBuildId);
			return MakeResponse(request, Status::Ok, encoder.data());
		case CoreOperation::Subscribe:
		case CoreOperation::Unsubscribe:
		{
			Decoder decoder(payload);
			std::uint16_t service{};
			if (!decoder.U16(service) || decoder.remaining() != 0 || service == 0)
				return MakeResponse(request, Status::InvalidArgument);
			const auto exists = std::ranges::any_of(kServices,
				[service](const ServiceDefinition& definition) { return definition.id == service; });
			const bool supportsEvents = service == static_cast<std::uint16_t>(ServiceId::Core) ||
				service == static_cast<std::uint16_t>(ServiceId::Input) ||
				service == static_cast<std::uint16_t>(ServiceId::Window);
			if (!exists || !supportsEvents)
				return MakeResponse(request, Status::NotSupported);
			if (service != static_cast<std::uint16_t>(ServiceId::Core) &&
				!HasPermission(session, 1, service))
				return MakeResponse(request, Status::PermissionDenied);
			if (static_cast<CoreOperation>(request.operation.get()) == CoreOperation::Subscribe)
				session.subscriptions.insert(service);
			else
				session.subscriptions.erase(service);
			return MakeResponse(request, Status::Ok);
		}
		case CoreOperation::GetStatistics:
			if (!payload.empty())
				return MakeResponse(request, Status::InvalidArgument);
			encoder.U64(session.acceptedRequests);
			encoder.U64(session.completedResponses);
			encoder.U32(static_cast<std::uint32_t>(session.responses.size()));
			return MakeResponse(request, Status::Ok, encoder.data());
		case CoreOperation::Cancel:
			return MakeResponse(request, Status::NotSupported);
		default:
			return MakeResponse(request, Status::NotSupported);
		}
	}
};

Cex2Host& Cex2Host::Instance()
{
	static Cex2Host instance;
	return instance;
}

Cex2Host::Cex2Host() : m_impl(std::make_unique<Impl>()) {}
Cex2Host::~Cex2Host() = default;

std::int32_t Cex2Host::Query(Cex2Owner& owner, std::uint32_t query,
	std::span<std::byte> output)
{
	if (owner.IsStopped())
		return static_cast<std::int32_t>(Error::InvalidArgument);
	if (query == static_cast<std::uint32_t>(cemuextend::transport::Query::MemoryLayout))
	{
		if (output.size() < sizeof(cemuextend::transport::MemoryLayout))
			return static_cast<std::int32_t>(Error::InvalidArgument);
		cemuextend::transport::MemoryLayout layout{};
#ifdef CEMU_CEX2_TESTING
		layout.mem2Base = 0x10000000;
		layout.mem2End = 0x50000000;
		layout.mappedMemoryBase = 0x60000000;
		layout.mappedMemoryEnd = 0xa0000000;
#else
		layout.mem2Base = mmuRange_MEM2.getBase();
		layout.mem2End = mmuRange_MEM2.getEnd();
		layout.mappedMemoryBase = MEMORY_MAPPED_AREA_ADDR;
		layout.mappedMemoryEnd = MEMORY_MAPPED_AREA_ADDR + MEMORY_MAPPED_AREA_SIZE;
#endif
		std::memcpy(output.data(), &layout, sizeof(layout));
		return static_cast<std::int32_t>(Error::Ok);
	}
	if (query != static_cast<std::uint32_t>(cemuextend::transport::Query::Info) ||
		output.size() < sizeof(cemuextend::transport::Info))
		return static_cast<std::int32_t>(Error::NotSupported);
	cemuextend::transport::Info info{};
	info.abiMajor = cemuextend::transport::kAbiMajor;
	info.abiMinor = cemuextend::transport::kAbiMinor;
	info.maximumMessageSize = cemuextend::transport::kMaximumMessageSize;
	info.maximumResponseQueue = cemuextend::transport::kMaximumResponseQueue;
	info.maximumPageEntries = cemuextend::transport::kMaximumPageEntries;
	info.hostBuildId = kCemuExtendBuildId;
	info.features = static_cast<std::uint64_t>(cemuextend::transport::Feature::CopyTransport) |
		static_cast<std::uint64_t>(cemuextend::transport::Feature::Cancellation) |
		static_cast<std::uint64_t>(cemuextend::transport::Feature::Pagination) |
		static_cast<std::uint64_t>(cemuextend::transport::Feature::PermissionRevocation) |
		static_cast<std::uint64_t>(cemuextend::transport::Feature::MemoryLayoutQuery);
	info.coreServiceVersion = 1;
	std::memcpy(output.data(), &info, sizeof(info));
	return static_cast<std::int32_t>(Error::Ok);
}

std::int32_t Cex2Host::Open(Cex2Owner& owner, std::span<const std::byte> options,
	std::uint32_t& sessionId)
{
	if (owner.IsStopped() || options.size() != sizeof(cemuextend::transport::OpenOptions))
		return static_cast<std::int32_t>(Error::InvalidArgument);
	cemuextend::transport::OpenOptions requested{};
	std::memcpy(&requested, options.data(), sizeof(requested));
	if (requested.abiMajor.get() != cemuextend::transport::kAbiMajor ||
		requested.abiMinor.get() > cemuextend::transport::kAbiMinor)
		return static_cast<std::int32_t>(Error::AbiMismatch);
	if (requested.flags.get() != 0 || requested.reserved.get() != 0 ||
		requested.maximumPendingRequests.get() == 0 ||
		requested.maximumPendingRequests.get() > cemuextend::transport::kMaximumResponseQueue)
		return static_cast<std::int32_t>(Error::InvalidArgument);

	std::lock_guard lock(m_impl->mutex);
	if (m_impl->sessions.size() >= 16)
		return static_cast<std::int32_t>(Error::Busy);
	for (const auto& [id, session] : m_impl->sessions)
		if (Impl::Owns(session, owner))
			return static_cast<std::int32_t>(Error::Busy);
	for (std::uint64_t attempt = 0; attempt <= std::numeric_limits<std::uint32_t>::max(); ++attempt)
	{
		sessionId = m_impl->nextSession++;
		if (sessionId != 0 && !m_impl->sessions.contains(sessionId))
			break;
		sessionId = 0;
	}
	if (!sessionId)
		return static_cast<std::int32_t>(Error::Busy);
	m_impl->sessions.emplace(sessionId, Impl::Session{&owner, owner.AddressSpaceId(), owner.Generation()});
	return static_cast<std::int32_t>(Error::Ok);
}

std::int32_t Cex2Host::Submit(Cex2Owner& owner, std::uint32_t sessionId,
	std::span<const std::byte> requestBytes)
{
	std::unique_lock lock(m_impl->mutex);
	const auto found = m_impl->sessions.find(sessionId);
	if (found == m_impl->sessions.end() || !Impl::Owns(found->second, owner))
		return static_cast<std::int32_t>(Error::PermissionDenied);
	auto& session = found->second;
	if (session.responses.size() + session.reservedResponses >=
		cemuextend::transport::kMaximumResponseQueue)
		return static_cast<std::int32_t>(Error::Busy);
	if (requestBytes.size() < sizeof(RequestHeader) ||
		requestBytes.size() > cemuextend::transport::kMaximumMessageSize)
	{
		m_impl->sessions.erase(found);
		return static_cast<std::int32_t>(Error::ProtocolError);
	}
	RequestHeader request{};
	std::memcpy(&request, requestBytes.data(), sizeof(request));
	if (request.totalSize.get() != requestBytes.size() || request.correlationId.get() == 0 ||
		request.flags.get() != 0)
	{
		m_impl->sessions.erase(found);
		return static_cast<std::int32_t>(Error::ProtocolError);
	}
	const auto correlationId = request.correlationId.get();
	if (!Impl::AdmitCorrelation(session, correlationId))
	{
		m_impl->sessions.erase(found);
		return static_cast<std::int32_t>(Error::ProtocolError);
	}
	const auto payload = requestBytes.subspan(sizeof(RequestHeader));
	const auto* definition = FindOperation(request.serviceId.get(), request.operation.get());
	const bool asynchronous = definition && request.operationVersion.get() == definition->version &&
		(definition->handler == Handler::Configuration || definition->handler == Handler::File);
	if (definition && request.operationVersion.get() == definition->version &&
		definition->handler == Handler::Clipboard)
	{
		if (!Impl::HasPermission(session, definition->permission,
			request.serviceId.get(), request.operation.get()))
		{
			session.responses.push_back(MakeResponse(request, Status::PermissionDenied));
			++session.acceptedRequests; return static_cast<std::int32_t>(Error::Ok);
		}
		if (session.clipboardPending)
		{
			session.responses.push_back(MakeResponse(request, Status::Busy));
			++session.acceptedRequests;
			return static_cast<std::int32_t>(Error::Ok);
		}
		std::string text;
		if (request.operation.get() == static_cast<std::uint16_t>(cemuextend::wire::ClipboardOperation::Get))
		{
			if (!payload.empty()) { session.responses.push_back(MakeResponse(request, Status::InvalidArgument)); ++session.acceptedRequests; return static_cast<std::int32_t>(Error::Ok); }
		}
		else
		{
			cemuextend::wire::Decoder decoder(payload);
			if (!decoder.String(text) || decoder.remaining() || text.size() > 64U * 1024U ||
				!Impl::IsValidUtf8(text))
			{ session.responses.push_back(MakeResponse(request, Status::InvalidArgument)); ++session.acceptedRequests; return static_cast<std::int32_t>(Error::Ok); }
		}
		const auto copiedHeader = request; const auto addressSpaceId = owner.AddressSpaceId();
		const auto generation = owner.Generation(); const auto principal = owner.Principal();
		++session.reservedResponses; session.clipboardPending = true;
		session.pending.emplace(correlationId, Impl::Session::Pending{definition->permission, copiedHeader,
			std::chrono::steady_clock::now() + std::chrono::seconds(5)});
		++session.acceptedRequests; session.bytesCopied += requestBytes.size();
		lock.unlock();
#ifdef CEMU_CEX2_TESTING
		m_impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::NotSupported);
#else
		AuditSensitiveUse(principal, request.operation.get() == 1 ? "Clipboard Read" : "Clipboard Write");
		if (request.operation.get() == static_cast<std::uint16_t>(cemuextend::wire::ClipboardOperation::Get))
			WindowSystem::GetClipboardTextAsync([impl = m_impl.get(), sessionId, addressSpaceId, generation, correlationId](bool success, std::string result) {
				if (!success) { impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::IoError); return; }
				if (result.size() > 64U * 1024U || !Impl::IsValidUtf8(result)) { impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::TooLarge); return; }
				impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::Ok,
					{reinterpret_cast<const std::byte*>(result.data()), result.size()});
			});
		else
			WindowSystem::SetClipboardTextAsync(std::move(text), [impl = m_impl.get(), sessionId, addressSpaceId, generation, correlationId](bool success) {
				impl->Complete(sessionId, addressSpaceId, generation, correlationId, success ? Status::Ok : Status::IoError);
			});
#endif
		return static_cast<std::int32_t>(Error::Ok);
	}
	if (definition && request.operationVersion.get() == definition->version &&
		definition->handler == Handler::Capture &&
		request.operation.get() == static_cast<std::uint16_t>(cemuextend::wire::CaptureOperation::Open))
	{
		if (!Impl::HasPermission(session, definition->permission,
			request.serviceId.get(), request.operation.get()))
		{ session.responses.push_back(MakeResponse(request, Status::PermissionDenied)); ++session.acceptedRequests; return static_cast<std::int32_t>(Error::Ok); }
		cemuextend::wire::Decoder decoder(payload); std::uint8_t drc{};
		if (!decoder.U8(drc) || decoder.remaining() || drc > 1)
		{ session.responses.push_back(MakeResponse(request, Status::InvalidArgument)); ++session.acceptedRequests; return static_cast<std::int32_t>(Error::Ok); }
		if (session.capture.handle && std::chrono::steady_clock::now() >= session.capture.expires) session.capture = {};
		if (session.capture.pending || session.capture.handle)
		{
			session.responses.push_back(MakeResponse(request, Status::Busy));
			++session.acceptedRequests;
			return static_cast<std::int32_t>(Error::Ok);
		}
		const auto copiedHeader = request; const auto addressSpaceId = owner.AddressSpaceId();
		const auto generation = owner.Generation(); const auto principal = owner.Principal();
		session.capture.pending = true; session.capture.mainWindow = drc == 0;
		++session.reservedResponses;
		session.pending.emplace(correlationId, Impl::Session::Pending{definition->permission, copiedHeader,
			std::chrono::steady_clock::now() + std::chrono::seconds(5)});
		++session.acceptedRequests; session.bytesCopied += requestBytes.size(); lock.unlock();
#ifdef CEMU_CEX2_TESTING
		m_impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::NotSupported);
#else
		AuditSensitiveUse(principal, "Capture");
		if (!g_renderer)
			m_impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::NotSupported);
		else
		{
			const bool mainWindow = drc == 0;
			const auto accepted = g_renderer->RequestScreenshot(
				[impl = m_impl.get(), sessionId, addressSpaceId, generation, correlationId, mainWindow]
				(const std::vector<uint8>& rgb, int width, int height, bool actualMainWindow) {
					Status status = Status::Ok; cemuextend::wire::CaptureOpenResponse response{};
					{
						std::lock_guard guard(impl->mutex); const auto found = impl->sessions.find(sessionId);
						if (found == impl->sessions.end() || found->second.addressSpaceId != addressSpaceId || found->second.generation != generation ||
							!found->second.pending.contains(correlationId) || !found->second.capture.pending) return std::optional<std::string>{};
						auto& capture = found->second.capture; const std::uint64_t w = width > 0 ? width : 0; const std::uint64_t h = height > 0 ? height : 0;
						if (actualMainWindow != mainWindow || w == 0 || h == 0 || w > (64ULL * 1024ULL * 1024ULL) / 3ULL / h || rgb.size() != w * h * 3ULL)
							status = Status::ProtocolError;
						else { capture.pending = false; capture.handle = correlationId; capture.width = width; capture.height = height; capture.expires = std::chrono::steady_clock::now() + std::chrono::seconds(30); capture.rgb.resize(rgb.size()); std::memcpy(capture.rgb.data(), rgb.data(), rgb.size()); response.handle = capture.handle; response.width = width; response.height = height; response.totalBytes = rgb.size(); response.format = 1; response.chunkSize = 64U * 1024U - sizeof(ResponseHeader); }
					}
					impl->Complete(sessionId, addressSpaceId, generation, correlationId, status,
						status == Status::Ok ? std::span<const std::byte>(reinterpret_cast<const std::byte*>(&response), sizeof(response)) : std::span<const std::byte>{});
					return std::optional<std::string>{};
				}, mainWindow);
			if (!accepted) m_impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::Busy);
		}
#endif
		return static_cast<std::int32_t>(Error::Ok);
	}
	if (asynchronous)
	{
		if (!Impl::HasPermission(session, definition->permission,
			request.serviceId.get(), request.operation.get()))
		{
			session.responses.push_back(MakeResponse(request, Status::PermissionDenied));
			++session.acceptedRequests;
			return static_cast<std::int32_t>(Error::Ok);
		}
		if (payload.size() > definition->maximumRequest)
		{
			session.responses.push_back(MakeResponse(request, Status::TooLarge));
			++session.acceptedRequests;
			return static_cast<std::int32_t>(Error::Ok);
		}
		const auto copiedHeader = request;
		std::vector<std::byte> copiedPayload(payload.begin(), payload.end());
		const auto titleId = owner.TitleId();
		const auto principal = owner.Principal();
		const auto addressSpaceId = owner.AddressSpaceId();
		const auto generation = owner.Generation();
		const auto permission = definition->permission;
		const auto maximumResponse = definition->maximumResponse;
		const auto service = static_cast<ServiceId>(request.serviceId.get());
		const auto operation = request.operation.get();
		++session.reservedResponses;
		session.pending.emplace(correlationId, Impl::Session::Pending{permission, copiedHeader,
			std::chrono::steady_clock::now() + std::chrono::seconds(5)});
		++session.acceptedRequests;
		session.bytesCopied += requestBytes.size();
		m_impl->Enqueue([impl = m_impl.get(), sessionId, addressSpaceId, generation,
			copiedHeader, copiedPayload = std::move(copiedPayload), titleId, principal,
			permission, maximumResponse, service, operation, correlationId]() mutable {
			{
				std::lock_guard lock(impl->mutex);
				const auto found = impl->sessions.find(sessionId);
				if (found == impl->sessions.end() || found->second.addressSpaceId != addressSpaceId ||
					found->second.generation != generation) return;
				auto& current = found->second;
				const auto pending = current.pending.find(correlationId);
				if (pending == current.pending.end()) return;
				Status rejected = Status::Ok;
				if (std::chrono::steady_clock::now() >= pending->second.deadline)
					rejected = Status::TimedOut;
				else if (!Impl::HasPermission(current, permission, copiedHeader.serviceId.get(),
					copiedHeader.operation.get()))
					rejected = Status::PermissionDenied;
				if (rejected != Status::Ok)
				{
					current.responses.push_back(MakeResponse(copiedHeader, rejected));
					current.pending.erase(pending);
					--current.reservedResponses;
					return;
				}
			}
			auto result = Cex2Storage::Dispatch(titleId, principal, service, operation, copiedPayload);
			std::lock_guard lock(impl->mutex);
			const auto found = impl->sessions.find(sessionId);
			if (found == impl->sessions.end() || found->second.addressSpaceId != addressSpaceId ||
				found->second.generation != generation) return;
			auto& current = found->second;
			const auto pending = current.pending.find(correlationId);
			if (pending == current.pending.end()) return;
			if (!Impl::HasPermission(current, permission, copiedHeader.serviceId.get(),
				copiedHeader.operation.get())) result = {Status::PermissionDenied};
			if (result.payload.size() > maximumResponse) result = {Status::TooLarge};
			auto response = MakeResponse(copiedHeader, result.status, result.payload);
			current.bytesCopied += response.size();
			current.pending.erase(pending);
			--current.reservedResponses;
			current.responses.push_back(std::move(response));
		});
		return static_cast<std::int32_t>(Error::Ok);
	}
	++session.reservedResponses;
	auto response = m_impl->Dispatch(session, request, payload);
	--session.reservedResponses;
	if (response.size() > cemuextend::transport::kMaximumMessageSize)
		response = MakeResponse(request, Status::TooLarge);
	++session.acceptedRequests;
	session.bytesCopied += requestBytes.size() + response.size();
	session.responses.push_back(std::move(response));
	return static_cast<std::int32_t>(Error::Ok);
}

std::int32_t Cex2Host::Poll(Cex2Owner& owner, std::uint32_t sessionId,
	std::span<std::byte> output, std::uint32_t& outputSize)
{
	outputSize = 0;
	std::lock_guard lock(m_impl->mutex);
	const auto found = m_impl->sessions.find(sessionId);
	if (found == m_impl->sessions.end() || !Impl::Owns(found->second, owner))
		return static_cast<std::int32_t>(Error::PermissionDenied);
	auto& session = found->second;
	for (auto pending = session.pending.begin(); pending != session.pending.end();)
	{
		if (std::chrono::steady_clock::now() < pending->second.deadline) { ++pending; continue; }
		const auto service = pending->second.header.serviceId.get();
		if (service == static_cast<std::uint16_t>(ServiceId::Clipboard)) session.clipboardPending = false;
		if (service == static_cast<std::uint16_t>(ServiceId::Capture)) session.capture = {};
		session.responses.push_back(MakeResponse(pending->second.header, Status::TimedOut));
		pending = session.pending.erase(pending); --session.reservedResponses;
	}
	if (session.responses.empty())
		return static_cast<std::int32_t>(Error::NotFound);
	if (output.size() < session.responses.front().size())
		return static_cast<std::int32_t>(Error::TooLarge);
	outputSize = static_cast<std::uint32_t>(session.responses.front().size());
	std::memcpy(output.data(), session.responses.front().data(), outputSize);
	session.responses.pop_front();
	++session.completedResponses;
	return static_cast<std::int32_t>(Error::Ok);
}

std::int32_t Cex2Host::Cancel(Cex2Owner& owner, std::uint32_t sessionId,
	std::uint32_t correlationId)
{
	if (!correlationId)
		return static_cast<std::int32_t>(Error::InvalidArgument);
	std::lock_guard lock(m_impl->mutex);
	const auto found = m_impl->sessions.find(sessionId);
	if (found == m_impl->sessions.end() || !Impl::Owns(found->second, owner))
		return static_cast<std::int32_t>(Error::PermissionDenied);
	if (const auto pending = found->second.pending.find(correlationId);
		pending != found->second.pending.end())
	{
		const auto service = pending->second.header.serviceId.get();
		if (service == static_cast<std::uint16_t>(ServiceId::Clipboard))
			found->second.clipboardPending = false;
		if (service == static_cast<std::uint16_t>(ServiceId::Capture))
			found->second.capture = {};
		found->second.responses.push_back(MakeResponse(pending->second.header, Status::Cancelled));
		found->second.pending.erase(pending);
		--found->second.reservedResponses;
		return static_cast<std::int32_t>(Error::Ok);
	}
	for (auto& response : found->second.responses)
	{
		ResponseHeader header{};
		std::memcpy(&header, response.data(), sizeof(header));
		if (header.correlationId.get() != correlationId)
			continue;
		header.status = static_cast<std::uint16_t>(Status::Cancelled);
		header.totalSize = sizeof(ResponseHeader);
		response.resize(sizeof(ResponseHeader));
		std::memcpy(response.data(), &header, sizeof(header));
		return static_cast<std::int32_t>(Error::Ok);
	}
	return static_cast<std::int32_t>(Error::NotFound);
}

std::int32_t Cex2Host::Close(Cex2Owner& owner, std::uint32_t sessionId)
{
	std::lock_guard lock(m_impl->mutex);
	const auto found = m_impl->sessions.find(sessionId);
	if (found == m_impl->sessions.end())
		return static_cast<std::int32_t>(Error::NotFound);
	if (!Impl::Owns(found->second, owner))
		return static_cast<std::int32_t>(Error::PermissionDenied);
	const bool hadTextInput = found->second.textInput.active;
	m_impl->sessions.erase(found);
	if (hadTextInput && m_impl->textInputWakeCallback)
		m_impl->textInputWakeCallback();
	return static_cast<std::int32_t>(Error::Ok);
}

void Cex2Host::CloseOwner(Cex2Owner& owner)
{
	std::lock_guard lock(m_impl->mutex);
	bool hadTextInput{};
	std::erase_if(m_impl->sessions, [&owner, &hadTextInput](const auto& entry) {
		const auto& session = entry.second;
		const bool remove = session.owner == &owner &&
			session.addressSpaceId == owner.AddressSpaceId() &&
			session.generation == owner.Generation();
		hadTextInput |= remove && session.textInput.active;
		return remove;
	});
	// Transfers are scoped to the address space, so they outlive one session of
	// it but never the owner that started them.
	Cex2Http::ReleaseSession(owner.AddressSpaceId());
	if (hadTextInput && m_impl->textInputWakeCallback)
		m_impl->textInputWakeCallback();
}

void Cex2Host::CloseAll()
{
	std::lock_guard lock(m_impl->mutex);
	const bool hadTextInput = std::ranges::any_of(m_impl->sessions,
		[](const auto& entry) { return entry.second.textInput.active; });
	for (const auto& entry : m_impl->sessions)
		Cex2Http::ReleaseSession(entry.second.addressSpaceId);
	m_impl->sessions.clear();
	if (hadTextInput && m_impl->textInputWakeCallback)
		m_impl->textInputWakeCallback();
}

#ifdef CEMU_CEX2_TESTING
void Cex2Host::ShutdownForTesting()
{
	// Joining the workers also releases OpenSSL's per-thread state before the
	// short-lived sanitizer test shuts the crypto library down.
	m_impl.reset();
}
#endif

void Cex2Host::ObserveVpad(std::int32_t channel, const VPADStatus& status,
	std::int32_t error, std::int32_t sampleCount)
{
	if (channel < 0 || channel >= 2) return;
	std::lock_guard lock(m_impl->mutex);
	for (auto& [id, session] : m_impl->sessions)
	{
		if (!Impl::HasPermission(session, 1, static_cast<std::uint16_t>(ServiceId::Input)))
			continue;
		cemuextend::wire::ObservedVpadState observed{};
		observed.frameNumber = CurrentFrameNumber();
		observed.sampleError = static_cast<std::uint32_t>(error);
		observed.hold = status.hold; observed.trigger = status.trig; observed.release = status.release;
		auto stick = [](float value) {
			return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
		};
		observed.leftX = stick(status.leftStick.x); observed.leftY = stick(status.leftStick.y);
		observed.rightX = stick(status.rightStick.x); observed.rightY = stick(status.rightStick.y);
		observed.gyroX = status.gyroChange.x; observed.gyroY = status.gyroChange.y;
		observed.gyroZ = status.gyroChange.z;
		observed.touchX = static_cast<float>(status.tpData.x);
		observed.touchY = static_cast<float>(status.tpData.y);
		observed.touched = status.tpData.touch != 0;
		session.observedVpad[channel] = observed;
		session.hasObservedVpad[channel] = sampleCount > 0;
		// A client that owns the pointer is operating in keyboard/mouse mode.
		// Keep the observed VPAD snapshot available for explicit queries, but
		// do not flood its service-wide input subscription with controller
		// events it intentionally disabled. Besides matching the input policy,
		// this keeps mouse button down/up pairs from being displaced by the
		// high-frequency VPAD stream.
		if (sampleCount > 0 &&
			session.pointerPolicy.mode ==
				static_cast<std::uint8_t>(cemuextend::wire::PointerMode::Default))
		{
			cemuextend::wire::ControllerEventPayload event{};
			event.identity.eventId = session.nextInputEventId++;
			event.identity.origin = static_cast<std::uint8_t>(cemuextend::wire::InputOrigin::ObservedVpad);
			event.identity.channel = static_cast<std::uint8_t>(channel == 0 ?
				cemuextend::wire::InputChannel::Vpad0 : cemuextend::wire::InputChannel::Vpad1);
			event.identity.deviceId = static_cast<std::uint16_t>(channel);
			event.identity.frameNumber = static_cast<std::uint32_t>(CurrentFrameNumber());
			event.buttonsLow = status.hold;
			event.buttonsHigh = 0;
			event.leftX = observed.leftX.get(); event.leftY = observed.leftY.get();
			event.rightX = observed.rightX.get(); event.rightY = observed.rightY.get();
			m_impl->EmitEvent(session, ServiceId::Input,
				static_cast<std::uint16_t>(cemuextend::wire::InputEvent::Controller),
				{reinterpret_cast<const std::byte*>(&event), sizeof(event)});
		}
	}
}

void Cex2Host::ApplyMappedVpad(std::int32_t channel, VPADStatus& status)
{
	if (channel < 0 || channel >= 2) return;
	std::lock_guard lock(m_impl->mutex);
	const auto now = std::chrono::steady_clock::now();
	bool replacePhysical{};
	for (auto& [id, session] : m_impl->sessions)
	{
		if (!Impl::HasPermission(session, 4, static_cast<std::uint16_t>(ServiceId::Input),
			static_cast<std::uint16_t>(cemuextend::wire::InputOperation::InjectMapped)) ||
			!session.hasMappedInjection[channel] ||
			now - session.mappedInjectionTime[channel] >
				std::chrono::milliseconds(250))
		{
			session.hasMappedInjection[channel] = false;
			continue;
		}
		const auto flags = session.mappedInjection[channel].flags;
		replacePhysical |= (flags & static_cast<std::uint8_t>(
			cemuextend::wire::MappedInputFlag::ReplacePhysical)) != 0;
	}
	if (replacePhysical)
	{
		// Keep physical touch, gyro, acceleration, and other sensors intact. Only
		// replace the controller-profile state owned by mapped input.
		status.hold = 0;
		status.trig = 0;
		status.release = 0;
		status.leftStick = {};
		status.rightStick = {};
	}
	for (auto& [id, session] : m_impl->sessions)
	{
		if (!session.hasMappedInjection[channel]) continue;
		auto& injected = session.mappedInjection[channel];
		status.hold |= injected.hold.get(); status.trig |= injected.trigger.get();
		status.release |= injected.release.get();
		status.leftStick.x = injected.leftX.get(); status.leftStick.y = injected.leftY.get();
		status.rightStick.x = injected.rightX.get(); status.rightStick.y = injected.rightY.get();
		injected.trigger = 0; injected.release = 0;
	}
}

void Cex2Host::KeyboardEvent(std::uint16_t usage, bool pressed, std::uint8_t modifiers)
{
	if (!usage) return;
	std::lock_guard lock(m_impl->mutex);
	for (auto& [id, session] : m_impl->sessions)
	{
		if (!Impl::HasPermission(session, 1, static_cast<std::uint16_t>(ServiceId::Input))) continue;
		if (pressed) session.pressedKeyboardUsages.insert(usage);
		else session.pressedKeyboardUsages.erase(usage);
		cemuextend::wire::KeyboardEventPayload event{};
		event.identity.eventId = session.nextInputEventId++;
		event.identity.origin = static_cast<std::uint8_t>(cemuextend::wire::InputOrigin::Physical);
		event.identity.channel = static_cast<std::uint8_t>(cemuextend::wire::InputChannel::Keyboard);
		event.identity.frameNumber = CurrentFrameNumber();
		event.usbHidUsage = usage; event.pressed = pressed; event.modifiers = modifiers;
		m_impl->EmitEvent(session, ServiceId::Input,
			static_cast<std::uint16_t>(cemuextend::wire::InputEvent::Keyboard),
			{reinterpret_cast<const std::byte*>(&event), sizeof(event)});
	}
}

void Cex2Host::KeyboardFocusLost()
{
	std::lock_guard lock(m_impl->mutex);
	for (auto& [id, session] : m_impl->sessions)
	{
		for (const auto usage : session.pressedKeyboardUsages)
		{
			cemuextend::wire::KeyboardEventPayload event{};
			event.identity.eventId = session.nextInputEventId++;
			event.identity.origin = static_cast<std::uint8_t>(cemuextend::wire::InputOrigin::Physical);
			event.identity.channel = static_cast<std::uint8_t>(cemuextend::wire::InputChannel::Keyboard);
			event.identity.frameNumber = CurrentFrameNumber();
			event.usbHidUsage = usage;
			m_impl->EmitEvent(session, ServiceId::Input,
				static_cast<std::uint16_t>(cemuextend::wire::InputEvent::Keyboard),
				{reinterpret_cast<const std::byte*>(&event), sizeof(event)});
		}
		session.pressedKeyboardUsages.clear();
	}
}

void Cex2Host::TextEvent(std::uint32_t codepoint, bool repeat)
{
	if (codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU))
		return;
	std::lock_guard lock(m_impl->mutex);
	for (auto& [id, session] : m_impl->sessions)
	{
		if (!Impl::HasPermission(session, 1, static_cast<std::uint16_t>(ServiceId::Input)))
			continue;
		cemuextend::wire::TextEventPayload event{};
		event.identity.eventId = session.nextInputEventId++;
		event.identity.origin = static_cast<std::uint8_t>(cemuextend::wire::InputOrigin::Physical);
		event.identity.channel = static_cast<std::uint8_t>(cemuextend::wire::InputChannel::Keyboard);
		event.identity.frameNumber = CurrentFrameNumber();
		event.codepoint = codepoint;
		event.repeat = repeat;
		m_impl->EmitEvent(session, ServiceId::Input,
			static_cast<std::uint16_t>(cemuextend::wire::InputEvent::Text),
			{reinterpret_cast<const std::byte*>(&event), sizeof(event)});
	}
}

Cex2HostTextInputState Cex2Host::EffectiveTextInput()
{
	std::lock_guard lock(m_impl->mutex);
	Cex2HostTextInputState result{};
	for (const auto& [id, session] : m_impl->sessions)
	{
		if (session.textInput.active && session.textInput.sequence > result.sequence &&
			Impl::HasPermission(session, 1,
				static_cast<std::uint16_t>(cemuextend::wire::ServiceId::Input),
				static_cast<std::uint16_t>(cemuextend::wire::InputOperation::SetTextInput)))
			result = session.textInput;
	}
	return result;
}

void Cex2Host::TextCompositionEvent(std::string_view text,
	std::string_view preedit, std::uint32_t preeditStart,
	std::uint32_t preeditCursor)
{
	using namespace cemuextend::wire;
	std::lock_guard lock(m_impl->mutex);
	Impl::Session* target{};
	for (auto& [id, session] : m_impl->sessions)
		if (session.textInput.active &&
			Impl::HasPermission(session, 1,
				static_cast<std::uint16_t>(ServiceId::Input),
				static_cast<std::uint16_t>(InputOperation::SetTextInput)) &&
			(target == nullptr || session.textInput.sequence > target->textInput.sequence))
			target = &session;
	if (target == nullptr || text.size() + preedit.size() > 4096 ||
		preeditStart > text.size() || preeditCursor > preedit.size() ||
		!Impl::IsValidUtf8(text) || !Impl::IsValidUtf8(preedit)) return;

	TextCompositionEventHeader event{};
	event.identity.eventId = target->nextInputEventId++;
	event.identity.origin = static_cast<std::uint8_t>(InputOrigin::Physical);
	event.identity.channel = static_cast<std::uint8_t>(InputChannel::Keyboard);
	event.identity.frameNumber = CurrentFrameNumber();
	event.requestId = target->textInput.requestId;
	event.revision = static_cast<std::uint32_t>(event.identity.eventId.get());
	event.committedBytes = static_cast<std::uint32_t>(text.size());
	event.preeditBytes = static_cast<std::uint32_t>(preedit.size());
	event.preeditStart = preeditStart;
	event.preeditCursor = preeditCursor;
	event.selectionStart = static_cast<std::uint32_t>(text.size());
	event.selectionEnd = static_cast<std::uint32_t>(text.size());
	event.flags = static_cast<std::uint8_t>(TextInputFlag::Active);
	std::vector<std::byte> payload(sizeof(event) + text.size() + preedit.size());
	std::memcpy(payload.data(), &event, sizeof(event));
	if (!text.empty()) std::memcpy(payload.data() + sizeof(event), text.data(), text.size());
	if (!preedit.empty())
		std::memcpy(payload.data() + sizeof(event) + text.size(),
			preedit.data(), preedit.size());
	m_impl->EmitEvent(*target, ServiceId::Input,
		static_cast<std::uint16_t>(InputEvent::TextComposition), payload);
}

void Cex2Host::SetTextInputWakeCallback(void (*callback)())
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->textInputWakeCallback = callback;
}

void Cex2Host::MouseEvent(cemuextend::wire::PointerSurface surface,
	std::int32_t x, std::int32_t y, std::int32_t deltaX, std::int32_t deltaY,
	std::int32_t wheelX, std::int32_t wheelY, std::uint32_t buttons,
	std::uint32_t changedButtons, std::int32_t contentWidth,
	std::int32_t contentHeight, bool insideContent, bool focused, std::uint8_t flags)
{
	using namespace cemuextend::wire;
	MouseEventPayloadV2 state{};
	state.identity.origin = static_cast<std::uint8_t>(InputOrigin::Physical);
	state.identity.channel = static_cast<std::uint8_t>(InputChannel::Mouse);
	state.identity.deviceId = static_cast<std::uint16_t>(surface);
	state.identity.frameNumber = static_cast<std::uint32_t>(CurrentFrameNumber());
	state.x = x;
	state.y = y;
	state.deltaX = deltaX;
	state.deltaY = deltaY;
	state.wheelX = wheelX;
	state.wheelY = wheelY;
	state.buttons = buttons;
	state.changedButtons = changedButtons;
	state.contentWidth = std::max(0, contentWidth);
	state.contentHeight = std::max(0, contentHeight);
	state.normalizedX = contentWidth > 0 ?
		std::clamp(static_cast<float>(x) / static_cast<float>(contentWidth), 0.0f, 1.0f) : 0.0f;
	state.normalizedY = contentHeight > 0 ?
		std::clamp(static_cast<float>(y) / static_cast<float>(contentHeight), 0.0f, 1.0f) : 0.0f;
	state.surface = static_cast<std::uint8_t>(surface);
	state.insideContent = insideContent;
	state.focused = focused;
	state.flags = flags;
	std::lock_guard lock(m_impl->mutex);
	m_impl->EmitMouseEventLocked(state);
}

void Cex2Host::PointerFocusChanged(bool focused)
{
	std::lock_guard lock(m_impl->mutex);
	auto state = m_impl->hostMouse;
	state.identity.frameNumber = static_cast<std::uint32_t>(CurrentFrameNumber());
	state.deltaX = 0;
	state.deltaY = 0;
	state.wheelX = 0;
	state.wheelY = 0;
	state.focused = focused;
	if (!focused)
	{
		state.insideContent = 0;
		state.changedButtons = state.buttons.get();
		state.buttons = 0;
		state.flags = 0;
	}
	else
	{
		state.changedButtons = 0;
	}
	m_impl->EmitMouseEventLocked(state);
}

cemuextend::wire::PointerPolicyPayload Cex2Host::EffectivePointerPolicy()
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->EffectivePointerPolicyLocked();
}

void Cex2Host::PermissionsChanged(Cex2Owner& owner, std::uint32_t permissions)
{
	std::lock_guard lock(m_impl->mutex);
	bool hadTextInput{};
	for (const auto& [id, session] : m_impl->sessions)
		hadTextInput |= session.owner == &owner && session.textInput.active;
	owner.SetGrantedPermissions(permissions);
	for (auto& [id, session] : m_impl->sessions)
	{
		if (session.owner != &owner) continue;
		std::erase_if(session.subscriptions, [&session](std::uint16_t service) {
			return service != static_cast<std::uint16_t>(ServiceId::Core) &&
				!Impl::HasPermission(session, 1, service);
		});
		for (auto response = session.responses.begin(); response != session.responses.end();)
		{
			ResponseHeader header{};
			std::memcpy(&header, response->data(), sizeof(header));
			const bool event = header.flags.get() == static_cast<std::uint16_t>(
				cemuextend::transport::ResponseFlag::Event);
			const auto* definition = FindOperation(header.serviceId.get(), header.operation.get());
			const auto required = event ? 1U : definition ? definition->permission : 0U;
			if (Impl::HasPermission(session, required, header.serviceId.get(), header.operation.get()))
			{
				++response;
				continue;
			}
			if (event)
			{
				response = session.responses.erase(response);
				continue;
			}
			RequestHeader request{};
			request.correlationId = header.correlationId;
			request.serviceId = header.serviceId;
			request.operation = header.operation;
			*response = MakeResponse(request, Status::PermissionDenied);
			++response;
		}
		if (!Impl::HasPermission(session, 1, static_cast<std::uint16_t>(ServiceId::Input)))
		{
			session.observedVpad = {};
			session.hasObservedVpad.fill(false);
			session.pressedKeyboardUsages.clear();
		}
		if (!Impl::HasPermission(session, 4, static_cast<std::uint16_t>(ServiceId::Input)))
			session.hasMappedInjection.fill(false);
		if (!Impl::HasPermission(session, 4, static_cast<std::uint16_t>(ServiceId::Window),
			static_cast<std::uint16_t>(cemuextend::wire::WindowOperation::SetPointerPolicy)))
			session.pointerPolicy = {};
		if (!Impl::HasPermission(session, 16, static_cast<std::uint16_t>(ServiceId::Capture)))
			session.capture = {};
		for (auto pending = session.pending.begin(); pending != session.pending.end();)
		{
			if (Impl::HasPermission(session, pending->second.permission,
				pending->second.header.serviceId.get(), pending->second.header.operation.get()))
			{ ++pending; continue; }
			session.responses.push_back(MakeResponse(pending->second.header, Status::PermissionDenied));
			const auto service = pending->second.header.serviceId.get();
			if (service == static_cast<std::uint16_t>(ServiceId::Clipboard)) session.clipboardPending = false;
			if (service == static_cast<std::uint16_t>(ServiceId::Capture)) session.capture = {};
			pending = session.pending.erase(pending);
			--session.reservedResponses;
		}
	}
	if (hadTextInput && m_impl->textInputWakeCallback)
		m_impl->textInputWakeCallback();
}

} // namespace cemuextend_hle
