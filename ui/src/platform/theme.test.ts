import { expect, test } from "bun:test";
import { isUiTheme, resolveInitialTheme } from "./theme";

test("accepts only the two application themes", () => {
  expect(isUiTheme("light")).toBe(true);
  expect(isUiTheme("dark")).toBe(true);
  expect(isUiTheme("system")).toBe(false);
  expect(isUiTheme(undefined)).toBe(false);
});

test("native application theme takes precedence over the system fallback", () => {
  expect(resolveInitialTheme("light")).toBe("light");
  expect(resolveInitialTheme("dark")).toBe("dark");
});
