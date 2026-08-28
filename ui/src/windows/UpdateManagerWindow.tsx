import { useCallback, useEffect, useRef, useState } from "react";
import type {
  TitleInstallSelection,
  UpdateManagerModel,
} from "../bridge/contracts";
import { subscribe } from "../bridge/events";
import { invoke } from "../bridge/native";
import {
  activateUpdateJob,
  routeUpdateJobEvent,
  type UpdateJobEvent,
} from "./updateEvents";

type Progress = { phase: string; completed: number; total: number };
type Operation = "title" | "packs";

function mib(value: number): string {
  return `${Math.ceil(value / 1048576).toLocaleString()} MiB`;
}

export function UpdateManagerWindow({ windowId }: { windowId: string }) {
  const [model, setModel] = useState<UpdateManagerModel>({
    titleRunning: false,
  });
  const [selection, setSelection] = useState<TitleInstallSelection>();
  const [operation, setOperation] = useState<Operation>();
  const [pendingRequest, setPendingRequest] = useState(false);
  const [conflictAccepted, setConflictAccepted] = useState(false);
  const [progress, setProgress] = useState<Progress>();
  const [error, setError] = useState("");
  const [message, setMessage] = useState("");
  const jobId = useRef<string | undefined>(undefined);
  const pending = useRef(new Map<string, UpdateJobEvent[]>());

  const applyJobEvent = useCallback((event: UpdateJobEvent) => {
    const payload = event.payload;
    if (event.type === "jobs.progress") {
      setProgress({
        phase: String(payload.phase ?? "working"),
        completed: Number(payload.bytesCompleted ?? payload.completed ?? 0),
        total: Number(payload.bytesTotal ?? payload.total ?? 0),
      });
      return;
    }
    jobId.current = undefined;
    setProgress(undefined);
    setOperation(undefined);
    if (payload.ok === true) {
      setMessage(
        payload.upToDate === true
          ? "Community graphic packs are already current."
          : payload.titleId
            ? "Title installed successfully."
            : "Community graphic packs updated successfully.",
      );
      setSelection(undefined);
    } else if (payload.error === "confirmationRequired") {
      setError(
        "Existing community packs must be replaced. Review and retry with replacement enabled.",
      );
    } else if (payload.error === "cancelled" || payload.error === 5)
      setMessage("Operation cancelled.");
    else setError(String(payload.diagnostic || "Update operation failed"));
  }, []);

  useEffect(() => {
    void invoke("updates.getModel")
      .then(setModel)
      .catch((reason: unknown) => setError(String(reason)));
    return subscribe((event) => {
      if (
        (event.type !== "jobs.progress" && event.type !== "jobs.completed") ||
        !event.payload ||
        typeof event.payload !== "object"
      )
        return;
      const payload = event.payload as Record<string, unknown>;
      if (
        payload.windowId !== windowId ||
        typeof payload.jobId !== "string" ||
        !/^(0|[1-9][0-9]*)$/.test(payload.jobId)
      )
        return;
      routeUpdateJobEvent(
        pending.current,
        jobId.current,
        payload.jobId,
        { type: event.type, payload },
        applyJobEvent,
      );
    });
  }, [applyJobEvent, windowId]);

  const activate = (id: string, kind: Operation) => {
    jobId.current = id;
    setOperation(kind);
    activateUpdateJob(pending.current, id, applyJobEvent);
  };
  const pick = async () => {
    setError("");
    setMessage("");
    setPendingRequest(true);
    try {
      setSelection((await invoke("updates.pickTitleSource")) ?? undefined);
      setConflictAccepted(false);
    } catch (reason) {
      setError(String(reason));
    } finally {
      setPendingRequest(false);
    }
  };
  const installTitle = async () => {
    if (!selection) return;
    if (selection.conflict !== "none" && !conflictAccepted) {
      setError("Confirm replacement of the existing title before installing.");
      return;
    }
    setError("");
    setMessage("");
    setPendingRequest(true);
    setProgress({
      phase: "starting",
      completed: 0,
      total: selection.requiredBytes,
    });
    try {
      const started = await invoke("updates.installTitle", {
        planToken: selection.planToken,
        acceptConflict: conflictAccepted,
      });
      activate(started.jobId, "title");
    } catch (reason) {
      setProgress(undefined);
      setError(String(reason));
    } finally {
      setPendingRequest(false);
    }
  };
  const updatePacks = async (replaceExisting: boolean) => {
    setError("");
    setMessage("");
    setPendingRequest(true);
    setProgress({ phase: "checking", completed: 0, total: 0 });
    try {
      const started = await invoke("graphicPacks.install", {
        kind: "community",
        replaceExisting,
      });
      activate(started.jobId, "packs");
    } catch (reason) {
      setProgress(undefined);
      setError(String(reason));
    } finally {
      setPendingRequest(false);
    }
  };
  const cancel = () => {
    if (jobId.current !== undefined)
      void invoke("jobs.cancel", { jobId: jobId.current }).catch(
        (reason: unknown) => setError(String(reason)),
      );
  };
  const percent = progress?.total
    ? Math.min(100, (progress.completed * 100) / progress.total)
    : undefined;
  const conflict =
    selection?.conflict !== undefined && selection.conflict !== "none";
  const busy = pendingRequest || operation !== undefined;

  return (
    <main className="role-window manager-window update-manager">
      <header>
        <div>
          <span className="eyebrow">Native update transactions</span>
          <h1>Updates</h1>
        </div>
        <div className="button-row">
          {operation && <button onClick={cancel}>Cancel</button>}
        </div>
      </header>
      {model.titleRunning && (
        <div className="notice" role="status">
          Stop the running title before installing titles or graphic packs.
        </div>
      )}
      {error && (
        <div className="notice error" role="alert">
          {error}
        </div>
      )}
      {message && (
        <div className="notice" role="status">
          {message}
        </div>
      )}
      <div className="update-grid">
        <section className="editor-panel">
          <h2>Install title content</h2>
          <p>
            Select a folder containing the title's <code>code</code>,{" "}
            <code>content</code>, and <code>meta</code> directories. The
            selected path remains native and is represented here by a one-use
            plan.
          </p>
          <div className="button-row">
            <button
              disabled={model.titleRunning || busy}
              onClick={() => void pick()}
            >
              Choose title folder…
            </button>
            {selection && (
              <button
                disabled={busy || (conflict && !conflictAccepted)}
                onClick={() => void installTitle()}
              >
                Install {selection.kind}
              </button>
            )}
          </div>
          {selection && (
            <dl className="detail-list compact">
              <div>
                <dt>Title</dt>
                <dd>{selection.titleName || selection.titleId}</dd>
              </div>
              <div>
                <dt>Title ID</dt>
                <dd>
                  <code>{selection.titleId}</code>
                </dd>
              </div>
              <div>
                <dt>Version / type</dt>
                <dd>
                  v{selection.version} · {selection.kind}
                </dd>
              </div>
              <div>
                <dt>Required / available</dt>
                <dd>
                  {mib(selection.requiredBytes)} /{" "}
                  {mib(selection.availableBytes)}
                </dd>
              </div>
              {conflict && (
                <div>
                  <dt>Conflict</dt>
                  <dd>
                    {selection.conflict}
                    {selection.installedVersion !== null
                      ? ` (installed v${selection.installedVersion})`
                      : ""}
                    . Installing will replace the existing title.
                    <label>
                      <input
                        type="checkbox"
                        checked={conflictAccepted}
                        onChange={(event) =>
                          setConflictAccepted(event.target.checked)
                        }
                      />{" "}
                      I understand and want to replace the installed title.
                    </label>
                  </dd>
                </div>
              )}
            </dl>
          )}
        </section>
        <section className="editor-panel">
          <h2>Community graphic packs</h2>
          <p>
            Check the HTTPS release source, download within native limits,
            safely extract, and atomically replace the downloaded pack set.
          </p>
          <div className="button-row">
            <button
              disabled={model.titleRunning || busy}
              onClick={() => void updatePacks(false)}
            >
              Check and update
            </button>
            <button
              disabled={model.titleRunning || busy}
              onClick={() => void updatePacks(true)}
            >
              Replace existing packs
            </button>
          </div>
        </section>
      </div>
      {progress && (
        <section className="operation-progress" aria-live="polite">
          <h2>
            {operation === "title"
              ? "Installing title"
              : "Updating graphic packs"}
          </h2>
          <progress max={100} value={percent} />
          <p>
            {progress.phase}
            {progress.total
              ? ` · ${mib(progress.completed)} / ${mib(progress.total)}`
              : ""}
          </p>
        </section>
      )}
    </main>
  );
}
