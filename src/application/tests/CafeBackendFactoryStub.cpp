#include "application/EmulationController.h"

namespace Application
{
	std::unique_ptr<IEmulationBackend> CreateCafeEmulationBackend(ApplicationEvents&)
	{
		return {};
	}
}
