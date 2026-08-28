import { expect, test } from "bun:test";
import type { PpcThread } from "../bridge/contracts";
import { matchesPpcThread } from "./ppcThreadsModel";

const thread: PpcThread = {
  address: "1000ABCD",
  identity: "1234567890ABCDEF",
  entryPoint: "02000000",
  stackLow: "10010000",
  stackHigh: "10020000",
  instructionPointer: "0300BEEF",
  linkRegister: "03000000",
  state: "waiting",
  requestedAffinity: 7,
  effectiveAffinity: 3,
  basePriority: 12,
  effectivePriority: 10,
  wakeUpTime: "1",
  totalCycles: "2",
  name: "GX2 worker",
  gpr: ["00000001", "00000002", "00000003", "00000004", "00000005"],
  cancelRequested: false,
  suspensionOwnedByFacade: false,
};

test("PPC thread filter covers copied identity and execution fields", () => {
  expect(matchesPpcThread(thread, "gx2")).toBe(true);
  expect(matchesPpcThread(thread, "abcd")).toBe(true);
  expect(matchesPpcThread(thread, "waiting")).toBe(true);
  expect(matchesPpcThread(thread, "beef")).toBe(true);
  expect(matchesPpcThread(thread, "audio")).toBe(false);
});
