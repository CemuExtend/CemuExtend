import { useCallback, useEffect, useRef, useState } from "react";
import type {
  EmulatedControllerType,
  InputDeviceCandidate,
  InputSettingsModel,
  PhysicalControllerSettings,
} from "../bridge/contracts";
import { invoke } from "../bridge/native";
import { Modal } from "../components/Modal";
import { getReferencePreviewScreen } from "../dev/referencePreview";
import { translateFormat } from "../i18n/runtime";

const controllerTypes: Array<[EmulatedControllerType, string]> = [
  ["disabled", "Disabled"],
  ["gamePad", "Wii U GamePad"],
  ["proController", "Wii U Pro Controller"],
  ["classicController", "Classic Controller"],
  ["wiimote", "Wiimote"],
];

export function InputSettingsWindow() {
  const previewIndex = getReferencePreviewScreen()?.index;
  const [previewModalOpen, setPreviewModalOpen] = useState(
    previewIndex === 27 || previewIndex === 28 || previewIndex === 80,
  );
  const [model, setModel] = useState<InputSettingsModel>();
  const [playerIndex, setPlayerIndex] = useState(0);
  const [api, setApi] = useState("");
  const [candidates, setCandidates] = useState<InputDeviceCandidate[]>([]);
  const [profile, setProfile] = useState("");
  const [captureTokens, setCaptureTokens] = useState<Record<string, number>>(
    {},
  );
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const requestSequence = useRef(0);
  const scanSequence = useRef(0);
  const load = useCallback(async () => {
    const sequence = ++requestSequence.current;
    const next = await invoke("input.getModel");
    if (sequence === requestSequence.current) setModel(next);
  }, []);
  useEffect(() => {
    void load().catch((reason: unknown) => setError(String(reason)));
  }, [load]);
  useEffect(() => {
    const timer = window.setInterval(() => {
      if (!busy)
        void load().catch((reason: unknown) => setError(String(reason)));
    }, 1000);
    return () => window.clearInterval(timer);
  }, [busy, load]);
  useEffect(() => {
    if (model && !api) setApi(model.availableApis[0] ?? "");
  }, [api, model]);
  const player = model?.players[playerIndex];
  const mutate = async (operation: () => Promise<InputSettingsModel>) => {
    const sequence = ++requestSequence.current;
    setBusy(true);
    setError("");
    try {
      const next = await operation();
      if (sequence === requestSequence.current) setModel(next);
    } catch (reason) {
      if (sequence === requestSequence.current) setError(String(reason));
    } finally {
      if (sequence === requestSequence.current) setBusy(false);
    }
  };
  const capture = async (mappingId: number, token: number) => {
    const sequence = ++requestSequence.current;
    setBusy(true);
    setError("");
    try {
      const deadline = Date.now() + 5000;
      while (Date.now() < deadline) {
        if (sequence !== requestSequence.current) return;
        const button = await invoke("input.captureButton", { token });
        if (button) {
          const next = await invoke("input.setMapping", {
            player: playerIndex,
            mappingId,
            controllerToken: token,
            buttonId: button.id,
          });
          if (sequence === requestSequence.current) setModel(next);
          return;
        }
        await new Promise((resolve) => setTimeout(resolve, 50));
      }
      if (sequence === requestSequence.current)
        setError(
          "No input was detected. Try again and press or move the control within five seconds.",
        );
    } catch (reason) {
      if (sequence === requestSequence.current) setError(String(reason));
    } finally {
      if (sequence === requestSequence.current) setBusy(false);
    }
  };
  if (!model || !player)
    return (
      <main className="role-window">
        <div className="spinner" aria-label="Loading" />
        {error && <div className="notice error">{error}</div>}
      </main>
    );
  return (
    <main className="role-window manager-window">
      <header>
        <div>
          <span className="eyebrow">Controllers & mappings</span>
          <h1>Input Settings</h1>
        </div>
        <div className="button-row">
          <button disabled={busy} onClick={() => void load()}>
            Refresh
          </button>
        </div>
      </header>
      {error && (
        <div className="notice error" role="alert">
          {error}
        </div>
      )}
      <nav className="button-row" aria-label="Players">
        {model.players.map((item) => (
          <button
            disabled={busy}
            className={item.player === playerIndex ? "primary" : ""}
            key={item.player}
            onClick={() => setPlayerIndex(item.player)}
          >
            Player {item.player + 1}
          </button>
        ))}
      </nav>
      {player.gameProfileLocked && (
        <div className="notice">
          This player is controlled by the running game profile. Persistent
          changes are locked.
        </div>
      )}
      <section className="editor-panel">
        <div className="form-grid">
          <label>
            Emulated controller
            <select
              disabled={busy || player.gameProfileLocked}
              value={player.type}
              onChange={(event) =>
                void mutate(() =>
                  invoke("input.setType", {
                    player: player.player,
                    type: event.target.value as EmulatedControllerType,
                    preserveDevices: true,
                  }),
                )
              }
            >
              {controllerTypes.map(([value, label]) => (
                <option key={value} value={value}>
                  {label}
                </option>
              ))}
            </select>
          </label>
          <label>
            Profile
            <input
              list="input-profiles"
              value={profile}
              maxLength={64}
              placeholder="Choose or type a new profile"
              onChange={(event) => setProfile(event.target.value)}
            />
            <datalist id="input-profiles">
              {model.profiles.map((name) => (
                <option key={name} value={name} />
              ))}
            </datalist>
          </label>
          <div className="button-row">
            <button
              disabled={!profile || busy || player.gameProfileLocked}
              onClick={() =>
                void mutate(() =>
                  invoke("input.profileLoad", {
                    player: player.player,
                    profile,
                  }),
                )
              }
            >
              Load
            </button>
            <button
              disabled={!profile || busy || player.gameProfileLocked}
              onClick={() =>
                void mutate(() =>
                  invoke("input.profileSave", {
                    player: player.player,
                    profile,
                  }),
                )
              }
            >
              Save
            </button>
            <button
              className="danger"
              disabled={!profile || busy}
              onClick={() =>
                void mutate(() => invoke("input.profileDelete", { profile }))
              }
            >
              Delete
            </button>
          </div>
        </div>
      </section>
      {player.type !== "disabled" && (
        <>
          <section className="editor-panel">
            <h2>Add physical controller</h2>
            <div className="button-row">
              <select
                value={api}
                onChange={(event) => {
                  ++scanSequence.current;
                  setApi(event.target.value);
                  setCandidates([]);
                }}
              >
                {model.availableApis.map((value) => (
                  <option key={value}>{value}</option>
                ))}
              </select>
              <button
                disabled={!api || busy}
                onClick={() => {
                  const sequence = ++scanSequence.current;
                  setBusy(true);
                  void invoke("input.enumerate", { api })
                    .then((next) => {
                      if (sequence === scanSequence.current)
                        setCandidates(next);
                    })
                    .catch((reason: unknown) => setError(String(reason)))
                    .finally(() => setBusy(false));
                }}
              >
                Scan
              </button>
            </div>
            <div className="selection-list">
              {candidates.map((candidate) => (
                <button
                  key={candidate.token}
                  disabled={busy}
                  onClick={() =>
                    void mutate(() =>
                      invoke("input.addDevice", {
                        player: player.player,
                        token: candidate.token,
                      }),
                    )
                  }
                >
                  <strong>{candidate.displayName}</strong>
                  <span>
                    {candidate.api} ·{" "}
                    {candidate.connected ? "Connected" : "Available"}
                  </span>
                </button>
              ))}
            </div>
          </section>
          <section className="editor-panel">
            <h2>Physical controllers</h2>
            {player.controllers.length === 0 ? (
              <p>No physical controllers assigned.</p>
            ) : (
              player.controllers.map((controller) => (
                <ControllerEditor
                  key={`${controller.token}:${model.generation}`}
                  controller={controller}
                  disabled={busy || player.gameProfileLocked}
                  onMutate={mutate}
                  player={player.player}
                />
              ))
            )}
          </section>
          <section className="editor-panel">
            <div className="pack-heading">
              <h2>Mappings</h2>
              <button
                disabled={busy || player.gameProfileLocked}
                onClick={() =>
                  void mutate(() =>
                    invoke("input.clearMapping", { player: player.player }),
                  )
                }
              >
                Clear all
              </button>
            </div>
            <div className="form-grid mapping-grid">
              {player.mappings.map((mapping) => {
                const captureKey = `${player.player}:${mapping.mappingId}`;
                const token =
                  captureTokens[captureKey] ??
                  mapping.controllerToken ??
                  player.controllers[0]?.token ??
                  0;
                return (
                  <div className="mapping-row" key={mapping.mappingId}>
                    <span className="mapping-row__name">{mapping.label}</span>
                    <select
                      className="mapping-row__controller"
                      aria-label={translateFormat("Controller for {mapping}", {
                        mapping: mapping.label,
                      })}
                      disabled={
                        busy ||
                        player.gameProfileLocked ||
                        player.controllers.length === 0
                      }
                      value={token}
                      onChange={(event) =>
                        setCaptureTokens((current) => ({
                          ...current,
                          [captureKey]: Number(event.target.value),
                        }))
                      }
                    >
                      {player.controllers.map((controller) => (
                        <option key={controller.token} value={controller.token}>
                          {controller.displayName}
                        </option>
                      ))}
                    </select>
                    <span className="mapping-row__binding">
                      {mapping.binding || "Unassigned"}
                    </span>
                    <div className="button-row">
                      <button
                        disabled={!token || busy || player.gameProfileLocked}
                        onClick={() => void capture(mapping.mappingId, token)}
                      >
                        Capture
                      </button>
                      <button
                        disabled={
                          !mapping.binding || busy || player.gameProfileLocked
                        }
                        onClick={() =>
                          void mutate(() =>
                            invoke("input.clearMapping", {
                              player: player.player,
                              mappingId: mapping.mappingId,
                            }),
                          )
                        }
                      >
                        Clear
                      </button>
                    </div>
                  </div>
                );
              })}
            </div>
          </section>
        </>
      )}
      {previewModalOpen && previewIndex === 27 && (
        <Modal
          title="Add input device"
          onClose={() => setPreviewModalOpen(false)}
        >
          <div className="field-stack">
            <label>
              <span>Input API</span>
              <select defaultValue="SDL">
                <option>SDL</option>
                <option>Keyboard</option>
              </select>
            </label>
            <div className="selection-list">
              <button aria-selected="true">
                <strong>Wireless Controller</strong>
                <span>SDL · Connected</span>
              </button>
              <button>
                <strong>Keyboard</strong>
                <span>Keyboard · Available</span>
              </button>
            </div>
          </div>
          <footer>
            <button onClick={() => setPreviewModalOpen(false)}>Cancel</button>
            <button
              className="primary"
              onClick={() => setPreviewModalOpen(false)}
            >
              Add device
            </button>
          </footer>
        </Modal>
      )}
      {previewModalOpen && previewIndex === 28 && (
        <Modal
          title="Controller calibration"
          onClose={() => setPreviewModalOpen(false)}
        >
          <div className="calibration-preview">
            <div className="calibration-preview__axis">
              <i />
              <span>Live stick position</span>
            </div>
            <label className="field-stack">
              <span>Deadzone · 15%</span>
              <input type="range" defaultValue="15" />
            </label>
            <label className="field-stack">
              <span>Range · 100%</span>
              <input type="range" defaultValue="100" />
            </label>
          </div>
          <footer>
            <button onClick={() => setPreviewModalOpen(false)}>Cancel</button>
            <button
              className="primary"
              onClick={() => setPreviewModalOpen(false)}
            >
              Apply calibration
            </button>
          </footer>
        </Modal>
      )}
      {previewModalOpen && previewIndex === 80 && (
        <Modal
          title="Bluetooth pairing"
          onClose={() => setPreviewModalOpen(false)}
        >
          <div className="notice">
            Make the controller discoverable, then select it below.
          </div>
          <div className="selection-list">
            <button aria-selected="true">
              <strong>Wireless Controller</strong>
              <span>Ready to pair · 84:30:95:12:48:CA</span>
            </button>
          </div>
          <footer>
            <button onClick={() => setPreviewModalOpen(false)}>Close</button>
            <button
              className="primary"
              onClick={() => setPreviewModalOpen(false)}
            >
              Pair
            </button>
          </footer>
        </Modal>
      )}
    </main>
  );
}

function ControllerEditor({
  controller,
  disabled,
  onMutate,
  player,
}: {
  controller: InputSettingsModel["players"][number]["controllers"][number];
  disabled: boolean;
  onMutate: (operation: () => Promise<InputSettingsModel>) => Promise<void>;
  player: number;
}) {
  const [settings, setSettings] = useState<PhysicalControllerSettings>(
    controller.settings,
  );
  const number = (
    group: "axis" | "rotation" | "trigger",
    key: "deadzone" | "range",
    value: number,
  ) =>
    setSettings((current) => ({
      ...current,
      [group]: { ...current[group], [key]: value },
    }));
  return (
    <article className="pack-card">
      <div className="pack-heading">
        <div>
          <h3>{controller.displayName}</h3>
          <span>
            {controller.api} ·{" "}
            {controller.connected ? "Connected" : "Disconnected"}
            {controller.wiimoteExtension
              ? ` · ${controller.wiimoteExtension}`
              : ""}
          </span>
        </div>
        <div className="button-row">
          <button
            disabled={disabled || controller.connected}
            onClick={() =>
              void onMutate(() =>
                invoke("input.connectDevice", { token: controller.token }),
              )
            }
          >
            Connect
          </button>
          <button
            disabled={disabled}
            onClick={() =>
              void onMutate(() =>
                invoke("input.calibrate", { token: controller.token }),
              )
            }
          >
            Calibrate
          </button>
          <button
            className="danger"
            disabled={disabled}
            onClick={() =>
              void onMutate(() =>
                invoke("input.removeDevice", {
                  player,
                  token: controller.token,
                }),
              )
            }
          >
            Remove
          </button>
        </div>
      </div>
      <div className="form-grid">
        {(["axis", "rotation", "trigger"] as const).flatMap((group) =>
          (["deadzone", "range"] as const).map((key) => (
            <label key={`${group}-${key}`}>
              {group} {key}
              <input
                type="number"
                min={key === "deadzone" ? 0 : 0.5}
                max={key === "deadzone" ? 1 : 2}
                step={0.01}
                value={settings[group][key]}
                onChange={(event) =>
                  number(group, key, event.target.valueAsNumber)
                }
              />
            </label>
          )),
        )}
        <label>
          Rumble
          <input
            disabled={!controller.hasRumble}
            type="number"
            min={0}
            max={1}
            step={0.05}
            value={settings.rumble}
            onChange={(event) =>
              setSettings((current) => ({
                ...current,
                rumble: event.target.valueAsNumber,
              }))
            }
          />
        </label>
        <label>
          <input
            disabled={!controller.hasMotion}
            type="checkbox"
            checked={settings.motion}
            onChange={(event) =>
              setSettings((current) => ({
                ...current,
                motion: event.target.checked,
              }))
            }
          />{" "}
          Motion
        </label>
        {settings.packetDelay !== undefined && (
          <label>
            Wiimote packet delay
            <input
              type="number"
              min={1}
              max={100}
              value={settings.packetDelay}
              onChange={(event) =>
                setSettings((current) => ({
                  ...current,
                  packetDelay: event.target.valueAsNumber,
                }))
              }
            />
          </label>
        )}
      </div>
      <button
        disabled={disabled}
        onClick={() =>
          void onMutate(() =>
            invoke("input.setDeviceSettings", {
              token: controller.token,
              settings,
            }),
          )
        }
      >
        Apply device settings
      </button>
    </article>
  );
}
