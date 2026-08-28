# Design — CemuExtend Web UI

This is the locked design and visual-verification contract for the embedded
React frontend. The 81 approved 1440 × 900 PNGs in
the supplied reference image directory are the visual source of truth.
Native RPC contracts, window ownership, render surfaces, and shutdown ordering
remain implementation boundaries; the React presentation is replaced in full.

## System

- Genre · modern-minimal desktop workbench
- Macrostructure · Workbench
- Theme · dark desktop — cool charcoal surfaces with cyan signal colour
- Axes · dark paper / utilitarian sans / cool accent
- Navigation · OS-managed window decoration with an edge-aligned menu/toolbar
- Footer · Ft2-derived single-line status bar
- Enrichment · supplied title covers only; no decorative imagery

## Audience, use, tone

- Audience · players managing Wii U titles; diagnostic tools remain secondary.
- Primary use · find a title, configure it, launch it, and inspect native jobs.
- Tone · technical, compact, direct, desktop-native.

## Fixed desktop geometry

All approved captures are 1440 × 900 at DPR 1 and 100 % browser zoom.

| Surface        | Geometry                                                    |
| -------------- | ----------------------------------------------------------- |
| Main client    | menu 27 · toolbar 51 · content fills · status 30            |
| Main content   | 18 px inline / 12 px block padding · action rail 254 px     |
| Modal client   | exact-context CemuMod approval only · 760 × 620             |
| Dense controls | fields/buttons 30 · side items 38 · tabs 38 · table rows 31 |
| Library        | list row 75 · grid tile 207 · artwork 158 · tile caption 49 |

Desktop geometry is exact. Responsive rules are adaptations for browser-based
QA and accessibility; they must not change these anchors at 1440 × 900.

## Colour

`tokens.css` is canonical. Every UI colour is consumed through a named token.

- Canvas · `oklch(17.764% 0 89.88)`
- Dark inset · `oklch(21.648% 0.00498 248.06)`
- Paper · `oklch(25.069% 0.0048 248.02)`
- Elevated paper · `oklch(28.377% 0.00466 247.99)`
- Rule · `oklch(37.264% 0.00743 240.02)`
- Ink · `oklch(93.08% 0.0029 264.54)`
- Accent · `oklch(61.465% 0.12583 235.32)`
- Selection · `oklch(41.021% 0.07321 231.58)`
- Success, warning, and error use their named semantic token pairs plus text or
  icon; colour is never the only state signal.

No gradient chrome, glass surfaces, large radii, ambient glows, or pure-white
base surface. Game artwork and runtime imagery remain full colour.

## Typography

- Display · Segoe UI / Noto Sans fallback, weight 700, roman.
- Body · Segoe UI / Noto Sans fallback, weight 400, roman.
- Mono · Cascadia Mono / Noto Sans Mono fallback, tabular numerals.
- Desktop base · 14 px to reproduce the approved Qt-style density.
- Headings remain roman; no italic emphasis or decorative section eyebrows.

## Interaction

- Dense desktop controls are 30 px; touch/coarse-pointer controls expand to 44 px.
- Focus uses the named cyan ring and appears instantly.
- Hover exists only behind `(hover: hover) and (pointer: fine)`.
- Buttons and navigation labels never wrap.
- Loading, error, success, disabled, active, and selected states preserve layout.
- Modal shade covers only the WebView client area; OS window decoration remains external.
- Reversible local actions prefer Undo; irreversible package/title deletion keeps
  an explicit confirmation dialog.

## Motion

- Motion is cut by default.
- Button press, modal entry, and progress are the only standard primitives.
- Only transform and opacity animate; focus never animates.
- Reduced motion disables animation and transition entirely.

## Responsive

- Verify 320, 375, 414, 768, and 1440 px widths.
- `html`, `body`, and `#root` use `overflow-x: clip`.
- Main toolbar hides text before controls wrap.
- Library right rail stacks below content under 60 rem.
- Embedded workspace tabs scroll horizontally before their labels wrap.
- Diagnostic split panes and tables stack or reduce columns; no horizontal page
  scroll is allowed.

## Screen families

- Main application · 00–16: library, game workspace, direct tool workspaces, and job centre.
- Core settings · 17–29: onboarding, general/full settings, input, and hotkeys.
- Content and accounts · 30–51: accounts, packs, CemuMod, titles, saves, updates,
  download catalogue, and checksum.
- Developer · 52–68: logs, memory, PPC tools, texture/audio, and USB portals.
- Runtime and system · 69–80: HUD, notifications, shaders, keyboard, errors,
  GamePad, profiles, About, fatal/shutdown, and Bluetooth pairing.

Each state uses the common primitives in `src/components` and keeps its native
RPC behavior. Pixel fixtures may use fixed English preview data, but production
screens always render live native data with ellipsis and overflow safeguards.

## Source-of-truth exceptions

`01_main_library_grid.png` has no generated HTML counterpart. It is an approved
master image and must be compared directly. Its grouped four-column library
variant is not inferred from the other 80 HTML files.

Runtime captures 69–73 composite the approved `open_air` scene only in preview.
The production runtime overlay stays transparent over the native render surface.

## Visual QA contract

1. Typecheck, lint, unit tests, and production build must pass.
2. Capture at 1440 × 900, DPR 1, zoom 100 %, English preview data.
3. Compare shell anchors at y = 34 / 61 / 112 / 870 / 900. Detached geometry is
   retained only by reference fixtures and the exact-package approval modal.
4. Inspect every modal with its parent window retained behind the shade.
5. Inspect 320 / 375 / 414 / 768 for overflow, wrapped controls, and clipped focus.
6. Review console errors and execute representative navigation, form, modal,
   table, and overlay interactions before handoff.

## Hallmark final audit

- Pre-emit critique · P5 H5 E4 S5 R5 V4.
- Workbench hierarchy, reference-specific content, restrained accent use, one SVG
  language, N9 desktop chrome, and Ft2 status lines pass the visual gates.
- All authored colours and font families are tokens; focus is immediate; motion
  has a reduced-motion cut; labels remain single-line controls.
- Settings, content, account, onboarding, About, and developer tools render in
  the main workspace; only exact-package approval opens a native modal.
  A full 81-screen pixel-regression pass remains required before claiming exact
  visual completion.
- Dense 30 px controls and exact non-4 px chrome dimensions are deliberate source
  fidelity requirements for this native desktop workbench. Touch breakpoints use
  44 px controls and responsive layouts.

## Exports

### tokens.css

[`tokens.css`](./tokens.css) is the complete source of truth.

### Tailwind v4 `@theme`

```css
@theme {
  --color-paper: oklch(25.069% 0.0048 248.02);
  --color-paper-2: oklch(26.736% 0.00473 248);
  --color-paper-3: oklch(28.377% 0.00466 247.99);
  --color-rule: oklch(37.264% 0.00743 240.02);
  --color-ink: oklch(93.08% 0.0029 264.54);
  --color-accent: oklch(61.465% 0.12583 235.32);
  --font-display: "Segoe UI", "Noto Sans", ui-sans-serif, sans-serif;
  --font-body: "Segoe UI", "Noto Sans", ui-sans-serif, sans-serif;
  --font-outlier: "Cascadia Mono", "Noto Sans Mono", ui-monospace, monospace;
  --spacing-md: 1.1429rem;
  --ease-out: cubic-bezier(0.16, 1, 0.3, 1);
}
```

### DTCG `tokens.json`

```json
{
  "$schema": "https://design-tokens.github.io/community-group/format/",
  "color": {
    "paper": { "$value": "oklch(25.069% 0.0048 248.02)", "$type": "color" },
    "ink": { "$value": "oklch(93.08% 0.0029 264.54)", "$type": "color" },
    "accent": { "$value": "oklch(61.465% 0.12583 235.32)", "$type": "color" }
  },
  "font": {
    "display": {
      "$value": "Segoe UI, Noto Sans, ui-sans-serif, sans-serif",
      "$type": "fontFamily"
    },
    "body": {
      "$value": "Segoe UI, Noto Sans, ui-sans-serif, sans-serif",
      "$type": "fontFamily"
    },
    "outlier": {
      "$value": "Cascadia Mono, Noto Sans Mono, ui-monospace, monospace",
      "$type": "fontFamily"
    }
  },
  "space": { "md": { "$value": "1.1429rem", "$type": "dimension" } }
}
```

### shadcn/ui CSS variables

```css
:root {
  --background: 25.069% 0.0048 248.02;
  --foreground: 93.08% 0.0029 264.54;
  --card: 28.377% 0.00466 247.99;
  --card-foreground: 93.08% 0.0029 264.54;
  --primary: 61.465% 0.12583 235.32;
  --primary-foreground: 98% 0.004 230;
  --muted: 37.264% 0.00743 240.02;
  --muted-foreground: 73.2% 0.00811 241.71;
  --border: 42.817% 0.00926 241.85;
  --input: 42.817% 0.00926 241.85;
  --ring: 73.161% 0.1383 230.13;
  --radius: 3px;
}
```
