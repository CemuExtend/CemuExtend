#include "EmulatedUSBDeviceAdapter.h"

#include "EmulatedUSBDeviceFrame.h"

namespace WxDeviceAdapters
{
	wxWindow* CreateEmulatedUSBDeviceWindow(wxWindow& parent)
	{
		return new EmulatedUSBDeviceFrame(&parent);
	}
}
