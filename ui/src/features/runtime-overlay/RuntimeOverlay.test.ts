import { describe, expect, test } from "bun:test";
import { overlayColor } from "./runtimeOverlayModel";

describe("runtime overlay color conversion", () => {
  test("normalizes configured colors to the monochrome app theme", () => {
    expect(overlayColor(0xff0000ff)).toBe("var(--color-ink)");
    expect(overlayColor(0x8000ff00)).toBe("var(--color-ink)");
  });
});
