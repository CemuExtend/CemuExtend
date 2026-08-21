import { expect, test } from "bun:test";
import { dispatchNativeEvent, subscribe } from "./events";

test("delivers only monotonically increasing native events", () => {
  const received: number[] = [];
  const unsubscribe = subscribe((event) => received.push(event.sequence));
  dispatchNativeEvent({ type: "test", sequence: 10, payload: {} });
  dispatchNativeEvent({ type: "test", sequence: 9, payload: {} });
  dispatchNativeEvent({ type: "test", sequence: 11, payload: {} });
  unsubscribe();
  expect(received).toEqual([10, 11]);
});
