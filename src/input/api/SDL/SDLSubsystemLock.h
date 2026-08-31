#pragma once

#include <mutex>

namespace InputSdl
{
	// SDL's gamepad subsystem owns the HIDAPI layer that Wiimote discovery uses
	// directly. Bringing that subsystem up or down on the SDL event thread while
	// the Wiimote connect thread is enumerating leaves SDL's platform entry points
	// unset for a moment, and the enumeration then calls through a null pointer -
	// a segfault at startup that lands on whichever thread lost the race. Both
	// sides take this lock so the two never overlap.
	inline std::mutex& SubsystemMutex()
	{
		static std::mutex mutex;
		return mutex;
	}
} // namespace InputSdl
