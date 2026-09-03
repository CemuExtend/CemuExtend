#include "Common/precompiled.h"

#include "Cafe/HW/Espresso/ModExecutionContext.h"
#include "Cafe/OS/libs/cemuextend/CemodPermission.h"
#include "Cafe/OS/libs/cemuextend/CemodWebUiHost.h"
#include "Cafe/OS/libs/cemuextend/Cex2Host.h"
#include "Cafe/OS/libs/cemuextend/Cex2Http.h"
#include "Cafe/OS/libs/cemuextend/Cex2Media.h"
#include "Cafe/OS/libs/cemuextend/Cex2Storage.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "cemuextend/services.hpp"
#include "cemuextend/transport.hpp"

#include <openssl/crypto.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

	[[noreturn]] void CheckFailed(const char* expression, int line)
	{
		std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
		std::abort();
	}

#define CHECK(condition)                       \
	do                                         \
	{                                          \
		if (!(condition))                      \
			CheckFailed(#condition, __LINE__); \
	}                                          \
	while (false)

	using cemuextend::transport::OpenOptions;
	using cemuextend::transport::RequestHeader;
	using cemuextend::transport::ResponseHeader;
	using cemuextend::wire::Error;
	using cemuextend::wire::Status;

	class TestWebUiHost final : public cemuextend_hle::ICemodWebUiHost
	{
	  public:
		void SetEventSink(EventSink value) override
		{
			eventSink = std::move(value);
		}

		bool Submit(cemuextend_hle::CemodWebUiHostRequest request,
					Completion completion) override
		{
			contents.push_back(request.content);
			requests.push_back(std::move(request));
			completions.push_back(std::move(completion));
			return accept;
		}

		void Cancel(std::uint64_t, std::uint32_t, std::uint32_t,
					std::uint32_t correlationId) override
		{
			cancelled.push_back(correlationId);
		}

		void CloseSession(std::uint64_t, std::uint32_t,
						  std::uint32_t sessionId) override
		{
			closedSessions.push_back(sessionId);
		}

		void CloseOwner(std::uint64_t, std::uint32_t) override
		{
			++closedOwners;
		}

		void CloseAll() override
		{
			++closedAll;
		}

		void Complete(std::size_t index, Status status,
					  std::span<const std::byte> payload = {})
		{
			auto completion = std::move(completions.at(index));
			completion(status, {payload.begin(), payload.end()});
		}

		void Emit(cemuextend_hle::CemodWebUiHostEvent event)
		{
			CHECK(eventSink);
			eventSink(std::move(event));
		}

		bool accept{true};
		EventSink eventSink;
		std::vector<cemuextend_hle::CemodWebUiHostRequest> requests;
		std::vector<Completion> completions;
		std::vector<std::shared_ptr<const cemuextend_hle::CemodWebUiContent>> contents;
		std::vector<std::uint32_t> cancelled;
		std::vector<std::uint32_t> closedSessions;
		std::uint32_t closedOwners{};
		std::uint32_t closedAll{};
	};

	std::vector<std::byte> Request(std::uint32_t correlation, std::uint16_t operation,
								   std::span<const std::byte> payload = {},
								   cemuextend::wire::ServiceId service = cemuextend::wire::ServiceId::Core)
	{
		std::vector<std::byte> result(sizeof(RequestHeader) + payload.size());
		auto& header = *reinterpret_cast<RequestHeader*>(result.data());
		header.totalSize = static_cast<std::uint32_t>(result.size());
		header.correlationId = correlation;
		header.serviceId = static_cast<std::uint16_t>(service);
		header.operation = operation;
		header.operationVersion = cemuextend::transport::kOperationVersion;
		header.flags = 0;
		if (!payload.empty())
			std::memcpy(result.data() + sizeof(header), payload.data(), payload.size());
		return result;
	}

	std::vector<std::byte> PollUntil(cemuextend_hle::Cex2Host& host,
									 ModExecutionContext& context, std::uint32_t session)
	{
		std::vector<std::byte> response(cemuextend::transport::kMaximumMessageSize);
		std::uint32_t size{};
		for (;;)
		{
			const auto result = host.Poll(context, session, response, size);
			if (result == static_cast<std::int32_t>(Error::Ok))
			{
				response.resize(size);
				return response;
			}
			CHECK(result == static_cast<std::int32_t>(Error::NotFound));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	std::uint32_t Open(cemuextend_hle::Cex2Host& host, ModExecutionContext& context)
	{
		OpenOptions options{};
		options.abiMajor = cemuextend::transport::kAbiMajor;
		options.abiMinor = cemuextend::transport::kAbiMinor;
		options.maximumPendingRequests = 128;
		std::uint32_t session{};
		const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(&options), sizeof(options));
		CHECK(host.Open(context, bytes, session) == static_cast<std::int32_t>(Error::Ok));
		CHECK(session != 0);
		return session;
	}

	std::uint32_t OpenV3(cemuextend_hle::Cex2Host& host, ModExecutionContext& context)
	{
		cemuextend::transport::OpenOptionsV3 options{};
		options.abiMajor = cemuextend::transport::kLatestAbiMajor;
		options.abiMinor = cemuextend::transport::kLatestAbiMinor;
		options.maximumPendingRequests = 128;
		options.requiredFeatures =
			static_cast<std::uint64_t>(cemuextend::transport::Feature::CopyTransport) |
			static_cast<std::uint64_t>(cemuextend::transport::Feature::Cancellation) |
			static_cast<std::uint64_t>(cemuextend::transport::Feature::Pagination) |
			static_cast<std::uint64_t>(cemuextend::transport::Feature::PermissionRevocation) |
			static_cast<std::uint64_t>(cemuextend::transport::Feature::MemoryLayoutQuery) |
			static_cast<std::uint64_t>(cemuextend::transport::Feature::VersionedSessions) |
			static_cast<std::uint64_t>(cemuextend::transport::Feature::InputSnapshotBatch);
		std::uint32_t session{};
		const auto bytes = std::span<const std::byte>(
			reinterpret_cast<const std::byte*>(&options), sizeof(options));
		CHECK(host.Open(context, bytes, session) == static_cast<std::int32_t>(Error::Ok));
		CHECK(session != 0);
		return session;
	}

	void TestOwnershipCopyAndCancel()
	{
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext first(1, 7, "publisher:first");
		ModExecutionContext second(2, 3, "publisher:second");

		std::array<std::byte, sizeof(cemuextend::transport::Info)> infoBytes{};
		CHECK(host.Query(first, static_cast<std::uint32_t>(cemuextend::transport::Query::Info), infoBytes) ==
			  static_cast<std::int32_t>(Error::Ok));
		const auto& info = *reinterpret_cast<const cemuextend::transport::Info*>(infoBytes.data());
		CHECK(info.abiMajor.get() == 2);
		CHECK(info.abiMinor.get() == 3);
		CHECK(info.maximumMessageSize.get() == 64U * 1024U);
		CHECK((info.features.get() & static_cast<std::uint64_t>(
										 cemuextend::transport::Feature::MemoryLayoutQuery)) != 0);
		std::array<std::byte, sizeof(cemuextend::transport::InfoV3)> infoV3Bytes{};
		CHECK(host.Query(first,
						 static_cast<std::uint32_t>(cemuextend::transport::Query::InfoV3),
						 infoV3Bytes) == static_cast<std::int32_t>(Error::Ok));
		const auto& infoV3 =
			*reinterpret_cast<const cemuextend::transport::InfoV3*>(infoV3Bytes.data());
		CHECK(infoV3.abiMajor.get() == 3 && infoV3.inputServiceVersion.get() == 3);
		CHECK(infoV3.maximumInputJournalBytes.get() == 16U * 1024U * 1024U);

		std::array<std::byte, sizeof(cemuextend::transport::MemoryLayout)> layoutBytes{};
		CHECK(host.Query(first,
						 static_cast<std::uint32_t>(cemuextend::transport::Query::MemoryLayout),
						 layoutBytes) == static_cast<std::int32_t>(Error::Ok));
		const auto& layout =
			*reinterpret_cast<const cemuextend::transport::MemoryLayout*>(layoutBytes.data());
		CHECK(layout.mem2Base.get() == 0x10000000);
		CHECK(layout.mem2End.get() == 0x50000000);
		CHECK(layout.mappedMemoryBase.get() == 0x60000000);
		CHECK(layout.mappedMemoryEnd.get() == 0xa0000000);

		const auto session = Open(host, first);
		const cemuextend::wire::Be64 cookie{0x1122334455667788ULL};
		const auto cookieBytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(&cookie), sizeof(cookie));
		auto request = Request(9, static_cast<std::uint16_t>(cemuextend::wire::CoreOperation::Ping), cookieBytes);
		CHECK(host.Submit(second, session, request) == static_cast<std::int32_t>(Error::PermissionDenied));
		CHECK(host.Submit(first, session, request) == static_cast<std::int32_t>(Error::Ok));
		CHECK(host.Cancel(first, session, 9) == static_cast<std::int32_t>(Error::Ok));

		std::array<std::byte, cemuextend::transport::kMaximumMessageSize> response{};
		std::uint32_t responseSize{};
		CHECK(host.Poll(second, session, response, responseSize) ==
			  static_cast<std::int32_t>(Error::PermissionDenied));
		CHECK(host.Poll(first, session, response, responseSize) == static_cast<std::int32_t>(Error::Ok));
		const auto& header = *reinterpret_cast<const ResponseHeader*>(response.data());
		CHECK(header.correlationId.get() == 9);
		CHECK(header.status.get() == static_cast<std::uint16_t>(Status::Cancelled));
		CHECK(responseSize == sizeof(ResponseHeader));
		CHECK(host.Close(first, session) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestDiagnosticsGraphicsApi()
	{
		using namespace cemuextend::wire;
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(91, 1, "diagnostics-graphics-api");
		context.SetGrantedPermissions(1);
		const auto session = Open(host, context);
		auto request = Request(1, static_cast<std::uint16_t>(DiagnosticsOperation::Get),
							   {}, ServiceId::Diagnostics);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		auto response = PollUntil(host, context, session);
		const auto* header = reinterpret_cast<const ResponseHeader*>(response.data());
		CHECK(header->status.get() == static_cast<std::uint16_t>(Status::Ok));
		CHECK(response.size() == sizeof(ResponseHeader) + sizeof(DiagnosticsPayload));
		const auto* diagnostics = reinterpret_cast<const DiagnosticsPayload*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(static_cast<GraphicsApi>(diagnostics->graphicsApi.get()) == GraphicsApi::Unknown);
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestTimingFrameRateOverride()
	{
		using namespace cemuextend::wire;
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(92, 1, "timing-frame-rate");
		context.SetGrantedPermissions(3);
		const std::uint32_t timingMask =
			1U << (static_cast<std::uint32_t>(ServiceId::Timing) - 1U);
		context.SetServicePermissions({timingMask, timingMask, 0});
		const auto session = Open(host, context);

		auto get = Request(1, static_cast<std::uint16_t>(TimingOperation::GetFrameRate),
						   {}, ServiceId::Timing);
		CHECK(host.Submit(context, session, get) == static_cast<std::int32_t>(Error::Ok));
		auto response = PollUntil(host, context, session);
		const auto* header = reinterpret_cast<const ResponseHeader*>(response.data());
		CHECK(header->status.get() == static_cast<std::uint16_t>(Status::Ok));
		const auto* initial = reinterpret_cast<const TimingFrameRatePayload*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(initial->frequency.get() == 60);

		TimingFrameRatePayload requested{};
		requested.frequency = 144;
		const auto bytes = std::span<const std::byte>(
			reinterpret_cast<const std::byte*>(&requested), sizeof(requested));
		auto set = Request(2, static_cast<std::uint16_t>(TimingOperation::SetFrameRate),
						   bytes, ServiceId::Timing);
		CHECK(host.Submit(context, session, set) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		header = reinterpret_cast<const ResponseHeader*>(response.data());
		CHECK(header->status.get() == static_cast<std::uint16_t>(Status::Ok));
		const auto* applied = reinterpret_cast<const TimingFrameRatePayload*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(applied->frequency.get() == 144);

		context.SetServicePermissions({timingMask, 0, 0});
		host.PermissionsChanged(context, 3);
		get = Request(3, static_cast<std::uint16_t>(TimingOperation::GetFrameRate),
					  {}, ServiceId::Timing);
		CHECK(host.Submit(context, session, get) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		const auto* revoked = reinterpret_cast<const TimingFrameRatePayload*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(revoked->frequency.get() == 60);
		context.SetServicePermissions({timingMask, timingMask, 0});
		host.PermissionsChanged(context, 3);

		requested.frequency = 501;
		auto invalid = Request(4, static_cast<std::uint16_t>(TimingOperation::SetFrameRate),
							   bytes, ServiceId::Timing);
		CHECK(host.Submit(context, session, invalid) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		header = reinterpret_cast<const ResponseHeader*>(response.data());
		CHECK(header->status.get() == static_cast<std::uint16_t>(Status::InvalidArgument));

		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
		const auto replacement = Open(host, context);
		get = Request(5, static_cast<std::uint16_t>(TimingOperation::GetFrameRate),
					  {}, ServiceId::Timing);
		CHECK(host.Submit(context, replacement, get) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, replacement);
		const auto* reset = reinterpret_cast<const TimingFrameRatePayload*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(reset->frequency.get() == 60);
		CHECK(host.Close(context, replacement) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestBackpressureAndProtocolReap()
	{
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(10, 1, "unsigned:package");
		auto session = Open(host, context);

		for (std::uint32_t index = 1; index <= cemuextend::transport::kMaximumResponseQueue; ++index)
		{
			auto request = Request(index, static_cast<std::uint16_t>(cemuextend::wire::CoreOperation::GetVersion));
			CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		}
		auto blocked = Request(1000, static_cast<std::uint16_t>(cemuextend::wire::CoreOperation::GetVersion));
		CHECK(host.Submit(context, session, blocked) == static_cast<std::int32_t>(Error::Busy));

		std::array<std::byte, cemuextend::transport::kMaximumMessageSize> response{};
		std::uint32_t responseSize{};
		CHECK(host.Poll(context, session, response, responseSize) == static_cast<std::int32_t>(Error::Ok));
		CHECK(host.Submit(context, session, blocked) == static_cast<std::int32_t>(Error::Ok));
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));

		session = Open(host, context);
		auto malformed = Request(1, static_cast<std::uint16_t>(cemuextend::wire::CoreOperation::Ping));
		reinterpret_cast<RequestHeader*>(malformed.data())->flags = 0x8000;
		CHECK(host.Submit(context, session, malformed) == static_cast<std::int32_t>(Error::ProtocolError));
		CHECK(host.Poll(context, session, response, responseSize) ==
			  static_cast<std::int32_t>(Error::PermissionDenied));
		const auto replacement = Open(host, context);
		CHECK(replacement != session);
		CHECK(host.Close(context, replacement) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestExactOnceAdmission()
	{
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(11, 1, "exactly-once-principal");
		const auto session = Open(host, context);
		auto request = Request(77,
							   static_cast<std::uint16_t>(cemuextend::wire::CoreOperation::GetVersion));
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		(void)PollUntil(host, context, session);
		CHECK(host.Submit(context, session, request) ==
			  static_cast<std::int32_t>(Error::ProtocolError));
		std::array<std::byte, cemuextend::transport::kMaximumMessageSize> response{};
		std::uint32_t responseSize{};
		CHECK(host.Poll(context, session, response, responseSize) ==
			  static_cast<std::int32_t>(Error::PermissionDenied));
		const auto replacement = Open(host, context);
		CHECK(replacement != session);
		CHECK(host.Close(context, replacement) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestPrincipalStorageAndPagination()
	{
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(20, 1, "storage-test-principal");
		context.SetTitleId(0xce02000000000001ULL);
		context.SetGrantedPermissions(3);
		const auto session = Open(host, context);

		cemuextend::wire::Encoder set;
		CHECK(set.String("answer"));
		set.U8(static_cast<std::uint8_t>(cemuextend::wire::ValueType::UnsignedInteger));
		set.U32(8);
		set.U64(42);
		auto request = Request(1, static_cast<std::uint16_t>(cemuextend::wire::ConfigurationOperation::Set),
							   set.data(), cemuextend::wire::ServiceId::Configuration);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		auto response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() == static_cast<std::uint16_t>(Status::Ok));

		cemuextend::wire::Encoder get;
		CHECK(get.String("answer"));
		request = Request(2, static_cast<std::uint16_t>(cemuextend::wire::ConfigurationOperation::Get),
						  get.data(), cemuextend::wire::ServiceId::Configuration);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() == static_cast<std::uint16_t>(Status::Ok));

		cemuextend::wire::Encoder write;
		CHECK(write.String("folder/file.bin"));
		write.U64(0);
		const std::array data{std::byte{1}, std::byte{2}, std::byte{3}};
		write.Bytes(data);
		request = Request(3, static_cast<std::uint16_t>(cemuextend::wire::FileOperation::Write),
						  write.data(), cemuextend::wire::ServiceId::File);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() == static_cast<std::uint16_t>(Status::Ok));

		cemuextend::wire::Encoder stat;
		CHECK(stat.String("folder/file.bin"));
		request = Request(4, static_cast<std::uint16_t>(cemuextend::wire::FileOperation::Stat),
						  stat.data(), cemuextend::wire::ServiceId::File);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() == static_cast<std::uint16_t>(Status::Ok));
		CHECK(response.size() == sizeof(ResponseHeader) + sizeof(cemuextend::wire::FileStatPayload));
		const auto& fileStat = *reinterpret_cast<const cemuextend::wire::FileStatPayload*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(fileStat.size.get() == data.size());
		CHECK(fileStat.modifiedTimeNs.get() != 0);
		CHECK(fileStat.mode.get() == 0600 && fileStat.type == 1);

		cemuextend::wire::Encoder sparse;
		CHECK(sparse.String("folder/file.bin"));
		sparse.U64(data.size() + 1);
		sparse.U8(0xff);
		request = Request(5, static_cast<std::uint16_t>(cemuextend::wire::FileOperation::Write),
						  sparse.data(), cemuextend::wire::ServiceId::File);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::InvalidArgument));

		cemuextend::wire::Encoder removeRoot;
		CHECK(removeRoot.String("."));
		request = Request(6, static_cast<std::uint16_t>(cemuextend::wire::FileOperation::Remove),
						  removeRoot.data(), cemuextend::wire::ServiceId::File);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::PermissionDenied));

		cemuextend::wire::Encoder read;
		CHECK(read.String("folder/file.bin"));
		read.U64(0);
		read.U32(64 * 1024);
		request = Request(7, static_cast<std::uint16_t>(cemuextend::wire::FileOperation::Read),
						  read.data(), cemuextend::wire::ServiceId::File);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(response.size() == sizeof(ResponseHeader) + data.size());
		CHECK(std::memcmp(response.data() + sizeof(ResponseHeader), data.data(), data.size()) == 0);

		// Aqua writes a complete temporary config and commits it by renaming
		// over the live file. Repeated saves must replace the destination rather
		// than failing after the first config has been created.
		const std::array replacementData{std::byte{4}, std::byte{5}};
		cemuextend::wire::Encoder replacementWrite;
		CHECK(replacementWrite.String("folder/file.bin.tmp"));
		replacementWrite.U64(0);
		replacementWrite.Bytes(replacementData);
		request = Request(100, static_cast<std::uint16_t>(cemuextend::wire::FileOperation::Write),
						  replacementWrite.data(), cemuextend::wire::ServiceId::File);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));

		cemuextend::wire::Encoder replace;
		CHECK(replace.String("folder/file.bin.tmp"));
		CHECK(replace.String("folder/file.bin"));
		request = Request(101, static_cast<std::uint16_t>(cemuextend::wire::FileOperation::Rename),
						  replace.data(), cemuextend::wire::ServiceId::File);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));

		cemuextend::wire::Encoder replacementRead;
		CHECK(replacementRead.String("folder/file.bin"));
		replacementRead.U64(0);
		replacementRead.U32(64 * 1024);
		request = Request(102, static_cast<std::uint16_t>(cemuextend::wire::FileOperation::Read),
						  replacementRead.data(), cemuextend::wire::ServiceId::File);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		CHECK(response.size() == sizeof(ResponseHeader) + replacementData.size());
		CHECK(std::memcmp(response.data() + sizeof(ResponseHeader), replacementData.data(),
						  replacementData.size()) == 0);

		cemuextend::wire::Encoder invalidType;
		CHECK(invalidType.String("invalid-bool"));
		invalidType.U8(static_cast<std::uint8_t>(cemuextend::wire::ValueType::Boolean));
		invalidType.U32(2);
		invalidType.U8(0);
		invalidType.U8(1);
		request = Request(8, static_cast<std::uint16_t>(cemuextend::wire::ConfigurationOperation::Set),
						  invalidType.data(), cemuextend::wire::ServiceId::Configuration);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::InvalidArgument));

		for (std::uint32_t index = 0; index < 3; ++index)
		{
			cemuextend::wire::Encoder pageValue;
			CHECK(pageValue.String("page-" + std::to_string(index)));
			pageValue.U8(static_cast<std::uint8_t>(cemuextend::wire::ValueType::Boolean));
			pageValue.U32(1);
			pageValue.U8(1);
			request = Request(9 + index,
							  static_cast<std::uint16_t>(cemuextend::wire::ConfigurationOperation::Set),
							  pageValue.data(), cemuextend::wire::ServiceId::Configuration);
			CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
			response = PollUntil(host, context, session);
			CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
				  static_cast<std::uint16_t>(Status::Ok));
		}
		cemuextend::wire::Encoder page;
		CHECK(page.String("page-"));
		page.U16(2);
		page.U32(0);
		request = Request(12,
						  static_cast<std::uint16_t>(cemuextend::wire::ConfigurationOperation::List),
						  page.data(), cemuextend::wire::ServiceId::Configuration);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		cemuextend::wire::Decoder pageResponse(std::span<const std::byte>(response).subspan(
			sizeof(ResponseHeader)));
		std::uint32_t pageCount{}, tokenSize{};
		std::uint8_t type{}, truncated{};
		std::string pageKey;
		std::span<const std::byte> token;
		CHECK(pageResponse.U32(pageCount) && pageCount == 2);
		for (std::uint32_t index = 0; index < pageCount; ++index)
			CHECK(pageResponse.String(pageKey) && pageKey.starts_with("page-") && pageResponse.U8(type));
		CHECK(pageResponse.U8(truncated) && truncated == 1);
		CHECK(pageResponse.U32(tokenSize) && tokenSize != 0 && pageResponse.Bytes(tokenSize, token));
		CHECK(pageResponse.remaining() == 0);
		cemuextend::wire::Encoder hugeOffset;
		CHECK(hugeOffset.String("folder/file.bin"));
		hugeOffset.U64(std::numeric_limits<std::uint64_t>::max());
		hugeOffset.U32(1);
		request = Request(13, static_cast<std::uint16_t>(cemuextend::wire::FileOperation::Read),
						  hugeOffset.data(), cemuextend::wire::ServiceId::File);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::InvalidArgument));

#ifndef _WIN32
		const auto titleRoot = cemuextend_hle::Cex2Storage::TestingRoot() /
							   "files" / "ce02000000000001";
		std::error_code filesystemError;
		std::filesystem::path namespaceRoot;
		for (std::filesystem::directory_iterator iterator(titleRoot, filesystemError), end;
			 !filesystemError && iterator != end; ++iterator)
		{
			if (iterator->is_directory())
			{
				namespaceRoot = iterator->path();
				break;
			}
		}
		CHECK(!namespaceRoot.empty());
		const auto outside = std::filesystem::temp_directory_path() / "cemuextend-cex2-outside";
		std::filesystem::create_directories(outside, filesystemError);
		CHECK(!filesystemError);
		{
			std::ofstream secret(outside / "secret.bin", std::ios::binary);
			secret.put('x');
			CHECK(secret.good());
		}
		std::filesystem::remove(namespaceRoot / "escape", filesystemError);
		filesystemError.clear();
		std::filesystem::create_directory_symlink(outside, namespaceRoot / "escape", filesystemError);
		CHECK(!filesystemError);
		cemuextend::wire::Encoder escapedStat;
		CHECK(escapedStat.String("escape/secret.bin"));
		request = Request(14, static_cast<std::uint16_t>(cemuextend::wire::FileOperation::Stat),
						  escapedStat.data(), cemuextend::wire::ServiceId::File);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::PermissionDenied));
		std::filesystem::remove(namespaceRoot / "escape", filesystemError);
		std::filesystem::remove_all(outside, filesystemError);
#endif
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestTitleServicePermissionsAndRevocation()
	{
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(30, 1, "permission-test-principal");
		context.SetGrantedPermissions(2);
		constexpr auto loggingBit = 1U << (static_cast<std::uint16_t>(cemuextend::wire::ServiceId::Logging) - 1U);
		context.SetServicePermissions({0, loggingBit, 0});
		const auto session = Open(host, context);

		cemuextend::wire::Encoder payload;
		payload.U8(static_cast<std::uint8_t>(cemuextend::wire::LogLevel::Info));
		CHECK(payload.String("permission test"));
		auto request = Request(1,
							   static_cast<std::uint16_t>(cemuextend::wire::LoggingOperation::Write),
							   payload.data(), cemuextend::wire::ServiceId::Logging);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));

		// A response already queued before revocation must not leak its result.
		context.SetServicePermissions({0, 0, 0});
		host.PermissionsChanged(context, 2);
		auto response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::PermissionDenied));

		request = Request(2,
						  static_cast<std::uint16_t>(cemuextend::wire::LoggingOperation::Write),
						  payload.data(), cemuextend::wire::ServiceId::Logging);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::PermissionDenied));

		context.SetServicePermissions({0, loggingBit, 0});
		host.PermissionsChanged(context, 2);
		request = Request(3,
						  static_cast<std::uint16_t>(cemuextend::wire::LoggingOperation::Write),
						  payload.data(), cemuextend::wire::ServiceId::Logging);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		host.PermissionsChanged(context, 0);
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::PermissionDenied));
		host.PermissionsChanged(context, 2);
		request = Request(4,
						  static_cast<std::uint16_t>(cemuextend::wire::LoggingOperation::Write),
						  payload.data(), cemuextend::wire::ServiceId::Logging);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestObservedInputSnapshot()
	{
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(31, 1, "input-snapshot-principal");
		context.SetGrantedPermissions(1);
		const auto session = Open(host, context);
		VPADStatus status{};
		status.hold = 0x1234;
		status.leftStick.x = 2.0f;
		status.leftStick.y = -2.0f;
		status.rightStick.x = std::numeric_limits<float>::quiet_NaN();
		status.rightStick.y = 0.5f;
		host.ObserveVpad(0, status, 0, 1);
		const std::array payload{std::byte{0}};
		auto request = Request(1,
							   static_cast<std::uint16_t>(cemuextend::wire::InputOperation::GetObserved),
							   payload, cemuextend::wire::ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		auto response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		CHECK(response.size() == sizeof(ResponseHeader) + sizeof(cemuextend::wire::ObservedVpadState));
		const auto& observed = *reinterpret_cast<const cemuextend::wire::ObservedVpadState*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(observed.hold.get() == 0x1234);
		CHECK(observed.leftX.get() == 1.0f && observed.leftY.get() == -1.0f);
		CHECK(observed.rightX.get() == 0.0f && observed.rightY.get() == 0.5f);

		context.SetServicePermissions({0, 0x1ffU, 0x1ffU});
		host.PermissionsChanged(context, 1);
		request = Request(2,
						  static_cast<std::uint16_t>(cemuextend::wire::InputOperation::GetObserved),
						  payload, cemuextend::wire::ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::PermissionDenied));
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestMappedInputReplacement()
	{
		using namespace cemuextend::wire;
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(33, 1, "mapped-replacement-principal");
		context.SetGrantedPermissions(5);
		context.SetServicePermissions({0x1ffU, 0, 0x1ffU});
		const auto session = Open(host, context);

		auto inject = [&](std::uint32_t correlation, const ObservedVpadState& state) {
			std::vector<std::byte> payload(1 + sizeof(state));
			payload[0] = std::byte{0};
			std::memcpy(payload.data() + 1, &state, sizeof(state));
			auto request = Request(correlation, static_cast<std::uint16_t>(InputOperation::InjectMapped), payload, ServiceId::Input);
			CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
			return PollUntil(host, context, session);
		};

		ObservedVpadState exclusive{};
		exclusive.hold = 0x11;
		exclusive.trigger = 0x10;
		exclusive.release = 0x20;
		exclusive.leftX = -0.25f;
		exclusive.leftY = 0.5f;
		exclusive.rightX = 0.75f;
		exclusive.rightY = -1.0f;
		exclusive.flags = static_cast<std::uint8_t>(MappedInputFlag::ReplacePhysical);
		auto response = inject(1, exclusive);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));

		VPADStatus status{};
		status.hold = 0x1000;
		status.trig = 0x2000;
		status.release = 0x4000;
		status.leftStick.x = 0.9f;
		status.leftStick.y = 0.8f;
		status.rightStick.x = 0.7f;
		status.rightStick.y = 0.6f;
		status.gyroChange.x = 12.0f;
		status.tpData.x = 321;
		status.tpData.y = 123;
		status.tpData.touch = 1;
		host.ApplyMappedVpad(0, status);
		CHECK(status.hold == 0x11 && status.trig == 0x10 && status.release == 0x20);
		CHECK(status.leftStick.x == -0.25f && status.leftStick.y == 0.5f);
		CHECK(status.rightStick.x == 0.75f && status.rightStick.y == -1.0f);
		CHECK(status.gyroChange.x == 12.0f);
		CHECK(status.tpData.x == 321 && status.tpData.y == 123 && status.tpData.touch == 1);

		ObservedVpadState additive{};
		additive.hold = 0x01;
		additive.trigger = 0x02;
		additive.release = 0x04;
		additive.leftX = 0.1f;
		additive.leftY = 0.2f;
		additive.rightX = 0.3f;
		additive.rightY = 0.4f;
		response = inject(2, additive);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		status = {};
		status.hold = 0x100;
		status.trig = 0x200;
		status.release = 0x400;
		host.ApplyMappedVpad(0, status);
		CHECK(status.hold == 0x101 && status.trig == 0x202 && status.release == 0x404);
		CHECK(status.leftStick.x == 0.1f && status.rightStick.y == 0.4f);

		ObservedVpadState invalid{};
		invalid.flags = 0x80;
		response = inject(3, invalid);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::InvalidArgument));
		invalid = {};
		invalid.reserved[0] = std::byte{1};
		response = inject(4, invalid);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::InvalidArgument));
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestMouseAndPointerPolicy()
	{
		using namespace cemuextend::wire;
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(32, 1, "mouse-pointer-principal");
		context.SetGrantedPermissions(5);
		const auto session = Open(host, context);

		Encoder subscription;
		subscription.U16(static_cast<std::uint16_t>(ServiceId::Input));
		auto request = Request(1, static_cast<std::uint16_t>(CoreOperation::Subscribe),
							   subscription.data());
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		auto response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));

		PointerPolicyPayload policy{};
		policy.mode = static_cast<std::uint8_t>(PointerMode::CapturedRelative);
		policy.cursor = static_cast<std::uint8_t>(PointerCursor::Arrow);
		policy.surface = static_cast<std::uint8_t>(PointerSurface::Tv);
		policy.flags = static_cast<std::uint32_t>(PointerPolicyFlag::PreferRawMouse) |
					   static_cast<std::uint32_t>(PointerPolicyFlag::ConfineToContent);
		request = Request(2, static_cast<std::uint16_t>(WindowOperation::SetPointerPolicy),
						  {reinterpret_cast<const std::byte*>(&policy), sizeof(policy)}, ServiceId::Window);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		CHECK(host.EffectivePointerPolicy().mode == policy.mode);
		CHECK((host.EffectivePointerPolicy().flags.get() & static_cast<std::uint32_t>(
															   PointerPolicyFlag::ConfineToContent)) != 0);

		VPADStatus physicalVpad{};
		physicalVpad.hold = 0x1234;
		host.ObserveVpad(0, physicalVpad, 0, 1);
		std::array<std::byte, cemuextend::transport::kMaximumMessageSize> noEvent{};
		std::uint32_t noEventSize{};
		CHECK(host.Poll(context, session, noEvent, noEventSize) ==
			  static_cast<std::int32_t>(Error::NotFound));
		CHECK(noEventSize == 0);

		host.MouseEvent(PointerSurface::Tv, 640, 360, 7, -4, 0, 1,
						static_cast<std::uint32_t>(MouseButton::Left),
						static_cast<std::uint32_t>(MouseButton::Left), 1280, 720, true, true,
						static_cast<std::uint8_t>(MouseEventFlag::RawRelative));
		response = PollUntil(host, context, session);
		const auto* eventHeader = reinterpret_cast<const ResponseHeader*>(response.data());
		CHECK(eventHeader->flags.get() == static_cast<std::uint16_t>(
											  cemuextend::transport::ResponseFlag::Event));
		CHECK(eventHeader->operation.get() == static_cast<std::uint16_t>(InputEvent::MouseV2));
		CHECK(response.size() == sizeof(ResponseHeader) + sizeof(MouseEventPayloadV2));
		const auto& mouseEvent = *reinterpret_cast<const MouseEventPayloadV2*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(mouseEvent.x.get() == 640 && mouseEvent.y.get() == 360);
		CHECK(mouseEvent.deltaX.get() == 7 && mouseEvent.deltaY.get() == -4);
		CHECK(mouseEvent.normalizedX.get() == 0.5f && mouseEvent.normalizedY.get() == 0.5f);
		CHECK(mouseEvent.buttons.get() == static_cast<std::uint32_t>(MouseButton::Left));
		CHECK(mouseEvent.flags == static_cast<std::uint8_t>(MouseEventFlag::RawRelative));

		request = Request(3, static_cast<std::uint16_t>(InputOperation::GetHostMouse),
						  {}, ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		const auto& snapshot = *reinterpret_cast<const MouseEventPayloadV2*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(snapshot.x.get() == 640 && snapshot.buttons.get() ==
											 static_cast<std::uint32_t>(MouseButton::Left));
		CHECK(snapshot.flags == static_cast<std::uint8_t>(MouseEventFlag::RawRelative));

		host.TextEvent(0x3042, false);
		response = PollUntil(host, context, session);
		const auto* textHeader = reinterpret_cast<const ResponseHeader*>(response.data());
		CHECK(textHeader->operation.get() == static_cast<std::uint16_t>(InputEvent::Text));
		CHECK(response.size() == sizeof(ResponseHeader) + sizeof(TextEventPayload));
		const auto& textEvent = *reinterpret_cast<const TextEventPayload*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(textEvent.codepoint.get() == 0x3042 && !textEvent.repeat);

		host.PointerFocusChanged(false);
		response = PollUntil(host, context, session);
		const auto& released = *reinterpret_cast<const MouseEventPayloadV2*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(!released.focused && released.buttons.get() == 0);
		CHECK(released.changedButtons.get() == static_cast<std::uint32_t>(MouseButton::Left));
		CHECK(host.EffectivePointerPolicy().mode == static_cast<std::uint8_t>(PointerMode::Default));

		host.PointerFocusChanged(true);
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->flags.get() ==
			  static_cast<std::uint16_t>(cemuextend::transport::ResponseFlag::Event));
		CHECK(host.EffectivePointerPolicy().mode == policy.mode);

		// A high polling-rate mouse must not fill the shared response queue with
		// redundant motion samples. Preserve the newest absolute state and the full
		// accumulated relative distance in one event.
		for (std::int32_t index = 0; index < 512; ++index)
			host.MouseEvent(PointerSurface::Tv, index, index, 1, -1, 0, 0, 0, 0,
							1280, 720, true, true, static_cast<std::uint8_t>(MouseEventFlag::RawRelative));
		response = PollUntil(host, context, session);
		const auto& coalesced = *reinterpret_cast<const MouseEventPayloadV2*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(coalesced.x.get() == 511 && coalesced.y.get() == 511);
		CHECK(coalesced.deltaX.get() == 512 && coalesced.deltaY.get() == -512);
		std::array<std::byte, cemuextend::transport::kMaximumMessageSize> empty{};
		std::uint32_t emptySize{};
		CHECK(host.Poll(context, session, empty, emptySize) ==
			  static_cast<std::int32_t>(Error::NotFound));

		// wxWidgets emits repeated KEY_DOWN events while a key is held. They carry
		// no additional raw state, so only the down/up transition pair should cross
		// the bounded transport queue.
		for (std::size_t repeat = 0; repeat < 512; ++repeat)
			host.KeyboardEvent(static_cast<std::uint16_t>(InputUsagePage::Keyboard),
							   0x04, true, 0);
		host.KeyboardEvent(static_cast<std::uint16_t>(InputUsagePage::Keyboard),
						   0x04, false, 0);
		response = PollUntil(host, context, session);
		const auto& keyDown = *reinterpret_cast<const KeyboardEventPayload*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(keyDown.usbHidUsage.get() == 0x04 && keyDown.pressed);
		response = PollUntil(host, context, session);
		const auto& keyUp = *reinterpret_cast<const KeyboardEventPayload*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(keyUp.usbHidUsage.get() == 0x04 && !keyUp.pressed);
		CHECK(host.Poll(context, session, empty, emptySize) ==
			  static_cast<std::int32_t>(Error::NotFound));

		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
		CHECK(host.EffectivePointerPolicy().mode == static_cast<std::uint8_t>(PointerMode::Default));
	}

	void TestInputV3SnapshotBatch()
	{
		using namespace cemuextend::wire;
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(320, 1, "input-v3-principal");
		context.SetGrantedPermissions(1);
		const auto session = OpenV3(host, context);
		host.KeyboardEvent(static_cast<std::uint16_t>(InputUsagePage::Keyboard),
						   0x04, true, 0);
		host.KeyboardEvent(static_cast<std::uint16_t>(InputUsagePage::Keyboard),
						   0x04, false, 0);

		InputReadRequestV3 read{};
		read.maximumBytes = 65520;
		auto request = Request(1, static_cast<std::uint16_t>(InputOperation::ReadBatchV3),
							   {reinterpret_cast<const std::byte*>(&read), sizeof(read)}, ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		auto response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<const ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		const auto* baseline = reinterpret_cast<const InputSnapshotV3Header*>(
			response.data() + sizeof(ResponseHeader));
		CHECK((baseline->flags.get() & static_cast<std::uint32_t>(InputBatchFlag::Baseline)) != 0);
		CHECK((baseline->flags.get() & static_cast<std::uint32_t>(InputBatchFlag::Routed)) != 0);
		CHECK(baseline->generation.get() != 0);
		CHECK(baseline->recordCount.get() == 4);

		for (int index = 0; index < 8000; ++index)
			host.MouseEvent(PointerSurface::Tv, index, index, 1, -1, 0, 0, 0, 0,
							1280, 720, true, true,
							static_cast<std::uint8_t>(MouseEventFlag::RawRelative), 77);
		host.KeyboardEvent(static_cast<std::uint16_t>(InputUsagePage::Consumer),
						   0x00e9, true, 0);
		host.KeyboardEvent(static_cast<std::uint16_t>(InputUsagePage::Consumer),
						   0x00e9, false, 0);

		read.generation = baseline->generation.get();
		read.acknowledgedSequence = baseline->nextSequence.get();
		request = Request(2, static_cast<std::uint16_t>(InputOperation::ReadBatchV3),
						  {reinterpret_cast<const std::byte*>(&read), sizeof(read)}, ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		const auto* snapshot = reinterpret_cast<const InputSnapshotV3Header*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(snapshot->surfaces[0].motionX.get() == 8000);
		CHECK(snapshot->surfaces[0].motionY.get() == -8000);
		CHECK(snapshot->recordCount.get() == 127);
		const auto records = response.data() + sizeof(ResponseHeader) +
							 sizeof(InputSnapshotV3Header) +
							 snapshot->pressedControlCount.get() * sizeof(InputControlV3);
		std::size_t offset{};
		std::int64_t totalX{};
		std::int64_t totalY{};
		const InputRecordV3* keyDown{};
		const InputRecordV3* keyUp{};
		for (std::uint32_t index = 0; index < snapshot->recordCount.get(); ++index)
		{
			const auto* record = reinterpret_cast<const InputRecordV3*>(records + offset);
			if (record->type == static_cast<std::uint8_t>(InputRecordType::PointerMotion))
			{
				CHECK(record->deviceId.get() == 77);
				CHECK(record->payloadBytes.get() == sizeof(InputPointerMotionV3));
				CHECK(record->auxiliary.get() >= 1 && record->auxiliary.get() <= 64);
				const auto* motion = reinterpret_cast<const InputPointerMotionV3*>(
					records + offset + sizeof(InputRecordV3));
				totalX += motion->deltaX.get();
				totalY += motion->deltaY.get();
			}
			else if (!keyDown)
				keyDown = record;
			else
				keyUp = record;
			offset += sizeof(InputRecordV3) + record->payloadBytes.get();
		}
		CHECK(totalX == 8000 && totalY == -8000);
		CHECK(keyDown != nullptr && keyUp != nullptr);
		CHECK(keyDown->type == static_cast<std::uint8_t>(InputRecordType::Key));
		CHECK(keyDown->usagePage.get() == static_cast<std::uint16_t>(InputUsagePage::Consumer));
		CHECK(keyDown->usage.get() == 0x00e9 && keyDown->value.get() == 1);
		CHECK(keyUp->usage.get() == 0x00e9 && keyUp->value.get() == 0);

		// Once a motion sequence has been delivered it must never be mutated by a
		// later coalesce; otherwise acknowledging the old sequence drops new motion.
		host.MouseEvent(PointerSurface::Tv, 8009, 8009, 9, -9, 0, 0, 0, 0,
						1280, 720, true, true,
						static_cast<std::uint8_t>(MouseEventFlag::RawRelative), 77);
		read.acknowledgedSequence = snapshot->nextSequence.get();
		request = Request(3, static_cast<std::uint16_t>(InputOperation::ReadBatchV3),
						  {reinterpret_cast<const std::byte*>(&read), sizeof(read)}, ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		const auto* postDelivery = reinterpret_cast<const InputSnapshotV3Header*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(postDelivery->recordCount.get() == 1);
		const auto* postDeliveryRecord = reinterpret_cast<const InputRecordV3*>(
			response.data() + sizeof(ResponseHeader) + sizeof(InputSnapshotV3Header));
		CHECK(postDeliveryRecord->type ==
			  static_cast<std::uint8_t>(InputRecordType::PointerMotion));
		const auto* postDeliveryMotion = reinterpret_cast<const InputPointerMotionV3*>(
			reinterpret_cast<const std::byte*>(postDeliveryRecord) + sizeof(InputRecordV3));
		CHECK(postDeliveryMotion->deltaX.get() == 9 && postDeliveryMotion->deltaY.get() == -9);

		TextInputRequestHeaderV3 textInput{};
		textInput.requestId = 77;
		textInput.maximumLength = 1'000'000;
		textInput.flags = static_cast<std::uint8_t>(TextInputFlag::Active);
		request = Request(4, static_cast<std::uint16_t>(InputOperation::SetTextInputV3),
						  {reinterpret_cast<const std::byte*>(&textInput), sizeof(textInput)}, ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<const ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));

		const std::string largePreedit(70'000, 'x');
		host.TextCompositionEvent({}, largePreedit, 0,
								  static_cast<std::uint32_t>(largePreedit.size()));
		read.acknowledgedSequence = postDelivery->nextSequence.get();
		std::size_t receivedTextBytes{};
		std::uint32_t correlation = 5;
		bool more{};
		do
		{
			request = Request(correlation++,
							  static_cast<std::uint16_t>(InputOperation::ReadBatchV3),
							  {reinterpret_cast<const std::byte*>(&read), sizeof(read)}, ServiceId::Input);
			CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
			response = PollUntil(host, context, session);
			const auto* page = reinterpret_cast<const InputSnapshotV3Header*>(
				response.data() + sizeof(ResponseHeader));
			std::size_t offset = sizeof(ResponseHeader) + sizeof(InputSnapshotV3Header) +
								 page->pressedControlCount.get() * sizeof(InputControlV3);
			for (std::uint32_t index = 0; index < page->recordCount.get(); ++index)
			{
				const auto* record = reinterpret_cast<const InputRecordV3*>(response.data() + offset);
				CHECK(record->type == static_cast<std::uint8_t>(InputRecordType::TextChunk));
				CHECK(record->payloadBytes.get() >= sizeof(InputTextChunkV3));
				receivedTextBytes += record->payloadBytes.get() - sizeof(InputTextChunkV3);
				offset += sizeof(InputRecordV3) + record->payloadBytes.get();
			}
			read.acknowledgedSequence = page->nextSequence.get();
			more = (page->flags.get() & static_cast<std::uint32_t>(InputBatchFlag::More)) != 0;
		}
		while (more);
		CHECK(receivedTextBytes == largePreedit.size());
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestInputV3OwnershipAndDeviceIdentity()
	{
		using namespace cemuextend::wire;
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(321, 1, "input-v3-routing-principal");
		context.SetGrantedPermissions(1);
		const auto session = OpenV3(host, context);
		InputReadRequestV3 read{};
		read.maximumBytes = 65520;
		auto request = Request(1, static_cast<std::uint16_t>(InputOperation::ReadBatchV3),
							   {reinterpret_cast<const std::byte*>(&read), sizeof(read)}, ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		auto response = PollUntil(host, context, session);
		const auto* baseline = reinterpret_cast<const InputSnapshotV3Header*>(
			response.data() + sizeof(ResponseHeader));

		InputOwnershipV3 ownership{};
		ownership.keyboardOwner = static_cast<std::uint8_t>(InputOwner::WebUi);
		ownership.pointerOwner = static_cast<std::uint8_t>(InputOwner::Title);
		ownership.textOwner = static_cast<std::uint8_t>(InputOwner::WebUi);
		ownership.flags = static_cast<std::uint8_t>(InputOwnershipFlag::WebUiTextFocused);
		host.InputOwnershipChanged(PointerSurface::Tv, ownership);
		constexpr auto page = static_cast<std::uint16_t>(InputUsagePage::Keyboard);
		host.KeyboardEvent(page, 0x04, true, 0, 11);
		host.KeyboardEvent(page, 0x04, true, 0, 22);
		host.KeyboardEvent(page, 0x04, false, 0, 11);
		host.KeyboardEvent(page, 0x04, false, 0, 22);
		host.MouseEvent(PointerSurface::Tv, 0, 0, 0, 0, 0, 0, 1, 1,
						0, 0, false, true, 0, 11, 1);
		host.MouseEvent(PointerSurface::Tv, 0, 0, 0, 0, 0, 0, 1, 1,
						0, 0, false, true, 0, 22, 1);
		host.MouseEvent(PointerSurface::Tv, 0, 0, 0, 0, 0, 0, 1, 1,
						0, 0, false, true, 0, 11, 0);
		host.MouseEvent(PointerSurface::Tv, 0, 0, 0, 0, 0, 0, 0, 1,
						0, 0, false, true, 0, 22, 0);

		read.generation = baseline->generation.get();
		read.acknowledgedSequence = baseline->nextSequence.get();
		request = Request(2, static_cast<std::uint16_t>(InputOperation::ReadBatchV3),
						  {reinterpret_cast<const std::byte*>(&read), sizeof(read)}, ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		const auto* snapshot = reinterpret_cast<const InputSnapshotV3Header*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(snapshot->recordCount.get() == 9);
		CHECK(snapshot->pressedControlCount.get() == 0);
		std::size_t offset = sizeof(ResponseHeader) + sizeof(InputSnapshotV3Header);
		const auto* ownerRecord = reinterpret_cast<const InputRecordV3*>(response.data() + offset);
		CHECK(ownerRecord->type == static_cast<std::uint8_t>(InputRecordType::Ownership));
		CHECK(ownerRecord->usage.get() == static_cast<std::uint16_t>(PointerSurface::Tv));
		CHECK(ownerRecord->payloadBytes.get() == sizeof(InputOwnershipV3));
		offset += sizeof(InputRecordV3) + ownerRecord->payloadBytes.get();
		const std::array<std::uint16_t, 4> devices{11, 22, 11, 22};
		const std::array<std::int32_t, 4> levels{1, 1, 0, 0};
		for (std::size_t index = 0; index < devices.size(); ++index)
		{
			const auto* record = reinterpret_cast<const InputRecordV3*>(response.data() + offset);
			CHECK(record->type == static_cast<std::uint8_t>(InputRecordType::Key));
			CHECK(record->deviceId.get() == devices[index]);
			CHECK(record->value.get() == levels[index]);
			offset += sizeof(InputRecordV3) + record->payloadBytes.get();
		}
		for (std::size_t index = 0; index < devices.size(); ++index)
		{
			const auto* record = reinterpret_cast<const InputRecordV3*>(response.data() + offset);
			CHECK(record->type == static_cast<std::uint8_t>(InputRecordType::MouseButton));
			CHECK(record->deviceId.get() == devices[index]);
			CHECK(record->value.get() == levels[index]);
			offset += sizeof(InputRecordV3) + record->payloadBytes.get();
		}
		host.KeyboardFocusLost();
		read.acknowledgedSequence = snapshot->nextSequence.get();
		request = Request(3, static_cast<std::uint16_t>(InputOperation::ReadBatchV3),
						  {reinterpret_cast<const std::byte*>(&read), sizeof(read)}, ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		const auto* released = reinterpret_cast<const InputSnapshotV3Header*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(released->recordCount.get() == 2);
		offset = sizeof(ResponseHeader) + sizeof(InputSnapshotV3Header);
		const auto* releasedOwner = reinterpret_cast<const InputRecordV3*>(response.data() + offset);
		CHECK(releasedOwner->type == static_cast<std::uint8_t>(InputRecordType::Ownership));
		offset += sizeof(InputRecordV3) + releasedOwner->payloadBytes.get();
		const auto* reset = reinterpret_cast<const InputRecordV3*>(response.data() + offset);
		CHECK(reset->type == static_cast<std::uint8_t>(InputRecordType::DeviceReset));
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestTextInputComposition()
	{
		using namespace cemuextend::wire;
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(33, 1, "text-input-principal");
		context.SetGrantedPermissions(5);
		const auto session = Open(host, context);
		struct WakeState
		{
			std::mutex mutex;
			std::condition_variable ready;
			std::size_t count{};
		};
		const auto wake = std::make_shared<WakeState>();
		host.SetTextInputWakeCallback([wake, &host] {
			// Wake handlers must run outside the host mutex so Application event
			// subscribers can safely query the current text-input state.
			(void)host.EffectiveTextInput();
			{
				std::lock_guard lock(wake->mutex);
				++wake->count;
			}
			wake->ready.notify_one();
		});

		Encoder subscription;
		subscription.U16(static_cast<std::uint16_t>(ServiceId::Input));
		auto request = Request(1, static_cast<std::uint16_t>(CoreOperation::Subscribe),
							   subscription.data());
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		auto response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));

		constexpr std::string_view initial = "search";
		TextInputRequestHeader input{};
		input.requestId = 42;
		input.maximumLength = 128;
		input.caretX = 320;
		input.caretY = 180;
		input.lineHeight = 22;
		input.textBytes = static_cast<std::uint32_t>(initial.size());
		input.flags = static_cast<std::uint8_t>(TextInputFlag::Active);
		std::vector<std::byte> payload(sizeof(input) + initial.size());
		std::memcpy(payload.data(), &input, sizeof(input));
		std::memcpy(payload.data() + sizeof(input), initial.data(), initial.size());
		request = Request(2, static_cast<std::uint16_t>(InputOperation::SetTextInput),
						  payload, ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		{
			std::unique_lock lock(wake->mutex);
			wake->ready.wait(lock, [&] { return wake->count >= 1; });
		}
		const auto state = host.EffectiveTextInput();
		CHECK(state.active && state.requestId == 42 && state.maximumLength == 128);
		CHECK(state.caretX == 320 && state.caretY == 180 && state.lineHeight == 22);
		CHECK(state.initialText == initial);
		const auto sequence = state.sequence;

		// Caret/preedit updates for the same Aqua field must not create a new
		// native session: doing so resets the OS composition after a few keys.
		input.caretX = 340;
		request = Request(3, static_cast<std::uint16_t>(InputOperation::SetTextInput),
						  payload, ServiceId::Input);
		std::memcpy(request.data() + sizeof(RequestHeader), &input, sizeof(input));
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		CHECK(host.EffectiveTextInput().sequence == sequence);
		CHECK(host.EffectiveTextInput().caretX == 340);
		{
			std::unique_lock lock(wake->mutex);
			wake->ready.wait(lock, [&] { return wake->count >= 2; });
		}

		constexpr std::string_view committed = "search";
		constexpr std::string_view preedit = "\xE6\x97\xA5\xE6\x9C\xAC";
		host.TextCompositionEvent(committed, preedit,
								  static_cast<std::uint32_t>(committed.size()),
								  static_cast<std::uint32_t>(preedit.size()));
		response = PollUntil(host, context, session);
		const auto* eventResponse = reinterpret_cast<const ResponseHeader*>(response.data());
		CHECK(eventResponse->flags.get() == static_cast<std::uint16_t>(
												cemuextend::transport::ResponseFlag::Event));
		CHECK(eventResponse->operation.get() ==
			  static_cast<std::uint16_t>(InputEvent::TextComposition));
		CHECK(response.size() == sizeof(ResponseHeader) +
									 sizeof(TextCompositionEventHeader) + committed.size() + preedit.size());
		const auto* event = reinterpret_cast<const TextCompositionEventHeader*>(
			response.data() + sizeof(ResponseHeader));
		CHECK(event->requestId.get() == 42);
		CHECK(event->committedBytes.get() == committed.size());
		CHECK(event->preeditBytes.get() == preedit.size());
		CHECK(event->preeditStart.get() == committed.size());
		CHECK(event->preeditCursor.get() == preedit.size());
		const std::string_view returned{
			reinterpret_cast<const char*>(response.data() + sizeof(ResponseHeader) + sizeof(*event)),
			committed.size()};
		CHECK(returned == committed);
		const std::string_view returnedPreedit{
			reinterpret_cast<const char*>(response.data() + sizeof(ResponseHeader) +
										  sizeof(*event) + committed.size()),
			preedit.size()};
		CHECK(returnedPreedit == preedit);

		input.flags = 0;
		input.textBytes = 0;
		request = Request(4, static_cast<std::uint16_t>(InputOperation::SetTextInput),
						  {reinterpret_cast<const std::byte*>(&input), sizeof(input)}, ServiceId::Input);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		CHECK(!host.EffectiveTextInput().active);
		{
			std::unique_lock lock(wake->mutex);
			wake->ready.wait(lock, [&] { return wake->count >= 3; });
		}
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
		host.SetTextInputWakeCallback({});
	}

	std::vector<std::byte> HttpStartPayload(std::string_view url)
	{
		cemuextend::wire::HttpStartRequest request{};
		request.timeoutMs = 1000;
		request.maximumBodyBytes = 4096;
		request.urlBytes = static_cast<std::uint32_t>(url.size());
		std::vector<std::byte> payload(sizeof(request) + url.size());
		std::memcpy(payload.data(), &request, sizeof(request));
		std::memcpy(payload.data() + sizeof(request), url.data(), url.size());
		return payload;
	}

	void TestHttpValidationAndOwnership()
	{
		using cemuextend::wire::HttpOperation;
		using cemuextend::wire::HttpPollRequest;
		using cemuextend::wire::HttpPollResponse;
		using cemuextend::wire::HttpState;

		constexpr std::uint64_t session = 0x1122;
		constexpr std::uint64_t otherSession = 0x3344;
		CHECK(cemuextend_hle::Cex2Http::ActiveTransfers() == 0);
		auto invalid = HttpStartPayload("file:///etc/passwd");
		auto result = cemuextend_hle::Cex2Http::Dispatch(session, "http-test",
														 static_cast<std::uint16_t>(HttpOperation::Start), invalid);
		CHECK(result.status == Status::InvalidArgument);
		CHECK(cemuextend_hle::Cex2Http::ActiveTransfers() == 0);

		auto start = HttpStartPayload("https://example.invalid/profile");
		result = cemuextend_hle::Cex2Http::Dispatch(session, "http-test",
													static_cast<std::uint16_t>(HttpOperation::Start), start);
		CHECK(result.status == Status::Ok);
		CHECK(result.payload.size() == sizeof(cemuextend::wire::HttpStartResponse));
		const auto* started =
			reinterpret_cast<const cemuextend::wire::HttpStartResponse*>(result.payload.data());
		const std::uint32_t handle = started->handle.get();
		CHECK(handle != 0 && cemuextend_hle::Cex2Http::ActiveTransfers() == 1);

		HttpPollRequest poll{};
		poll.handle = handle;
		poll.length = 64;
		const auto pollBytes =
			std::span<const std::byte>(reinterpret_cast<const std::byte*>(&poll), sizeof(poll));
		result = cemuextend_hle::Cex2Http::Dispatch(otherSession, "other",
													static_cast<std::uint16_t>(HttpOperation::Poll), pollBytes);
		CHECK(result.status == Status::NotFound);
		result = cemuextend_hle::Cex2Http::Dispatch(session, "http-test",
													static_cast<std::uint16_t>(HttpOperation::Poll), pollBytes);
		CHECK(result.status == Status::Ok && result.payload.size() == sizeof(HttpPollResponse));
		const auto* polled = reinterpret_cast<const HttpPollResponse*>(result.payload.data());
		CHECK(static_cast<HttpState>(polled->state) == HttpState::Failed);
		CHECK(polled->error.get() == static_cast<std::int32_t>(Status::NotSupported));

		cemuextend::wire::Be32 releaseHandle{handle};
		const auto releaseBytes = std::span<const std::byte>(
			reinterpret_cast<const std::byte*>(&releaseHandle), sizeof(releaseHandle));
		result = cemuextend_hle::Cex2Http::Dispatch(session, "http-test",
													static_cast<std::uint16_t>(HttpOperation::Release), releaseBytes);
		CHECK(result.status == Status::Ok);
		CHECK(cemuextend_hle::Cex2Http::ActiveTransfers() == 0);
	}

	void TestHttpPermissionGate()
	{
		using cemuextend::wire::HttpOperation;
		using cemuextend::wire::ServiceId;

		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(90, 1, "http-permission-principal");
		const auto session = Open(host, context);
		auto payload = HttpStartPayload("file:///etc/passwd");

		auto request = Request(1, static_cast<std::uint16_t>(HttpOperation::Start),
							   payload, ServiceId::Http);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		auto response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::PermissionDenied));

		context.SetGrantedPermissions(cemuextend_hle::kCemodNetworkPermission);
		request = Request(2, static_cast<std::uint16_t>(HttpOperation::Start),
						  payload, ServiceId::Http);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		// Reaching URL validation proves Network did not get rejected by the
		// unrelated title-wide service mask.
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::InvalidArgument));
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
	}

	std::vector<std::byte> MediaOpenPayload(std::string_view url)
	{
		using namespace cemuextend::wire;
		MediaOpenRequest request{};
		request.width = 16;
		request.height = 16;
		request.framesPerSecond = 1;
		request.paletteEntries = 2;
		request.urlBytes = static_cast<std::uint32_t>(url.size());
		std::vector<std::byte> payload(sizeof(request) + url.size() + 8U);
		std::memcpy(payload.data(), &request, sizeof(request));
		std::memcpy(payload.data() + sizeof(request), url.data(), url.size());
		payload[payload.size() - 4U] = std::byte{0xff};
		payload[payload.size() - 1U] = std::byte{0xff};
		return payload;
	}

	void TestMediaValidationOwnershipAndPermission()
	{
		using namespace cemuextend::wire;
		constexpr std::uint64_t owner = 0x4455;
		CHECK(cemuextend_hle::Cex2Media::ActiveSessions() == 0);
		auto invalid = MediaOpenPayload("https://example.com/video");
		auto result = cemuextend_hle::Cex2Media::Dispatch(
			owner, "media-test", static_cast<std::uint16_t>(MediaOperation::Open), invalid);
		CHECK(result.status == Status::InvalidArgument);

		auto valid = MediaOpenPayload("https://youtu.be/abcdefghijk");
		result = cemuextend_hle::Cex2Media::Dispatch(
			owner, "media-test", static_cast<std::uint16_t>(MediaOperation::Open), valid);
		CHECK(result.status == Status::Ok && result.payload.size() == sizeof(MediaOpenResponse));
		const auto handle = reinterpret_cast<const MediaOpenResponse*>(result.payload.data())->handle.get();
		CHECK(handle != 0 && cemuextend_hle::Cex2Media::ActiveSessions() == 1);

		MediaStatusRequest statusRequest{};
		statusRequest.handle = handle;
		const auto statusBytes = std::span<const std::byte>(
			reinterpret_cast<const std::byte*>(&statusRequest), sizeof(statusRequest));
		result = cemuextend_hle::Cex2Media::Dispatch(
			owner + 1, "other", static_cast<std::uint16_t>(MediaOperation::Status), statusBytes);
		CHECK(result.status == Status::NotFound);

		Be32 close{handle};
		result = cemuextend_hle::Cex2Media::Dispatch(
			owner,
			"media-test",
			static_cast<std::uint16_t>(MediaOperation::Close),
			{reinterpret_cast<const std::byte*>(&close), sizeof(close)});
		CHECK(result.status == Status::Ok && cemuextend_hle::Cex2Media::ActiveSessions() == 0);

		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		ModExecutionContext context(93, 1, "media-permission-principal");
		const auto session = Open(host, context);
		auto request = Request(1,
							   static_cast<std::uint16_t>(MediaOperation::Open), valid, ServiceId::Media);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		auto response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::PermissionDenied));

		context.SetGrantedPermissions(cemuextend_hle::kCemodNetworkPermission);
		request = Request(2,
						  static_cast<std::uint16_t>(MediaOperation::Open), valid, ServiceId::Media);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::PermissionDenied));

		context.SetGrantedPermissions(cemuextend_hle::kCemodNetworkPermission | 4U);
		request = Request(3,
						  static_cast<std::uint16_t>(MediaOperation::Open), invalid, ServiceId::Media);
		CHECK(host.Submit(context, session, request) == static_cast<std::int32_t>(Error::Ok));
		response = PollUntil(host, context, session);
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::InvalidArgument));
		CHECK(host.Close(context, session) == static_cast<std::int32_t>(Error::Ok));
	}

	void TestWebUiServiceLifecycle()
	{
		using namespace cemuextend::wire;
		auto& host = cemuextend_hle::Cex2Host::Instance();
		host.CloseAll();
		auto webUiHost = std::make_shared<TestWebUiHost>();
		host.ConfigureHost({}, {}, webUiHost);

		CemodPackage package;
		CemodWebUiView view;
		view.entry = "ui/index.html";
		view.windowMode = true;
		view.window.emplace();
		CemodWebUi webUi;
		webUi.bridgeVersion = 1;
		webUi.views.emplace("main", std::move(view));
		package.manifest.webUi = std::move(webUi);
		package.uiAssets.emplace("ui/index.html", std::vector<std::byte>{std::byte{'x'}});

		ModExecutionContext context(92, 1, "ui-service-principal");
		context.SetPackage(&package);
		context.SetGrantedPermissions(cemuextend_hle::kCemodUiPermission);
		const auto firstSession = Open(host, context);
		auto pollUi = [&](std::uint32_t session, std::string_view step) {
			std::vector<std::byte> response(cemuextend::transport::kMaximumMessageSize);
			std::uint32_t size{};
			const auto result = host.Poll(context, session, response, size);
			if (result != static_cast<std::int32_t>(Error::Ok))
				std::cerr << "Web UI poll failed at " << step << ": " << result << '\n';
			CHECK(result == static_cast<std::int32_t>(Error::Ok));
			response.resize(size);
			return response;
		};

		auto makeCreate = [](std::uint32_t correlation) {
			constexpr std::string_view viewId = "main";
			constexpr std::string_view json = "{}";
			UiCreateRequestHeader header{};
			header.viewBytes = static_cast<std::uint32_t>(viewId.size());
			header.contextBytes = static_cast<std::uint32_t>(json.size());
			header.mode = static_cast<std::uint8_t>(UiMode::Window);
			header.visible = 1;
			std::vector<std::byte> payload(sizeof(header) + viewId.size() + json.size());
			std::memcpy(payload.data(), &header, sizeof(header));
			std::memcpy(payload.data() + sizeof(header), viewId.data(), viewId.size());
			std::memcpy(payload.data() + sizeof(header) + viewId.size(), json.data(), json.size());
			return Request(correlation, static_cast<std::uint16_t>(UiOperation::Create),
						   payload, ServiceId::Ui);
		};

		auto request = makeCreate(1);
		CHECK(host.Submit(context, firstSession, request) == static_cast<std::int32_t>(Error::Ok));

		UiCreateResponse created{};
		created.handle = 41;
		const auto createdBytes = std::span<const std::byte>(
			reinterpret_cast<const std::byte*>(&created), sizeof(created));
		webUiHost->Complete(0, Status::Ok, createdBytes);
		auto response = pollUi(firstSession, "first create");
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		CHECK(host.Close(context, firstSession) == static_cast<std::int32_t>(Error::Ok));
		const auto secondSession = Open(host, context);
		request = makeCreate(2);
		CHECK(host.Submit(context, secondSession, request) == static_cast<std::int32_t>(Error::Ok));
		CHECK(webUiHost->requests.size() == 2);
		CHECK(webUiHost->contents[0] == webUiHost->contents[1]);
		webUiHost->Complete(1, Status::Ok, createdBytes);
		(void)pollUi(secondSession, "second create");

		Encoder subscription;
		subscription.U16(static_cast<std::uint16_t>(ServiceId::Ui));
		request = Request(3, static_cast<std::uint16_t>(CoreOperation::Subscribe),
						  subscription.data());
		CHECK(host.Submit(context, secondSession, request) == static_cast<std::int32_t>(Error::Ok));
		response = pollUi(secondSession, "subscribe");
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Ok));
		UiHandleRequest ready{};
		ready.handle = 41;
		const auto* readyBegin = reinterpret_cast<const std::byte*>(&ready);
		std::vector<std::byte> readyPayload(readyBegin, readyBegin + sizeof(ready));
		webUiHost->Emit({context.AddressSpaceId(), context.Generation(), secondSession,
						 UiEvent::Ready, std::move(readyPayload)});
		response = pollUi(secondSession, "ready event");
		const auto* event = reinterpret_cast<const ResponseHeader*>(response.data());
		CHECK(event->serviceId.get() == static_cast<std::uint16_t>(ServiceId::Ui));
		CHECK(event->operation.get() == static_cast<std::uint16_t>(UiEvent::Ready));

		request = makeCreate(4);
		CHECK(host.Submit(context, secondSession, request) == static_cast<std::int32_t>(Error::Ok));
		CHECK(host.Cancel(context, secondSession, 4) == static_cast<std::int32_t>(Error::Ok));
		CHECK(webUiHost->cancelled == std::vector<std::uint32_t>{4});
		response = pollUi(secondSession, "cancel");
		CHECK(reinterpret_cast<ResponseHeader*>(response.data())->status.get() ==
			  static_cast<std::uint16_t>(Status::Cancelled));

		CHECK(webUiHost->closedSessions == std::vector<std::uint32_t>{firstSession});
		host.PermissionsChanged(context, 0);
		CHECK(webUiHost->closedOwners == 1);
		CHECK(host.Close(context, secondSession) == static_cast<std::int32_t>(Error::Ok));
		host.ConfigureHost({}, {}, {});
	}

} // namespace

int main(int argc, char** argv)
{
	const bool pointerOnly = argc == 2 && std::string_view(argv[1]) == "--pointer-only";
	const bool diagnosticsOnly =
		argc == 2 && std::string_view(argv[1]) == "--diagnostics-only";
	if (diagnosticsOnly)
	{
		TestDiagnosticsGraphicsApi();
	}
	else if (pointerOnly)
	{
		TestMappedInputReplacement();
		TestMouseAndPointerPolicy();
		TestInputV3SnapshotBatch();
		TestInputV3OwnershipAndDeviceIdentity();
		TestTextInputComposition();
	}
	else
	{
		TestOwnershipCopyAndCancel();
		TestBackpressureAndProtocolReap();
		TestExactOnceAdmission();
		TestPrincipalStorageAndPagination();
		TestTitleServicePermissionsAndRevocation();
		TestObservedInputSnapshot();
		TestMappedInputReplacement();
		TestMouseAndPointerPolicy();
		TestHttpValidationAndOwnership();
		TestHttpPermissionGate();
		TestMediaValidationOwnershipAndPermission();
		TestWebUiServiceLifecycle();
		TestDiagnosticsGraphicsApi();
		TestTimingFrameRateOverride();
	}
	cemuextend_hle::Cex2Host::Instance().ShutdownForTesting();
	// OpenSSL keeps provider/configuration state alive until process shutdown.
	// Release it explicitly so LeakSanitizer can distinguish product leaks from
	// the library's process-lifetime caches in this short-lived test binary.
	OPENSSL_cleanup();
	return 0;
}
