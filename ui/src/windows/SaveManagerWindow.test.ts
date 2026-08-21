import { expect, test } from "bun:test";
import { isValidSavePersistentId } from "./saveManagerModel";

test("accepts only exact supported persistent ids", () => {
  expect(isValidSavePersistentId("80000001")).toBe(true);
  expect(isValidSavePersistentId("FFFFFFFF")).toBe(true);
  expect(isValidSavePersistentId("80000000")).toBe(false);
  expect(isValidSavePersistentId("800000010")).toBe(false);
  expect(isValidSavePersistentId("../save1")).toBe(false);
});
