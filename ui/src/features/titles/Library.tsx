import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { Title } from "../../bridge/contracts";
import {
  launchTitle,
  loadTitleIcon,
  listTitles,
  refreshTitles,
  subscribeToTitles,
} from "../../domain/titles/repository";
import { en } from "../../i18n/en";
import { translate } from "../../i18n/runtime";
import type { OpenToolHandler } from "../../platform/native/tools";
import { CemuIcon } from "../../components/CemuIcon";
import { getReferencePreviewScreen } from "../../dev/referencePreview";
import { titleCover, type TitleCoverName } from "../../assets/titleCovers";

type SortKey = "name" | "lastPlayed" | "playTime";
type ViewMode = "list" | "grid";
const PREVIEW_COVERS: TitleCoverName[] = [
  "open_air",
  "kart",
  "splatter",
  "builder",
  "wind",
  "cosmo",
  "starship",
  "garden",
];

function coverFor(titleId: string): string {
  const hash = [...titleId].reduce(
    (value, char) => value + char.charCodeAt(0),
    0,
  );
  return titleCover(PREVIEW_COVERS[hash % PREVIEW_COVERS.length]);
}

function messageFrom(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

function hoursFromMinutes(minutes: number): string {
  return (minutes / 60).toLocaleString(undefined, {
    maximumFractionDigits: 1,
  });
}

function durationFromMinutes(minutes: number): string {
  return `${Math.floor(minutes / 60)}h ${String(minutes % 60).padStart(2, "0")}m`;
}

export function Library({
  onOpenTool,
  onOpenGame,
}: {
  onOpenTool: OpenToolHandler;
  onOpenGame: (titleId: string) => void;
}) {
  const [titles, setTitles] = useState<Title[]>([]);
  const previewScreen = getReferencePreviewScreen();
  const masterList = previewScreen?.index === 0;
  const [query, setQuery] = useState(
    previewScreen?.index === 3 ? "No matching title" : "",
  );
  const [region, setRegion] = useState("all");
  const [sort, setSort] = useState<SortKey>("name");
  const [view, setView] = useState<ViewMode>(
    previewScreen?.index === 1 ? "grid" : "list",
  );
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [error, setError] = useState("");
  const [selectedId, setSelectedId] = useState<string>();
  const [launchingId, setLaunchingId] = useState<string>();
  const [permissionTitleId, setPermissionTitleId] = useState<string>();
  const iconCache = useRef(new Map<string, string | null>());
  const pendingIcons = useRef(new Set<string>());

  const requestTitleIcons = useCallback(
    (next: Title[]) => {
      if (previewScreen) return;
      for (const title of next) {
        if (
          title.iconDataUrl ||
          iconCache.current.has(title.titleId) ||
          pendingIcons.current.has(title.titleId)
        )
          continue;
        pendingIcons.current.add(title.titleId);
        void loadTitleIcon(title.titleId)
          .then((result) => {
            iconCache.current.set(result.titleId, result.iconDataUrl);
            if (!result.iconDataUrl) return;
            setTitles((current) =>
              current.map((entry) =>
                entry.titleId === result.titleId
                  ? { ...entry, iconDataUrl: result.iconDataUrl ?? undefined }
                  : entry,
              ),
            );
          })
          .catch(() => {
            iconCache.current.delete(title.titleId);
          })
          .finally(() => pendingIcons.current.delete(title.titleId));
      }
    },
    [previewScreen],
  );

  useEffect(() => {
    const syncSearch = (event: Event) =>
      setQuery((event as CustomEvent<string>).detail);
    window.addEventListener("cemu-library-search", syncSearch);
    return () => window.removeEventListener("cemu-library-search", syncSearch);
  }, []);

  const load = useCallback(async () => {
    setLoading(true);
    setError("");
    try {
      const next = await listTitles();
      const hydrated = next.map((title) => ({
        ...title,
        iconDataUrl:
          title.iconDataUrl ||
          iconCache.current.get(title.titleId) ||
          undefined,
      }));
      setTitles(hydrated);
      requestTitleIcons(hydrated);
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
  }, [requestTitleIcons]);

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
          previewScreen && previewScreen.index <= 3
            ? 0
            : sort === "name"
              ? a.name.localeCompare(b.name)
              : sort === "playTime"
                ? b.playTimeMinutes - a.playTimeMinutes
                : (b.lastPlayed ?? "").localeCompare(a.lastPlayed ?? ""),
        ),
    [titles, query, region, sort, previewScreen],
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
      }
      setLaunchingId(undefined);
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
      const hydrated = next.map((title) => ({
        ...title,
        iconDataUrl:
          title.iconDataUrl ||
          iconCache.current.get(title.titleId) ||
          undefined,
      }));
      setTitles(hydrated);
      requestTitleIcons(hydrated);
    } catch (reason) {
      setError(messageFrom(reason));
    } finally {
      setRefreshing(false);
    }
  };

  return (
    <section className="library-page" aria-labelledby="library-title">
      <div className="library-main">
        <header className="page-heading library-heading">
          <div>
            <h1 id="library-title">
              {masterList ? "Game Library" : en.library.title}
            </h1>
            <p>
              {masterList
                ? "Installed Wii U titles, updates, DLC and homebrew."
                : en.library.description}
            </p>
          </div>
          <p className="page-heading__state">
            {visible.length} / {masterList ? 38 : titles.length}{" "}
            {en.library.results}
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
          <div
            className="view-control"
            role="group"
            aria-label={en.library.viewLabel}
          >
            <span className="view-control__label" aria-hidden="true">
              {en.library.viewLabel}
            </span>
            <div className="view-control__buttons">
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
            </div>
          </div>
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
                      }}
                      onDoubleClick={() => void launch(title)}
                      onKeyDown={(event) => {
                        if (event.key === "Enter") {
                          event.preventDefault();
                          void launch(title);
                        }
                      }}
                    >
                      {title.iconDataUrl || previewScreen ? (
                        <img
                          className={
                            title.iconDataUrl && !previewScreen
                              ? "native-title-icon"
                              : undefined
                          }
                          src={title.iconDataUrl || coverFor(title.titleId)}
                          alt=""
                          width="50"
                          height="50"
                        />
                      ) : (
                        <span
                          className="game-thumbnail-placeholder"
                          aria-hidden="true"
                        >
                          <CemuIcon name="library" />
                        </span>
                      )}
                      <span className="game-select__copy">
                        <strong>{title.name}</strong>
                        <code>{title.titleId}</code>
                        <small>WUA</small>
                      </span>
                      <span className="game-select__meta">
                        <b>
                          {title.region} · v{title.version}
                        </b>
                        <small>
                          {en.library.region} · {en.library.version}
                        </small>
                      </span>
                      <span className="game-select__meta">
                        <b>
                          {masterList
                            ? durationFromMinutes(title.playTimeMinutes)
                            : `${hoursFromMinutes(title.playTimeMinutes)}h`}
                        </b>
                        <small>{en.library.playTime}</small>
                      </span>
                      <span className="game-select__meta">
                        <b>{title.lastPlayed || en.library.never}</b>
                        <small>{en.library.lastPlayed}</small>
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
      </div>

      <aside
        className="game-workspace right-rail"
        aria-labelledby="workspace-title"
      >
        {selectedTitle &&
          (selectedTitle.iconDataUrl || previewScreen ? (
            <img
              className={`cover-large ${selectedTitle.iconDataUrl && !previewScreen ? "native-title-icon" : ""}`}
              src={selectedTitle.iconDataUrl || coverFor(selectedTitle.titleId)}
              alt=""
              width="230"
              height="156"
            />
          ) : (
            <span className="cover-large cover-placeholder" aria-hidden="true">
              <CemuIcon name="library" />
            </span>
          ))}
        <header className="workspace-heading">
          <h2 id="workspace-title">
            {selectedTitle?.name ?? en.library.noSelection}
          </h2>
          {selectedTitle && <code>{selectedTitle.titleId}</code>}
        </header>
        {selectedTitle ? (
          <>
            <div className="workspace-actions action-list">
              {masterList ? (
                <>
                  <button
                    className="primary-action"
                    onClick={() => void launch(selectedTitle)}
                  >
                    <CemuIcon name="play" />
                    Launch
                  </button>
                  <button className="danger-action">
                    <CemuIcon name="warning" />
                    Stop
                  </button>
                  <button onClick={() => onOpenGame(selectedTitle.titleId)}>
                    <CemuIcon name="settings" />
                    Edit Profile
                  </button>
                  <button
                    onClick={() =>
                      onOpenTool("graphic-packs", {
                        titleId: selectedTitle.titleId,
                      })
                    }
                  >
                    <CemuIcon name="library" />
                    Graphic Packs
                  </button>
                  <button onClick={() => onOpenTool("input-settings")}>
                    <CemuIcon name="controller" />
                    Input Profile
                  </button>
                  <button onClick={() => onOpenTool("save-manager")}>
                    <CemuIcon name="box" />
                    Save Data
                  </button>
                  <button onClick={() => onOpenTool("cemod-manager")}>
                    <CemuIcon name="mods" />
                    CemuMods
                  </button>
                  <button>
                    <CemuIcon name="folder" />
                    Open Folder
                  </button>
                  <button>
                    <CemuIcon name="link" />
                    Create Shortcut
                  </button>
                </>
              ) : (
                <>
                  <button
                    className="primary-action"
                    disabled={launchingId === selectedTitle.titleId}
                    onClick={() => void launch(selectedTitle)}
                  >
                    <CemuIcon name="play" />
                    {launchingId === selectedTitle.titleId
                      ? en.library.launching
                      : en.library.play}
                  </button>
                  <button onClick={() => onOpenGame(selectedTitle.titleId)}>
                    <CemuIcon name="settings" />
                    {en.library.openWorkspace}
                  </button>
                  <button
                    onClick={() =>
                      onOpenTool("graphic-packs", {
                        titleId: selectedTitle.titleId,
                      })
                    }
                  >
                    <CemuIcon name="library" />
                    {en.library.graphicPacks}
                  </button>
                  <button onClick={() => onOpenTool("input-settings")}>
                    <CemuIcon name="controller" />
                    {en.library.gameSettings}
                  </button>
                  <button onClick={() => onOpenTool("save-manager")}>
                    <CemuIcon name="box" />
                    {en.library.saves}
                  </button>
                  <button onClick={() => onOpenTool("cemod-manager")}>
                    <CemuIcon name="mods" />
                    {en.library.mods}
                  </button>
                  <span className="workspace-status" role="status">
                    {permissionTitleId === selectedTitle.titleId
                      ? en.library.permissionRequired
                      : en.library.ready}
                  </span>
                </>
              )}
            </div>
            <dl className="workspace-facts mini-facts">
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
                  {masterList ? (
                    durationFromMinutes(selectedTitle.playTimeMinutes)
                  ) : (
                    <>
                      {hoursFromMinutes(selectedTitle.playTimeMinutes)}{" "}
                      {translate(en.library.hours)}
                    </>
                  )}
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
          </>
        ) : (
          <div className="library-state">
            <p>{en.library.noSelection}</p>
          </div>
        )}
      </aside>
    </section>
  );
}
