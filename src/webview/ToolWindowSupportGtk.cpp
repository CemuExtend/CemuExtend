#include "webview/ToolWindowSupport.h"

#if defined(__linux__)

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include "host/contracts/HostContracts.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace WebFrontend
{
	namespace
	{
		class GtkToolWindowSupport final : public IToolWindowSupport
		{
		  public:
			GtkToolWindowSupport(GtkWidget* parent, bool modal,
								 std::function<void()> closeHandler)
				: m_window(gtk_window_new(GTK_WINDOW_TOPLEVEL)),
				  m_closeHandler(std::move(closeHandler))
			{
				if (!GTK_IS_WINDOW(m_window) || !GTK_IS_WINDOW(parent))
					throw std::invalid_argument("parent must be a GTK window");
				gtk_window_set_transient_for(GTK_WINDOW(m_window), GTK_WINDOW(parent));
				gtk_window_set_modal(GTK_WINDOW(m_window), modal ? TRUE : FALSE);
				gtk_window_set_default_size(GTK_WINDOW(m_window), 800, 600);
				m_deleteHandler = g_signal_connect(m_window, "delete-event",
												   G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
													   auto& self = *static_cast<GtkToolWindowSupport*>(data);
													   if (self.m_closeHandler)
														   self.m_closeHandler();
													   return TRUE;
												   }),
												   this);
			}

			~GtkToolWindowSupport() override
			{
				if (m_window && GTK_IS_WIDGET(m_window) && m_deleteHandler)
					g_signal_handler_disconnect(m_window, m_deleteHandler);
				if (m_window && GTK_IS_WIDGET(m_window))
					gtk_widget_destroy(m_window);
			}

			void* GetWindow() const override
			{
				return m_window;
			}

			void SetSize(std::int32_t width, std::int32_t height) override
			{
				if (!m_window)
					return;
				gtk_window_set_default_size(GTK_WINDOW(m_window), std::max(1, width),
									std::max(1, height));
				if (gtk_widget_get_visible(m_window))
					gtk_window_resize(GTK_WINDOW(m_window), std::max(1, width),
								  std::max(1, height));
			}

			void SetTitle(std::string_view title) override
			{
				if (!m_window)
					return;
				const std::string ownedTitle(title);
				gtk_window_set_title(GTK_WINDOW(m_window), ownedTitle.c_str());
			}

			void* GetBrowserParentWindow() const override
			{
				auto& self = *const_cast<GtkToolWindowSupport*>(this);
				self.EnsureBrowserContainer();
				auto* display = self.BrowserXDisplay();
				if (!display)
					throw std::runtime_error(
						"CEF native browser requires an X11 or XWayland GTK session");
				// Let Chromium create the child with the root window's default visual.
				// AttachBrowser() reparents it into the GTK drawing area as soon as CEF
				// reports OnAfterCreated.
				return reinterpret_cast<void*>(
					static_cast<std::uintptr_t>(DefaultRootWindow(display)));
			}

			Host::RenderRegionBounds GetBrowserBounds() const override
			{
				if (!m_browserContainer)
					return {};
				GtkAllocation allocation{};
				gtk_widget_get_allocation(m_browserContainer, &allocation);
				if (auto* window = gtk_widget_get_window(m_browserContainer);
					window && GDK_IS_X11_WINDOW(window))
				{
					XWindowAttributes attributes{};
					auto* display = gdk_x11_display_get_xdisplay(
						gdk_window_get_display(window));
					if (XGetWindowAttributes(display, gdk_x11_window_get_xid(window),
									 &attributes))
						return {0, 0, std::max(1, attributes.width),
								std::max(1, attributes.height)};
				}
				return {0, 0, std::max(1, allocation.width),
						std::max(1, allocation.height)};
			}

			double GetBrowserDpiScale() const override
			{
				return m_browserContainer
						   ? static_cast<double>(gtk_widget_get_scale_factor(m_browserContainer))
						   : 1.0;
			}

			void AttachBrowser(void* widget) override
			{
				EnsureBrowserContainer();
				if (!widget)
					throw std::invalid_argument("CEF browser window must not be null");
				const auto child = static_cast<::Window>(
					reinterpret_cast<std::uintptr_t>(widget));
				auto* display = BrowserXDisplay();
				const auto parent = BrowserParentXWindow();
				if (!display || !parent)
					throw std::runtime_error("CEF browser parent is not a valid X11 window");

				auto* gdkDisplay = gtk_widget_get_display(m_browserContainer);
				gdk_x11_display_error_trap_push(gdkDisplay);
				::Window root{}, currentParent{}, *children{};
				unsigned childCount{};
				const bool queried = XQueryTree(display, child, &root, &currentParent,
										 &children, &childCount) != 0;
				if (children)
					XFree(children);
				if (queried)
				{
					if (currentParent != parent)
						XReparentWindow(display, child, parent, 0, 0);
					const auto bounds = GetBrowserBounds();
					XMoveResizeWindow(display, child, 0, 0,
						static_cast<unsigned>(std::max(1, bounds.width)),
						static_cast<unsigned>(std::max(1, bounds.height)));
					XMapWindow(display, child);
				}
				XSync(display, False);
				const auto xError = gdk_x11_display_error_trap_pop(gdkDisplay);
				if (!queried || xError)
					throw std::invalid_argument("CEF browser window is not a valid X11 child");
				m_browserChild = child;
			}

			void ResizeBrowser() override
			{
				if (!m_browserChild)
					return;
				auto* display = BrowserXDisplay();
				if (!display)
					return;
				const auto bounds = GetBrowserBounds();
				auto* gdkDisplay = gtk_widget_get_display(m_browserContainer);
				gdk_x11_display_error_trap_push(gdkDisplay);
				XMoveResizeWindow(display, m_browserChild, 0, 0,
							  static_cast<unsigned>(std::max(1, bounds.width)),
							  static_cast<unsigned>(std::max(1, bounds.height)));
				XSync(display, False);
				if (gdk_x11_display_error_trap_pop(gdkDisplay))
					m_browserChild = None;
			}

			void FocusBrowser() override
			{
				if (m_browserContainer)
					gtk_widget_grab_focus(m_browserContainer);
				if (m_browserChild)
				{
					if (auto* display = BrowserXDisplay())
					{
						auto* gdkDisplay = gtk_widget_get_display(m_browserContainer);
						gdk_x11_display_error_trap_push(gdkDisplay);
						XRaiseWindow(display, m_browserChild);
						XSetInputFocus(display, m_browserChild, RevertToParent, CurrentTime);
						XSync(display, False);
						if (gdk_x11_display_error_trap_pop(gdkDisplay))
							m_browserChild = None;
					}
				}
			}

			void DetachBrowser(void* widget) override
			{
				const auto child = static_cast<::Window>(
					reinterpret_cast<std::uintptr_t>(widget));
				if (!widget || child == m_browserChild)
					m_browserChild = None;
			}
			void Show() override
			{
				gtk_widget_show_all(m_window);
				Focus();
			}

			void Focus() override
			{
				if (m_window && GTK_IS_WINDOW(m_window))
				{
					gtk_window_present(GTK_WINDOW(m_window));
					gtk_window_set_urgency_hint(GTK_WINDOW(m_window), FALSE);
					if (m_browserChild)
						FocusBrowser();
				}
			}

			std::optional<std::filesystem::path> PickDirectory(std::string_view title) override
			{
				const std::string ownedTitle(title);
				GtkWidget* dialog = gtk_file_chooser_dialog_new(ownedTitle.c_str(),
																GTK_WINDOW(m_window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
																"_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, nullptr);
				if (!dialog)
					throw std::runtime_error("failed to create the folder picker");
				std::optional<std::filesystem::path> result;
				if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
				{
					gchar* selected = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
					if (selected)
					{
						result = std::filesystem::path(selected);
						g_free(selected);
					}
				}
				gtk_widget_destroy(dialog);
				return result;
			}

		  private:
			void EnsureBrowserContainer()
			{
				if (m_browserContainer)
					return;
				if (gtk_bin_get_child(GTK_BIN(m_window)))
					throw std::logic_error(
						"an unexpected child is already attached to the CEF tool window");
				m_browserContainer = gtk_drawing_area_new();
				gtk_widget_set_hexpand(m_browserContainer, TRUE);
				gtk_widget_set_vexpand(m_browserContainer, TRUE);
				gtk_widget_set_can_focus(m_browserContainer, TRUE);
				gtk_widget_add_events(m_browserContainer, GDK_FOCUS_CHANGE_MASK);
				gtk_container_add(GTK_CONTAINER(m_window), m_browserContainer);
				g_signal_connect(m_browserContainer, "size-allocate",
					G_CALLBACK(+[](GtkWidget*, GtkAllocation*, gpointer data) {
						static_cast<GtkToolWindowSupport*>(data)->ResizeBrowser();
					}), this);
				g_signal_connect(m_browserContainer, "focus-in-event",
					G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
						static_cast<GtkToolWindowSupport*>(data)->FocusBrowser();
						return FALSE;
					}), this);
				gtk_widget_realize(m_window);
				gtk_widget_realize(m_browserContainer);
			}

			Display* BrowserXDisplay() const
			{
				if (!m_browserContainer)
					return nullptr;
				auto* window = gtk_widget_get_window(m_browserContainer);
				if (!window || !GDK_IS_X11_WINDOW(window))
					return nullptr;
				return gdk_x11_display_get_xdisplay(gdk_window_get_display(window));
			}

			::Window BrowserParentXWindow() const
			{
				if (!m_browserContainer)
					return None;
				auto* window = gtk_widget_get_window(m_browserContainer);
				return window && GDK_IS_X11_WINDOW(window)
						   ? gdk_x11_window_get_xid(window)
						   : None;
			}

			GtkWidget* m_window{};
			GtkWidget* m_browserContainer{};
			::Window m_browserChild{None};
			std::function<void()> m_closeHandler;
			gulong m_deleteHandler{};
		};
	} // namespace

	std::unique_ptr<IToolWindowSupport> CreateToolWindowSupport(
		void* parent, bool modal, std::function<void()> closeHandler)
	{
		return std::make_unique<GtkToolWindowSupport>(GTK_WIDGET(parent), modal,
													  std::move(closeHandler));
	}
} // namespace WebFrontend

#endif
