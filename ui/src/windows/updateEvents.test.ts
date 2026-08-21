import { expect, test } from "bun:test";
import { activateUpdateJob, routeUpdateJobEvent } from "./updateEvents";

test("buffers events until the owning update job is activated", () => {
  const pending = new Map<
    string,
    Array<{ type: string; payload: Record<string, unknown> }>
  >();
  const applied: string[] = [];
  routeUpdateJobEvent(
    pending,
    undefined,
    "11",
    { type: "jobs.progress", payload: {} },
    (event) => applied.push(event.type),
  );
  routeUpdateJobEvent(
    pending,
    "12",
    "11",
    { type: "jobs.completed", payload: {} },
    (event) => applied.push(event.type),
  );
  expect(applied).toEqual([]);
  activateUpdateJob(pending, "11", (event) => applied.push(event.type));
  expect(applied).toEqual(["jobs.progress", "jobs.completed"]);
  expect(pending.size).toBe(0);
});

test("bounds unclaimed job events", () => {
  const pending = new Map<
    string,
    Array<{ type: string; payload: Record<string, unknown> }>
  >();
  for (let job = 1; job <= 8; job++)
    routeUpdateJobEvent(
      pending,
      undefined,
      String(job),
      { type: "jobs.progress", payload: {} },
      () => {},
    );
  expect([...pending.keys()]).toEqual(["5", "6", "7", "8"]);
});
