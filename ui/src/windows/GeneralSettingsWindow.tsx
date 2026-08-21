import { useCallback, useEffect, useMemo, useState } from "react";
import type { FrontendSettings } from "../bridge/contracts";
import { invoke } from "../bridge/native";
import { openWindow } from "../bridge/windows";

export function GeneralSettingsWindow() {
  const [model, setModel] = useState<FrontendSettings>();
  const [gamePaths, setGamePaths] = useState<string[]>([]);
  const [candidatePath, setCandidatePath] = useState("");
  const [startFullscreen, setStartFullscreen] = useState(false);
  const [openPad, setOpenPad] = useState(false);
  const [checkUpdates, setCheckUpdates] = useState(false);
  const [saveScreenshots, setSaveScreenshots] = useState(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [saved, setSaved] = useState(false);
  const install = useCallback((snapshot: FrontendSettings) => {
    setModel(snapshot);
    setGamePaths(snapshot.gamePaths);
    setStartFullscreen(snapshot.startFullscreen);
    setOpenPad(snapshot.openPad);
    setCheckUpdates(snapshot.checkUpdates);
    setSaveScreenshots(snapshot.saveScreenshots);
  }, []);
  const load = useCallback(
    async () => install(await invoke("settings.getFrontend")),
    [install],
  );
  useEffect(() => {
    void load().catch((reason: unknown) => setError(String(reason)));
  }, [load]);
  const normalizedCandidate = candidatePath.trim();
  const canAdd = useMemo(
    () =>
      normalizedCandidate.length > 0 &&
      !gamePaths.includes(normalizedCandidate),
    [gamePaths, normalizedCandidate],
  );
  const dirty = model
    ? gamePaths.join("\0") !== model.gamePaths.join("\0") ||
      startFullscreen !== model.startFullscreen ||
      openPad !== model.openPad ||
      checkUpdates !== model.checkUpdates ||
      saveScreenshots !== model.saveScreenshots
    : false;
  async function apply() {
    if (!model) return;
    setBusy(true);
    setError("");
    setSaved(false);
    try {
      const result = await invoke("settings.applyFrontend", {
        revision: model.revision,
        gamePaths,
        startFullscreen,
        openPad,
        checkUpdates,
        saveScreenshots,
        completeSetup: false,
      });
      if (!result.ok) {
        install(result.snapshot);
        setError(result.diagnostic || "Settings could not be applied.");
        return;
      }
      install(result.snapshot);
      setSaved(true);
    } catch (reason) {
      setError(String(reason));
      try {
        install(await invoke("settings.getFrontend"));
      } catch {
        /* Preserve apply error. */
      }
    } finally {
      setBusy(false);
    }
  }
  if (!model)
    return (
      <main className="role-window">
        <h1>General Settings</h1>
        <p>{error || "Loading settings…"}</p>
      </main>
    );
  return (
    <main className="role-window manager-window">
      <header>
        <div>
          <span className="eyebrow">Library & startup</span>
          <h1>General Settings</h1>
        </div>
        <div className="button-row">
          <button
            disabled={busy || !dirty || model.titleRunning}
            onClick={() => void apply()}
          >
            Apply
          </button>
          <button onClick={() => void invoke("window.close")}>Close</button>
        </div>
      </header>
      {error && (
        <div className="notice error" role="alert">
          {error}
        </div>
      )}
      {saved && (
        <div className="notice" role="status">
          Settings saved.
        </div>
      )}
      {model.titleRunning && (
        <div className="notice">
          Stop the running title before changing these settings.
        </div>
      )}
      <section className="editor-panel">
        <h2>Game paths</h2>
        <p>
          Directories are canonicalized and validated by the native settings
          service when applied.
        </p>
        <div className="button-row">
          <input
            disabled={busy || model.titleRunning}
            value={candidatePath}
            placeholder="Absolute game directory path"
            onChange={(event) => setCandidatePath(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === "Enter" && canAdd) {
                event.preventDefault();
                setGamePaths((paths) => [...paths, normalizedCandidate]);
                setCandidatePath("");
              }
            }}
          />
          <button
            disabled={!canAdd || busy || model.titleRunning}
            onClick={() => {
              setGamePaths((paths) => [...paths, normalizedCandidate]);
              setCandidatePath("");
            }}
          >
            Add
          </button>
        </div>
        {gamePaths.length === 0 ? (
          <p className="muted">No game paths configured.</p>
        ) : (
          <ul className="path-list">
            {gamePaths.map((path) => (
              <li key={path}>
                <code>{path}</code>
                <button
                  disabled={busy || model.titleRunning}
                  onClick={() =>
                    setGamePaths((paths) =>
                      paths.filter((item) => item !== path),
                    )
                  }
                >
                  Remove
                </button>
              </li>
            ))}
          </ul>
        )}
      </section>
      <section className="editor-panel">
        <h2>Startup & screenshots</h2>
        <div className="form-grid">
          <label className="check-row">
            <input
              type="checkbox"
              checked={startFullscreen}
              disabled={
                busy || model.titleRunning || model.fullscreenOverride !== null
              }
              onChange={(event) => setStartFullscreen(event.target.checked)}
            />
            Start games in fullscreen
          </label>
          <label className="check-row">
            <input
              type="checkbox"
              checked={openPad}
              disabled={busy || model.titleRunning}
              onChange={(event) => setOpenPad(event.target.checked)}
            />
            Open the separate GamePad view
          </label>
          <label className="check-row">
            <input
              type="checkbox"
              checked={checkUpdates}
              disabled={
                busy || model.titleRunning || !model.updateChecksSupported
              }
              onChange={(event) => setCheckUpdates(event.target.checked)}
            />
            Automatically check for updates
          </label>
          <label className="check-row">
            <input
              type="checkbox"
              checked={saveScreenshots}
              disabled={busy || model.titleRunning}
              onChange={(event) => setSaveScreenshots(event.target.checked)}
            />
            Save screenshots to disk instead of copying them to the clipboard
          </label>
        </div>
        {model.fullscreenOverride !== null && (
          <p className="muted">
            Fullscreen startup is controlled by the command line.
          </p>
        )}
        {!model.updateChecksSupported && (
          <p className="muted">Update checks are managed by this package.</p>
        )}
      </section>
      <section className="editor-panel">
        <h2>Related settings</h2>
        <div className="button-row">
          <button
            onClick={() =>
              void openWindow("input-settings").catch((reason: unknown) =>
                setError(String(reason)),
              )
            }
          >
            Input Settings
          </button>
          <button
            onClick={() =>
              void openWindow("account-manager").catch((reason: unknown) =>
                setError(String(reason)),
              )
            }
          >
            Account Manager
          </button>
          <button
            onClick={() =>
              void openWindow("graphic-packs").catch((reason: unknown) =>
                setError(String(reason)),
              )
            }
          >
            Graphic Packs
          </button>
        </div>
      </section>
      <footer className="button-row">
        <button
          disabled={busy || !dirty || model.titleRunning}
          onClick={() => void apply()}
        >
          Apply
        </button>
        <button
          disabled={busy || !dirty}
          onClick={() => {
            install(model);
            setError("");
          }}
        >
          Revert
        </button>
      </footer>
    </main>
  );
}
