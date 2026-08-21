import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "../bridge/native";
import type { AudioVoiceDiagnosticPage } from "../bridge/contracts";

export function AudioDebuggerWindow() {
  const [activeOnly, setActiveOnly] = useState(true);
  const [page, setPage] = useState<AudioVoiceDiagnosticPage>();
  const [error, setError] = useState("");
  const requestSequence = useRef(0);
  const load = useCallback(async () => {
    const sequence = ++requestSequence.current;
    setError("");
    try {
      const result = await invoke("diagnostics.getAudioVoices", {
        generation: "0",
        offset: 0,
        limit: 200,
        activeOnly,
      });
      if (sequence === requestSequence.current) setPage(result);
    } catch (reason) {
      if (sequence === requestSequence.current) setError(String(reason));
    }
  }, [activeOnly]);
  useEffect(() => {
    void load();
  }, [load]);
  useEffect(() => {
    const timer = window.setInterval(() => void load(), 1000);
    return () => window.clearInterval(timer);
  }, [load]);
  return (
    <main className="tool-window diagnostic-window">
      <header>
        <div>
          <div className="eyebrow">Audio diagnostics</div>
          <h1>AX voices</h1>
        </div>
        <button onClick={() => void load()}>Refresh snapshot</button>
      </header>
      <div className="toolbar embedded">
        <label className="check-row">
          <input
            type="checkbox"
            checked={activeOnly}
            onChange={(event) => setActiveOnly(event.target.checked)}
          />{" "}
          Active only
        </label>
        <span>
          {page
            ? `${page.total} copied voices · generation ${page.generation}`
            : "Loading…"}
        </span>
      </div>
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
      {page?.diagnostic && <p className="warning">{page.diagnostic}</p>}
      <div className="diagnostic-table-wrap">
        <table className="diagnostic-table">
          <thead>
            <tr>
              <th>Voice</th>
              <th>Format</th>
              <th>Current</th>
              <th>Loop</th>
              <th>End</th>
              <th>Volume</th>
              <th>SRC</th>
              <th>Filters</th>
              <th>Mix</th>
            </tr>
          </thead>
          <tbody>
            {page?.rows.map((row) => (
              <tr key={row.id}>
                <td>{row.index}</td>
                <td>{row.format}</td>
                <td>{row.currentOffset}</td>
                <td>{row.looping ? row.loopOffset : "—"}</td>
                <td>{row.endOffset}</td>
                <td>
                  {row.volume} ({row.volumeDelta})
                </td>
                <td>{`0x${row.sourceRatio.toString(16).padStart(8, "0")}`}</td>
                <td>
                  {[row.lowPassEnabled && "LPF", row.biquadEnabled && "Biquad"]
                    .filter(Boolean)
                    .join(", ") || "—"}
                </td>
                <td>
                  <code>{row.deviceMix}</code>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </main>
  );
}
