#pragma once

#include "input/emulated/ControllerStatus.h"
#include "Cafe/OS/RPL/COSModule.h"

namespace padscore
{
	void start();
	COSModule* GetModule();
} // namespace padscore

constexpr int kWPADMaxControllers = 4;
constexpr int kKPADMaxControllers = 7;

#define WPAD_ERR_NONE 0
#define WPAD_ERR_NO_CONTROLLER -1
#define WPAD_ERR_BUSY -2
#define WPAD_ERR_INVALID -4

#define WPAD_FMT_CORE 0
#define WPAD_FMT_CORE_ACC 1
#define WPAD_FMT_CORE_ACC_DPD 2
