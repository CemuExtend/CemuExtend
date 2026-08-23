import { useState } from "react";
import type { Bootstrap } from "../bridge/contracts";
import { GamePage } from "../features/titles/GamePage";
import { Library } from "../features/titles/Library";
import { en } from "../i18n/en";
import { SCREEN_REGISTRY } from "./screenRegistry";
import type { UiTheme } from "../platform/theme";
import { openTool, type OpenToolHandler } from "../platform/native/tools";
import {
  developerNavigation,
  pageTools,
  primaryNavigation,
  routeLabel,
  type AppRoute,
  type MainRoute,
  type ToolRoute,
} from "./navigation";

const PAGE_COPY = {
  mods: en.pages.mods,
  downloads: en.pages.downloads,
  controllers: en.pages.controllers,
  accounts: en.pages.accounts,
  settings: en.pages.settings,
  developer: en.pages.developer,
} as const;

function NavigationButton({
  route,
  currentRoute,
  label,
  onNavigate,
}: {
  route: MainRoute;
  currentRoute: AppRoute;
  label: string;
  onNavigate: (route: MainRoute) => void;
}) {
  const active = route === currentRoute;
  return (
    <button
      className="shell-nav__item"
      aria-current={active ? "page" : undefined}
      onClick={() => onNavigate(route)}
    >
      <span aria-hidden="true">{active ? "■" : "□"}</span>
      {label}
    </button>
  );
}

function ToolLanding({
  route,
  onOpenTool,
}: {
  route: ToolRoute;
  onOpenTool: OpenToolHandler;
}) {
  const copy = PAGE_COPY[route];
  return (
    <section className="tool-landing" aria-labelledby={`${route}-title`}>
      <header className="page-heading">
        <div>
          <h1 id={`${route}-title`}>{copy.title}</h1>
          <p>{copy.description}</p>
        </div>
        <p className="page-heading__state">{en.shell.nativeWorkspace}</p>
      </header>
      <div className="tool-directory" role="list">
        {pageTools[route].map((tool) => (
          <div className="tool-directory__row" role="listitem" key={tool.role}>
            <div>
              <h2>{SCREEN_REGISTRY[tool.role].title}</h2>
              <p>{SCREEN_REGISTRY[tool.role].description}</p>
            </div>
            <button
              className="button-secondary"
              onClick={() => onOpenTool(tool.role)}
            >
              {en.pages.open}
            </button>
          </div>
        ))}
      </div>
    </section>
  );
}

export function AppShell({
  bootstrap,
  theme,
  onThemeChange,
}: {
  bootstrap: Bootstrap;
  theme: UiTheme;
  onThemeChange: (theme: UiTheme) => void;
}) {
  const previewGameId = import.meta.env.DEV
    ? (new URLSearchParams(window.location.search).get("game") ?? undefined)
    : undefined;
  const [route, setRoute] = useState<AppRoute>(
    previewGameId ? "game" : "library",
  );
  const [activeGameId, setActiveGameId] = useState<string | undefined>(
    previewGameId,
  );
  const [developerVisible, setDeveloperVisible] = useState(false);
  const [notice, setNotice] = useState("");

  const handleOpenTool: OpenToolHandler = (role, context) => {
    setNotice(en.pages.opening);
    void openTool(role, context)
      .then(() => setNotice(en.pages.opened))
      .catch((reason: unknown) =>
        setNotice(reason instanceof Error ? reason.message : String(reason)),
      );
  };

  const navigate = (nextRoute: MainRoute) => setRoute(nextRoute);
  const openGame = (titleId: string) => {
    setActiveGameId(titleId);
    setRoute("game");
  };

  return (
    <div className="app-shell">
      <aside className="shell-nav">
        <header className="shell-brand">
          <span className="shell-brand__mark" aria-hidden="true">
            CE
          </span>
          <div>
            <strong>{en.appName}</strong>
            <span>v{bootstrap.appVersion}</span>
          </div>
        </header>
        <nav aria-label={en.shell.navigationLabel}>
          {primaryNavigation.map((item) => (
            <NavigationButton
              key={item.route}
              {...item}
              currentRoute={route}
              onNavigate={navigate}
            />
          ))}
        </nav>
        <div className="shell-nav__developer">
          <button
            className="shell-nav__disclosure"
            aria-expanded={developerVisible}
            onClick={() => setDeveloperVisible((current) => !current)}
          >
            {developerVisible
              ? en.shell.developerHide
              : en.shell.developerToggle}
          </button>
          {developerVisible && (
            <NavigationButton
              {...developerNavigation}
              currentRoute={route}
              onNavigate={navigate}
            />
          )}
        </div>
        <footer className="shell-nav__footer">
          <span>{bootstrap.platform}</span>
          <span>WebView host</span>
        </footer>
      </aside>

      <div className="shell-stage">
        <header className="shell-topbar">
          <label className="shell-route-picker">
            <span>{en.shell.pageLabel}</span>
            <select
              value={route}
              onChange={(event) => setRoute(event.target.value as MainRoute)}
            >
              {route === "game" && (
                <option value="game">{en.navigation.game}</option>
              )}
              {primaryNavigation.map((item) => (
                <option key={item.route} value={item.route}>
                  {item.label}
                </option>
              ))}
              {developerVisible && (
                <option value={developerNavigation.route}>
                  {developerNavigation.label}
                </option>
              )}
            </select>
          </label>
          <p className="shell-breadcrumb">
            <span>{en.shell.currentPage}</span>
            <strong>{routeLabel(route)}</strong>
          </p>
          <div className="shell-actions">
            <details className="job-center">
              <summary>{en.shell.jobs} · 0</summary>
              <div className="job-center__panel">
                <strong>{en.shell.jobsEmpty}</strong>
                <p>{en.shell.jobsHint}</p>
              </div>
            </details>
            <button
              className="theme-toggle"
              onClick={() =>
                onThemeChange(theme === "light" ? "dark" : "light")
              }
              aria-label={
                theme === "light"
                  ? en.shell.useDarkTheme
                  : en.shell.useWhiteTheme
              }
            >
              <span aria-hidden="true">{theme === "light" ? "◐" : "◑"}</span>
              {theme === "light" ? en.shell.whiteTheme : en.shell.darkTheme}
            </button>
          </div>
        </header>

        {notice && (
          <div className="shell-notice" role="status" aria-live="polite">
            <span>{notice}</span>
            <button onClick={() => setNotice("")}>{en.shell.dismiss}</button>
          </div>
        )}

        <main className="shell-content">
          {route === "library" ? (
            <Library onOpenTool={handleOpenTool} onOpenGame={openGame} />
          ) : route === "game" ? (
            activeGameId ? (
              <GamePage
                titleId={activeGameId}
                onBack={() => setRoute("library")}
                onOpenTool={handleOpenTool}
              />
            ) : (
              <Library onOpenTool={handleOpenTool} onOpenGame={openGame} />
            )
          ) : (
            <ToolLanding route={route} onOpenTool={handleOpenTool} />
          )}
        </main>
      </div>
    </div>
  );
}
