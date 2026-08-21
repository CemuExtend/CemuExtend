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

export type ChecksumContent = { locationUid: string; titleId: string; name: string; version: number; region: string; type: "base" | "update" | "dlc" | "system"; format: "folder" | "wud" | "nus" | "wua" | "wuhb" };
export type ChecksumModel = { entries: ChecksumContent[] };

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

export type FrontendSettings = {
  revision: number;
  gamePaths: string[];
  startFullscreen: boolean;
  openPad: boolean;
  checkUpdates: boolean;
  saveScreenshots: boolean;
  updateChecksSupported: boolean;
  portableMode: boolean;
  titleRunning: boolean;
  setupCompleted: boolean;
  fullscreenOverride: boolean | null;
};

export type FrontendSettingsUpdate = Pick<FrontendSettings,
  "revision" | "gamePaths" | "startFullscreen" | "openPad" | "checkUpdates" | "saveScreenshots"> & {
  completeSetup: boolean;
};

export type FrontendSettingsApplyResult = {
  ok: boolean;
  error: "none" | "conflict" | "titleRunning" | "fullscreenOverride" | "updateUnsupported" | "invalidPath" | "storageFailed" | "saveFailed";
  snapshot: FrontendSettings;
  diagnostic: string;
};

export type EmulatedControllerType = "disabled" | "gamePad" | "proController" | "classicController" | "wiimote";
export type ControllerAxisSettings = { deadzone: number; range: number };
export type PhysicalControllerSettings = { axis: ControllerAxisSettings; rotation: ControllerAxisSettings; trigger: ControllerAxisSettings; rumble: number; motion: boolean; packetDelay?: number };
export type PhysicalController = { token: number; api: string; displayName: string; connected: boolean; hasBattery: boolean; lowBattery: boolean; hasMotion: boolean; hasRumble: boolean; wiimoteExtension?: "none" | "nunchuck" | "classic" | "motionPlus"; settings: PhysicalControllerSettings };
export type CapturedInputButton = { id: number; label: string };
export type InputMapping = { mappingId: number; label: string; binding: string; controllerToken?: number };
export type InputPlayer = { player: number; type: EmulatedControllerType; gameProfileLocked: boolean; profileName: string; controllers: PhysicalController[]; mappings: InputMapping[] };
export type InputSettingsModel = { generation: number; players: InputPlayer[]; profiles: string[]; availableApis: string[] };
export type InputDeviceCandidate = { token: number; api: string; displayName: string; connected: boolean };

export type HotkeyAction = "toggleFullscreen" | "toggleFullscreenAlternative" | "exitFullscreen" | "takeScreenshot" | "toggleFastForward" | "endEmulation" | "exitApplication";
export type HotkeyBinding = { action: HotkeyAction; keyboardUsage: number; keyboardModifiers: number; controllerButton: number | null; controllerLabel: string };
export type HotkeySettingsModel = { revision: number; controllerModifier: number | null; controllerModifierLabel: string; controller: { token: number; displayName: string } | null; bindings: HotkeyBinding[] };
export type HotkeySettingsUpdate = { revision: number; controllerModifier: number | null; bindings: Array<Omit<HotkeyBinding, "controllerLabel">> };
export type HotkeySettingsApplyResult = { ok: boolean; error: "none" | "conflict" | "invalidBinding" | "duplicateBinding" | "saveFailed"; snapshot: HotkeySettingsModel; diagnostic: string };

declare global {
  interface Window {
    cemuInvoke?: (requestJson: string) => Promise<string>;
    __CEMU_BOOTSTRAP__?: Partial<Bootstrap>;
    __cemuDispatchEvent?: (event: NativeEvent) => void;
  }
}
