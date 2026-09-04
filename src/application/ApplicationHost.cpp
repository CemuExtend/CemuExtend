#include "Common/precompiled.h"
#include "application/ApplicationHost.h"

#include "Cafe/OS/libs/cemuextend/Cex2Host.h"
#include "Cafe/OS/libs/cemuextend/Cex2Microphone.h"
#include "Cafe/OS/libs/sysapp/sysapp.h"
#include "Cafe/OS/libs/HostInputFocus.h"
#include "Cafe/HW/Espresso/TcpGecko/TcpGeckoServer.h"
#include "Cafe/CafeSystem.h"

namespace Application
{
	void ConnectHost(const HostBindings& bindings)
	{
		cemuextend_hle::Cex2Host::Instance().ConfigureHost(
			bindings.clipboard, bindings.windowMetrics, bindings.cemodWebUi,
			bindings.windowControl);
		sysapp::ConfigureExternalLauncher(bindings.externalLauncher);
		CafeHost::ConfigureInputFocus(bindings.inputFocus);
		CafeSystem::ConfigureCanvasHost(bindings.canvas);
	}

	void DisconnectHost()
	{
		CafeSystem::ConfigureCanvasHost({});
		CafeHost::ConfigureInputFocus({});
		sysapp::ConfigureExternalLauncher({});
		cemuextend_hle::Cex2Host::Instance().ConfigureHost({}, {}, {}, {});
	}

	void SetCemuExtendFocusPaused(bool paused, std::uint64_t sequence)
	{
		TcpGecko::SetGuestPaused(TcpGecko::PauseOwner::CemuExtendFocusLoss, paused, sequence);
	}

	void ReleaseCemuExtendMicrophones()
	{
		cemuextend_hle::Cex2Microphone::ReleaseAll();
	}
} // namespace Application
