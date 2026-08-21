import type { LoggingEntry, LoggingSnapshot, NativeEvent } from "../bridge/contracts";

export const maximumUiEntries = 2048;

export function loggingSnapshotFromEvent(event: NativeEvent): LoggingSnapshot | undefined {
  if (event.type !== "logging.entries" || !event.payload || typeof event.payload !== "object") return undefined;
  const value = event.payload as Partial<LoggingSnapshot>;
  if (!Array.isArray(value.entries) || !Number.isSafeInteger(value.nextSequence) ||
    !Number.isSafeInteger(value.firstAvailableSequence) || !Number.isSafeInteger(value.droppedEntries) ||
    !Number.isSafeInteger(value.retainedBytes) || typeof value.truncated !== "boolean") return undefined;
  if (!value.entries.every((entry) => entry && Number.isSafeInteger(entry.sequence) &&
    (entry.level === "info" || entry.level === "warning" || entry.level === "error") &&
    typeof entry.category === "string" && typeof entry.message === "string")) return undefined;
  return value as LoggingSnapshot;
}

export function mergeLoggingEntries(current: LoggingEntry[], incoming: LoggingEntry[]): LoggingEntry[] {
  const bySequence = new Map<number, LoggingEntry>();
  for (const entry of current) bySequence.set(entry.sequence, entry);
  for (const entry of incoming) bySequence.set(entry.sequence, entry);
  return [...bySequence.values()].sort((left, right) => left.sequence - right.sequence).slice(-maximumUiEntries);
}
