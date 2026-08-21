import { useEffect, useState } from "react";
import { invoke } from "../bridge/native";
import type { Bootstrap } from "../bridge/contracts";
import { Library } from "../features/titles/Library";
import { RoleWindow } from "../windows/RoleWindow";

const fallback: Bootstrap = { windowId: 0, windowRole: "main-library", appVersion: "development", platform: "unknown", theme: "system", shuttingDown: false };

export function App() {
  const [bootstrap, setBootstrap] = useState<Bootstrap>({ ...fallback, ...window.__CEMU_BOOTSTRAP__ });
  const [error, setError] = useState("");
  useEffect(() => { void invoke("system.bootstrap").then(setBootstrap).catch((reason: unknown) => setError(String(reason))); }, []);
  useEffect(() => { document.documentElement.dataset.theme = bootstrap.theme; }, [bootstrap.theme]);
  if (bootstrap.shuttingDown) return <main className="empty"><h1>Closing CemuExtend…</h1></main>;
  if (error) return <main className="fatal" role="alert"><h1>Native host unavailable</h1><p>{error}</p></main>;
  return bootstrap.windowRole === "main-library" ? <Library /> : <RoleWindow role={bootstrap.windowRole} windowId={bootstrap.windowId} context={bootstrap.context} />;
}
