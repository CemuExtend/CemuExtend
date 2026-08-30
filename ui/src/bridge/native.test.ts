import { afterEach, describe, expect, test } from "bun:test";
import { invoke, NativeRpcError } from "./native";
import { rpcWireLimits } from "../generated/protocol";

const originalWindow = globalThis.window;
const originalDateNow = Date.now;

afterEach(() => {
  Date.now = originalDateNow;
  Object.defineProperty(globalThis, "window", {
    configurable: true,
    value: originalWindow,
  });
});

function installBridge(bridge?: (request: string) => Promise<string>): void {
  Object.defineProperty(globalThis, "window", {
    configurable: true,
    value: { cemuInvoke: bridge },
  });
}

function successfulResponse(requestJson: string): string {
  const { id } = JSON.parse(requestJson) as { id: string };
  return JSON.stringify({ id, ok: true, result: {} });
}

function requestIdAfter(requestId: string): string {
  const separator = requestId.lastIndexOf("-");
  const sequence = Number.parseInt(requestId.slice(separator + 1), 36);
  return `${requestId.slice(0, separator + 1)}${(sequence + 1).toString(36)}`;
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
      return JSON.stringify({
        id: request.id,
        ok: true,
        result: { sequence: "7" },
      });
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

  test("accepts an ASCII request at the byte limit", async () => {
    Date.now = () => 1;
    const requests: string[] = [];
    installBridge(async (requestJson) => {
      requests.push(requestJson);
      return successfulResponse(requestJson);
    });

    await invoke("overlay.getSnapshot");
    const nextRequestId = requestIdAfter(
      (JSON.parse(requests[0]) as { id: string }).id,
    );
    const emptyRequest = JSON.stringify({
      id: nextRequestId,
      method: "system.openExternalUrl",
      params: { url: "" },
    });
    const url = "a".repeat(rpcWireLimits.maxRequestBytes - emptyRequest.length);

    await invoke("system.openExternalUrl", { url });

    expect(requests).toHaveLength(2);
    expect(new TextEncoder().encode(requests[1]).byteLength).toBe(
      rpcWireLimits.maxRequestBytes,
    );
  });

  test("rejects an ASCII request over the byte limit", async () => {
    let invocationCount = 0;
    installBridge(async (requestJson) => {
      invocationCount += 1;
      return successfulResponse(requestJson);
    });
    const url = "a".repeat(rpcWireLimits.maxRequestBytes);

    await expect(
      invoke("system.openExternalUrl", { url }),
    ).rejects.toMatchObject({
      code: "request_too_large",
      message: "RPC request exceeds the maximum size",
      details: undefined,
    });
    expect(invocationCount).toBe(0);
  });

  test("measures non-ASCII requests by UTF-8 bytes", async () => {
    Date.now = () => 1;
    const requests: string[] = [];
    installBridge(async (requestJson) => {
      requests.push(requestJson);
      return successfulResponse(requestJson);
    });

    await invoke("overlay.getSnapshot");
    const nextRequestId = requestIdAfter(
      (JSON.parse(requests[0]) as { id: string }).id,
    );
    const emptyRequest = JSON.stringify({
      id: nextRequestId,
      method: "system.openExternalUrl",
      params: { url: "" },
    });
    const emojiCount =
      Math.floor((rpcWireLimits.maxRequestBytes - emptyRequest.length) / 4) + 1;
    const url = "😀".repeat(emojiCount);
    const requestJson = JSON.stringify({
      id: nextRequestId,
      method: "system.openExternalUrl",
      params: { url },
    });

    expect(requestJson.length).toBeLessThan(rpcWireLimits.maxRequestBytes);
    expect(new TextEncoder().encode(requestJson).byteLength).toBeGreaterThan(
      rpcWireLimits.maxRequestBytes,
    );
    await expect(
      invoke("system.openExternalUrl", { url }),
    ).rejects.toMatchObject({
      code: "request_too_large",
      message: "RPC request exceeds the maximum size",
      details: undefined,
    });
    expect(requests).toHaveLength(1);
  });
});
