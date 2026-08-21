import { expect, test } from "bun:test";
import { activateJob, routeJobEvent } from "./checksumEvents";

test("replays checksum events that arrive before the start response", () => {
  const pending = new Map<number, Array<{ type: string; payload: string }>>();
  const applied: string[] = [];
  routeJobEvent(pending, undefined, 7, { type: "jobs.progress", payload: "first" }, (event) => applied.push(event.payload));
  routeJobEvent(pending, undefined, 7, { type: "jobs.completed", payload: "done" }, (event) => applied.push(event.payload));
  expect(applied).toEqual([]);
  activateJob(pending, 7, (event) => applied.push(event.payload));
  expect(applied).toEqual(["first", "done"]);
  expect(pending.has(7)).toBe(false);
});
