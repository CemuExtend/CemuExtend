import { describe, expect, test } from "bun:test";
import { parseCemodSnapshot } from "./cemodEvents";

describe("parseCemodSnapshot", () => {
  test("accepts only the owned job completion", () => {
    const payload = {
      jobId: "7",
      ok: true,
      error: "none",
      diagnostic: "",
      snapshot: {
        generation: "9",
        selectedTitleId: null,
        packages: [],
        cancelled: false,
      },
    };
    expect(
      parseCemodSnapshot(
        { type: "cemod.snapshot", sequence: "1", payload },
        "7",
      )?.snapshot.generation,
    ).toBe("9");
    expect(
      parseCemodSnapshot(
        { type: "cemod.snapshot", sequence: "2", payload },
        "8",
      ),
    ).toBeUndefined();
  });
});
