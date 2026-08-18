#include "wxgui/debugger/DebuggerWindowAdapter.h"

#include "wxgui/debugger/DebuggerWindow2.h"

#include "Cafe/HW/Espresso/Debugger/GDBStub.h"

namespace
{
	DebuggerWindow2& AsDebuggerWindow(wxWindow& window)
	{
		return static_cast<DebuggerWindow2&>(window);
	}
}

namespace WxDebuggerAdapters
{
	wxWindow* CreateDebuggerWindow(wxFrame& parent, const wxRect& displaySize)
	{
		if (s_debuggerWindow)
			return nullptr;
		return new DebuggerWindow2(parent, displaySize);
	}

	void RequestCloseDebuggerWindow(wxWindow& window)
	{
		window.Close();
	}

	void DestroyDebuggerWindow(wxWindow& window)
	{
		auto& debuggerWindow = AsDebuggerWindow(window);
		debuggerWindow.CleanupForDestroy();
		if (s_debuggerWindow == &window)
			s_debuggerWindow = nullptr;
		window.Destroy();
	}

	void NotifyGameLoaded(wxWindow& window)
	{
		AsDebuggerWindow(window).OnGameLoaded();
	}

	void NotifyParentMove(wxWindow& window, const wxPoint& position, const wxSize& size)
	{
		AsDebuggerWindow(window).OnParentMove(position, size);
	}

	bool HasDebuggerWindow()
	{
		return s_debuggerWindow != nullptr;
	}

	bool IsDebuggerWindowOrChild(const wxWindow* window)
	{
		while (window)
		{
			if (window == s_debuggerWindow)
				return true;
			window = window->GetParent();
		}
		return false;
	}

	void EnsureGdbStub(std::uint16_t port)
	{
		if (!g_gdbstub)
			g_gdbstub = std::make_unique<GDBServer>(port);
	}

	void ToggleGdbStub(std::uint16_t port)
	{
		if (g_gdbstub)
		{
			// Preserve the existing process-lifetime toggle semantics.
			g_gdbstub.release();
			return;
		}
		g_gdbstub = std::make_unique<GDBServer>(port);
	}

	bool IsGdbStubEnabled()
	{
		return g_gdbstub != nullptr;
	}
}
