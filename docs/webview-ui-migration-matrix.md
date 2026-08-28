# CemuExtend WebView UI matrix

This document replaces the legacy wx-to-WebView progress list. It records the
approved 81-screen reference UI, the React/native ownership boundary, and the stable
visual-preview route used for QA.

## Visual source of truth

- Reference · the supplied reference image directory
- Canvas · 1440 × 900, DPR 1, browser zoom 100 %
- Theme · dark desktop, defined in `ui/tokens.css`
- Rules · `ui/design.md`
- Preview · `?preview=1&screen=<reference slug>`

The reference PNG wins when generated HTML and the image differ. In particular,
`01_main_library_grid.png` is a preserved master image and has no HTML source.

## Ownership rules

| Layer              | Owns                                                                                 |
| ------------------ | ------------------------------------------------------------------------------------ |
| React              | layout, controls, keyboard focus, presentation state, responsive fallback            |
| Application façade | copied models, validation, operation plans, generations                              |
| WebView host       | main-window lifetime, approval-modal lifetime, DPI, file dialogs, render composition |
| Emulation runtime  | launch/shutdown, render surfaces, input capture, debugger memory                     |

React must never invent successful native work. Preview data is fixed and
labelled by the `preview=1` route; production screens always use RPC data.

## Screen matrix

| Screens | Family                         | React destination                                    | Preview routing                  | Native boundary                |
| ------- | ------------------------------ | ---------------------------------------------------- | -------------------------------- | ------------------------------ |
| 00–03   | Library list/grid/empty/filter | `Library`                                            | main-library + fixture state     | `TitleCatalog`, launch façade  |
| 04–09   | Game workspace tabs            | `GamePage`                                           | main-library + game/tab state    | launch + embedded managers     |
| 10–15   | Main direct tool workspaces    | `AppShell` / `EmbeddedToolWorkspace`                 | route selected from screen index | native façades in main WebView |
| 16      | Job centre                     | `AppShell` popover                                   | fixed three-job preview          | operation façades/events       |
| 17–18   | Getting Started                | `GettingStartedWindow`                               | getting-started                  | settings + folder dialog       |
| 19–25   | General/full settings          | `GeneralSettingsWindow` / `ReferenceDetachedPreview` | general-settings                 | frontend/settings façades      |
| 26–28   | Input/add/calibrate            | `InputSettingsWindow`                                | input-settings                   | input enumeration/capture      |
| 29      | Hotkeys                        | `HotkeySettingsWindow`                               | hotkey-settings                  | stable HID usage mapping       |
| 30–32   | Account manager                | `AccountManagerWindow`                               | account-manager                  | account façade                 |
| 33–36   | Graphic packs                  | `GraphicPacksWindow`                                 | graphic-packs                    | packs + native picker          |
| 37–38   | CemuMod manager/details        | `CemodManagerWindow`                                 | cemod-manager                    | package inspection/jobs        |
| 39      | CemuMod permissions            | `CemodPermissionsWindow`                             | permission context fixture       | exact package generation       |
| 40–43   | Title manager/operations       | `TitleManagerWindow` / operation previews            | title-manager                    | title install/convert/delete   |
| 44–47   | Save manager/operations        | `SaveManagerWindow` / operation previews             | save-manager                     | save import/export/transfer    |
| 48–50   | Updates/catalogue              | `UpdateManagerWindow` / catalogue previews           | update-manager                   | opaque update plans/jobs       |
| 51      | Checksum                       | `ChecksumToolWindow`                                 | checksum-tool                    | checksum jobs/events           |
| 52      | Logging                        | `LoggingWindow`                                      | logging                          | retained copied log buffer     |
| 53      | Memory search                  | `MemorySearcherWindow`                               | memory-searcher                  | bounded typed scan façade      |
| 54–59   | PPC debugger views             | `PpcDebuggerWindow` / diagnostic previews            | ppc-debugger                     | copied registers/disassembly   |
| 60      | PPC threads                    | `PpcThreadsWindow`                                   | ppc-threads                      | thread snapshot/commands       |
| 61      | Texture relations              | `TextureRelationsWindow`                             | texture-relations                | paged copied diagnostics       |
| 62      | Audio voices                   | `AudioDebuggerWindow`                                | audio-debugger                   | paged copied diagnostics       |
| 63–68   | Emulated USB portals           | `EmulatedUsbDevicesWindow` / portal previews         | emulated-usb-devices             | USB enumeration/state          |
| 69–73   | Runtime overlays               | `RuntimeOverlayRoot`                                 | stats/notice/shader/key/error    | transparent render composition |
| 74      | Separate GamePad               | `gamepad-preview` fixture                            | main-library special state       | native render/input host       |
| 75–76   | Game profile                   | `GameProfilePreview` / profile façade                | dedicated profile state          | profile façade                 |
| 77      | About                          | `AboutWindow`                                        | about                            | build information façade       |
| 78      | Native unavailable             | `App` fatal state                                    | main-library special state       | bootstrap failure              |
| 79      | Shutdown                       | `App` shutdown state                                 | bootstrap `shuttingDown`         | ordered native teardown        |
| 80      | Bluetooth pairing              | input preview state                                  | input-settings                   | platform Bluetooth service     |

## Shared geometry

| Primitive                            | Size                                |
| ------------------------------------ | ----------------------------------- |
| Main title / menu / toolbar / status | 34 / 27 / 51 / 30 px                |
| Reference-fixture side navigation    | 193 px                              |
| Detached header / footer             | 59 / 49 px                          |
| Library action rail                  | 254 px                              |
| Field / button                       | 30 px desktop, 44 px coarse pointer |
| Side item / tab / table row          | 38 / 38 / 31 px                     |
| Library list row                     | 75 px                               |
| Library grid tile                    | 207 px (158 artwork + 49 caption)   |

## Required verification

1. `bun run typecheck`
2. `bun run lint`
3. `bun test`
4. `bun run build`
5. Browser capture at 1440 × 900 for representative main, embedded workspace,
   approval modal, developer, runtime, GamePad, fatal, and shutdown states.
6. Overflow and click-label checks at 320, 375, 414, and 768 px.
7. Console review. The unpackaged in-app browser may report its own CSP warning;
   application errors remain a failure.

## Completion language

- **Visual complete** · React renders the approved structure and state.
- **Native complete** · RPC behavior and ownership tests exist.
- **Validated** · visual and native requirements both passed.

Do not mark a native capability complete because its preview screen exists.
