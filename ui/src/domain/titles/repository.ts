import type {
  Title,
  TitleLaunchResult,
  TitleLaunchState,
} from "../../bridge/contracts";
import { subscribe } from "../../bridge/events";
import { invoke } from "../../bridge/native";

export type TitlesEvent =
  | { type: "changed" }
  | { type: "launch-state"; state: TitleLaunchState }
  | { type: "diagnostic"; message: string };

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

function isLaunchState(value: unknown): value is TitleLaunchState {
  return (
    isRecord(value) &&
    typeof value.status === "string" &&
    typeof value.titleId === "string"
  );
}

export function listTitles(): Promise<Title[]> {
  return invoke("titles.list");
}

export function loadTitleIcon(
  titleId: string,
): Promise<{ titleId: string; iconDataUrl: string | null }> {
  return invoke("titles.icon", { titleId });
}

export async function refreshTitles(): Promise<Title[]> {
  await invoke("titles.refresh");
  return listTitles();
}

export function launchTitle(titleId: string): Promise<TitleLaunchResult> {
  return invoke("titles.launch", { titleId });
}

export function subscribeToTitles(
  listener: (event: TitlesEvent) => void,
): () => void {
  return subscribe((event) => {
    if (event.type === "titles.changed") {
      listener({ type: "changed" });
      return;
    }
    if (event.type === "titles.launchState" && isLaunchState(event.payload)) {
      listener({ type: "launch-state", state: event.payload });
      return;
    }
    if (
      (event.type === "system.diagnostic" ||
        event.type === "window.openFailed") &&
      isRecord(event.payload) &&
      typeof event.payload.message === "string"
    )
      listener({ type: "diagnostic", message: event.payload.message });
  });
}
