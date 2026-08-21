#include "webview/ToolWindowSupport.h"

#if defined(__linux__)

#include <gtk/gtk.h>

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
				m_deleteHandler = g_signal_connect(m_window, "delete-event",
					G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
						auto& self = *static_cast<GtkToolWindowSupport*>(data);
						if (self.m_closeHandler) self.m_closeHandler();
						return TRUE;
					}), this);
			}

			~GtkToolWindowSupport() override
			{
				if (m_window && GTK_IS_WIDGET(m_window) && m_deleteHandler)
					g_signal_handler_disconnect(m_window, m_deleteHandler);
				if (m_window && GTK_IS_WIDGET(m_window)) gtk_widget_destroy(m_window);
			}

			void* GetWindow() const override { return m_window; }
			void Show() override { gtk_widget_show_all(m_window); Focus(); }

			void Focus() override
			{
				if (m_window && GTK_IS_WINDOW(m_window))
				{
					gtk_window_present(GTK_WINDOW(m_window));
					gtk_window_set_urgency_hint(GTK_WINDOW(m_window), FALSE);
				}
			}

			std::optional<std::filesystem::path> PickDirectory(std::string_view title) override
			{
				const std::string ownedTitle(title);
				GtkWidget* dialog = gtk_file_chooser_dialog_new(ownedTitle.c_str(),
					GTK_WINDOW(m_window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
					"_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, nullptr);
				if (!dialog) throw std::runtime_error("failed to create the folder picker");
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
			GtkWidget* m_window{};
			std::function<void()> m_closeHandler;
			gulong m_deleteHandler{};
		};
	}

	std::unique_ptr<IToolWindowSupport> CreateToolWindowSupport(
		void* parent, bool modal, std::function<void()> closeHandler)
	{
		return std::make_unique<GtkToolWindowSupport>(GTK_WIDGET(parent), modal,
			std::move(closeHandler));
	}
}

#endif
