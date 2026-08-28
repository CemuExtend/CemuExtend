import { expect, test } from "bun:test";
import { implementedWindowRoles } from "../generated/roles";
import { SCREEN_REGISTRY } from "./screenRegistry";

test("every implemented Native window has a shared screen definition", () => {
  expect(Object.keys(SCREEN_REGISTRY).sort()).toEqual(
    [...implementedWindowRoles].sort(),
  );
});

test("screen definitions contain presentation metadata only", () => {
  for (const definition of Object.values(SCREEN_REGISTRY))
    expect(Object.keys(definition).sort()).toEqual([
      "category",
      "description",
      "title",
    ]);
});
