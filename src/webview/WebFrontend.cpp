#include "Common/precompiled.h"

#include "application/ApplicationRuntime.h"
#include "application/EmulationController.h"
#include "frontend/FrontendRuntime.h"
#include "webview/MainWindowState.h"
#include "webview/RpcDispatcher.h"
#include "webview/generated/WebAssets.h"
#include "webview/generated/RpcMethods.h"
#include "util/helpers/helpers.h"

#include <array>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <webview/webview.h>

namespace
{
	using WebFrontend::MainWindowState;
	using WebFrontend::RpcDispatcher;
	class Runtime;
	struct RuntimeCallbackGate
	{
		std::mutex mutex;
		Runtime* target{};
	};

	std::string JsonString(std::string_view value)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string Base64(std::string_view value)
	{
		static constexpr std::string_view alphabet =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string output;
		output.reserve((value.size() + 2) / 3 * 4);
		for (std::size_t offset = 0; offset < value.size(); offset += 3)
		{
			const auto remaining = value.size() - offset;
			const auto first = static_cast<unsigned char>(value[offset]);
			const auto second = remaining > 1 ? static_cast<unsigned char>(value[offset + 1]) : 0;
			const auto third = remaining > 2 ? static_cast<unsigned char>(value[offset + 2]) : 0;
			const std::uint32_t bits = (first << 16) | (second << 8) | third;
			output.push_back(alphabet[(bits >> 18) & 0x3f]);
			output.push_back(alphabet[(bits >> 12) & 0x3f]);
			output.push_back(remaining > 1 ? alphabet[(bits >> 6) & 0x3f] : '=');
			output.push_back(remaining > 2 ? alphabet[bits & 0x3f] : '=');
		}
		return output;
	}

	std::string TitleIdString(std::uint64_t titleId)
	{
		std::array<char, 17> text{};
		std::snprintf(text.data(), text.size(), "%016llx",
			static_cast<unsigned long long>(titleId));
		return text.data();
	}

	std::uint64_t ParseTitleId(const rapidjson::Value& params)
	{
		const auto found = params.FindMember("titleId");
		if (found == params.MemberEnd() || !found->value.IsString() ||
			found->value.GetStringLength() != 16)
			throw std::invalid_argument("titleId must contain exactly 16 hexadecimal digits");
		std::uint64_t value{};
		const std::string_view text(found->value.GetString(), found->value.GetStringLength());
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw std::invalid_argument("titleId must contain exactly 16 hexadecimal digits");
		return value;
	}

	class Runtime final
	{
	public:
		Runtime()
			: m_webview(webview_create(
#if defined(NDEBUG)
				0,
#else
				1,
#endif
				nullptr))
		{
			if (!m_webview)
				throw std::runtime_error("failed to create the native webview window");
			m_windowState = std::make_unique<MainWindowState>(
				reinterpret_cast<std::uintptr_t>(webview_get_window(m_webview)));
			m_callbackGate->target = this;
			RegisterRpc();
			m_applicationEvents = m_controller.Events().Subscribe(
				[gate = m_callbackGate](const Application::Event& event) {
					std::scoped_lock lock(gate->mutex);
					if (gate->target)
						gate->target->ForwardEvent(event);
				});
			m_titleEvents = m_controller.SubscribeTitleCatalog(
				[gate = m_callbackGate](const Application::TitleCatalogEvent&) {
					std::scoped_lock lock(gate->mutex);
					if (gate->target)
						gate->target->Emit("titles.changed", "{}");
				});
			if (webview_bind(m_webview, "cemuInvoke", &Runtime::Invoke, this) != WEBVIEW_ERROR_OK)
				throw std::runtime_error("failed to install the native RPC binding");
			webview_set_title(m_webview, "CemuExtend");
			webview_set_size(m_webview, 1100, 720, WEBVIEW_HINT_NONE);
		}

		~Runtime()
		{
			m_stopping.store(true, std::memory_order_release);
			m_eventStopping->store(true, std::memory_order_release);
			{
				std::scoped_lock lock(m_callbackGate->mutex);
				m_callbackGate->target = nullptr;
			}
			m_titleEvents.Reset();
			m_applicationEvents.Reset();
			m_rpc.BeginShutdown();
			if (m_windowState)
				(void)m_windowState->BeginShutdown();
			if (m_webview)
			{
				webview_unbind(m_webview, "cemuInvoke");
				webview_destroy(m_webview);
			}
		}

		void Run()
		{
			if (const char* devUrl = std::getenv("CEMU_WEB_UI_DEV_URL"); devUrl && *devUrl)
			{
				const std::string_view url(devUrl);
				if (!url.starts_with("http://127.0.0.1:") && !url.starts_with("http://localhost:"))
					throw std::runtime_error("CEMU_WEB_UI_DEV_URL must use a loopback HTTP origin");
				webview_navigate(m_webview, devUrl);
			}
			else
			{
				const std::string html(reinterpret_cast<const char*>(WebAssets::html),
					WebAssets::htmlSize);
				webview_set_html(m_webview, html.c_str());
			}
			webview_run(m_webview);
		}

	private:
		struct PendingEvent
		{
			std::shared_ptr<std::atomic_bool> stopping;
			std::string script;
		};

		static void DispatchEvent(webview_t webview, void* argument)
		{
			std::unique_ptr<PendingEvent> pending(static_cast<PendingEvent*>(argument));
			if (!pending->stopping->load(std::memory_order_acquire))
				webview_eval(webview, pending->script.c_str());
		}

		void Emit(std::string_view type, std::string_view payloadJson)
		{
			std::scoped_lock eventLock(m_eventMutex);
			if (m_stopping.load(std::memory_order_acquire))
				return;
			const auto sequence = ++m_eventSequence;
			const auto event = std::string(R"({"type":)") + JsonString(type) +
				R"(,"sequence":)" + std::to_string(sequence) + R"(,"payload":)" +
				std::string(payloadJson) + "}";
			auto pending = std::make_unique<PendingEvent>();
			pending->stopping = m_eventStopping;
			pending->script = "window.__cemuDispatchEvent?.(JSON.parse(new TextDecoder().decode(Uint8Array.from(atob('" +
				Base64(event) + "'),c=>c.charCodeAt(0)))));";
			if (webview_dispatch(m_webview, &Runtime::DispatchEvent, pending.get()) == WEBVIEW_ERROR_OK)
				pending.release();
		}

		void ForwardEvent(const Application::Event& event)
		{
			switch (event.type)
			{
			case Application::EventType::LoadingStarted: Emit("emulation.loading", "{}"); break;
			case Application::EventType::GameLoaded: Emit("emulation.loaded", "{}"); break;
			case Application::EventType::GameExited:
				(void)m_windowState->FinishEmulation();
				Emit("emulation.exited", "{}");
				break;
			case Application::EventType::PpcProcessExited:
				Emit("emulation.processExited", std::string(R"({"status":)") +
					std::to_string(event.processStatus) + "}");
				break;
			case Application::EventType::PerformanceUpdated:
				Emit("emulation.performance", std::string(R"({"fps":)") +
					std::to_string(event.framesPerSecond) + "}");
				break;
			case Application::EventType::Diagnostic:
				Emit("system.diagnostic", std::string(R"({"message":)") +
					JsonString(event.diagnostic) + "}");
				break;
			case Application::EventType::GameListRefreshRequested: Emit("titles.changed", "{}"); break;
			case Application::EventType::TextInputWakeRequested: Emit("input.textWakeRequested", "{}"); break;
			}
		}

		static void Invoke(const char* sequence, const char* arguments, void* context)
		{
			auto& self = *static_cast<Runtime*>(context);
			rapidjson::Document array;
			array.Parse(arguments);
			std::string response;
			if (!array.IsArray() || array.Size() != 1 || !array[0].IsString())
				response = R"({"id":"","ok":false,"error":{"code":"invalid_binding_call","message":"cemuInvoke expects one JSON string"}})";
			else
				response = self.m_rpc.Dispatch(
					std::string_view(array[0].GetString(), array[0].GetStringLength()));
			const auto encoded = JsonString(response);
			webview_return(self.m_webview, sequence, 0, encoded.c_str());
		}

		void RegisterRpc()
		{
			m_rpc.Register("system.bootstrap", [this](const rapidjson::Value&) {
				return std::string(R"({"windowRole":"main-library","appVersion":"2.0","platform":")") +
#if BOOST_OS_WINDOWS
					"windows"
#elif BOOST_OS_MACOS
					"macos"
#else
					"linux"
#endif
					+ R"(","theme":"system","shuttingDown":)" +
					(m_rpc.IsShuttingDown() ? "true}" : "false}");
			});
			m_rpc.Register("system.quit", [this](const rapidjson::Value&) {
				m_rpc.BeginShutdown();
				(void)m_windowState->BeginShutdown();
				webview_terminate(m_webview);
				return "{}";
			});
			m_rpc.Register("window.close", [this](const rapidjson::Value&) {
				webview_terminate(m_webview);
				return "{}";
			});
			m_rpc.Register("window.getModel", [this](const rapidjson::Value& params) {
				const auto role = params.FindMember("role");
				if (role == params.MemberEnd() || !role->value.IsString())
					throw std::invalid_argument("role is required");
				const auto state = m_windowState->Snapshot();
				return std::string(R"({"role":)") +
					JsonString({role->value.GetString(), role->value.GetStringLength()}) +
					R"(,"emulationState":)" + std::to_string(static_cast<unsigned>(m_controller.State())) +
					R"(,"mainWindowGeneration":)" + std::to_string(state.generation) + "}";
			});
			m_rpc.Register("titles.list", [this](const rapidjson::Value&) {
				rapidjson::Document document(rapidjson::kArrayType);
				auto& allocator = document.GetAllocator();
				for (const auto& game : m_controller.ListGames())
				{
					rapidjson::Value item(rapidjson::kObjectType);
					const auto titleId = TitleIdString(game.titleId);
					item.AddMember("titleId", rapidjson::Value(titleId.c_str(), allocator), allocator);
					item.AddMember("name", rapidjson::Value(game.name.c_str(), allocator), allocator);
					const auto path = _pathToUtf8(game.basePath);
					item.AddMember("path", rapidjson::Value(path.c_str(), allocator), allocator);
					item.AddMember("region", rapidjson::Value(game.regionName.c_str(), allocator), allocator);
					item.AddMember("version", game.version, allocator);
					item.AddMember("playTimeMinutes", game.playStats.minutesPlayed, allocator);
					if (game.playStats.available)
					{
						std::array<char, 11> date{};
						std::snprintf(date.data(), date.size(), "%04u-%02u-%02u",
							game.playStats.lastPlayedYear, game.playStats.lastPlayedMonth,
							game.playStats.lastPlayedDay);
						item.AddMember("lastPlayed", rapidjson::Value(date.data(), allocator), allocator);
					}
					else
						item.AddMember("lastPlayed", rapidjson::Value(rapidjson::kNullType), allocator);
					document.PushBack(item, allocator);
				}
				rapidjson::StringBuffer buffer;
				rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
				document.Accept(writer);
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("titles.refresh", [this](const rapidjson::Value&) {
				m_controller.RefreshTitles();
				return "{}";
			});
			m_rpc.Register("titles.launch", [this](const rapidjson::Value& params) {
				const auto game = m_controller.GetGame(ParseTitleId(params));
				if (!game)
					throw std::invalid_argument("titleId is not present in the game library");
				return Launch(game->basePath);
			});
			m_rpc.Register("emulation.stop", [this](const rapidjson::Value&) {
				const auto result = m_controller.Stop();
				if (!result.stopped)
					throw std::runtime_error(result.diagnostic);
				(void)m_windowState->FinishEmulation();
				return "{}";
			});
			m_rpc.VerifyMethods(WebFrontend::Generated::RpcMethods);
		}

		std::string Launch(const fs::path& path)
		{
			if (!m_windowState->BeginLaunch())
				throw std::runtime_error("main window is not ready to launch a title");
			const auto result = m_controller.Launch({path},
				[this](const Application::LaunchResult&) {
					if (!m_windowState->CommitLaunch())
						throw std::runtime_error("main window content transition failed");
				},
				[this] { (void)m_windowState->RollbackLaunch(); });
			if (!result)
			{
				(void)m_windowState->RollbackLaunch();
				throw std::runtime_error(result.diagnostic.empty() ? "title launch failed" : result.diagnostic);
			}
			return std::string(R"({"titleId":")") + TitleIdString(result.titleId) + "\"}";
		}

		webview_t m_webview{};
		RpcDispatcher m_rpc;
		Application::EmulationController m_controller;
		std::unique_ptr<MainWindowState> m_windowState;
		std::shared_ptr<RuntimeCallbackGate> m_callbackGate{std::make_shared<RuntimeCallbackGate>()};
		Application::EventSubscription m_applicationEvents;
		Application::TitleCatalogSubscription m_titleEvents;
		std::atomic_bool m_stopping{};
		std::shared_ptr<std::atomic_bool> m_eventStopping{std::make_shared<std::atomic_bool>()};
		std::mutex m_eventMutex;
		std::uint64_t m_eventSequence{};
	};
}

void Frontend::Run()
{
	SetThreadName("cemu-web-ui");
	CemuCommonInit();
	Runtime runtime;
	runtime.Run();
}
