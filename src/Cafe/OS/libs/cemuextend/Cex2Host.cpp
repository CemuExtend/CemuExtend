#include "Common/precompiled.h"

#include "Cafe/OS/libs/cemuextend/Cex2Host.h"
#include "Cafe/OS/libs/cemuextend/CemodPermission.h"
#include "Cafe/OS/libs/cemuextend/Cex2Owner.h"
#include "Cafe/OS/libs/cemuextend/Cex2Http.h"
#include "Cafe/OS/libs/cemuextend/Cex2Storage.h"
#include "Cafe/OS/libs/cemuextend/CemodWebUiHost.h"
#include "host/contracts/HostContracts.h"

#include "Cafe/HW/Espresso/ModExecutionContext.h"
#ifndef CEMU_CEX2_TESTING
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LatteTiming.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cemu/Logging/CemuLogging.h"
#endif
#include "Cafe/OS/libs/cemuextend/BuildId.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "cemuextend/services.hpp"
#include "cemuextend/transport.hpp"

#include <openssl/crypto.h>
#include <rapidjson/document.h>

#include <deque>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <thread>

namespace cemuextend_hle
{
	namespace
	{

		std::uint64_t CurrentFrameNumber()
		{
#ifdef CEMU_CEX2_TESTING
			return 0;
#else
			return LatteGPUState.frameCounter;
#endif
		}

		cemuextend::wire::GraphicsApi CurrentGraphicsApi()
		{
#ifdef CEMU_CEX2_TESTING
			return cemuextend::wire::GraphicsApi::Unknown;
#else
			if (!g_renderer)
				return cemuextend::wire::GraphicsApi::Unknown;
			switch (g_renderer->GetType())
			{
			case RendererAPI::OpenGL:
				return cemuextend::wire::GraphicsApi::OpenGL;
			case RendererAPI::Vulkan:
				return cemuextend::wire::GraphicsApi::Vulkan;
			case RendererAPI::Metal:
				return cemuextend::wire::GraphicsApi::Metal;
			default:
				return cemuextend::wire::GraphicsApi::Unknown;
			}
#endif
		}

#ifdef CEMU_CEX2_TESTING
		sint32 g_testGuestFrameRate{-1};
		void SetGuestFrameRate(sint32 frequency)
		{
			g_testGuestFrameRate = frequency;
		}
		void ClearGuestFrameRate()
		{
			g_testGuestFrameRate = -1;
		}
		sint32 EffectiveFrameRate()
		{
			return g_testGuestFrameRate > 0 ? g_testGuestFrameRate : 60;
		}
#else
		void SetGuestFrameRate(sint32 frequency)
		{
			LatteTiming_setGuestCustomVsyncFrequency(frequency);
		}
		void ClearGuestFrameRate()
		{
			LatteTiming_disableGuestCustomVsyncFrequency();
		}
		sint32 EffectiveFrameRate()
		{
			return LatteTiming_getEffectiveVsyncFrequency();
		}
#endif

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
			Ui,
			Timing,
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
			OperationDefinition{1, 1, 1, 0, 0, 4096, 0, 0, Handler::Core},
			OperationDefinition{1, 2, 1, 0, 8, 8, 0, 0, Handler::Core},
			OperationDefinition{1, 3, 1, 0, 0, 32, 0, 0, Handler::Core},
			OperationDefinition{1, 4, 1, 0, 2, 0, 0, 0, Handler::Core},
			OperationDefinition{1, 5, 1, 0, 2, 0, 0, 0, Handler::Core},
			OperationDefinition{1, 6, 1, 0, 0, 128, 0, 0, Handler::Core},
			OperationDefinition{2, 1, 1, 4, sizeof(cemuextend::wire::ControllerEventPayload), 0, 0, 0, Handler::Input},
			OperationDefinition{2, 2, 1, 4, 1 + sizeof(cemuextend::wire::ObservedVpadState), 0, 0, 0, Handler::Input},
			OperationDefinition{2, 3, 1, 1, 1, sizeof(cemuextend::wire::ObservedVpadState), 0, 0, Handler::Input},
			OperationDefinition{2, 4, 1, 1, 0, sizeof(cemuextend::wire::MouseEventPayloadV2), 0, 0, Handler::Input},
			OperationDefinition{2, 5, 1, 1, sizeof(cemuextend::wire::TextInputRequestHeader) + 4096, 0, 0, 0, Handler::Input},
			OperationDefinition{2, 6, 1, 1, sizeof(cemuextend::wire::InputReadRequestV3), 65520, 0, 0, Handler::Input},
			OperationDefinition{2, 7, 1, 1, 65520, 0, 0, 0, Handler::Input},
			OperationDefinition{3, 1, 1, 2, 4096 + 5, 0, 20, 50, Handler::Logging},
			OperationDefinition{4, 1, 1, 1, 260, 65520, 0, 0, Handler::Configuration},
			OperationDefinition{4, 2, 1, 2, 65520, 0, 0, 0, Handler::Configuration},
			OperationDefinition{4, 3, 1, 2, 260, 0, 0, 0, Handler::Configuration},
			OperationDefinition{4, 4, 1, 1, 65520, 65520, 0, 0, Handler::Configuration},
			OperationDefinition{5, 1, 1, 1, 4096, 64, 0, 0, Handler::File},
			OperationDefinition{5, 2, 1, 1, 4096, 65520, 0, 0, Handler::File},
			OperationDefinition{5, 3, 1, 1, 4096, 65520, 0, 0, Handler::File},
			OperationDefinition{5, 4, 1, 2, 65520, 0, 0, 0, Handler::File},
			OperationDefinition{5, 5, 1, 2, 4096, 0, 0, 0, Handler::File},
			OperationDefinition{5, 6, 1, 2, 4096, 0, 0, 0, Handler::File},
			OperationDefinition{5, 7, 1, 2, 8192, 0, 0, 0, Handler::File},
			OperationDefinition{6, 1, 1, 8, 0, 65520, 0, 0, Handler::Clipboard},
			OperationDefinition{6, 2, 1, 8, 65520, 0, 0, 0, Handler::Clipboard},
			OperationDefinition{7, 1, 1, 1, 0, sizeof(cemuextend::wire::WindowStatePayload), 0, 0, Handler::Window},
			OperationDefinition{7, 2, 1, 4, sizeof(cemuextend::wire::PointerPolicyPayload), sizeof(cemuextend::wire::PointerPolicyPayload), 0, 0, Handler::Window},
			OperationDefinition{7, 3, 1, 1, 0, sizeof(cemuextend::wire::PointerPolicyPayload), 0, 0, Handler::Window},
			OperationDefinition{8, 1, 1, 16, 1, 64, 0, 0, Handler::Capture},
			OperationDefinition{8, 2, 1, 16, 8, 65520, 0, 0, Handler::Capture},
			OperationDefinition{8, 3, 1, 16, 4, 0, 0, 0, Handler::Capture},
			OperationDefinition{9, 1, 1, 1, 0, sizeof(cemuextend::wire::DiagnosticsPayload), 0, 0, Handler::Diagnostics},
			OperationDefinition{10, 1, 1, kCemodNetworkPermission, sizeof(cemuextend::wire::HttpStartRequest) + 2048, sizeof(cemuextend::wire::HttpStartResponse), 0, 0, Handler::Http},
			OperationDefinition{10, 2, 1, kCemodNetworkPermission, sizeof(cemuextend::wire::HttpPollRequest), 65520, 0, 0, Handler::Http},
			OperationDefinition{10, 3, 1, kCemodNetworkPermission, 4, 0, 0, 0, Handler::Http},
			OperationDefinition{11, 1, 1, kCemodUiPermission, sizeof(cemuextend::wire::UiCreateRequestHeader) + cemuextend::wire::kMaximumUiNameBytes + cemuextend::wire::kMaximumUiContextBytes, sizeof(cemuextend::wire::UiCreateResponse), 4, 8, Handler::Ui},
			OperationDefinition{11, 2, 1, kCemodUiPermission, sizeof(cemuextend::wire::UiHandleRequest), 0, 20, 40, Handler::Ui},
			OperationDefinition{11, 3, 1, kCemodUiPermission, sizeof(cemuextend::wire::UiMessageHeader) + cemuextend::wire::kMaximumUiNameBytes + cemuextend::wire::kMaximumUiJsonBytes, 0, 100, 200, Handler::Ui},
			OperationDefinition{11, 4, 1, kCemodUiPermission, sizeof(cemuextend::wire::UiReplyHeader) + cemuextend::wire::kMaximumUiJsonBytes, 0, 100, 200, Handler::Ui},
			OperationDefinition{11, 5, 1, kCemodUiPermission, sizeof(cemuextend::wire::UiVisibleRequest), 0, 30, 60, Handler::Ui},
			OperationDefinition{11, 6, 1, kCemodUiPermission, sizeof(cemuextend::wire::UiBoundsRequest), 0, 30, 60, Handler::Ui},
			OperationDefinition{11, 7, 1, kCemodUiPermission, sizeof(cemuextend::wire::UiHandleRequest), 0, 30, 60, Handler::Ui},
			OperationDefinition{11, 8, 1, kCemodUiPermission, sizeof(cemuextend::wire::UiTitleRequestHeader) + 256, 0, 20, 40, Handler::Ui},
			OperationDefinition{11, 9, 1, kCemodUiPermission, sizeof(cemuextend::wire::UiInteractiveRequest), 0, 30, 60, Handler::Ui},
			OperationDefinition{12, 1, 1, 1, 0, sizeof(cemuextend::wire::TimingFrameRatePayload), 10, 20, Handler::Timing},
			OperationDefinition{12, 2, 1, 2, sizeof(cemuextend::wire::TimingFrameRatePayload), sizeof(cemuextend::wire::TimingFrameRatePayload), 10, 20, Handler::Timing},
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
			ServiceDefinition{2, 3, 7, 64U * 1024U, 64U * 1024U},
			ServiceDefinition{3, 1, 2, 4U * 1024U, 64},
			ServiceDefinition{4, 1, 3, 64U * 1024U, 64U * 1024U},
			ServiceDefinition{5, 1, 3, 64U * 1024U, 64U * 1024U},
			ServiceDefinition{6, 1, 8, 64U * 1024U, 64U * 1024U},
			ServiceDefinition{7, 1, 1, 64, 256},
			ServiceDefinition{8, 1, 16, 64, 64U * 1024U},
			ServiceDefinition{9, 1, 1, 64, 4U * 1024U},
			ServiceDefinition{10, 1, 32, 64U * 1024U, 64U * 1024U},
			ServiceDefinition{11, 1, kCemodUiPermission, 64U * 1024U, 64U * 1024U},
			ServiceDefinition{12, 1, 3, sizeof(cemuextend::wire::TimingFrameRatePayload), sizeof(cemuextend::wire::TimingFrameRatePayload)},
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
			std::uint32_t id{};
			std::uint64_t addressSpaceId{};
			std::uint32_t generation{};
			std::uint16_t negotiatedMajor{cemuextend::transport::kAbiMajor};
			std::uint64_t inputGeneration{};
			std::uint64_t inputAcknowledgedSequence{};
			bool inputBaselineDelivered{};
			bool inputOverflow{};
			std::set<std::pair<std::uint16_t, std::uint16_t>> inputBaselineControls;
			std::array<cemuextend::wire::InputSurfaceStateV3, 2> inputBaselineSurfaces{};
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
			std::shared_ptr<const CemodWebUiContent> webUiContent;
			// Compact exact-once admission history. Sequential IDs occupy one range;
			// pathological sparse IDs are bounded and reap the session.
			std::map<std::uint32_t, std::uint32_t> admittedRanges;
			double loggingTokens{50.0};
			std::chrono::steady_clock::time_point loggingLastRefill{std::chrono::steady_clock::now()};
			std::set<std::uint16_t> pressedKeyboardUsages;
			sint32 frameRate{-1};
			std::uint64_t frameRateSequence{};
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
		std::shared_ptr<Host::IClipboard> clipboard;
		std::shared_ptr<Host::IWindowMetrics> windowMetrics;
		std::shared_ptr<ICemodWebUiHost> webUi;
		std::map<std::pair<std::uint64_t, std::uint32_t>,
				 std::shared_ptr<const CemodWebUiContent>>
			webUiContents;
		std::uint64_t nextTextInputSequence{1};
		std::uint64_t nextTextTransferId{1};
		std::function<void()> textInputWakeCallback;
		std::unordered_map<std::uint32_t, Session> sessions;
		std::uint32_t nextSession{1};
		std::uint64_t nextPointerPolicySequence{1};
		std::uint64_t nextFrameRateSequence{1};
		cemuextend::wire::MouseEventPayloadV2 hostMouse{};
		struct InputJournalRecord
		{
			cemuextend::wire::InputRecordV3 header{};
			std::vector<std::byte> payload;
			std::uint32_t targetSession{};
		};
		static constexpr std::size_t MaximumInputJournalBytes = 16U * 1024U * 1024U;
		std::deque<InputJournalRecord> inputJournal;
		std::size_t inputJournalBytes{};
		std::uint64_t inputGeneration{1};
		std::uint64_t nextInputSequence{1};
		std::uint64_t lastDeliveredInputSequence{};
		std::set<std::pair<std::uint16_t, std::uint16_t>> hostPressedKeyboardUsages;
		std::set<std::tuple<std::uint16_t, std::uint16_t, std::uint16_t>>
			hostPressedKeyboardDevices;
		std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint32_t>
			hostPressedKeyboardRefCounts;
		std::array<cemuextend::wire::InputSurfaceStateV3, 2> inputSurfaces{};
		std::map<std::pair<std::uint16_t, std::uint8_t>, std::uint32_t>
			hostPointerDeviceButtons;
		std::array<cemuextend::wire::InputOwnershipV3, 2> inputOwnership{};
		std::uint32_t nextInputOwnershipEpoch{1};
		std::mutex workMutex;
		std::condition_variable workReady;
		std::deque<std::function<void()>> work;
		bool stopping{};
		std::array<std::thread, 2> workers;

		Impl()
		{
			for (auto& ownership : inputOwnership)
			{
				ownership.keyboardOwner = static_cast<std::uint8_t>(cemuextend::wire::InputOwner::Title);
				ownership.pointerOwner = static_cast<std::uint8_t>(cemuextend::wire::InputOwner::Title);
				ownership.textOwner = static_cast<std::uint8_t>(cemuextend::wire::InputOwner::Title);
			}
			hostMouse.focused = 1;
			inputSurfaces[0].surface = static_cast<std::uint8_t>(cemuextend::wire::PointerSurface::Tv);
			inputSurfaces[1].surface = static_cast<std::uint8_t>(cemuextend::wire::PointerSurface::Drc);
			inputSurfaces[0].focused = 1;
			inputSurfaces[1].focused = 1;
			for (auto& worker : workers)
				worker = std::thread([this] {
					for (;;)
					{
						std::function<void()> task;
						{
							std::unique_lock lock(workMutex);
							workReady.wait(lock, [this] { return stopping || !work.empty(); });
							if (stopping && work.empty())
								break;
							task = std::move(work.front());
							work.pop_front();
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
			StopWorkers();
		}

		void StopWorkers()
		{
			{
				std::lock_guard lock(workMutex);
				stopping = true;
			}
			workReady.notify_all();
			for (auto& worker : workers)
				if (worker.joinable())
					worker.join();
		}

		void Enqueue(std::function<void()> task)
		{
			{
				std::lock_guard lock(workMutex);
				work.push_back(std::move(task));
			}
			workReady.notify_one();
		}

		void RefreshGuestFrameRateLocked()
		{
			const Session* selected{};
			for (const auto& [id, session] : sessions)
			{
				(void)id;
				if (session.frameRate > 0 &&
					(selected == nullptr || session.frameRateSequence > selected->frameRateSequence))
					selected = &session;
			}
			if (selected != nullptr)
				SetGuestFrameRate(selected->frameRate);
			else
				ClearGuestFrameRate();
		}

		void QueueTextInputWakeLocked()
		{
			if (!textInputWakeCallback)
				return;
			auto callback = textInputWakeCallback;
			Enqueue([callback = std::move(callback)] { callback(); });
		}

		void Complete(std::uint32_t sessionId, std::uint64_t addressSpaceId, std::uint32_t generation,
					  std::uint32_t correlationId, Status status, std::span<const std::byte> payload = {})
		{
			std::lock_guard lock(mutex);
			const auto found = sessions.find(sessionId);
			if (found == sessions.end() || found->second.addressSpaceId != addressSpaceId ||
				found->second.generation != generation)
				return;
			auto& session = found->second;
			const auto pending = session.pending.find(correlationId);
			if (pending == session.pending.end())
				return;
			if (!HasPermission(session, pending->second.permission,
							   pending->second.header.serviceId.get(), pending->second.header.operation.get()))
				status = Status::PermissionDenied;
			const auto service = pending->second.header.serviceId.get();
			if (service == static_cast<std::uint16_t>(ServiceId::Clipboard))
				session.clipboardPending = false;
			if (service == static_cast<std::uint16_t>(ServiceId::Capture) && status != Status::Ok)
				session.capture = {};
			auto response = MakeResponse(pending->second.header, status, payload);
			session.bytesCopied += response.size();
			session.responses.push_back(std::move(response));
			session.pending.erase(pending);
			--session.reservedResponses;
		}

		void PushUiEvent(CemodWebUiHostEvent event)
		{
			std::lock_guard lock(mutex);
			const auto found = sessions.find(event.sessionId);
			if (found == sessions.end() || found->second.addressSpaceId != event.addressSpaceId ||
				found->second.generation != event.generation)
				return;
			auto& session = found->second;
			if (!HasPermission(session, kCemodUiPermission,
							   static_cast<std::uint16_t>(ServiceId::Ui)) ||
				!session.subscriptions.contains(static_cast<std::uint16_t>(ServiceId::Ui)) ||
				event.payload.size() > cemuextend::transport::kMaximumMessageSize - sizeof(ResponseHeader))
				return;
			EmitEvent(session, ServiceId::Ui, static_cast<std::uint16_t>(event.event), event.payload);
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
			const bool uiService =
				service == static_cast<std::uint16_t>(cemuextend::wire::ServiceId::Ui) &&
				permission == kCemodUiPermission;
			return granted && (service == 0 || networkService || uiService ||
							   session.owner->IsServiceAllowed(service, permission, operation));
		}

		static std::uint32_t EventPermission(std::uint16_t service)
		{
			return service == static_cast<std::uint16_t>(ServiceId::Ui)
					   ? kCemodUiPermission
					   : 1U;
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
				if (joinsNext)
					session.admittedRanges.erase(next);
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
				if (lead < 0x80)
				{
					++index;
					continue;
				}
				std::size_t count = (lead & 0xe0) == 0xc0 ? 1 : (lead & 0xf0) == 0xe0 ? 2
															: (lead & 0xf8) == 0xf0	  ? 3
																					  : 0;
				if (!count || index + count >= text.size())
					return false;
				std::uint32_t codepoint = lead & (0x7fU >> count);
				for (std::size_t offset = 1; offset <= count; ++offset)
				{
					const auto next = static_cast<std::uint8_t>(text[index + offset]);
					if ((next & 0xc0) != 0x80)
						return false;
					codepoint = (codepoint << 6) | (next & 0x3f);
				}
				if ((count == 1 && codepoint < 0x80) || (count == 2 && codepoint < 0x800) ||
					(count == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
					(codepoint >= 0xd800 && codepoint <= 0xdfff))
					return false;
				index += count + 1;
			}
			return true;
		}

		static bool IsValidUiName(std::string_view value)
		{
			return !value.empty() && value.size() <= cemuextend::wire::kMaximumUiNameBytes &&
				   !value.starts_with("cemu.") &&
				   std::ranges::all_of(value, [](unsigned char character) {
					   return std::isalnum(character) || character == '_' || character == '.' ||
							  character == '-';
				   });
		}

		static bool IsValidJson(std::string_view value, std::size_t maximum)
		{
			if (value.empty() || value.size() > maximum || !IsValidUtf8(value))
				return false;
			rapidjson::Document document;
			document.Parse(value.data(), value.size());
			return !document.HasParseError();
		}

		[[nodiscard]] static std::uint64_t MonotonicTimeNs()
		{
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
												  std::chrono::steady_clock::now().time_since_epoch())
												  .count());
		}

		void TrimInputJournalLocked()
		{
			std::uint64_t minimumAcknowledged = nextInputSequence - 1;
			bool hasReader{};
			for (const auto& [id, session] : sessions)
			{
				if (session.negotiatedMajor < 3 || session.inputGeneration != inputGeneration)
					continue;
				hasReader = true;
				minimumAcknowledged = std::min(minimumAcknowledged,
											   session.inputAcknowledgedSequence);
			}
			if (!hasReader)
				minimumAcknowledged = nextInputSequence - 1;
			while (!inputJournal.empty() &&
				   inputJournal.front().header.sequence.get() <= minimumAcknowledged)
			{
				inputJournalBytes -= sizeof(cemuextend::wire::InputRecordV3) +
									 inputJournal.front().payload.size();
				inputJournal.pop_front();
			}
			while (inputJournalBytes > MaximumInputJournalBytes && !inputJournal.empty())
			{
				const auto evicted = inputJournal.front().header.sequence.get();
				for (auto& [id, session] : sessions)
				{
					if (session.negotiatedMajor >= 3 &&
						session.inputGeneration == inputGeneration &&
						session.inputAcknowledgedSequence < evicted)
					{
						session.inputOverflow = true;
						session.inputAcknowledgedSequence = evicted;
					}
				}
				inputJournalBytes -= sizeof(cemuextend::wire::InputRecordV3) +
									 inputJournal.front().payload.size();
				inputJournal.pop_front();
			}
		}

		void AppendInputRecordLocked(cemuextend::wire::InputRecordType type,
									 std::uint16_t usagePage = 0, std::uint16_t usage = 0,
									 std::int32_t value = 0, std::int32_t auxiliary = 0,
									 std::uint8_t flags = 0,
									 std::span<const std::byte> payload = {},
									 std::uint32_t targetSession = 0,
									 std::uint16_t deviceId = 0)
		{
			const bool hasV3Reader = std::ranges::any_of(sessions, [](const auto& item) {
				return item.second.negotiatedMajor >= 3;
			});
			if (!hasV3Reader)
				return;
			InputJournalRecord record{};
			record.header.sequence = nextInputSequence++;
			record.header.timestampNs = MonotonicTimeNs();
			record.header.type = static_cast<std::uint8_t>(type);
			record.header.flags = flags;
			record.header.usagePage = usagePage;
			record.header.usage = usage;
			record.header.deviceId = deviceId;
			record.header.value = value;
			record.header.auxiliary = auxiliary;
			record.header.payloadBytes = static_cast<std::uint32_t>(payload.size());
			record.payload.assign(payload.begin(), payload.end());
			record.targetSession = targetSession;
			inputJournalBytes += sizeof(record.header) + record.payload.size();
			inputJournal.push_back(std::move(record));
			TrimInputJournalLocked();
		}

		void AppendPointerMotionLocked(const cemuextend::wire::MouseEventPayloadV2& state)
		{
			using namespace cemuextend::wire;
			const auto surface = static_cast<PointerSurface>(state.surface);
			const std::size_t surfaceIndex = surface == PointerSurface::Drc ? 1U : 0U;
			InputPointerMotionV3 motion{};
			motion.x = state.x.get();
			motion.y = state.y.get();
			motion.deltaX = state.deltaX.get();
			motion.deltaY = state.deltaY.get();
			motion.contentWidth = state.contentWidth.get();
			motion.contentHeight = state.contentHeight.get();
			motion.surface = state.surface;
			motion.insideContent = state.insideContent;
			motion.focused = state.focused;
			motion.rawRelative =
				(state.flags & static_cast<std::uint8_t>(MouseEventFlag::RawRelative)) != 0;
			const auto epoch = inputOwnership[surfaceIndex].epoch.get();
			const auto deviceId = state.identity.deviceId.get();
			if (!inputJournal.empty())
			{
				auto& previous = inputJournal.back();
				if (previous.header.sequence.get() > lastDeliveredInputSequence &&
					previous.targetSession == 0 &&
					previous.header.type == static_cast<std::uint8_t>(InputRecordType::PointerMotion) &&
					previous.header.usage.get() == static_cast<std::uint16_t>(surface) &&
					previous.header.deviceId.get() == deviceId &&
					static_cast<std::uint32_t>(previous.header.value.get()) == epoch &&
					previous.header.auxiliary.get() < 64 &&
					previous.payload.size() == sizeof(InputPointerMotionV3))
				{
					InputPointerMotionV3 old{};
					std::memcpy(&old, previous.payload.data(), sizeof(old));
					motion.deltaX = SaturatingAdd(old.deltaX.get(), motion.deltaX.get());
					motion.deltaY = SaturatingAdd(old.deltaY.get(), motion.deltaY.get());
					std::memcpy(previous.payload.data(), &motion, sizeof(motion));
					previous.header.auxiliary = previous.header.auxiliary.get() + 1;
					return;
				}
			}
			AppendInputRecordLocked(InputRecordType::PointerMotion, 0,
									static_cast<std::uint16_t>(surface), static_cast<std::int32_t>(epoch), 1, 0,
									{reinterpret_cast<const std::byte*>(&motion), sizeof(motion)}, 0, deviceId);
		}

		void AppendInputBaselineRecordsLocked(Session& session)
		{
			using namespace cemuextend::wire;
			for (std::size_t index = 0; index < inputOwnership.size(); ++index)
			{
				auto& current = inputOwnership[index];
				if (current.epoch.get() == 0)
				{
					if (nextInputOwnershipEpoch == 0)
						nextInputOwnershipEpoch = 1;
					current.epoch = nextInputOwnershipEpoch++;
				}
				AppendInputRecordLocked(InputRecordType::Ownership, 0,
										static_cast<std::uint16_t>(index == 0 ? PointerSurface::Tv : PointerSurface::Drc),
										0, 0, 0, {reinterpret_cast<const std::byte*>(&current), sizeof(current)},
										session.id);
			}
			for (const auto [deviceId, usagePage, usage] : hostPressedKeyboardDevices)
				AppendInputRecordLocked(InputRecordType::Key, usagePage, usage, 1, 0,
										static_cast<std::uint8_t>(InputRecordFlag::Pressed), {}, session.id, deviceId);
			for (const auto& [identity, buttons] : hostPointerDeviceButtons)
			{
				const auto [deviceId, surface] = identity;
				for (std::uint16_t bit = 0; bit < 32; ++bit)
					if ((buttons & (1U << bit)) != 0)
						AppendInputRecordLocked(InputRecordType::MouseButton,
												static_cast<std::uint16_t>(InputUsagePage::Button), bit + 1, 1,
												static_cast<std::int32_t>(surface ==
																		  static_cast<std::uint8_t>(PointerSurface::Drc)),
												static_cast<std::uint8_t>(InputRecordFlag::Pressed), {}, session.id,
												deviceId);
			}
		}

		void AppendTextRecordLocked(std::string_view text, std::uint8_t flags,
									std::uint32_t requestId, std::uint32_t revision,
									std::uint32_t preeditStart, std::uint32_t preeditCursor,
									std::uint32_t targetSession)
		{
			using namespace cemuextend::wire;
			const std::uint64_t transfer = nextTextTransferId++;
			constexpr std::size_t MaximumChunkBytes = 32U * 1024U;
			for (std::size_t offset = 0; offset < text.size() || (text.empty() && offset == 0);)
			{
				const std::size_t bytes = std::min(MaximumChunkBytes, text.size() - offset);
				InputTextChunkV3 chunk{};
				chunk.transferId = transfer;
				chunk.totalBytes = text.size();
				chunk.offset = offset;
				chunk.requestId = requestId;
				chunk.revision = revision;
				chunk.preeditStart = preeditStart;
				chunk.preeditCursor = preeditCursor;
				chunk.selectionStart = static_cast<std::uint32_t>(text.size());
				chunk.selectionEnd = static_cast<std::uint32_t>(text.size());
				std::vector<std::byte> payload(sizeof(chunk) + bytes);
				std::memcpy(payload.data(), &chunk, sizeof(chunk));
				if (bytes != 0)
					std::memcpy(payload.data() + sizeof(chunk), text.data() + offset, bytes);
				std::uint8_t recordFlags = flags;
				if (offset == 0)
					recordFlags |= static_cast<std::uint8_t>(InputRecordFlag::TextBegin);
				if (offset + bytes == text.size())
					recordFlags |= static_cast<std::uint8_t>(InputRecordFlag::TextEnd);
				AppendInputRecordLocked(InputRecordType::TextChunk, 0, 0, 0, 0,
										recordFlags, payload, targetSession);
				if (text.empty())
					break;
				offset += bytes;
			}
		}

		Status BuildInputBatchLocked(Session& session,
									 const cemuextend::wire::InputReadRequestV3& request,
									 std::vector<std::byte>& output)
		{
			using namespace cemuextend::wire;
			if (session.negotiatedMajor < 3 || request.flags.get() != 0)
				return Status::NotSupported;
			const std::size_t maximumBytes = request.maximumBytes.get();
			if (maximumBytes < sizeof(InputSnapshotV3Header) || maximumBytes > 65520)
				return Status::InvalidArgument;

			std::uint32_t responseFlags{
				static_cast<std::uint32_t>(InputBatchFlag::Routed)};
			const auto requestedGeneration = request.generation.get();
			const auto acknowledged = request.acknowledgedSequence.get();
			const bool firstBaseline = !session.inputBaselineDelivered;
			const bool overflowBaseline = session.inputOverflow;
			const bool requiresBaseline = firstBaseline || requestedGeneration != inputGeneration ||
										  session.inputGeneration != inputGeneration || session.inputOverflow;
			if (requiresBaseline)
			{
				responseFlags |= static_cast<std::uint32_t>(InputBatchFlag::Baseline);
				if (session.inputOverflow)
					responseFlags |= static_cast<std::uint32_t>(InputBatchFlag::JournalOverflow);
				session.inputGeneration = inputGeneration;
				// The open-time cutoff is retained on the first baseline so short
				// press/release pairs captured before the first read remain visible.
				// A generation reset or overflow cannot preserve an ordered prefix.
				if (!firstBaseline || overflowBaseline)
				{
					session.inputAcknowledgedSequence = nextInputSequence - 1;
					AppendInputBaselineRecordsLocked(session);
				}
				session.inputBaselineDelivered = true;
				session.inputOverflow = false;
			}
			else
			{
				if (acknowledged < session.inputAcknowledgedSequence ||
					acknowledged >= nextInputSequence)
					return Status::InvalidArgument;
				session.inputAcknowledgedSequence = acknowledged;
			}
			TrimInputJournalLocked();

			const auto& pressedControls = firstBaseline && !overflowBaseline
											  ? session.inputBaselineControls
											  : hostPressedKeyboardUsages;
			const auto& surfaces = firstBaseline && !overflowBaseline
									   ? session.inputBaselineSurfaces
									   : inputSurfaces;
			const std::size_t pressedBytes = pressedControls.size() * sizeof(InputControlV3);
			if (sizeof(InputSnapshotV3Header) + pressedBytes > maximumBytes)
				return Status::TooLarge;
			output.resize(sizeof(InputSnapshotV3Header) + pressedBytes);
			InputSnapshotV3Header header{};
			header.generation = inputGeneration;
			header.captureTimeNs = MonotonicTimeNs();
			header.pressedControlCount = static_cast<std::uint32_t>(pressedControls.size());
			header.surfaces = surfaces;
			std::size_t writeOffset = sizeof(header);
			for (const auto [usagePage, usage] : pressedControls)
			{
				InputControlV3 control{};
				control.usagePage = usagePage;
				control.usage = usage;
				std::memcpy(output.data() + writeOffset, &control, sizeof(control));
				writeOffset += sizeof(control);
			}

			std::uint64_t nextSequence = session.inputAcknowledgedSequence;
			std::uint64_t firstSequence{};
			std::uint64_t oldestTimestamp{};
			std::uint32_t recordCount{};
			std::uint32_t recordBytes{};
			bool more{};
			for (const auto& record : inputJournal)
			{
				const auto sequence = record.header.sequence.get();
				if (sequence <= session.inputAcknowledgedSequence)
					continue;
				if (record.targetSession != 0 && record.targetSession != session.id)
				{
					nextSequence = sequence;
					continue;
				}
				const std::size_t bytes = sizeof(record.header) + record.payload.size();
				if (output.size() + bytes > maximumBytes)
				{
					more = true;
					break;
				}
				const std::size_t oldSize = output.size();
				output.resize(oldSize + bytes);
				std::memcpy(output.data() + oldSize, &record.header, sizeof(record.header));
				if (!record.payload.empty())
					std::memcpy(output.data() + oldSize + sizeof(record.header),
								record.payload.data(), record.payload.size());
				if (firstSequence == 0)
				{
					firstSequence = sequence;
					oldestTimestamp = record.header.timestampNs.get();
				}
				nextSequence = sequence;
				++recordCount;
				recordBytes += static_cast<std::uint32_t>(bytes);
			}
			if (more)
				responseFlags |= static_cast<std::uint32_t>(InputBatchFlag::More);
			header.firstSequence = firstSequence;
			header.nextSequence = nextSequence;
			header.flags = responseFlags;
			header.recordCount = recordCount;
			header.recordBytes = recordBytes;
			lastDeliveredInputSequence = std::max(lastDeliveredInputSequence, nextSequence);
			if (oldestTimestamp != 0)
				header.oldestCaptureAgeNs = header.captureTimeNs.get() - oldestTimestamp;
			std::memcpy(output.data(), &header, sizeof(header));
			return Status::Ok;
		}

		void EmitEvent(Session& session, ServiceId service, std::uint16_t operation,
					   std::span<const std::byte> payload)
		{
			const auto serviceId = static_cast<std::uint16_t>(service);
			if ((service != ServiceId::Core &&
				 !HasPermission(session, EventPermission(serviceId), serviceId, operation)) ||
				!session.subscriptions.contains(serviceId) ||
				session.responses.size() + session.reservedResponses >=
					cemuextend::transport::kMaximumResponseQueue)
			{
				if (session.subscriptions.contains(static_cast<std::uint16_t>(service)))
					++session.droppedEvents;
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
			if (!payload.empty())
				std::memcpy(result.data() + sizeof(ResponseHeader), payload.data(), payload.size());
			session.responses.push_back(std::move(result));
		}

		static std::int32_t SaturatingAdd(std::int32_t left, std::int32_t right)
		{
			const auto sum = static_cast<std::int64_t>(left) + right;
			return static_cast<std::int32_t>(std::clamp<std::int64_t>(sum,
																	  std::numeric_limits<std::int32_t>::min(),
																	  std::numeric_limits<std::int32_t>::max()));
		}

		static std::int64_t SaturatingAdd64(std::int64_t left, std::int64_t right)
		{
			if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right)
				return std::numeric_limits<std::int64_t>::max();
			if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)
				return std::numeric_limits<std::int64_t>::min();
			return left + right;
		}

		static bool CoalesceMouseMotion(Session& session,
										cemuextend::wire::MouseEventPayloadV2& event)
		{
			using namespace cemuextend::wire;
			if (event.changedButtons.get() != 0 || event.wheelX.get() != 0 ||
				event.wheelY.get() != 0 || session.responses.empty())
				return false;

			auto& response = session.responses.back();
			if (response.size() != sizeof(ResponseHeader) + sizeof(MouseEventPayloadV2))
				return false;
			ResponseHeader header{};
			std::memcpy(&header, response.data(), sizeof(header));
			if (header.flags.get() != static_cast<std::uint16_t>(
										  cemuextend::transport::ResponseFlag::Event) ||
				header.serviceId.get() != static_cast<std::uint16_t>(ServiceId::Input) ||
				header.operation.get() != static_cast<std::uint16_t>(InputEvent::MouseV2))
				return false;

			MouseEventPayloadV2 previous{};
			std::memcpy(&previous, response.data() + sizeof(header), sizeof(previous));
			if (previous.changedButtons.get() != 0 || previous.wheelX.get() != 0 ||
				previous.wheelY.get() != 0 || previous.surface != event.surface ||
				previous.buttons.get() != event.buttons.get() ||
				previous.focused != event.focused || previous.flags != event.flags)
				return false;

			// Absolute position and policy state come from the newest sample, while
			// relative motion must retain the complete distance since the guest's last
			// poll. Button and wheel events are never coalesced because their ordering
			// drives actions in the guest.
			event.deltaX = SaturatingAdd(previous.deltaX.get(), event.deltaX.get());
			event.deltaY = SaturatingAdd(previous.deltaY.get(), event.deltaY.get());
			std::memcpy(response.data() + sizeof(header), &event, sizeof(event));
			return true;
		}

		void EmitMouseEventLocked(const cemuextend::wire::MouseEventPayloadV2& state,
								  std::uint32_t deviceButtons = std::numeric_limits<std::uint32_t>::max())
		{
			using namespace cemuextend::wire;
			const std::size_t surfaceIndex =
				state.surface == static_cast<std::uint8_t>(PointerSurface::Drc) ? 1U : 0U;
			auto& snapshot = inputSurfaces[surfaceIndex];
			const bool previousFocused = snapshot.focused != 0;
			if (deviceButtons == std::numeric_limits<std::uint32_t>::max())
				deviceButtons = state.buttons.get();
			const auto deviceKey = std::pair{state.identity.deviceId.get(), state.surface};
			if (deviceButtons)
				hostPointerDeviceButtons[deviceKey] = deviceButtons;
			else
				hostPointerDeviceButtons.erase(deviceKey);
			const bool positionChanged = snapshot.x.get() != state.x.get() ||
										 snapshot.y.get() != state.y.get() || state.deltaX.get() != 0 ||
										 state.deltaY.get() != 0 || snapshot.contentWidth.get() != state.contentWidth.get() ||
										 snapshot.contentHeight.get() != state.contentHeight.get() ||
										 snapshot.insideContent != state.insideContent ||
										 snapshot.rawRelative !=
											 ((state.flags & static_cast<std::uint8_t>(MouseEventFlag::RawRelative)) != 0);
			snapshot.x = state.x.get();
			snapshot.y = state.y.get();
			snapshot.contentWidth = state.contentWidth.get();
			snapshot.contentHeight = state.contentHeight.get();
			snapshot.motionX = SaturatingAdd64(snapshot.motionX.get(), state.deltaX.get());
			snapshot.motionY = SaturatingAdd64(snapshot.motionY.get(), state.deltaY.get());
			const auto wheelXQ16 = static_cast<std::int64_t>(state.wheelX.get()) * 65536 / 120;
			const auto wheelYQ16 = static_cast<std::int64_t>(state.wheelY.get()) * 65536 / 120;
			snapshot.wheelXQ16 = SaturatingAdd64(snapshot.wheelXQ16.get(), wheelXQ16);
			snapshot.wheelYQ16 = SaturatingAdd64(snapshot.wheelYQ16.get(), wheelYQ16);
			snapshot.buttons = state.buttons.get();
			snapshot.insideContent = state.insideContent;
			snapshot.focused = state.focused;
			snapshot.rawRelative =
				(state.flags & static_cast<std::uint8_t>(MouseEventFlag::RawRelative)) != 0;
			if (positionChanged)
				AppendPointerMotionLocked(state);

			for (std::uint16_t bit = 0; bit < 32; ++bit)
			{
				if ((state.changedButtons.get() & (1U << bit)) == 0)
					continue;
				const bool pressed = (deviceButtons & (1U << bit)) != 0;
				AppendInputRecordLocked(InputRecordType::MouseButton,
										static_cast<std::uint16_t>(InputUsagePage::Button),
										static_cast<std::uint16_t>(bit + 1), pressed ? 1 : 0,
										static_cast<std::int32_t>(surfaceIndex),
										pressed ? static_cast<std::uint8_t>(InputRecordFlag::Pressed) : 0,
										{}, 0, state.identity.deviceId.get());
			}
			if (state.wheelX.get() != 0 || state.wheelY.get() != 0)
			{
				AppendInputRecordLocked(InputRecordType::MouseWheel, 0,
										static_cast<std::uint16_t>(surfaceIndex),
										static_cast<std::int32_t>(std::clamp<std::int64_t>(
											wheelXQ16, std::numeric_limits<std::int32_t>::min(),
											std::numeric_limits<std::int32_t>::max())),
										static_cast<std::int32_t>(std::clamp<std::int64_t>(
											wheelYQ16, std::numeric_limits<std::int32_t>::min(),
											std::numeric_limits<std::int32_t>::max())),
										0, {}, 0,
										state.identity.deviceId.get());
			}
			if (previousFocused != (state.focused != 0))
			{
				AppendInputRecordLocked(InputRecordType::Focus, 0,
										static_cast<std::uint16_t>(surfaceIndex), state.focused != 0 ? 1 : 0);
			}
			const auto middle = static_cast<std::uint32_t>(MouseButton::Middle);
			const bool perspectiveTransition = (state.changedButtons.get() & middle) != 0;
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
				if (perspectiveTransition)
				{
#ifndef CEMU_CEX2_TESTING
					cemuLog_log(LogType::Force,
								"CEX2-PERSPECTIVE mouse session={} event={} buttons={} changed={} "
								"queued={} reserved={} dropped={}",
								id, event.identity.eventId.get(), event.buttons.get(),
								event.changedButtons.get(), session.responses.size(),
								session.reservedResponses, session.droppedEvents);
#endif
				}
				if (CoalesceMouseMotion(session, event))
					continue;
				[[maybe_unused]] const std::size_t queuedBefore = session.responses.size();
				[[maybe_unused]] const std::uint64_t droppedBefore = session.droppedEvents;
				EmitEvent(session, ServiceId::Input,
						  static_cast<std::uint16_t>(InputEvent::MouseV2),
						  {reinterpret_cast<const std::byte*>(&event), sizeof(event)});
				if (perspectiveTransition)
				{
#ifndef CEMU_CEX2_TESTING
					cemuLog_log(LogType::Force,
								"CEX2-PERSPECTIVE mouse-emit session={} event={} enqueued={} "
								"queued={}->{} dropped={}->{}",
								id, event.identity.eventId.get(),
								session.responses.size() != queuedBefore, queuedBefore,
								session.responses.size(), droppedBefore, session.droppedEvents);
#endif
				}
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
				if (request.operation.get() == static_cast<std::uint16_t>(InputOperation::ReadBatchV3))
				{
					if (payload.size() != sizeof(InputReadRequestV3))
						return MakeResponse(request, Status::InvalidArgument);
					InputReadRequestV3 inputRequest{};
					std::memcpy(&inputRequest, payload.data(), sizeof(inputRequest));
					std::vector<std::byte> response;
					const auto status = BuildInputBatchLocked(session, inputRequest, response);
					return MakeResponse(request, status, response);
				}
				if (request.operation.get() == static_cast<std::uint16_t>(InputOperation::SetTextInputV3))
				{
					if (session.negotiatedMajor < 3 || payload.size() < sizeof(TextInputRequestHeaderV3))
						return MakeResponse(request, Status::NotSupported);
					TextInputRequestHeaderV3 header{};
					std::memcpy(&header, payload.data(), sizeof(header));
					const auto textBytes = header.initialTextBytes.get();
					const auto allowedFlags = static_cast<std::uint8_t>(TextInputFlag::Active) |
											  static_cast<std::uint8_t>(TextInputFlag::Multiline);
					if (textBytes > std::numeric_limits<std::size_t>::max() ||
						payload.size() != sizeof(header) + static_cast<std::size_t>(textBytes) ||
						(header.flags & ~allowedFlags) != 0 ||
						header.reserved != std::array<std::byte, 7>{} ||
						header.lineHeight.get() < 0)
						return MakeResponse(request, Status::InvalidArgument);
					const std::string_view text{
						reinterpret_cast<const char*>(payload.data() + sizeof(header)),
						static_cast<std::size_t>(textBytes)};
					if (!IsValidUtf8(text))
						return MakeResponse(request, Status::InvalidArgument);
					const bool active = (header.flags &
										 static_cast<std::uint8_t>(TextInputFlag::Active)) != 0;
					const auto requestId = header.requestId.get();
					if (active && (!session.textInput.active ||
								   session.textInput.requestId != requestId))
						session.textInput.sequence = nextTextInputSequence++;
					session.textInput.active = active;
					session.textInput.requestId = requestId;
					session.textInput.maximumLength = header.maximumLength.get();
					session.textInput.caretX = header.caretX.get();
					session.textInput.caretY = header.caretY.get();
					session.textInput.lineHeight = header.lineHeight.get();
					session.textInput.initialText.assign(text);
					QueueTextInputWakeLocked();
					return MakeResponse(request, Status::Ok);
				}
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
					if (!IsValidUtf8(text))
						return MakeResponse(request, Status::InvalidArgument);
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
					QueueTextInputWakeLocked();
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
				if (channel >= 2)
					return MakeResponse(request, Status::InvalidArgument);
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
				if (session.loggingTokens < 1.0)
					return MakeResponse(request, Status::Busy);
				--session.loggingTokens;
				std::string escaped;
				constexpr char hex[] = "0123456789abcdef";
				for (std::size_t index = 0; index < message.size(); ++index)
				{
					const auto character = static_cast<unsigned char>(message[index]);
					if (character < 0x20 || character == 0x7f)
					{
						escaped.append("\\x");
						escaped.push_back(hex[character >> 4]);
						escaped.push_back(hex[character & 15]);
					}
					else if (character == 0xc2 && index + 1 < message.size() &&
							 static_cast<unsigned char>(message[index + 1]) >= 0x80 &&
							 static_cast<unsigned char>(message[index + 1]) <= 0x9f)
					{
						const auto control = static_cast<unsigned char>(message[++index]);
						escaped.append("\\u00");
						escaped.push_back(hex[control >> 4]);
						escaped.push_back(hex[control & 15]);
					}
					else
						escaped.push_back(static_cast<char>(character));
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
					if (!payload.empty())
						return MakeResponse(request, Status::InvalidArgument);
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
				const auto window = windowMetrics ? windowMetrics->GetWindowMetrics() : Host::WindowMetricsSnapshot{};
				state.tvWidth = std::max(0, window.physicalWidth);
				state.tvHeight = std::max(0, window.physicalHeight);
				state.drcWidth = std::max(0, window.physicalPadWidth);
				state.drcHeight = std::max(0, window.physicalPadHeight);
				state.dpiScale = static_cast<float>(window.dpiScale);
				state.focused = window.appActive;
				state.fullscreen = window.fullscreen;
#endif
				return MakeResponse(request, Status::Ok,
									{reinterpret_cast<const std::byte*>(&state), sizeof(state)});
			}
			if (definition->handler == Handler::Diagnostics)
			{
				if (!payload.empty())
					return MakeResponse(request, Status::InvalidArgument);
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
				diagnostics.graphicsApi = static_cast<std::uint32_t>(CurrentGraphicsApi());
				return MakeResponse(request, Status::Ok,
									{reinterpret_cast<const std::byte*>(&diagnostics), sizeof(diagnostics)});
			}
			if (definition->handler == Handler::Timing)
			{
				if (request.operation.get() == static_cast<std::uint16_t>(TimingOperation::GetFrameRate))
				{
					if (!payload.empty())
						return MakeResponse(request, Status::InvalidArgument);
					TimingFrameRatePayload response{};
					response.frequency = EffectiveFrameRate();
					return MakeResponse(request, Status::Ok,
										{reinterpret_cast<const std::byte*>(&response), sizeof(response)});
				}
				if (payload.size() != sizeof(TimingFrameRatePayload))
					return MakeResponse(request, Status::InvalidArgument);
				TimingFrameRatePayload requested{};
				std::memcpy(&requested, payload.data(), sizeof(requested));
				const sint32 frequency = requested.frequency.get();
				if (!((frequency >= 30 && frequency <= 500) || frequency == 10000))
					return MakeResponse(request, Status::InvalidArgument);
				session.frameRate = frequency;
				session.frameRateSequence = nextFrameRateSequence++;
				RefreshGuestFrameRateLocked();
				TimingFrameRatePayload response{};
				response.frequency = frequency;
				return MakeResponse(request, Status::Ok,
									{reinterpret_cast<const std::byte*>(&response), sizeof(response)});
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
				Decoder decoder(payload);
				std::uint32_t handle{};
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
											service == static_cast<std::uint16_t>(ServiceId::Window) ||
											service == static_cast<std::uint16_t>(ServiceId::Ui);
				if (!exists || !supportsEvents)
					return MakeResponse(request, Status::NotSupported);
				if (service != static_cast<std::uint16_t>(ServiceId::Core) &&
					!HasPermission(session, EventPermission(service), service))
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

	Cex2Host::Cex2Host() : m_impl(std::make_shared<Impl>()) {}
	Cex2Host::~Cex2Host()
	{
		// Keep the host-owned reference alive while joining. Worker tasks also hold
		// shared references; letting the last one die on its own worker would make
		// Impl attempt to join the current thread from its destructor.
		if (m_impl)
			m_impl->StopWorkers();
	}

	void Cex2Host::ConfigureHost(std::shared_ptr<Host::IClipboard> clipboard,
								 std::shared_ptr<Host::IWindowMetrics> windowMetrics,
								 std::shared_ptr<ICemodWebUiHost> webUi)
	{
		std::shared_ptr<ICemodWebUiHost> previous;
		{
			std::scoped_lock lock(m_impl->mutex);
			m_impl->clipboard = std::move(clipboard);
			m_impl->windowMetrics = std::move(windowMetrics);
			previous = std::exchange(m_impl->webUi, webUi);
		}
		if (previous && previous != webUi)
			previous->SetEventSink({});
		if (webUi)
		{
			const std::weak_ptr weak = m_impl;
			webUi->SetEventSink([weak](CemodWebUiHostEvent event) {
				if (const auto impl = weak.lock())
					impl->PushUiEvent(std::move(event));
			});
		}
	}

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
		if (query == static_cast<std::uint32_t>(cemuextend::transport::Query::InfoV3))
		{
			if (output.size() < sizeof(cemuextend::transport::InfoV3))
				return static_cast<std::int32_t>(Error::InvalidArgument);
			cemuextend::transport::InfoV3 info{};
			info.abiMajor = cemuextend::transport::kLatestAbiMajor;
			info.abiMinor = cemuextend::transport::kLatestAbiMinor;
			info.minimumAbiMajor = cemuextend::transport::kAbiMajor;
			info.minimumAbiMinor = cemuextend::transport::kAbiMinor;
			info.maximumMessageSize = cemuextend::transport::kMaximumMessageSize;
			info.maximumResponseQueue = cemuextend::transport::kMaximumResponseQueue;
			info.maximumPageEntries = cemuextend::transport::kMaximumPageEntries;
			info.maximumInputJournalBytes = Impl::MaximumInputJournalBytes;
			info.hostBuildId = kCemuExtendBuildId;
			info.features = static_cast<std::uint64_t>(cemuextend::transport::Feature::CopyTransport) |
							static_cast<std::uint64_t>(cemuextend::transport::Feature::Cancellation) |
							static_cast<std::uint64_t>(cemuextend::transport::Feature::Pagination) |
							static_cast<std::uint64_t>(cemuextend::transport::Feature::PermissionRevocation) |
							static_cast<std::uint64_t>(cemuextend::transport::Feature::MemoryLayoutQuery) |
							static_cast<std::uint64_t>(cemuextend::transport::Feature::VersionedSessions) |
							static_cast<std::uint64_t>(cemuextend::transport::Feature::InputSnapshotBatch);
			info.coreServiceVersion = 1;
			info.inputServiceVersion = 3;
			std::memcpy(output.data(), &info, sizeof(info));
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
		if (owner.IsStopped())
			return static_cast<std::int32_t>(Error::InvalidArgument);
		std::uint16_t negotiatedMajor{};
		std::uint32_t maximumPendingRequests{};
		if (options.size() == sizeof(cemuextend::transport::OpenOptions))
		{
			cemuextend::transport::OpenOptions requested{};
			std::memcpy(&requested, options.data(), sizeof(requested));
			if (requested.abiMajor.get() != cemuextend::transport::kAbiMajor ||
				requested.abiMinor.get() > cemuextend::transport::kAbiMinor)
				return static_cast<std::int32_t>(Error::AbiMismatch);
			if (requested.flags.get() != 0 || requested.reserved.get() != 0)
				return static_cast<std::int32_t>(Error::InvalidArgument);
			negotiatedMajor = requested.abiMajor.get();
			maximumPendingRequests = requested.maximumPendingRequests.get();
		}
		else if (options.size() == sizeof(cemuextend::transport::OpenOptionsV3))
		{
			cemuextend::transport::OpenOptionsV3 requested{};
			std::memcpy(&requested, options.data(), sizeof(requested));
			constexpr auto supported =
				static_cast<std::uint64_t>(cemuextend::transport::Feature::CopyTransport) |
				static_cast<std::uint64_t>(cemuextend::transport::Feature::Cancellation) |
				static_cast<std::uint64_t>(cemuextend::transport::Feature::Pagination) |
				static_cast<std::uint64_t>(cemuextend::transport::Feature::PermissionRevocation) |
				static_cast<std::uint64_t>(cemuextend::transport::Feature::MemoryLayoutQuery) |
				static_cast<std::uint64_t>(cemuextend::transport::Feature::VersionedSessions) |
				static_cast<std::uint64_t>(cemuextend::transport::Feature::InputSnapshotBatch);
			if (requested.abiMajor.get() != cemuextend::transport::kLatestAbiMajor ||
				requested.abiMinor.get() > cemuextend::transport::kLatestAbiMinor)
				return static_cast<std::int32_t>(Error::AbiMismatch);
			if (requested.flags.get() != 0 || requested.reserved.get() != 0 ||
				(requested.requiredFeatures.get() & supported) != requested.requiredFeatures.get())
				return static_cast<std::int32_t>(Error::InvalidArgument);
			negotiatedMajor = requested.abiMajor.get();
			maximumPendingRequests = requested.maximumPendingRequests.get();
		}
		else
		{
			return static_cast<std::int32_t>(Error::InvalidArgument);
		}
		if (maximumPendingRequests == 0 ||
			maximumPendingRequests > cemuextend::transport::kMaximumResponseQueue)
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
		auto [inserted, created] = m_impl->sessions.emplace(sessionId, Impl::Session{});
		if (!created)
			return static_cast<std::int32_t>(Error::Busy);
		inserted->second.owner = &owner;
		inserted->second.id = sessionId;
		inserted->second.addressSpaceId = owner.AddressSpaceId();
		inserted->second.generation = owner.Generation();
		inserted->second.negotiatedMajor = negotiatedMajor;
		if (negotiatedMajor >= 3)
		{
			inserted->second.inputGeneration = m_impl->inputGeneration;
			inserted->second.inputAcknowledgedSequence = m_impl->nextInputSequence - 1;
			inserted->second.inputBaselineControls = m_impl->hostPressedKeyboardUsages;
			inserted->second.inputBaselineSurfaces = m_impl->inputSurfaces;
			m_impl->AppendInputBaselineRecordsLocked(inserted->second);
		}
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
			m_impl->RefreshGuestFrameRateLocked();
			return static_cast<std::int32_t>(Error::ProtocolError);
		}
		RequestHeader request{};
		std::memcpy(&request, requestBytes.data(), sizeof(request));
		if (request.totalSize.get() != requestBytes.size() || request.correlationId.get() == 0 ||
			request.flags.get() != 0)
		{
			m_impl->sessions.erase(found);
			m_impl->RefreshGuestFrameRateLocked();
			return static_cast<std::int32_t>(Error::ProtocolError);
		}
		const auto correlationId = request.correlationId.get();
		if (!Impl::AdmitCorrelation(session, correlationId))
		{
			m_impl->sessions.erase(found);
			m_impl->RefreshGuestFrameRateLocked();
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
				++session.acceptedRequests;
				return static_cast<std::int32_t>(Error::Ok);
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
				if (!payload.empty())
				{
					session.responses.push_back(MakeResponse(request, Status::InvalidArgument));
					++session.acceptedRequests;
					return static_cast<std::int32_t>(Error::Ok);
				}
			}
			else
			{
				cemuextend::wire::Decoder decoder(payload);
				if (!decoder.String(text) || decoder.remaining() || text.size() > 64U * 1024U ||
					!Impl::IsValidUtf8(text))
				{
					session.responses.push_back(MakeResponse(request, Status::InvalidArgument));
					++session.acceptedRequests;
					return static_cast<std::int32_t>(Error::Ok);
				}
			}
			const auto copiedHeader = request;
			const auto addressSpaceId = owner.AddressSpaceId();
			const auto generation = owner.Generation();
			const auto principal = owner.Principal();
			auto clipboard = m_impl->clipboard;
			++session.reservedResponses;
			session.clipboardPending = true;
			session.pending.emplace(correlationId, Impl::Session::Pending{definition->permission, copiedHeader,
																		  std::chrono::steady_clock::now() + std::chrono::seconds(5)});
			++session.acceptedRequests;
			session.bytesCopied += requestBytes.size();
			lock.unlock();
#ifdef CEMU_CEX2_TESTING
			m_impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::NotSupported);
#else
			AuditSensitiveUse(principal, request.operation.get() == 1 ? "Clipboard Read" : "Clipboard Write");
			if (!clipboard)
			{
				m_impl->Complete(sessionId, addressSpaceId, generation, correlationId,
								 Status::NotSupported);
				return static_cast<std::int32_t>(Error::Ok);
			}
			if (request.operation.get() == static_cast<std::uint16_t>(cemuextend::wire::ClipboardOperation::Get))
				clipboard->GetTextAsync([impl = m_impl, sessionId, addressSpaceId, generation, correlationId](bool success, std::string result) {
					if (!success)
					{
						impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::IoError);
						return;
					}
					if (result.size() > 64U * 1024U || !Impl::IsValidUtf8(result))
					{
						impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::TooLarge);
						return;
					}
					impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::Ok,
								   {reinterpret_cast<const std::byte*>(result.data()), result.size()});
				});
			else
				clipboard->SetTextAsync(std::move(text), [impl = m_impl, sessionId, addressSpaceId, generation, correlationId](bool success) {
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
			{
				session.responses.push_back(MakeResponse(request, Status::PermissionDenied));
				++session.acceptedRequests;
				return static_cast<std::int32_t>(Error::Ok);
			}
			cemuextend::wire::Decoder decoder(payload);
			std::uint8_t drc{};
			if (!decoder.U8(drc) || decoder.remaining() || drc > 1)
			{
				session.responses.push_back(MakeResponse(request, Status::InvalidArgument));
				++session.acceptedRequests;
				return static_cast<std::int32_t>(Error::Ok);
			}
			if (session.capture.handle && std::chrono::steady_clock::now() >= session.capture.expires)
				session.capture = {};
			if (session.capture.pending || session.capture.handle)
			{
				session.responses.push_back(MakeResponse(request, Status::Busy));
				++session.acceptedRequests;
				return static_cast<std::int32_t>(Error::Ok);
			}
			const auto copiedHeader = request;
			const auto addressSpaceId = owner.AddressSpaceId();
			const auto generation = owner.Generation();
			const auto principal = owner.Principal();
			session.capture.pending = true;
			session.capture.mainWindow = drc == 0;
			++session.reservedResponses;
			session.pending.emplace(correlationId, Impl::Session::Pending{definition->permission, copiedHeader,
																		  std::chrono::steady_clock::now() + std::chrono::seconds(5)});
			++session.acceptedRequests;
			session.bytesCopied += requestBytes.size();
			lock.unlock();
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
					[impl = m_impl, sessionId, addressSpaceId, generation, correlationId, mainWindow](const std::vector<uint8>& rgb, int width, int height, bool actualMainWindow) {
						Status status = Status::Ok;
						cemuextend::wire::CaptureOpenResponse response{};
						{
							std::lock_guard guard(impl->mutex);
							const auto found = impl->sessions.find(sessionId);
							if (found == impl->sessions.end() || found->second.addressSpaceId != addressSpaceId || found->second.generation != generation ||
								!found->second.pending.contains(correlationId) || !found->second.capture.pending)
								return std::optional<std::string>{};
							auto& capture = found->second.capture;
							const std::uint64_t w = width > 0 ? width : 0;
							const std::uint64_t h = height > 0 ? height : 0;
							if (actualMainWindow != mainWindow || w == 0 || h == 0 || w > (64ULL * 1024ULL * 1024ULL) / 3ULL / h || rgb.size() != w * h * 3ULL)
								status = Status::ProtocolError;
							else
							{
								capture.pending = false;
								capture.handle = correlationId;
								capture.width = width;
								capture.height = height;
								capture.expires = std::chrono::steady_clock::now() + std::chrono::seconds(30);
								capture.rgb.resize(rgb.size());
								std::memcpy(capture.rgb.data(), rgb.data(), rgb.size());
								response.handle = capture.handle;
								response.width = width;
								response.height = height;
								response.totalBytes = rgb.size();
								response.format = 1;
								response.chunkSize = 64U * 1024U - sizeof(ResponseHeader);
							}
						}
						impl->Complete(sessionId, addressSpaceId, generation, correlationId, status,
									   status == Status::Ok ? std::span<const std::byte>(reinterpret_cast<const std::byte*>(&response), sizeof(response)) : std::span<const std::byte>{});
						return std::optional<std::string>{};
					},
					mainWindow);
				if (!accepted)
					m_impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::Busy);
			}
#endif
			return static_cast<std::int32_t>(Error::Ok);
		}
		if (definition && request.operationVersion.get() == definition->version &&
			definition->handler == Handler::Ui)
		{
			using namespace cemuextend::wire;
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
			const auto* package = owner.Package();
			if (!package || !package->manifest.webUi || package->uiAssets.empty())
			{
				session.responses.push_back(MakeResponse(request, Status::NotFound));
				++session.acceptedRequests;
				return static_cast<std::int32_t>(Error::Ok);
			}
			const auto& webUiManifest = *package->manifest.webUi;

			Status validation = Status::Ok;
			const auto operation = static_cast<UiOperation>(request.operation.get());
			switch (operation)
			{
			case UiOperation::Create:
			{
				if (payload.size() < sizeof(UiCreateRequestHeader))
				{
					validation = Status::InvalidArgument;
					break;
				}
				UiCreateRequestHeader header{};
				std::memcpy(&header, payload.data(), sizeof(header));
				const auto viewBytes = header.viewBytes.get();
				const auto contextBytes = header.contextBytes.get();
				if (viewBytes == 0 || viewBytes > kMaximumUiNameBytes ||
					contextBytes > kMaximumUiContextBytes ||
					viewBytes > payload.size() - sizeof(header) ||
					contextBytes != payload.size() - sizeof(header) - viewBytes ||
					header.mode > static_cast<std::uint8_t>(UiMode::Overlay) ||
					header.surface > static_cast<std::uint8_t>(UiSurface::Drc) ||
					header.visible > 1 || header.interactive > 1 ||
					header.width.get() < 0 || header.height.get() < 0 ||
					(header.width.get() == 0) != (header.height.get() == 0) ||
					header.width.get() > 16384 || header.height.get() > 16384)
				{
					validation = Status::InvalidArgument;
					break;
				}
				const auto* text = reinterpret_cast<const char*>(payload.data() + sizeof(header));
				const std::string_view viewId(text, viewBytes);
				const std::string_view context(text + viewBytes, contextBytes);
				const auto view = webUiManifest.views.find(std::string(viewId));
				if (!Impl::IsValidUiName(viewId) ||
					!Impl::IsValidJson(context, kMaximumUiContextBytes))
					validation = Status::InvalidArgument;
				else if (view == webUiManifest.views.end())
					validation = Status::NotFound;
				else if ((header.mode == static_cast<std::uint8_t>(UiMode::Window) &&
						  !view->second.windowMode) ||
						 (header.mode == static_cast<std::uint8_t>(UiMode::Overlay) &&
						  (!view->second.overlayMode ||
						   std::ranges::none_of(view->second.overlay->surfaces,
												[&](CemodWebUiSurface surface) {
													return static_cast<std::uint8_t>(surface) == header.surface;
												}) ||
						   (header.interactive != 0 && !view->second.overlay->interactive))))
					validation = Status::PermissionDenied;
				break;
			}
			case UiOperation::Close:
			case UiOperation::Focus:
			{
				UiHandleRequest value{};
				if (payload.size() != sizeof(value))
					validation = Status::InvalidArgument;
				else
				{
					std::memcpy(&value, payload.data(), sizeof(value));
					if (!value.handle.get())
						validation = Status::InvalidArgument;
				}
				break;
			}
			case UiOperation::Emit:
			{
				UiMessageHeader header{};
				if (payload.size() < sizeof(header))
				{
					validation = Status::InvalidArgument;
					break;
				}
				std::memcpy(&header, payload.data(), sizeof(header));
				const auto nameBytes = header.nameBytes.get();
				const auto jsonBytes = header.jsonBytes.get();
				if (!header.handle.get() || header.callId.get() || header.flags.get() ||
					nameBytes > kMaximumUiNameBytes || jsonBytes > kMaximumUiJsonBytes ||
					nameBytes > payload.size() - sizeof(header) ||
					jsonBytes != payload.size() - sizeof(header) - nameBytes)
				{
					validation = Status::InvalidArgument;
					break;
				}
				const auto* text = reinterpret_cast<const char*>(payload.data() + sizeof(header));
				if (!Impl::IsValidUiName({text, nameBytes}) ||
					!Impl::IsValidJson({text + nameBytes, jsonBytes}, kMaximumUiJsonBytes))
					validation = Status::InvalidArgument;
				break;
			}
			case UiOperation::Reply:
			{
				UiReplyHeader header{};
				if (payload.size() < sizeof(header))
				{
					validation = Status::InvalidArgument;
					break;
				}
				std::memcpy(&header, payload.data(), sizeof(header));
				const auto jsonBytes = header.jsonBytes.get();
				if (!header.handle.get() || !header.callId.get() || header.success > 1 ||
					header.reserved != std::array<std::byte, 3>{} ||
					jsonBytes > kMaximumUiJsonBytes ||
					jsonBytes != payload.size() - sizeof(header) ||
					!Impl::IsValidJson({reinterpret_cast<const char*>(payload.data() + sizeof(header)),
										jsonBytes},
									   kMaximumUiJsonBytes))
					validation = Status::InvalidArgument;
				break;
			}
			case UiOperation::SetVisible:
			{
				UiVisibleRequest value{};
				if (payload.size() != sizeof(value))
					validation = Status::InvalidArgument;
				else
				{
					std::memcpy(&value, payload.data(), sizeof(value));
					if (!value.handle.get() || value.visible > 1 ||
						value.reserved != std::array<std::byte, 3>{})
						validation = Status::InvalidArgument;
				}
				break;
			}
			case UiOperation::SetBounds:
			{
				UiBoundsRequest value{};
				if (payload.size() != sizeof(value))
					validation = Status::InvalidArgument;
				else
				{
					std::memcpy(&value, payload.data(), sizeof(value));
					if (!value.handle.get() || value.width.get() <= 0 || value.height.get() <= 0 ||
						value.width.get() > 16384 || value.height.get() > 16384)
						validation = Status::InvalidArgument;
				}
				break;
			}
			case UiOperation::SetTitle:
			{
				UiTitleRequestHeader header{};
				if (payload.size() < sizeof(header))
				{
					validation = Status::InvalidArgument;
					break;
				}
				std::memcpy(&header, payload.data(), sizeof(header));
				const auto titleBytes = header.titleBytes.get();
				if (!header.handle.get() || titleBytes == 0 || titleBytes > 256 ||
					titleBytes != payload.size() - sizeof(header) ||
					!Impl::IsValidUtf8({reinterpret_cast<const char*>(payload.data() + sizeof(header)),
										titleBytes}))
					validation = Status::InvalidArgument;
				break;
			}
			case UiOperation::SetInteractive:
			{
				UiInteractiveRequest value{};
				if (payload.size() != sizeof(value))
					validation = Status::InvalidArgument;
				else
				{
					std::memcpy(&value, payload.data(), sizeof(value));
					if (!value.handle.get() || value.interactive > 1 ||
						value.reserved != std::array<std::byte, 3>{})
						validation = Status::InvalidArgument;
				}
				break;
			}
			default:
				validation = Status::NotSupported;
			}
			if (validation != Status::Ok)
			{
				session.responses.push_back(MakeResponse(request, validation));
				++session.acceptedRequests;
				return static_cast<std::int32_t>(Error::Ok);
			}

			auto webUi = m_impl->webUi;
			if (!webUi)
			{
				session.responses.push_back(MakeResponse(request, Status::NotSupported));
				++session.acceptedRequests;
				return static_cast<std::int32_t>(Error::Ok);
			}
			if (!session.webUiContent)
			{
				const auto contentKey = std::pair{owner.AddressSpaceId(), owner.Generation()};
				const auto cached = m_impl->webUiContents.find(contentKey);
				if (cached != m_impl->webUiContents.end())
					session.webUiContent = cached->second;
				else
				{
					auto content = std::make_shared<CemodWebUiContent>();
					content->principal = owner.Principal();
					content->modId = package->manifest.modId;
					content->titleId = owner.TitleId();
					content->manifest = webUiManifest;
					content->assets = package->uiAssets;
					session.webUiContent = content;
					m_impl->webUiContents.emplace(contentKey, std::move(content));
				}
			}
			const auto addressSpaceId = owner.AddressSpaceId();
			const auto generation = owner.Generation();
			++session.reservedResponses;
			session.pending.emplace(correlationId, Impl::Session::Pending{
													   definition->permission, request,
													   std::chrono::steady_clock::now() + std::chrono::seconds(10)});
			++session.acceptedRequests;
			session.bytesCopied += requestBytes.size();
			CemodWebUiHostRequest hostRequest{addressSpaceId, generation, sessionId, correlationId, operation, {payload.begin(), payload.end()}, session.webUiContent};
			lock.unlock();
			const bool accepted = webUi->Submit(std::move(hostRequest),
												[impl = m_impl, sessionId, addressSpaceId, generation, correlationId](
													Status status, std::vector<std::byte> response) {
													impl->Complete(sessionId, addressSpaceId, generation, correlationId,
																   status, response);
												});
			if (!accepted)
				m_impl->Complete(sessionId, addressSpaceId, generation, correlationId, Status::Busy);
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
#ifdef CEMU_CEX2_TESTING
			constexpr auto asynchronousDeadline = std::chrono::seconds(30);
#else
			constexpr auto asynchronousDeadline = std::chrono::seconds(5);
#endif
			++session.reservedResponses;
			session.pending.emplace(correlationId, Impl::Session::Pending{permission, copiedHeader,
																		  std::chrono::steady_clock::now() + asynchronousDeadline});
			++session.acceptedRequests;
			session.bytesCopied += requestBytes.size();
			m_impl->Enqueue([impl = m_impl, sessionId, addressSpaceId, generation,
							 copiedHeader, copiedPayload = std::move(copiedPayload), titleId, principal,
							 permission, maximumResponse, service, operation, correlationId]() mutable {
				{
					std::lock_guard lock(impl->mutex);
					const auto found = impl->sessions.find(sessionId);
					if (found == impl->sessions.end() || found->second.addressSpaceId != addressSpaceId ||
						found->second.generation != generation)
						return;
					auto& current = found->second;
					const auto pending = current.pending.find(correlationId);
					if (pending == current.pending.end())
						return;
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
					found->second.generation != generation)
					return;
				auto& current = found->second;
				const auto pending = current.pending.find(correlationId);
				if (pending == current.pending.end())
					return;
				if (!Impl::HasPermission(current, permission, copiedHeader.serviceId.get(),
										 copiedHeader.operation.get()))
					result = {Status::PermissionDenied};
				if (result.payload.size() > maximumResponse)
					result = {Status::TooLarge};
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
		std::unique_lock lock(m_impl->mutex);
		const auto found = m_impl->sessions.find(sessionId);
		if (found == m_impl->sessions.end() || !Impl::Owns(found->second, owner))
			return static_cast<std::int32_t>(Error::PermissionDenied);
		auto& session = found->second;
		std::vector<std::uint32_t> timedOutUi;
		for (auto pending = session.pending.begin(); pending != session.pending.end();)
		{
			if (std::chrono::steady_clock::now() < pending->second.deadline)
			{
				++pending;
				continue;
			}
			const auto service = pending->second.header.serviceId.get();
			if (service == static_cast<std::uint16_t>(ServiceId::Ui))
				timedOutUi.push_back(pending->first);
			if (service == static_cast<std::uint16_t>(ServiceId::Clipboard))
				session.clipboardPending = false;
			if (service == static_cast<std::uint16_t>(ServiceId::Capture))
				session.capture = {};
			session.responses.push_back(MakeResponse(pending->second.header, Status::TimedOut));
			pending = session.pending.erase(pending);
			--session.reservedResponses;
		}
		auto webUi = m_impl->webUi;
		const auto addressSpaceId = session.addressSpaceId;
		const auto generation = session.generation;
		auto cancelTimedOut = [&] {
			lock.unlock();
			if (webUi)
				for (const auto correlationId : timedOutUi)
					webUi->Cancel(addressSpaceId, generation, sessionId, correlationId);
		};
		if (session.responses.empty())
		{
			cancelTimedOut();
			return static_cast<std::int32_t>(Error::NotFound);
		}
		if (output.size() < session.responses.front().size())
		{
			cancelTimedOut();
			return static_cast<std::int32_t>(Error::TooLarge);
		}
		outputSize = static_cast<std::uint32_t>(session.responses.front().size());
		std::memcpy(output.data(), session.responses.front().data(), outputSize);
		session.responses.pop_front();
		++session.completedResponses;
		cancelTimedOut();
		return static_cast<std::int32_t>(Error::Ok);
	}

	std::int32_t Cex2Host::Cancel(Cex2Owner& owner, std::uint32_t sessionId,
								  std::uint32_t correlationId)
	{
		if (!correlationId)
			return static_cast<std::int32_t>(Error::InvalidArgument);
		std::unique_lock lock(m_impl->mutex);
		const auto found = m_impl->sessions.find(sessionId);
		if (found == m_impl->sessions.end() || !Impl::Owns(found->second, owner))
			return static_cast<std::int32_t>(Error::PermissionDenied);
		if (const auto pending = found->second.pending.find(correlationId);
			pending != found->second.pending.end())
		{
			const auto service = pending->second.header.serviceId.get();
			const bool ui = service == static_cast<std::uint16_t>(ServiceId::Ui);
			const auto addressSpaceId = found->second.addressSpaceId;
			const auto generation = found->second.generation;
			auto webUi = m_impl->webUi;
			if (service == static_cast<std::uint16_t>(ServiceId::Clipboard))
				found->second.clipboardPending = false;
			if (service == static_cast<std::uint16_t>(ServiceId::Capture))
				found->second.capture = {};
			found->second.responses.push_back(MakeResponse(pending->second.header, Status::Cancelled));
			found->second.pending.erase(pending);
			--found->second.reservedResponses;
			lock.unlock();
			if (ui && webUi)
				webUi->Cancel(addressSpaceId, generation, sessionId, correlationId);
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
		std::unique_lock lock(m_impl->mutex);
		const auto found = m_impl->sessions.find(sessionId);
		if (found == m_impl->sessions.end())
			return static_cast<std::int32_t>(Error::NotFound);
		if (!Impl::Owns(found->second, owner))
			return static_cast<std::int32_t>(Error::PermissionDenied);
		const bool hadTextInput = found->second.textInput.active;
		const auto addressSpaceId = found->second.addressSpaceId;
		const auto generation = found->second.generation;
		auto webUi = m_impl->webUi;
		m_impl->sessions.erase(found);
		m_impl->RefreshGuestFrameRateLocked();
		if (hadTextInput)
			m_impl->QueueTextInputWakeLocked();
		lock.unlock();
		if (webUi)
			webUi->CloseSession(addressSpaceId, generation, sessionId);
		return static_cast<std::int32_t>(Error::Ok);
	}

	void Cex2Host::CloseOwner(Cex2Owner& owner)
	{
		std::unique_lock lock(m_impl->mutex);
		bool hadTextInput{};
		std::erase_if(m_impl->sessions, [&owner, &hadTextInput](const auto& entry) {
			const auto& session = entry.second;
			const bool remove = session.owner == &owner &&
								session.addressSpaceId == owner.AddressSpaceId() &&
								session.generation == owner.Generation();
			hadTextInput |= remove && session.textInput.active;
			return remove;
		});
		m_impl->RefreshGuestFrameRateLocked();
		// Transfers are scoped to the address space, so they outlive one session of
		// it but never the owner that started them.
		Cex2Http::ReleaseSession(owner.AddressSpaceId());
		auto webUi = m_impl->webUi;
		const auto addressSpaceId = owner.AddressSpaceId();
		const auto generation = owner.Generation();
		m_impl->webUiContents.erase({addressSpaceId, generation});
		if (hadTextInput)
			m_impl->QueueTextInputWakeLocked();
		lock.unlock();
		if (webUi)
			webUi->CloseOwner(addressSpaceId, generation);
	}

	void Cex2Host::CloseAll()
	{
		std::unique_lock lock(m_impl->mutex);
		const bool hadTextInput = std::ranges::any_of(m_impl->sessions,
													  [](const auto& entry) { return entry.second.textInput.active; });
		for (const auto& entry : m_impl->sessions)
			Cex2Http::ReleaseSession(entry.second.addressSpaceId);
		m_impl->sessions.clear();
		m_impl->RefreshGuestFrameRateLocked();
		m_impl->webUiContents.clear();
		auto webUi = m_impl->webUi;
		if (hadTextInput)
			m_impl->QueueTextInputWakeLocked();
		lock.unlock();
		if (webUi)
			webUi->CloseAll();
	}

#ifdef CEMU_CEX2_TESTING
	void Cex2Host::ShutdownForTesting()
	{
		// Joining the workers also releases OpenSSL's per-thread state before the
		// short-lived sanitizer test shuts the crypto library down.
		if (m_impl)
			m_impl->StopWorkers();
		m_impl.reset();
	}
#endif

	void Cex2Host::ObserveVpad(std::int32_t channel, const VPADStatus& status,
							   std::int32_t error, std::int32_t sampleCount)
	{
		if (channel < 0 || channel >= 2)
			return;
		std::lock_guard lock(m_impl->mutex);
		for (auto& [id, session] : m_impl->sessions)
		{
			if (!Impl::HasPermission(session, 1, static_cast<std::uint16_t>(ServiceId::Input)))
				continue;
			cemuextend::wire::ObservedVpadState observed{};
			observed.frameNumber = CurrentFrameNumber();
			observed.sampleError = static_cast<std::uint32_t>(error);
			observed.hold = status.hold;
			observed.trigger = status.trig;
			observed.release = status.release;
			auto stick = [](float value) {
				return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
			};
			observed.leftX = stick(status.leftStick.x);
			observed.leftY = stick(status.leftStick.y);
			observed.rightX = stick(status.rightStick.x);
			observed.rightY = stick(status.rightStick.y);
			observed.gyroX = status.gyroChange.x;
			observed.gyroY = status.gyroChange.y;
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
				event.identity.channel = static_cast<std::uint8_t>(channel == 0 ? cemuextend::wire::InputChannel::Vpad0 : cemuextend::wire::InputChannel::Vpad1);
				event.identity.deviceId = static_cast<std::uint16_t>(channel);
				event.identity.frameNumber = static_cast<std::uint32_t>(CurrentFrameNumber());
				event.buttonsLow = status.hold;
				event.buttonsHigh = 0;
				event.leftX = observed.leftX.get();
				event.leftY = observed.leftY.get();
				event.rightX = observed.rightX.get();
				event.rightY = observed.rightY.get();
				m_impl->EmitEvent(session, ServiceId::Input,
								  static_cast<std::uint16_t>(cemuextend::wire::InputEvent::Controller),
								  {reinterpret_cast<const std::byte*>(&event), sizeof(event)});
			}
		}
	}

	void Cex2Host::ApplyMappedVpad(std::int32_t channel, VPADStatus& status)
	{
		if (channel < 0 || channel >= 2)
			return;
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
			if (!session.hasMappedInjection[channel])
				continue;
			auto& injected = session.mappedInjection[channel];
			status.hold |= injected.hold.get();
			status.trig |= injected.trigger.get();
			status.release |= injected.release.get();
			status.leftStick.x = injected.leftX.get();
			status.leftStick.y = injected.leftY.get();
			status.rightStick.x = injected.rightX.get();
			status.rightStick.y = injected.rightY.get();
			injected.trigger = 0;
			injected.release = 0;
		}
	}

	void Cex2Host::InputOwnershipChanged(cemuextend::wire::PointerSurface surface,
										 cemuextend::wire::InputOwnershipV3 ownership)
	{
		using namespace cemuextend::wire;
		if (surface != PointerSurface::Tv && surface != PointerSurface::Drc)
			return;
		const auto validOwner = [](std::uint8_t value) {
			return value <= static_cast<std::uint8_t>(InputOwner::None);
		};
		if (!validOwner(ownership.keyboardOwner) || !validOwner(ownership.pointerOwner) ||
			!validOwner(ownership.textOwner))
			return;
		const auto allowedFlags = static_cast<std::uint8_t>(InputOwnershipFlag::WebUiTextFocused) |
								  static_cast<std::uint8_t>(InputOwnershipFlag::WebUiPointerCaptured);
		if ((ownership.flags & ~allowedFlags) != 0)
			return;
		std::lock_guard lock(m_impl->mutex);
		const auto index = surface == PointerSurface::Drc ? 1U : 0U;
		auto& current = m_impl->inputOwnership[index];
		if (current.keyboardOwner == ownership.keyboardOwner &&
			current.pointerOwner == ownership.pointerOwner &&
			current.textOwner == ownership.textOwner && current.flags == ownership.flags)
			return;
		if (m_impl->nextInputOwnershipEpoch == 0)
			m_impl->nextInputOwnershipEpoch = 1;
		ownership.epoch = m_impl->nextInputOwnershipEpoch++;
		current = ownership;
		m_impl->AppendInputRecordLocked(InputRecordType::Ownership, 0,
										static_cast<std::uint16_t>(surface), 0, 0, 0,
										{reinterpret_cast<const std::byte*>(&ownership), sizeof(ownership)});
	}

	void Cex2Host::KeyboardEvent(std::uint16_t usagePage, std::uint16_t usage,
								 bool pressed, std::uint8_t modifiers, std::uint16_t deviceId)
	{
		if (!usagePage || !usage)
			return;
		std::lock_guard lock(m_impl->mutex);
		const auto control = std::pair{usagePage, usage};
		const auto deviceControl = std::tuple{deviceId, usagePage, usage};
		const bool deviceWasPressed = m_impl->hostPressedKeyboardDevices.contains(deviceControl);
		if (deviceWasPressed != pressed)
		{
			if (pressed)
			{
				m_impl->hostPressedKeyboardDevices.insert(deviceControl);
				auto& count = m_impl->hostPressedKeyboardRefCounts[control];
				if (++count == 1)
					m_impl->hostPressedKeyboardUsages.insert(control);
			}
			else
			{
				m_impl->hostPressedKeyboardDevices.erase(deviceControl);
				if (auto count = m_impl->hostPressedKeyboardRefCounts.find(control);
					count != m_impl->hostPressedKeyboardRefCounts.end() && --count->second == 0)
				{
					m_impl->hostPressedKeyboardRefCounts.erase(count);
					m_impl->hostPressedKeyboardUsages.erase(control);
				}
			}
			m_impl->AppendInputRecordLocked(
				cemuextend::wire::InputRecordType::Key,
				usagePage, usage,
				pressed ? 1 : 0, modifiers,
				pressed ? static_cast<std::uint8_t>(cemuextend::wire::InputRecordFlag::Pressed) : 0,
				{}, 0, deviceId);
		}
		if (usagePage != static_cast<std::uint16_t>(cemuextend::wire::InputUsagePage::Keyboard) ||
			usage >= 256)
			return;
		const bool aggregatePressed = m_impl->hostPressedKeyboardUsages.contains(control);
		for (auto& [id, session] : m_impl->sessions)
		{
			if (!Impl::HasPermission(session, 1, static_cast<std::uint16_t>(ServiceId::Input)))
				continue;
			const bool wasPressed = session.pressedKeyboardUsages.contains(usage);
			if (wasPressed == aggregatePressed)
				continue;
			if (aggregatePressed)
				session.pressedKeyboardUsages.insert(usage);
			else
				session.pressedKeyboardUsages.erase(usage);
			cemuextend::wire::KeyboardEventPayload event{};
			event.identity.eventId = session.nextInputEventId++;
			event.identity.origin = static_cast<std::uint8_t>(cemuextend::wire::InputOrigin::Physical);
			event.identity.channel = static_cast<std::uint8_t>(cemuextend::wire::InputChannel::Keyboard);
			event.identity.frameNumber = CurrentFrameNumber();
			event.usbHidUsage = usage;
			event.pressed = aggregatePressed;
			event.modifiers = modifiers;
			m_impl->EmitEvent(session, ServiceId::Input,
							  static_cast<std::uint16_t>(cemuextend::wire::InputEvent::Keyboard),
							  {reinterpret_cast<const std::byte*>(&event), sizeof(event)});
		}
	}

	void Cex2Host::KeyboardFocusLost()
	{
		std::lock_guard lock(m_impl->mutex);
		for (std::size_t index = 0; index < m_impl->inputOwnership.size(); ++index)
		{
			auto& ownership = m_impl->inputOwnership[index];
			const auto title = static_cast<std::uint8_t>(cemuextend::wire::InputOwner::Title);
			if (ownership.keyboardOwner == title && ownership.pointerOwner == title &&
				ownership.textOwner == title && ownership.flags == 0)
				continue;
			if (m_impl->nextInputOwnershipEpoch == 0)
				m_impl->nextInputOwnershipEpoch = 1;
			ownership.epoch = m_impl->nextInputOwnershipEpoch++;
			ownership.keyboardOwner = title;
			ownership.pointerOwner = title;
			ownership.textOwner = title;
			ownership.flags = 0;
			m_impl->AppendInputRecordLocked(cemuextend::wire::InputRecordType::Ownership, 0,
											static_cast<std::uint16_t>(index == 0
																		   ? cemuextend::wire::PointerSurface::Tv
																		   : cemuextend::wire::PointerSurface::Drc),
											0, 0, 0, {reinterpret_cast<const std::byte*>(&ownership), sizeof(ownership)});
		}
		for (const auto [deviceId, usagePage, usage] : m_impl->hostPressedKeyboardDevices)
			m_impl->AppendInputRecordLocked(
				cemuextend::wire::InputRecordType::Key,
				usagePage, usage, 0, 0, 0, {}, 0, deviceId);
		m_impl->hostPressedKeyboardDevices.clear();
		m_impl->hostPressedKeyboardRefCounts.clear();
		m_impl->hostPressedKeyboardUsages.clear();
		m_impl->AppendInputRecordLocked(cemuextend::wire::InputRecordType::DeviceReset);
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
		std::array<char, 4> utf8{};
		std::size_t utf8Bytes{};
		if (codepoint <= 0x7f)
			utf8[utf8Bytes++] = static_cast<char>(codepoint);
		else if (codepoint <= 0x7ff)
		{
			utf8[utf8Bytes++] = static_cast<char>(0xc0U | (codepoint >> 6U));
			utf8[utf8Bytes++] = static_cast<char>(0x80U | (codepoint & 0x3fU));
		}
		else if (codepoint <= 0xffff)
		{
			utf8[utf8Bytes++] = static_cast<char>(0xe0U | (codepoint >> 12U));
			utf8[utf8Bytes++] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU));
			utf8[utf8Bytes++] = static_cast<char>(0x80U | (codepoint & 0x3fU));
		}
		else
		{
			utf8[utf8Bytes++] = static_cast<char>(0xf0U | (codepoint >> 18U));
			utf8[utf8Bytes++] = static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU));
			utf8[utf8Bytes++] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU));
			utf8[utf8Bytes++] = static_cast<char>(0x80U | (codepoint & 0x3fU));
		}
		m_impl->AppendTextRecordLocked(
			std::string_view(utf8.data(), utf8Bytes),
			static_cast<std::uint8_t>(cemuextend::wire::InputRecordFlag::TextCommit),
			0, 0, 0, 0, 0);
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
			const auto textOperation = session.negotiatedMajor >= 3
										   ? cemuextend::wire::InputOperation::SetTextInputV3
										   : cemuextend::wire::InputOperation::SetTextInput;
			if (session.textInput.active && session.textInput.sequence > result.sequence &&
				Impl::HasPermission(session, 1,
									static_cast<std::uint16_t>(cemuextend::wire::ServiceId::Input),
									static_cast<std::uint16_t>(textOperation)))
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
									static_cast<std::uint16_t>(session.negotiatedMajor >= 3
																   ? InputOperation::SetTextInputV3
																   : InputOperation::SetTextInput)) &&
				(target == nullptr || session.textInput.sequence > target->textInput.sequence))
				target = &session;
		if (target == nullptr ||
			(target->negotiatedMajor < 3 && text.size() + preedit.size() > 4096) ||
			preeditStart > text.size() || preeditCursor > preedit.size() ||
			!Impl::IsValidUtf8(text) || !Impl::IsValidUtf8(preedit))
			return;

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
		const auto revision = event.revision.get();
		if (target->negotiatedMajor >= 3)
		{
			if (!text.empty())
				m_impl->AppendTextRecordLocked(
					text, static_cast<std::uint8_t>(InputRecordFlag::TextCommit),
					target->textInput.requestId, revision, preeditStart, preeditCursor,
					target->id);
			m_impl->AppendTextRecordLocked(
				preedit, static_cast<std::uint8_t>(InputRecordFlag::TextPreedit),
				target->textInput.requestId, revision, preeditStart, preeditCursor,
				target->id);
			return;
		}
		std::vector<std::byte> payload(sizeof(event) + text.size() + preedit.size());
		std::memcpy(payload.data(), &event, sizeof(event));
		if (!text.empty())
			std::memcpy(payload.data() + sizeof(event), text.data(), text.size());
		if (!preedit.empty())
			std::memcpy(payload.data() + sizeof(event) + text.size(),
						preedit.data(), preedit.size());
		m_impl->EmitEvent(*target, ServiceId::Input,
						  static_cast<std::uint16_t>(InputEvent::TextComposition), payload);
	}

	void Cex2Host::SetTextInputWakeCallback(std::function<void()> callback)
	{
		std::lock_guard lock(m_impl->mutex);
		m_impl->textInputWakeCallback = std::move(callback);
	}

	void Cex2Host::MouseEvent(cemuextend::wire::PointerSurface surface,
							  std::int32_t x, std::int32_t y, std::int32_t deltaX, std::int32_t deltaY,
							  std::int32_t wheelX, std::int32_t wheelY, std::uint32_t buttons,
							  std::uint32_t changedButtons, std::int32_t contentWidth,
							  std::int32_t contentHeight, bool insideContent, bool focused, std::uint8_t flags,
							  std::uint16_t deviceId, std::uint32_t deviceButtons)
	{
		using namespace cemuextend::wire;
		MouseEventPayloadV2 state{};
		state.identity.origin = static_cast<std::uint8_t>(InputOrigin::Physical);
		state.identity.channel = static_cast<std::uint8_t>(InputChannel::Mouse);
		state.identity.deviceId = deviceId;
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
		state.normalizedX = contentWidth > 0 ? std::clamp(static_cast<float>(x) / static_cast<float>(contentWidth), 0.0f, 1.0f) : 0.0f;
		state.normalizedY = contentHeight > 0 ? std::clamp(static_cast<float>(y) / static_cast<float>(contentHeight), 0.0f, 1.0f) : 0.0f;
		state.surface = static_cast<std::uint8_t>(surface);
		state.insideContent = insideContent;
		state.focused = focused;
		state.flags = flags;
		std::lock_guard lock(m_impl->mutex);
		m_impl->EmitMouseEventLocked(state, deviceButtons);
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
			m_impl->hostPointerDeviceButtons.clear();
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
		std::unique_lock lock(m_impl->mutex);
		const bool closeUi = (owner.GrantedPermissions() & kCemodUiPermission) != 0 &&
							 (permissions & kCemodUiPermission) == 0;
		bool hadTextInput{};
		bool frameRateChanged{};
		for (const auto& [id, session] : m_impl->sessions)
			hadTextInput |= session.owner == &owner && session.textInput.active;
		owner.SetGrantedPermissions(permissions);
		for (auto& [id, session] : m_impl->sessions)
		{
			if (session.owner != &owner)
				continue;
			std::erase_if(session.subscriptions, [&session](std::uint16_t service) {
				return service != static_cast<std::uint16_t>(ServiceId::Core) &&
					   !Impl::HasPermission(session, Impl::EventPermission(service), service);
			});
			for (auto response = session.responses.begin(); response != session.responses.end();)
			{
				ResponseHeader header{};
				std::memcpy(&header, response->data(), sizeof(header));
				const bool event = header.flags.get() == static_cast<std::uint16_t>(
															 cemuextend::transport::ResponseFlag::Event);
				const auto* definition = FindOperation(header.serviceId.get(), header.operation.get());
				const auto required = event		   ? Impl::EventPermission(header.serviceId.get())
									  : definition ? definition->permission
												   : 0U;
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
			if (session.frameRate > 0 &&
				!Impl::HasPermission(session, 2, static_cast<std::uint16_t>(ServiceId::Timing),
									 static_cast<std::uint16_t>(cemuextend::wire::TimingOperation::SetFrameRate)))
			{
				session.frameRate = -1;
				session.frameRateSequence = 0;
				frameRateChanged = true;
			}
			for (auto pending = session.pending.begin(); pending != session.pending.end();)
			{
				if (Impl::HasPermission(session, pending->second.permission,
										pending->second.header.serviceId.get(), pending->second.header.operation.get()))
				{
					++pending;
					continue;
				}
				session.responses.push_back(MakeResponse(pending->second.header, Status::PermissionDenied));
				const auto service = pending->second.header.serviceId.get();
				if (service == static_cast<std::uint16_t>(ServiceId::Clipboard))
					session.clipboardPending = false;
				if (service == static_cast<std::uint16_t>(ServiceId::Capture))
					session.capture = {};
				pending = session.pending.erase(pending);
				--session.reservedResponses;
			}
		}
		if (frameRateChanged)
			m_impl->RefreshGuestFrameRateLocked();
		if (hadTextInput)
			m_impl->QueueTextInputWakeLocked();
		auto webUi = m_impl->webUi;
		const auto addressSpaceId = owner.AddressSpaceId();
		const auto generation = owner.Generation();
		if (closeUi)
			m_impl->webUiContents.erase({addressSpaceId, generation});
		lock.unlock();
		if (closeUi && webUi)
			webUi->CloseOwner(addressSpaceId, generation);
	}

} // namespace cemuextend_hle
