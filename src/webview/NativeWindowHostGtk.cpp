#include "webview/NativeWindowHost.h"

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <webkit2/webkit2.h>
#ifdef HAS_WAYLAND
#include <gdk/gdkwayland.h>
#endif

namespace WebFrontend
{
	namespace
	{
		struct GtkInputBinding
		{
			Host::PointerSurface surface{Host::PointerSurface::Main};
			INativeWindowHost::InputHandler* handler{};
			std::unordered_set<std::uint32_t> pressedKeys;
			bool captured{};
			bool rawMouseEnabled{};
			bool warpCapture{};
			bool positionValid{};
			double lastX{};
			double lastY{};
		};

		void DispatchGtkInput(GtkWidget* source, GtkInputBinding* binding,
							  NativeInputEvent event)
		{
			if (!binding || !binding->handler || !*binding->handler)
				return;
			event.surface = binding->surface;
			GtkAllocation allocation{};
			gtk_widget_get_allocation(source, &allocation);
			const auto scale = gtk_widget_get_scale_factor(source);
			event.contentWidth = allocation.width * scale;
			event.contentHeight = allocation.height * scale;
			(*binding->handler)(event);
		}

		void ConnectInput(GtkWidget* widget, Host::PointerSurface surface,
						  INativeWindowHost::InputHandler* handler)
		{
			gtk_widget_add_events(widget, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
											  GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK | GDK_TOUCH_MASK |
											  GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);
			auto* binding = new GtkInputBinding{surface, handler};
			g_object_set_data_full(G_OBJECT(widget), "cemu-input-binding", binding, +[](gpointer data) { delete static_cast<GtkInputBinding*>(data); });
			g_signal_connect(widget, "motion-notify-event", G_CALLBACK((+[](GtkWidget* source, GdkEventMotion* event, gpointer data) -> gboolean {
								 auto* binding = static_cast<GtkInputBinding*>(data);
								 const auto scale = gtk_widget_get_scale_factor(source);
								 if (binding->captured)
								 {
									 if (binding->warpCapture)
									 {
										 GtkAllocation allocation{};
										 gtk_widget_get_allocation(source, &allocation);
										 const auto centerX = allocation.width / 2;
										 const auto centerY = allocation.height / 2;
										 const auto deltaX = static_cast<std::int32_t>((event->x - centerX) * scale);
										 const auto deltaY = static_cast<std::int32_t>((event->y - centerY) * scale);
										 if (deltaX || deltaY)
										 {
											 if (binding->rawMouseEnabled)
												 DispatchGtkInput(source, binding, {.kind = NativeInputKind::RawMouse, .deltaX = deltaX, .deltaY = deltaY});
											 else
												 DispatchGtkInput(source, binding, {.kind = NativeInputKind::PointerMove, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .insideContent = true});
										 }
										 auto* window = gtk_widget_get_window(source);
										 auto* xdisplay = gdk_x11_display_get_xdisplay(gtk_widget_get_display(source));
										 XWarpPointer(xdisplay, None, gdk_x11_window_get_xid(window),
													  0, 0, 0, 0, centerX * scale, centerY * scale);
										 XFlush(xdisplay);
										 return TRUE;
									 }
									 if (binding->positionValid)
									 {
										 if (binding->rawMouseEnabled)
											 DispatchGtkInput(source, binding, {.kind = NativeInputKind::RawMouse, .deltaX = static_cast<std::int32_t>((event->x - binding->lastX) * scale), .deltaY = static_cast<std::int32_t>((event->y - binding->lastY) * scale)});
										 else
											 DispatchGtkInput(source, binding, {.kind = NativeInputKind::PointerMove, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .insideContent = true});
									 }
									 binding->lastX = event->x;
									 binding->lastY = event->y;
									 binding->positionValid = true;
									 return TRUE;
								 }
								 NativeInputEvent input{.kind = NativeInputKind::PointerMove,
														.x = static_cast<std::int32_t>(event->x * scale),
														.y = static_cast<std::int32_t>(event->y * scale),
														.insideContent = true};
								 DispatchGtkInput(source, binding, std::move(input));
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(widget, "button-press-event", G_CALLBACK((+[](GtkWidget* source, GdkEventButton* event, gpointer data) -> gboolean {
								 const auto scale = gtk_widget_get_scale_factor(source);
								 DispatchGtkInput(source, static_cast<GtkInputBinding*>(data), {.kind = NativeInputKind::PointerButton, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .button = event->button, .pressed = true, .insideContent = true});
								 gtk_widget_grab_focus(source);
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(widget, "button-release-event", G_CALLBACK((+[](GtkWidget* source, GdkEventButton* event, gpointer data) -> gboolean {
								 const auto scale = gtk_widget_get_scale_factor(source);
								 DispatchGtkInput(source, static_cast<GtkInputBinding*>(data), {.kind = NativeInputKind::PointerButton, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .button = event->button, .pressed = false, .insideContent = true});
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(widget, "scroll-event", G_CALLBACK((+[](GtkWidget* source, GdkEventScroll* event, gpointer data) -> gboolean {
								 double dx{}, dy{};
								 if (!gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(event), &dx, &dy))
								 {
									 dx = event->direction == GDK_SCROLL_LEFT ? -1 : event->direction == GDK_SCROLL_RIGHT ? 1
																														  : 0;
									 dy = event->direction == GDK_SCROLL_UP ? -1 : event->direction == GDK_SCROLL_DOWN ? 1
																													   : 0;
								 }
								 DispatchGtkInput(source, static_cast<GtkInputBinding*>(data), {.kind = NativeInputKind::PointerWheel, .wheelX = static_cast<std::int32_t>(-dx * 120.0), .wheelY = static_cast<std::int32_t>(-dy * 120.0)});
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(widget, "key-press-event", G_CALLBACK((+[](GtkWidget* source, GdkEventKey* event, gpointer data) -> gboolean {
								 auto* binding = static_cast<GtkInputBinding*>(data);
								 const auto modifiers = static_cast<std::uint8_t>(
									 ((event->state & GDK_CONTROL_MASK) ? 1U : 0U) |
									 ((event->state & GDK_SHIFT_MASK) ? 2U : 0U) |
									 ((event->state & GDK_MOD1_MASK) ? 4U : 0U) |
									 ((event->state & GDK_META_MASK) ? 8U : 0U));
								 const bool repeat = !binding->pressedKeys.insert(event->keyval).second;
								 DispatchGtkInput(source, binding, {.kind = NativeInputKind::Key, .key = event->keyval, .modifiers = modifiers, .pressed = true, .repeat = repeat});
								 if (event->string && event->length > 0)
									 DispatchGtkInput(source, binding, {.kind = NativeInputKind::Character, .repeat = repeat, .text = std::string(event->string, event->length)});
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(widget, "key-release-event", G_CALLBACK((+[](GtkWidget* source, GdkEventKey* event, gpointer data) -> gboolean {
								 auto* binding = static_cast<GtkInputBinding*>(data);
								 bool autoRepeatRelease = false;
								 if (auto* next = gdk_event_peek())
								 {
									 autoRepeatRelease = next->type == GDK_KEY_PRESS &&
														 next->key.hardware_keycode == event->hardware_keycode &&
														 next->key.time == event->time;
									 gdk_event_free(next);
								 }
								 if (autoRepeatRelease)
									 return TRUE;
								 binding->pressedKeys.erase(event->keyval);
								 const auto modifiers = static_cast<std::uint8_t>(
									 ((event->state & GDK_CONTROL_MASK) ? 1U : 0U) |
									 ((event->state & GDK_SHIFT_MASK) ? 2U : 0U) |
									 ((event->state & GDK_MOD1_MASK) ? 4U : 0U) |
									 ((event->state & GDK_META_MASK) ? 8U : 0U));
								 DispatchGtkInput(source, binding, {.kind = NativeInputKind::Key, .key = event->keyval, .modifiers = modifiers, .pressed = false});
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(widget, "focus-out-event", G_CALLBACK((+[](GtkWidget* source, GdkEventFocus*, gpointer data) -> gboolean {
								 auto* binding = static_cast<GtkInputBinding*>(data);
								 binding->pressedKeys.clear();
								 DispatchGtkInput(source, binding,
												  {.kind = NativeInputKind::FocusLost});
								 return FALSE;
							 })),
							 binding);
			g_signal_connect(widget, "touch-event", G_CALLBACK((+[](GtkWidget* source, GdkEventTouch* event, gpointer data) -> gboolean {
								 const auto scale = gtk_widget_get_scale_factor(source);
								 const bool pressed = event->type != GDK_TOUCH_END && event->type != GDK_TOUCH_CANCEL;
								 DispatchGtkInput(source, static_cast<GtkInputBinding*>(data), {.kind = NativeInputKind::Touch, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .touchId = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(event->sequence)), .pressed = pressed, .insideContent = true});
								 return TRUE;
							 })),
							 binding);
		}

		bool IsWaylandDisplay(GdkDisplay* display)
		{
			const auto waylandType = g_type_from_name("GdkWaylandDisplay");
			return display && waylandType != G_TYPE_INVALID &&
				   g_type_is_a(G_OBJECT_TYPE(display), waylandType);
		}

		void ConfigureTransparentGtkWindow(GtkWidget* window)
		{
			if (!window)
				throw std::logic_error("transparent GTK window is unavailable");
			const auto screen = gtk_widget_get_screen(window);
			const auto visual = screen ? gdk_screen_get_rgba_visual(screen) : nullptr;
			if (!visual)
				throw std::runtime_error(
					"the WebView runtime overlay requires an RGBA visual");
			gtk_widget_set_visual(window, visual);
			gtk_widget_set_app_paintable(window, TRUE);
		}

		bool PaintCompositedChild(GtkWidget* parent, GtkWidget* child, cairo_t* cr,
								  cairo_operator_t op)
		{
			if (!parent || !child || !cr)
				return false;
			const auto childWindow = gtk_widget_get_window(child);
			if (!childWindow || !gdk_window_get_composited(childWindow))
				return false;
			gint x{};
			gint y{};
			if (!gtk_widget_translate_coordinates(child, parent, 0, 0, &x, &y))
				return false;
			const auto width = gtk_widget_get_allocated_width(child);
			const auto height = gtk_widget_get_allocated_height(child);
			if (width <= 0 || height <= 0)
				return false;
			cairo_save(cr);
			cairo_rectangle(cr, x, y, width, height);
			cairo_clip(cr);
			cairo_set_operator(cr, op);
			gdk_cairo_set_source_window(cr, childWindow, x, y);
			cairo_paint(cr);
			cairo_restore(cr);
			return true;
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
			explicit GtkRenderRegion(GtkWidget* stack, INativeWindowHost::InputHandler* inputHandler)
				: m_stack(stack), m_widget(gtk_drawing_area_new())
			{
				gtk_widget_set_hexpand(m_widget, TRUE);
				gtk_widget_set_vexpand(m_widget, TRUE);
				gtk_widget_set_can_focus(m_widget, TRUE);
				gtk_stack_add_named(GTK_STACK(m_stack), m_widget, "render");
				gtk_widget_realize(m_widget);
				ConnectInput(m_widget, Host::PointerSurface::Main, inputHandler);
			}

			~GtkRenderRegion() override
			{
				PrepareForDestroy();
			}

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

			void RequestFocus() override
			{
				gtk_widget_grab_focus(m_widget);
			}
			GtkWidget* Widget() const
			{
				return m_widget;
			}
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
			explicit GtkPadRenderRegion(std::string title, Host::PointerSurface surface,
										std::function<void()> closeHandler,
										std::function<void()> metricsHandler, INativeWindowHost::InputHandler* inputHandler)
				: m_closeHandler(std::move(closeHandler)),
				  m_metricsHandler(std::move(metricsHandler))
			{
				m_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
				try
				{
					ConfigureTransparentGtkWindow(m_window);
					gtk_window_set_title(GTK_WINDOW(m_window), title.c_str());
					gtk_window_set_default_size(GTK_WINDOW(m_window),
												surface == Host::PointerSurface::Main ? 1280 : 854,
												surface == Host::PointerSurface::Main ? 720 : 480);
					m_overlay = gtk_overlay_new();
					m_widget = gtk_drawing_area_new();
					if (const auto screen = gtk_widget_get_screen(m_window))
					{
						if (const auto visual = gdk_screen_get_system_visual(screen))
							gtk_widget_set_visual(m_widget, visual);
					}
					gtk_widget_set_hexpand(m_widget, TRUE);
					gtk_widget_set_vexpand(m_widget, TRUE);
					gtk_widget_set_can_focus(m_widget, TRUE);
					gtk_container_add(GTK_CONTAINER(m_overlay), m_widget);
					gtk_container_add(GTK_CONTAINER(m_window), m_overlay);
					g_signal_connect(m_window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
										 auto& self = *static_cast<GtkPadRenderRegion*>(data);
										 if (self.m_closeHandler)
											 self.m_closeHandler();
										 return TRUE;
									 }),
									 this);
					g_signal_connect(m_widget, "size-allocate", G_CALLBACK(+[](GtkWidget*, GtkAllocation*, gpointer data) {
										 auto& self = *static_cast<GtkPadRenderRegion*>(data);
										 if (self.m_metricsHandler)
											 self.m_metricsHandler();
									 }),
									 this);
					g_signal_connect_after(m_window, "draw", G_CALLBACK(+[](GtkWidget* source, cairo_t* cr, gpointer data) -> gboolean {
										 auto& self = *static_cast<GtkPadRenderRegion*>(data);
										 if (self.m_compositeReady && self.m_widget)
											 PaintCompositedChild(source, self.m_widget, cr,
																	 CAIRO_OPERATOR_SOURCE);
										 if (self.m_webCompositeReady && self.m_overlayWebView)
											 PaintCompositedChild(source, self.m_overlayWebView, cr,
																	 CAIRO_OPERATOR_OVER);
										 return FALSE;
									 }),
									 this);
					g_signal_connect(m_widget, "unrealize", G_CALLBACK(+[](GtkWidget*, gpointer data) {
										 static_cast<GtkPadRenderRegion*>(data)->m_compositeReady = false;
									 }),
									 this);
#if !defined(CEMU_OVERLAY_BACKEND_WEBVIEW)
					FinalizeCompositeSurface();
#endif
					ConnectInput(m_widget, surface, inputHandler);
				} catch (...)
				{
					if (m_window)
						gtk_widget_destroy(m_window);
					m_window = nullptr;
					m_overlay = nullptr;
					m_widget = nullptr;
					throw;
				}
			}

			~GtkPadRenderRegion() override
			{
				PrepareForDestroy();
			}
			Host::NativeWindowHandle GetWindowHandle() const override
			{
				return NativeHandle(m_window);
			}
			Host::NativeWindowHandle GetSurfaceHandle() const override
			{
				if (!m_compositeReady)
					throw std::logic_error(
						"the GTK render surface was requested before its final overlay hierarchy was realized");
				return NativeHandle(m_widget);
			}
			int GetScaleFactor() const
			{
				return gtk_widget_get_scale_factor(m_widget);
			}
			GtkWidget* Widget() const
			{
				return m_widget;
			}
			GtkWidget* WindowWidget() const
			{
				return m_window;
			}
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
			void SetFullscreen(bool fullscreen)
			{
				if (fullscreen)
					gtk_window_fullscreen(GTK_WINDOW(m_window));
				else
					gtk_window_unfullscreen(GTK_WINDOW(m_window));
			}
			bool IsActive() const
			{
				return m_window && gtk_window_is_active(GTK_WINDOW(m_window)) != FALSE;
			}
			void PrepareOverlayWebViewCreate()
			{
				if (!m_window || !m_overlay || m_overlayDetached)
					return;
				DisableCompositeSurface();
				g_object_ref(m_overlay);
				gtk_container_remove(GTK_CONTAINER(m_window), m_overlay);
				m_overlayDetached = true;
			}
			void AttachOverlayWebView(GtkWidget* webView)
			{
				if (!m_window || !m_overlay || !webView)
					throw std::logic_error("GamePad overlay host is unavailable");
				g_object_ref(webView);
				if (const auto parent = gtk_widget_get_parent(webView))
					gtk_container_remove(GTK_CONTAINER(parent), webView);
				RestoreOverlayParent();
				gtk_widget_set_halign(webView, GTK_ALIGN_FILL);
				gtk_widget_set_valign(webView, GTK_ALIGN_FILL);
				gtk_widget_set_hexpand(webView, TRUE);
				gtk_widget_set_vexpand(webView, TRUE);
				gtk_overlay_add_overlay(GTK_OVERLAY(m_overlay), webView);
				g_object_unref(webView);
				m_overlayWebView = webView;
				try
				{
					FinalizeCompositeSurface();
					FinalizeWebCompositeSurface();
					SetOverlayInteractive(false);
					RequestCompositeRedraw();
				} catch (...)
				{
					DetachOverlayWebView(webView);
					throw;
				}
			}
			void DetachOverlayWebView(GtkWidget* webView)
			{
				if (!m_window || !m_overlay || webView != m_overlayWebView)
					return;
				DisableWebCompositeSurface();
				g_object_ref(webView);
				gtk_container_remove(GTK_CONTAINER(m_overlay), webView);
				PrepareOverlayWebViewCreate();
				gtk_container_add(GTK_CONTAINER(m_window), webView);
				g_object_unref(webView);
				m_overlayWebView = nullptr;
			}
			void RestoreOverlayParent()
			{
				if (!m_window || !m_overlay)
					return;
				if (m_overlayDetached)
				{
					gtk_container_add(GTK_CONTAINER(m_window), m_overlay);
					g_object_unref(m_overlay);
					m_overlayDetached = false;
				}
			}
			void SetOverlayInteractive(bool interactive)
			{
				if (!m_overlayWebView)
					return;
				const bool changed = !m_overlayInteractionInitialized ||
									 interactive != m_overlayInteractive;
				m_overlayInteractionInitialized = true;
				m_overlayInteractive = interactive;
				gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(m_overlay),
													 m_overlayWebView, interactive ? FALSE : TRUE);
				if (!changed)
					return;
				if (interactive)
					gtk_widget_grab_focus(m_overlayWebView);
				else
					gtk_widget_grab_focus(m_widget);
			}
			void RequestCompositeRedraw()
			{
				if (!m_window || !m_widget || !m_compositeReady)
					return;
				const auto parentWindow = gtk_widget_get_window(m_window);
				if (!parentWindow)
					return;
				gint x{};
				gint y{};
				if (!gtk_widget_translate_coordinates(m_widget, m_window, 0, 0, &x, &y))
					return;
				const auto width = gtk_widget_get_allocated_width(m_widget);
				const auto height = gtk_widget_get_allocated_height(m_widget);
				if (width <= 0 || height <= 0)
					return;
				GdkRectangle rect{x, y, width, height};
				// Redraw only the parent. Invalidating child windows clears the Vulkan
				// presentation target before the XComposite pixmap can be sampled.
				gdk_window_invalidate_rect(parentWindow, &rect, FALSE);
			}
			void PrepareForDestroy() override
			{
				if (std::exchange(m_prepared, true) || !m_window)
					return;
				m_closeHandler = {};
				m_metricsHandler = {};
				RestoreOverlayParent();
				DisableCompositeSurface();
				gtk_widget_destroy(m_window);
				m_window = nullptr;
				m_overlay = nullptr;
				m_widget = nullptr;
				m_overlayWebView = nullptr;
			}

		  private:
			void FinalizeCompositeSurface()
			{
				if (!m_window || !m_overlay || !m_widget || m_overlayDetached)
					throw std::logic_error("the GTK render overlay hierarchy is incomplete");
				gtk_widget_show_all(m_window);
				gtk_widget_realize(m_widget);
				const auto renderWindow = gtk_widget_get_window(m_widget);
				const auto display = renderWindow ? gdk_window_get_display(renderWindow) : nullptr;
				if (!renderWindow || !display || !GDK_IS_X11_DISPLAY(display) ||
					!gdk_display_supports_composite(display))
					throw std::runtime_error(
						"the WebView runtime overlay requires X11 XComposite/XDamage child composition");
				if (!gdk_window_get_composited(renderWindow))
					gdk_window_set_composited(renderWindow, TRUE);
				m_compositeReady = true;
				RequestCompositeRedraw();
				gtk_widget_grab_focus(m_widget);
			}
			void FinalizeWebCompositeSurface()
			{
				if (!m_overlayWebView)
					throw std::logic_error("overlay WebView is unavailable");
				gtk_widget_realize(m_overlayWebView);
				const auto webWindow = gtk_widget_get_window(m_overlayWebView);
				const auto display = webWindow ? gdk_window_get_display(webWindow) : nullptr;
				if (!webWindow || !display || !GDK_IS_X11_WINDOW(webWindow) ||
					!gdk_display_supports_composite(display))
					throw std::runtime_error(
						"the WebKitGTK overlay requires an X11 composited child window");
				if (!gdk_window_get_composited(webWindow))
					gdk_window_set_composited(webWindow, TRUE);
				m_webCompositeReady = true;
			}
			void DisableWebCompositeSurface()
			{
				if (m_overlayWebView)
				{
					if (const auto webWindow = gtk_widget_get_window(m_overlayWebView);
						webWindow && gdk_window_get_composited(webWindow))
						gdk_window_set_composited(webWindow, FALSE);
				}
				m_webCompositeReady = false;
			}
			void DisableCompositeSurface()
			{
				DisableWebCompositeSurface();
				if (m_widget)
				{
					if (const auto renderWindow = gtk_widget_get_window(m_widget);
						renderWindow && gdk_window_get_composited(renderWindow))
						gdk_window_set_composited(renderWindow, FALSE);
				}
				m_compositeReady = false;
			}

			GtkWidget* m_window{};
			GtkWidget* m_overlay{};
			GtkWidget* m_widget{};
			GtkWidget* m_overlayWebView{};
			std::function<void()> m_closeHandler;
			std::function<void()> m_metricsHandler;
			bool m_overlayDetached{};
			bool m_overlayInteractionInitialized{};
			bool m_overlayInteractive{};
			bool m_compositeReady{};
			bool m_webCompositeReady{};
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
				g_signal_connect(m_window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
									 auto& self = *static_cast<GtkWindowHost*>(data);
									 if (self.m_closeHandler)
										 self.m_closeHandler();
									 return TRUE;
								 }),
								 this);
				g_signal_connect(m_window, "size-allocate", G_CALLBACK(+[](GtkWidget*, GtkAllocation*, gpointer data) {
									 static_cast<GtkWindowHost*>(data)->NotifyMetrics();
								 }),
								 this);
				g_signal_connect(m_window, "window-state-event", G_CALLBACK(+[](GtkWidget*, GdkEventWindowState*, gpointer data) -> gboolean {
									 static_cast<GtkWindowHost*>(data)->NotifyMetrics();
									 return FALSE;
								 }),
								 this);
				g_signal_connect(m_window, "focus-in-event", G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
									 static_cast<GtkWindowHost*>(data)->NotifyMetrics();
									 return FALSE;
								 }),
								 this);
				g_signal_connect(m_window, "focus-out-event", G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
									 static_cast<GtkWindowHost*>(data)->NotifyMetrics();
									 return FALSE;
								 }),
								 this);
			}

			~GtkWindowHost() override
			{
				ReleasePointerGrab();
				DestroyPadRenderRegion();
				DestroyMainRenderRegion();
				if (m_window)
					gtk_widget_destroy(m_window);
			}

			void* GetNativeWindow() const override
			{
				return m_window;
			}
			Host::NativeWindowHandle GetMainWindowHandle() const override
			{
				return NativeHandle(m_window);
			}

			Host::WindowMetricsSnapshot GetMetrics() const override
			{
				GtkAllocation allocation{};
				int scale = gtk_widget_get_scale_factor(m_window);
				bool appActive = gtk_window_is_active(GTK_WINDOW(m_window)) != FALSE;
				if (m_renderRegion)
				{
					const auto bounds = m_renderRegion->GetBounds();
					allocation.width = bounds.width;
					allocation.height = bounds.height;
					scale = m_renderRegion->GetScaleFactor();
					appActive = m_renderRegion->IsActive();
				}
				else
					gtk_widget_get_allocation(m_stack ? m_stack : m_window, &allocation);
				auto metrics = Host::WindowMetricsSnapshot{
					.appActive = appActive,
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
				m_stack = gtk_stack_new();
				gtk_stack_set_transition_type(GTK_STACK(m_stack), GTK_STACK_TRANSITION_TYPE_NONE);
				gtk_widget_set_hexpand(m_stack, TRUE);
				gtk_widget_set_vexpand(m_stack, TRUE);
				gtk_stack_add_named(GTK_STACK(m_stack), m_webView, "library");
				m_overlay = gtk_overlay_new();
				gtk_container_add(GTK_CONTAINER(m_overlay), m_stack);
				m_textInput = gtk_entry_new();
				gtk_entry_set_has_frame(GTK_ENTRY(m_textInput), FALSE);
				gtk_entry_set_input_purpose(GTK_ENTRY(m_textInput), GTK_INPUT_PURPOSE_FREE_FORM);
				gtk_widget_set_opacity(m_textInput, 0.0);
				gtk_widget_set_halign(m_textInput, GTK_ALIGN_START);
				gtk_widget_set_valign(m_textInput, GTK_ALIGN_START);
				gtk_widget_set_size_request(m_textInput, 2, 24);
				gtk_overlay_add_overlay(GTK_OVERLAY(m_overlay), m_textInput);
				gtk_widget_hide(m_textInput);
				g_signal_connect(m_textInput, "changed", G_CALLBACK(+[](GtkEditable* editable, gpointer data) {
									 auto& self = *static_cast<GtkWindowHost*>(data);
									 if (self.m_textInputUpdating || !self.m_inputHandler)
										 return;
									 const char* text = gtk_entry_get_text(GTK_ENTRY(editable));
									 const auto characterOffset = gtk_editable_get_position(editable);
									 const char* cursor = text ? g_utf8_offset_to_pointer(text, characterOffset) : nullptr;
									 self.m_inputHandler({.kind = NativeInputKind::TextComposition,
														  .text = text ? text : "",
														  .preedit = self.m_textPreedit,
														  .textCursor = text && cursor ? static_cast<std::uint32_t>(cursor - text) : 0,
														  .selectionLength = static_cast<std::uint32_t>(self.m_textPreedit.size()),
														  .textSequence = self.m_textInputSequence});
								 }),
								 this);
				g_signal_connect(m_textInput, "preedit-changed", G_CALLBACK(+[](GtkEntry*, gchar* preedit, gpointer data) {
									 auto& self = *static_cast<GtkWindowHost*>(data);
									 self.m_textPreedit = preedit ? preedit : "";
									 if (!self.m_textInputUpdating && self.m_inputHandler)
									 {
										 const char* text = gtk_entry_get_text(GTK_ENTRY(self.m_textInput));
										 const auto characterOffset = gtk_editable_get_position(GTK_EDITABLE(self.m_textInput));
										 const char* cursor = text ? g_utf8_offset_to_pointer(text, characterOffset) : nullptr;
										 self.m_inputHandler({.kind = NativeInputKind::TextComposition,
															  .text = text ? text : "",
															  .preedit = self.m_textPreedit,
															  .textCursor = text && cursor ? static_cast<std::uint32_t>(cursor - text) : 0,
															  .selectionLength = static_cast<std::uint32_t>(self.m_textPreedit.size()),
															  .textSequence = self.m_textInputSequence});
									 }
								 }),
								 this);
				gtk_box_pack_start(GTK_BOX(m_root), m_overlay, TRUE, TRUE, 0);
				gtk_container_add(GTK_CONTAINER(m_window), m_root);
				g_object_unref(m_webView);
				ShowLibrary();
			}

			void ConfigureRuntimeOverlayWebView(void* browserController) override
			{
				if (!WEBKIT_IS_WEB_VIEW(browserController))
					throw std::runtime_error("failed to acquire the WebKitGTK browser controller");
				auto* view = WEBKIT_WEB_VIEW(browserController);
				auto* settings = webkit_web_view_get_settings(view);
				if (!settings)
					throw std::runtime_error("failed to acquire the WebKitGTK runtime settings");
				webkit_settings_set_hardware_acceleration_policy(
					settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
				GdkRGBA transparent{};
				webkit_web_view_set_background_color(view, &transparent);
			}

			void PrepareWebViewDestroy(void* widget) override
			{
				if (!m_root || widget != m_webView)
					return;
				DestroyMainRenderRegion();
				g_object_ref(m_webView);
				if (const auto parent = gtk_widget_get_parent(m_webView))
					gtk_container_remove(GTK_CONTAINER(parent), m_webView);
				gtk_container_remove(GTK_CONTAINER(m_window), m_root);
				gtk_container_add(GTK_CONTAINER(m_window), m_webView);
				g_object_unref(m_webView);
				m_root = nullptr;
				m_overlay = nullptr;
				m_stack = nullptr;
				m_webView = nullptr;
				m_textInput = nullptr;
			}

			void Show() override
			{
				gtk_widget_show_all(m_window);
				ShowLibrary();
			}

			void HideLauncher() override
			{
				gtk_widget_hide(m_window);
			}

			bool IsLauncherVisible() const override
			{
				return m_window && gtk_widget_get_visible(m_window) != FALSE;
			}

			void ShowLibrary() override
			{
				if (!m_stack || !m_webView)
					return;
				SetRuntimeOverlayMode(false, true);
				gtk_widget_set_sensitive(m_webView, TRUE);
				gtk_stack_set_visible_child(GTK_STACK(m_stack), m_webView);
				gtk_widget_grab_focus(m_webView);
			}

			Host::IRenderRegion& CreateMainRenderRegion() override
			{
				if (!m_renderRegion)
					m_renderRegion = std::make_unique<GtkPadRenderRegion>(
						"CemuExtend Game", Host::PointerSurface::Main,
						[this] { if (m_gameCloseHandler) m_gameCloseHandler(); },
						[this] { NotifyMetrics(); }, &m_inputHandler);
				return *m_renderRegion;
			}

			void* GetOverlayWebViewParent(Host::PointerSurface surface) const override
			{
				const auto* region = surface == Host::PointerSurface::Main
									 ? m_renderRegion.get() : m_padRenderRegion.get();
				return region ? region->WindowWidget() : nullptr;
			}

			void PrepareMainOverlayWebViewCreate() override
			{
				if (m_renderRegion)
					m_renderRegion->PrepareOverlayWebViewCreate();
			}

			void AttachMainOverlayWebView(void* widget) override
			{
				if (!m_renderRegion)
					throw std::logic_error("game render region is unavailable");
				m_renderRegion->AttachOverlayWebView(GTK_WIDGET(widget));
			}

			void DetachMainOverlayWebView(void* widget) override
			{
				if (m_renderRegion)
					m_renderRegion->DetachOverlayWebView(GTK_WIDGET(widget));
			}

			void RestoreMainOverlayParent() override
			{
				if (m_renderRegion)
					m_renderRegion->RestoreOverlayParent();
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
				region.SetVisible(true);
#if defined(CEMU_OVERLAY_BACKEND_WEBVIEW)
				SetRuntimeOverlayMode(true, false);
#else
				gtk_widget_set_sensitive(m_webView, FALSE);
				region.RequestFocus();
#endif
			}

			void SetRuntimeOverlayMode(bool active, bool interactive) override
			{
				m_runtimeOverlay = active;
				m_runtimeOverlayInteractive = interactive;
				if (active && interactive)
					ReleasePointerGrab();
				if (m_renderRegion)
					m_renderRegion->SetOverlayInteractive(active && interactive);
			}

			void RequestRenderRedraw(Host::PointerSurface surface) override
			{
				auto* region = surface == Host::PointerSurface::Main
								 ? m_renderRegion.get() : m_padRenderRegion.get();
				if (region)
					region->RequestCompositeRedraw();
			}

			Host::IRenderRegion& CreatePadRenderRegion() override
			{
				if (!m_padRenderRegion)
				{
					m_padRenderRegion = std::make_unique<GtkPadRenderRegion>(
						"CemuExtend GamePad", Host::PointerSurface::Pad,
						[this] {
							if (m_padCloseHandler)
								m_padCloseHandler();
						},
						[this] { if (m_padMetricsEnabled) NotifyMetrics(); }, &m_inputHandler);
				}
				return *m_padRenderRegion;
			}

			void PreparePadOverlayWebViewCreate() override
			{
				if (m_padRenderRegion)
					m_padRenderRegion->PrepareOverlayWebViewCreate();
			}

			void AttachPadOverlayWebView(void* widget) override
			{
				if (!m_padRenderRegion)
					throw std::logic_error("GamePad render region is unavailable");
				m_padRenderRegion->AttachOverlayWebView(GTK_WIDGET(widget));
			}

			void DetachPadOverlayWebView(void* widget) override
			{
				if (m_padRenderRegion)
					m_padRenderRegion->DetachOverlayWebView(GTK_WIDGET(widget));
			}

			void RestorePadOverlayParent() override
			{
				if (m_padRenderRegion)
					m_padRenderRegion->RestoreOverlayParent();
			}

			void SetPadRuntimeOverlayMode(bool interactive) override
			{
				if (interactive)
					ReleasePointerGrab();
				if (m_padRenderRegion)
					m_padRenderRegion->SetOverlayInteractive(interactive);
			}

			void DestroyPadRenderRegion() override
			{
				m_padMetricsEnabled = false;
				m_padRenderRegion.reset();
				NotifyMetrics();
			}

			bool IsPadRenderRegionOpen() const override
			{
				return m_padRenderRegion != nullptr;
			}

			void SetFullscreen(bool fullscreen) override
			{
				m_fullscreen = fullscreen;
				if (m_renderRegion)
					m_renderRegion->SetFullscreen(fullscreen);
			}

			void SetCloseHandler(CloseHandler handler) override
			{
				m_closeHandler = std::move(handler);
			}

			void SetGameCloseHandler(GameCloseHandler handler) override
			{
				m_gameCloseHandler = std::move(handler);
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
			void SetInputHandler(InputHandler handler) override
			{
				m_inputHandler = std::move(handler);
			}
			void ApplyPointerPresentation(const NativePointerPresentation& presentation) override
			{
				auto* widget = presentation.surface == Host::PointerSurface::Main
								   ? (m_renderRegion ? m_renderRegion->Widget() : nullptr)
								   : (m_padRenderRegion ? m_padRenderRegion->Widget() : nullptr);
				if (!widget || !gtk_widget_get_window(widget))
					return;
				auto* binding = static_cast<GtkInputBinding*>(
					g_object_get_data(G_OBJECT(widget), "cemu-input-binding"));
				const bool wantsCapture = presentation.ownsPointer && !presentation.showCursor;
				if (binding && binding->captured != wantsCapture)
					binding->positionValid = false;
				auto* display = gtk_widget_get_display(widget);
				auto* cursor = presentation.showCursor
								   ? gdk_cursor_new_from_name(display, presentation.cursor == 1 ? "text" : presentation.cursor == 7 ? "pointer"
																																	: "default")
								   : gdk_cursor_new_for_display(display, GDK_BLANK_CURSOR);
				gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
				if (cursor)
					g_object_unref(cursor);
				auto* seat = gdk_display_get_default_seat(display);
				const bool grabPointer = presentation.confine ||
										 (presentation.ownsPointer && !presentation.showCursor);
				bool grabbed = grabPointer && m_grabbedWidget == widget;
				if (grabPointer && !grabbed)
				{
					ReleasePointerGrab();
					if (GDK_IS_X11_DISPLAY(display))
					{
						auto* xdisplay = gdk_x11_display_get_xdisplay(display);
						const auto xid = gdk_x11_window_get_xid(gtk_widget_get_window(widget));
						grabbed = XGrabPointer(xdisplay, xid, True,
											   PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
											   GrabModeAsync, GrabModeAsync, xid, None, CurrentTime) == GrabSuccess;
						m_x11Grabbed = grabbed;
					}
					else
					{
						grabbed = gdk_seat_grab(seat, gtk_widget_get_window(widget),
												GDK_SEAT_CAPABILITY_POINTER, FALSE, nullptr, nullptr, nullptr, nullptr) == GDK_GRAB_SUCCESS;
						if (grabbed)
							m_grabbedSeat = seat;
					}
					if (grabbed)
						m_grabbedWidget = widget;
				}
				else if (!grabPointer && m_grabbedWidget == widget)
				{
					ReleasePointerGrab();
				}
				if (binding)
				{
					binding->captured = wantsCapture && grabbed;
					binding->rawMouseEnabled = presentation.rawMouseEnabled;
					binding->warpCapture = binding->captured && GDK_IS_X11_DISPLAY(display);
					if (binding->warpCapture && presentation.enteringCapture)
					{
						GtkAllocation allocation{};
						gtk_widget_get_allocation(widget, &allocation);
						auto* xdisplay = gdk_x11_display_get_xdisplay(display);
						const auto scale = gtk_widget_get_scale_factor(widget);
						XWarpPointer(xdisplay, None, gdk_x11_window_get_xid(gtk_widget_get_window(widget)),
									 0, 0, 0, 0, allocation.width * scale / 2, allocation.height * scale / 2);
						XFlush(xdisplay);
					}
				}
			}
			void UpdateTextInput(const NativeTextInputRequest& request) override
			{
				if (!m_textInput)
					return;
				if (!request.active)
				{
					gtk_entry_reset_im_context(GTK_ENTRY(m_textInput));
					gtk_widget_hide(m_textInput);
					m_textInputSequence = 0;
					m_textPreedit.clear();
					if (m_renderRegion)
						m_renderRegion->RequestFocus();
					return;
				}
				if (!m_renderRegion)
					return;
				const auto bounds = m_renderRegion->GetBounds();
				const auto x = std::clamp(request.caretX * bounds.width / 1280, 0,
										  std::max(0, bounds.width - 1));
				const auto y = std::clamp(request.caretY * bounds.height / 720, 0,
										  std::max(0, bounds.height - 1));
				if (m_textPreedit.empty())
				{
					gtk_widget_set_margin_start(m_textInput, x);
					gtk_widget_set_margin_top(m_textInput, y);
				}
				gtk_widget_set_size_request(m_textInput, 2,
											std::clamp(request.lineHeight * bounds.height / 720, 20, 64));
				if (m_textInputSequence != request.sequence)
				{
					m_textInputUpdating = true;
					m_textInputSequence = request.sequence;
					m_textPreedit.clear();
					gtk_entry_set_text(GTK_ENTRY(m_textInput), request.initialText.c_str());
					gtk_entry_set_max_length(GTK_ENTRY(m_textInput),
											 static_cast<gint>(std::min<std::uint32_t>(request.maximumLength, G_MAXINT)));
					gtk_editable_set_position(GTK_EDITABLE(m_textInput), -1);
					m_textInputUpdating = false;
				}
				gtk_widget_show(m_textInput);
				gtk_widget_grab_focus(m_textInput);
			}
			std::string GetKeyName(std::uint32_t key) const override
			{
				const char* name = gdk_keyval_name(key);
				return name ? name : std::string{};
			}
			std::pair<bool, std::string> GetClipboardText() override
			{
				auto* text = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
				if (!text)
					return {false, {}};
				std::string result(text);
				g_free(text);
				return {true, std::move(result)};
			}
			bool SetClipboardText(std::string text) override
			{
				auto* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
				gtk_clipboard_set_text(clipboard, text.data(), static_cast<gint>(text.size()));
				gtk_clipboard_store(clipboard);
				return true;
			}
			bool SetClipboardImage(std::span<const std::uint8_t> rgb,
								   std::int32_t width, std::int32_t height) override
			{
				if (width <= 0 || height <= 0 || rgb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3)
					return false;
				auto* image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
				if (!image)
					return false;
				const auto stride = gdk_pixbuf_get_rowstride(image);
				auto* target = gdk_pixbuf_get_pixels(image);
				for (std::int32_t row = 0; row < height; ++row)
					std::memcpy(target + row * stride, rgb.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 3,
								static_cast<std::size_t>(width) * 3);
				auto* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
				gtk_clipboard_set_image(clipboard, image);
				gtk_clipboard_store(clipboard);
				g_object_unref(image);
				return true;
			}
			bool OpenExternalUrl(std::string url) override
			{
				GError* error{};
				const bool success = gtk_show_uri_on_window(GTK_WINDOW(m_window), url.c_str(),
															GDK_CURRENT_TIME, &error) != FALSE;
				if (error)
					g_error_free(error);
				return success;
			}

			std::optional<std::string> PickTitleInstallSource() override
			{
				GtkWidget* dialog = gtk_file_chooser_dialog_new("Select title to install",
																GTK_WINDOW(m_window), GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel",
																GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_ACCEPT, nullptr);
				auto* filter = gtk_file_filter_new();
				gtk_file_filter_set_name(filter, "Wii U title metadata (meta.xml)");
				gtk_file_filter_add_pattern(filter, "meta.xml");
				gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
				std::optional<std::string> result;
				if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
				{
					char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
					if (path)
					{
						result = path;
						g_free(path);
					}
				}
				gtk_widget_destroy(dialog);
				return result;
			}

			std::optional<std::string> PickWuaDestination(
				std::string suggestedFileName) override
			{
				GtkWidget* dialog = gtk_file_chooser_dialog_new("Save Wii U game archive",
																GTK_WINDOW(m_window), GTK_FILE_CHOOSER_ACTION_SAVE, "Cancel",
																GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, nullptr);
				gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
				gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggestedFileName.c_str());
				auto* filter = gtk_file_filter_new();
				gtk_file_filter_set_name(filter, "Wii U archives (*.wua)");
				gtk_file_filter_add_pattern(filter, "*.wua");
				gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
				std::optional<std::string> result;
				if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
				{
					char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
					if (path)
					{
						result = path;
						g_free(path);
					}
				}
				gtk_widget_destroy(dialog);
				return result;
			}

		  private:
			void ReleasePointerGrab()
			{
				if (m_x11Grabbed)
				{
					auto* display = gdk_display_get_default();
					if (display && GDK_IS_X11_DISPLAY(display))
						XUngrabPointer(gdk_x11_display_get_xdisplay(display), CurrentTime);
				}
				if (m_grabbedSeat)
					gdk_seat_ungrab(m_grabbedSeat);
				m_x11Grabbed = false;
				m_grabbedSeat = nullptr;
				m_grabbedWidget = nullptr;
			}

			void NotifyMetrics()
			{
				if (m_metricsHandler && m_window)
					m_metricsHandler(GetMetrics());
			}

			GtkWidget* m_window{};
			GtkWidget* m_root{};
			GtkWidget* m_overlay{};
			GtkWidget* m_stack{};
			GtkWidget* m_webView{};
			bool m_runtimeOverlay{};
			bool m_runtimeOverlayInteractive{};
			GtkWidget* m_textInput{};
			std::unique_ptr<GtkPadRenderRegion> m_renderRegion;
			std::unique_ptr<GtkPadRenderRegion> m_padRenderRegion;
			CloseHandler m_closeHandler;
			GameCloseHandler m_gameCloseHandler;
			MetricsHandler m_metricsHandler;
			PadCloseHandler m_padCloseHandler;
			InputHandler m_inputHandler;
			std::uint64_t m_textInputSequence{};
			std::string m_textPreedit;
			bool m_textInputUpdating{};
			GdkSeat* m_grabbedSeat{};
			GtkWidget* m_grabbedWidget{};
			bool m_x11Grabbed{};
			bool m_padMetricsEnabled{};
			bool m_fullscreen{};
		};
	} // namespace

	std::unique_ptr<INativeWindowHost> CreateNativeWindowHost()
	{
		return std::make_unique<GtkWindowHost>();
	}
} // namespace WebFrontend

#endif
