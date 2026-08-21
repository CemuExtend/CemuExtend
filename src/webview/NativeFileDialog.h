#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace WebFrontend
{
	// Native-only boundary used by web roles. Paths selected here are retained by
	// the host and are never accepted from JavaScript.
	[[nodiscard]] std::optional<std::filesystem::path> SelectArchiveToOpen(
		void* ownerWindow, std::string_view title);
	[[nodiscard]] std::optional<std::filesystem::path> SelectArchiveToSave(
		void* ownerWindow, std::string_view title, std::string_view suggestedName);
}
