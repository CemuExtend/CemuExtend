import type { NativeEvent } from "./contracts";

let lastSequence = 0n;
const listeners = new Set<(event: NativeEvent) => void>();

export function subscribe(listener: (event: NativeEvent) => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function dispatchNativeEvent(value: unknown): void {
  if (!value || typeof value !== "object") return;
  const event = value as NativeEvent;
  if (typeof event.type !== "string" || typeof event.sequence !== "string" || !/^[0-9]+$/.test(event.sequence)) return;
  const sequence = BigInt(event.sequence);
  if (sequence <= lastSequence) return;
  lastSequence = sequence;
  listeners.forEach((listener) => listener(event));
}

if (typeof window !== "undefined") {
  Object.defineProperty(window, "__cemuDispatchEvent", {
    configurable: false,
    writable: false,
    value: dispatchNativeEvent
  });
}
