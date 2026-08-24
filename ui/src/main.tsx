import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./app/App";
import { ErrorBoundary } from "./components/ErrorBoundary";
import { installLocalization } from "./i18n/runtime";
import { installDevNativeMock } from "./dev/nativeMock";
import "./bridge/events";
import "../tokens.css";
import "./styles/main.css";
import "./styles/shell.css";
import "./styles/detached.css";
import "./styles/runtime-overlay.css";

function render() {
  const previewRequested =
    new URLSearchParams(window.location.search).get("preview") === "1";
  if (previewRequested || (import.meta.env.DEV && !window.cemuInvoke)) {
    installDevNativeMock();
  }
  installLocalization(window.__CEMU_BOOTSTRAP__?.language ?? "system");
  createRoot(document.getElementById("root")!).render(
    <StrictMode>
      <ErrorBoundary>
        <App />
      </ErrorBoundary>
    </StrictMode>,
  );
}

render();
