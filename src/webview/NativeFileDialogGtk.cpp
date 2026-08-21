#include "webview/NativeFileDialog.h"

#if defined(__linux__)
#include <gtk/gtk.h>

namespace WebFrontend
{
	namespace
	{
		std::optional<std::filesystem::path> Select(void* owner, std::string_view title,
			GtkFileChooserAction action, std::string_view suggested)
		{
			auto* dialog = gtk_file_chooser_dialog_new(std::string(title).c_str(),
				GTK_WINDOW(owner), action, "_Cancel", GTK_RESPONSE_CANCEL,
				action == GTK_FILE_CHOOSER_ACTION_OPEN ? "_Open" : "_Save",
				GTK_RESPONSE_ACCEPT, nullptr);
			if (action == GTK_FILE_CHOOSER_ACTION_SAVE)
			{
				gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
				gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog),
					std::string(suggested).c_str());
			}
			auto* filter = gtk_file_filter_new();
			gtk_file_filter_set_name(filter, "ZIP save archives (*.zip)");
			gtk_file_filter_add_pattern(filter, "*.zip");
			gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
			std::optional<std::filesystem::path> result;
			if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
			{
				if (gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)))
				{
					result = std::filesystem::path(filename);
					g_free(filename);
				}
			}
			gtk_widget_destroy(dialog);
			return result;
		}
	}

	std::optional<std::filesystem::path> SelectArchiveToOpen(void* owner,
		std::string_view title) { return Select(owner, title, GTK_FILE_CHOOSER_ACTION_OPEN, {}); }
	std::optional<std::filesystem::path> SelectArchiveToSave(void* owner,
		std::string_view title, std::string_view suggestedName)
	{
		return Select(owner, title, GTK_FILE_CHOOSER_ACTION_SAVE, suggestedName);
	}
}
#endif
