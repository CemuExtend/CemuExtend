import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type {
  CemodManagerSnapshot,
  CemodPackage,
  NativeEvent,
} from "../bridge/contracts";
import { subscribe } from "../bridge/events";
import { invoke } from "../bridge/native";
import { translateFormat } from "../i18n/runtime";
import { cemodEventJobId, parseCemodSnapshot } from "./cemodEvents";

type PendingAction = {
  kind: "permissions" | "legacy";
  titleId: string;
  packageKey: string;
};

export function CemodManagerWindow({ windowId }: { windowId: string }) {
  const [model, setModel] = useState<CemodManagerSnapshot>();
  const [selectedKey, setSelectedKey] = useState("");
  const [selectedTitle, setSelectedTitle] = useState("");
  const [query, setQuery] = useState("");
  const [busyJob, setBusyJob] = useState<string>();
  const [error, setError] = useState("");
  const [message, setMessage] = useState("");
  const activeJob = useRef<string | undefined>(undefined);
  const pendingAction = useRef<PendingAction | undefined>(undefined);
  const queuedEvents = useRef(new Map<string, NativeEvent>());

  const applyCompletion = useCallback(
    async (event: NativeEvent, jobId: string) => {
      const completed = parseCemodSnapshot(event, jobId);
      if (!completed) return;
      activeJob.current = undefined;
      setBusyJob(undefined);
      if (!completed.ok || completed.snapshot.cancelled) {
        setError(
          completed.diagnostic || `Discovery failed (${completed.error})`,
        );
        pendingAction.current = undefined;
        return;
      }
      const action = pendingAction.current;
      pendingAction.current = undefined;
      if (!action) {
        setModel(completed.snapshot);
        setSelectedKey((current) =>
          completed.snapshot.packages.some(
            (item) => item.packageKey === current,
          )
            ? current
            : (completed.snapshot.packages[0]?.packageKey ?? ""),
        );
        return;
      }
      const exact = completed.snapshot.packages.find(
        (item) =>
          item.packageKey === action.packageKey &&
          item.titleIds.includes(action.titleId),
      );
      if (!exact) {
        setError("The selected package is no longer installed for this title.");
        return;
      }
      if (action.kind === "permissions") {
        await invoke("cemod.openPermissions", {
          requestId: `cemod-${Date.now().toString(36)}`,
          titleId: action.titleId,
          packageKey: exact.packageKey,
          generation: completed.snapshot.generation,
        });
      } else {
        const result = await invoke("cemod.importLegacy", {
          generation: completed.snapshot.generation,
          titleId: action.titleId,
          packageKey: exact.packageKey,
          confirmed: true,
        });
        if (!result.ok) {
          setError(result.diagnostic || `Import failed (${result.error})`);
          setModel(result.snapshot);
        } else {
          setMessage("Legacy package data imported.");
          setModel(result.snapshot);
        }
      }
    },
    [],
  );

  const activate = useCallback(
    (jobId: string) => {
      activeJob.current = jobId;
      setBusyJob(jobId);
      const queued = queuedEvents.current.get(jobId);
      queuedEvents.current.delete(jobId);
      if (queued)
        void applyCompletion(queued, jobId).catch((reason: unknown) =>
          setError(reason instanceof Error ? reason.message : String(reason)),
        );
    },
    [applyCompletion],
  );

  const startDiscovery = useCallback(
    async (titleId?: string, action?: PendingAction) => {
      setError("");
      setMessage("");
      pendingAction.current = action;
      try {
        activate(
          (await invoke("cemod.discover", titleId ? { titleId } : {})).jobId,
        );
      } catch (reason) {
        pendingAction.current = undefined;
        setError(reason instanceof Error ? reason.message : String(reason));
      }
    },
    [activate],
  );

  useEffect(() => {
    void startDiscovery();
  }, [startDiscovery]);
  useEffect(
    () =>
      subscribe((event) => {
        if (event.type === "cemod.changed") {
          if (!activeJob.current) void startDiscovery();
          return;
        }
        const eventJob = cemodEventJobId(event);
        if (!eventJob) return;
        if (eventJob !== activeJob.current) {
          queuedEvents.current.set(eventJob, event);
          return;
        }
        void applyCompletion(event, eventJob).catch((reason: unknown) =>
          setError(reason instanceof Error ? reason.message : String(reason)),
        );
      }),
    [applyCompletion, startDiscovery],
  );

  const packages = useMemo(
    () =>
      (model?.packages ?? []).filter((item) =>
        `${item.pluginName} ${item.author} ${item.modId} ${item.packageDigest}`
          .toLowerCase()
          .includes(query.toLowerCase()),
      ),
    [model, query],
  );
  const selected = model?.packages.find(
    (item) => item.packageKey === selectedKey,
  );
  useEffect(() => {
    setSelectedTitle((current) =>
      selected?.titleIds.includes(current)
        ? current
        : (selected?.titleIds[0] ?? ""),
    );
  }, [selected]);

  function exactAction(kind: PendingAction["kind"], item: CemodPackage) {
    if (!selectedTitle || !item.titleIds.includes(selectedTitle)) {
      setError("Choose the title whose approval should be changed.");
      return;
    }
    if (
      kind === "legacy" &&
      !window.confirm(
        `Import legacy CemuMod package data for “${item.pluginName}” and title ${selectedTitle}? This changes files for that title.`,
      )
    )
      return;
    void startDiscovery(selectedTitle, {
      kind,
      titleId: selectedTitle,
      packageKey: item.packageKey,
    });
  }

  return (
    <main
      className="role-window manager-window cemod-window"
      data-window-id={windowId}
    >
      <header>
        <div>
          <span className="eyebrow">Exact package approvals</span>
          <h1>CemuMod Manager</h1>
        </div>
        <div className="button-row">
          <button
            disabled={busyJob !== undefined}
            onClick={() => void startDiscovery()}
          >
            Refresh
          </button>
          {busyJob !== undefined && (
            <button
              onClick={() => void invoke("jobs.cancel", { jobId: busyJob })}
            >
              Cancel
            </button>
          )}
          <button onClick={() => void invoke("window.close")}>Close</button>
        </div>
      </header>
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
      <div className="toolbar embedded">
        <input
          type="search"
          placeholder="Filter packages or digests"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
        />
        <span>
          {model
            ? translateFormat("{count} packages · generation {generation}", {
                count: model.packages.length,
                generation: model.generation,
              })
            : busyJob !== undefined
              ? "Inspecting installed packages…"
              : "No snapshot"}
        </span>
      </div>
      <div className="split-view">
        <div className="selection-list" aria-busy={busyJob !== undefined}>
          {packages.map((item) => (
            <button
              key={item.packageKey}
              className={selectedKey === item.packageKey ? "selected" : ""}
              onClick={() => setSelectedKey(item.packageKey)}
            >
              <strong>
                {item.pluginName || item.modId || "Unnamed package"}
              </strong>
              <span>
                {item.status} · {item.author || "Unknown author"}
              </span>
              <code>{item.packageDigest}</code>
            </button>
          ))}
          {model && packages.length === 0 && (
            <div className="empty">No CemuMod packages found.</div>
          )}
        </div>
        <section className="editor-panel">
          {selected ? (
            <>
              <div className="pack-heading">
                <div>
                  <h2>{selected.pluginName || selected.modId}</h2>
                  <p>{selected.description || "No package description."}</p>
                </div>
                <span
                  className={`status-pill ${selected.approved ? "approved" : "pending"}`}
                >
                  {selected.approved ? "Approved" : "Approval required"}
                </span>
              </div>
              {!selected.runtimeAvailable && (
                <div className="warning">
                  The CemuMod runtime is unavailable in this build. Approval can
                  be reviewed and stored, but this package cannot run.
                </div>
              )}
              {selected.headless && (
                <div className="warning">
                  This package targets a headless runtime. No interactive
                  runtime is available for it.
                </div>
              )}
              {!selected.valid && (
                <div className="error">
                  Package inspection failed or the manifest is invalid. Approval
                  is disabled.
                </div>
              )}
              {selected.warnings.map((warning) => (
                <div className="warning" key={warning}>
                  {warning}
                </div>
              ))}
              <dl className="detail-list compact">
                <div>
                  <dt>Exact package digest</dt>
                  <dd>
                    <code className="digest">{selected.packageDigest}</code>
                  </dd>
                </div>
                <div>
                  <dt>Identity</dt>
                  <dd>
                    {selected.modIdentity || selected.principal || "Unverified"}
                  </dd>
                </div>
                <div>
                  <dt>Scope</dt>
                  <dd>{selected.scope}</dd>
                </div>
                <div>
                  <dt>Target title</dt>
                  <dd>
                    <select
                      aria-label="Approval title"
                      value={selectedTitle}
                      onChange={(event) => setSelectedTitle(event.target.value)}
                    >
                      {selected.titleIds.map((titleId) => (
                        <option key={titleId} value={titleId}>
                          {titleId}
                        </option>
                      ))}
                    </select>
                  </dd>
                </div>
                <div>
                  <dt>Permissions</dt>
                  <dd>
                    {
                      selected.permissions.filter((item) => item.requested)
                        .length
                    }{" "}
                    requested / 11 defined
                  </dd>
                </div>
              </dl>
              <div className="actions">
                <button
                  disabled={
                    !selected.valid || !selectedTitle || busyJob !== undefined
                  }
                  onClick={() => exactAction("permissions", selected)}
                >
                  Review exact permissions
                </button>
                <button
                  disabled={
                    !selected.valid || !selectedTitle || busyJob !== undefined
                  }
                  onClick={() => exactAction("legacy", selected)}
                >
                  Import legacy data…
                </button>
              </div>
            </>
          ) : (
            <div className="empty">
              Select a package to inspect its exact digest and permissions.
            </div>
          )}
        </section>
      </div>
    </main>
  );
}
