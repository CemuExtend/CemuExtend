import type { CemuIconName } from "../components/CemuIcon";

export type PreviewNavigationItem = {
  label: string;
  icon: CemuIconName;
  indices: number[];
};

const settings: PreviewNavigationItem[] = [
  { label: "General", icon: "settings", indices: [17, 18, 19, 20] },
  { label: "Graphics", icon: "display", indices: [21] },
  { label: "Audio", icon: "audio", indices: [22] },
  { label: "Overlay", icon: "display", indices: [23] },
  { label: "Input", icon: "controller", indices: [26, 27, 28, 80] },
  { label: "Hotkeys", icon: "key", indices: [29] },
  { label: "Accounts", icon: "account", indices: [30, 31, 32] },
  { label: "CemuExtend", icon: "mods", indices: [33, 34, 35, 36, 37, 38, 39] },
  { label: "Debug", icon: "terminal", indices: [24] },
  { label: "TCPGecko", icon: "link", indices: [25] },
  { label: "About", icon: "help", indices: [77] },
];

const onboarding: PreviewNavigationItem[] = [
  { label: "Welcome", icon: "help", indices: [] },
  { label: "Game Library", icon: "library", indices: [17] },
  { label: "Startup", icon: "play", indices: [18] },
];

const content: PreviewNavigationItem[] = [
  { label: "Accounts", icon: "account", indices: [30, 31, 32] },
  { label: "Graphic Packs", icon: "display", indices: [33, 34, 35, 36] },
  { label: "CemuMod Manager", icon: "mods", indices: [37, 38] },
  { label: "Permissions", icon: "check", indices: [39] },
  { label: "Title Manager", icon: "library", indices: [40, 41, 42, 43] },
  { label: "Save Manager", icon: "box", indices: [44, 45, 46, 47] },
  { label: "Downloads", icon: "download", indices: [48, 49, 50] },
  { label: "Checksum", icon: "check", indices: [51] },
];

const developer: PreviewNavigationItem[] = [
  { label: "Logging", icon: "terminal", indices: [52] },
  { label: "Memory Search", icon: "search", indices: [53] },
  { label: "PPC Debugger", icon: "tools", indices: [54, 55, 56, 57, 58, 59] },
  { label: "PPC Threads", icon: "list", indices: [60] },
  { label: "Texture Relations", icon: "image", indices: [61] },
  { label: "Audio Voices", icon: "audio", indices: [62] },
  {
    label: "USB Devices",
    icon: "controller",
    indices: [63, 64, 65, 66, 67, 68],
  },
];

const usb: PreviewNavigationItem[] = [
  { label: "Overview", icon: "download", indices: [63] },
  { label: "Skylanders", icon: "box", indices: [64, 67] },
  { label: "Disney Infinity", icon: "box", indices: [65] },
  { label: "LEGO Dimensions", icon: "box", indices: [66, 68] },
];

export function previewNavigationFor(index: number): PreviewNavigationItem[] {
  if (index === 17 || index === 18) return onboarding;
  if (index >= 63 && index <= 68) return usb;
  if (index >= 52 && index <= 68) return developer;
  if (index >= 30 && index <= 51) return content;
  return settings;
}

export function previewTitleFor(index: number, fallback: string): string {
  if (index >= 21 && index <= 25) return "General Settings";
  if (index === 49) return "Application Update";
  if (index === 50) return "Download Manager";
  if (index >= 63 && index <= 68) return "Emulated USB Devices";
  if (index >= 55 && index <= 59) {
    const view = [
      "Registers",
      "Memory Dump",
      "Breakpoints",
      "Modules",
      "Symbols",
    ][index - 55];
    return `PPC Debugger — ${view}`;
  }
  return fallback;
}
