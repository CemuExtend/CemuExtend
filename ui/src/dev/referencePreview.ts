import type { ActiveWindowRole } from "../bridge/contracts";
import type { MainRoute } from "../app/navigation";

export type ReferencePreviewScreen = {
  index: number;
  slug: string;
  role: ActiveWindowRole;
};

function roleForIndex(index: number): ActiveWindowRole {
  if (
    index <= 16 ||
    index === 74 ||
    index === 75 ||
    index === 76 ||
    index >= 78
  )
    return "main-library";
  if (index <= 18) return "getting-started";
  if (index <= 25) return "general-settings";
  if (index <= 28 || index === 80) return "input-settings";
  if (index === 29) return "hotkey-settings";
  if (index <= 32) return "account-manager";
  if (index <= 36) return "graphic-packs";
  if (index <= 38) return "cemod-manager";
  if (index === 39) return "cemod-permissions";
  if (index <= 43) return "title-manager";
  if (index <= 47) return "save-manager";
  if (index <= 50) return "update-manager";
  if (index === 51) return "checksum-tool";
  if (index === 52) return "logging";
  if (index === 53) return "memory-searcher";
  if (index <= 59) return "ppc-debugger";
  if (index === 60) return "ppc-threads";
  if (index === 61) return "texture-relations";
  if (index === 62) return "audio-debugger";
  if (index <= 68) return "emulated-usb-devices";
  if (index <= 73) return "runtime-overlay";
  return index === 77 ? "about" : "main-library";
}

export function getReferencePreviewScreen():
  ReferencePreviewScreen | undefined {
  const search = new URLSearchParams(window.location.search);
  if (search.get("preview") !== "1") return undefined;
  const slug = search.get("screen");
  const match = slug?.match(/^(\d{2})_/);
  if (!match) return undefined;
  const index = Number(match[1]);
  if (!Number.isInteger(index) || index < 0 || index > 80) return undefined;
  return { index, slug: slug!, role: roleForIndex(index) };
}

export function mainRouteForScreen(
  index: number,
): MainRoute | "game" | undefined {
  if (index <= 3) return "library";
  if ((index >= 4 && index <= 9) || index === 75 || index === 76) return "game";
  const routes: Partial<Record<number, MainRoute>> = {
    10: "mods",
    11: "downloads",
    12: "controllers",
    13: "accounts",
    14: "settings",
    15: "developer",
    16: "library",
  };
  return routes[index];
}

export function overlayModeForScreen(index: number): string | undefined {
  return (
    {
      69: "stats",
      70: "notifications",
      71: "shader",
      72: "keyboard",
      73: "error",
    } as Record<number, string>
  )[index];
}

export function hasReferenceDetachedPreview(index: number): boolean {
  return (
    (index >= 21 && index <= 25) ||
    (index >= 30 && index <= 32) ||
    (index >= 40 && index <= 50) ||
    (index >= 55 && index <= 59) ||
    (index >= 63 && index <= 68)
  );
}
