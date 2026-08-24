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
};

export const SCREEN_REGISTRY: Record<
  ImplementedToolWindowRole,
  ScreenDefinition
> = {
  "general-settings": {
    title: "General Settings",
    category: "Settings",
    description: "Library, startup, storage, appearance, and advanced options.",
  },
  "input-settings": {
    title: "Input Settings",
    category: "Controllers",
    description: "Players, devices, mappings, profiles, and calibration.",
  },
  "hotkey-settings": {
    title: "Hotkey Settings",
    category: "Controllers",
    description: "Keyboard and controller shortcuts.",
  },
  "graphic-packs": {
    title: "Graphic Packs",
    category: "Mods",
    description: "Title enhancements, presets, installation, and refresh.",
  },
  "title-manager": {
    title: "Title Manager",
    category: "Downloads",
    description: "Installed content, conversion, verification, and removal.",
  },
  "cemod-manager": {
    title: "CemuMod Manager",
    category: "Mods",
    description: "Packages, discovery, compatibility, and approvals.",
  },
  "cemod-permissions": {
    title: "CemuMod Permissions",
    category: "Security",
    description: "Exact-package permission review and approval changes.",
  },
  "account-manager": {
    title: "Account Manager",
    category: "Accounts",
    description: "Local identities, active account, and network service.",
  },
  "save-manager": {
    title: "Save Manager",
    category: "Saves",
    description: "Save inspection, import, export, transfer, and removal.",
  },
  "update-manager": {
    title: "Downloads & Updates",
    category: "Downloads",
    description: "Tracked title and graphic-pack installation jobs.",
  },
  logging: {
    title: "Logging",
    category: "Developer",
    description: "Live structured logs with filtering and retention status.",
  },
  "memory-searcher": {
    title: "Memory Searcher",
    category: "Developer",
    description: "Typed scans, filtering, progress, and paged results.",
  },
  "ppc-debugger": {
    title: "PPC Debugger",
    category: "Developer",
    description: "Execution control, disassembly, registers, and breakpoints.",
  },
  "audio-debugger": {
    title: "Audio Voices",
    category: "Developer",
    description: "Active audio voice diagnostics.",
  },
  "texture-relations": {
    title: "Texture Relations",
    category: "Developer",
    description: "Texture cache relationships and diagnostic snapshots.",
  },
  "ppc-threads": {
    title: "PPC Threads",
    category: "Developer",
    description: "Thread state, priority, ownership, and execution controls.",
  },
  "emulated-usb-devices": {
    title: "Emulated USB",
    category: "Developer",
    description: "Emulated USB device state and attachment controls.",
  },
  "checksum-tool": {
    title: "Checksum Tool",
    category: "Developer",
    description: "Content verification jobs and detailed results.",
  },
  "getting-started": {
    title: "Getting Started",
    category: "Onboarding",
    description: "Initial frontend, library, and controller setup.",
  },
  about: {
    title: "About CemuExtend",
    category: "About",
    description: "Version, project, licence, and support information.",
  },
};
