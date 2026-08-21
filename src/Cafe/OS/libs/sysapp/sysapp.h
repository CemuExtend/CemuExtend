#include "Cafe/OS/RPL/COSModule.h"

#include <memory>

namespace Host
{
	class IExternalLauncher;
}

namespace sysapp
{
	COSModule* GetModule();
	void ConfigureExternalLauncher(std::shared_ptr<Host::IExternalLauncher> launcher);
} // namespace sysapp

uint64 _SYSGetSystemApplicationTitleId(sint32 index);
