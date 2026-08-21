import { expect, test } from "bun:test";
import { matchWindowOpenEvent } from "./windows";

test("correlates asynchronous native window completion", () => {
  expect(matchWindowOpenEvent("window.opened", { requestId: "other", windowId: "3" }, "wanted")).toBeUndefined();
  expect(matchWindowOpenEvent("window.opened", { requestId: "wanted", windowId: "7" }, "wanted")).toEqual({ windowId: "7" });
  expect(matchWindowOpenEvent("window.openFailed", { requestId: "wanted", message: "modal blocked" }, "wanted")).toEqual({ error: "modal blocked" });
  expect(matchWindowOpenEvent("window.opened", { requestId: "wanted", windowId: "1.5" }, "wanted")).toEqual({ error: "The native window could not be opened" });
});
