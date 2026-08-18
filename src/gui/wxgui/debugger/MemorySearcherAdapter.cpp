#include "wxgui/debugger/MemorySearcherAdapter.h"

#include "wxgui/MemorySearcherTool.h"

namespace WxDebuggerAdapters
{
wxWindow* CreateMemorySearcherWindow(wxWindow& parent)
{
	return new MemorySearcherTool(&parent);
}

void CloseMemorySearcherWindow(wxWindow& window)
{
	auto& memorySearcher = static_cast<MemorySearcherTool&>(window);
	memorySearcher.PrepareForShutdown();
	memorySearcher.Close();
}
}
