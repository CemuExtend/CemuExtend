#include "webview/RpcDispatcher.h"

#include <stdexcept>

#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace WebFrontend
{
	namespace
	{
		std::string Serialize(const rapidjson::Document& document)
		{
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			document.Accept(writer);
			return {buffer.GetString(), buffer.GetSize()};
		}

		void AddString(rapidjson::Value& object, const char* name,
					   std::string_view value, rapidjson::Document::AllocatorType& allocator)
		{
			object.AddMember(rapidjson::StringRef(name),
							 rapidjson::Value(value.data(), static_cast<rapidjson::SizeType>(value.size()), allocator),
							 allocator);
		}
	} // namespace

	void RpcDispatcher::Register(std::string method, Handler handler)
	{
		if (method.empty() || !handler)
			throw std::invalid_argument("RPC method and handler are required");
		std::scoped_lock lock(m_mutex);
		if (m_shuttingDown)
			throw std::logic_error("cannot register RPC method during shutdown");
		if (!m_handlers.emplace(std::move(method), std::move(handler)).second)
			throw std::logic_error("duplicate RPC method");
	}

	void RpcDispatcher::VerifyMethods(std::span<const std::string_view> methods) const
	{
		std::scoped_lock lock(m_mutex);
		if (m_handlers.size() != methods.size())
			throw std::logic_error("native RPC handlers do not match the generated contract");
		for (const auto method : methods)
		{
			if (!m_handlers.contains(std::string(method)))
				throw std::logic_error("generated RPC method has no native handler: " + std::string(method));
		}
	}

	std::string RpcDispatcher::Dispatch(std::string_view requestJson)
	{
		if (requestJson.size() > 1024 * 1024)
			return Error({}, "request_too_large", "RPC request exceeded the 1 MiB limit");

		rapidjson::Document request;
		request.Parse(requestJson.data(), requestJson.size());
		if (request.HasParseError())
			return Error({}, "malformed_json", rapidjson::GetParseError_En(request.GetParseError()));
		if (!request.IsObject())
			return Error({}, "invalid_request", "RPC request must be an object");
		const auto id = request.FindMember("id");
		const auto method = request.FindMember("method");
		const auto params = request.FindMember("params");
		if (id == request.MemberEnd() || !id->value.IsString() ||
			id->value.GetStringLength() == 0 || id->value.GetStringLength() > 128)
			return Error({}, "invalid_id", "id must be a non-empty string of at most 128 bytes");
		const std::string_view requestId{id->value.GetString(), id->value.GetStringLength()};
		if (method == request.MemberEnd() || !method->value.IsString() ||
			method->value.GetStringLength() == 0 || method->value.GetStringLength() > 128)
			return Error(requestId, "invalid_method", "method must be a non-empty string of at most 128 bytes");
		if (params == request.MemberEnd() || !params->value.IsObject())
			return Error(requestId, "invalid_params", "params must be an object");

		Handler handler;
		{
			std::scoped_lock lock(m_mutex);
			if (m_shuttingDown)
				return Error(requestId, "shutting_down", "application shutdown has started");
			if (!m_requestIds.emplace(requestId).second)
				return Error(requestId, "duplicate_request", "request id has already been used");
			m_requestIdOrder.emplace_back(requestId);
			if (m_requestIdOrder.size() > 4096)
			{
				m_requestIds.erase(m_requestIdOrder.front());
				m_requestIdOrder.pop_front();
			}
			const std::string methodName(method->value.GetString(), method->value.GetStringLength());
			const auto found = m_handlers.find(methodName);
			if (found == m_handlers.end())
				return Error(requestId, "method_not_found", "RPC method is not allowed");
			handler = found->second;
		}

		try
		{
			const auto result = handler(params->value);
			rapidjson::Document validation;
			validation.Parse(result.data(), result.size());
			if (validation.HasParseError())
				return Error(requestId, "internal_error", "RPC handler produced invalid JSON");
			return Success(requestId, result);
		} catch (const std::invalid_argument& exception)
		{
			return Error(requestId, "invalid_params", exception.what());
		} catch (const std::exception& exception)
		{
			return Error(requestId, "operation_failed", exception.what());
		} catch (...)
		{
			return Error(requestId, "internal_error", "unknown native exception");
		}
	}

	void RpcDispatcher::BeginShutdown()
	{
		std::scoped_lock lock(m_mutex);
		m_shuttingDown = true;
	}

	bool RpcDispatcher::IsShuttingDown() const
	{
		std::scoped_lock lock(m_mutex);
		return m_shuttingDown;
	}

	std::string RpcDispatcher::Error(std::string_view id, std::string_view code,
									 std::string_view message)
	{
		rapidjson::Document document(rapidjson::kObjectType);
		auto& allocator = document.GetAllocator();
		AddString(document, "id", id, allocator);
		document.AddMember("ok", false, allocator);
		rapidjson::Value error(rapidjson::kObjectType);
		AddString(error, "code", code, allocator);
		AddString(error, "message", message, allocator);
		document.AddMember("error", error, allocator);
		return Serialize(document);
	}

	std::string RpcDispatcher::Success(std::string_view id, std::string_view resultJson)
	{
		rapidjson::Document result;
		result.Parse(resultJson.data(), resultJson.size());
		rapidjson::Document document(rapidjson::kObjectType);
		auto& allocator = document.GetAllocator();
		AddString(document, "id", id, allocator);
		document.AddMember("ok", true, allocator);
		rapidjson::Value value;
		value.CopyFrom(result, allocator);
		document.AddMember("result", value, allocator);
		return Serialize(document);
	}
} // namespace WebFrontend
