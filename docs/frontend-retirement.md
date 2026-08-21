# wxWidgets frontend retirement

The native desktop application is now the React/WebView frontend. CMake accepts
only `CEMU_FRONTEND=webview` (the default) and `CEMU_FRONTEND=headless`.

The retired wxWidgets frontend is not configured, compiled, or linked. Its
vcpkg feature, overlay port, CMake finder, target graph, self-containment tests,
and translation keywords have been removed. `frontend_retirement_guards`
checks the active build configuration so the dependency cannot return through
an accidental target or manifest edit.

Frontend-neutral settings and runtime services live under `src/config`,
`src/application`, `src/frontend`, and `src/host`. In particular, legacy wx
configuration keys are still imported by `CemuConfig` for user migration, while
new values are persisted in the neutral `Frontend` section. The update worker
mailbox and its concurrency regression test also live in the Application layer;
the retained legacy include only forwards to that neutral contract. Headless
startup continues to use the same core and application services without
creating a native UI.

The legacy `src/gui/wxgui` source tree has been removed after confirming that
the parallel PPC debugger and CemuMod launch/permission migrations no longer
modify it. Compatibility behavior now lives behind the application/domain and
web role contracts instead of retaining an unbuildable UI snapshot.
