#pragma once
#include "Cafe/OS/RPL/COSModule.h"

namespace nn
{
	namespace erreula
	{
		void render(bool mainWindow);
		bool SelectRuntimeOverlayButton(std::uint64_t generation, bool rightButton);

		COSModule* GetModule();
	} // namespace erreula
} // namespace nn
