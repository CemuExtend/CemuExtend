#include "webview/NativeFileDialog.h"

#if defined(_WIN32)
#include <commdlg.h>
#include <windows.h>

namespace WebFrontend
{
	namespace
	{
		std::wstring Utf16(std::string_view value)
		{
			if (value.empty())
				return {};
			const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
												  value.data(), static_cast<int>(value.size()), nullptr, 0);
			if (size <= 0)
				return {};
			std::wstring result(static_cast<std::size_t>(size), L'\0');
			if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
									static_cast<int>(value.size()), result.data(), size) != size)
				return {};
			return result;
		}

		std::optional<std::filesystem::path> Select(void* owner, bool save,
													std::string_view title, std::string_view suggested)
		{
			std::wstring buffer(32768, L'\0');
			if (save)
			{
				const auto name = Utf16(suggested);
				if (name.size() >= buffer.size())
					return {};
				std::copy(name.begin(), name.end(), buffer.begin());
			}
			const auto dialogTitle = Utf16(title);
			OPENFILENAMEW request{};
			request.lStructSize = sizeof(request);
			request.hwndOwner = static_cast<HWND>(owner);
			request.lpstrFile = buffer.data();
			request.nMaxFile = static_cast<DWORD>(buffer.size());
			request.lpstrTitle = dialogTitle.empty() ? nullptr : dialogTitle.c_str();
			request.lpstrFilter = L"ZIP save archives (*.zip)\0*.zip\0\0";
			request.lpstrDefExt = L"zip";
			request.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
			if (!(save ? GetSaveFileNameW(&request) : GetOpenFileNameW(&request)))
				return {};
			return std::filesystem::path(buffer.c_str());
		}
	} // namespace
	std::optional<std::filesystem::path> SelectArchiveToOpen(void* owner, std::string_view title)
	{
		return Select(owner, false, title, {});
	}
	std::optional<std::filesystem::path> SelectArchiveToSave(void* owner, std::string_view title,
															 std::string_view suggested)
	{
		return Select(owner, true, title, suggested);
	}
} // namespace WebFrontend
#endif
