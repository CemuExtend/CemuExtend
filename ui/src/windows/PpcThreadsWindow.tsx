import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { PpcThreadCommand, PpcThreadsModel } from "../bridge/contracts";
import { invoke } from "../bridge/native";
import { matchesPpcThread } from "./ppcThreadsModel";

const affinity = (value: number) =>
  [0, 1, 2].map((bit) => (value & (1 << bit) ? "1" : "0")).join("");

export function PpcThreadsWindow() {
  const [model, setModel] = useState<PpcThreadsModel>();
  const [selectedAddress, setSelectedAddress] = useState("");
  const [query, setQuery] = useState("");
  const [autoRefresh, setAutoRefresh] = useState(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const refreshSequence = useRef(0);

  const refresh = useCallback(async () => {
    const sequence = ++refreshSequence.current;
    try {
      const next = await invoke("diagnostics.ppcThreadsSnapshot");
      if (sequence !== refreshSequence.current) return;
      setModel(next);
      setSelectedAddress((current) =>
        next.threads.some((thread) => thread.address === current)
          ? current
          : (next.threads[0]?.address ?? ""),
      );
      setError("");
    } catch (reason) {
      if (sequence !== refreshSequence.current) return;
      setError(String(reason));
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);
  useEffect(() => {
    if (!autoRefresh) return;
    const timer = window.setInterval(() => {
      if (!busy) void refresh();
    }, 500);
    return () => window.clearInterval(timer);
  }, [autoRefresh, busy, refresh]);

  const visible = useMemo(
    () =>
      (model?.threads ?? []).filter((thread) =>
        matchesPpcThread(thread, query),
      ),
    [model, query],
  );
  const selected = model?.threads.find(
    (thread) => thread.address === selectedAddress,
  );

  const execute = async (command: PpcThreadCommand) => {
    if (!model || !selected) return;
    setBusy(true);
    try {
      const result = await invoke("diagnostics.ppcThreadCommand", {
        generation: model.generation,
        threadAddress: selected.address,
        threadIdentity: selected.identity,
        command,
      });
      setError(
        result.applied
          ? ""
          : result.diagnostic || "The thread command was not applied.",
      );
      await refresh();
    } catch (reason) {
      setError(String(reason));
      await refresh();
    } finally {
      setBusy(false);
    }
  };

  return (
    <main className="role-window manager-window ppc-threads-window">
      <header>
        <div>
          <span className="eyebrow">Live diagnostics</span>
          <h1>PPC Threads</h1>
        </div>
        <div className="button-row">
          <button disabled={busy} onClick={() => void refresh()}>
            Refresh
          </button>
          <label className="checkbox-label">
            <input
              type="checkbox"
              checked={autoRefresh}
              onChange={(event) => setAutoRefresh(event.target.checked)}
            />{" "}
            Auto refresh
          </label>
          <button onClick={() => void invoke("window.close")}>Close</button>
        </div>
      </header>
      {error && (
        <div className="notice error" role="alert">
          {error}
        </div>
      )}
      {model && !model.available && (
        <div className="notice" role="status">
          {model.diagnostic || "Start a title to inspect PPC threads."}
        </div>
      )}
      <div className="toolbar embedded">
        <input
          type="search"
          placeholder="Filter address, name, state, or PC"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
        />
        <span>{visible.length} threads</span>
      </div>
      <div className="split-view ppc-thread-split">
        <div className="table-scroll">
          <table className="diagnostic-table">
            <thead>
              <tr>
                <th>Address</th>
                <th>Name</th>
                <th>State</th>
                <th>PC</th>
                <th>Priority</th>
                <th>Affinity</th>
                <th>Cycles</th>
              </tr>
            </thead>
            <tbody>
              {visible.map((thread) => (
                <tr
                  key={thread.address}
                  className={
                    thread.address === selectedAddress ? "selected" : ""
                  }
                  onClick={() => setSelectedAddress(thread.address)}
                >
                  <td>
                    <code>{thread.address}</code>
                  </td>
                  <td>{thread.name || "(unnamed)"}</td>
                  <td>{thread.state}</td>
                  <td>
                    <code>{thread.instructionPointer}</code>
                  </td>
                  <td>{thread.effectivePriority}</td>
                  <td>{affinity(thread.effectiveAffinity)}</td>
                  <td>{thread.totalCycles}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <section className="editor-panel">
          {selected ? (
            <>
              <h2>{selected.name || `Thread ${selected.address}`}</h2>
              <div className="button-row diagnostic-actions">
                <button
                  disabled={busy || selected.state === "suspended"}
                  onClick={() => void execute("suspend")}
                >
                  Suspend
                </button>
                <button
                  disabled={busy || !selected.suspensionOwnedByFacade}
                  onClick={() => void execute("resume")}
                >
                  Resume
                </button>
                <button disabled={busy} onClick={() => void execute("boost1")}>
                  Priority −1
                </button>
                <button
                  disabled={busy}
                  onClick={() => void execute("decrease1")}
                >
                  Priority +1
                </button>
              </div>
              <dl className="detail-list compact">
                <div>
                  <dt>Address / entry</dt>
                  <dd>
                    <code>
                      {selected.address} / {selected.entryPoint}
                    </code>
                  </dd>
                </div>
                <div>
                  <dt>Stack</dt>
                  <dd>
                    <code>
                      {selected.stackLow} – {selected.stackHigh}
                    </code>
                  </dd>
                </div>
                <div>
                  <dt>PC / LR</dt>
                  <dd>
                    <code>
                      {selected.instructionPointer} / {selected.linkRegister}
                    </code>
                  </dd>
                </div>
                <div>
                  <dt>Priority</dt>
                  <dd>
                    {selected.basePriority} base / {selected.effectivePriority}{" "}
                    effective
                  </dd>
                </div>
                <div>
                  <dt>Affinity</dt>
                  <dd>
                    {affinity(selected.requestedAffinity)} requested /{" "}
                    {affinity(selected.effectiveAffinity)} effective
                  </dd>
                </div>
                <div>
                  <dt>GPR r3–r7</dt>
                  <dd>
                    <code>{selected.gpr.join(" · ")}</code>
                  </dd>
                </div>
                <div>
                  <dt>Wake / cycles</dt>
                  <dd>
                    {selected.wakeUpTime} / {selected.totalCycles}
                  </dd>
                </div>
                {selected.waitingMutex && (
                  <div>
                    <dt>Waiting mutex</dt>
                    <dd>
                      <code>{selected.waitingMutex.address}</code>, owner{" "}
                      <code>{selected.waitingMutex.owner}</code>, lock count{" "}
                      {selected.waitingMutex.lockCount}
                    </dd>
                  </div>
                )}
                {selected.cancelRequested && (
                  <div>
                    <dt>Cancellation</dt>
                    <dd>Requested</dd>
                  </div>
                )}
              </dl>
            </>
          ) : (
            <p>Select a thread to inspect its copied diagnostic snapshot.</p>
          )}
        </section>
      </div>
    </main>
  );
}
