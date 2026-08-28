#pragma once

#include <memory>

namespace Host
{
	class IClipboard;
	class IExternalLauncher;
	class IInputFocus;
	class ICanvasHost;
	class IWindowMetrics;
} // namespace Host

namespace cemuextend_hle
{
	class ICemodWebUiHost;
}

namespace Application
{
	struct HostBindings
	{
		std::shared_ptr<Host::IWindowMetrics> windowMetrics;
		std::shared_ptr<Host::IClipboard> clipboard;
		std::shared_ptr<Host::IExternalLauncher> externalLauncher;
		std::shared_ptr<Host::IInputFocus> inputFocus;
		std::shared_ptr<Host::ICanvasHost> canvas;
		std::shared_ptr<cemuextend_hle::ICemodWebUiHost> cemodWebUi;
	};

	// Composition-root wiring for legacy Cafe modules whose guest ABI entrypoints
	// cannot use constructor injection. Each module receives only its narrow port.
	void ConnectHost(const HostBindings& bindings);
	void DisconnectHost();
} // namespace Application
