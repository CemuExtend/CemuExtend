#include "application/EmulationController.h"
#include "application/ApplicationRuntime.h"
#include "application/ApplicationPaths.h"
#include "frontend/FrontendRuntime.h"
#include "input/InputManager.h"

void Frontend::Run()
{
	Application::InitializePaths();
	// Headless composition root. Construct the Application boundary first so
	// Cafe initialization has an event sink and injected input context.
	Application::EmulationController emulation;
	CemuCommonInit();
	uint32 shutdownAttempts = 0;
	for (;;)
	{
		const auto shutdown = emulation.ShutdownApplication();
		if (shutdown.stopped)
			break;
		if ((++shutdownAttempts % 100) == 1)
			cemuLog_log(LogType::Force,
				"Headless shutdown retained resources; retrying: {}", shutdown.diagnostic);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	InputManager::instance().Shutdown();
}
