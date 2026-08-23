#include "Cafe/OS/RPL/COSModule.h"

void swkbd_render(bool mainWindow);
bool swkbd_hasKeyboardInputHook();
void swkbd_keyInput(uint32 keyCode);
std::uint64_t swkbd_overlayGeneration();

namespace swkbd
{
	COSModule* GetModule();
}
