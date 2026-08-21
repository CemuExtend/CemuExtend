#include "webview/NativeWindowHost.h"

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#ifdef HAS_WAYLAND
#include <gdk/gdkwayland.h>
#endif

namespace WebFrontend
{
	namespace
	{
		bool IsWaylandDisplay(GdkDisplay* display)
		{
			const auto waylandType = g_type_from_name("GdkWaylandDisplay");
			return display && waylandType != G_TYPE_INVALID &&
				g_type_is_a(G_OBJECT_TYPE(display), waylandType);
		}

		Host::NativeWindowHandle NativeHandle(GtkWidget* widget)
		{
			if (!widget)
				return {};
			gtk_widget_realize(widget);
			auto* window = gtk_widget_get_window(widget);
			if (!window)
				return {};
			auto* display = gdk_window_get_display(window);
			Host::NativeWindowHandle result;
			if (GDK_IS_X11_WINDOW(window))
			{
				result.backend = Host::NativeWindowBackend::X11;
				result.display = gdk_x11_display_get_xdisplay(display);
				result.surface = reinterpret_cast<void*>(gdk_x11_window_get_xid(window));
			}
#ifdef HAS_WAYLAND
			else if (GDK_IS_WAYLAND_WINDOW(window))
			{
				result.backend = Host::NativeWindowBackend::Wayland;
				result.display = gdk_wayland_display_get_wl_display(display);
				result.surface = gdk_wayland_window_get_wl_surface(window);
			}
#endif
			return result;
		}

		class GtkRenderRegion final : public Host::IRenderRegion
		{
		public:
			explicit GtkRenderRegion(GtkWidget* stack)
				: m_stack(stack), m_widget(gtk_drawing_area_new())
			{
				gtk_widget_set_hexpand(m_widget, TRUE);
				gtk_widget_set_vexpand(m_widget, TRUE);
				gtk_widget_set_can_focus(m_widget, TRUE);
				gtk_stack_add_named(GTK_STACK(m_stack), m_widget, "render");
				gtk_widget_realize(m_widget);
			}

			~GtkRenderRegion() override { PrepareForDestroy(); }

			Host::NativeWindowHandle GetWindowHandle() const override
			{
				return NativeHandle(gtk_widget_get_toplevel(m_widget));
			}

			Host::NativeWindowHandle GetSurfaceHandle() const override
			{
				return NativeHandle(m_widget);
			}

			Host::RenderRegionBounds GetBounds() const override
			{
				GtkAllocation allocation{};
				gtk_widget_get_allocation(m_widget, &allocation);
				return {allocation.x, allocation.y, allocation.width, allocation.height};
			}

			void SetBounds(Host::RenderRegionBounds bounds) override
			{
				gtk_widget_set_size_request(m_widget,
					std::max(1, bounds.width), std::max(1, bounds.height));
				gtk_widget_set_margin_start(m_widget, std::max(0, bounds.x));
				gtk_widget_set_margin_top(m_widget, std::max(0, bounds.y));
			}

			void SetVisible(bool visible) override
			{
				if (visible)
					gtk_widget_show(m_widget);
				else
					gtk_widget_hide(m_widget);
			}

			void RequestFocus() override { gtk_widget_grab_focus(m_widget); }

			void PrepareForDestroy() override
			{
				if (std::exchange(m_prepared, true) || !m_widget)
					return;
				gtk_widget_hide(m_widget);
				gtk_container_remove(GTK_CONTAINER(m_stack), m_widget);
				m_widget = nullptr;
			}

		private:
			GtkWidget* m_stack{};
			GtkWidget* m_widget{};
			bool m_prepared{};
		};

		class GtkPadRenderRegion final : public Host::IRenderRegion
		{
		public:
			explicit GtkPadRenderRegion(std::function<void()> closeHandler,
				std::function<void()> metricsHandler)
				: m_closeHandler(std::move(closeHandler)),
				  m_metricsHandler(std::move(metricsHandler))
			{
				m_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
				gtk_window_set_title(GTK_WINDOW(m_window), "CemuExtend GamePad");
				gtk_window_set_default_size(GTK_WINDOW(m_window), 854, 480);
				m_widget = gtk_drawing_area_new();
				gtk_widget_set_can_focus(m_widget, TRUE);
				gtk_container_add(GTK_CONTAINER(m_window), m_widget);
				g_signal_connect(m_window, "delete-event", G_CALLBACK(+[](
					GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
						auto& self = *static_cast<GtkPadRenderRegion*>(data);
						if (self.m_closeHandler)
							self.m_closeHandler();
						return TRUE;
					}), this);
				g_signal_connect(m_widget, "size-allocate", G_CALLBACK(+[](
					GtkWidget*, GtkAllocation*, gpointer data) {
						auto& self = *static_cast<GtkPadRenderRegion*>(data);
						if (self.m_metricsHandler)
							self.m_metricsHandler();
					}), this);
				gtk_widget_show_all(m_window);
				gtk_widget_realize(m_widget);
				gtk_widget_grab_focus(m_widget);
			}

			~GtkPadRenderRegion() override { PrepareForDestroy(); }
			Host::NativeWindowHandle GetWindowHandle() const override { return NativeHandle(m_window); }
			Host::NativeWindowHandle GetSurfaceHandle() const override { return NativeHandle(m_widget); }
			int GetScaleFactor() const { return gtk_widget_get_scale_factor(m_widget); }
			Host::RenderRegionBounds GetBounds() const override
			{
				GtkAllocation allocation{};
				gtk_widget_get_allocation(m_widget, &allocation);
				return {0, 0, allocation.width, allocation.height};
			}
			void SetBounds(Host::RenderRegionBounds bounds) override
			{
				gtk_window_resize(GTK_WINDOW(m_window), std::max(1, bounds.width),
					std::max(1, bounds.height));
			}
			void SetVisible(bool visible) override
			{
				if (visible)
					gtk_widget_show(m_window);
				else
					gtk_widget_hide(m_window);
			}
			void RequestFocus() override
			{
				gtk_window_present(GTK_WINDOW(m_window));
				gtk_widget_grab_focus(m_widget);
			}
			void PrepareForDestroy() override
			{
				if (std::exchange(m_prepared, true) || !m_window)
					return;
				m_closeHandler = {};
				m_metricsHandler = {};
				gtk_widget_destroy(m_window);
				m_window = nullptr;
				m_widget = nullptr;
			}

		private:
			GtkWidget* m_window{};
			GtkWidget* m_widget{};
			std::function<void()> m_closeHandler;
			std::function<void()> m_metricsHandler;
			bool m_prepared{};
		};

		class GtkWindowHost final : public INativeWindowHost
		{
		public:
			GtkWindowHost()
			{
				if (!gtk_init_check(nullptr, nullptr))
					throw std::runtime_error("GTK initialization failed");
#ifndef HAS_WAYLAND
				if (IsWaylandDisplay(gdk_display_get_default()))
					throw std::runtime_error(
						"the current GTK session uses Wayland, but Cemu was built with ENABLE_WAYLAND=OFF");
#endif
				m_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
				gtk_window_set_title(GTK_WINDOW(m_window), "CemuExtend");
				gtk_window_set_default_size(GTK_WINDOW(m_window), 1100, 720);
				g_signal_connect(m_window, "delete-event", G_CALLBACK(+[](
					GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
						auto& self = *static_cast<GtkWindowHost*>(data);
						if (self.m_closeHandler)
							self.m_closeHandler();
						return TRUE;
					}), this);
				g_signal_connect(m_window, "size-allocate", G_CALLBACK(+[](
					GtkWidget*, GtkAllocation*, gpointer data) {
						static_cast<GtkWindowHost*>(data)->NotifyMetrics();
					}), this);
				g_signal_connect(m_window, "window-state-event", G_CALLBACK(+[](
					GtkWidget*, GdkEventWindowState*, gpointer data) -> gboolean {
						static_cast<GtkWindowHost*>(data)->NotifyMetrics();
						return FALSE;
					}), this);
				g_signal_connect(m_window, "focus-in-event", G_CALLBACK(+[](
					GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
						static_cast<GtkWindowHost*>(data)->NotifyMetrics();
						return FALSE;
					}), this);
				g_signal_connect(m_window, "focus-out-event", G_CALLBACK(+[](
					GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
						static_cast<GtkWindowHost*>(data)->NotifyMetrics();
						return FALSE;
					}), this);
			}

			~GtkWindowHost() override
			{
				DestroyPadRenderRegion();
				DestroyMainRenderRegion();
				if (m_window)
					gtk_widget_destroy(m_window);
			}

			void* GetNativeWindow() const override { return m_window; }
			Host::NativeWindowHandle GetMainWindowHandle() const override
			{
				return NativeHandle(m_window);
			}

			Host::WindowMetricsSnapshot GetMetrics() const override
			{
				GtkAllocation allocation{};
				gtk_widget_get_allocation(m_stack ? m_stack : m_window, &allocation);
				const auto scale = gtk_widget_get_scale_factor(m_window);
				auto metrics = Host::WindowMetricsSnapshot{
					.appActive = gtk_window_is_active(GTK_WINDOW(m_window)) != FALSE,
					.fullscreen = m_fullscreen,
					.width = allocation.width,
					.height = allocation.height,
					.physicalWidth = allocation.width * scale,
					.physicalHeight = allocation.height * scale,
					.dpiScale = static_cast<double>(scale),
				};
				if (m_padRenderRegion)
				{
					const auto pad = m_padRenderRegion->GetBounds();
					const auto padScale = m_padRenderRegion->GetScaleFactor();
					metrics.padOpen = true;
					metrics.padWidth = pad.width;
					metrics.padHeight = pad.height;
					metrics.physicalPadWidth = pad.width * padScale;
					metrics.physicalPadHeight = pad.height * padScale;
					metrics.padDpiScale = static_cast<double>(padScale);
				}
				return metrics;
			}

			void AttachWebView(void* widget) override
			{
				if (m_root || !GTK_IS_WIDGET(widget))
					throw std::logic_error("webview widget cannot be attached");
				m_webView = GTK_WIDGET(widget);
				g_object_ref(m_webView);
				gtk_container_remove(GTK_CONTAINER(m_window), m_webView);

				m_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
				m_menuBar = BuildMenu();
				m_stack = gtk_stack_new();
				gtk_stack_set_transition_type(GTK_STACK(m_stack), GTK_STACK_TRANSITION_TYPE_NONE);
				gtk_widget_set_hexpand(m_stack, TRUE);
				gtk_widget_set_vexpand(m_stack, TRUE);
				gtk_stack_add_named(GTK_STACK(m_stack), m_webView, "library");
				gtk_box_pack_start(GTK_BOX(m_root), m_menuBar, FALSE, FALSE, 0);
				gtk_box_pack_start(GTK_BOX(m_root), m_stack, TRUE, TRUE, 0);
				gtk_container_add(GTK_CONTAINER(m_window), m_root);
				g_object_unref(m_webView);
				ShowLibrary();
			}

			void PrepareWebViewDestroy(void* widget) override
			{
				if (!m_root || widget != m_webView)
					return;
				DestroyMainRenderRegion();
				g_object_ref(m_webView);
				gtk_container_remove(GTK_CONTAINER(m_stack), m_webView);
				gtk_container_remove(GTK_CONTAINER(m_window), m_root);
				gtk_container_add(GTK_CONTAINER(m_window), m_webView);
				g_object_unref(m_webView);
				m_root = nullptr;
				m_stack = nullptr;
				m_menuBar = nullptr;
				m_webView = nullptr;
			}

			void Show() override { gtk_widget_show_all(m_window); ShowLibrary(); }

			void ShowLibrary() override
			{
				if (!m_stack || !m_webView)
					return;
				gtk_widget_set_sensitive(m_webView, TRUE);
				gtk_stack_set_visible_child(GTK_STACK(m_stack), m_webView);
				gtk_widget_grab_focus(m_webView);
			}

			Host::IRenderRegion& CreateMainRenderRegion() override
			{
				if (!m_stack)
					throw std::logic_error("webview content host is not attached");
				if (!m_renderRegion)
					m_renderRegion = std::make_unique<GtkRenderRegion>(m_stack);
				return *m_renderRegion;
			}

			void DestroyMainRenderRegion() override
			{
				if (m_renderRegion)
					m_renderRegion->PrepareForDestroy();
				m_renderRegion.reset();
			}

			void ShowRenderRegion() override
			{
				auto& region = CreateMainRenderRegion();
				gtk_widget_set_sensitive(m_webView, FALSE);
				region.SetVisible(true);
				gtk_stack_set_visible_child_name(GTK_STACK(m_stack), "render");
				region.RequestFocus();
			}

			Host::IRenderRegion& CreatePadRenderRegion() override
			{
				if (!m_padRenderRegion)
				{
					m_padRenderRegion = std::make_unique<GtkPadRenderRegion>(
						[this] {
							if (m_padCloseHandler)
								m_padCloseHandler();
						},
						[this] { if (m_padMetricsEnabled) NotifyMetrics(); });
				}
				return *m_padRenderRegion;
			}

			void DestroyPadRenderRegion() override
			{
				m_padMetricsEnabled = false;
				m_padRenderRegion.reset();
				NotifyMetrics();
			}

			bool IsPadRenderRegionOpen() const override { return m_padRenderRegion != nullptr; }

			void SetFullscreen(bool fullscreen) override
			{
				m_fullscreen = fullscreen;
				if (fullscreen)
				{
					gtk_widget_hide(m_menuBar);
					gtk_window_fullscreen(GTK_WINDOW(m_window));
				}
				else
				{
					gtk_window_unfullscreen(GTK_WINDOW(m_window));
					gtk_widget_show(m_menuBar);
				}
			}

			void SetCloseHandler(CloseHandler handler) override
			{
				m_closeHandler = std::move(handler);
			}

			void SetMenuHandler(MenuHandler handler) override
			{
				m_menuHandler = std::move(handler);
			}

			void SetMetricsHandler(MetricsHandler handler) override
			{
				m_metricsHandler = std::move(handler);
				NotifyMetrics();
			}

			void SetPadCloseHandler(PadCloseHandler handler) override
			{
				m_padCloseHandler = std::move(handler);
			}
			void SetPadMetricsEnabled(bool enabled) override
			{
				m_padMetricsEnabled = enabled;
			}

		private:
			void NotifyMetrics()
			{
				if (m_metricsHandler && m_window)
					m_metricsHandler(GetMetrics());
			}

			GtkWidget* MenuItem(const char* label, MenuCommand command)
			{
				auto* item = gtk_menu_item_new_with_label(label);
				g_object_set_data(G_OBJECT(item), "cemu-command",
					GINT_TO_POINTER(static_cast<int>(command) + 1));
				g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem* item, gpointer data) {
					auto& self = *static_cast<GtkWindowHost*>(data);
					const auto raw = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "cemu-command"));
					if (self.m_menuHandler && raw > 0)
						self.m_menuHandler(static_cast<MenuCommand>(raw - 1));
				}), this);
				return item;
			}

			GtkWidget* BuildMenu()
			{
				auto* bar = gtk_menu_bar_new();
				auto addMenu = [bar](const char* name) {
					auto* root = gtk_menu_item_new_with_label(name);
					auto* menu = gtk_menu_new();
					gtk_menu_item_set_submenu(GTK_MENU_ITEM(root), menu);
					gtk_menu_shell_append(GTK_MENU_SHELL(bar), root);
					return menu;
				};
				auto* file = addMenu("File");
				gtk_menu_shell_append(GTK_MENU_SHELL(file), MenuItem("Load", MenuCommand::Load));
				gtk_menu_shell_append(GTK_MENU_SHELL(file), MenuItem("End emulation", MenuCommand::EndEmulation));
				gtk_menu_shell_append(GTK_MENU_SHELL(file), MenuItem("Exit", MenuCommand::Exit));
				auto* options = addMenu("Options");
				gtk_menu_shell_append(GTK_MENU_SHELL(options), MenuItem("Fullscreen", MenuCommand::ToggleFullscreen));
				gtk_menu_shell_append(GTK_MENU_SHELL(options), MenuItem("Separate GamePad view", MenuCommand::TogglePadView));
				gtk_menu_shell_append(GTK_MENU_SHELL(options), MenuItem("General Settings", MenuCommand::GeneralSettings));
				gtk_menu_shell_append(GTK_MENU_SHELL(options), MenuItem("Input Settings", MenuCommand::InputSettings));
				auto* tools = addMenu("Tools");
				gtk_menu_shell_append(GTK_MENU_SHELL(tools), MenuItem("Graphic Packs", MenuCommand::GraphicPacks));
				gtk_menu_shell_append(GTK_MENU_SHELL(tools), MenuItem("Title Manager", MenuCommand::TitleManager));
				(void)addMenu("CPU");
				(void)addMenu("NFC");
				auto* debug = addMenu("Debug");
				gtk_menu_shell_append(GTK_MENU_SHELL(debug), MenuItem("Logging", MenuCommand::Logging));
				auto* help = addMenu("Help");
				gtk_menu_shell_append(GTK_MENU_SHELL(help), MenuItem("About", MenuCommand::About));
				return bar;
			}

			GtkWidget* m_window{};
			GtkWidget* m_root{};
			GtkWidget* m_menuBar{};
			GtkWidget* m_stack{};
			GtkWidget* m_webView{};
			std::unique_ptr<GtkRenderRegion> m_renderRegion;
			std::unique_ptr<GtkPadRenderRegion> m_padRenderRegion;
			CloseHandler m_closeHandler;
			MenuHandler m_menuHandler;
			MetricsHandler m_metricsHandler;
			PadCloseHandler m_padCloseHandler;
			bool m_padMetricsEnabled{};
			bool m_fullscreen{};
		};
	}

	std::unique_ptr<INativeWindowHost> CreateNativeWindowHost()
	{
		return std::make_unique<GtkWindowHost>();
	}
}

#endif
