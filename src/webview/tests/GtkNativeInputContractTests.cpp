#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

int main()
{
	std::ifstream input(std::string(CEMU_SOURCE_DIR) +
						"/src/webview/NativeWindowHostGtk.cpp");
	const std::string source{std::istreambuf_iterator<char>{input},
						 std::istreambuf_iterator<char>{}};
	assert(!source.empty());

	// X11 detectable repeat suppresses the synthetic release half of an
	// autorepeat pair. The old GDK look-ahead remains only as a fallback for
	// servers which do not support the XKB option.
	assert(source.find("XkbSetDetectableAutoRepeat") != std::string::npos);
	assert(source.find("if (!binding->detectableAutoRepeat)") != std::string::npos);
	assert(source.find("g_idle_add_full") != std::string::npos);
	assert(source.find("focusGeneration") != std::string::npos);
	assert(source.find("gtk_widget_in_destruction") != std::string::npos);

	// Captured gameplay uses unaccelerated XInput2 device counts. GDK motion and
	// cursor warping remain a fallback only, otherwise an 8 kHz mouse is
	// accelerated, coalesced, and periodically produces a large center delta.
	assert(source.find("XI_RawMotion") != std::string::npos);
	assert(source.find("XISelectEvents") != std::string::npos);
	assert(source.find("raw->raw_values") != std::string::npos);
	assert(source.find("RawMouse().IsActive(binding)") != std::string::npos);
	assert(source.find("&& !usesRawMouse") != std::string::npos);

	// Losing application focus must publish metrics and release held inputs.
	// Reclaiming X focus from the focus-out callback creates a focus tug-of-war
	// whose transient FocusLost events repeatedly interrupt held gameplay keys.
	const auto focusOut = source.find(
		"g_signal_connect(m_window, \"focus-out-event\"");
	assert(focusOut != std::string::npos);
	const auto focusOutEnd = source.find("this);", focusOut);
	assert(focusOutEnd != std::string::npos);
	assert(source.substr(focusOut, focusOutEnd - focusOut)
			   .find("ClaimInputFocus") == std::string::npos);

	std::ifstream frontendInput(std::string(CEMU_SOURCE_DIR) +
								"/src/webview/WebFrontend.cpp");
	const std::string frontend{std::istreambuf_iterator<char>{frontendInput},
						   std::istreambuf_iterator<char>{}};
	const auto metricsHandler = frontend.find(
		"void HandleMetrics(Host::WindowMetricsSnapshot metrics)");
	assert(metricsHandler != std::string::npos);
	const auto effectiveFocus = frontend.find("EffectivePointerFocus", metricsHandler);
	const auto metricsUpdate = frontend.find("m_hostState->UpdateMetrics(metrics)", metricsHandler);
	assert(effectiveFocus != std::string::npos && metricsUpdate != std::string::npos);
	assert(effectiveFocus < metricsUpdate);
	const auto focusGain = frontend.find("void ConfirmNativeKeyboardFocus()", metricsHandler);
	const auto focusLoss = frontend.find("void ConfirmNativeFocusLoss()", metricsHandler);
	assert(focusGain != std::string::npos && focusLoss != std::string::npos);
	const auto gainMetrics = frontend.find("m_hostState->UpdateMetrics(metrics)", focusGain);
	const auto gainPointer = frontend.find("m_controller.PointerFocusChanged(true)", focusGain);
	assert(gainMetrics != std::string::npos && gainPointer != std::string::npos);
	assert(gainMetrics < gainPointer);
	assert(frontend.substr(focusGain, gainPointer - focusGain)
			   .find("SetWindowFocus") == std::string::npos);
	const auto keyUsesGain = frontend.find("ConfirmNativeKeyboardFocus();", gainPointer);
	assert(keyUsesGain != std::string::npos);
}
