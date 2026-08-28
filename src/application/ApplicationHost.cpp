#include "Common/precompiled.h"
#include "application/ApplicationHost.h"

#include "Cafe/OS/libs/cemuextend/Cex2Host.h"
#include "Cafe/OS/libs/sysapp/sysapp.h"
#include "Cafe/OS/libs/HostInputFocus.h"
#include "Cafe/CafeSystem.h"

namespace Application
{
	void ConnectHost(const HostBindings& bindings)
	{
		cemuextend_hle::Cex2Host::Instance().ConfigureHost(
			bindings.clipboard, bindings.windowMetrics, bindings.cemodWebUi);
		sysapp::ConfigureExternalLauncher(bindings.externalLauncher);
		CafeHost::ConfigureInputFocus(bindings.inputFocus);
		CafeSystem::ConfigureCanvasHost(bindings.canvas);
	}

	void DisconnectHost()
	{
		CafeSystem::ConfigureCanvasHost({});
		CafeHost::ConfigureInputFocus({});
		sysapp::ConfigureExternalLauncher({});
		cemuextend_hle::Cex2Host::Instance().ConfigureHost({}, {}, {});
	}
} // namespace Application
