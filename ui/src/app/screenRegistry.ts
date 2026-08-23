import type { ImplementedToolWindowRole } from "../bridge/contracts";

export type ScreenCategory =
  | "Settings"
  | "Controllers"
  | "Mods"
  | "Security"
  | "Downloads"
  | "Accounts"
  | "Saves"
  | "Developer"
  | "Onboarding"
  | "About";

export type ScreenDefinition = {
  title: string;
  category: ScreenCategory;
  description: string;
  detachable: boolean;
};

export const SCREEN_REGISTRY: Record<
  ImplementedToolWindowRole,
  ScreenDefinition
> = {
  "general-settings": {
    title: "General settings",
    category: "Settings",
    description: "Library, startup, storage, appearance, and advanced options.",
    detachable: false,
  },
  "input-settings": {
    title: "Input settings",
    category: "Controllers",
    description: "Players, devices, mappings, profiles, and calibration.",
    detachable: false,
  },
  "hotkey-settings": {
    title: "Hotkey settings",
    category: "Controllers",
    description: "Keyboard and controller shortcuts.",
    detachable: false,
  },
  "graphic-packs": {
    title: "Graphic packs",
    category: "Mods",
    description: "Title enhancements, presets, installation, and refresh.",
    detachable: false,
  },
  "title-manager": {
    title: "Title manager",
    category: "Downloads",
    description: "Installed content, conversion, verification, and removal.",
    detachable: false,
  },
  "cemod-manager": {
    title: "CemuMod manager",
    category: "Mods",
    description: "Packages, discovery, compatibility, and approvals.",
    detachable: false,
  },
  "cemod-permissions": {
    title: "CemuMod permissions",
    category: "Security",
    description: "Exact-package permission review and approval changes.",
    detachable: false,
  },
  "account-manager": {
    title: "Account manager",
    category: "Accounts",
    description: "Local identities, active account, and network service.",
    detachable: false,
  },
  "save-manager": {
    title: "Save manager",
    category: "Saves",
    description: "Save inspection, import, export, transfer, and removal.",
    detachable: false,
  },
  "update-manager": {
    title: "Downloads and updates",
    category: "Downloads",
    description: "Tracked title and graphic-pack installation jobs.",
    detachable: false,
  },
  logging: {
    title: "Logging",
    category: "Developer",
    description: "Live structured logs with filtering and retention status.",
    detachable: true,
  },
  "memory-searcher": {
    title: "Memory search",
    category: "Developer",
    description: "Typed scans, filtering, progress, and paged results.",
    detachable: true,
  },
  "ppc-debugger": {
    title: "PPC debugger",
    category: "Developer",
    description: "Execution control, disassembly, registers, and breakpoints.",
    detachable: true,
  },
  "audio-debugger": {
    title: "Audio voices",
    category: "Developer",
    description: "Active audio voice diagnostics.",
    detachable: true,
  },
  "texture-relations": {
    title: "Texture relations",
    category: "Developer",
    description: "Texture cache relationships and diagnostic snapshots.",
    detachable: true,
  },
  "ppc-threads": {
    title: "PPC threads",
    category: "Developer",
    description: "Thread state, priority, ownership, and execution controls.",
    detachable: true,
  },
  "emulated-usb-devices": {
    title: "USB devices",
    category: "Developer",
    description: "Emulated USB device state and attachment controls.",
    detachable: true,
  },
  "checksum-tool": {
    title: "Checksum tool",
    category: "Developer",
    description: "Content verification jobs and detailed results.",
    detachable: true,
  },
  "getting-started": {
    title: "Getting started",
    category: "Onboarding",
    description: "Initial frontend, library, and controller setup.",
    detachable: false,
  },
  about: {
    title: "About CemuExtend",
    category: "About",
    description: "Version, project, licence, and support information.",
    detachable: false,
  },
};
