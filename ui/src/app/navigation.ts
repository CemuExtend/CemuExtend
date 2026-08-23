import type { ImplementedToolWindowRole } from "../bridge/contracts";
import { en } from "../i18n/en";

export type MainRoute =
  | "library"
  | "mods"
  | "downloads"
  | "controllers"
  | "accounts"
  | "settings"
  | "developer";
export type AppRoute = MainRoute | "game";
export type ToolRoute = Exclude<MainRoute, "library">;

export type NavigationItem = {
  route: MainRoute;
  label: string;
};

export type ToolAction = {
  role: ImplementedToolWindowRole;
};

export const primaryNavigation: NavigationItem[] = [
  { route: "library", label: en.navigation.library },
  { route: "mods", label: en.navigation.mods },
  { route: "downloads", label: en.navigation.downloads },
  { route: "controllers", label: en.navigation.controllers },
  { route: "accounts", label: en.navigation.accounts },
  { route: "settings", label: en.navigation.settings },
];

export const developerNavigation: NavigationItem = {
  route: "developer",
  label: en.navigation.developer,
};

export const pageTools: Record<ToolRoute, ToolAction[]> = {
  mods: [{ role: "cemod-manager" }, { role: "graphic-packs" }],
  downloads: [{ role: "update-manager" }, { role: "title-manager" }],
  controllers: [{ role: "input-settings" }, { role: "hotkey-settings" }],
  accounts: [{ role: "account-manager" }],
  settings: [
    { role: "general-settings" },
    { role: "getting-started" },
    { role: "about" },
  ],
  developer: [
    { role: "logging" },
    { role: "memory-searcher" },
    { role: "ppc-debugger" },
    { role: "ppc-threads" },
    { role: "audio-debugger" },
    { role: "texture-relations" },
    { role: "emulated-usb-devices" },
    { role: "checksum-tool" },
  ],
};

export function routeLabel(route: AppRoute): string {
  if (route === "game") return en.navigation.gameWorkspace;
  return route === "developer"
    ? developerNavigation.label
    : (primaryNavigation.find((item) => item.route === route)?.label ?? route);
}
