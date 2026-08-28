import { useCallback, useEffect, useRef, useState } from "react";
import type {
  MemorySearchPage,
  MemorySearchSession,
  MemorySearchStatus,
  MemoryValueType,
} from "../bridge/contracts";
import { invoke } from "../bridge/native";
import { translate, translateFormat } from "../i18n/runtime";
import {
  pageOffset,
  parseMemoryValue,
  progressPercent,
} from "./memorySearchModel";

const PAGE_SIZE = 100;
const typeOptions: Array<{ value: MemoryValueType; label: string }> = [
  { value: "float32", label: "float" },
  { value: "float64", label: "double" },
  { value: "int8", label: "int8" },
  { value: "int16", label: "int16" },
  { value: "int32", label: "int32" },
  { value: "int64", label: "int64" },
];

export function MemorySearcherWindow() {
  const [type, setType] = useState<MemoryValueType>("float32");
  const [input, setInput] = useState("");
  const [scanMiB, setScanMiB] = useState(256);
  const [session, setSession] = useState<MemorySearchSession>();
  const sessionRef = useRef<MemorySearchSession | undefined>(undefined);
  const [status, setStatus] = useState<MemorySearchStatus>();
  const [page, setPage] = useState<MemorySearchPage>();
  const [pageNumber, setPageNumber] = useState(0);
  const [error, setError] = useState("");

  const loadPage = useCallback(
    async (active: MemorySearchSession, nextPage: number, total: number) => {
      const offset = pageOffset(nextPage, PAGE_SIZE, total);
      const result = await invoke("memorySearch.page", {
        sessionToken: active.sessionToken,
        generation: active.generation,
        offset,
        limit: PAGE_SIZE,
      });
      if (
        sessionRef.current?.sessionToken === active.sessionToken &&
        sessionRef.current.generation === active.generation
      ) {
        setPage(result);
        setPageNumber(Math.floor(offset / PAGE_SIZE));
      }
    },
    [],
  );

  useEffect(() => {
    sessionRef.current = session;
    if (!session) return;
    let stopped = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const poll = async () => {
      try {
        const next = await invoke("memorySearch.status", {
          sessionToken: session.sessionToken,
        });
        if (stopped) return;
        setStatus(next);
        if (next.state === "scanning")
          timer = setTimeout(() => void poll(), 150);
        else if (next.state === "complete")
          await loadPage(session, 0, next.resultCount);
        else if (next.state === "failed")
          setError(next.diagnostic || translate("Memory search failed."));
      } catch (reason) {
        if (!stopped) setError(String(reason));
      }
    };
    void poll();
    return () => {
      stopped = true;
      if (timer) clearTimeout(timer);
    };
  }, [loadPage, session]);

  const begin = async (filter: boolean) => {
    const parsed = parseMemoryValue(type, input);
    if (typeof parsed === "string") {
      setError(parsed);
      return;
    }
    setError("");
    setPage(undefined);
    setStatus(undefined);
    setPageNumber(0);
    try {
      const next =
        filter && session
          ? await invoke("memorySearch.filter", {
              sessionToken: session.sessionToken,
              generation: session.generation,
              value: parsed,
            })
          : await invoke("memorySearch.start", {
              value: parsed,
              maximumBytes: scanMiB * 1024 * 1024,
            });
      setSession(next);
    } catch (reason) {
      setError(String(reason));
    }
  };
  const cancel = async () => {
    if (!session) return;
    try {
      await invoke("memorySearch.cancel", {
        sessionToken: session.sessionToken,
        generation: session.generation,
      });
      setStatus((current) =>
        current ? { ...current, state: "cancelled" } : current,
      );
    } catch (reason) {
      setError(String(reason));
    }
  };
  const clear = () => {
    setSession(undefined);
    sessionRef.current = undefined;
    setStatus(undefined);
    setPage(undefined);
    setPageNumber(0);
    setError("");
  };
  const busy = status?.state === "scanning";
  const totalPages = Math.max(1, Math.ceil((page?.total ?? 0) / PAGE_SIZE));

  return (
    <main className="role-window memory-search-window">
      <header>
        <div>
          <h1>Memory Searcher</h1>
          <p>Typed scans, filtering, progress, and paged results.</p>
        </div>
      </header>
      {error && (
        <div className="notice error" role="alert">
          {error}
        </div>
      )}
      <section
        className="memory-search-controls"
        aria-label={translate("Search controls")}
      >
        <label>
          Data type
          <select
            disabled={busy}
            value={type}
            onChange={(event) => setType(event.target.value as MemoryValueType)}
          >
            {typeOptions.map((option) => (
              <option key={option.value} value={option.value}>
                {option.label}
              </option>
            ))}
          </select>
        </label>
        <label>
          Value
          <input
            value={input}
            onChange={(event) => setInput(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === "Enter" && !busy) void begin(!!session);
            }}
          />
        </label>
        <label>
          Scan cap
          <select
            disabled={busy}
            value={scanMiB}
            onChange={(event) => setScanMiB(Number(event.target.value))}
          >
            <option value={64}>64 MiB</option>
            <option value={128}>128 MiB</option>
            <option value={256}>256 MiB</option>
            <option value={512}>512 MiB</option>
          </select>
        </label>
        <div className="button-row">
          <button
            className="primary"
            disabled={busy || !input.trim()}
            onClick={() => void begin(false)}
          >
            Search
          </button>
          <button
            disabled={!session || busy || status?.state !== "complete"}
            onClick={() => void begin(true)}
          >
            Filter
          </button>
          {busy && <button onClick={() => void cancel()}>Cancel</button>}
          <button disabled={!session} onClick={clear}>
            Clear
          </button>
        </div>
      </section>
      <section className="memory-search-summary" aria-live="polite">
        {status ? (
          <div className="memory-search-progress">
            <progress max={100} value={progressPercent(status)} />
            <span>
              {translate(
                status.state === "scanning" ? "Scanning" : status.state,
              )}
              : {Math.round(status.bytesScanned / 1048576)} /{" "}
              {Math.round(status.bytesTotal / 1048576)} MiB ·{" "}
              {translateFormat("{count} results", {
                count: status.resultCount.toLocaleString(),
              })}
            </span>
            {status.scanCapReached && (
              <span className="warning">
                Scan stopped at the selected byte cap.
              </span>
            )}
            {status.resultCapReached && (
              <span className="warning">Result cap reached (50,000).</span>
            )}
          </div>
        ) : (
          <p className="muted">
            Start a running Wii U title, then search mapped guest memory.
            Addresses are session-bound diagnostics; the web UI never receives
            native pointers.
          </p>
        )}
      </section>
      <section
        className="memory-results"
        aria-label={translate("Memory search results")}
      >
        <div className="memory-result-header">
          <strong>Address</strong>
          <strong>Value</strong>
          <strong>Type</strong>
        </div>
        {page?.results.map((result) => (
          <div className="memory-result-row" key={result.address.value}>
            <code>{result.address.value}</code>
            <code>{result.value.text}</code>
            <span>{result.value.type}</span>
          </div>
        ))}
        {status?.state === "complete" && !page?.results.length && (
          <p className="empty-results">No matching values found.</p>
        )}
      </section>
      <footer className="memory-pagination">
        <button
          disabled={!page || pageNumber === 0}
          onClick={() =>
            session && void loadPage(session, pageNumber - 1, page?.total ?? 0)
          }
        >
          Previous
        </button>
        <span>
          {translateFormat("Page {page} of {pages}", {
            page: pageNumber + 1,
            pages: totalPages,
          })}
        </span>
        <button
          disabled={!page || pageNumber + 1 >= totalPages}
          onClick={() =>
            session && void loadPage(session, pageNumber + 1, page?.total ?? 0)
          }
        >
          Next
        </button>
      </footer>
    </main>
  );
}
