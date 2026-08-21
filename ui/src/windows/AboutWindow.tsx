import { useEffect, useState } from "react";
import type { AboutInfo } from "../bridge/contracts";
import { invoke } from "../bridge/native";

export function AboutWindow() {
  const [model, setModel] = useState<AboutInfo>();
  const [error, setError] = useState("");
  useEffect(() => {
    void invoke("about.get")
      .then(setModel)
      .catch((reason: unknown) => setError(String(reason)));
  }, []);
  return (
    <main className="role-window about-window">
      <header>
        <div>
          <span className="eyebrow">CemuExtend</span>
          <h1>About</h1>
        </div>
        <button onClick={() => void invoke("window.close")}>Close</button>
      </header>
      {error && (
        <div className="notice error" role="alert">
          {error}
        </div>
      )}
      {!model ? (
        <div className="spinner" aria-label="Loading" />
      ) : (
        <>
          <section className="hero-card">
            <div className="app-mark">CE</div>
            <div>
              <h2>{model.name}</h2>
              <p>Version {model.version}</p>
              <code>{model.commit}</code>
            </div>
          </section>
          <dl className="detail-list">
            <div>
              <dt>Built</dt>
              <dd>{model.buildDate}</dd>
            </div>
            <div>
              <dt>Frontend</dt>
              <dd>{model.frontend}</dd>
            </div>
            <div>
              <dt>WebView</dt>
              <dd>{model.webviewEngine}</dd>
            </div>
            <div>
              <dt>Original authors</dt>
              <dd>{model.originalAuthors.join(", ")}</dd>
            </div>
          </dl>
          <section>
            <h2>Open-source components</h2>
            <dl className="detail-list">
              {model.libraries.map((library) => (
                <div key={library.name}>
                  <dt>{library.name}</dt>
                  <dd>
                    {library.license} ·{" "}
                    <button
                      className="link-button"
                      onClick={() =>
                        void invoke("system.openExternalUrl", {
                          url: library.url,
                        }).catch((reason: unknown) => setError(String(reason)))
                      }
                    >
                      Source
                    </button>
                  </dd>
                </div>
              ))}
            </dl>
          </section>
          <section>
            <h2>Project links</h2>
            <div className="button-row">
              {model.links.map((link) => (
                <button
                  key={link.url}
                  onClick={() =>
                    void invoke("system.openExternalUrl", {
                      url: link.url,
                    }).catch((reason: unknown) => setError(String(reason)))
                  }
                >
                  {link.label}
                </button>
              ))}
            </div>
          </section>
        </>
      )}
    </main>
  );
}
