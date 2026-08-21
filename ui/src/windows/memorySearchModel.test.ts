import { describe, expect, test } from "bun:test";
import { pageOffset, parseMemoryValue, progressPercent } from "./memorySearchModel";

describe("memory search model", () => {
  test("validates typed integer bounds without losing int64 precision", () => {
    expect(parseMemoryValue("int8", "128")).toBe("Value is outside the int8 range.");
    expect(parseMemoryValue("int64", "9223372036854775807")).toEqual({ type: "int64", text: "9223372036854775807" });
    expect(typeof parseMemoryValue("int64", "9223372036854775808")).toBe("string");
  });
  test("rejects non-finite float input", () => expect(typeof parseMemoryValue("float32", "Infinity")).toBe("string"));
  test("clamps pagination and progress", () => {
    expect(pageOffset(10, 100, 215)).toBe(200);
    expect(pageOffset(-1, 100, 215)).toBe(0);
    expect(progressPercent({ generation: "1", state: "scanning", bytesScanned: 20, bytesTotal: 10, resultCount: 0, resultCapReached: false, scanCapReached: false, diagnostic: "" })).toBe(100);
  });
});
