#pragma once

class wxWindow;

namespace WxDebuggerAdapters
{
	wxWindow* CreateMemorySearcherWindow(wxWindow& parent);
	void CloseMemorySearcherWindow(wxWindow& window);
} // namespace WxDebuggerAdapters
