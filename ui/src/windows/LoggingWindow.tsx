import { useEffect, useMemo, useRef, useState } from "react";
import type { LoggingEntry, LoggingLevel } from "../bridge/contracts";
import { subscribe } from "../bridge/events";
import { invoke } from "../bridge/native";
import { loggingSnapshotFromEvent, mergeLoggingEntries } from "./loggingEvents";

export function LoggingWindow() {
  const [entries, setEntries] = useState<LoggingEntry[]>([]);
  const [query, setQuery] = useState("");
  const [category, setCategory] = useState("all");
  const [level, setLevel] = useState<"all" | LoggingLevel>("all");
  const [autoscroll, setAutoscroll] = useState(true);
  const [dropped, setDropped] = useState(0);
  const [error, setError] = useState("");
  const end = useRef<HTMLDivElement>(null);
  // A clear RPC and the native event stream are asynchronous. Keep a sequence
  // watermark so a snapshot/event that was already in flight cannot resurrect
  // entries cleared by the user.
  const clearedThrough = useRef(0);

  useEffect(() => {
    let active = true;
    const unsubscribe = subscribe((event) => {
      if (event.type === "logging.cleared") {
        const watermark = event.payload && typeof event.payload === "object" &&
          Number.isSafeInteger((event.payload as { clearedThroughSequence?: unknown }).clearedThroughSequence)
          ? (event.payload as { clearedThroughSequence: number }).clearedThroughSequence : 0;
        clearedThrough.current = Math.max(clearedThrough.current, watermark);
        setEntries((current) => current.filter((entry) => entry.sequence > clearedThrough.current));
        return;
      }
      const snapshot = loggingSnapshotFromEvent(event);
      if (!snapshot) return;
      setEntries((current) => mergeLoggingEntries(current,
        snapshot.entries.filter((entry) => entry.sequence > clearedThrough.current)));
      setDropped(snapshot.droppedEntries);
    });
    void invoke("logging.getSnapshot").then((snapshot) => {
      if (!active) return;
      setEntries((current) => mergeLoggingEntries(current,
        snapshot.entries.filter((entry) => entry.sequence > clearedThrough.current)));
      setDropped(snapshot.droppedEntries);
    }).catch((reason: unknown) => active && setError(String(reason)));
    return () => { active = false; unsubscribe(); };
  }, []);

  const categories = useMemo(() => [...new Set(entries.map((entry) => entry.category).filter(Boolean))].sort(), [entries]);
  const visible = useMemo(() => {
    const needle = query.trim().toLocaleLowerCase();
    return entries.filter((entry) => (category === "all" || entry.category === category) &&
      (level === "all" || entry.level === level) &&
      (!needle || `${entry.category} ${entry.message}`.toLocaleLowerCase().includes(needle)));
  }, [category, entries, level, query]);

  useEffect(() => { if (autoscroll) end.current?.scrollIntoView({ block: "end" }); }, [autoscroll, visible]);
  const clear = async () => {
    setError("");
    try {
      const result = await invoke("logging.clear");
      clearedThrough.current = Math.max(clearedThrough.current, result.clearedThroughSequence);
      setEntries((current) => current.filter((entry) => entry.sequence > clearedThrough.current));
    } catch (reason) { setError(String(reason)); }
  };

  return <main className="role-window logging-window"><header><div><span className="eyebrow">Diagnostics</span><h1>Logging</h1></div><div className="button-row"><button onClick={() => void clear()}>Clear</button><button onClick={() => void invoke("window.close")}>Close</button></div></header>
    {error && <div className="notice error" role="alert">{error}</div>}
    <div className="toolbar embedded logging-toolbar"><input aria-label="Filter log messages" type="search" placeholder="Filter categories or messages" value={query} onChange={(event) => setQuery(event.target.value)} /><select aria-label="Category" value={category} onChange={(event) => setCategory(event.target.value)}><option value="all">All categories</option>{categories.map((value) => <option key={value} value={value}>{value}</option>)}</select><select aria-label="Level" value={level} onChange={(event) => setLevel(event.target.value as typeof level)}><option value="all">All levels</option><option value="info">Info</option><option value="warning">Warnings</option><option value="error">Errors</option></select><label className="check-row"><input type="checkbox" checked={autoscroll} onChange={(event) => setAutoscroll(event.target.checked)} />Auto-scroll</label></div>
    <div className="logging-status">{visible.length} shown / {entries.length} retained{dropped > 0 ? ` · ${dropped} older entries discarded` : ""}</div>
    <section className="log-list" aria-live="polite" aria-label="Log messages">{visible.map((entry) => <article key={entry.sequence} className={`log-entry ${entry.level}`}><span className="log-level">{entry.level}</span><span className="log-category">{entry.category || "General"}</span><pre>{entry.message}</pre></article>)}<div ref={end} /></section>
  </main>;
}
