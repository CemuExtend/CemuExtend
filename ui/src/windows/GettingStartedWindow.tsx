import { useEffect, useMemo, useState } from "react";
import type { FrontendSettings } from "../bridge/contracts";
import { invoke } from "../bridge/native";
import { openWindow } from "../bridge/windows";

export function GettingStartedWindow() {
  const [model, setModel] = useState<FrontendSettings | null>(null);
  const [gamePaths, setGamePaths] = useState<string[]>([]);
  const [candidatePath, setCandidatePath] = useState("");
  const [startFullscreen, setStartFullscreen] = useState(false);
  const [openPad, setOpenPad] = useState(false);
  const [checkUpdates, setCheckUpdates] = useState(true);
  const [page, setPage] = useState<0 | 1>(0);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  useEffect(() => {
    void invoke("settings.getFrontend").then((value) => {
      setModel(value);
      setGamePaths(value.gamePaths);
      setStartFullscreen(value.startFullscreen);
      setOpenPad(value.openPad);
      setCheckUpdates(value.checkUpdates);
    }).catch((reason: unknown) => setError(String(reason)));
  }, []);

  const normalizedCandidate = candidatePath.trim();
  const canAdd = useMemo(() => normalizedCandidate.length > 0 &&
    !gamePaths.includes(normalizedCandidate), [gamePaths, normalizedCandidate]);

  function addGamePath() {
    if (!canAdd) return;
    setGamePaths((paths) => [...paths, normalizedCandidate]);
    setCandidatePath("");
  }

  async function finish() {
    if (!model) return;
    setBusy(true);
    setError("");
    try {
      const result = await invoke("settings.applyFrontend", {
        revision: model.revision,
        gamePaths,
        startFullscreen,
        openPad,
        checkUpdates,
        completeSetup: true,
      });
      setModel(result.snapshot);
      if (!result.ok) {
        if (result.error === "conflict") {
          setGamePaths(result.snapshot.gamePaths);
          setStartFullscreen(result.snapshot.startFullscreen);
          setOpenPad(result.snapshot.openPad);
          setCheckUpdates(result.snapshot.checkUpdates);
        }
        setError(result.diagnostic || "The settings could not be applied.");
        return;
      }
      await invoke("window.close");
    } catch (reason) {
      setError(String(reason));
      try {
        const latest = await invoke("settings.getFrontend");
        setModel(latest);
        setGamePaths(latest.gamePaths);
        setStartFullscreen(latest.startFullscreen);
        setOpenPad(latest.openPad);
        setCheckUpdates(latest.checkUpdates);
      } catch { /* Preserve the actionable apply error. */ }
    } finally {
      setBusy(false);
    }
  }

  if (!model) return <main className="tool-window"><h1>Getting Started</h1><p>{error || "Loading settings…"}</p></main>;

  return <main className="tool-window wizard-window">
    <header><div><p className="eyebrow">First-run setup</p><h1>Getting Started</h1></div><span>Step {page + 1} of 2</span></header>
    {error && <p className="error" role="alert">{error}</p>}
    {model.titleRunning && <p className="warning">Stop the running title before applying setup changes.</p>}

    {page === 0 ? <section className="settings-section">
      <h2>Game library</h2>
      <p>Add directories that contain Wii U games, updates, or DLC. Paths are validated by the native host when you finish.</p>
      <div className="row">
        <input value={candidatePath} onChange={(event) => setCandidatePath(event.target.value)}
          onKeyDown={(event) => { if (event.key === "Enter") { event.preventDefault(); addGamePath(); } }}
          placeholder="Absolute game directory path" aria-label="Game directory path" />
        <button disabled={!canAdd} onClick={addGamePath}>Add path</button>
      </div>
      {gamePaths.length === 0 ? <p className="muted">No game paths configured.</p> : <ul className="path-list">
        {gamePaths.map((path) => <li key={path}><code>{path}</code><button onClick={() => setGamePaths((paths) => paths.filter((item) => item !== path))}>Remove</button></li>)}
      </ul>}
      <div className="card">
        <h3>Graphic packs and mods</h3>
        <p>Download community packs or install a custom pack, then configure presets for your games.</p>
        <button onClick={() => void openWindow("graphic-packs").catch((reason: unknown) => setError(String(reason)))}>Open Graphic Packs</button>
      </div>
    </section> : <section className="settings-section">
      <h2>Startup preferences</h2>
      <label className="check-row"><input type="checkbox" checked={startFullscreen} disabled={model.fullscreenOverride !== null} onChange={(event) => setStartFullscreen(event.target.checked)} />Start games in fullscreen</label>
      {model.fullscreenOverride !== null && <p className="muted">Fullscreen startup is controlled by the command line for this session.</p>}
      <label className="check-row"><input type="checkbox" checked={openPad} onChange={(event) => setOpenPad(event.target.checked)} />Open the separate GamePad view when a game starts</label>
      <label className="check-row"><input type="checkbox" checked={checkUpdates} disabled={!model.updateChecksSupported} onChange={(event) => setCheckUpdates(event.target.checked)} />Automatically check for updates</label>
      {!model.updateChecksSupported && <p className="muted">Automatic update checks are managed by this Linux package.</p>}
      <div className="card"><h3>Controller setup</h3><p>Configure each player's emulated controller, devices, profiles, and mappings before launching a game.</p><button onClick={() => void openWindow("input-settings").catch((reason: unknown) => setError(String(reason)))}>Open Input Settings</button></div>
    </section>}

    <footer className="actions">
      <button onClick={() => void invoke("window.close")} disabled={busy}>Cancel</button>
      {page === 1 && <button onClick={() => setPage(0)} disabled={busy}>Previous</button>}
      {page === 0 ? <button className="primary" onClick={() => setPage(1)}>Next</button> :
        <button className="primary" disabled={busy || model.titleRunning} onClick={() => void finish()}>{busy ? "Applying…" : "Finish"}</button>}
    </footer>
  </main>;
}
