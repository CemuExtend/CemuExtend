import type { ReactNode } from "react";
import { en } from "../i18n/en";
import type { ScreenDefinition } from "./screenRegistry";

export function DetachedToolShell({
  definition,
  children,
}: {
  definition: ScreenDefinition;
  children: ReactNode;
}) {
  return (
    <div
      className="detached-shell"
      data-category={definition.category.toLocaleLowerCase()}
    >
      <header className="detached-shell__bar">
        <span className="detached-shell__mark" aria-hidden="true">
          CE
        </span>
        <div className="detached-shell__identity">
          <p>
            {definition.detachable ? en.shell.detachable : definition.category}
          </p>
          <h1>{definition.title}</h1>
        </div>
        <span className="detached-shell__scope">{en.shell.appTheme}</span>
      </header>
      <p className="detached-shell__description">{definition.description}</p>
      <div className="detached-shell__body">{children}</div>
    </div>
  );
}
