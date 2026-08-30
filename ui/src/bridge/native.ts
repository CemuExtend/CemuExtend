import type { RpcRequest, RpcResponse } from "./contracts";
import type { RpcContract, RpcMethod } from "../generated/contracts";
import { rpcWireLimits } from "../generated/protocol";

let nextId = 0;

export class NativeRpcError extends Error {
  constructor(
    public readonly code: string,
    message: string,
    public readonly details?: unknown,
  ) {
    super(message);
    this.name = "NativeRpcError";
  }
}

export async function invoke<M extends RpcMethod>(
  method: M,
  ...args: RpcContract[M]["params"] extends undefined
    ? []
    : [params: RpcContract[M]["params"]]
): Promise<RpcContract[M]["result"]> {
  if (!window.cemuInvoke)
    throw new NativeRpcError(
      "bridge_unavailable",
      "Native bridge is unavailable",
    );
  const request: RpcRequest = {
    id: `${Date.now().toString(36)}-${(++nextId).toString(36)}`,
    method,
    params: args[0] ?? {},
  };
  const requestJson = JSON.stringify(request);
  if (
    new TextEncoder().encode(requestJson).byteLength >
    rpcWireLimits.maxRequestBytes
  )
    throw new NativeRpcError(
      "request_too_large",
      "RPC request exceeds the maximum size",
    );
  const raw: unknown = JSON.parse(await window.cemuInvoke(requestJson));
  if (
    !raw ||
    typeof raw !== "object" ||
    !("id" in raw) ||
    !("ok" in raw) ||
    typeof raw.id !== "string" ||
    typeof raw.ok !== "boolean"
  ) {
    throw new NativeRpcError(
      "malformed_response",
      "Native bridge returned a malformed RPC response",
    );
  }
  const response = raw as RpcResponse<RpcContract[M]["result"]>;
  if (response.id !== request.id)
    throw new NativeRpcError(
      "response_mismatch",
      "RPC response ID did not match request",
    );
  if (!response.ok) {
    if (
      !response.error ||
      typeof response.error.code !== "string" ||
      typeof response.error.message !== "string"
    )
      throw new NativeRpcError(
        "malformed_response",
        "Native bridge returned a malformed RPC error",
      );
    throw new NativeRpcError(
      response.error.code,
      response.error.message,
      response.error.details,
    );
  }
  return response.result;
}
