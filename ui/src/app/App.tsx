import { useCallback, useEffect, useRef, useState } from "react";
import type { Bootstrap } from "../bridge/contracts";
import { subscribe } from "../bridge/events";
import { invoke } from "../bridge/native";
import { en } from "../i18n/en";
import { setUiLanguage } from "../i18n/runtime";
import { loadBootstrap } from "../platform/native/bootstrap";
import {
  applyTheme,
  isUiTheme,
  resolveInitialTheme,
  type UiTheme,
} from "../platform/theme";
import { RoleWindow } from "../windows/RoleWindow";
import { RuntimeOverlayRoot } from "../features/runtime-overlay/RuntimeOverlay";
import { AppShell } from "./AppShell";

const fallback: Bootstrap = {
  windowId: "0",
  windowRole: "main-library",
  appVersion: "development",
  platform: "unknown",
  theme: "system",
  themeRevision: "0",
  language: "system",
  languageRevision: "0",
  shuttingDown: false,
};

export function App() {
  const [bootstrap, setBootstrap] = useState<Bootstrap>({
    ...fallback,
    ...window.__CEMU_BOOTSTRAP__,
  });
  const [error, setError] = useState("");
  const [theme, setTheme] = useState<UiTheme>(() =>
    resolveInitialTheme(bootstrap.theme),
  );
  const themeRevision = useRef(
    /^(0|[1-9][0-9]*)$/.test(bootstrap.themeRevision)
      ? BigInt(bootstrap.themeRevision)
      : 0n,
  );
  const languageRevision = useRef(
    /^(0|[1-9][0-9]*)$/.test(bootstrap.languageRevision)
      ? BigInt(bootstrap.languageRevision)
      : 0n,
  );
  const acceptTheme = useCallback((nextTheme: UiTheme, revision: string) => {
    if (!/^(0|[1-9][0-9]*)$/.test(revision)) return;
    const nextRevision = BigInt(revision);
    if (nextRevision < themeRevision.current) return;
    themeRevision.current = nextRevision;
    setTheme(nextTheme);
  }, []);
  useEffect(() => {
    void loadBootstrap()
      .then((value) => {
        setBootstrap(value);
        setUiLanguage(value.language);
        if (isUiTheme(value.theme))
          acceptTheme(value.theme, value.themeRevision);
      })
      .catch((reason: unknown) => setError(String(reason)));
  }, [acceptTheme]);
  useEffect(() => applyTheme(theme), [theme]);
  useEffect(
    () =>
      subscribe((event) => {
        if (
          event.type === "language.changed" &&
          event.payload &&
          typeof event.payload === "object" &&
          "language" in event.payload &&
          typeof event.payload.language === "string" &&
          "revision" in event.payload &&
          typeof event.payload.revision === "string" &&
          /^(0|[1-9][0-9]*)$/.test(event.payload.revision)
        ) {
          const revision = BigInt(event.payload.revision);
          if (revision >= languageRevision.current) {
            languageRevision.current = revision;
            setUiLanguage(event.payload.language);
          }
          return;
        }
        if (
          event.type !== "theme.changed" ||
          !event.payload ||
          typeof event.payload !== "object" ||
          !("theme" in event.payload) ||
          !isUiTheme(event.payload.theme) ||
          !("revision" in event.payload) ||
          typeof event.payload.revision !== "string"
        )
          return;
        acceptTheme(event.payload.theme, event.payload.revision);
      }),
    [acceptTheme],
  );
  useEffect(() => {
    void invoke("theme.get")
      .then((result) => acceptTheme(result.theme, result.revision))
      .catch((reason: unknown) => setError(String(reason)));
  }, [acceptTheme]);

  const changeTheme = (nextTheme: UiTheme) => {
    void invoke("theme.set", { theme: nextTheme })
      .then((result) => acceptTheme(result.theme, result.revision))
      .catch((reason: unknown) => setError(String(reason)));
  };
  if (bootstrap.shuttingDown)
    return (
      <main className="empty">
        <h1>{en.shell.closing}</h1>
      </main>
    );
  if (error)
    return (
      <main className="fatal" role="alert">
        <h1>{en.shell.nativeUnavailable}</h1>
        <p>{error}</p>
      </main>
    );
  if (bootstrap.windowRole === "runtime-overlay") return <RuntimeOverlayRoot />;
  return bootstrap.windowRole === "main-library" ? (
    <>
      <div className="main-application">
        <AppShell
          bootstrap={bootstrap}
          theme={theme}
          onThemeChange={changeTheme}
        />
      </div>
      <RuntimeOverlayRoot />
    </>
  ) : (
    <RoleWindow
      role={bootstrap.windowRole}
      windowId={bootstrap.windowId}
      context={bootstrap.context}
    />
  );
}
