import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { Account, AccountManagerModel, AccountUpdate } from "../bridge/contracts";
import { invoke } from "../bridge/native";
import { Modal } from "../components/Modal";

function editable(account: Account): AccountUpdate {
  return { persistentId: account.persistentId, miiName: account.miiName, birthYear: account.birthYear, birthMonth: account.birthMonth, birthDay: account.birthDay, gender: account.gender, email: account.email, country: account.country };
}

export function AccountManagerWindow() {
  const [model, setModel] = useState<AccountManagerModel>();
  const [selectedId, setSelectedId] = useState<number>();
  const selectedIdRef = useRef<number | undefined>(undefined);
  const [draft, setDraft] = useState<AccountUpdate>();
  const [newName, setNewName] = useState("");
  const [createOpen, setCreateOpen] = useState(false);
  const [deleteOpen, setDeleteOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const load = useCallback(async () => {
    const next = await invoke("accounts.getModel");
    setModel(next);
    const currentId = selectedIdRef.current;
    const id = currentId && next.accounts.some((account) => account.persistentId === currentId) ? currentId : next.accounts[0]?.persistentId;
    selectedIdRef.current = id;
    setSelectedId(id);
    setDraft(id ? editable(next.accounts.find((account) => account.persistentId === id)!) : undefined);
  }, []);
  useEffect(() => { void load().catch((reason: unknown) => setError(String(reason))); }, [load]);
  const selected = useMemo(() => model?.accounts.find((account) => account.persistentId === selectedId), [model, selectedId]);
  const selectedNetwork = model?.networkSettings.find((setting) => setting.persistentId === selectedId);
  const select = (account: Account) => { selectedIdRef.current = account.persistentId; setSelectedId(account.persistentId); setDraft(editable(account)); setError(""); };
  const perform = async (operation: () => Promise<unknown>) => {
    setBusy(true); setError("");
    try { await operation(); await load(); } catch (reason) { setError(String(reason)); } finally { setBusy(false); }
  };
  const setField = <K extends keyof AccountUpdate>(key: K, value: AccountUpdate[K]) => setDraft((current) => current ? { ...current, [key]: value } : current);
  return <main className="role-window manager-window">
    <header><div><span className="eyebrow">Online & saves</span><h1>Account Manager</h1></div><div className="button-row"><button disabled={!model?.hasFreeSlots || model.titleRunning || busy} onClick={() => setCreateOpen(true)}>New account</button><button onClick={() => void invoke("window.close")}>Close</button></div></header>
    {error && <div className="notice error" role="alert">{error}</div>}
    {model?.titleRunning && <div className="notice" role="status">Account changes are locked while a title is running.</div>}
    {!model ? <div className="spinner" aria-label="Loading" /> : <div className="split-view">
      <aside className="selection-list" aria-label="Accounts">{model.accounts.map((account) => <button className={account.persistentId === selectedId ? "selected" : ""} key={account.persistentId} onClick={() => select(account)}><strong>{account.miiName}{account.persistentId === model.activePersistentId ? " · Active" : ""}</strong><code>{account.persistentIdHex}</code><span>{account.validOnlineAccount ? "Online ready" : "Local account"}</span></button>)}</aside>
      <section className="editor-panel">{selected && draft ? <form onSubmit={(event) => { event.preventDefault(); void perform(() => invoke("accounts.update", draft)); }}>
        <div className="pack-heading"><h2>Edit {selected.miiName}</h2><button type="button" disabled={busy || model.titleRunning || selected.persistentId === model.activePersistentId} onClick={() => void perform(() => invoke("accounts.setActive", { persistentId: selected.persistentId }))}>Make active</button></div><div className="form-grid">
          <label>Mii name<input disabled={model.titleRunning} required maxLength={10} value={draft.miiName} onChange={(event) => setField("miiName", event.target.value)} /></label>
          <label>Email<input disabled={model.titleRunning} type="email" value={draft.email} onChange={(event) => setField("email", event.target.value)} /></label>
          <label>Birth year<input disabled={model.titleRunning} type="number" min={0} max={2100} value={draft.birthYear} onChange={(event) => setField("birthYear", event.target.valueAsNumber)} /></label>
          <label>Birth month<input disabled={model.titleRunning} type="number" min={0} max={12} value={draft.birthMonth} onChange={(event) => setField("birthMonth", event.target.valueAsNumber)} /></label>
          <label>Birth day<input disabled={model.titleRunning} type="number" min={0} max={31} value={draft.birthDay} onChange={(event) => setField("birthDay", event.target.valueAsNumber)} /></label>
          <label>Gender<select disabled={model.titleRunning} value={draft.gender} onChange={(event) => setField("gender", Number(event.target.value))}><option value={0}>Female</option><option value={1}>Male</option><option value={2}>Legacy / unspecified</option></select></label>
          <label>Country<select disabled={model.titleRunning} value={draft.country} onChange={(event) => setField("country", Number(event.target.value))}>{model.countries.map((country) => <option key={country.code} value={country.code}>{country.name}</option>)}</select></label>
          <label>Network service<select disabled={busy || model.titleRunning || !selectedNetwork?.validation.validAccount} value={selectedNetwork?.service ?? "offline"} onChange={(event) => void perform(() => invoke("accounts.setNetworkService", { persistentId: selected.persistentId, service: event.target.value as "offline" | "nintendo" | "pretendo" | "custom" | "plasma" }))}><option value="offline">Offline</option><option value="nintendo">Nintendo</option><option value="pretendo">Pretendo</option><option value="plasma">Plasma</option><option value="custom">Custom</option></select></label>
        </div>{selectedNetwork && !selectedNetwork.validation.validAccount && <div className="notice">Online setup is incomplete{selectedNetwork.validation.missingFiles.length ? `: ${selectedNetwork.validation.missingFiles.join(", ")}` : "."} OTP: {selectedNetwork.validation.otp}; SEEPROM: {selectedNetwork.validation.seeprom}; account: {selectedNetwork.validation.accountError}.</div>}<div className="button-row"><button type="submit" disabled={busy || model.titleRunning}>Save changes</button><button type="button" disabled={busy} onClick={() => void load().catch((reason: unknown) => setError(String(reason)))}>Recheck online setup</button><button type="button" className="danger" disabled={busy || model.titleRunning || model.accounts.length <= 1} onClick={() => setDeleteOpen(true)}>Delete account</button></div>
      </form> : <p>Select an account.</p>}</section>
    </div>}
    {createOpen && model && <Modal title="Create account" onClose={() => setCreateOpen(false)}><form onSubmit={(event) => { event.preventDefault(); const persistentId = model.nextPersistentId; setCreateOpen(false); void perform(() => invoke("accounts.create", { persistentId, miiName: newName })); }}><p>Persistent ID: <code>{model.nextPersistentId.toString(16).padStart(8, "0")}</code></p><label>Mii name<input autoFocus required maxLength={10} value={newName} onChange={(event) => setNewName(event.target.value)} /></label><div className="button-row"><button type="submit">Create</button><button type="button" onClick={() => setCreateOpen(false)}>Cancel</button></div></form></Modal>}
    {deleteOpen && selected && <Modal title="Delete account" onClose={() => setDeleteOpen(false)}><p>Delete <strong>{selected.miiName}</strong>? Save data is not deleted.</p><div className="button-row"><button className="danger" onClick={() => { setDeleteOpen(false); void perform(() => invoke("accounts.delete", { persistentId: selected.persistentId })); }}>Delete</button><button onClick={() => setDeleteOpen(false)}>Cancel</button></div></Modal>}
  </main>;
}
