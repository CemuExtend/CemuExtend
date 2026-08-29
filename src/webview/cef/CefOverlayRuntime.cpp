#include "Common/precompiled.h"

#include "webview/cef/CefOverlayRuntime.h"
#include "webview/cef/CefOverlayFrameMailbox.h"
#include "webview/cef/CefNativeUiLoop.h"
#include "webview/generated/WebAssets.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_request.h"
#include "include/cef_resource_handler.h"
#include "include/cef_scheme.h"
#include "include/cef_stream.h"
#include "include/cef_values.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"
#include "include/wrapper/cef_resource_manager.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#if defined(OS_MAC)
#include "include/wrapper/cef_library_loader.h"
#endif

#if defined(OS_WIN)
#include <windows.h>
#elif defined(OS_MAC)
#include <mach-o/dyld.h>
#endif

#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace WebFrontend::CefOverlay
{
	namespace
	{
		constexpr char kScheme[] = "cemu";
		constexpr char kDomain[] = "ui";

		std::string JsonString(std::string_view value)
		{
			std::string result{"\""};
			for (const unsigned char character : value)
			{
				switch (character)
				{
				case '\\': result += "\\\\"; break;
				case '"': result += "\\\""; break;
				case '\b': result += "\\b"; break;
				case '\f': result += "\\f"; break;
				case '\n': result += "\\n"; break;
				case '\r': result += "\\r"; break;
				case '\t': result += "\\t"; break;
				default:
					if (character < 0x20)
						result += fmt::format("\\u{:04x}", character);
					else
						result += static_cast<char>(character);
				}
			}
			result += '"';
			return result;
		}

		std::string PlatformName()
		{
#if defined(OS_WIN)
			return "windows";
#elif defined(OS_MAC)
			return "macos";
#else
			return "linux";
#endif
		}

		std::optional<std::string> LoopbackOrigin(std::string_view url)
		{
			CefURLParts parts;
			if (!CefParseURL(std::string(url), parts))
				return std::nullopt;
			const std::string scheme = CefString(&parts.scheme);
			const std::string host = CefString(&parts.host);
			const std::string port = CefString(&parts.port);
			if (scheme != "http" || (host != "127.0.0.1" && host != "localhost") || port.empty())
				return std::nullopt;
			return scheme + "://" + host + ':' + port;
		}

		bool IsAllowedUrl(std::string_view candidate, std::string_view initialUrl)
		{
			if (candidate.starts_with("cemu://ui/"))
				return true;
			const auto initialOrigin = LoopbackOrigin(initialUrl);
			const auto candidateOrigin = LoopbackOrigin(candidate);
			return initialOrigin && candidateOrigin && *initialOrigin == *candidateOrigin;
		}

		std::string MakeBootstrap(const BrowserDescriptor& descriptor)
		{
			if (!descriptor.bootstrapJson.empty())
				return descriptor.bootstrapJson;
			std::string context = descriptor.contextJson.empty() ? "{}" : descriptor.contextJson;
			std::string result = "{windowId:" + JsonString(std::to_string(descriptor.windowId)) +
				",windowRole:" + JsonString(descriptor.role) +
				",platform:" + JsonString(PlatformName()) +
				",language:'system',context:" + context;
			if (descriptor.overlaySurface)
			{
				const char* surface = *descriptor.overlaySurface == Host::PointerSurface::Main ? "tv" : "pad";
				result += ",surface:" + JsonString(surface) + ",overlaySurface:" + JsonString(surface);
			}
			result += '}';
			return result;
		}

		void* NativeHandlePointer(CefWindowHandle handle)
		{
#if defined(OS_LINUX)
			return reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle));
#else
			return static_cast<void*>(handle);
#endif
		}

		CefMessageRouterConfig RouterConfig()
		{
			CefMessageRouterConfig config;
			config.js_query_function = "cemuCefQuery";
			config.js_cancel_function = "cemuCefQueryCancel";
			return config;
		}

		std::mutex g_contextTasksMutex;
		bool g_contextInitialized{};
		std::vector<std::function<void()>> g_contextTasks;

		class App final : public CefApp,
					  public CefBrowserProcessHandler,
					  public CefRenderProcessHandler
		{
		  public:
			CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
			CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
			void OnContextInitialized() override;

			void OnScheduleMessagePumpWork(std::int64_t delayMs) override
			{
				CefNative::ScheduleCefMessagePump(
					std::chrono::milliseconds(std::max<std::int64_t>(delayMs, 0)),
					[] { DoProcessMessageLoopWork(); });
			}

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
					const char* headless = std::getenv("CEMU_CEF_HEADLESS");
					const bool runHeadless = headless && std::string_view(headless) == "1";
					if (runHeadless)
					{
						commandLine->AppendSwitch("headless");
						commandLine->AppendSwitchWithValue("ozone-platform", "headless");
					}
#if defined(OS_LINUX)
					// The launcher and the tool windows host a windowed CEF child that
					// Cemu reparents into its GTK container by XID, and Chromium picks
					// GTK's GDK backend to match the Ozone platform it selected. In a
					// Wayland session that pairing hands out wl_surfaces the reparenting
					// path cannot use, so pin Ozone to X11 and let XWayland bridge the
					// desktop. An explicit switch on the command line still wins.
					else if (std::getenv("DISPLAY") &&
							 !commandLine->HasSwitch("ozone-platform") &&
							 !commandLine->HasSwitch("ozone-platform-hint"))
					{
						commandLine->AppendSwitchWithValue("ozone-platform", "x11");
					}
#endif
				}
			}

			void OnWebKitInitialized() override
			{
				m_rendererRouter = CefMessageRouterRendererSide::Create(RouterConfig());
			}

			void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
				CefRefPtr<CefDictionaryValue> extraInfo) override
			{
				if (extraInfo && extraInfo->HasKey("bootstrap"))
					m_bootstrap.insert_or_assign(browser->GetIdentifier(), extraInfo->GetString("bootstrap"));
			}

			void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override
			{
				m_bootstrap.erase(browser->GetIdentifier());
			}

			void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				CefRefPtr<CefV8Context> context) override
			{
				m_rendererRouter->OnContextCreated(browser, frame, context);
				if (!frame->IsMain())
					return;
				const auto entry = m_bootstrap.find(browser->GetIdentifier());
				if (entry == m_bootstrap.end())
					return;
				const std::string script =
					"window.__CEMU_BOOTSTRAP__=" + entry->second + ";"
					"window.cemuInvoke=(request)=>new Promise((resolve,reject)=>"
					"window.cemuCefQuery({request,onSuccess:resolve,onFailure:(code,message)=>"
					"reject(new Error('CEF RPC '+code+': '+message))}));"
					"(()=>{const mark=()=>{if(document.documentElement&&"
					"window.__CEMU_BOOTSTRAP__.overlaySurface)"
					"document.documentElement.dataset.runtimeOverlay='active'};mark();"
					"document.addEventListener('DOMContentLoaded',mark,{once:true})})();";
				CefRefPtr<CefV8Value> result;
				CefRefPtr<CefV8Exception> exception;
				context->Eval(script, frame->GetURL(), 0, result, exception);
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
			std::unordered_map<int, std::string> m_bootstrap;
			IMPLEMENT_REFCOUNTING(App);
		};

		CefRefPtr<App> g_app;
		bool g_initialized{};
		int g_argc{};
		char** g_argv{};
		std::vector<std::string> g_arguments;
		std::vector<char*> g_argumentPointers;
		std::mutex g_runtimesMutex;
		std::vector<std::weak_ptr<BrowserRuntime>> g_runtimes;
#if defined(OS_MAC)
		std::unique_ptr<CefScopedLibraryLoader> g_libraryLoader;
#endif

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

		std::filesystem::path ExecutablePath(std::error_code& error)
		{
#if defined(OS_WIN)
			std::wstring buffer(32768, L'\0');
			const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (!length || length >= buffer.size())
			{
				error = std::make_error_code(std::errc::no_such_file_or_directory);
				return {};
			}
			buffer.resize(length);
			return std::filesystem::path(buffer);
#elif defined(OS_MAC)
			std::uint32_t size{};
			_NSGetExecutablePath(nullptr, &size);
			std::string buffer(size, '\0');
			if (_NSGetExecutablePath(buffer.data(), &size) != 0)
			{
				error = std::make_error_code(std::errc::no_such_file_or_directory);
				return {};
			}
			buffer.resize(std::char_traits<char>::length(buffer.c_str()));
			return std::filesystem::weakly_canonical(buffer, error);
#else
			return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
		}

		std::filesystem::path CefResourceDirectory(const std::filesystem::path& executable)
		{
			if (const char* overridePath = std::getenv("CEMU_CEF_RESOURCES_DIR");
				overridePath && *overridePath)
				return overridePath;
#if defined(OS_MAC)
			const auto contents = executable.parent_path().parent_path();
			return contents / "Frameworks" / "Chromium Embedded Framework.framework" / "Resources";
#else
			return executable.parent_path();
#endif
		}

		std::filesystem::path CefSubprocessPath(const std::filesystem::path& executable)
		{
			if (const char* overridePath = std::getenv("CEMU_CEF_SUBPROCESS_PATH");
				overridePath && *overridePath)
				return overridePath;
#if defined(OS_MAC)
			const auto helper = executable.parent_path().parent_path() / "Frameworks" /
				"Cemu Helper.app" / "Contents" / "MacOS" / "Cemu Helper";
			std::error_code ignored;
			return std::filesystem::exists(helper, ignored) ? helper : executable;
#else
			return executable;
#endif
		}

		std::filesystem::path CefCacheRoot()
		{
#if defined(OS_WIN)
			if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData)
				return std::filesystem::path(localAppData) / "CemuExtend" / "cef";
#elif defined(OS_MAC)
			if (const char* home = std::getenv("HOME"); home && *home)
				return std::filesystem::path(home) / "Library" / "Caches" / "CemuExtend" / "cef";
#else
			if (const char* xdgCache = std::getenv("XDG_CACHE_HOME"); xdgCache && *xdgCache)
				return std::filesystem::path(xdgCache) / "CemuExtend" / "cef";
			if (const char* home = std::getenv("HOME"); home && *home)
				return std::filesystem::path(home) / ".cache" / "CemuExtend" / "cef";
#endif
			return std::filesystem::temp_directory_path() / "cemu-cef-cache";
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
			Client(RuntimeImpl& owner, BrowserDescriptor descriptor);
			CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
			CefRefPtr<CefRenderHandler> GetRenderHandler() override
			{
				return m_descriptor.presentation == BrowserPresentation::OverlayOsr ? this : nullptr;
			}
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
			std::uint64_t WindowId() const { return m_descriptor.windowId; }
			const BrowserDescriptor& Descriptor() const { return m_descriptor; }

		  private:
			RuntimeImpl& m_owner;
			BrowserDescriptor m_descriptor;
			mutable std::mutex m_sizeMutex;
			int m_width{1};
			int m_height{1};
			double m_scale{1.0};
			CefRefPtr<CefBrowser> m_browser;
			CefRefPtr<CefMessageRouterBrowserSide> m_router;
			bool m_closeRequested{};
			IMPLEMENT_REFCOUNTING(Client);
		};

		class RuntimeImpl final : public BrowserRuntime,
							 public std::enable_shared_from_this<RuntimeImpl>
		{
		  public:
			RuntimeImpl(RpcHandler rpc, std::function<void(Host::PointerSurface)> redraw,
				ClosedHandler closed, WindowClosedHandler windowClosed)
				: m_rpc(std::move(rpc)), m_mailbox(std::move(redraw)), m_closed(std::move(closed)),
				  m_windowClosed(std::move(windowClosed)) {}

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
			bool CreateBrowser(const BrowserDescriptor& descriptor) override;
			bool CloseWindow(std::uint64_t windowId) override;
			void ResizeWindow(std::uint64_t windowId, int width, int height, double scale) override;
			void SetWindowFocus(std::uint64_t windowId, bool focused) override;
			void ExecuteWindowEvent(std::uint64_t windowId, std::string_view name,
				std::string_view payload, std::uint64_t sequence) override;
			void ExecuteWindowScript(std::uint64_t windowId, std::string_view script) override;
			bool HasWindow(std::uint64_t windowId) const override;
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
			void Created(std::uint64_t windowId, CefRefPtr<CefBrowser> browser);
			void Closed(std::uint64_t windowId);
			void LaunchBrowser(std::uint64_t windowId);
			FrameMailbox& Mailbox() { return m_mailbox; }

		  private:
			static std::size_t Index(Host::PointerSurface surface) { return surface == Host::PointerSurface::Main ? 0 : 1; }
			CefRefPtr<Client> Get(Host::PointerSurface surface) const;
			CefRefPtr<Client> Get(std::uint64_t windowId) const;
			RpcHandler m_rpc;
			FrameMailbox m_mailbox;
			ClosedHandler m_closed;
			WindowClosedHandler m_windowClosed;
			mutable std::mutex m_mutex;
			std::unordered_map<std::uint64_t, CefRefPtr<Client>> m_clients;
			std::array<std::optional<std::uint64_t>, 2> m_surfaceWindows;
			std::array<bool, 2> m_interactive{};
			std::condition_variable m_closeCondition;
		};

		Client::Client(RuntimeImpl& owner, BrowserDescriptor descriptor)
			: m_owner(owner), m_descriptor(std::move(descriptor)),
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
			if (!m_descriptor.overlaySurface)
				return;
			std::vector<Host::OverlayDirtyRect> rects;
			rects.reserve(dirty.size());
			for (const auto& rect : dirty)
				rects.push_back({rect.x, rect.y, rect.width, rect.height});
			if (type == PET_VIEW)
				m_owner.Mailbox().PublishView(*m_descriptor.overlaySurface, width, height, buffer, width * 4, rects);
			else if (type == PET_POPUP)
				m_owner.Mailbox().PublishPopup(*m_descriptor.overlaySurface, width, height, buffer, width * 4, rects);
		}

		void Client::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show)
		{
			if (!m_descriptor.overlaySurface)
				return;
			m_owner.Mailbox().SetPopupVisible(*m_descriptor.overlaySurface, show);
			if (!show)
				browser->GetHost()->Invalidate(PET_VIEW);
		}

		void Client::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect)
		{
			if (!m_descriptor.overlaySurface)
				return;
			const double scale = Scale();
			m_owner.Mailbox().SetPopupRect(*m_descriptor.overlaySurface, {
				static_cast<int>(std::lround(rect.x * scale)),
				static_cast<int>(std::lround(rect.y * scale)),
				static_cast<int>(std::lround(rect.width * scale)),
				static_cast<int>(std::lround(rect.height * scale))});
		}

		void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser)
		{
			m_browser = browser;
			m_owner.Created(m_descriptor.windowId, browser);
			if (m_descriptor.presentation == BrowserPresentation::NativeChild &&
				m_descriptor.nativeBrowserCreated)
			{
				try
				{
					m_descriptor.nativeBrowserCreated(
						NativeHandlePointer(browser->GetHost()->GetWindowHandle()));
				}
				catch (...)
				{
					cemuLog_log(LogType::Force, "CEF native browser attach callback failed for window {}",
						m_descriptor.windowId);
				}
			}
			if (m_closeRequested)
			{
				browser->GetHost()->CloseBrowser(true);
				return;
			}
		}

		void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser)
		{
			if (m_descriptor.presentation == BrowserPresentation::NativeChild &&
				m_descriptor.nativeBrowserClosing)
			{
				try
				{
					m_descriptor.nativeBrowserClosing(
						NativeHandlePointer(browser->GetHost()->GetWindowHandle()));
				}
				catch (...)
				{
					cemuLog_log(LogType::Force, "CEF native browser detach callback failed for window {}",
						m_descriptor.windowId);
				}
			}
			m_router->OnBeforeClose(browser);
			m_router->RemoveHandler(this);
			m_browser = nullptr;
			m_owner.Closed(m_descriptor.windowId);
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
			if (!IsAllowedUrl(url, m_descriptor.initialUrl))
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
			callback->Success(m_owner.Dispatch(m_descriptor.windowId, request.ToString()));
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
			const auto window = m_surfaceWindows[Index(surface)];
			if (!window)
				return nullptr;
			const auto client = m_clients.find(*window);
			return client == m_clients.end() ? nullptr : client->second;
		}

		CefRefPtr<Client> RuntimeImpl::Get(std::uint64_t windowId) const
		{
			std::scoped_lock lock(m_mutex);
			const auto client = m_clients.find(windowId);
			return client == m_clients.end() ? nullptr : client->second;
		}

		bool RuntimeImpl::Create(Host::PointerSurface surface, std::uint64_t windowId,
			int width, int height, double scale)
		{
			BrowserDescriptor descriptor;
			descriptor.windowId = windowId;
			descriptor.role = "runtime-overlay";
			descriptor.presentation = BrowserPresentation::OverlayOsr;
			descriptor.overlaySurface = surface;
			descriptor.bounds.width = width;
			descriptor.bounds.height = height;
			descriptor.dpiScale = scale;
			return CreateBrowser(descriptor);
		}

		bool RuntimeImpl::CreateBrowser(const BrowserDescriptor& descriptor)
		{
			CEF_REQUIRE_UI_THREAD();
			if (descriptor.bounds.width <= 0 || descriptor.bounds.height <= 0 ||
				descriptor.dpiScale <= 0.0 || descriptor.initialUrl.empty())
				return false;
			if (!descriptor.initialUrl.starts_with("cemu://ui/") &&
				!LoopbackOrigin(descriptor.initialUrl))
				return false;
			if (descriptor.presentation == BrowserPresentation::OverlayOsr &&
				!descriptor.overlaySurface)
				return false;
			if (descriptor.presentation == BrowserPresentation::NativeChild &&
				!descriptor.nativeParent)
				return false;

			CefRefPtr<Client> client = new Client(*this, descriptor);
			client->SetSize(descriptor.bounds.width, descriptor.bounds.height, descriptor.dpiScale);
			{
				std::scoped_lock lock(m_mutex);
				if (m_clients.contains(descriptor.windowId))
					return true;
				if (descriptor.overlaySurface && m_surfaceWindows[Index(*descriptor.overlaySurface)])
					return false;
				m_clients.emplace(descriptor.windowId, client);
				if (descriptor.overlaySurface)
				{
					m_surfaceWindows[Index(*descriptor.overlaySurface)] = descriptor.windowId;
					m_mailbox.Reopen(*descriptor.overlaySurface);
				}
			}

			auto launch = [weak = weak_from_this(), windowId = descriptor.windowId] {
				if (const auto runtime = weak.lock())
					runtime->LaunchBrowser(windowId);
			};
			bool launchNow{};
			{
				std::scoped_lock lock(g_contextTasksMutex);
				launchNow = g_contextInitialized;
				if (!launchNow)
					g_contextTasks.emplace_back(launch);
			}
			if (launchNow)
				launch();
			return true;
		}

		void RuntimeImpl::LaunchBrowser(std::uint64_t windowId)
		{
			CEF_REQUIRE_UI_THREAD();
			const auto client = Get(windowId);
			if (!client || client->Browser())
				return;
			const auto& descriptor = client->Descriptor();

			CefWindowInfo windowInfo;
			CefWindowHandle parent = kNullWindowHandle;
#if defined(OS_LINUX)
			if (descriptor.nativeParent)
				parent = static_cast<CefWindowHandle>(reinterpret_cast<std::uintptr_t>(descriptor.nativeParent));
#else
			if (descriptor.nativeParent)
				parent = static_cast<CefWindowHandle>(descriptor.nativeParent);
#endif
			if (descriptor.presentation == BrowserPresentation::OverlayOsr)
			{
				windowInfo.SetAsWindowless(parent);
				windowInfo.shared_texture_enabled = false;
			}
			else
			{
				windowInfo.SetAsChild(parent, CefRect(descriptor.bounds.x, descriptor.bounds.y,
					descriptor.bounds.width, descriptor.bounds.height));
			}
			CefBrowserSettings settings;
			settings.background_color = descriptor.presentation == BrowserPresentation::OverlayOsr
				? CefColorSetARGB(0, 0, 0, 0) : CefColorSetARGB(255, 32, 32, 32);
			if (descriptor.presentation == BrowserPresentation::OverlayOsr)
			{
				int frameRate = 60;
				if (const char* value = std::getenv("CEMU_CEF_OVERLAY_FPS"))
					frameRate = std::clamp(std::atoi(value), 30, 60);
				settings.windowless_frame_rate = frameRate;
			}
			auto extraInfo = CefDictionaryValue::Create();
			extraInfo->SetString("bootstrap", MakeBootstrap(descriptor));
			if (!CefBrowserHost::CreateBrowser(windowInfo, client, descriptor.initialUrl,
				settings, extraInfo, nullptr))
			{
				cemuLog_log(LogType::Force, "CEF browser creation failed for role {} window {}",
					descriptor.role, descriptor.windowId);
				Closed(descriptor.windowId);
			}
		}

		void App::OnContextInitialized()
		{
			CEF_REQUIRE_UI_THREAD();
			std::vector<std::function<void()>> tasks;
			{
				std::scoped_lock lock(g_contextTasksMutex);
				g_contextInitialized = true;
				tasks.swap(g_contextTasks);
			}
			for (auto& task : tasks)
				task();
		}

		void RuntimeImpl::Created(std::uint64_t windowId, CefRefPtr<CefBrowser> browser)
		{
			if (auto client = Get(windowId); client && client->Descriptor().overlaySurface)
				browser->GetHost()->SetFocus(m_interactive[Index(*client->Descriptor().overlaySurface)]);
		}

		void RuntimeImpl::Closed(std::uint64_t windowId)
		{
			std::optional<Host::PointerSurface> surface;
			{
				std::scoped_lock lock(m_mutex);
				const auto client = m_clients.find(windowId);
				if (client == m_clients.end())
					return;
				surface = client->second->Descriptor().overlaySurface;
				m_clients.erase(client);
				if (surface)
					m_surfaceWindows[Index(*surface)].reset();
			}
			if (surface)
				m_mailbox.BeginClose(*surface);
			m_closeCondition.notify_all();
			try
			{
				if (surface && m_closed)
					m_closed(*surface);
			}
			catch (...)
			{
				cemuLog_log(LogType::Force, "CEF overlay close callback failed for window {}", windowId);
			}
			try
			{
				if (m_windowClosed)
					m_windowClosed(windowId);
			}
			catch (...)
			{
				cemuLog_log(LogType::Force, "CEF window close callback failed for window {}", windowId);
			}
		}

		void RuntimeImpl::Close(Host::PointerSurface surface)
		{
			m_mailbox.BeginClose(surface);
			if (auto client = Get(surface))
				CloseWindow(client->WindowId());
		}

		bool RuntimeImpl::CloseWindow(std::uint64_t windowId)
		{
			CEF_REQUIRE_UI_THREAD();
			if (auto client = Get(windowId))
			{
				if (client->Descriptor().overlaySurface)
					m_mailbox.BeginClose(*client->Descriptor().overlaySurface);
				if (client->Browser())
					client->RequestClose();
				else
					Closed(windowId);
				return true;
			}
			return false;
		}

		void RuntimeImpl::CloseAll()
		{
			if (!g_initialized)
				return;
			for (;;)
			{
				std::vector<CefRefPtr<Client>> clients;
				{
					std::scoped_lock lock(m_mutex);
					if (m_clients.empty())
						break;
					clients.reserve(m_clients.size());
					for (const auto& [windowId, client] : m_clients)
						clients.push_back(client);
				}
				for (const auto& client : clients)
				{
					if (client->Browser())
						client->RequestClose();
					else
						Closed(client->WindowId());
				}
				CefDoMessageLoopWork();
				std::unique_lock lock(m_mutex);
				m_closeCondition.wait_for(lock, std::chrono::milliseconds(5));
			}
		}

		void RuntimeImpl::Resize(Host::PointerSurface surface, int width, int height, double scale)
		{
			if (auto client = Get(surface))
				ResizeWindow(client->WindowId(), width, height, scale);
		}

		void RuntimeImpl::ResizeWindow(std::uint64_t windowId, int width, int height, double scale)
		{
			if (width <= 0 || height <= 0 || scale <= 0.0)
				return;
			if (auto client = Get(windowId))
			{
				client->SetSize(width, height, scale);
				if (auto browser = client->Browser())
				{
					if (client->Descriptor().presentation == BrowserPresentation::OverlayOsr)
					{
						browser->GetHost()->NotifyScreenInfoChanged();
						browser->GetHost()->WasResized();
					}
					else
						browser->GetHost()->NotifyMoveOrResizeStarted();
				}
			}
		}

		void RuntimeImpl::SetInteractive(Host::PointerSurface surface, bool interactive)
		{
			m_interactive[Index(surface)] = interactive;
			if (auto client = Get(surface); client && client->Browser())
				client->Browser()->GetHost()->SetFocus(interactive);
		}

		void RuntimeImpl::SetWindowFocus(std::uint64_t windowId, bool focused)
		{
			if (auto client = Get(windowId); client && client->Browser())
				client->Browser()->GetHost()->SetFocus(focused);
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
			if (auto client = Get(surface))
				ExecuteWindowEvent(client->WindowId(), name, payload, sequence);
		}

		void RuntimeImpl::ExecuteScript(Host::PointerSurface surface, std::string_view script)
		{
			if (auto client = Get(surface))
				ExecuteWindowScript(client->WindowId(), script);
		}

		void RuntimeImpl::ExecuteWindowEvent(std::uint64_t windowId, std::string_view name,
			std::string_view payload, std::uint64_t sequence)
		{
			ExecuteWindowScript(windowId, "window.__cemuDispatchEvent?.({type:" + JsonString(name) +
				",sequence:" + JsonString(std::to_string(sequence)) + ",payload:" +
				std::string(payload) + "})");
		}

		void RuntimeImpl::ExecuteWindowScript(std::uint64_t windowId, std::string_view script)
		{
			if (auto client = Get(windowId); client && client->Browser())
				if (auto frame = client->Browser()->GetMainFrame())
					frame->ExecuteJavaScript(std::string(script), frame->GetURL(), 0);
		}

		bool RuntimeImpl::HasWindow(std::uint64_t windowId) const
		{
			return static_cast<bool>(Get(windowId));
		}
	} // namespace

	int ExecuteSubprocess(int argc, char* argv[])
	{
#if defined(OS_MAC)
		if (!g_libraryLoader)
		{
			auto loader = std::make_unique<CefScopedLibraryLoader>();
			if (!loader->LoadInMain())
				return 1;
			g_libraryLoader = std::move(loader);
		}
#endif
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
#if defined(OS_WIN)
		CefMainArgs args(GetModuleHandleW(nullptr));
#else
		CefMainArgs args(g_argc, g_argv);
#endif
		const int result = CefExecuteProcess(args, g_app, nullptr);
		RestoreArgumentPointers();
#if defined(OS_MAC)
		if (result >= 0)
			g_libraryLoader.reset();
#endif
		return result;
	}

	int ExecuteHelperSubprocess(int argc, char* argv[])
	{
#if defined(OS_MAC)
		CefScopedLibraryLoader loader;
		if (!loader.LoadInHelper())
			return 1;
		g_arguments.clear();
		g_arguments.reserve(static_cast<std::size_t>(std::max(argc, 0)));
		for (int index = 0; index < argc; ++index)
			g_arguments.emplace_back(argv[index] ? argv[index] : "");
		RestoreArgumentPointers();
		g_app = new App;
		CefMainArgs args(g_argc, g_argv);
		const int result = CefExecuteProcess(args, g_app, nullptr);
		RestoreArgumentPointers();
		g_app = nullptr;
		return result;
#else
		return ExecuteSubprocess(argc, argv);
#endif
	}

	bool InitializeProcessRuntime()
	{
		if (g_initialized)
			return true;
		{
			std::scoped_lock lock(g_contextTasksMutex);
			g_contextInitialized = false;
			g_contextTasks.clear();
		}
		if (!CefNative::InitializeNativeUiLoop())
		{
			cemuLog_log(LogType::Force, "Unable to initialize the native CEF UI loop");
			return false;
		}
		if (!g_app)
			g_app = new App;
		std::error_code executableError;
		const auto executable = ExecutablePath(executableError);
		if (executableError || executable.empty())
		{
			cemuLog_log(LogType::Force, "Unable to resolve the executable path for CEF: {}",
				executableError.message());
			CefNative::ShutdownNativeUiLoop();
			return false;
		}
		const auto resources = CefResourceDirectory(executable);
		const auto subprocess = CefSubprocessPath(executable);
		if (!g_argv || g_argc <= 0)
		{
			cemuLog_log(LogType::Force, "CEF overlay initialization is missing process arguments");
			CefNative::ShutdownNativeUiLoop();
			return false;
		}
#if defined(OS_WIN)
		CefMainArgs args(GetModuleHandleW(nullptr));
#else
		CefMainArgs args(g_argc, g_argv);
#endif
		CefSettings settings;
		settings.windowless_rendering_enabled = true;
		settings.multi_threaded_message_loop = false;
		// CefNativeUiLoop translates CEF's requested deadlines into GLib, Win32,
		// or CFRunLoop work on the frontend UI thread and supplies a bounded
		// watchdog when Chromium does not schedule the next cycle itself.
		settings.external_message_pump = true;
		settings.background_color = CefColorSetARGB(0, 0, 0, 0);
#if defined(OS_WIN)
		CefString(&settings.resources_dir_path) = resources.wstring();
		CefString(&settings.locales_dir_path) = (resources / "locales").wstring();
		CefString(&settings.browser_subprocess_path) = subprocess.wstring();
#else
		CefString(&settings.resources_dir_path) = resources.string();
		CefString(&settings.locales_dir_path) = (resources / "locales").string();
		CefString(&settings.browser_subprocess_path) = subprocess.string();
#endif
		const auto cacheRoot = CefCacheRoot();
		std::error_code cacheError;
		std::filesystem::create_directories(cacheRoot, cacheError);
		if (cacheError)
		{
			cemuLog_log(LogType::Force, "Unable to create the CEF cache directory: {}", cacheError.message());
			CefNative::ShutdownNativeUiLoop();
			return false;
		}
#if defined(OS_WIN)
		CefString(&settings.root_cache_path) = cacheRoot.wstring();
#else
		CefString(&settings.root_cache_path) = cacheRoot.string();
#endif
#if defined(OS_WIN)
		// Cemu's Windows CEF distribution is built without cef_sandbox.lib.
		settings.no_sandbox = true;
#endif
		if (const char* noSandbox = std::getenv("CEMU_CEF_NO_SANDBOX"); noSandbox && std::string_view(noSandbox) == "1")
			settings.no_sandbox = true;
		if (!CefInitialize(args, settings, g_app, nullptr))
		{
			cemuLog_log(LogType::Force, "CefInitialize failed for the Runtime Overlay");
			CefNative::ShutdownNativeUiLoop();
			return false;
		}
		if (!CefRegisterSchemeHandlerFactory(kScheme, kDomain, new AssetFactory))
		{
			CefShutdown();
			CefNative::ShutdownNativeUiLoop();
			cemuLog_log(LogType::Force, "CEF overlay scheme registration failed");
			return false;
		}
		g_initialized = true;
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
		std::vector<std::shared_ptr<BrowserRuntime>> runtimes;
		{
			std::scoped_lock lock(g_runtimesMutex);
			for (auto& runtime : g_runtimes)
				if (auto active = runtime.lock())
					runtimes.push_back(std::move(active));
			g_runtimes.clear();
		}
		for (const auto& runtime : runtimes)
			runtime->CloseAll();
		g_initialized = false;
		CefClearSchemeHandlerFactories();
		CefShutdown();
		{
			std::scoped_lock lock(g_contextTasksMutex);
			g_contextInitialized = false;
			g_contextTasks.clear();
		}
		CefNative::ShutdownNativeUiLoop();
		g_app = nullptr;
#if defined(OS_MAC)
		g_libraryLoader.reset();
#endif
	}

	bool IsProcessRuntimeInitialized()
	{
		return g_initialized;
	}

	std::shared_ptr<BrowserRuntime> CreateBrowserRuntime(BrowserRuntime::RpcHandler rpc,
		std::function<void(Host::PointerSurface)> redraw, BrowserRuntime::ClosedHandler closed,
		BrowserRuntime::WindowClosedHandler windowClosed)
	{
		if (!g_initialized)
			return {};
		auto runtime = std::make_shared<RuntimeImpl>(std::move(rpc), std::move(redraw),
			std::move(closed), std::move(windowClosed));
		{
			std::scoped_lock lock(g_runtimesMutex);
			std::erase_if(g_runtimes, [](const auto& value) { return value.expired(); });
			g_runtimes.emplace_back(runtime);
		}
		return runtime;
	}
} // namespace WebFrontend::CefOverlay
