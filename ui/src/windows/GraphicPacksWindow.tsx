import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { GraphicPack, GraphicPackInstallRequest, GraphicPackMutation } from "../bridge/contracts";
import { invoke } from "../bridge/native";
import { subscribe } from "../bridge/events";
import { Modal } from "../components/Modal";

type JobPayload = Record<string, unknown>;
type InstallView = {
  request: GraphicPackInstallRequest;
  jobId?: number;
  phase: string;
  completed: number;
  total: number;
  currentPath: string;
  confirmation?: string;
  cancelling?: boolean;
};

export function GraphicPacksWindow({ windowId, initialTitleId }: { windowId: number; initialTitleId?: string }) {
  const [packs, setPacks] = useState<GraphicPack[]>([]);
  const [selectedKey, setSelectedKey] = useState("");
  const [query, setQuery] = useState("");
  const [titleId, setTitleId] = useState(initialTitleId);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState("");
  const [error, setError] = useState("");
  const [install, setInstall] = useState<InstallView>();
  const [customUrl, setCustomUrl] = useState("");
  const activeJob = useRef<number | undefined>(undefined);
  const pendingJobEvents = useRef(new Map<number, Array<{ type: string; payload: JobPayload }>>());

  const load = useCallback(async () => {
    const next = await invoke("graphicPacks.list");
    setPacks(next);
    setSelectedKey((key) => next.some((pack) => pack.key === key) ? key : next[0]?.key ?? "");
  }, []);

  const applyJobEvent = useCallback((type: string, payload: JobPayload) => {
    const jobId = payload.jobId;
    if (typeof jobId !== "number") return;
    if (type === "jobs.progress") {
      setInstall((current) => current?.jobId === jobId ? { ...current, phase: typeof payload.phase === "string" ? payload.phase : current.phase, completed: typeof payload.completed === "number" ? payload.completed : 0, total: typeof payload.total === "number" ? payload.total : 0, currentPath: typeof payload.currentPath === "string" ? payload.currentPath : "" } : current);
      return;
    }
    if (type !== "jobs.completed") return;
    activeJob.current = undefined;
    const ok = payload.ok === true;
    const errorCode = typeof payload.error === "string" ? payload.error : "ioFailure";
    const diagnostic = typeof payload.diagnostic === "string" ? payload.diagnostic : "Graphic-pack installation failed";
    if (ok) {
      setInstall(undefined);
      setMessage(payload.upToDate === true ? "Community graphic packs are already up to date." : "Graphic packs installed and refreshed.");
      if (Array.isArray(payload.removedEnabledPaths) && payload.removedEnabledPaths.length) setMessage(`Graphic packs installed. Reconfigure removed packs: ${payload.removedEnabledPaths.join(", ")}`);
      void load().catch((reason: unknown) => setError(String(reason)));
    } else if (errorCode === "confirmationRequired") {
      setInstall((current) => current ? { ...current, jobId: undefined, confirmation: diagnostic, cancelling: false } : current);
    } else if (errorCode === "cancelled") {
      setInstall(undefined);
      setMessage("Graphic-pack installation cancelled.");
    } else {
      setInstall((current) => current ? { ...current, jobId: undefined, cancelling: false } : current);
      setError(diagnostic);
    }
  }, [load]);

  useEffect(() => {
    void load().catch((reason: unknown) => setError(String(reason)));
    return subscribe((event) => {
      if (typeof event.payload !== "object" || !event.payload) return;
      const payload = event.payload as JobPayload;
      if (event.type === "window.contextChanged" && payload.windowId === windowId) {
        setTitleId(typeof payload.titleId === "string" ? payload.titleId : undefined);
        return;
      }
      if ((event.type !== "jobs.progress" && event.type !== "jobs.completed") || payload.windowId !== windowId || typeof payload.jobId !== "number") return;
      if (activeJob.current === payload.jobId) applyJobEvent(event.type, payload);
      else pendingJobEvents.current.set(payload.jobId, [...(pendingJobEvents.current.get(payload.jobId) ?? []), { type: event.type, payload }]);
    });
  }, [applyJobEvent, load, windowId]);

  const beginInstall = async (request: GraphicPackInstallRequest) => {
    setError(""); setMessage("");
    setInstall({ request, phase: "checking", completed: 0, total: 0, currentPath: "" });
    try {
      const { jobId } = await invoke("graphicPacks.install", request);
      activeJob.current = jobId;
      setInstall((current) => current ? { ...current, jobId, confirmation: undefined, cancelling: false } : current);
      const queued = pendingJobEvents.current.get(jobId) ?? [];
      pendingJobEvents.current.delete(jobId);
      queued.forEach((event) => applyJobEvent(event.type, event.payload));
    } catch (reason) {
      setInstall(undefined); setError(String(reason));
    }
  };

  const filtered = useMemo(() => { const needle = query.trim().toLocaleLowerCase(); return packs.filter((pack) => !titleId || pack.universal || pack.titleIds.includes(titleId)).filter((pack) => !needle || `${pack.name} ${pack.description} ${pack.virtualPath}`.toLocaleLowerCase().includes(needle)); }, [packs, query, titleId]);
  const selected = filtered.find((pack) => pack.key === selectedKey) ?? filtered[0];
  const mutate = async (operation: () => Promise<GraphicPackMutation>) => {
    setBusy(true); setError(""); setMessage("");
    try { const result = await operation(); if (result.info) setPacks((items) => items.map((item) => item.key === result.info!.key ? result.info! : item)); else await load(); setMessage(result.diagnostic || (result.requiresRestart ? "Saved. Restart the title to apply all changes." : "Changes applied.")); } catch (reason) { setError(String(reason)); } finally { setBusy(false); }
  };
  const presetNames = (pack: GraphicPack, category: string) => pack.presets.filter((preset) => preset.category === category && preset.visible);
  const cancelInstall = () => {
    if (!install?.jobId) { setInstall(undefined); return; }
    setInstall({ ...install, cancelling: true });
    void invoke("jobs.cancel", { jobId: install.jobId }).catch((reason: unknown) => setError(String(reason)));
  };

  return <main className="role-window manager-window">
    <header><div><span className="eyebrow">Enhancements & fixes</span><h1>Graphic Packs</h1></div><div className="button-row"><button disabled={busy || !!install} onClick={() => void beginInstall({ kind: "community", replaceExisting: false })}>Get community packs</button><button disabled={busy || !!install} onClick={() => setInstall({ request: { kind: "customUrl", url: "", replaceExisting: false }, phase: "ready", completed: 0, total: 0, currentPath: "" })}>Install from URL</button><button disabled={busy} onClick={() => void (async () => { setBusy(true); try { const result = await invoke("graphicPacks.refresh"); await load(); setMessage(result.diagnostic || "Graphic packs refreshed."); } catch (reason) { setError(String(reason)); } finally { setBusy(false); } })()}>Refresh</button><button onClick={() => void invoke("window.close")}>Close</button></div></header>
    <div className="toolbar embedded"><input type="search" placeholder="Search graphic packs" value={query} onChange={(event) => setQuery(event.target.value)} />{titleId && <><code>{titleId}</code><button onClick={() => setTitleId(undefined)}>Show all titles</button></>}<span>{filtered.length} packs</span></div>
    {error && <div className="notice error" role="alert">{error}</div>}{message && <div className="notice" role="status">{message}</div>}
    <div className="split-view"><aside className="selection-list">{filtered.map((pack) => <button className={pack.key === selected?.key ? "selected" : ""} key={pack.key} onClick={() => setSelectedKey(pack.key)}><strong>{pack.name}</strong><span>{pack.enabled ? "Enabled" : "Disabled"} · v{pack.version}</span><code>{pack.virtualPath}</code></button>)}</aside>
      <section className="editor-panel">{selected ? <><div className="pack-heading"><div><h2>{selected.name}</h2><p>{selected.description || "No description provided."}</p></div><label className="switch-row"><input type="checkbox" checked={selected.enabled} disabled={busy || !selected.supportedVersion} onChange={(event) => void mutate(() => invoke("graphicPacks.setEnabled", { key: selected.key, enabled: event.target.checked }))} /> Enabled</label></div>
        {!selected.supportedVersion && <div className="notice error">This graphic pack version is not supported.</div>}
        <dl className="detail-list compact"><div><dt>Contents</dt><dd>{[selected.hasShaders && "shaders", selected.hasPatches && "patches", selected.hasCustomVsync && "custom VSync"].filter(Boolean).join(", ") || "configuration"}</dd></div><div><dt>Status</dt><dd>{selected.activated ? "Active" : selected.enabled ? "Enabled for next launch" : "Disabled"}</dd></div><div><dt>Titles</dt><dd>{selected.universal ? "All titles" : selected.titleIds.join(", ") || "No installed title match"}</dd></div></dl>
        {selected.presetOrder.map((category) => { const options = presetNames(selected, category); if (!options.length) return null; const active = options.find((preset) => preset.active)?.name ?? ""; return <label className="preset-row" key={category}><span>{category}</span><select disabled={busy || !selected.enabled} value={active} onChange={(event) => void mutate(() => invoke("graphicPacks.setPreset", { key: selected.key, category, preset: event.target.value }))}>{options.map((preset) => <option key={preset.name}>{preset.name}</option>)}</select></label>; })}
        <div className="button-row"><button disabled={busy || !selected.activated} onClick={() => void mutate(() => invoke("graphicPacks.reload", { key: selected.key }))}>Reload shaders</button><button disabled={busy} onClick={() => void invoke("graphicPacks.save").then(() => setMessage("Graphic pack settings saved.")).catch((reason: unknown) => setError(String(reason)))}>Save</button></div>
      </> : <p>No graphic packs were found.</p>}</section></div>
    {install && <Modal title={install.request.kind === "community" ? "Community graphic packs" : "Install custom graphic pack"} onClose={cancelInstall}>
      {install.request.kind === "customUrl" && !install.jobId && !install.confirmation && <form onSubmit={(event) => { event.preventDefault(); void beginInstall({ kind: "customUrl", url: customUrl, replaceExisting: false }); }}><label>HTTPS ZIP URL<input autoFocus required type="url" pattern="https://.*" value={customUrl} onChange={(event) => setCustomUrl(event.target.value)} placeholder="https://example.com/pack.zip" /></label><div className="button-row"><button type="submit">Download and install</button><button type="button" onClick={() => setInstall(undefined)}>Cancel</button></div></form>}
      {install.jobId && <><p>{install.cancelling ? "Cancelling…" : `${install.phase[0]?.toUpperCase() ?? ""}${install.phase.slice(1)}…`}</p><progress max={install.total || 1} value={install.total ? install.completed : undefined} /><p className="muted">{install.currentPath}</p><button disabled={install.cancelling} onClick={cancelInstall}>Cancel</button></>}
      {install.confirmation && <><p>{install.confirmation}</p><div className="button-row"><button onClick={() => void beginInstall({ ...install.request, url: install.request.url || customUrl, replaceExisting: true })}>Replace existing packs</button><button onClick={() => setInstall(undefined)}>Cancel</button></div></>}
    </Modal>}
  </main>;
}
