#pragma once

namespace Frontend
{
	// Platform composition-root entry point. The selected frontend owns host-port
	// installation, its event loop, and the matching teardown sequence.
	void Run();
} // namespace Frontend
