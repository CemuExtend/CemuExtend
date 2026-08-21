#include "webview/NativeFileDialog.h"

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>

namespace WebFrontend
{
	std::optional<std::filesystem::path> SelectArchiveToOpen(void* owner, std::string_view)
	{
		NSOpenPanel* panel = [NSOpenPanel openPanel];
		[panel setAllowedFileTypes:@[@"zip"]]; [panel setAllowsMultipleSelection:NO];
		if ([panel runModal] != NSModalResponseOK) return {};
		return std::filesystem::path([[[panel URL] path] UTF8String]);
	}
	std::optional<std::filesystem::path> SelectArchiveToSave(void* owner, std::string_view,
		std::string_view suggested)
	{
		NSSavePanel* panel = [NSSavePanel savePanel]; [panel setAllowedFileTypes:@[@"zip"]];
		[panel setNameFieldStringValue:[NSString stringWithUTF8String:std::string(suggested).c_str()]];
		if ([panel runModal] != NSModalResponseOK) return {};
		return std::filesystem::path([[[panel URL] path] UTF8String]);
	}
}
#endif
