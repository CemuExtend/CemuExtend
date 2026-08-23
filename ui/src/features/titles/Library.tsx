import { useCallback, useEffect, useMemo, useState } from "react";
import type { Title } from "../../bridge/contracts";
import {
  launchTitle,
  listTitles,
  refreshTitles,
  subscribeToTitles,
} from "../../domain/titles/repository";
import { en } from "../../i18n/en";
import type { OpenToolHandler } from "../../platform/native/tools";

type SortKey = "name" | "lastPlayed" | "playTime";
type ViewMode = "list" | "grid";
type WorkspaceTab =
  "overview" | "mods" | "graphic-packs" | "saves" | "settings";

const WORKSPACE_TABS: Array<{ id: WorkspaceTab; label: string }> = [
  { id: "overview", label: en.library.overview },
  { id: "mods", label: en.library.mods },
  { id: "graphic-packs", label: en.library.graphicPacks },
  { id: "saves", label: en.library.saves },
  { id: "settings", label: en.library.gameSettings },
];

function messageFrom(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

function hoursFromMinutes(minutes: number): string {
  return (minutes / 60).toLocaleString(undefined, {
    maximumFractionDigits: 1,
  });
}

export function Library({
  onOpenTool,
  onOpenGame,
}: {
  onOpenTool: OpenToolHandler;
  onOpenGame: (titleId: string) => void;
}) {
  const [titles, setTitles] = useState<Title[]>([]);
  const [query, setQuery] = useState("");
  const [region, setRegion] = useState("all");
  const [sort, setSort] = useState<SortKey>("name");
  const [view, setView] = useState<ViewMode>("list");
  const [activeTab, setActiveTab] = useState<WorkspaceTab>("overview");
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [error, setError] = useState("");
  const [selectedId, setSelectedId] = useState<string>();
  const [launchingId, setLaunchingId] = useState<string>();
  const [permissionTitleId, setPermissionTitleId] = useState<string>();

  const load = useCallback(async () => {
    setLoading(true);
    setError("");
    try {
      const next = await listTitles();
      setTitles(next);
      setSelectedId((current) =>
        current && next.some((title) => title.titleId === current)
          ? current
          : next[0]?.titleId,
      );
    } catch (reason) {
      setError(messageFrom(reason));
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void load();
    return subscribeToTitles((event) => {
      if (event.type === "changed") {
        void load();
        return;
      }
      if (event.type === "diagnostic") {
        setError(event.message);
        return;
      }
      const { state } = event;
      setSelectedId(state.titleId);
      if (state.status === "awaitingPermission") {
        setPermissionTitleId(state.titleId);
        setLaunchingId(undefined);
      } else if (state.status === "started" || state.status === "shutdown") {
        setPermissionTitleId(undefined);
        setLaunchingId(undefined);
      } else {
        setPermissionTitleId(undefined);
        setLaunchingId(undefined);
        setError(state.diagnostic || en.library.launchFailed);
      }
    });
  }, [load]);

  const regions = useMemo(
    () => [...new Set(titles.map((title) => title.region))].sort(),
    [titles],
  );
  const visible = useMemo(
    () =>
      titles
        .filter((title) => region === "all" || title.region === region)
        .filter((title) =>
          `${title.name} ${title.titleId}`
            .toLocaleLowerCase()
            .includes(query.toLocaleLowerCase()),
        )
        .sort((a, b) =>
          sort === "name"
            ? a.name.localeCompare(b.name)
            : sort === "playTime"
              ? b.playTimeMinutes - a.playTimeMinutes
              : (b.lastPlayed ?? "").localeCompare(a.lastPlayed ?? ""),
        ),
    [titles, query, region, sort],
  );
  const selectedTitle = titles.find((title) => title.titleId === selectedId);

  const launch = async (title: Title) => {
    setSelectedId(title.titleId);
    setLaunchingId(title.titleId);
    setPermissionTitleId(undefined);
    setError("");
    try {
      const result = await launchTitle(title.titleId);
      if (result.status === "awaitingPermission") {
        setPermissionTitleId(title.titleId);
        setLaunchingId(undefined);
      }
    } catch (reason) {
      setLaunchingId(undefined);
      setError(messageFrom(reason));
    }
  };

  const refresh = async () => {
    setRefreshing(true);
    setError("");
    try {
      const next = await refreshTitles();
      setTitles(next);
    } catch (reason) {
      setError(messageFrom(reason));
    } finally {
      setRefreshing(false);
    }
  };

  return (
    <section className="library-page" aria-labelledby="library-title">
      <header className="page-heading library-heading">
        <div>
          <h1 id="library-title">{en.library.title}</h1>
          <p>{en.library.description}</p>
        </div>
        <p className="page-heading__state">
          {visible.length} / {titles.length} {en.library.results}
        </p>
      </header>

      <div className="library-controls" aria-label="Game library controls">
        <label className="search-control">
          <span>{en.library.searchLabel}</span>
          <input
            type="search"
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            placeholder={en.library.searchPlaceholder}
          />
        </label>
        <label>
          <span>{en.library.regionLabel}</span>
          <select
            value={region}
            onChange={(event) => setRegion(event.target.value)}
          >
            <option value="all">{en.library.allRegions}</option>
            {regions.map((value) => (
              <option key={value}>{value}</option>
            ))}
          </select>
        </label>
        <label>
          <span>{en.library.sortLabel}</span>
          <select
            value={sort}
            onChange={(event) => setSort(event.target.value as SortKey)}
          >
            <option value="name">{en.library.sortName}</option>
            <option value="lastPlayed">{en.library.sortLastPlayed}</option>
            <option value="playTime">{en.library.sortPlayTime}</option>
          </select>
        </label>
        <fieldset className="view-control">
          <legend>{en.library.viewLabel}</legend>
          <button
            aria-pressed={view === "list"}
            onClick={() => setView("list")}
          >
            {en.library.listView}
          </button>
          <button
            aria-pressed={view === "grid"}
            onClick={() => setView("grid")}
          >
            {en.library.gridView}
          </button>
        </fieldset>
        <button
          className="button-secondary"
          disabled={refreshing}
          onClick={() => void refresh()}
        >
          {refreshing ? en.library.refreshing : en.library.refresh}
        </button>
      </div>

      {error && (
        <div className="inline-notice" role="alert">
          <span>{error}</span>
          <button onClick={() => setError("")}>{en.shell.dismiss}</button>
        </div>
      )}

      <div className="library-workbench">
        <section className="game-browser" aria-label={en.library.title}>
          {loading ? (
            <div className="library-state" aria-busy="true">
              <span className="spinner" aria-hidden="true" />
              <p>{en.library.loading}</p>
            </div>
          ) : visible.length === 0 ? (
            <div className="library-state">
              <h2>{en.library.emptyTitle}</h2>
              <p>
                {titles.length
                  ? en.library.emptyFiltered
                  : en.library.emptyLibrary}
              </p>
              {!titles.length && (
                <button onClick={() => onOpenTool("general-settings")}>
                  {en.library.openSettings}
                </button>
              )}
            </div>
          ) : (
            <ul className="game-list" data-view={view} role="listbox">
              {visible.map((title) => {
                const selected = title.titleId === selectedId;
                const launching = title.titleId === launchingId;
                return (
                  <li className="game-list__item" key={title.titleId}>
                    <button
                      className="game-select"
                      role="option"
                      aria-selected={selected}
                      onClick={() => {
                        setSelectedId(title.titleId);
                        setActiveTab("overview");
                      }}
                      onDoubleClick={() => void launch(title)}
                      onKeyDown={(event) => {
                        if (event.key === "Enter") {
                          event.preventDefault();
                          void launch(title);
                        }
                      }}
                    >
                      {title.iconDataUrl ? (
                        <img src={title.iconDataUrl} alt="" />
                      ) : (
                        <span className="game-monogram" aria-hidden="true">
                          {title.name.slice(0, 1)}
                        </span>
                      )}
                      <span className="game-select__copy">
                        <strong>{title.name}</strong>
                        <code>{title.titleId}</code>
                        <span>
                          {title.region} · v{title.version} ·{" "}
                          {hoursFromMinutes(title.playTimeMinutes)}h
                        </span>
                      </span>
                      <span className="game-select__status">
                        {launching
                          ? en.library.launching
                          : selected
                            ? en.library.selected
                            : ""}
                      </span>
                    </button>
                  </li>
                );
              })}
            </ul>
          )}
        </section>

        <aside className="game-workspace" aria-labelledby="workspace-title">
          <header className="workspace-heading">
            <p>{en.library.workspace}</p>
            <h2 id="workspace-title">
              {selectedTitle?.name ?? en.library.noSelection}
            </h2>
          </header>
          {selectedTitle ? (
            <>
              <div className="workspace-actions">
                <button
                  className="button-primary"
                  disabled={launchingId === selectedTitle.titleId}
                  onClick={() => void launch(selectedTitle)}
                >
                  {launchingId === selectedTitle.titleId
                    ? en.library.launching
                    : en.library.play}
                </button>
                <button
                  className="button-secondary"
                  onClick={() => onOpenGame(selectedTitle.titleId)}
                >
                  {en.library.openWorkspace}
                </button>
                <span className="workspace-status" role="status">
                  {permissionTitleId === selectedTitle.titleId
                    ? en.library.permissionRequired
                    : en.library.ready}
                </span>
              </div>
              <div className="workspace-tabs" role="tablist">
                {WORKSPACE_TABS.map((tab) => (
                  <button
                    key={tab.id}
                    role="tab"
                    aria-selected={activeTab === tab.id}
                    onClick={() => setActiveTab(tab.id)}
                  >
                    {tab.label}
                  </button>
                ))}
              </div>
              <div className="workspace-panel" role="tabpanel">
                {activeTab === "overview" ? (
                  <dl className="workspace-facts">
                    <div>
                      <dt>{en.library.region}</dt>
                      <dd>{selectedTitle.region}</dd>
                    </div>
                    <div>
                      <dt>{en.library.version}</dt>
                      <dd>{selectedTitle.version}</dd>
                    </div>
                    <div>
                      <dt>{en.library.playTime}</dt>
                      <dd>
                        {hoursFromMinutes(selectedTitle.playTimeMinutes)}{" "}
                        {en.library.hours}
                      </dd>
                    </div>
                    <div>
                      <dt>{en.library.lastPlayed}</dt>
                      <dd>{selectedTitle.lastPlayed || en.library.never}</dd>
                    </div>
                    <div className="workspace-facts__wide">
                      <dt>{en.library.path}</dt>
                      <dd>{selectedTitle.path}</dd>
                    </div>
                  </dl>
                ) : (
                  <div className="workspace-tool">
                    <p>
                      {
                        WORKSPACE_TABS.find((tab) => tab.id === activeTab)
                          ?.label
                      }
                    </p>
                    <button
                      className="button-secondary"
                      onClick={() => {
                        if (activeTab === "graphic-packs")
                          onOpenTool("graphic-packs", {
                            titleId: selectedTitle.titleId,
                          });
                        else if (activeTab === "mods")
                          onOpenTool("cemod-manager");
                        else if (activeTab === "saves")
                          onOpenTool("save-manager");
                        else onOpenTool("general-settings");
                      }}
                    >
                      {en.library.openManager}
                    </button>
                  </div>
                )}
              </div>
            </>
          ) : (
            <div className="library-state">
              <p>{en.library.noSelection}</p>
            </div>
          )}
        </aside>
      </div>
    </section>
  );
}
