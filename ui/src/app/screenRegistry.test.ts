import { expect, test } from "bun:test";
import { implementedWindowRoles } from "../generated/roles";
import { SCREEN_REGISTRY } from "./screenRegistry";

test("every implemented Native window has a shared screen definition", () => {
  expect(Object.keys(SCREEN_REGISTRY).sort()).toEqual(
    [...implementedWindowRoles].sort(),
  );
});

test("only monitoring workspaces remain detachable", () => {
  for (const definition of Object.values(SCREEN_REGISTRY)) {
    if (definition.detachable) expect(definition.category).toBe("Developer");
  }
});
