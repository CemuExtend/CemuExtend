import { describe, expect, test } from "bun:test";
import type { NativeEvent } from "../bridge/contracts";
import {
  loggingSnapshotFromEvent,
  maximumUiEntries,
  mergeLoggingEntries,
} from "./loggingEvents";

describe("logging events", () => {
  test("validates typed entry payloads", () => {
    const event: NativeEvent = {
      type: "logging.entries",
      sequence: "1",
      payload: {
        entries: [
          {
            sequence: "4",
            level: "warning",
            category: "GX2",
            message: "warning",
          },
        ],
        firstAvailableSequence: "4",
        nextSequence: "5",
        droppedEntries: "0",
        retainedBytes: "10",
        truncated: false,
      },
    };
    expect(loggingSnapshotFromEvent(event)?.entries[0]?.category).toBe("GX2");
    expect(
      loggingSnapshotFromEvent({
        ...event,
        payload: {
          ...(event.payload as object),
          entries: [{ sequence: "1", level: "debug" }],
        },
      }),
    ).toBeUndefined();
  });

  test("deduplicates, orders, and bounds retained entries", () => {
    const entries = Array.from(
      { length: maximumUiEntries + 5 },
      (_, sequence) => ({
        sequence: String(sequence),
        level: "info" as const,
        category: "",
        message: `${sequence}`,
      }),
    );
    const merged = mergeLoggingEntries(entries.slice(0, 10), entries.slice(5));
    expect(merged).toHaveLength(maximumUiEntries);
    expect(merged[0]?.sequence).toBe("5");
    expect(merged.at(-1)?.sequence).toBe(String(maximumUiEntries + 4));
  });
});
