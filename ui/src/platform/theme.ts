import type { Bootstrap } from "../bridge/contracts";

export type UiTheme = "light" | "dark";

export function isUiTheme(value: unknown): value is UiTheme {
  return value === "light" || value === "dark";
}

function prefersDarkTheme(): boolean {
  try {
    return window.matchMedia?.("(prefers-color-scheme: dark)").matches ?? false;
  } catch {
    return false;
  }
}

export function resolveInitialTheme(nativeTheme: Bootstrap["theme"]): UiTheme {
  if (nativeTheme === "light" || nativeTheme === "dark") return nativeTheme;
  return prefersDarkTheme() ? "dark" : "light";
}

export function applyTheme(theme: UiTheme): void {
  document.documentElement.dataset.theme = theme;
  document.documentElement.style.colorScheme = theme;
}
