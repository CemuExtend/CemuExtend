import { useCallback, useEffect, useRef, useState } from "react";
import type { SaveManagerModel, SaveTitle } from "../bridge/contracts";
import { subscribe } from "../bridge/events";
import { invoke } from "../bridge/native";
import { activateJob, routeJobEvent } from "./checksumEvents";
import { isValidSavePersistentId } from "./saveManagerModel";

type JobEvent = { type: string; payload: Record<string, unknown> };
type Progress = { filesCompleted: number; filesTotal: number; bytesCompleted: number; bytesTotal: number };
export function SaveManagerWindow({ windowId }: { windowId: string }) {
  const [model, setModel] = useState<SaveManagerModel>({ scanning: false, accounts: [], titles: [] });
  const [titleId, setTitleId] = useState(""); const [persistentId, setPersistentId] = useState("");
  const [targetId, setTargetId] = useState(""); const [error, setError] = useState(""); const [status, setStatus] = useState("");
  const [progress, setProgress] = useState<Progress>(); const jobId = useRef<string | undefined>(undefined);
  const pendingEvents = useRef(new Map<string, JobEvent[]>());
  const load = useCallback(async () => {
    const next = await invoke("save.getModel"); setModel(next);
    setTitleId((current) => next.titles.some((title) => title.titleId === current) ? current : next.titles[0]?.titleId ?? "");
  }, []);
  const selectedTitle = model.titles.find((title) => title.titleId === titleId);
  const selectedSave = selectedTitle?.saves.find((save) => save.persistentId === persistentId);
  useEffect(() => {
    if (!selectedTitle?.saves.some((save) => save.persistentId === persistentId)) setPersistentId(selectedTitle?.saves[0]?.persistentId ?? "");
  }, [persistentId, selectedTitle]);
  const applyJobEvent = useCallback((event: JobEvent) => {
    const payload = event.payload;
    if (event.type === "jobs.progress") {
      setProgress({ filesCompleted: Number(payload.filesCompleted ?? 0), filesTotal: Number(payload.filesTotal ?? 0), bytesCompleted: Number(payload.bytesCompleted ?? 0), bytesTotal: Number(payload.bytesTotal ?? 0) }); return;
    }
    jobId.current = undefined; setProgress(undefined);
    if (payload.ok === true) { setStatus(`${String(payload.operation)} completed`); void load(); }
    else setError(String(payload.diagnostic || "Save archive operation failed"));
  }, [load]);
  useEffect(() => {
    void load().catch((reason) => setError(String(reason)));
    return subscribe((event) => {
      if (event.type === "titles.changed") { void load(); return; }
      if ((event.type !== "jobs.progress" && event.type !== "jobs.completed") || !event.payload || typeof event.payload !== "object") return;
      const payload = event.payload as Record<string, unknown>;
      if (payload.windowId !== windowId || typeof payload.jobId !== "string" || !/^(0|[1-9][0-9]*)$/.test(payload.jobId)) return;
      routeJobEvent(pendingEvents.current, jobId.current, payload.jobId, { type: event.type, payload }, applyJobEvent);
    });
  }, [applyJobEvent, load, windowId]);
  const run = async (action: () => Promise<void>) => { setError(""); setStatus(""); try { await action(); } catch (reason) { setError(String(reason)); } };
  const activate = (started: { jobId: string }) => { jobId.current = started.jobId; setProgress({ filesCompleted: 0, filesTotal: 0, bytesCompleted: 0, bytesTotal: 0 }); activateJob(pendingEvents.current, started.jobId, applyJobEvent); };
  const remove = () => run(async () => {
    if (!selectedSave || !selectedTitle) return;
    const prepared = await invoke("save.delete.prepare", { titleId: selectedTitle.titleId, persistentId: selectedSave.persistentId });
    if (!window.confirm(`Permanently delete save ${selectedSave.persistentId} for ${selectedTitle.name || selectedTitle.titleId}?`)) return;
    await invoke("save.delete", prepared); setStatus("Save deleted"); await load();
  });
  const transfer = () => run(async () => {
    if (!selectedSave || !selectedTitle) return;
    const inspected = await invoke("save.transfer.inspect", { titleId: selectedTitle.titleId, sourcePersistentId: selectedSave.persistentId, targetPersistentId: targetId });
    const overwrite = inspected.targetState === "directory";
    if (!window.confirm(`${overwrite ? "Overwrite the existing target save and move" : "Move"} ${selectedSave.persistentId} to ${targetId}?`)) return;
    await invoke("save.transfer", { confirmationToken: inspected.confirmationToken }); setStatus("Save transferred"); await load();
  });
  const importArchive = () => run(async () => {
    if (!selectedTitle) return;
    const picked = await invoke("save.import.pick", { titleId: selectedTitle.titleId }); if (!picked.selected || !picked.fileToken) return;
    const target = targetId || model.accounts[0]?.persistentId || "";
    const inspected = await invoke("save.import.inspect", { fileToken: picked.fileToken, titleId: selectedTitle.titleId, persistentId: target });
    const warnings = [inspected.titleMismatch ? `Archive title ${inspected.sourceTitleId ?? "unknown"} differs.` : "", inspected.targetState === "directory" ? "The target save will be overwritten." : ""].filter(Boolean).join("\n");
    if (!window.confirm(`Import ${picked.name ?? "archive"} to account ${target}?${warnings ? `\n\n${warnings}` : ""}`)) return;
    activate(await invoke("save.import.start", { confirmationToken: inspected.confirmationToken }));
  });
  const exportArchive = () => run(async () => {
    if (!selectedSave || !selectedTitle) return;
    const picked = await invoke("save.export.pick", { titleId: selectedTitle.titleId, persistentId: selectedSave.persistentId });
    if (!picked.selected || !picked.confirmationToken) return;
    activate(await invoke("save.export.start", { confirmationToken: picked.confirmationToken }));
  });
  const cancel = () => { if (jobId.current !== undefined) void invoke("jobs.cancel", { jobId: jobId.current }).catch((reason: unknown) => setError(String(reason))); };
  const percent = progress ? progress.bytesTotal ? progress.bytesCompleted * 100 / progress.bytesTotal : progress.filesTotal ? progress.filesCompleted * 100 / progress.filesTotal : 0 : 0;
  return <main className="role-window manager-window"><header><div><span className="eyebrow">Account save data</span><h1>Save Manager</h1></div><div className="button-row">{progress && <button onClick={cancel}>Cancel</button>}<button onClick={() => void invoke("window.close")}>Close</button></div></header>
    {error && <div className="notice error" role="alert">{error}</div>}{status && <div className="notice">{status}</div>}
    <div className="split-view"><aside className="selection-list">{model.titles.map((title) => <button key={title.titleId} className={title.titleId === titleId ? "selected" : ""} disabled={!!progress} onClick={() => setTitleId(title.titleId)}><strong>{title.name || title.titleId}</strong><code>{title.titleId}</code><span>{title.saves.length} save account(s)</span></button>)}</aside>
      <section className="editor-panel">{selectedTitle ? <SaveEditor title={selectedTitle} persistentId={persistentId} setPersistentId={setPersistentId} targetId={targetId} setTargetId={setTargetId} busy={!!progress} onDelete={remove} onTransfer={transfer} onImport={importArchive} onExport={exportArchive} accounts={model.accounts} /> : <p>No installed save data was found.</p>}
      {progress && <><progress max={100} value={percent} /><p className="muted">{progress.filesCompleted}/{progress.filesTotal} files · {Math.round(progress.bytesCompleted / 1048576)}/{Math.round(progress.bytesTotal / 1048576)} MiB</p></>}</section></div></main>;
}

function SaveEditor({ title, persistentId, setPersistentId, targetId, setTargetId, busy, onDelete, onTransfer, onImport, onExport, accounts }: { title: SaveTitle; persistentId: string; setPersistentId: (id: string) => void; targetId: string; setTargetId: (id: string) => void; busy: boolean; onDelete: () => void; onTransfer: () => void; onImport: () => void; onExport: () => void; accounts: SaveManagerModel["accounts"] }) {
  const validTarget = isValidSavePersistentId(targetId);
  return <><h2>{title.name || title.titleId}</h2><code>{title.titleId}</code><div className="form-grid"><label>Save account<select value={persistentId} onChange={(event) => setPersistentId(event.target.value)}>{title.saves.map((save) => <option key={save.persistentId} value={save.persistentId}>{save.persistentId}{save.accountName ? ` (${save.accountName})` : ""}</option>)}</select></label><label>Transfer / import target<input list="save-accounts" value={targetId} placeholder="80000001" onChange={(event) => setTargetId(event.target.value.trim())} /><datalist id="save-accounts">{accounts.map((account) => <option key={account.persistentId} value={account.persistentId}>{account.name}</option>)}</datalist></label></div><div className="button-row"><button disabled={!persistentId || busy} onClick={onExport}>Export…</button><button disabled={!validTarget || !persistentId || busy} onClick={onTransfer}>Transfer</button><button disabled={!validTarget || busy} onClick={onImport}>Import…</button><button className="danger" disabled={!persistentId || busy} onClick={onDelete}>Delete</button></div><p className="muted">Import and export paths are chosen by the native file dialog; this page never receives or submits filesystem paths.</p></>;
}
