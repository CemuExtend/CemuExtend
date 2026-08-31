import { useEffect, useMemo, useState } from "react";
import type { FrontendSettings } from "../bridge/contracts";
import { invoke } from "../bridge/native";
import { useWorkspaceNavigation } from "../app/workspaceNavigation";
import {
  getUiLanguage,
  setUiLanguage,
  translate,
  uiLanguages,
} from "../i18n/runtime";
import { getReferencePreviewScreen } from "../dev/referencePreview";

export function GettingStartedWindow() {
  const navigate = useWorkspaceNavigation();
  const [model, setModel] = useState<FrontendSettings | null>(null);
  const [gamePaths, setGamePaths] = useState<string[]>([]);
  const [candidatePath, setCandidatePath] = useState("");
  const [startFullscreen, setStartFullscreen] = useState(false);
  const [openPad, setOpenPad] = useState(false);
  const [checkUpdates, setCheckUpdates] = useState(true);
  const [language, setLanguage] = useState(getUiLanguage());
  const [page, setPage] = useState<0 | 1>(
    getReferencePreviewScreen()?.index === 18 ? 1 : 0,
  );
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  useEffect(() => {
    void Promise.all([invoke("settings.getFrontend"), invoke("language.get")])
      .then(([value, languageState]) => {
        setModel(value);
        setGamePaths(value.gamePaths);
        setStartFullscreen(value.startFullscreen);
        setOpenPad(value.openPad);
        setCheckUpdates(value.checkUpdates);
        setLanguage(languageState.language);
        setUiLanguage(languageState.language);
      })
      .catch((reason: unknown) => setError(String(reason)));
  }, []);

  const normalizedCandidate = candidatePath.trim();
  const canAdd = useMemo(
    () =>
      normalizedCandidate.length > 0 &&
      !gamePaths.includes(normalizedCandidate),
    [gamePaths, normalizedCandidate],
  );

  function addGamePath() {
    if (!canAdd) return;
    setGamePaths((paths) => [...paths, normalizedCandidate]);
    setCandidatePath("");
  }

  async function changeLanguage(nextLanguage: string) {
    setBusy(true);
    setError("");
    try {
      const result = await invoke("language.set", { language: nextLanguage });
      setLanguage(result.language);
      setUiLanguage(result.language);
    } catch (reason) {
      setError(String(reason));
    } finally {
      setBusy(false);
    }
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
        saveScreenshots: model.saveScreenshots,
        crashDump: model.crashDump,
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
      await navigate("general-settings");
    } catch (reason) {
      setError(String(reason));
      try {
        const latest = await invoke("settings.getFrontend");
        setModel(latest);
        setGamePaths(latest.gamePaths);
        setStartFullscreen(latest.startFullscreen);
        setOpenPad(latest.openPad);
        setCheckUpdates(latest.checkUpdates);
      } catch {
        /* Preserve the actionable apply error. */
      }
    } finally {
      setBusy(false);
    }
  }

  if (!model)
    return (
      <main className="tool-window">
        <h1>Getting Started</h1>
        <p>{error || "Loading settings…"}</p>
      </main>
    );

  return (
    <main className="tool-window wizard-window">
      <header>
        <div>
          <p className="eyebrow">First-run setup</p>
          <h1>Getting Started</h1>
        </div>
        <span>{translate(`Step ${page + 1} of 2`)}</span>
      </header>
      <div className="wizard-scroll">
        {error && (
          <p className="error" role="alert">
            {error}
          </p>
        )}
        {model.titleRunning && (
          <p className="warning">
            Stop the running title before applying setup changes.
          </p>
        )}
        {getReferencePreviewScreen()?.index === 17 && (
          <div className="notice">
            Choose the interface language and add at least one directory
            containing Wii U titles.
          </div>
        )}
        {getReferencePreviewScreen()?.index === 18 && (
          <div className="notice">
            These options can be changed later in General Settings.
          </div>
        )}

        {page === 0 ? (
          <section className="settings-section">
            <div className="card">
              <h2>Interface language</h2>
              <label className="field-stack">
                <span>Language</span>
                <select
                  value={language}
                  disabled={busy}
                  onChange={(event) => void changeLanguage(event.target.value)}
                >
                  {uiLanguages.map((entry) => (
                    <option key={entry.code} value={entry.code}>
                      {entry.name}
                    </option>
                  ))}
                </select>
              </label>
            </div>
            <h2>Game library</h2>
            <p>
              Add directories that contain Wii U games, updates, or DLC. Paths
              are validated by the native host when you finish.
            </p>
            <div className="row">
              <input
                value={candidatePath}
                onChange={(event) => setCandidatePath(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === "Enter") {
                    event.preventDefault();
                    addGamePath();
                  }
                }}
                placeholder="Absolute game directory path"
                aria-label="Game directory path"
              />
              <button disabled={!canAdd} onClick={addGamePath}>
                Add path
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
            <div className="card">
              <h3>Graphic packs and mods</h3>
              <p>
                Download community packs or install a custom pack, then
                configure presets for your games.
              </p>
              <button
                onClick={() =>
                  void navigate("graphic-packs").catch((reason: unknown) =>
                    setError(String(reason)),
                  )
                }
              >
                Open Graphic Packs
              </button>
            </div>
          </section>
        ) : (
          <section className="settings-section">
            <h2>Startup preferences</h2>
            <label className="check-row">
              <input
                type="checkbox"
                checked={startFullscreen}
                disabled={model.fullscreenOverride !== null}
                onChange={(event) => setStartFullscreen(event.target.checked)}
              />
              Start games in fullscreen
            </label>
            {model.fullscreenOverride !== null && (
              <p className="muted">
                Fullscreen startup is controlled by the command line for this
                session.
              </p>
            )}
            <label className="check-row">
              <input
                type="checkbox"
                checked={openPad}
                onChange={(event) => setOpenPad(event.target.checked)}
              />
              Open the separate GamePad view when a game starts
            </label>
            <label className="check-row">
              <input
                type="checkbox"
                checked={checkUpdates}
                disabled={!model.updateChecksSupported}
                onChange={(event) => setCheckUpdates(event.target.checked)}
              />
              Automatically check for updates
            </label>
            {!model.updateChecksSupported && (
              <p className="muted">
                Automatic update checks are managed by this Linux package.
              </p>
            )}
            <div className="card">
              <h3>Controller setup</h3>
              <p>
                Configure each player's emulated controller, devices, profiles,
                and mappings before launching a game.
              </p>
              <button
                onClick={() =>
                  void navigate("input-settings").catch((reason: unknown) =>
                    setError(String(reason)),
                  )
                }
              >
                Open Input Settings
              </button>
            </div>
          </section>
        )}
      </div>

      <footer className="actions">
        <button
          onClick={() => void navigate("general-settings")}
          disabled={busy}
        >
          Cancel
        </button>
        {page === 1 && (
          <button onClick={() => setPage(0)} disabled={busy}>
            Previous
          </button>
        )}
        {page === 0 ? (
          <button className="primary" onClick={() => setPage(1)}>
            Next
          </button>
        ) : (
          <button
            className="primary"
            disabled={busy || model.titleRunning}
            onClick={() => void finish()}
          >
            {busy ? "Applying…" : "Finish"}
          </button>
        )}
      </footer>
    </main>
  );
}
