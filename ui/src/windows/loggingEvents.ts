import type {
  LoggingEntry,
  LoggingSnapshot,
  NativeEvent,
} from "../bridge/contracts";

export const maximumUiEntries = 2048;
const decimalId = /^(0|[1-9][0-9]*)$/;

export function loggingSnapshotFromEvent(
  event: NativeEvent,
): LoggingSnapshot | undefined {
  if (
    event.type !== "logging.entries" ||
    !event.payload ||
    typeof event.payload !== "object"
  )
    return undefined;
  const value = event.payload as Partial<LoggingSnapshot>;
  if (
    !Array.isArray(value.entries) ||
    typeof value.nextSequence !== "string" ||
    !decimalId.test(value.nextSequence) ||
    typeof value.firstAvailableSequence !== "string" ||
    !decimalId.test(value.firstAvailableSequence) ||
    typeof value.droppedEntries !== "string" ||
    !decimalId.test(value.droppedEntries) ||
    typeof value.retainedBytes !== "string" ||
    !decimalId.test(value.retainedBytes) ||
    typeof value.truncated !== "boolean"
  )
    return undefined;
  if (
    !value.entries.every(
      (entry) =>
        entry &&
        typeof entry.sequence === "string" &&
        decimalId.test(entry.sequence) &&
        (entry.level === "info" ||
          entry.level === "warning" ||
          entry.level === "error") &&
        typeof entry.category === "string" &&
        typeof entry.message === "string",
    )
  )
    return undefined;
  return value as LoggingSnapshot;
}

export function mergeLoggingEntries(
  current: LoggingEntry[],
  incoming: LoggingEntry[],
): LoggingEntry[] {
  const bySequence = new Map<string, LoggingEntry>();
  for (const entry of current) bySequence.set(entry.sequence, entry);
  for (const entry of incoming) bySequence.set(entry.sequence, entry);
  return [...bySequence.values()]
    .sort((left, right) => {
      const lhs = BigInt(left.sequence);
      const rhs = BigInt(right.sequence);
      return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
    })
    .slice(-maximumUiEntries);
}
