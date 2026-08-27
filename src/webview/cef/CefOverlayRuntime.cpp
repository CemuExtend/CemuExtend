#include "Common/precompiled.h"

#include "webview/cef/CefOverlayRuntime.h"
#include "webview/cef/CefOverlayFrameMailbox.h"
#include "webview/generated/WebAssets.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_request.h"
#include "include/cef_resource_handler.h"
#include "include/cef_scheme.h"
#include "include/cef_stream.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"
#include "include/wrapper/cef_resource_manager.h"
#include "include/wrapper/cef_stream_resource_handler.h"

#include <glib.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unistd.h>

namespace WebFrontend::CefOverlay
{
	namespace
	{
		constexpr char kScheme[] = "cemu";
		constexpr char kDomain[] = "ui";
		constexpr char kUrl[] = "cemu://ui/index.html";

		CefMessageRouterConfig RouterConfig()
		{
			CefMessageRouterConfig config;
			config.js_query_function = "cemuCefQuery";
			config.js_cancel_function = "cemuCefQueryCancel";
			return config;
		}

		class App final : public CefApp,
					  public CefBrowserProcessHandler,
					  public CefRenderProcessHandler
		{
		  public:
			CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
			CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

			void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override
			{
				registrar->AddCustomScheme(kScheme,
					CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
					CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
			}

			void OnBeforeCommandLineProcessing(const CefString& processType,
				CefRefPtr<CefCommandLine> commandLine) override
			{
				if (processType.empty())
				{
					commandLine->AppendSwitch("no-first-run");
					commandLine->AppendSwitch("disable-gpu");
					commandLine->AppendSwitch("disable-gpu-compositing");
					// Chromium 151 may still launch a software GPU process after
					// --disable-gpu. On portable Linux runtimes that process can mix
					// host GL drivers with bundled userspace libraries and abort before
					// OSR starts. CPU compositing is sufficient for the BGRA OnPaint path.
					commandLine->AppendSwitch("disable-software-rasterizer");
					commandLine->AppendSwitch("disable-features=CalculateNativeWinOcclusion");
					if (const char* headless = std::getenv("CEMU_CEF_HEADLESS");
						headless && std::string_view(headless) == "1")
					{
						commandLine->AppendSwitch("headless");
						commandLine->AppendSwitchWithValue("ozone-platform", "headless");
					}
				}
			}

			void OnWebKitInitialized() override
			{
				m_rendererRouter = CefMessageRouterRendererSide::Create(RouterConfig());
			}

			void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				CefRefPtr<CefV8Context> context) override
			{
				m_rendererRouter->OnContextCreated(browser, frame, context);
			}

			void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				CefRefPtr<CefV8Context> context) override
			{
				m_rendererRouter->OnContextReleased(browser, frame, context);
			}

			bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
				CefRefPtr<CefFrame> frame, CefProcessId sourceProcess,
				CefRefPtr<CefProcessMessage> message) override
			{
				return m_rendererRouter->OnProcessMessageReceived(
					browser, frame, sourceProcess, message);
			}

		  private:
			CefRefPtr<CefMessageRouterRendererSide> m_rendererRouter;
			IMPLEMENT_REFCOUNTING(App);
		};

		CefRefPtr<App> g_app;
		bool g_initialized{};
		guint g_pumpSource{};
		int g_argc{};
		char** g_argv{};
		std::vector<std::string> g_arguments;
		std::vector<char*> g_argumentPointers;

		void RestoreArgumentPointers()
		{
			g_argumentPointers.clear();
			g_argumentPointers.reserve(g_arguments.size() + 1);
			for (auto& argument : g_arguments)
				g_argumentPointers.push_back(argument.data());
			g_argumentPointers.push_back(nullptr);
			g_argc = static_cast<int>(g_arguments.size());
			g_argv = g_argumentPointers.data();
		}

		gboolean Pump(gpointer)
		{
			if (g_initialized)
				CefDoMessageLoopWork();
			return g_initialized ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
		}

		class MemoryReadHandler final : public CefReadHandler
		{
		  public:
			explicit MemoryReadHandler(std::string data) : m_data(std::move(data)) {}
			size_t Read(void* output, size_t size, size_t count) override
			{
				if (!size || !count)
					return 0;
				const auto available = m_data.size() - std::min(m_position, m_data.size());
				const auto elements = std::min(count, available / size);
				const auto bytes = elements * size;
				std::memcpy(output, m_data.data() + m_position, bytes);
				m_position += bytes;
				return elements;
			}
			int Seek(std::int64_t offset, int whence) override
			{
				std::int64_t origin = whence == SEEK_CUR ? static_cast<std::int64_t>(m_position)
					: whence == SEEK_END ? static_cast<std::int64_t>(m_data.size()) : 0;
				const auto next = origin + offset;
				if (next < 0 || static_cast<std::size_t>(next) > m_data.size())
					return -1;
				m_position = static_cast<std::size_t>(next);
				return 0;
			}
			std::int64_t Tell() override { return static_cast<std::int64_t>(m_position); }
			int Eof() override { return m_position >= m_data.size(); }
			bool MayBlock() override { return false; }
		  private:
			std::string m_data;
			std::size_t m_position{};
			IMPLEMENT_REFCOUNTING(MemoryReadHandler);
		};

		class AssetFactory final : public CefSchemeHandlerFactory
		{
		  public:
			CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
				const CefString&, CefRefPtr<CefRequest> request) override
			{
				CEF_REQUIRE_IO_THREAD();
				const CefURLParts parts = [] (const CefString& url) {
					CefURLParts value;
					CefParseURL(url, value);
					return value;
				}(request->GetURL());
				const std::string path = CefString(&parts.path);
				if (path != "/" && path != "/index.html")
					return nullptr;
				const std::string query = CefString(&parts.query);
				const bool pad = query.find("surface=pad") != std::string::npos;
				std::string windowId = "0";
				if (const auto start = query.find("windowId="); start != std::string::npos)
				{
					const auto valueStart = start + std::string_view("windowId=").size();
					const auto valueEnd = query.find('&', valueStart);
					windowId = query.substr(valueStart, valueEnd - valueStart);
					if (windowId.empty() || !std::ranges::all_of(windowId, [](unsigned char value) { return std::isdigit(value); }))
						return nullptr;
				}
				const std::string surface = pad ? "pad" : "tv";
				constexpr std::string_view bootstrapNonce = "cemu-cef-bootstrap";
				const std::string bootstrap =
					"<script nonce='" + std::string(bootstrapNonce) +
					"'>window.__CEMU_BOOTSTRAP__={windowId:'" + windowId +
					"',windowRole:'runtime-overlay',surface:'" + surface +
					"',overlaySurface:'" + surface +
					"',platform:'linux',language:'system'};document.documentElement.dataset.runtimeOverlay='active';"
					"window.cemuInvoke=(request)=>new Promise((resolve,reject)=>window.cemuCefQuery({request,onSuccess:resolve,onFailure:(code,message)=>reject(new Error('CEF RPC '+code+': '+message))}));</script>";
				const bool smoke = [] {
					const char* value = std::getenv("CEMU_CEF_OSR_SMOKE");
					return value && std::string_view(value) == "1";
				}();
				std::string html;
				if (smoke)
				{
					// Keep the CI OSR probe independent of the production React bundle's
					// parse time. The painted element is created only after a successful
					// round trip through the same CEF message-router RPC bridge.
					html = R"(<!doctype html><html><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="script-src 'nonce-cemu-cef-bootstrap'; style-src 'unsafe-inline'"><style>html,body{margin:0;width:100%;height:100%;background:transparent}#smoke{display:none;width:96px;height:48px;background:#25a7f0}</style></head><body><div id="smoke"></div><script nonce="cemu-cef-bootstrap">window.cemuInvoke(JSON.stringify({id:'cef-osr-smoke',method:'system.bootstrap',params:{}})).then(()=>document.getElementById('smoke').style.display='block');</script></body></html>)";
				}
				else
				{
					html.assign(reinterpret_cast<const char*>(WebAssets::html), WebAssets::htmlSize);
				}
				// The embedded document only permits the generated application hash. Add a
				// narrowly scoped nonce for this native-generated bootstrap, which must run
				// before the React bundle defines the bridge consumers.
				const auto scriptPolicy = html.find("script-src ");
				if (scriptPolicy == std::string::npos)
					return nullptr;
				html.insert(scriptPolicy + std::string_view("script-src ").size(),
					"'nonce-" + std::string(bootstrapNonce) + "' ");
				const auto head = html.find("<head>");
				if (head == std::string::npos)
					return nullptr;
				html.insert(head + std::string_view("<head>").size(), bootstrap);
				auto stream = CefStreamReader::CreateForHandler(new MemoryReadHandler(std::move(html)));
				return new CefStreamResourceHandler(200, "OK", "text/html", {}, stream);
			}

			IMPLEMENT_REFCOUNTING(AssetFactory);
		};

		class RuntimeImpl;

		class Client final : public CefClient,
					 public CefLifeSpanHandler,
					 public CefRenderHandler,
					 public CefLoadHandler,
					 public CefRequestHandler,
					 public CefMessageRouterBrowserSide::Handler
		{
		  public:
			Client(RuntimeImpl& owner, Host::PointerSurface surface, std::uint64_t windowId);
			CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
			CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
			CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
			CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

			void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override;
			bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& screenInfo) override;
			void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
				const RectList& dirtyRects, const void* buffer, int width, int height) override;
			void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override;
			void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override;
			void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
			void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
			void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
				ErrorCode errorCode, const CefString& errorText, const CefString&) override;
			bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				CefRefPtr<CefRequest> request, bool userGesture, bool isRedirect) override;
			bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				CefProcessId sourceProcess, CefRefPtr<CefProcessMessage> message) override;
			bool OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, std::int64_t,
				const CefString& request, bool, CefRefPtr<Callback> callback) override;

			void SetSize(int width, int height, double scale);
			std::pair<int, int> Size() const;
			double Scale() const;
			void RequestClose();
			CefRefPtr<CefBrowser> Browser() const { return m_browser; }
			bool HasReceivedPaint() const { return m_receivedPaint.load(std::memory_order_acquire); }

		  private:
			RuntimeImpl& m_owner;
			Host::PointerSurface m_surface;
			std::uint64_t m_windowId;
			mutable std::mutex m_sizeMutex;
			int m_width{1};
			int m_height{1};
			double m_scale{1.0};
			CefRefPtr<CefBrowser> m_browser;
			CefRefPtr<CefMessageRouterBrowserSide> m_router;
			std::atomic_bool m_receivedPaint{};
			bool m_closeRequested{};
			IMPLEMENT_REFCOUNTING(Client);
		};

		class RuntimeImpl final : public BrowserRuntime
		{
		  public:
			RuntimeImpl(RpcHandler rpc, std::function<void(Host::PointerSurface)> redraw,
				ClosedHandler closed)
				: m_rpc(std::move(rpc)), m_mailbox(std::move(redraw)), m_closed(std::move(closed)) {}

			~RuntimeImpl() override { CloseAll(); }
			bool Create(Host::PointerSurface surface, std::uint64_t windowId,
				int physicalWidth, int physicalHeight, double dpiScale) override;
			void Close(Host::PointerSurface surface) override;
			void CloseAll() override;
			void Resize(Host::PointerSurface surface, int physicalWidth,
				int physicalHeight, double dpiScale) override;
			void SetInteractive(Host::PointerSurface surface, bool interactive) override;
			bool SendInput(const NativeInputEvent& event) override;
			void ExecuteEvent(Host::PointerSurface surface, std::string_view name,
				std::string_view jsonPayload, std::uint64_t sequence) override;
			void ExecuteScript(Host::PointerSurface surface, std::string_view script) override;
			std::optional<Host::OverlayFrameSnapshot> AcquireLatestOverlayFrame(
				Host::PointerSurface surface, std::uint64_t afterSequence) override
			{
				return m_mailbox.AcquireLatestOverlayFrame(surface, afterSequence);
			}

			std::string Dispatch(std::uint64_t windowId, std::string_view request)
			{
				try { return m_rpc(windowId, request); }
				catch (const std::exception&)
				{
					return R"({"id":"","ok":false,"error":{"code":"cef_bridge_failure","message":"native RPC exception"}})";
				}
				catch (...) { return R"({"id":"","ok":false,"error":{"code":"cef_bridge_failure","message":"unknown native exception"}})"; }
			}
			void Created(Host::PointerSurface surface, CefRefPtr<CefBrowser> browser);
			void Closed(Host::PointerSurface surface);
			FrameMailbox& Mailbox() { return m_mailbox; }

		  private:
			static std::size_t Index(Host::PointerSurface surface) { return surface == Host::PointerSurface::Main ? 0 : 1; }
			CefRefPtr<Client> Get(Host::PointerSurface surface) const;
			RpcHandler m_rpc;
			FrameMailbox m_mailbox;
			ClosedHandler m_closed;
			mutable std::mutex m_mutex;
			std::array<CefRefPtr<Client>, 2> m_clients;
			std::array<bool, 2> m_interactive{};
			std::condition_variable m_closeCondition;
			bool m_closeAllStarted{};
		};

		Client::Client(RuntimeImpl& owner, Host::PointerSurface surface, std::uint64_t windowId)
			: m_owner(owner), m_surface(surface), m_windowId(windowId),
			  m_router(CefMessageRouterBrowserSide::Create(RouterConfig()))
		{
			m_router->AddHandler(this, false);
		}

		void Client::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect)
		{
			const auto [width, height] = Size();
			rect = CefRect(0, 0, width, height);
		}

		bool Client::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info)
		{
			const auto [width, height] = Size();
			info.device_scale_factor = static_cast<float>(Scale());
			info.rect = info.available_rect = CefRect(0, 0, width, height);
			return true;
		}

		void Client::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
			const RectList& dirty, const void* buffer, int width, int height)
		{
			m_receivedPaint.store(true, std::memory_order_release);
			std::vector<Host::OverlayDirtyRect> rects;
			rects.reserve(dirty.size());
			for (const auto& rect : dirty)
				rects.push_back({rect.x, rect.y, rect.width, rect.height});
			if (type == PET_VIEW)
				m_owner.Mailbox().PublishView(m_surface, width, height, buffer, width * 4, rects);
			else if (type == PET_POPUP)
				m_owner.Mailbox().PublishPopup(m_surface, width, height, buffer, width * 4, rects);
		}

		void Client::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show)
		{
			m_owner.Mailbox().SetPopupVisible(m_surface, show);
			if (!show)
				browser->GetHost()->Invalidate(PET_VIEW);
		}

		void Client::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect)
		{
			const double scale = Scale();
			m_owner.Mailbox().SetPopupRect(m_surface, {
				static_cast<int>(std::lround(rect.x * scale)),
				static_cast<int>(std::lround(rect.y * scale)),
				static_cast<int>(std::lround(rect.width * scale)),
				static_cast<int>(std::lround(rect.height * scale))});
		}

		void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser)
		{
			m_browser = browser;
			m_owner.Created(m_surface, browser);
			if (m_closeRequested)
			{
				browser->GetHost()->CloseBrowser(true);
				return;
			}
			auto* pending = new CefRefPtr<Client>(this);
			g_timeout_add_full(G_PRIORITY_DEFAULT, 5000,
				[](gpointer data) -> gboolean {
					auto& client = **static_cast<CefRefPtr<Client>*>(data);
					if (client.Browser() && !client.HasReceivedPaint())
						cemuLog_log(LogType::Force,
							"CEF Runtime Overlay did not produce a first OnPaint within 5 seconds");
					return G_SOURCE_REMOVE;
				}, pending,
				[](gpointer data) { delete static_cast<CefRefPtr<Client>*>(data); });
		}

		void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser)
		{
			m_router->OnBeforeClose(browser);
			m_router->RemoveHandler(this);
			m_browser = nullptr;
			m_owner.Closed(m_surface);
		}

		void Client::OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
			ErrorCode code, const CefString& text, const CefString&)
		{
			if (code != ERR_ABORTED)
				cemuLog_log(LogType::Force, "CEF overlay load failed: {} ({})", text.ToString(), static_cast<int>(code));
		}

		bool Client::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
			CefRefPtr<CefRequest> request, bool, bool)
		{
			const std::string url = request->GetURL();
			if (!url.starts_with("cemu://ui/"))
				return true;
			m_router->OnBeforeBrowse(browser, frame);
			return false;
		}

		bool Client::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
			CefRefPtr<CefFrame> frame, CefProcessId source, CefRefPtr<CefProcessMessage> message)
		{
			return m_router->OnProcessMessageReceived(browser, frame, source, message);
		}

		bool Client::OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, std::int64_t,
			const CefString& request, bool, CefRefPtr<Callback> callback)
		{
			callback->Success(m_owner.Dispatch(m_windowId, request.ToString()));
			return true;
		}

		void Client::SetSize(int width, int height, double scale)
		{
			std::scoped_lock lock(m_sizeMutex);
			m_scale = std::clamp(scale, 0.5, 8.0);
			// CEF's view rectangle and input coordinates are device-independent;
			// OnPaint and popup pixel buffers are scaled to physical pixels.
			m_width = std::max(static_cast<int>(std::lround(width / m_scale)), 1);
			m_height = std::max(static_cast<int>(std::lround(height / m_scale)), 1);
		}

		std::pair<int, int> Client::Size() const
		{
			std::scoped_lock lock(m_sizeMutex);
			return {m_width, m_height};
		}

		double Client::Scale() const
		{
			std::scoped_lock lock(m_sizeMutex);
			return m_scale;
		}

		void Client::RequestClose()
		{
			if (m_closeRequested)
				return;
			m_closeRequested = true;
			if (m_browser)
				m_browser->GetHost()->CloseBrowser(true);
		}

		CefRefPtr<Client> RuntimeImpl::Get(Host::PointerSurface surface) const
		{
			std::scoped_lock lock(m_mutex);
			return m_clients[Index(surface)];
		}

		bool RuntimeImpl::Create(Host::PointerSurface surface, std::uint64_t windowId,
			int width, int height, double scale)
		{
			CEF_REQUIRE_UI_THREAD();
			if (width <= 0 || height <= 0)
				return false;
			{
				std::scoped_lock lock(m_mutex);
				if (m_clients[Index(surface)])
					return true;
				m_mailbox.Reopen(surface);
				m_clients[Index(surface)] = new Client(*this, surface, windowId);
				m_clients[Index(surface)]->SetSize(width, height, scale);
			}
			CefWindowInfo windowInfo;
			windowInfo.SetAsWindowless(kNullWindowHandle);
			windowInfo.shared_texture_enabled = false;
			CefBrowserSettings settings;
			settings.background_color = CefColorSetARGB(0, 0, 0, 0);
			int frameRate = 60;
			if (const char* value = std::getenv("CEMU_CEF_OVERLAY_FPS"))
				frameRate = std::clamp(std::atoi(value), 30, 60);
			settings.windowless_frame_rate = frameRate;
			const std::string surfaceName = surface == Host::PointerSurface::Main ? "tv" : "pad";
			const std::string url = std::string(kUrl) + "?surface=" + surfaceName +
				"&windowId=" + std::to_string(windowId);
			if (!CefBrowserHost::CreateBrowser(windowInfo, Get(surface), url, settings, nullptr, nullptr))
			{
				std::scoped_lock lock(m_mutex);
				m_clients[Index(surface)] = nullptr;
				cemuLog_log(LogType::Force, "CEF overlay browser creation failed for {}", surfaceName);
				return false;
			}
			return true;
		}

		void RuntimeImpl::Created(Host::PointerSurface surface, CefRefPtr<CefBrowser> browser)
		{
			browser->GetHost()->SetFocus(m_interactive[Index(surface)]);
		}

		void RuntimeImpl::Closed(Host::PointerSurface surface)
		{
			{
				std::scoped_lock lock(m_mutex);
				m_clients[Index(surface)] = nullptr;
			}
			m_mailbox.BeginClose(surface);
			m_closeCondition.notify_all();
			if (m_closed)
				m_closed(surface);
		}

		void RuntimeImpl::Close(Host::PointerSurface surface)
		{
			CEF_REQUIRE_UI_THREAD();
			m_mailbox.BeginClose(surface);
			if (auto client = Get(surface))
				client->RequestClose();
		}

		void RuntimeImpl::CloseAll()
		{
			if (!g_initialized || m_closeAllStarted)
				return;
			m_closeAllStarted = true;
			Close(Host::PointerSurface::Pad);
			Close(Host::PointerSurface::Main);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while ((Get(Host::PointerSurface::Main) || Get(Host::PointerSurface::Pad)) &&
				std::chrono::steady_clock::now() < deadline)
			{
				// Client remembers a close requested before OnAfterCreated and sends it
				// once the asynchronous browser creation completes.
				CefDoMessageLoopWork();
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			if (Get(Host::PointerSurface::Main) || Get(Host::PointerSurface::Pad))
				cemuLog_log(LogType::Force, "CEF overlay browser shutdown timed out");
		}

		void RuntimeImpl::Resize(Host::PointerSurface surface, int width, int height, double scale)
		{
			if (width <= 0 || height <= 0)
				return;
			if (auto client = Get(surface))
			{
				client->SetSize(width, height, scale);
				if (auto browser = client->Browser())
				{
					browser->GetHost()->NotifyScreenInfoChanged();
					browser->GetHost()->WasResized();
				}
			}
		}

		void RuntimeImpl::SetInteractive(Host::PointerSurface surface, bool interactive)
		{
			m_interactive[Index(surface)] = interactive;
			if (auto client = Get(surface); client && client->Browser())
				client->Browser()->GetHost()->SetFocus(interactive);
		}

		bool RuntimeImpl::SendInput(const NativeInputEvent& event)
		{
			if (!m_interactive[Index(event.surface)])
				return false;
			auto client = Get(event.surface);
			if (!client || !client->Browser())
				return false;
			auto host = client->Browser()->GetHost();
			const double scale = client->Scale();
			CefMouseEvent mouse;
			mouse.x = static_cast<int>(std::lround(event.x / scale));
			mouse.y = static_cast<int>(std::lround(event.y / scale));
			mouse.modifiers =
				((event.modifiers & 1U) ? EVENTFLAG_CONTROL_DOWN : 0) |
				((event.modifiers & 2U) ? EVENTFLAG_SHIFT_DOWN : 0) |
				((event.modifiers & 4U) ? EVENTFLAG_ALT_DOWN : 0) |
				((event.modifiers & 8U) ? EVENTFLAG_COMMAND_DOWN : 0);
			switch (event.kind)
			{
			case NativeInputKind::PointerMove:
				host->SendMouseMoveEvent(mouse, !event.insideContent); return true;
			case NativeInputKind::PointerButton:
				host->SendMouseClickEvent(mouse,
					event.button == 3 ? MBT_RIGHT : event.button == 2 ? MBT_MIDDLE : MBT_LEFT,
					!event.pressed, 1); return true;
			case NativeInputKind::PointerWheel:
				host->SendMouseWheelEvent(mouse, event.wheelX, event.wheelY); return true;
			case NativeInputKind::Key:
			{
				CefKeyEvent key;
				key.type = event.pressed ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
				key.native_key_code = static_cast<int>(event.key);
				key.windows_key_code = static_cast<int>(event.key);
				key.modifiers = mouse.modifiers;
				host->SendKeyEvent(key); return true;
			}
			case NativeInputKind::Character:
				for (const auto value : CefString(event.text).ToString16())
				{
					CefKeyEvent key; key.type = KEYEVENT_CHAR; key.character = value;
					key.unmodified_character = value; host->SendKeyEvent(key);
				} return true;
			case NativeInputKind::Touch:
			{
				CefTouchEvent touch;
				touch.id = static_cast<int>(event.touchId);
				touch.x = static_cast<float>(event.x / scale);
				touch.y = static_cast<float>(event.y / scale);
				touch.type = event.pressed ? CEF_TET_PRESSED : CEF_TET_RELEASED;
				touch.pointer_type = CEF_POINTER_TYPE_TOUCH;
				host->SendTouchEvent(touch); return true;
			}
			case NativeInputKind::FocusLost:
				host->SetFocus(false); return true;
			default: return false;
			}
		}

		void RuntimeImpl::ExecuteEvent(Host::PointerSurface surface, std::string_view name,
			std::string_view payload, std::uint64_t sequence)
		{
			// Use Runtime's shared sequence. Mixing an independent counter with
			// broadcast events would make bridge/events.ts discard valid updates.
			ExecuteScript(surface, "window.__cemuDispatchEvent?.({type:'" + std::string(name) +
				"',sequence:'" + std::to_string(sequence) + "',payload:" +
				std::string(payload) + "})");
		}

		void RuntimeImpl::ExecuteScript(Host::PointerSurface surface, std::string_view script)
		{
			if (auto client = Get(surface); client && client->Browser())
				if (auto frame = client->Browser()->GetMainFrame())
					frame->ExecuteJavaScript(std::string(script), frame->GetURL(), 0);
		}
	} // namespace

	int ExecuteSubprocess(int argc, char* argv[])
	{
		// CefExecuteProcess may reorder argv while Chromium normalizes switches.
		// Cemu parses its own value-taking options afterwards, so passing the
		// caller-owned array can separate e.g. --title-id from its value. Keep an
		// owned copy for both CEF calls and rebuild its pointer order afterwards.
		g_arguments.clear();
		g_arguments.reserve(static_cast<std::size_t>(std::max(argc, 0)));
		for (int index = 0; index < argc; ++index)
			g_arguments.emplace_back(argv[index] ? argv[index] : "");
		RestoreArgumentPointers();
		g_app = new App;
		CefMainArgs args(g_argc, g_argv);
		const int result = CefExecuteProcess(args, g_app, nullptr);
		RestoreArgumentPointers();
		return result;
	}

	bool InitializeProcessRuntime()
	{
		if (g_initialized)
			return true;
		if (!g_app)
			g_app = new App;
		std::error_code executableError;
		const auto executable = std::filesystem::read_symlink("/proc/self/exe", executableError);
		if (executableError || executable.empty())
		{
			cemuLog_log(LogType::Force, "Unable to resolve the executable path for CEF: {}",
				executableError.message());
			return false;
		}
		const auto directory = executable.parent_path();
		if (!g_argv || g_argc <= 0)
		{
			cemuLog_log(LogType::Force, "CEF overlay initialization is missing process arguments");
			return false;
		}
		CefMainArgs args(g_argc, g_argv);
		CefSettings settings;
		settings.windowless_rendering_enabled = true;
		settings.multi_threaded_message_loop = false;
		// Cemu and the standalone OSR test call CefDoMessageLoopWork on a fixed
		// cadence. external_message_pump would additionally require implementing
		// OnScheduleMessagePumpWork; enabling it without that callback can starve
		// delayed browser creation and shutdown tasks.
		settings.external_message_pump = false;
		settings.background_color = CefColorSetARGB(0, 0, 0, 0);
		CefString(&settings.resources_dir_path) = directory.string();
		CefString(&settings.locales_dir_path) = (directory / "locales").string();
		CefString(&settings.browser_subprocess_path) = executable.string();
		std::filesystem::path cacheRoot;
		if (const char* xdgCache = std::getenv("XDG_CACHE_HOME"); xdgCache && *xdgCache)
			cacheRoot = std::filesystem::path(xdgCache) / "CemuExtend" / "cef-overlay";
		else
			cacheRoot = std::filesystem::temp_directory_path() /
				("cemu-cef-overlay-" + std::to_string(static_cast<unsigned long>(getuid())));
		std::error_code cacheError;
		std::filesystem::create_directories(cacheRoot, cacheError);
		if (cacheError)
		{
			cemuLog_log(LogType::Force, "Unable to create the CEF cache directory: {}", cacheError.message());
			return false;
		}
		CefString(&settings.root_cache_path) = cacheRoot.string();
		if (const char* noSandbox = std::getenv("CEMU_CEF_NO_SANDBOX"); noSandbox && std::string_view(noSandbox) == "1")
			settings.no_sandbox = true;
		if (!CefInitialize(args, settings, g_app, nullptr))
		{
			cemuLog_log(LogType::Force, "CefInitialize failed for the Runtime Overlay");
			return false;
		}
		if (!CefRegisterSchemeHandlerFactory(kScheme, kDomain, new AssetFactory))
		{
			CefShutdown();
			cemuLog_log(LogType::Force, "CEF overlay scheme registration failed");
			return false;
		}
		g_initialized = true;
		g_pumpSource = g_timeout_add_full(G_PRIORITY_DEFAULT, 5, &Pump, nullptr, nullptr);
		return true;
	}

	void DoProcessMessageLoopWork()
	{
		if (g_initialized)
			CefDoMessageLoopWork();
	}

	void ShutdownProcessRuntime()
	{
		if (!g_initialized)
			return;
		g_initialized = false;
		if (g_pumpSource)
			g_source_remove(g_pumpSource);
		g_pumpSource = 0;
		CefClearSchemeHandlerFactories();
		CefShutdown();
		g_app = nullptr;
	}

	std::shared_ptr<BrowserRuntime> CreateBrowserRuntime(BrowserRuntime::RpcHandler rpc,
		std::function<void(Host::PointerSurface)> redraw, BrowserRuntime::ClosedHandler closed)
	{
		if (!g_initialized)
			return {};
		return std::make_shared<RuntimeImpl>(std::move(rpc), std::move(redraw), std::move(closed));
	}
} // namespace WebFrontend::CefOverlay
