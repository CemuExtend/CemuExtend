import type { ImplementedToolWindowRole, WindowOpenRequest } from "./contracts";
import { subscribe } from "./events";
import { invoke, NativeRpcError } from "./native";

type ToolRole = ImplementedToolWindowRole;
let nextOpenRequest = 0;

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

export type WindowOpenCompletion = { windowId: string } | { error: string };

export function matchWindowOpenEvent(
  type: string,
  payload: unknown,
  requestId: string,
): WindowOpenCompletion | undefined {
  if (
    (type !== "window.opened" && type !== "window.openFailed") ||
    !isRecord(payload) ||
    payload.requestId !== requestId
  )
    return undefined;
  if (
    type === "window.opened" &&
    typeof payload.windowId === "string" &&
    /^[0-9]+$/.test(payload.windowId)
  )
    return { windowId: payload.windowId };
  return {
    error:
      typeof payload.message === "string"
        ? payload.message
        : "The native window could not be opened",
  };
}

export function openWindow(
  role: "graphic-packs",
  context?: { titleId?: string },
): Promise<string>;
export function openWindow(
  role: Exclude<ToolRole, "graphic-packs">,
): Promise<string>;
export function openWindow(
  role: ToolRole,
  context?: { titleId?: string },
): Promise<string> {
  const requestId = `open-${Date.now().toString(36)}-${(++nextOpenRequest).toString(36)}`;
  return new Promise<string>((resolve, reject) => {
    let settled = false;
    const finish = (result: { windowId: string } | { error: Error }) => {
      if (settled) return;
      settled = true;
      unsubscribe();
      window.clearTimeout(timeout);
      if ("error" in result) reject(result.error);
      else resolve(result.windowId);
    };
    const unsubscribe = subscribe((event) => {
      const completion = matchWindowOpenEvent(
        event.type,
        event.payload,
        requestId,
      );
      if (!completion) return;
      if ("windowId" in completion) finish({ windowId: completion.windowId });
      else
        finish({
          error: new NativeRpcError("window_open_failed", completion.error),
        });
    });
    const timeout = window.setTimeout(
      () =>
        finish({
          error: new NativeRpcError(
            "window_open_timeout",
            `Timed out while opening ${role}`,
          ),
        }),
      15_000,
    );
    const params: WindowOpenRequest =
      role === "graphic-packs"
        ? { role, requestId, context }
        : { role: role as Exclude<ToolRole, "graphic-packs">, requestId };
    void invoke("window.open", params).catch((reason: unknown) =>
      finish({
        error: reason instanceof Error ? reason : new Error(String(reason)),
      }),
    );
  });
}
