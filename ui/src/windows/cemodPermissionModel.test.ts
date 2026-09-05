import { describe, expect, test } from "bun:test";
import type { CemodPermission } from "../bridge/contracts";
import { isCemodPermissionModelValid } from "./cemodPermissionModel";

function permission(bit: string, requested: boolean): CemodPermission {
  return {
    name: `Permission ${bit}`,
    bit,
    requested,
    granted: false,
    dangerous: false,
    manifestMismatch: false,
  };
}

describe("isCemodPermissionModelValid", () => {
  test("accepts an older host model when it covers the complete request", () => {
    expect(
      isCemodPermissionModelValid({
        permissions: [permission("1", true), permission("2", false)],
        requestedPermissions: "1",
      }),
    ).toBe(true);
  });

  test("rejects a model that omits a requested permission", () => {
    expect(
      isCemodPermissionModelValid({
        permissions: [permission("1", true)],
        requestedPermissions: "3",
      }),
    ).toBe(false);
  });

  test("rejects a requested flag that disagrees with the request mask", () => {
    expect(
      isCemodPermissionModelValid({
        permissions: [permission("1", false)],
        requestedPermissions: "1",
      }),
    ).toBe(false);
  });

  test("rejects duplicate, non-power-of-two, and malformed bits", () => {
    expect(
      isCemodPermissionModelValid({
        permissions: [permission("1", true), permission("1", false)],
        requestedPermissions: "1",
      }),
    ).toBe(false);
    expect(
      isCemodPermissionModelValid({
        permissions: [permission("3", true)],
        requestedPermissions: "3",
      }),
    ).toBe(false);
    expect(
      isCemodPermissionModelValid({
        permissions: [permission("not-a-number", true)],
        requestedPermissions: "1",
      }),
    ).toBe(false);
  });
});
