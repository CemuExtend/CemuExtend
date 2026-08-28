import { en } from "../i18n/en";

export type MainRoute =
  | "library"
  | "mods"
  | "downloads"
  | "controllers"
  | "accounts"
  | "settings"
  | "developer"
  | "help";
export type AppRoute = MainRoute | "game";

export type NavigationItem = {
  route: MainRoute;
  label: string;
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

export const helpNavigation: NavigationItem = {
  route: "help",
  label: en.navigation.help,
};

export function routeLabel(route: AppRoute): string {
  if (route === "game") return en.navigation.gameWorkspace;
  if (route === "developer") return developerNavigation.label;
  if (route === "help") return helpNavigation.label;
  return primaryNavigation.find((item) => item.route === route)?.label ?? route;
}
