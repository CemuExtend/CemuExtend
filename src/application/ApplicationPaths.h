#pragma once

namespace Application
{
	// Initializes ActiveSettings for frontends which do not use wxStandardPaths.
	// This must run before CemuCommonInit loads settings.xml.
	void InitializePaths();
} // namespace Application
