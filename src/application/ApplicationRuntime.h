#pragma once

// Process-wide initialization shared by the graphical, headless and LLE
// frontends. The Application target owns this orchestration so executable
// entry points do not provide hidden symbols required by static libraries.
void CemuCommonInit();
void HandlePostUpdate();
