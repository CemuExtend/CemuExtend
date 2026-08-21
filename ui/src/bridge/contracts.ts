import { implementedWindowRoles, windowRoles } from "../generated/roles";
export { implementedWindowRoles, windowRoles };

export type WindowRole = (typeof windowRoles)[number];
export type ImplementedToolWindowRole = (typeof implementedWindowRoles)[number];
export type ActiveWindowRole = "main-library" | ImplementedToolWindowRole;

export type RpcRequest = { id: string; method: string; params: unknown };
export type RpcError = { code: string; message: string; details?: unknown };
export type RpcResponse<T = unknown> =
  | { id: string; ok: true; result: T }
  | { id: string; ok: false; error: RpcError };

export type Bootstrap = {
  windowId: number;
  windowRole: ActiveWindowRole;
  appVersion: string;
  platform: string;
  theme: "light" | "dark" | "system";
  shuttingDown: boolean;
  context?: { titleId?: string };
};

export type WindowOpenRequest =
  | { role: "graphic-packs"; requestId: string; context?: { titleId?: string } }
  | { role: Exclude<ImplementedToolWindowRole, "graphic-packs">; requestId: string };

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

export type AboutInfo = {
  name: string;
  version: string;
  commit: string;
  buildDate: string;
  frontend: "webview-react";
  webviewEngine: string;
  originalAuthors: string[];
  libraries: Array<{ name: string; license: string; url: string }>;
  links: Array<{ label: string; url: string }>;
};

export type Account = {
  persistentId: number;
  persistentIdHex: string;
  miiName: string;
  birthYear: number;
  birthMonth: number;
  birthDay: number;
  gender: number;
  email: string;
  country: number;
  validOnlineAccount: boolean;
};

export type AccountCountry = { code: number; name: string };

export type AccountManagerModel = {
  accounts: Account[];
  countries: AccountCountry[];
  nextPersistentId: number;
  hasFreeSlots: boolean;
  activePersistentId: number;
  titleRunning: boolean;
  networkSettings: Array<{
    persistentId: number;
    service: "offline" | "nintendo" | "pretendo" | "custom" | "plasma";
    validation: {
      validAccount: boolean;
      otp: "missing" | "corrupted" | "ok";
      seeprom: "missing" | "corrupted" | "ok";
      missingFiles: string[];
      accountError: "none" | "noAccountId" | "noPasswordCached" | "passwordCacheEmpty" | "noPrincipalId";
    };
  }>;
  onlineEnvironment: {
    requiredFilesAvailable: boolean;
    otpPresent: boolean;
    seepromPresent: boolean;
    consoleCertificateAvailable: boolean;
  };
};

export type AccountUpdate = Omit<Account, "persistentIdHex" | "validOnlineAccount">;
export type AccountNetworkService = AccountManagerModel["networkSettings"][number]["service"];

export type GraphicPackPreset = {
  category: string;
  name: string;
  active: boolean;
  visible: boolean;
};

export type GraphicPack = {
  key: string;
  virtualPath: string;
  name: string;
  description: string;
  version: number;
  universal: boolean;
  enabled: boolean;
  activated: boolean;
  defaultEnabled: boolean;
  hasShaders: boolean;
  hasPatches: boolean;
  hasCustomVsync: boolean;
  supportedVersion: boolean;
  titleIds: string[];
  presetOrder: string[];
  presets: GraphicPackPreset[];
};

export type GraphicPackMutation = {
  changed: boolean;
  titleRunning: boolean;
  requiresRestart: boolean;
  applied: boolean;
  reloaded: boolean;
  diagnostic: string;
  info?: GraphicPack;
};

export type GraphicPackInstallKind = "community" | "customUrl";
export type GraphicPackInstallRequest = {
  kind: GraphicPackInstallKind;
  url?: string;
  replaceExisting: boolean;
};

declare global {
  interface Window {
    cemuInvoke?: (requestJson: string) => Promise<string>;
    __CEMU_BOOTSTRAP__?: Partial<Bootstrap>;
    __cemuDispatchEvent?: (event: NativeEvent) => void;
  }
}
