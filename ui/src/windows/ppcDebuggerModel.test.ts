import { describe, expect, test } from "bun:test";
import { centerAddress, parseGuestAddress } from "./ppcDebuggerModel";

describe("PPC debugger address model", () => {
  test("normalizes bounded guest addresses", () => {
    expect(parseGuestAddress("0x1000")).toBe("00001000");
    expect(parseGuestAddress("100000000")).toBeUndefined();
    expect(parseGuestAddress("native:pointer")).toBeUndefined();
  });
  test("clamps scrolling to aligned guest range", () => {
    expect(centerAddress("00000004", -2)).toBe("00000000");
    expect(centerAddress("FFFFFFFC", 2)).toBe("FFFFFFFC");
  });
});
