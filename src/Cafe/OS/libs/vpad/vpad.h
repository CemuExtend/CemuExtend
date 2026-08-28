#pragma once

#include "input/emulated/ControllerStatus.h"
#include "Cafe/OS/RPL/COSModule.h"

namespace vpad
{
	void load();
	void start();
} // namespace vpad

#define VPAD_MAX_CONTROLLERS (2)

namespace vpad
{
	COSModule* GetModule();
}
