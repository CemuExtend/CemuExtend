import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { subscribe } from "../../bridge/events";
import type {
  OverlayPosition,
  OverlayTextStyle,
  RuntimeOverlaySnapshot,
} from "../../bridge/contracts";
import { invoke } from "../../bridge/native";
import { overlayColor } from "./runtimeOverlayModel";

const LOWER_KEYS = [
  ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "⌫"],
  ["q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "/"],
  ["a", "s", "d", "f", "g", "h", "j", "k", "l", ":", "'"],
  ["z", "x", "c", "v", "b", "n", "m", ",", ".", "?", "!"],
] as const;
const UPPER_KEYS = [
  ["#", "[", "]", "$", "%", "^", "&", "*", "(", ")", "_", "⌫"],
  ["Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "@"],
  ["A", "S", "D", "F", "G", "H", "J", "K", "L", ";", '"'],
  ["Z", "X", "C", "V", "B", "N", "M", "<", ">", "+", "="],
] as const;

const MIN_OVERLAY_SCALE = 50;
const MAX_OVERLAY_SCALE = 300;
const MAX_OVERLAY_VIEWPORT_SCALE = 4;
const PERCENT_SCALE = 100;

function styleProperties(style: OverlayTextStyle) {
  const scale = Math.min(
    MAX_OVERLAY_SCALE,
    Math.max(MIN_OVERLAY_SCALE, style.scale),
  );
  return {
    color: overlayColor(style.color),
    fontSize: `clamp(${MIN_OVERLAY_SCALE / PERCENT_SCALE}em, ${scale / PERCENT_SCALE}em, ${MAX_OVERLAY_VIEWPORT_SCALE}vmin)`,
  };
}

function Stats({ snapshot }: { snapshot: RuntimeOverlaySnapshot }) {
  const { stats, visibility } = snapshot;
  const lines: Array<[string, string]> = [];
  if (visibility.fps) lines.push(["FPS", stats.fps.toFixed(2)]);
  if (visibility.drawCalls)
    lines.push([
      "Draws/f",
      `${stats.drawCalls.toLocaleString()} (fast: ${stats.fastDrawCalls.toLocaleString()})`,
    ]);
  if (visibility.cpuUsage) lines.push(["CPU", `${stats.cpuUsage.toFixed(2)}%`]);
  if (visibility.cpuPerCore)
    stats.cpuPerCore.forEach((usage, index) =>
      lines.push([`CPU #${index + 1}`, `${usage.toFixed(2)}%`]),
    );
  if (visibility.ramUsage)
    lines.push(["RAM", `${stats.ramUsageMb.toLocaleString()} MB`]);
  if (visibility.vramUsage && stats.vramUsageMb >= 0 && stats.vramTotalMb >= 0)
    lines.push([
      "VRAM",
      `${stats.vramUsageMb.toLocaleString()} / ${stats.vramTotalMb.toLocaleString()} MB`,
    ]);
  if (visibility.debug)
    stats.debugLines.forEach(({ label, value }) => lines.push([label, value]));
  if (!lines.length) return null;
  return (
    <dl className="runtime-overlay__panel runtime-overlay__stats">
      {lines.map(([label, value], index) => (
        <div key={`${label}-${index}`}>
          <dt>{label}</dt>
          <dd>{value}</dd>
        </div>
      ))}
    </dl>
  );
}

function Notices({ snapshot }: { snapshot: RuntimeOverlaySnapshot }) {
  const [receivedAt, setReceivedAt] = useState(() => Date.now());
  const [, setClock] = useState(0);
  useEffect(() => setReceivedAt(Date.now()), [snapshot.sequence]);
  useEffect(() => {
    if (!snapshot.notices.some((notice) => notice.remainingMs > 0)) return;
    const timer = window.setInterval(() => setClock((value) => value + 1), 100);
    return () => window.clearInterval(timer);
  }, [snapshot.notices]);
  const elapsed = Date.now() - receivedAt;
  const visible = snapshot.notices.filter(
    (notice) => notice.remainingMs === 0 || notice.remainingMs > elapsed,
  );
  if (!visible.length) return null;
  return (
    <div className="runtime-overlay__notices" aria-live="polite">
      {visible.map((notice) => (
        <div
          className={`runtime-overlay__panel runtime-overlay__notice runtime-overlay__notice--${notice.kind}`}
          key={notice.id}
        >
          <span className="runtime-overlay__notice-icon" aria-hidden="true">
            {notice.kind === "battery"
              ? "▱"
              : notice.kind === "controller"
                ? "⌁"
                : notice.kind === "account"
                  ? "●"
                  : notice.kind === "shader" || notice.kind === "pipeline"
                    ? "◌"
                    : "◆"}
          </span>
          <span>{notice.text}</span>
        </div>
      ))}
    </div>
  );
}

function ShaderProgress({ snapshot }: { snapshot: RuntimeOverlaySnapshot }) {
  const progress = snapshot.shaderProgress;
  if (!progress.visible) return null;
  const percent = progress.total
    ? Math.min(100, Math.round((progress.current / progress.total) * 100))
    : 0;
  return (
    <section className="runtime-overlay__shader-progress" aria-live="polite">
      <div className="runtime-overlay__shader-card">
        <p>
          {progress.pipelines
            ? "Loading cached pipelines…"
            : "Loading cached shaders…"}
        </p>
        <progress max={Math.max(1, progress.total)} value={progress.current} />
        <strong>
          {progress.current.toLocaleString()} /{" "}
          {progress.total.toLocaleString()} ({percent}%)
        </strong>
      </div>
      {!progress.pipelines && (
        <dl className="runtime-overlay__shader-counts">
          <div>
            <dt>Vertex shaders</dt>
            <dd>{progress.vertexShaders}</dd>
          </div>
          <div>
            <dt>Pixel shaders</dt>
            <dd>{progress.pixelShaders}</dd>
          </div>
          <div>
            <dt>Geometry shaders</dt>
            <dd>{progress.geometryShaders}</dd>
          </div>
        </dl>
      )}
    </section>
  );
}

function SoftwareKeyboard({ snapshot }: { snapshot: RuntimeOverlaySnapshot }) {
  const keyboard = snapshot.keyboard;
  const [shifted, setShifted] = useState(keyboard.shifted);
  useEffect(
    () => setShifted(keyboard.shifted),
    [keyboard.generation, keyboard.shifted],
  );
  const submit = useCallback(
    (keyCode: number) =>
      invoke("overlay.submitKeyboardKey", {
        generation: keyboard.generation,
        keyCode,
      }),
    [keyboard.generation],
  );
  useEffect(() => {
    if (!keyboard.active) return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.ctrlKey || event.metaKey || event.altKey) return;
      const keyCode =
        event.key === "Backspace"
          ? 8
          : event.key === "Enter"
            ? 13
            : Array.from(event.key).length === 1
              ? event.key.codePointAt(0)
              : undefined;
      if (keyCode === undefined) return;
      event.preventDefault();
      void submit(keyCode);
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [keyboard.active, submit]);
  if (!keyboard.active) return null;
  const rows = shifted ? UPPER_KEYS : LOWER_KEYS;
  return (
    <section
      className="runtime-overlay__modal-layer runtime-overlay__keyboard"
      role="dialog"
      aria-modal="true"
      aria-label="Software keyboard"
    >
      <div className="runtime-overlay__input-preview">
        ⌨ <span>{keyboard.text}</span>
        <i aria-hidden="true" />
      </div>
      <div className="runtime-overlay__key-grid">
        {rows.map((row, rowIndex) => (
          <div className="runtime-overlay__key-row" key={rowIndex}>
            {row.map((key) => (
              <button
                key={key}
                onClick={() =>
                  void submit(key === "⌫" ? 8 : key.codePointAt(0)!)
                }
              >
                {key}
              </button>
            ))}
          </div>
        ))}
        <div className="runtime-overlay__key-row runtime-overlay__key-row--actions">
          <button
            aria-pressed={shifted}
            onClick={() => setShifted((value) => !value)}
          >
            ⇧ Shift
          </button>
          <button
            className="runtime-overlay__space"
            onClick={() => void submit(32)}
          >
            Space
          </button>
          <button onClick={() => void submit(13)}>✓ OK</button>
        </div>
      </div>
    </section>
  );
}

function ErrorDialog({ snapshot }: { snapshot: RuntimeOverlaySnapshot }) {
  const dialog = snapshot.errorDialog;
  if (!dialog.active) return null;
  const select = (rightButton: boolean) =>
    invoke("overlay.selectErrorButton", {
      generation: dialog.generation,
      rightButton,
    });
  return (
    <section
      className="runtime-overlay__modal-layer"
      role="dialog"
      aria-modal="true"
      aria-labelledby="runtime-error-title"
    >
      <div
        className="runtime-overlay__dialog"
        style={{ opacity: dialog.opacity }}
      >
        <h1 id="runtime-error-title">{dialog.title}</h1>
        <p>{dialog.message}</p>
        <div className="runtime-overlay__dialog-actions">
          <button autoFocus onClick={() => void select(false)}>
            {dialog.leftButton}
          </button>
          {dialog.rightButton && (
            <button onClick={() => void select(true)}>
              {dialog.rightButton}
            </button>
          )}
        </div>
      </div>
    </section>
  );
}

export function RuntimeOverlayRoot() {
  const [snapshot, setSnapshot] = useState<RuntimeOverlaySnapshot>();
  const rootRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const accept = (next: RuntimeOverlaySnapshot) =>
      setSnapshot((current) =>
        !current || BigInt(next.sequence) >= BigInt(current.sequence)
          ? next
          : current,
      );
    const unsubscribe = subscribe((event) => {
      if (event.type === "overlay.changed")
        accept(event.payload as RuntimeOverlaySnapshot);
      else if (event.type === "emulation.loaded")
        setSnapshot((current) =>
          current
            ? {
                ...current,
                shaderProgress: {
                  ...current.shaderProgress,
                  visible: false,
                },
              }
            : current,
        );
    });
    void invoke("overlay.getSnapshot").then(accept);
    return unsubscribe;
  }, []);
  useEffect(() => {
    if (!snapshot || snapshot.interaction === "passive") return;
    const navigate = (event: Event) => {
      const action = (event as CustomEvent<string>).detail;
      if (
        (action === "cancel" || action === "input") &&
        snapshot.interaction === "softwareKeyboard"
      ) {
        window.dispatchEvent(
          new KeyboardEvent("keydown", {
            key: action === "cancel" ? "Backspace" : "Enter",
          }),
        );
        return;
      }
      const buttons = Array.from(
        rootRef.current?.querySelectorAll<HTMLButtonElement>(
          "button:not(:disabled)",
        ) ?? [],
      );
      if (!buttons.length) return;
      const active = document.activeElement;
      const index = buttons.findIndex((button) => button === active);
      if (action === "activate") {
        if (index >= 0) buttons[index].click();
        else buttons[0].focus();
        return;
      }
      if (!["left", "right", "up", "down"].includes(action)) return;
      const delta = action === "left" || action === "up" ? -1 : 1;
      buttons[
        (index < 0 ? 0 : index + delta + buttons.length) % buttons.length
      ].focus();
    };
    window.addEventListener("cemu-overlay-navigate", navigate);
    return () => window.removeEventListener("cemu-overlay-navigate", navigate);
  }, [snapshot]);
  const groups = useMemo(() => {
    if (!snapshot) return new Map<OverlayPosition, React.ReactNode[]>();
    const result = new Map<OverlayPosition, React.ReactNode[]>();
    const add = (position: OverlayPosition, content: React.ReactNode) => {
      if (position === "disabled") return;
      const items = result.get(position) ?? [];
      items.push(content);
      result.set(position, items);
    };
    add(
      snapshot.overlayStyle.position,
      <div style={styleProperties(snapshot.overlayStyle)} key="stats">
        <Stats snapshot={snapshot} />
      </div>,
    );
    add(
      snapshot.notificationStyle.position,
      <div style={styleProperties(snapshot.notificationStyle)} key="notices">
        <Notices snapshot={snapshot} />
      </div>,
    );
    return result;
  }, [snapshot]);
  if (!snapshot) return <div className="runtime-overlay-root" />;
  return (
    <div
      ref={rootRef}
      className={`runtime-overlay-root runtime-overlay-root--${snapshot.interaction}`}
    >
      <ShaderProgress snapshot={snapshot} />
      {[...groups].map(([position, content]) => (
        <div
          className={`runtime-overlay__anchor runtime-overlay__anchor--${position}`}
          key={position}
        >
          {content}
        </div>
      ))}
      <SoftwareKeyboard snapshot={snapshot} />
      <ErrorDialog snapshot={snapshot} />
    </div>
  );
}
