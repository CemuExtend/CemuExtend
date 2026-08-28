#include "webview/cef/CefOverlayRuntime.h"

int main(int argc, char* argv[])
{
	return WebFrontend::CefOverlay::ExecuteHelperSubprocess(argc, argv);
}
