import { afterEach, describe, expect, test } from "bun:test";
import { invoke, NativeRpcError } from "./native";

const originalWindow = globalThis.window;

afterEach(() => {
  Object.defineProperty(globalThis, "window", {
    configurable: true,
    value: originalWindow,
  });
});

function installBridge(
  bridge?: (request: string) => Promise<string>,
): void {
  Object.defineProperty(globalThis, "window", {
    configurable: true,
    value: { cemuInvoke: bridge },
  });
}

describe("production native bridge", () => {
  test("fails explicitly when no native bridge was injected", async () => {
    installBridge();
    try {
      await invoke("overlay.getSnapshot");
      throw new Error("invoke unexpectedly succeeded");
    } catch (error) {
      expect(error).toBeInstanceOf(NativeRpcError);
      expect((error as NativeRpcError).code).toBe("bridge_unavailable");
    }
  });

  test("preserves and validates the RPC response ID", async () => {
    installBridge(async (raw) => {
      const request = JSON.parse(raw) as { id: string; method: string };
      expect(request.method).toBe("overlay.getSnapshot");
      return JSON.stringify({ id: request.id, ok: true, result: { sequence: "7" } });
    });
    const result = await invoke("overlay.getSnapshot");
    expect(result.sequence).toBe("7");

    installBridge(async () =>
      JSON.stringify({ id: "wrong", ok: true, result: {} }),
    );
    await expect(invoke("overlay.getSnapshot")).rejects.toMatchObject({
      code: "response_mismatch",
    });
  });
});
