# Design — CemuExtend

A locked design system for the embedded React application. Native WebView hosts,
generated RPC contracts, and detachable developer windows remain implementation
boundaries; migrated product flows live in one application shell.

## Genre

modern-minimal, with an austere and utilitarian desktop application voice.

## Audience, use, tone

- Audience: players managing and launching Wii U titles; developer tools are secondary.
- Primary use: find a title, inspect its workspace, configure it, and launch it.
- Tone: austere, technical, compact, direct.

## Macrostructure family

- App pages: Workbench with a persistent N3 side-rail and a compact command header.
- Developer pages: Workbench with split panes, dense tables, and detachable windows.
- Content pages: Long Document, used only for onboarding and About content.

Every generated Native role is registered in `src/app/screenRegistry.ts` and
rendered through the shared `DetachedToolShell`. Normal user workflows remain
discoverable from the primary App Shell; monitoring workspaces remain detachable.

## Theme

Only two colour values are allowed anywhere in UI chrome: black and white.
No chromatic accent, grey token, translucent overlay, colour-mix, or colour gradient.

- White Theme: paper `oklch(100% 0 0)`, ink `oklch(0% 0 0)`.
- Dark Theme: paper `oklch(0% 0 0)`, ink `oklch(100% 0 0)`.
- Active, selected, busy, warning, error, and success states use inversion, border
  weight/style, text, and symbols rather than additional colours.

## Typography

- Display: native UI monospace, weight 700, normal style.
- Body: native UI sans, weight 400.
- Mono: native UI monospace, weight 400.
- Headings remain roman; no italic headings.

## Spacing

A 4-point named scale is defined in `tokens.css`. New CSS must consume named
tokens and should introduce a named token before adding a repeated raw dimension.

## Motion

- Motion is cut by default.
- Interactive feedback may use transform or opacity only.
- Reduced-motion disables spatial changes.
- Focus appears instantly and is never animated.

## Interaction stance

- Minimum control height is 44 CSS pixels.
- Keyboard focus is a high-contrast outline using the current ink colour.
- No colour-only state communication.
- Reversible actions favour Undo; destructive actions require explicit language.
- Loading, empty, error, cancelled, and permission-required states use stable layouts.

## CTA voice

- Primary: inverted black/white rectangular control, imperative label.
- Secondary: paper background, ink border, imperative label.
- Buttons and navigation labels never wrap.

## Per-page allowances

- App pages do not use decorative enrichment; function carries the page.
- Game artwork is treated as content and rendered monochrome in the shell.
- Developer pages prefer tables and split panes over cards.

## What pages must share

- Two-value black/white palette.
- Body, display, and mono font roles.
- Side-rail hierarchy, control geometry, focus treatment, and spacing scale.
- Compact density and plain-language labels.

## What pages may differ on

- Split-pane proportions through named size tokens.
- Table column layouts based on the diagnostic data.
- Which detachable Native developer window a page opens.

## Exports

### tokens.css

The canonical drop-in CSS export is [`tokens.css`](./tokens.css).

### Tailwind v4 `@theme`

```css
@theme {
  --color-paper: oklch(100% 0 0);
  --color-ink: oklch(0% 0 0);
  --color-accent: oklch(0% 0 0);
  --font-display: ui-monospace, monospace;
  --font-body: ui-sans-serif, sans-serif;
  --spacing-md: 1.5rem;
  --ease-out: cubic-bezier(0.16, 1, 0.3, 1);
}
```

### DTCG `tokens.json`

```json
{
  "color": {
    "paper": { "$value": "oklch(100% 0 0)", "$type": "color" },
    "ink": { "$value": "oklch(0% 0 0)", "$type": "color" },
    "accent": { "$value": "oklch(0% 0 0)", "$type": "color" }
  },
  "font": {
    "display": { "$value": "ui-monospace", "$type": "fontFamily" },
    "body": { "$value": "ui-sans-serif", "$type": "fontFamily" }
  },
  "space": {
    "md": { "$value": "1.5rem", "$type": "dimension" }
  }
}
```

### shadcn/ui CSS variables

```css
:root {
  --background: 100% 0 0;
  --foreground: 0% 0 0;
  --primary: 0% 0 0;
  --primary-foreground: 100% 0 0;
  --muted: 100% 0 0;
  --muted-foreground: 0% 0 0;
  --border: 0% 0 0;
  --input: 0% 0 0;
  --ring: 0% 0 0;
  --radius: 0;
}
```
