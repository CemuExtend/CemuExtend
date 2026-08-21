import { useCallback, useEffect, useMemo, useState } from "react";
import type {
  HotkeyAction,
  HotkeyBinding,
  HotkeySettingsModel,
} from "../bridge/contracts";
import { invoke } from "../bridge/native";
import { hotkeyKeyboardLabel, hotkeyUsageForCode } from "./hotkeyKeys";

const actions: Array<[HotkeyAction, string]> = [
  ["toggleFullscreen", "Toggle fullscreen"],
  ["toggleFullscreenAlternative", "Toggle fullscreen (alternative)"],
  ["exitFullscreen", "Exit fullscreen"],
  ["takeScreenshot", "Take screenshot"],
  ["toggleFastForward", "Toggle fast-forward"],
  ["endEmulation", "End emulation"],
  ["exitApplication", "Exit application"],
];

type ControllerCapture = HotkeyAction | "modifier";
const wireBinding = (binding: HotkeyBinding) => ({
  action: binding.action,
  keyboardUsage: binding.keyboardUsage,
  keyboardModifiers: binding.keyboardModifiers,
  controllerButton: binding.controllerButton,
});

export function HotkeySettingsWindow() {
  const [model, setModel] = useState<HotkeySettingsModel>();
  const [bindings, setBindings] = useState<HotkeyBinding[]>([]);
  const [modifier, setModifier] = useState<number | null>(null);
  const [modifierLabel, setModifierLabel] = useState("");
  const [keyboardCapture, setKeyboardCapture] = useState<HotkeyAction>();
  const [controllerCapture, setControllerCapture] =
    useState<ControllerCapture>();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [saved, setSaved] = useState(false);
  const install = useCallback((snapshot: HotkeySettingsModel) => {
    setModel(snapshot);
    setBindings(snapshot.bindings.map((binding) => ({ ...binding })));
    setModifier(snapshot.controllerModifier);
    setModifierLabel(snapshot.controllerModifierLabel);
  }, []);
  useEffect(() => {
    void invoke("hotkeys.get")
      .then(install)
      .catch((reason: unknown) => setError(String(reason)));
  }, [install]);
  useEffect(() => {
    if (!keyboardCapture) return;
    const capture = (event: KeyboardEvent) => {
      event.preventDefault();
      event.stopPropagation();
      const usage = hotkeyUsageForCode(event.code);
      if (!usage) {
        setError(
          `The key ${event.code || event.key} cannot be used as a hotkey.`,
        );
        return;
      }
      const modifiers =
        (event.ctrlKey ? 1 : 0) |
        (event.shiftKey ? 2 : 0) |
        (event.altKey ? 4 : 0) |
        (event.metaKey ? 8 : 0);
      setBindings((current) =>
        current.map((binding) =>
          binding.action === keyboardCapture
            ? { ...binding, keyboardUsage: usage, keyboardModifiers: modifiers }
            : binding,
        ),
      );
      setKeyboardCapture(undefined);
      setError("");
      setSaved(false);
    };
    window.addEventListener("keydown", capture, true);
    return () => window.removeEventListener("keydown", capture, true);
  }, [keyboardCapture]);
  const original = useMemo(
    () =>
      model
        ? JSON.stringify({
            modifier: model.controllerModifier,
            bindings: model.bindings.map(wireBinding),
          })
        : "",
    [model],
  );
  const draft = useMemo(
    () => JSON.stringify({ modifier, bindings: bindings.map(wireBinding) }),
    [bindings, modifier],
  );
  const dirty = original !== draft;
  const clearKeyboard = (action: HotkeyAction) =>
    setBindings((current) =>
      current.map((binding) =>
        binding.action === action
          ? { ...binding, keyboardUsage: 0, keyboardModifiers: 0 }
          : binding,
      ),
    );
  const clearController = (action: HotkeyAction) =>
    setBindings((current) =>
      current.map((binding) =>
        binding.action === action
          ? { ...binding, controllerButton: null, controllerLabel: "" }
          : binding,
      ),
    );
  async function captureController(target: ControllerCapture) {
    if (!model?.controller) return;
    setBusy(true);
    setControllerCapture(target);
    setError("");
    try {
      const deadline = Date.now() + 5000;
      while (Date.now() < deadline) {
        const captured = await invoke("input.captureButton", {
          token: model.controller.token,
        });
        if (captured) {
          if (target === "modifier") {
            setModifier(captured.id);
            setModifierLabel(captured.label);
          } else
            setBindings((current) =>
              current.map((binding) =>
                binding.action === target
                  ? {
                      ...binding,
                      controllerButton: captured.id,
                      controllerLabel: captured.label,
                    }
                  : binding,
              ),
            );
          setSaved(false);
          return;
        }
        await new Promise((resolve) => window.setTimeout(resolve, 50));
      }
      setError("No controller input was detected within five seconds.");
    } catch (reason) {
      setError(String(reason));
    } finally {
      setControllerCapture(undefined);
      setBusy(false);
    }
  }
  async function apply() {
    if (!model) return;
    setBusy(true);
    setError("");
    setSaved(false);
    try {
      const result = await invoke("hotkeys.apply", {
        revision: model.revision,
        controllerModifier: modifier,
        bindings: bindings.map(wireBinding),
      });
      install(result.snapshot);
      if (!result.ok) {
        setError(
          result.diagnostic || `Hotkey settings failed: ${result.error}`,
        );
        return;
      }
      setSaved(true);
    } catch (reason) {
      setError(String(reason));
    } finally {
      setBusy(false);
    }
  }
  if (!model)
    return (
      <main className="role-window">
        <h1>Hotkey Settings</h1>
        <p>{error || "Loading hotkeys…"}</p>
      </main>
    );
  return (
    <main className="role-window manager-window">
      <header>
        <div>
          <span className="eyebrow">Global shortcuts</span>
          <h1>Hotkey Settings</h1>
        </div>
        <div className="button-row">
          <button disabled={busy || !dirty} onClick={() => void apply()}>
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
          Hotkeys saved.
        </div>
      )}
      <section className="editor-panel">
        <h2>Controller modifier</h2>
        <p>
          Controller shortcuts fire only while this button is held. Capture uses
          Player 1&apos;s first physical controller; the saved shortcut works on
          any connected controller, matching the classic frontend.
        </p>
        <div className="button-row">
          <strong>
            {modifierLabel ||
              (modifier === null ? "Unassigned" : `Button ${modifier}`)}
          </strong>
          <button
            disabled={busy || !model.controller}
            onClick={() => void captureController("modifier")}
          >
            {controllerCapture === "modifier" ? "Listening…" : "Capture"}
          </button>
          <button
            disabled={busy || modifier === null}
            onClick={() => {
              setModifier(null);
              setModifierLabel("");
            }}
          >
            Clear
          </button>
        </div>
        {model.controller ? (
          <p className="muted">
            Capture controller: {model.controller.displayName}
          </p>
        ) : (
          <p className="muted">
            Assign a physical controller to Player 1 to capture controller
            hotkeys.
          </p>
        )}
      </section>
      <section className="editor-panel">
        <div className="hotkey-grid hotkey-header">
          <strong>Action</strong>
          <strong>Keyboard</strong>
          <strong>Controller</strong>
        </div>
        {actions.map(([action, label]) => {
          const binding = bindings.find(
            (candidate) => candidate.action === action,
          );
          if (!binding) return null;
          return (
            <div className="hotkey-grid" key={action}>
              <strong>{label}</strong>
              <div>
                <span>
                  {keyboardCapture === action
                    ? "Press a key…"
                    : hotkeyKeyboardLabel(binding)}
                </span>
                <div className="button-row">
                  <button
                    disabled={busy || Boolean(keyboardCapture)}
                    onClick={() => setKeyboardCapture(action)}
                  >
                    Capture
                  </button>
                  <button
                    disabled={busy || !binding.keyboardUsage}
                    onClick={() => clearKeyboard(action)}
                  >
                    Clear
                  </button>
                </div>
              </div>
              <div>
                <span>
                  {binding.controllerLabel ||
                    (binding.controllerButton === null
                      ? "Unassigned"
                      : `Button ${binding.controllerButton}`)}
                </span>
                <div className="button-row">
                  <button
                    disabled={busy || !model.controller}
                    onClick={() => void captureController(action)}
                  >
                    {controllerCapture === action ? "Listening…" : "Capture"}
                  </button>
                  <button
                    disabled={busy || binding.controllerButton === null}
                    onClick={() => clearController(action)}
                  >
                    Clear
                  </button>
                </div>
              </div>
            </div>
          );
        })}
      </section>
      <footer className="button-row">
        <button disabled={busy || !dirty} onClick={() => void apply()}>
          Apply
        </button>
        <button disabled={busy || !dirty} onClick={() => install(model)}>
          Revert
        </button>
      </footer>
    </main>
  );
}
