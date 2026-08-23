import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type {
  ManagedContentDeletePlanView,
  TitleInstallPlanView,
  TitleManagerEntry,
  TitleManagerModel,
  WuaConversionPlanView,
} from "../bridge/contracts";
import { subscribe } from "../bridge/events";
import { invoke } from "../bridge/native";
import { translate, translateFormat } from "../i18n/runtime";
import { Modal } from "../components/Modal";
import { activateJob, routeJobEvent } from "./checksumEvents";

type JobEvent = { type: string; payload: Record<string, unknown> };
type Progress = {
  operation: string;
  phase: string;
  filesCompleted: number;
  filesTotal: number;
  bytesCompleted: number;
  bytesTotal: number;
  currentPath: string;
};
type Confirmation =
  | { kind: "install"; plan: TitleInstallPlanView }
  | { kind: "wua"; plan: WuaConversionPlanView }
  | { kind: "delete"; plan: ManagedContentDeletePlanView };

const mib = (bytes: number) => Math.round(bytes / 1048576);

export function TitleManagerWindow({ windowId }: { windowId: string }) {
  const [model, setModel] = useState<TitleManagerModel>({
    scanning: false,
    entries: [],
  });
  const [selectedUid, setSelectedUid] = useState("");
  const [query, setQuery] = useState("");
  const [kind, setKind] = useState("all");
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [progress, setProgress] = useState<Progress>();
  const [confirmation, setConfirmation] = useState<Confirmation>();
  const jobId = useRef<string | undefined>(undefined);
  const pendingEvents = useRef(new Map<string, JobEvent[]>());
  const loadSequence = useRef(0);
  const load = useCallback(async () => {
    const sequence = ++loadSequence.current;
    try {
      const next = await invoke("titleManager.getModel");
      if (sequence !== loadSequence.current) return;
      setModel(next);
      setSelectedUid((current) =>
        next.entries.some((entry) => entry.locationUid === current)
          ? current
          : (next.entries[0]?.locationUid ?? ""),
      );
    } catch (reason) {
      if (sequence === loadSequence.current) setError(String(reason));
    }
  }, []);
  const applyJobEvent = useCallback(
    (event: JobEvent) => {
      const payload = event.payload;
      if (event.type === "jobs.progress") {
        setProgress({
          operation: String(payload.operation ?? "checksum"),
          phase: String(payload.phase ?? "working"),
          filesCompleted: Number(payload.filesCompleted ?? 0),
          filesTotal: Number(payload.filesTotal ?? 0),
          bytesCompleted: Number(payload.bytesCompleted ?? 0),
          bytesTotal: Number(payload.bytesTotal ?? 0),
          currentPath: String(payload.currentPath ?? ""),
        });
        return;
      }
      jobId.current = undefined;
      setProgress(undefined);
      if (payload.ok === true) {
        if (payload.operation === "wuaConversion")
          setNotice("WUA archive created successfully.");
        else if (payload.operation === "titleInstall")
          setNotice(
            payload.diagnostic
              ? `Title installed. ${String(payload.diagnostic)}`
              : "Title installed successfully.",
          );
        else if (payload.checksum && typeof payload.checksum === "object") {
          const result = payload.checksum as {
            files?: unknown[];
            imageSha256?: string;
          };
          setNotice(
            result.imageSha256
              ? `Image SHA-256: ${result.imageSha256}`
              : `${result.files?.length ?? 0} files verified`,
          );
        }
        void load();
      } else setError(String(payload.diagnostic || "Title operation failed"));
    },
    [load],
  );
  useEffect(() => {
    void load();
    return subscribe((event) => {
      if (event.type === "titles.changed") {
        void load();
        return;
      }
      if (
        (event.type !== "jobs.progress" && event.type !== "jobs.completed") ||
        !event.payload ||
        typeof event.payload !== "object"
      )
        return;
      const payload = event.payload as Record<string, unknown>;
      if (payload.windowId !== windowId || typeof payload.jobId !== "string")
        return;
      routeJobEvent(
        pendingEvents.current,
        jobId.current,
        payload.jobId,
        { type: event.type, payload },
        applyJobEvent,
      );
    });
  }, [applyJobEvent, load, windowId]);
  const entries = useMemo(() => {
    const needle = query.trim().toLocaleLowerCase();
    return model.entries.filter(
      (entry) =>
        (kind === "all" || entry.type === kind) &&
        (!needle ||
          `${entry.name} ${entry.titleId} ${entry.path} ${entry.region}`
            .toLocaleLowerCase()
            .includes(needle)),
    );
  }, [kind, model.entries, query]);
  const selected = model.entries.find(
    (entry) => entry.locationUid === selectedUid,
  );
  const beginJob = (started: { jobId: string }, operation: string) => {
    jobId.current = started.jobId;
    setProgress({
      operation,
      phase: "starting",
      filesCompleted: 0,
      filesTotal: 0,
      bytesCompleted: 0,
      bytesTotal: 0,
      currentPath: "",
    });
    activateJob(pendingEvents.current, started.jobId, applyJobEvent);
  };
  const attempt = async (action: () => Promise<void>) => {
    setError("");
    setNotice("");
    try {
      await action();
    } catch (reason) {
      setProgress(undefined);
      setError(String(reason));
    }
  };
  const refresh = () =>
    attempt(async () => {
      await invoke("titleManager.refresh");
      await load();
    });
  const launch = (entry: TitleManagerEntry) =>
    attempt(async () => {
      await invoke("titleManager.launch", { locationUid: entry.locationUid });
      await invoke("window.close");
    });
  const verify = (entry: TitleManagerEntry) =>
    attempt(async () =>
      beginJob(
        await invoke("checksum.start", { locationUid: entry.locationUid }),
        "checksum",
      ),
    );
  const planInstall = () =>
    attempt(async () => {
      const source = await invoke("titleManager.pickInstallSource");
      if (!source.cancelled)
        setConfirmation({
          kind: "install",
          plan: await invoke("titleManager.planInstall", {
            sourceToken: source.sourceToken,
          }),
        });
    });
  const planWua = (entry: TitleManagerEntry) =>
    attempt(async () =>
      setConfirmation({
        kind: "wua",
        plan: await invoke("titleManager.planWua", {
          locationUid: entry.locationUid,
        }),
      }),
    );
  const planDelete = (entry: TitleManagerEntry) =>
    attempt(async () =>
      setConfirmation({
        kind: "delete",
        plan: await invoke("titleManager.planDelete", {
          locationUid: entry.locationUid,
        }),
      }),
    );
  const confirmOperation = () =>
    attempt(async () => {
      const current = confirmation;
      if (!current) return;
      setConfirmation(undefined);
      if (current.kind === "install") {
        const decision =
          current.plan.conflict === "none" ? "proceed" : "acceptConflict";
        beginJob(
          await invoke("titleManager.startInstall", {
            planToken: current.plan.planToken,
            decision,
          }),
          "titleInstall",
        );
      } else if (current.kind === "wua") {
        const destination = await invoke("titleManager.pickWuaDestination", {
          suggestedFileName: current.plan.suggestedFileName,
        });
        if (!destination.cancelled)
          beginJob(
            await invoke("titleManager.startWua", {
              planToken: current.plan.planToken,
              destinationToken: destination.destinationToken,
            }),
            "wuaConversion",
          );
      } else {
        await invoke("titleManager.delete", {
          planToken: current.plan.planToken,
        });
        setNotice("Managed content deleted.");
        await load();
      }
    });
  const cancel = () => {
    if (jobId.current !== undefined)
      void invoke("jobs.cancel", { jobId: jobId.current }).catch(
        (reason: unknown) => setError(String(reason)),
      );
  };
  const percent = progress
    ? progress.bytesTotal
      ? (progress.bytesCompleted * 100) / progress.bytesTotal
      : progress.filesTotal
        ? (progress.filesCompleted * 100) / progress.filesTotal
        : 0
    : 0;
  const busy = !!progress || !!confirmation;
  return (
    <main className="role-window manager-window">
      <header>
        <div>
          <span className="eyebrow">Installed content</span>
          <h1>Title Manager</h1>
        </div>
        <div className="button-row">
          <button
            disabled={model.scanning || busy}
            onClick={() => void refresh()}
          >
            Refresh
          </button>
          <button disabled={busy} onClick={() => void planInstall()}>
            Install title…
          </button>
          {progress && <button onClick={cancel}>Cancel</button>}
          <button onClick={() => void invoke("window.close")}>Close</button>
        </div>
      </header>
      {error && (
        <div className="notice error" role="alert">
          {error}
        </div>
      )}
      {notice && (
        <div className="notice" role="status">
          {notice}
        </div>
      )}
      <div className="toolbar embedded">
        <input
          type="search"
          placeholder="Filter titles, IDs, or paths"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
        />
        <select value={kind} onChange={(event) => setKind(event.target.value)}>
          <option value="all">All content</option>
          <option value="base">Base</option>
          <option value="update">Updates</option>
          <option value="dlc">DLC</option>
          <option value="system">System</option>
        </select>
        <span>
          {translateFormat("{count} installations", { count: entries.length })}
          {model.scanning ? ` · ${translate("Scanning…")}` : ""}
        </span>
      </div>
      <div className="split-view">
        <aside className="selection-list">
          {entries.map((entry) => (
            <button
              key={entry.locationUid}
              className={entry.locationUid === selectedUid ? "selected" : ""}
              disabled={busy}
              onClick={() => {
                setSelectedUid(entry.locationUid);
                setNotice("");
              }}
            >
              <strong>{entry.name || entry.titleId}</strong>
              <span>
                {entry.type} · {entry.format} · v{entry.version}
              </span>
              <code>{entry.titleId}</code>
            </button>
          ))}
        </aside>
        <section className="editor-panel">
          {selected ? (
            <>
              <h2>{selected.name || selected.titleId}</h2>
              <dl className="detail-list compact">
                <div>
                  <dt>Title ID</dt>
                  <dd>
                    <code>{selected.titleId}</code>
                  </dd>
                </div>
                <div>
                  <dt>Installed path</dt>
                  <dd>
                    <code>{selected.path}</code>
                  </dd>
                </div>
                <div>
                  <dt>Region / version</dt>
                  <dd>
                    {selected.region || "Unknown"} · v{selected.version}
                  </dd>
                </div>
                <div>
                  <dt>Content</dt>
                  <dd>
                    {selected.type} / {selected.format}
                  </dd>
                </div>
              </dl>
              <div className="button-row">
                {selected.canLaunch && (
                  <button disabled={busy} onClick={() => void launch(selected)}>
                    Launch title
                  </button>
                )}
                <button
                  disabled={!selected.canVerify || busy}
                  onClick={() => void verify(selected)}
                >
                  Verify integrity
                </button>
                <button
                  disabled={!selected.canConvert || busy}
                  onClick={() => void planWua(selected)}
                >
                  Convert to WUA…
                </button>
                <button
                  className="danger"
                  disabled={!selected.canDelete || busy}
                  onClick={() => void planDelete(selected)}
                >
                  Delete managed content…
                </button>
              </div>
              {progress && (
                <>
                  <progress max={100} value={percent} />
                  <p className="muted">
                    {progress.operation} · {progress.phase}:{" "}
                    {progress.filesTotal
                      ? `${progress.filesCompleted}/${progress.filesTotal} files · `
                      : ""}
                    {mib(progress.bytesCompleted)}/{mib(progress.bytesTotal)}{" "}
                    MiB
                    {progress.currentPath ? ` · ${progress.currentPath}` : ""}
                  </p>
                </>
              )}
            </>
          ) : (
            <p>No managed content matches the filter.</p>
          )}
        </section>
      </div>
      {confirmation && (
        <Modal
          title={
            confirmation.kind === "install"
              ? "Confirm title installation"
              : confirmation.kind === "wua"
                ? "Confirm WUA conversion"
                : "Permanently delete managed content?"
          }
          onClose={() => setConfirmation(undefined)}
        >
          <div className={confirmation.kind === "delete" ? "warning" : "card"}>
            {confirmation.kind === "install" ? (
              <>
                <p>
                  <strong>
                    {confirmation.plan.titleName || confirmation.plan.titleId}
                  </strong>{" "}
                  · {confirmation.plan.kind} v{confirmation.plan.version}
                </p>
                <p>
                  {mib(confirmation.plan.requiredBytes)} MiB required ·{" "}
                  {mib(confirmation.plan.availableBytes)} MiB available
                </p>
                {confirmation.plan.conflict !== "none" && (
                  <p className="warning">
                    Existing installation conflict: {confirmation.plan.conflict}
                    . Continuing will replace it transactionally.
                  </p>
                )}
              </>
            ) : confirmation.kind === "wua" ? (
              <>
                <p>
                  The current plan will be revalidated immediately before
                  conversion.
                </p>
                <ul>
                  {confirmation.plan.items.map((item) => (
                    <li key={`${item.role}-${item.titleId}`}>
                      {item.role}: {item.titleId} v{item.version}
                      <br />
                      <code>{item.displayPath}</code>
                    </li>
                  ))}
                </ul>
              </>
            ) : (
              <>
                <p>
                  <strong>
                    {confirmation.plan.name || confirmation.plan.titleId}
                  </strong>
                </p>
                <p>
                  <code>{confirmation.plan.displayPath}</code>
                </p>
                <p>
                  This cannot be undone. The catalog identity and filesystem
                  fingerprint will be revalidated before deletion.
                </p>
              </>
            )}
          </div>
          <div className="actions">
            <button onClick={() => setConfirmation(undefined)}>Cancel</button>
            <button
              className={confirmation.kind === "delete" ? "danger" : "primary"}
              onClick={() => void confirmOperation()}
            >
              {confirmation.kind === "delete"
                ? "Delete permanently"
                : "Continue"}
            </button>
          </div>
        </Modal>
      )}
    </main>
  );
}
