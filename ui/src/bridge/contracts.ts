export const windowRoles = [
  "main-library", "general-settings", "input-settings", "hotkey-settings",
  "graphic-packs", "title-manager", "cemod-manager", "cemod-permissions",
  "account-manager", "save-manager", "update-manager", "logging",
  "memory-searcher", "ppc-debugger", "audio-debugger", "texture-relations",
  "ppc-threads", "emulated-usb-devices", "checksum-tool", "getting-started", "about"
] as const;

export type WindowRole = (typeof windowRoles)[number];

export type RpcRequest = { id: string; method: string; params: unknown };
export type RpcError = { code: string; message: string; details?: unknown };
export type RpcResponse<T = unknown> =
  | { id: string; ok: true; result: T }
  | { id: string; ok: false; error: RpcError };

export type Bootstrap = {
  windowRole: WindowRole;
  appVersion: string;
  platform: string;
  theme: "light" | "dark" | "system";
  shuttingDown: boolean;
};

export type Title = {
  titleId: string;
  name: string;
  path: string;
  region: string;
  version: number;
  playTimeMinutes: number;
  lastPlayed: string | null;
  iconDataUrl?: string;
};

export type NativeEvent = { type: string; sequence: number; payload: unknown };

export type RpcContract = {
  "system.bootstrap": { params: undefined; result: Bootstrap };
  "system.quit": { params: undefined; result: Record<string, never> };
  "window.close": { params: undefined; result: Record<string, never> };
  "window.getModel": { params: { role: WindowRole }; result: Record<string, unknown> };
  "titles.list": { params: undefined; result: Title[] };
  "titles.refresh": { params: undefined; result: Record<string, never> };
  "titles.launch": { params: { titleId: string }; result: { titleId: string } };
  "emulation.stop": { params: undefined; result: Record<string, never> };
};

export type RpcMethod = keyof RpcContract;

declare global {
  interface Window {
    cemuInvoke?: (requestJson: string) => Promise<string>;
    __CEMU_BOOTSTRAP__?: Partial<Bootstrap>;
    __cemuDispatchEvent?: (event: NativeEvent) => void;
  }
}
