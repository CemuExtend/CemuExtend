#include "Common/precompiled.h"

#include "webview/CemodWebUiFrontend.h"
#include "webview/ToolWindowSupport.h"

#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <atomic>
#include <map>
#include <mutex>
#include <tuple>

namespace WebFrontend
{
	namespace
	{
		using namespace cemuextend::wire;
		using cemuextend_hle::CemodWebUiHostEvent;
		using cemuextend_hle::CemodWebUiHostRequest;

		template<typename T>
		bool ReadStruct(std::span<const std::byte> bytes, T& value, std::size_t offset = 0)
		{
			if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
				return false;
			std::memcpy(&value, bytes.data() + offset, sizeof(T));
			return true;
		}

		template<typename T>
		void Append(std::vector<std::byte>& output, const T& value)
		{
			const auto* begin = reinterpret_cast<const std::byte*>(&value);
			output.insert(output.end(), begin, begin + sizeof(value));
		}

		void AppendText(std::vector<std::byte>& output, std::string_view value)
		{
			const auto* begin = reinterpret_cast<const std::byte*>(value.data());
			output.insert(output.end(), begin, begin + value.size());
		}

		std::string JsonString(std::string_view value)
		{
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string JsonValue(const rapidjson::Value& value)
		{
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			value.Accept(writer);
			return {buffer.GetString(), buffer.GetSize()};
		}

		bool ValidName(std::string_view value)
		{
			if (value.empty() || value.size() > kMaximumUiNameBytes || value.starts_with("cemu."))
				return false;
			return std::ranges::all_of(value, [](unsigned char character) {
				return (character >= 'a' && character <= 'z') ||
					(character >= 'A' && character <= 'Z') ||
					(character >= '0' && character <= '9') || character == '.' ||
					character == '_' || character == '-' || character == ':';
			});
		}

		std::string OriginId(std::string_view principal)
		{
			std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
			SHA256(reinterpret_cast<const unsigned char*>(principal.data()), principal.size(),
				digest.data());
			// A URL host label is limited to 63 characters. Keep 224 bits of the
			// principal digest so the "mod-" prefix and lowercase hex fit in one label.
			constexpr std::size_t kOriginDigestBytes = 28;
			static_assert(4 + kOriginDigestBytes * 2 <= 63);
			constexpr char hex[] = "0123456789abcdef";
			std::string result{"mod-"};
			result.reserve(4 + kOriginDigestBytes * 2);
			for (std::size_t index = 0; index < kOriginDigestBytes; ++index)
			{
				const auto value = digest[index];
				result += hex[value >> 4U];
				result += hex[value & 0xfU];
			}
			return result;
		}

		std::string NetworkPolicyKey(const CemodWebUiNetwork& network)
		{
			std::string material;
			material += network.credentials ? '1' : '0';
			material += network.persistentStorage ? '1' : '0';
			material += network.allowPrivateNetwork ? '1' : '0';
			for (const auto& origin : network.connect)
			{
				material += "\nconnect:";
				material += origin;
			}
			for (const auto& origin : network.resources)
			{
				material += "\nresource:";
				material += origin;
			}
			std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
			SHA256(reinterpret_cast<const unsigned char*>(material.data()), material.size(),
				digest.data());
			constexpr char hex[] = "0123456789abcdef";
			std::string result;
			result.reserve(digest.size() * 2);
			for (const auto value : digest)
			{
				result += hex[value >> 4U];
				result += hex[value & 0xfU];
			}
			return result;
		}

		std::vector<std::byte> MessagePayload(std::uint32_t handle, std::uint32_t callId,
			std::string_view name, std::string_view json)
		{
			UiMessageHeader header{};
			header.handle = handle;
			header.callId = callId;
			header.nameBytes = static_cast<std::uint32_t>(name.size());
			header.jsonBytes = static_cast<std::uint32_t>(json.size());
			std::vector<std::byte> result;
			result.reserve(sizeof(header) + name.size() + json.size());
			Append(result, header);
			AppendText(result, name);
			AppendText(result, json);
			return result;
		}
	} // namespace
	using cemuextend::wire::Status;
	using cemuextend::wire::UiCloseReason;
	using cemuextend::wire::UiEvent;
	using cemuextend::wire::UiMode;
	using cemuextend::wire::UiOperation;
	using cemuextend::wire::UiSurface;

	struct CemodWebUiFrontend::Impl
	{
		using RequestKey = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t>;

		struct PendingCall
		{
			std::int64_t queryId{};
			std::function<void(bool, std::string)> completion;
		};

		struct Instance
		{
			std::uint32_t handle{};
			std::uint64_t windowId{};
			std::uint64_t addressSpaceId{};
			std::uint32_t generation{};
			std::uint32_t sessionId{};
			std::shared_ptr<const cemuextend_hle::CemodWebUiContent> content;
			std::string viewId;
			std::string contextJson;
			UiMode mode{UiMode::Window};
			UiSurface surface{UiSurface::Tv};
			bool visible{true};
			bool interactive{};
			bool ready{};
			std::uint64_t documentGeneration{1};
			bool closing{};
			UiCloseReason closeReason{UiCloseReason::InternalError};
			std::unique_ptr<IToolWindowSupport> window;
			std::map<std::uint32_t, PendingCall> calls;
			std::vector<std::pair<std::string, std::string>> queuedEvents;
		};

		void* parent{};
		std::shared_ptr<CefOverlay::BrowserRuntime> browsers;
		PostTask postTask;
		std::mutex mutex;
		EventSink eventSink;
		std::map<RequestKey, std::shared_ptr<std::atomic_bool>> pendingRequests;
		std::map<std::uint32_t, std::unique_ptr<Instance>> instances;
		std::uint32_t nextHandle{};
		std::uint32_t nextCallId{};
		std::weak_ptr<CemodWebUiFrontend> owner;
		std::atomic_bool stopping{};

		Impl(void* parentValue, std::shared_ptr<CefOverlay::BrowserRuntime> browserValue,
			PostTask postValue)
			: parent(parentValue), browsers(std::move(browserValue)), postTask(std::move(postValue)) {}

		void Publish(const Instance& instance, UiEvent event, std::vector<std::byte> payload)
		{
			EventSink sink;
			{
				std::scoped_lock lock(mutex);
				sink = eventSink;
			}
			if (sink)
			{
				try
				{
					sink(CemodWebUiHostEvent{instance.addressSpaceId, instance.generation,
						instance.sessionId, event, std::move(payload)});
				}
				catch (...)
				{
					cemuLog_log(LogType::Force, "Cemod Web UI event sink failed");
				}
			}
		}

		Instance* Find(std::uint32_t handle, const CemodWebUiHostRequest& request)
		{
			const auto found = instances.find(handle);
			if (found == instances.end() || found->second->addressSpaceId != request.addressSpaceId ||
				found->second->generation != request.generation ||
				found->second->sessionId != request.sessionId)
				return nullptr;
			return found->second.get();
		}

		std::shared_ptr<CefOverlay::BrowserAssetBundle> Assets(
			const cemuextend_hle::CemodWebUiContent& content)
		{
			auto result = std::make_shared<CefOverlay::BrowserAssetBundle>();
			result->originId = OriginId(content.principal);
			result->assets = content.assets;
			for (const auto& [id, view] : content.manifest.views)
				result->viewEntries.emplace(id, view.entry);
			result->connectOrigins = content.manifest.network.connect;
			result->resourceOrigins = content.manifest.network.resources;
			result->networkPolicyKey = NetworkPolicyKey(content.manifest.network);
			result->credentials = content.manifest.network.credentials;
			result->persistentStorage = content.manifest.network.persistentStorage;
			result->allowPrivateNetwork = content.manifest.network.allowPrivateNetwork;
			return result;
		}

		std::string Bootstrap(const Instance& instance) const
		{
			const char* mode = instance.mode == UiMode::Window ? "window" : "overlay";
			const char* surface = instance.mode == UiMode::Window ? nullptr
				: (instance.surface == UiSurface::Tv ? "tv" : "drc");
			return std::string{"{"} +
				"\"bridgeVersion\":" + std::to_string(instance.content->manifest.bridgeVersion) +
				",\"modId\":" + JsonString(instance.content->modId) +
				",\"viewId\":" + JsonString(instance.viewId) +
				",\"instanceId\":" + JsonString(std::to_string(instance.handle)) +
				",\"mode\":" + JsonString(mode) +
				",\"surface\":" + (surface ? JsonString(surface) : "null") +
				",\"context\":" + instance.contextJson + "}";
		}

		void Process(CemodWebUiHostRequest request, Completion completion);
		Status CreateInstance(const CemodWebUiHostRequest& request,
			std::vector<std::byte>& response);
		void HandleQuery(std::uint32_t handle, std::int64_t queryId, std::string request,
			std::function<void(bool, std::string)> completion);
		void CancelQuery(std::uint32_t handle, std::int64_t queryId);
		void MainFrameReloaded(std::uint32_t handle);
		void WindowBoundsChanged(std::uint32_t handle, Host::RenderRegionBounds bounds);
		void WindowFocusChanged(std::uint32_t handle, bool focused);
		void CloseInstance(std::uint32_t handle, UiCloseReason reason);
		void BrowserClosed(std::uint32_t handle);
		void CloseMatching(std::function<bool(const Instance&)> predicate,
			UiCloseReason reason);
	};

	Status CemodWebUiFrontend::Impl::CreateInstance(const CemodWebUiHostRequest& request,
		std::vector<std::byte>& response)
	{
		UiCreateRequestHeader header{};
		if (!ReadStruct(std::span{request.payload}, header))
			return Status::InvalidArgument;
		const auto viewBytes = header.viewBytes.get();
		const auto contextBytes = header.contextBytes.get();
		if (sizeof(header) + viewBytes + contextBytes != request.payload.size() || !request.content)
			return Status::InvalidArgument;
		const auto* text = reinterpret_cast<const char*>(request.payload.data() + sizeof(header));
		const std::string viewId{text, viewBytes};
		const std::string context{text + viewBytes, contextBytes};
		const auto view = request.content->manifest.views.find(viewId);
		if (view == request.content->manifest.views.end())
			return Status::NotFound;
		const auto mode = static_cast<UiMode>(header.mode);
		const auto surface = static_cast<UiSurface>(header.surface);
		if (mode != UiMode::Window && mode != UiMode::Overlay)
			return Status::InvalidArgument;
		if (mode == UiMode::Window && !view->second.window)
			return Status::InvalidArgument;
		if (mode == UiMode::Overlay && (!view->second.overlay ||
			std::ranges::none_of(view->second.overlay->surfaces,
				[&](CemodWebUiSurface allowed) {
					return static_cast<std::uint8_t>(allowed) == static_cast<std::uint8_t>(surface);
				})))
			return Status::PermissionDenied;
		if (view->second.singleInstance)
		{
			const auto existing = std::ranges::find_if(instances, [&](const auto& item) {
				return item.second->addressSpaceId == request.addressSpaceId &&
					item.second->generation == request.generation &&
					item.second->sessionId == request.sessionId && item.second->viewId == viewId &&
					item.second->mode == mode &&
					(mode == UiMode::Window || item.second->surface == surface);
			});
			if (existing != instances.end())
			{
				if (existing->second->window) existing->second->window->Focus();
				UiCreateResponse created{};
				created.handle = existing->first;
				Append(response, created);
				return Status::Ok;
			}
		}
		if (std::ranges::count_if(instances, [&](const auto& item) {
				return item.second->addressSpaceId == request.addressSpaceId &&
					item.second->generation == request.generation &&
					item.second->mode == UiMode::Window;
			}) >= 4)
			return Status::Busy;
		auto instance = std::make_unique<Instance>();
		do { ++nextHandle; } while (!nextHandle || instances.contains(nextHandle));
		instance->handle = nextHandle;
		instance->windowId = 0x4000000000000000ULL | nextHandle;
		instance->addressSpaceId = request.addressSpaceId;
		instance->generation = request.generation;
		instance->sessionId = request.sessionId;
		instance->content = request.content;
		instance->viewId = viewId;
		instance->contextJson = context;
		instance->mode = mode;
		instance->surface = surface;
		instance->visible = header.visible != 0;
		instance->interactive = header.interactive != 0;
			const auto handle = instance->handle;
			const auto windowId = instance->windowId;
			const auto weakOwner = owner;
			auto assets = Assets(*request.content);
			CefOverlay::BrowserDescriptor browser;
			browser.windowId = windowId;
			browser.role = "cemod-web-ui";
			browser.bootstrapJson = Bootstrap(*instance);
			browser.initialUrl = "cemod-ui://" + assets->originId + "/" + viewId + "/";
			browser.cemodAssets = std::move(assets);
			IToolWindowSupport* support{};
			if (mode == UiMode::Window)
			{
				instance->window = CreateToolWindowSupport(parent, false, [weakOwner, handle] {
					if (const auto active = weakOwner.lock())
						active->m_impl->CloseInstance(handle, UiCloseReason::User);
				});
				const auto& windowConfig = *view->second.window;
				const auto minimumWidth = static_cast<std::int32_t>(windowConfig.minimumWidth.value_or(1));
				const auto minimumHeight = static_cast<std::int32_t>(windowConfig.minimumHeight.value_or(1));
				const auto width = std::max(minimumWidth, header.width.get() > 0 ? header.width.get()
					: static_cast<std::int32_t>(windowConfig.width.value_or(960)));
				const auto height = std::max(minimumHeight, header.height.get() > 0 ? header.height.get()
					: static_cast<std::int32_t>(windowConfig.height.value_or(540)));
				instance->window->SetTitle(windowConfig.title.value_or(request.content->modId));
				instance->window->SetMinimumSize(minimumWidth, minimumHeight);
				instance->window->SetResizable(windowConfig.resizable.value_or(true));
				instance->window->SetStateCallbacks(
					[weakOwner, handle](Host::RenderRegionBounds bounds) {
						if (const auto active = weakOwner.lock())
							active->m_impl->WindowBoundsChanged(handle, bounds);
					},
					[weakOwner, handle](bool focused) {
						if (const auto active = weakOwner.lock())
							active->m_impl->WindowFocusChanged(handle, focused);
					});
				instance->window->SetSize(width, height);
				support = instance->window.get();
				browser.presentation = CefOverlay::BrowserPresentation::NativeChild;
				browser.nativeParent = support->GetBrowserParentWindow();
				browser.bounds = support->GetBrowserBounds();
				browser.bounds.width = std::max(browser.bounds.width, 1);
				browser.bounds.height = std::max(browser.bounds.height, 1);
				browser.dpiScale = support->GetBrowserDpiScale();
				browser.nativeBrowserCreated = [support](void* child) { support->AttachBrowser(child); };
				browser.nativeBrowserClosing = [support](void* child) { support->DetachBrowser(child); };
			}
			else
			{
				const auto& overlayConfig = *view->second.overlay;
				browser.presentation = CefOverlay::BrowserPresentation::OverlayOsr;
				browser.overlaySurface = surface == UiSurface::Tv
					? Host::PointerSurface::Main : Host::PointerSurface::Pad;
				browser.overlayLayer = CefOverlay::OverlayLayer::Cemod;
				browser.cemodOverlayOrder = overlayConfig.order == CemodWebUiOverlayOrder::BelowBuiltin
					? CefOverlay::CemodOverlayOrder::BelowBuiltin
					: CefOverlay::CemodOverlayOrder::AboveBuiltin;
				browser.overlayVisible = instance->visible;
				browser.overlayInteractive = instance->interactive;
				browser.overlayTransparent = overlayConfig.transparent;
				browser.bounds = {0, 0, 1, 1};
				browser.dpiScale = 1.0;
			}
		browser.cemodQuery = [weakOwner, handle](std::int64_t queryId, std::string query,
			std::function<void(bool, std::string)> callback) {
			if (const auto active = weakOwner.lock())
				active->m_impl->HandleQuery(handle, queryId, std::move(query), std::move(callback));
			else
				callback(false, R"({"code":"DISCONNECTED","message":"Cemod UI host stopped","details":null})");
		};
		browser.cemodQueryCancelled = [weakOwner, handle](std::int64_t queryId) {
			if (const auto active = weakOwner.lock())
				active->m_impl->CancelQuery(handle, queryId);
		};
		browser.cemodMainFrameReloaded = [weakOwner, handle] {
			if (const auto active = weakOwner.lock())
				active->m_impl->MainFrameReloaded(handle);
		};
		browser.cemodRendererTerminated = [weakOwner, handle] {
			if (const auto active = weakOwner.lock())
				active->m_impl->CloseInstance(handle, UiCloseReason::RendererCrashed);
		};
		browser.closed = [weakOwner, handle] {
			if (const auto active = weakOwner.lock())
				active->m_impl->BrowserClosed(handle);
		};

		instances.emplace(handle, std::move(instance));
		if (!browsers->CreateBrowser(browser))
		{
			auto failed = instances.extract(handle);
			failed.mapped()->window.reset();
				return mode == UiMode::Overlay ? Status::Busy : Status::IoError;
			}
			if (header.visible && support)
				support->Show();
		UiCreateResponse created{};
		created.handle = handle;
		Append(response, created);
		return Status::Ok;
	}

	void CemodWebUiFrontend::Impl::Process(CemodWebUiHostRequest request,
		Completion completion)
	{
		std::vector<std::byte> response;
		Status status = Status::Ok;
		if (request.operation == UiOperation::Create)
			status = CreateInstance(request, response);
		else
		{
			UiHandleRequest handleRequest{};
			if (!ReadStruct(std::span{request.payload}, handleRequest))
				status = Status::InvalidArgument;
			else if (auto* instance = Find(handleRequest.handle.get(), request); !instance)
				status = Status::NotFound;
			else
			{
				switch (request.operation)
				{
				case UiOperation::Close:
					CloseInstance(instance->handle, UiCloseReason::Wps);
					break;
				case UiOperation::Focus:
					if (instance->window)
						instance->window->Focus();
					else if (instance->visible && instance->interactive)
						browsers->SetOverlayInteractive(instance->windowId, true);
					break;
				case UiOperation::SetVisible:
				{
					UiVisibleRequest value{};
					ReadStruct(std::span{request.payload}, value);
					instance->visible = value.visible != 0;
					if (instance->window)
					{
						if (value.visible) instance->window->Show();
						else instance->window->Hide();
					}
					else
						browsers->SetOverlayVisible(instance->windowId, instance->visible);
					break;
				}
				case UiOperation::SetBounds:
				{
					UiBoundsRequest value{};
					ReadStruct(std::span{request.payload}, value);
					if (!instance->window)
					{
						status = Status::NotSupported;
						break;
					}
					const auto& view = instance->content->manifest.views.at(instance->viewId);
					const auto minimumWidth = static_cast<std::int32_t>(
						view.window->minimumWidth.value_or(1));
					const auto minimumHeight = static_cast<std::int32_t>(
						view.window->minimumHeight.value_or(1));
					const auto width = std::max(value.width.get(), minimumWidth);
					const auto height = std::max(value.height.get(), minimumHeight);
					if (instance->window)
						instance->window->SetBounds(value.x.get(), value.y.get(),
							width, height);
					browsers->ResizeWindow(instance->windowId, width, height,
						instance->window ? instance->window->GetBrowserDpiScale() : 1.0);
					break;
				}
				case UiOperation::SetTitle:
				{
					UiTitleRequestHeader value{};
					ReadStruct(std::span{request.payload}, value);
					const std::string_view title{reinterpret_cast<const char*>(request.payload.data() + sizeof(value)),
						value.titleBytes.get()};
					if (instance->window) instance->window->SetTitle(title);
					else status = Status::NotSupported;
					break;
				}
				case UiOperation::SetInteractive:
				{
					if (instance->mode != UiMode::Overlay)
					{
						status = Status::NotSupported;
						break;
					}
					UiInteractiveRequest value{};
					ReadStruct(std::span{request.payload}, value);
					const auto& view = instance->content->manifest.views.at(instance->viewId);
					if (value.interactive && (!view.overlay || !view.overlay->interactive))
					{
						status = Status::PermissionDenied;
						break;
					}
					instance->interactive = value.interactive != 0;
					browsers->SetOverlayInteractive(instance->windowId, instance->interactive);
					break;
				}
					break;
				case UiOperation::Emit:
				{
					UiMessageHeader value{};
					ReadStruct(std::span{request.payload}, value);
					const auto* text = reinterpret_cast<const char*>(request.payload.data() + sizeof(value));
					std::string name{text, value.nameBytes.get()};
					std::string json{text + value.nameBytes.get(), value.jsonBytes.get()};
					if (!instance->ready)
					{
						if (instance->queuedEvents.size() >= 128)
							status = Status::Busy;
						else
							instance->queuedEvents.emplace_back(std::move(name), std::move(json));
					}
					else
						browsers->ExecuteCemodEvent(instance->windowId, name, json);
					break;
				}
				case UiOperation::Reply:
				{
					UiReplyHeader value{};
					ReadStruct(std::span{request.payload}, value);
					const auto call = instance->calls.find(value.callId.get());
					if (call == instance->calls.end())
						status = Status::NotFound;
					else
					{
						auto callback = std::move(call->second.completion);
						instance->calls.erase(call);
						std::string json{reinterpret_cast<const char*>(request.payload.data() + sizeof(value)),
							value.jsonBytes.get()};
						callback(value.success != 0, std::move(json));
					}
					break;
				}
				default:
					status = Status::NotSupported;
				}
			}
		}
		if (completion)
			completion(status, std::move(response));
	}

	void CemodWebUiFrontend::Impl::HandleQuery(std::uint32_t handle, std::int64_t queryId,
		std::string request, std::function<void(bool, std::string)> completion)
	{
		const auto found = instances.find(handle);
		if (found == instances.end() || found->second->closing)
		{
			completion(false, R"({"code":"PAGE_CLOSED","message":"Cemod UI is closed","details":null})");
			return;
		}
		auto& instance = *found->second;
		rapidjson::Document document;
		document.Parse(request.data(), request.size());
		if (document.HasParseError() || !document.IsObject() ||
			!document.HasMember("bridge") || !document["bridge"].IsInt() ||
			document["bridge"].GetInt() != 1 || !document.HasMember("kind") ||
			!document["kind"].IsString() || !document.HasMember("value"))
		{
			completion(false, R"({"code":"INVALID_ARGUMENT","message":"Invalid Cemod bridge request","details":null})");
			return;
		}
		const std::string_view kind{document["kind"].GetString(), document["kind"].GetStringLength()};
		const auto& value = document["value"];
		if (kind == "ready")
		{
			if (instance.ready)
			{
				completion(false, R"({"code":"ALREADY_READY","message":"Cemod UI is already ready","details":null})");
				return;
			}
			const auto json = JsonValue(value);
			if (json.size() > kMaximumUiJsonBytes)
			{
				completion(false, R"({"code":"TOO_LARGE","message":"Ready data is too large","details":null})");
				return;
			}
			instance.ready = true;
			Publish(instance, UiEvent::Ready, MessagePayload(handle, 0, {}, json));
			for (const auto& [name, payload] : instance.queuedEvents)
				browsers->ExecuteCemodEvent(instance.windowId, name, payload);
			instance.queuedEvents.clear();
			completion(true, "null");
			return;
		}
		if (kind == "close")
		{
			completion(true, "null");
			CloseInstance(handle, UiCloseReason::JavaScript);
			return;
		}
		if ((kind != "call" && kind != "send") || !value.IsObject() ||
			!value.HasMember("name") || !value["name"].IsString() ||
			!value.HasMember("data"))
		{
			completion(false, R"({"code":"INVALID_ARGUMENT","message":"Invalid Cemod message","details":null})");
			return;
		}
		const std::string name{value["name"].GetString(), value["name"].GetStringLength()};
		const auto json = JsonValue(value["data"]);
		if (!ValidName(name) || json.size() > kMaximumUiJsonBytes)
		{
			completion(false, R"({"code":"INVALID_ARGUMENT","message":"Invalid Cemod message name or data","details":null})");
			return;
		}
		if (kind == "send")
		{
			Publish(instance, UiEvent::Message, MessagePayload(handle, 0, name, json));
			completion(true, "null");
			return;
		}
		if (instance.calls.size() >= 64)
		{
			completion(false,
				R"({"code":"BUSY","message":"Too many pending Cemod UI calls","details":null})");
			return;
		}
		do { ++nextCallId; } while (!nextCallId || instance.calls.contains(nextCallId));
		const auto callId = nextCallId;
		instance.calls.emplace(callId, PendingCall{queryId, std::move(completion)});
		Publish(instance, UiEvent::Call, MessagePayload(handle, callId, name, json));
	}

	void CemodWebUiFrontend::Impl::CancelQuery(std::uint32_t handle, std::int64_t queryId)
	{
		const auto found = instances.find(handle);
		if (found == instances.end())
			return;
		auto& instance = *found->second;
		const auto call = std::ranges::find_if(instance.calls, [&](const auto& item) {
			return item.second.queryId == queryId;
		});
		if (call == instance.calls.end())
			return;
		UiMessageHeader cancelled{};
		cancelled.handle = handle;
		cancelled.callId = call->first;
		std::vector<std::byte> payload;
		Append(payload, cancelled);
		instance.calls.erase(call);
		Publish(instance, UiEvent::Cancel, std::move(payload));
	}

	void CemodWebUiFrontend::Impl::MainFrameReloaded(std::uint32_t handle)
	{
		const auto found = instances.find(handle);
		if (found == instances.end() || found->second->closing)
			return;
		auto& instance = *found->second;
		++instance.documentGeneration;
		instance.ready = false;
		instance.queuedEvents.clear();
		// CefMessageRouterBrowserSide cancels every old query before this callback.
		// A non-empty map here would indicate a broken query ownership invariant.
		if (!instance.calls.empty())
		{
			cemuLog_log(LogType::Force,
				"Cemod Web UI reload left {} pending calls for handle {}",
				instance.calls.size(), handle);
		}
	}

	void CemodWebUiFrontend::Impl::WindowBoundsChanged(std::uint32_t handle,
		Host::RenderRegionBounds bounds)
	{
		const auto found = instances.find(handle);
		if (found == instances.end() || found->second->closing)
			return;
		UiBoundsEvent event{};
		event.handle = handle;
		event.x = bounds.x;
		event.y = bounds.y;
		event.width = bounds.width;
		event.height = bounds.height;
		std::vector<std::byte> payload;
		Append(payload, event);
		Publish(*found->second, UiEvent::BoundsChanged, std::move(payload));
	}

	void CemodWebUiFrontend::Impl::WindowFocusChanged(std::uint32_t handle, bool focused)
	{
		const auto found = instances.find(handle);
		if (found == instances.end() || found->second->closing)
			return;
		browsers->SetWindowFocus(found->second->windowId, focused);
		UiFocusEvent event{};
		event.handle = handle;
		event.focused = focused ? 1 : 0;
		std::vector<std::byte> payload;
		Append(payload, event);
		Publish(*found->second, UiEvent::FocusChanged, std::move(payload));
	}

	void CemodWebUiFrontend::Impl::CloseInstance(std::uint32_t handle, UiCloseReason reason)
	{
		const auto found = instances.find(handle);
		if (found == instances.end() || std::exchange(found->second->closing, true))
			return;
		found->second->closeReason = reason;
		if (!browsers->CloseWindow(found->second->windowId))
			BrowserClosed(handle);
	}

	void CemodWebUiFrontend::Impl::BrowserClosed(std::uint32_t handle)
	{
		const auto found = instances.find(handle);
		if (found == instances.end())
			return;
		auto instance = std::move(found->second);
		instances.erase(found);
		for (auto& [callId, call] : instance->calls)
		{
			(void)callId;
			if (call.completion)
				call.completion(false,
					R"({"code":"PAGE_CLOSED","message":"Cemod UI was closed","details":null})");
		}
		instance->calls.clear();
		instance->window.reset();
		UiClosedEvent closed{};
		closed.handle = handle;
		closed.reason = static_cast<std::uint8_t>(instance->closeReason);
		std::vector<std::byte> payload;
		Append(payload, closed);
		Publish(*instance, UiEvent::Closed, std::move(payload));
	}

	void CemodWebUiFrontend::Impl::CloseMatching(std::function<bool(const Instance&)> predicate,
		UiCloseReason reason)
	{
		std::vector<std::uint32_t> handles;
		for (const auto& [handle, instance] : instances)
			if (predicate(*instance)) handles.push_back(handle);
		for (const auto handle : handles)
			CloseInstance(handle, reason);
	}

	std::shared_ptr<CemodWebUiFrontend> CemodWebUiFrontend::Create(void* parent,
		std::shared_ptr<CefOverlay::BrowserRuntime> browsers, PostTask postTask)
	{
		if (!parent || !browsers || !postTask)
			return {};
		auto result = std::shared_ptr<CemodWebUiFrontend>(new CemodWebUiFrontend(
			std::make_unique<Impl>(parent, std::move(browsers), std::move(postTask))));
		result->m_impl->owner = result;
		return result;
	}

	CemodWebUiFrontend::CemodWebUiFrontend(std::unique_ptr<Impl> impl)
		: m_impl(std::move(impl)) {}

	CemodWebUiFrontend::~CemodWebUiFrontend() = default;

	void CemodWebUiFrontend::SetEventSink(EventSink sink)
	{
		std::scoped_lock lock(m_impl->mutex);
		m_impl->eventSink = std::move(sink);
	}

	bool CemodWebUiFrontend::Submit(CemodWebUiHostRequest request, Completion completion)
	{
		if (m_impl->stopping.load(std::memory_order_acquire))
			return false;
		const Impl::RequestKey key{request.addressSpaceId, request.generation,
			request.sessionId, request.correlationId};
		auto cancelled = std::make_shared<std::atomic_bool>(false);
		{
			std::scoped_lock lock(m_impl->mutex);
			if (m_impl->stopping.load(std::memory_order_acquire))
				return false;
			if (m_impl->pendingRequests.contains(key))
				return false;
			m_impl->pendingRequests.emplace(key, cancelled);
		}
		const auto weak = weak_from_this();
		auto completionCalled = std::make_shared<std::atomic_bool>(false);
		auto completeOnce = [completion = std::move(completion), completionCalled](Status status,
			std::vector<std::byte> response) mutable {
			if (!completionCalled->exchange(true, std::memory_order_acq_rel) && completion)
				completion(status, std::move(response));
		};
		if (!m_impl->postTask([weak, key, cancelled, request = std::move(request),
				completeOnce = std::move(completeOnce)]() mutable {
				const auto self = weak.lock();
				if (!self) return;
				{
					std::scoped_lock lock(self->m_impl->mutex);
					self->m_impl->pendingRequests.erase(key);
				}
				if (cancelled->load(std::memory_order_acquire))
					return;
				if (self->m_impl->stopping.load(std::memory_order_acquire))
					return;
				try
				{
					self->m_impl->Process(std::move(request), completeOnce);
				}
				catch (const std::exception& error)
				{
					cemuLog_log(LogType::Force, "Cemod Web UI operation failed: {}", error.what());
					completeOnce(Status::IoError, {});
				}
				catch (...)
				{
					cemuLog_log(LogType::Force, "Cemod Web UI operation failed");
					completeOnce(Status::IoError, {});
				}
			}))
		{
			std::scoped_lock lock(m_impl->mutex);
			m_impl->pendingRequests.erase(key);
			return false;
		}
		return true;
	}

	void CemodWebUiFrontend::Cancel(std::uint64_t addressSpaceId, std::uint32_t generation,
		std::uint32_t sessionId, std::uint32_t correlationId)
	{
		std::scoped_lock lock(m_impl->mutex);
		const auto found = m_impl->pendingRequests.find(
			Impl::RequestKey{addressSpaceId, generation, sessionId, correlationId});
		if (found != m_impl->pendingRequests.end())
			found->second->store(true, std::memory_order_release);
	}

	void CemodWebUiFrontend::CloseSession(std::uint64_t addressSpaceId, std::uint32_t generation,
		std::uint32_t sessionId)
	{
		const auto weak = weak_from_this();
		(void)m_impl->postTask([weak, addressSpaceId, generation, sessionId] {
			if (const auto self = weak.lock())
				self->m_impl->CloseMatching([&](const Impl::Instance& value) {
					return value.addressSpaceId == addressSpaceId && value.generation == generation &&
						value.sessionId == sessionId;
				}, UiCloseReason::ConnectionClosed);
		});
	}

	void CemodWebUiFrontend::CloseOwner(std::uint64_t addressSpaceId, std::uint32_t generation)
	{
		const auto weak = weak_from_this();
		(void)m_impl->postTask([weak, addressSpaceId, generation] {
			if (const auto self = weak.lock())
				self->m_impl->CloseMatching([&](const Impl::Instance& value) {
					return value.addressSpaceId == addressSpaceId && value.generation == generation;
				}, UiCloseReason::ModUnloaded);
		});
	}

	void CemodWebUiFrontend::CloseAll()
	{
		const auto weak = weak_from_this();
		(void)m_impl->postTask([weak] {
			if (const auto self = weak.lock())
				self->m_impl->CloseMatching([](const Impl::Instance&) { return true; },
					UiCloseReason::CemuClosed);
		});
	}

	void CemodWebUiFrontend::BeginShutdown()
	{
		if (m_impl->stopping.exchange(true, std::memory_order_acq_rel))
			return;
		std::scoped_lock lock(m_impl->mutex);
		for (const auto& [key, cancelled] : m_impl->pendingRequests)
		{
			(void)key;
			cancelled->store(true, std::memory_order_release);
		}
		m_impl->eventSink = {};
	}

	void CemodWebUiFrontend::Shutdown()
	{
		m_impl->CloseMatching([](const Impl::Instance&) { return true; }, UiCloseReason::CemuClosed);
	}
} // namespace WebFrontend
