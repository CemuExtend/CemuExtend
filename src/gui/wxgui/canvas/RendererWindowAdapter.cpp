#include "wxgui/canvas/RendererWindowAdapter.h"

#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VsyncDriver.h"
#endif

namespace WxRendererAdapters
{
	void NotifyWindowPositionChanged()
	{
#ifdef ENABLE_VULKAN
		VsyncDriver_notifyWindowPosChanged();
#endif
	}
}
