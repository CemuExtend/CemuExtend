#pragma once

#include <functional>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include <rapidjson/document.h>

namespace WebFrontend
{
	class RpcDispatcher final
	{
	public:
		using Handler = std::function<std::string(const rapidjson::Value& params)>;

		void Register(std::string method, Handler handler);
		void VerifyMethods(std::span<const std::string_view> methods) const;
		[[nodiscard]] std::string Dispatch(std::string_view requestJson);
		void BeginShutdown();
		[[nodiscard]] bool IsShuttingDown() const;

	private:
		[[nodiscard]] static std::string Error(std::string_view id,
			std::string_view code, std::string_view message);
		[[nodiscard]] static std::string Success(std::string_view id,
			std::string_view resultJson);

		mutable std::mutex m_mutex;
		std::unordered_map<std::string, Handler> m_handlers;
		std::unordered_set<std::string> m_requestIds;
		std::deque<std::string> m_requestIdOrder;
		bool m_shuttingDown{};
	};
}
