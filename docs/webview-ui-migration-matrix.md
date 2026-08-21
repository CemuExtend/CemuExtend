# WebView UI migration matrix

This matrix records the observed wxWidgets UI ownership and the destination contract. Status is intentionally conservative: an item is only `Completed` after its native behavior and tests have moved, not when a route exists.

| Legacy wx source | Screen / behavior | Application or domain destination | React window role | Kind | Native responsibility | Test | Status |
|---|---|---|---|---|---|---|---|
| `CemuApp.*`, `WxFrontendRuntime.cpp` | process/UI lifecycle and host wiring | `ApplicationRuntime`, `ApplicationHost` | main-library | top-level | UI thread, shutdown ordering | Docker build/CTest | In progress |
| `MainWindow.*` | main menu, library/render switching, launch | `EmulationController`, `MainWindowState` | main-library | top-level | native menu/content host/render surface | state transition tests | In progress |
| `components/wxGameList.*` | searchable game library and launch | `TitleCatalog` | main-library | modeless | title drop and native context menu | React tests | In progress |
| `GeneralSettings2.*` | general/graphics/audio/network/account settings | settings façade required | general-settings | modal | native dialogs/surface refresh | required | Not started |
| `GameProfileWindow.*` | per-title profile | `GameProfileFacade` | general-settings | modal | none | façade tests required | Not started |
| `input/InputSettings2.*`, controller panels | input profiles and binding capture | input façade required | input-settings | modal | native capture/input devices | required | Not started |
| `input/HotkeySettings.*` | hotkey profiles | input façade required | hotkey-settings | modeless singleton | native key capture | required | Not started |
| `input/InputAPIAddWindow.*`, `PairingDialog.*` | add/pair input device | input façade required | input-settings | modal | Bluetooth/native capture | required | Not started |
| `GraphicPacksWindow2.*` | graphic pack management | `GraphicPackFacade` | graphic-packs | modeless singleton | native folder dialog | required | Not started |
| `DownloadGraphicPacksWindow.*`, `DownloadCustomGraphicPackWindow.*` | pack download/install | graphic pack operation service required | graphic-packs | modal operation | file dialog/progress worker | required | Not started |
| `CemodPluginManagerDialog.*`, `CemodManagementModel.*` | `.cemod` catalog and enablement | `EmulationController` cemod façade | cemod-manager | modeless singleton | package selection | existing model tests + RPC tests | Not started |
| `CemodPermissionDialog.*` | launch permission decision | permission transaction service required | cemod-permissions | modal | parent disable/transaction token | required | Not started |
| `TitleManager.*`, `components/wxTitleManagerList.*` | installed content management | `TitleCatalog`, `TitleInstallFacade` | title-manager | modeless singleton | native install dialog | required | Not started |
| `dialogs/CreateAccount/*` | account creation/edit | `AccountFacade` | account-manager | modal child | none | required | Not started |
| `dialogs/SaveImport/*` | save import/transfer | `SaveFacade` | save-manager | modal | open/save dialogs | required | Not started |
| `CemuUpdateWindow.*`, `GameUpdateWindow.*`, `DownloadGraphicPacksWindow.*` | title/community-pack update | `TitleInstallFacade`, `GraphicPackFacade` | update-manager | modal | native source picker; opaque plan and install transactions | plan ownership + React event + Docker build/CTest | Completed |
| `ChecksumTool.*` | content checksum | `ContentOperations` | checksum-tool | modeless singleton | file/save dialog | required | Not started |
| `LoggingWindow.*`, `components/wxLogCtrl.*` | streaming/exporting logs | logging façade required | logging | modeless singleton | save dialog | required | Not started |
| `GettingStartedDialog.*` | first-run configuration | settings façade required | getting-started | modal | folder dialog | required | Not started |
| `PadViewFrame.*` | independent GamePad render/input window | host render/input contracts | n/a | native tool window | render surface, DPI, mouse/touch | Docker build/CTest + lifecycle review | Completed |
| `EmulatedUSBDevices/*` | emulated USB device management | device façade required | emulated-usb-devices | modeless singleton | device enumeration | required | Not started |
| `MemorySearcherTool.*` | memory range search | debugger façade required | memory-searcher | modeless singleton | none | required | Not started |
| `debugger/DebuggerWindow2.*` and controls | PPC debugger/disassembly/registers | debugger façade required | ppc-debugger | modeless singleton | none | required | Not started |
| `AudioDebuggerWindow.*` | audio debugger | debugger façade required | audio-debugger | modeless singleton | none | required | Not started |
| `windows/TextureRelationViewer/*` | texture relations | debugger façade required | texture-relations | modeless singleton | none | required | Not started |
| `windows/PPCThreadsViewer/*` | PPC thread viewer | debugger façade required | ppc-threads | modeless singleton | none | required | Not started |
| `DownloadManager` menu/panel | download queue | operation façade required | update-manager | modeless | notifications | required | Not started |
| About block in `MainWindow.cpp` | build/license information | build-info façade required | about | modal | external link launch | required | Not started |
| `canvas/*Canvas.*`, `RendererWindowAdapter.*` | Vulkan/OpenGL/Metal host surfaces | frontend-neutral render/input/IME host contracts | n/a | native child surface | GPU/native handle, raw mouse, touch, IME | Docker build/CTest + bridge tests | In progress |
| `wxCemuConfig.*`, `WxWindowState.*` | config/window persistence | application settings + host window state | n/a | native | geometry/DPI/fullscreen | required | Not started |

The current branch establishes the selectable frontend, pinned webview dependency, embedded React library, typed RPC dispatcher, ordered application-event bridge, persistent native main/render hosts, synchronized GamePad hosting, and native keyboard/mouse/touch/IME services. Rows remain conservative where Windows/macOS runtime validation or the corresponding React workflow is still outstanding.
