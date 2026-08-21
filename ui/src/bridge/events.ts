import type { NativeEvent } from "./contracts";

let lastSequence = 0;
const listeners = new Set<(event: NativeEvent) => void>();

export function subscribe(listener: (event: NativeEvent) => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function dispatchNativeEvent(value: unknown): void {
  if (!value || typeof value !== "object") return;
  const event = value as NativeEvent;
  if (typeof event.type !== "string" || !Number.isSafeInteger(event.sequence) || event.sequence <= lastSequence) return;
  lastSequence = event.sequence;
  listeners.forEach((listener) => listener(event));
}

if (typeof window !== "undefined") {
  Object.defineProperty(window, "__cemuDispatchEvent", {
    configurable: false,
    writable: false,
    value: dispatchNativeEvent
  });
}
