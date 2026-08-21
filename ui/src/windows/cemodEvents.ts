import type { CemodManagerSnapshot, NativeEvent } from "../bridge/contracts";

function isRecord(value: unknown): value is Record<string, unknown> { return typeof value === "object" && value !== null; }

export type CemodSnapshotCompletion = { jobId: string; ok: boolean; error: string; diagnostic: string; snapshot: CemodManagerSnapshot };

export function parseCemodSnapshot(event: NativeEvent, jobId: string): CemodSnapshotCompletion | undefined {
  if (event.type !== "cemod.snapshot" || !isRecord(event.payload) || event.payload.jobId !== jobId) return undefined;
  const value = event.payload;
  if (typeof value.ok !== "boolean" || typeof value.error !== "string" || typeof value.diagnostic !== "string" || !isRecord(value.snapshot)) return undefined;
  const snapshot = value.snapshot as CemodManagerSnapshot;
  if (typeof snapshot.generation !== "string" || !Array.isArray(snapshot.packages) || typeof snapshot.cancelled !== "boolean") return undefined;
  return { jobId, ok: value.ok, error: value.error, diagnostic: value.diagnostic, snapshot };
}

export function cemodEventJobId(event: NativeEvent): string | undefined {
  if (event.type !== "cemod.snapshot" || !isRecord(event.payload) || typeof event.payload.jobId !== "string") return undefined;
  return event.payload.jobId;
}
