#pragma once
#include "Cafe/OS/RPL/COSModule.h"
namespace nn::fp
{
	COSModule* GetModule();
	// Called after IOSU FPD producers and all title PPC threads have stopped.
	void ResetForTitleShutdown();
} // namespace nn::fp
