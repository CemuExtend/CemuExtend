import { describe, expect, test } from "bun:test";
import { implementedWindowRoles } from "../generated/roles";
import { WORKSPACE_ROLES, workspaceRouteForRole } from "./workspaceRegistry";

describe("embedded tool workspaces", () => {
  test("maps every non-modal tool exactly once into the main window", () => {
    const embedded = Object.values(WORKSPACE_ROLES).flat();
    const expected = implementedWindowRoles.filter(
      (role) => role !== "cemod-permissions",
    );
    expect([...new Set(embedded)].sort()).toEqual([...expected].sort());
    expect(embedded).toHaveLength(expected.length);
  });

  test("keeps exact package approval as the only detached modal", () => {
    expect(workspaceRouteForRole("cemod-permissions")).toBeUndefined();
  });
});
