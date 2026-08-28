import { expect, test } from "bun:test";
import { hotkeyUsageForCode } from "./hotkeyKeys";

test("maps browser keyboard codes to stable USB HID usages", () => {
  expect(hotkeyUsageForCode("KeyA")).toBe(0x04);
  expect(hotkeyUsageForCode("F1")).toBe(0x3a);
  expect(hotkeyUsageForCode("F12")).toBe(0x45);
  expect(hotkeyUsageForCode("PrintScreen")).toBe(0x46);
  expect(hotkeyUsageForCode("F13")).toBe(0x68);
  expect(hotkeyUsageForCode("F24")).toBe(0x73);
  expect(hotkeyUsageForCode("AudioVolumeUp")).toBeUndefined();
});
