import type { Bootstrap } from "../../bridge/contracts";
import { invoke } from "../../bridge/native";

export function loadBootstrap(): Promise<Bootstrap> {
  return invoke("system.bootstrap");
}
