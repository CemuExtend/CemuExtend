import { useEffect, useState } from "react";
import type { WindowRole } from "../bridge/contracts";
import { invoke } from "../bridge/native";

const labels: Record<Exclude<WindowRole, "main-library">, string> = {
  "general-settings": "General Settings", "input-settings": "Input Settings", "hotkey-settings": "Hotkey Settings",
  "graphic-packs": "Graphic Packs", "title-manager": "Title Manager", "cemod-manager": "CemuExtend Manager",
  "cemod-permissions": "CemuExtend Permissions", "account-manager": "Account Manager", "save-manager": "Save Manager",
  "update-manager": "Updates", "logging": "Logging", "memory-searcher": "Memory Searcher", "ppc-debugger": "PPC Debugger",
  "audio-debugger": "Audio Debugger", "texture-relations": "Texture Relations", "ppc-threads": "PPC Threads",
  "emulated-usb-devices": "Emulated USB Devices", "checksum-tool": "Checksum Tool", "getting-started": "Getting Started", "about": "About CemuExtend"
};

export function RoleWindow({ role }: { role: Exclude<WindowRole, "main-library"> }) {
  const [model, setModel] = useState<Record<string, unknown>>({});
  const [error, setError] = useState("");
  useEffect(() => { void invoke("window.getModel", { role }).then(setModel).catch((reason: unknown) => setError(String(reason))); }, [role]);
  return <main className="role-window"><header><div><span className="eyebrow">CemuExtend</span><h1>{labels[role]}</h1></div><button onClick={() => void invoke("window.close")}>Close</button></header>
    {error ? <div className="notice error" role="alert">{error}</div> : <section className="property-grid">{Object.entries(model).map(([key, value]) => <div key={key}><strong>{key}</strong><span>{typeof value === "object" ? JSON.stringify(value) : String(value)}</span></div>)}</section>}
  </main>;
}
