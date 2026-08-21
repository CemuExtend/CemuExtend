#include "application/DiagnosticFacade.h"

#include <stdexcept>

int main()
{
	Application::DiagnosticFacade facade;
	const auto texture = facade.GetTexturePage(0, 0, 50, false, true);
	if (texture.generation == 0 || texture.rows.size() > 50 || texture.offset != 0)
		return 1;
	const auto same = facade.GetTexturePage(texture.generation, 999999, 50, false, true);
	if (same.generation != texture.generation || same.offset != same.total || !same.rows.empty())
		return 2;

	bool rejected{};
	try { (void)facade.GetTexturePage(texture.generation + 1, 0, 50, false, true); }
	catch (const std::invalid_argument&) { rejected = true; }
	if (!rejected)
		return 3;
	rejected = false;
	try { (void)facade.GetAudioVoicePage(0, 0, 0, true); }
	catch (const std::invalid_argument&) { rejected = true; }
	if (!rejected)
		return 4;
	return 0;
}
