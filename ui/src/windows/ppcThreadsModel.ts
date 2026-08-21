import type { PpcThread } from "../bridge/contracts";

export const matchesPpcThread = (thread: PpcThread, query: string): boolean => {
  const needle = query.trim().toLowerCase();
  return !needle || [thread.address, thread.name, thread.state, thread.instructionPointer]
    .some((value) => value.toLowerCase().includes(needle));
};
