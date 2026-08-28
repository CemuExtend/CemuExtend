import type { ReactNode } from "react";
import type { ImplementedToolWindowRole } from "../bridge/contracts";
import { CemuIcon } from "../components/CemuIcon";
import { en } from "../i18n/en";
import { getReferencePreviewScreen } from "../dev/referencePreview";
import {
  previewNavigationFor,
  previewTitleFor,
} from "../dev/previewNavigation";
import { invoke } from "../bridge/native";
import type { ScreenDefinition } from "./screenRegistry";

export function DetachedToolShell({
  role,
  definition,
  children,
}: {
  role: ImplementedToolWindowRole;
  definition: ScreenDefinition;
  children: ReactNode;
}) {
  const developer = definition.category === "Developer";
  const preview = getReferencePreviewScreen();
  const previewNavigation = preview
    ? previewNavigationFor(preview.index)
    : undefined;
  const displayedTitle = preview
    ? previewTitleFor(preview.index, definition.title)
    : definition.title;
  const headerTitle =
    preview && preview.index >= 55 && preview.index <= 59
      ? definition.title
      : displayedTitle;
  const childOwnsFooter =
    !preview && (role === "getting-started" || role === "cemod-permissions");
  const showNavigation = Boolean(previewNavigation);
  return (
    <div
      className="detached-shell app-window"
      data-layout={developer ? "diagnostic" : "page-container"}
    >
      <div className="detached-body">
        {showNavigation && (
          <aside className="side-nav" aria-label={en.shell.navigationLabel}>
            {previewNavigation?.map((item) => (
              <button
                key={item.label}
                className="side-item"
                aria-current={
                  item.indices.includes(preview!.index) ? "page" : undefined
                }
              >
                <CemuIcon name={item.icon} />
                <span>{item.label}</span>
              </button>
            ))}
          </aside>
        )}
        <section className="detached-stage">
          <header className="detached-header">
            <div>
              <h1>{headerTitle}</h1>
              {!preview && <p>{definition.description}</p>}
            </div>
            {preview &&
            (preview.index === 17 || preview.index === 18) ? null : preview &&
              preview.index >= 54 &&
              preview.index <= 59 ? (
              <div className="button-row">
                <button>
                  <CemuIcon name="play" />
                  Run
                </button>
                <button>Ⅱ&nbsp; Pause</button>
                <button>→&nbsp; Step</button>
              </div>
            ) : preview ? (
              <button>
                <CemuIcon name="refresh" />
                Refresh
              </button>
            ) : null}
          </header>
          <div className="detached-content">{children}</div>
          {!childOwnsFooter && (
            <footer className="detached-footer">
              {preview && (preview.index === 17 || preview.index === 18) ? (
                <span>Step {preview.index === 17 ? 1 : 2} of 2</span>
              ) : preview && preview.index >= 19 && preview.index <= 29 ? (
                <button>
                  <CemuIcon name="help" />
                  Help
                </button>
              ) : null}
              <div className="button-row">
                {preview && preview.index >= 19 && preview.index <= 29 && (
                  <button>Revert</button>
                )}
                <button onClick={() => void invoke("window.close")}>
                  {preview?.index === 17 || preview?.index === 18
                    ? "Cancel"
                    : "Close"}
                </button>
                {preview?.index === 18 && <button>Previous</button>}
                {preview && preview.index >= 17 && preview.index <= 29 && (
                  <button className="button-primary">
                    {preview.index === 17 || preview.index === 18
                      ? preview.index === 17
                        ? "Next"
                        : "Finish"
                      : "Apply"}
                  </button>
                )}
              </div>
            </footer>
          )}
        </section>
      </div>
    </div>
  );
}
