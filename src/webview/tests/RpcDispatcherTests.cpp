#include "webview/RpcDispatcher.h"

#include <cassert>
#include <string>

int main()
{
	WebFrontend::RpcDispatcher rpc;
	rpc.Register("echo", [](const rapidjson::Value& params) {
		if (!params.HasMember("value") || !params["value"].IsString())
			throw std::invalid_argument("value is required");
		return std::string{"\""} + params["value"].GetString() + "\"";
	});
	auto success = rpc.Dispatch(R"({"id":"1","method":"echo","params":{"value":"ok"}})");
	assert(success.find(R"("ok":true)") != std::string::npos);
	assert(success.find(R"("result":"ok")") != std::string::npos);
	auto duplicate = rpc.Dispatch(R"({"id":"1","method":"echo","params":{"value":"ok"}})");
	assert(duplicate.find("duplicate_request") != std::string::npos);
	auto unknown = rpc.Dispatch(R"({"id":"2","method":"native.eval","params":{}})");
	assert(unknown.find("method_not_found") != std::string::npos);
	auto malformed = rpc.Dispatch("{");
	assert(malformed.find("malformed_json") != std::string::npos);
	rpc.BeginShutdown();
	auto shutdown = rpc.Dispatch(R"({"id":"3","method":"echo","params":{"value":"ok"}})");
	assert(shutdown.find("shutting_down") != std::string::npos);
}
