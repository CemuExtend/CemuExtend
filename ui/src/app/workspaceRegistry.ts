import type { ImplementedToolWindowRole } from "../bridge/contracts";
import type { MainRoute } from "./navigation";

export type WorkspaceRoute = Exclude<MainRoute, "library">;

export const WORKSPACE_ROLES: Record<
  WorkspaceRoute,
  readonly ImplementedToolWindowRole[]
> = {
  mods: ["cemod-manager", "graphic-packs"],
  downloads: ["update-manager", "title-manager", "save-manager"],
  controllers: ["input-settings", "hotkey-settings"],
  accounts: ["account-manager"],
  settings: ["general-settings", "getting-started"],
  developer: [
    "logging",
    "memory-searcher",
    "ppc-debugger",
    "ppc-threads",
    "audio-debugger",
    "texture-relations",
    "emulated-usb-devices",
    "checksum-tool",
  ],
  help: ["about"],
};

export const DEFAULT_WORKSPACE_ROLE = Object.fromEntries(
  Object.entries(WORKSPACE_ROLES).map(([route, roles]) => [route, roles[0]]),
) as Record<WorkspaceRoute, ImplementedToolWindowRole>;

export function workspaceRouteForRole(
  role: ImplementedToolWindowRole,
): WorkspaceRoute | undefined {
  return (
    Object.entries(WORKSPACE_ROLES) as [
      WorkspaceRoute,
      readonly ImplementedToolWindowRole[],
    ][]
  ).find(([, roles]) => roles.includes(role))?.[0];
}
