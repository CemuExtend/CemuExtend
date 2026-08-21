import { useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "../../bridge/native";
import { subscribe } from "../../bridge/events";
import type { Title } from "../../bridge/contracts";
import { openWindow as openNativeWindow } from "../../bridge/windows";

type SortKey = "name" | "lastPlayed" | "playTime";

export function Library() {
  const [titles, setTitles] = useState<Title[]>([]);
  const [query, setQuery] = useState("");
  const [region, setRegion] = useState("all");
  const [sort, setSort] = useState<SortKey>("name");
  const [view, setView] = useState<"grid" | "list">("grid");
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [selected, setSelected] = useState<string>();
  const scrollHost = useRef<HTMLDivElement>(null);

  const load = async () => {
    setLoading(true); setError("");
    try { setTitles(await invoke("titles.list")); }
    catch (reason) { setError(reason instanceof Error ? reason.message : String(reason)); }
    finally { setLoading(false); }
  };
  useEffect(() => { void load(); return subscribe((event) => {
    if (event.type === "titles.changed") void load();
    else if ((event.type === "system.diagnostic" || event.type === "window.openFailed") && typeof event.payload === "object" && event.payload && "message" in event.payload && typeof event.payload.message === "string") setError(event.payload.message);
  }); }, []);

  const visible = useMemo(() => titles
    .filter((title) => region === "all" || title.region === region)
    .filter((title) => `${title.name} ${title.titleId}`.toLocaleLowerCase().includes(query.toLocaleLowerCase()))
    .sort((a, b) => sort === "name" ? a.name.localeCompare(b.name) : sort === "playTime" ? b.playTimeMinutes - a.playTimeMinutes : (b.lastPlayed ?? "").localeCompare(a.lastPlayed ?? "")),
  [titles, query, region, sort]);
  const regions = [...new Set(titles.map((title) => title.region))].sort();
  const launch = async (title: Title) => {
    setSelected(title.titleId); setError("");
    try { await invoke("titles.launch", { titleId: title.titleId }); }
    catch (reason) { setError(reason instanceof Error ? reason.message : String(reason)); }
  };
  const reportOpen = (operation: Promise<string>) => { void operation.catch((reason: unknown) => setError(reason instanceof Error ? reason.message : String(reason))); };

  return <main className="library">
    <section className="toolbar" aria-label="Game library controls">
      <input type="search" value={query} onChange={(event) => setQuery(event.target.value)} placeholder="Search games or Title ID" autoFocus />
      <select value={region} onChange={(event) => setRegion(event.target.value)} aria-label="Region"><option value="all">All regions</option>{regions.map((value) => <option key={value}>{value}</option>)}</select>
      <select value={sort} onChange={(event) => setSort(event.target.value as SortKey)} aria-label="Sort"><option value="name">Name</option><option value="lastPlayed">Last played</option><option value="playTime">Play time</option></select>
      <div className="segmented"><button aria-pressed={view === "grid"} onClick={() => setView("grid")}>Grid</button><button aria-pressed={view === "list"} onClick={() => setView("list")}>List</button></div>
      <button onClick={() => void invoke("titles.refresh").then(load).catch((reason: unknown) => setError(reason instanceof Error ? reason.message : String(reason)))}>Refresh</button>
      <button onClick={() => reportOpen(openNativeWindow("graphic-packs"))}>Graphic packs</button>
      <button onClick={() => reportOpen(openNativeWindow("cemod-manager"))}>CemuMods</button>
      <button onClick={() => reportOpen(openNativeWindow("update-manager"))}>Updates</button>
      <button onClick={() => reportOpen(openNativeWindow("account-manager"))}>Accounts</button>
      <button onClick={() => reportOpen(openNativeWindow("about"))}>About</button>
    </section>
    {error && <div className="notice error" role="alert">{error}<button onClick={() => setError("")}>Dismiss</button></div>}
    <div className={`title-host ${view}`} ref={scrollHost} role="listbox" aria-busy={loading}>
      {loading ? <div className="empty"><span className="spinner" />Loading game library…</div> : visible.length === 0 ? <div className="empty"><h2>No games found</h2><p>{titles.length ? "Try changing search or filters." : "Add a game path in General Settings, then refresh."}</p></div> : visible.map((title) =>
        <article key={title.titleId} className={selected === title.titleId ? "selected" : ""} role="option" aria-selected={selected === title.titleId} tabIndex={0} onClick={() => setSelected(title.titleId)} onDoubleClick={() => void launch(title)} onKeyDown={(event) => { if (event.key === "Enter") void launch(title); }}>
          {title.iconDataUrl ? <img src={title.iconDataUrl} alt="" /> : <div className="icon-fallback">{title.name.slice(0, 1)}</div>}
          <div className="title-info"><h2>{title.name}</h2><code>{title.titleId}</code><p>{title.region} · v{title.version} · {Math.round(title.playTimeMinutes / 60)}h played</p></div>
          <div className="button-row"><button onClick={(event) => { event.stopPropagation(); reportOpen(openNativeWindow("graphic-packs", { titleId: title.titleId })); }}>Packs</button><button onClick={(event) => { event.stopPropagation(); void launch(title); }}>Play</button></div>
        </article>)}
    </div>
    <footer>{visible.length} of {titles.length} titles{selected ? ` · ${titles.find((title) => title.titleId === selected)?.name ?? ""}` : ""}</footer>
  </main>;
}
