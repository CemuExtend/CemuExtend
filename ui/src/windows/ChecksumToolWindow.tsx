import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { ChecksumContent, ChecksumModel } from "../bridge/contracts";
import { subscribe } from "../bridge/events";
import { invoke } from "../bridge/native";
import { translateFormat } from "../i18n/runtime";
import { activateJob, routeJobEvent } from "./checksumEvents";

type Progress = {
  phase: string;
  filesCompleted: number;
  filesTotal: number;
  bytesCompleted: number;
  bytesTotal: number;
};
type Result = {
  titleId: string;
  version: number;
  region: number;
  imageSha256: string;
  files: Array<{ path: string; sha256: string }>;
};

export function ChecksumToolWindow({ windowId }: { windowId: string }) {
  const [model, setModel] = useState<ChecksumModel>({ entries: [] });
  const [selectedUid, setSelectedUid] = useState("");
  const [query, setQuery] = useState("");
  const [progress, setProgress] = useState<Progress>();
  const [result, setResult] = useState<Result>();
  const [error, setError] = useState("");
  const jobId = useRef<string | undefined>(undefined);
  const pendingEvents = useRef(
    new Map<
      string,
      Array<{ type: string; payload: Record<string, unknown> }>
    >(),
  );
  const applyJobEvent = useCallback(
    (type: string, payload: Record<string, unknown>) => {
      if (type === "jobs.progress")
        setProgress({
          phase: String(payload.phase ?? "working"),
          filesCompleted: Number(payload.filesCompleted ?? 0),
          filesTotal: Number(payload.filesTotal ?? 0),
          bytesCompleted: Number(payload.bytesCompleted ?? 0),
          bytesTotal: Number(payload.bytesTotal ?? 0),
        });
      else {
        jobId.current = undefined;
        setProgress(undefined);
        if (
          payload.ok === true &&
          payload.checksum &&
          typeof payload.checksum === "object"
        )
          setResult(payload.checksum as Result);
        else
          setError(String(payload.diagnostic || "Checksum operation failed"));
      }
    },
    [],
  );
  useEffect(() => {
    void invoke("checksum.getModel")
      .then((next) => {
        setModel(next);
        setSelectedUid(next.entries[0]?.locationUid ?? "");
      })
      .catch((reason: unknown) => setError(String(reason)));
    return subscribe((event) => {
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
        (queued) => applyJobEvent(queued.type, queued.payload),
      );
    });
  }, [applyJobEvent, windowId]);
  const entries = useMemo(() => {
    const needle = query.trim().toLocaleLowerCase();
    return model.entries.filter(
      (entry) =>
        !needle ||
        `${entry.name} ${entry.titleId} ${entry.type} ${entry.format}`
          .toLocaleLowerCase()
          .includes(needle),
    );
  }, [model, query]);
  const selected = model.entries.find(
    (entry) => entry.locationUid === selectedUid,
  );
  const start = async () => {
    if (!selectedUid) return;
    setError("");
    setResult(undefined);
    setProgress({
      phase: "starting",
      filesCompleted: 0,
      filesTotal: 0,
      bytesCompleted: 0,
      bytesTotal: 0,
    });
    try {
      const started = await invoke("checksum.start", {
        locationUid: selectedUid,
      });
      jobId.current = started.jobId;
      activateJob(pendingEvents.current, started.jobId, (event) =>
        applyJobEvent(event.type, event.payload),
      );
    } catch (reason) {
      setProgress(undefined);
      setError(String(reason));
    }
  };
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
  return (
    <main className="role-window manager-window">
      <header>
        <div>
          <span className="eyebrow">Content integrity</span>
          <h1>Checksum Tool</h1>
        </div>
        <div className="button-row">
          <button
            disabled={!selected || !!progress}
            onClick={() => void start()}
          >
            Generate checksum
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
      <div className="toolbar embedded">
        <input
          type="search"
          placeholder="Filter installed content"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
        />
        <span>
          {translateFormat("{count} installations", { count: entries.length })}
        </span>
      </div>
      <div className="split-view">
        <aside className="selection-list">
          {entries.map((entry: ChecksumContent) => (
            <button
              key={entry.locationUid}
              className={entry.locationUid === selectedUid ? "selected" : ""}
              disabled={!!progress}
              onClick={() => {
                setSelectedUid(entry.locationUid);
                setResult(undefined);
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
                  <dt>Region</dt>
                  <dd>{selected.region}</dd>
                </div>
                <div>
                  <dt>Content</dt>
                  <dd>
                    {selected.type} / {selected.format}
                  </dd>
                </div>
              </dl>
              {progress && (
                <>
                  <progress max={100} value={percent} />
                  <p>
                    {progress.phase}: {progress.filesCompleted}/
                    {progress.filesTotal} files ·{" "}
                    {Math.round(progress.bytesCompleted / 1048576)}/
                    {Math.round(progress.bytesTotal / 1048576)} MiB
                  </p>
                </>
              )}
              {result && (
                <>
                  <div className="notice">
                    Checksum generated for{" "}
                    {result.files.length
                      ? `${result.files.length} files`
                      : "the game image"}
                    .
                  </div>
                  {result.imageSha256 && (
                    <p>
                      <strong>Image SHA-256</strong>
                      <br />
                      <code>{result.imageSha256}</code>
                    </p>
                  )}
                  <div className="checksum-results">
                    {result.files.map((file) => (
                      <div key={file.path}>
                        <code>{file.sha256}</code>
                        <span>{file.path}</span>
                      </div>
                    ))}
                  </div>
                </>
              )}
            </>
          ) : (
            <p>No installed content was found.</p>
          )}
        </section>
      </div>
    </main>
  );
}
