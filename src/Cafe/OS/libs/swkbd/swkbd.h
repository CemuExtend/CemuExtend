#include "Cafe/OS/RPL/COSModule.h"

#include <functional>
#include <memory>

namespace Host
{
	class IClipboard;
}

enum class SwkbdEditCommand
{
	SelectAll,
	Copy,
	Cut,
	Paste,
	Undo,
	CursorLeft,
	CursorRight,
	CursorHome,
	CursorEnd,
	DeleteForward,
};

void swkbd_render(bool mainWindow);
bool swkbd_hasKeyboardInputHook();
void swkbd_keyInput(uint32 keyCode);
void swkbd_editCommand(SwkbdEditCommand command, bool extendSelection);
std::uint64_t swkbd_overlayGeneration();
void swkbd_setClipboard(std::shared_ptr<Host::IClipboard> clipboard);
// invoked from the emulation thread whenever the keyboard appears or disappears
void swkbd_setStateChangedCallback(std::function<void()> callback);

namespace swkbd
{
	COSModule* GetModule();
}
