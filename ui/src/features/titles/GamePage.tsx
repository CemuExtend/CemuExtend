import { useCallback, useEffect, useState } from "react";
import type { Title } from "../../bridge/contracts";
import {
  launchTitle,
  listTitles,
  subscribeToTitles,
} from "../../domain/titles/repository";
import { en } from "../../i18n/en";
import type { OpenToolHandler } from "../../platform/native/tools";
import { getReferencePreviewScreen } from "../../dev/referencePreview";

type GameTab =
  "overview" | "mods" | "graphic-packs" | "saves" | "settings" | "play";
type LaunchStatus = "ready" | "starting" | "started" | "permission" | "failed";

const GAME_TABS: Array<{ id: GameTab; label: string }> = [
  { id: "overview", label: en.game.tabs.overview },
  { id: "mods", label: en.game.tabs.mods },
  { id: "graphic-packs", label: en.game.tabs.graphicPacks },
  { id: "saves", label: en.game.tabs.saves },
  { id: "settings", label: en.game.tabs.settings },
  { id: "play", label: en.game.tabs.play },
];

const LAUNCH_LABELS: Record<LaunchStatus, string> = {
  ready: en.game.launchReady,
  starting: en.game.launchStarting,
  started: en.game.launchStarted,
  permission: en.game.launchPermission,
  failed: en.game.launchFailed,
};

function messageFrom(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

function hoursFromMinutes(minutes: number): string {
  return (minutes / 60).toLocaleString(undefined, {
    maximumFractionDigits: 1,
  });
}

export function GamePage({
  titleId,
  onBack,
  onOpenTool,
}: {
  titleId: string;
  onBack: () => void;
  onOpenTool: OpenToolHandler;
}) {
  const previewIndex = getReferencePreviewScreen()?.index;
  const previewTab: GameTab | undefined =
    previewIndex !== undefined && previewIndex >= 4 && previewIndex <= 9
      ? GAME_TABS[previewIndex - 4]?.id
      : previewIndex === 76
        ? "settings"
        : previewIndex === 75
          ? "overview"
          : undefined;
  const [title, setTitle] = useState<Title>();
  const [activeTab, setActiveTab] = useState<GameTab>(previewTab ?? "overview");
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [launchStatus, setLaunchStatus] = useState<LaunchStatus>("ready");

  const load = useCallback(async () => {
    setLoading(true);
    setError("");
    try {
      const titles = await listTitles();
      setTitle(titles.find((candidate) => candidate.titleId === titleId));
    } catch (reason) {
      setError(messageFrom(reason));
    } finally {
      setLoading(false);
    }
  }, [titleId]);

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
      if (event.state.titleId !== titleId) return;
      if (event.state.status === "awaitingPermission")
        setLaunchStatus("permission");
      else if (event.state.status === "started") setLaunchStatus("started");
      else if (event.state.status === "shutdown") setLaunchStatus("ready");
      else {
        setLaunchStatus("failed");
        setError(event.state.diagnostic || en.library.launchFailed);
      }
    });
  }, [load, titleId]);

  const launch = async () => {
    setLaunchStatus("starting");
    setError("");
    try {
      const result = await launchTitle(titleId);
      setLaunchStatus(
        result.status === "awaitingPermission" ? "permission" : "started",
      );
    } catch (reason) {
      setLaunchStatus("failed");
      setError(messageFrom(reason));
    }
  };

  if (loading)
    return (
      <section className="game-page game-page--state" aria-busy="true">
        <span className="spinner" aria-hidden="true" />
        <p>{en.game.loading}</p>
      </section>
    );

  if (error && !title)
    return (
      <section className="game-page game-page--state" role="alert">
        <h1>{en.game.notFound}</h1>
        <p>{error}</p>
        <div className="button-row">
          <button onClick={onBack}>{en.game.back}</button>
          <button className="button-primary" onClick={() => void load()}>
            {en.game.retry}
          </button>
        </div>
      </section>
    );

  if (!title)
    return (
      <section className="game-page game-page--state">
        <h1>{en.game.notFound}</h1>
        <button onClick={onBack}>{en.game.back}</button>
      </section>
    );

  const managerAction = () => {
    if (activeTab === "mods") onOpenTool("cemod-manager");
    else if (activeTab === "graphic-packs")
      onOpenTool("graphic-packs", { titleId });
    else if (activeTab === "saves") onOpenTool("save-manager");
    else onOpenTool("general-settings");
  };

  return (
    <section className="game-page" aria-labelledby="game-page-title">
      <button className="game-page__back" onClick={onBack}>
        ← {en.game.back}
      </button>

      <header className="game-page__header">
        <span className="game-page__monogram" aria-hidden="true">
          {title.name.slice(0, 1)}
        </span>
        <div className="game-page__identity">
          <p>{en.game.workspace}</p>
          <h1 id="game-page-title">{title.name}</h1>
          <code>{title.titleId}</code>
        </div>
        <div className="game-page__launch">
          <span data-state={launchStatus}>{LAUNCH_LABELS[launchStatus]}</span>
          <button
            className="button-primary"
            data-state={launchStatus === "starting" ? "loading" : undefined}
            disabled={launchStatus === "starting"}
            onClick={() => void launch()}
          >
            {launchStatus === "starting"
              ? en.library.launching
              : en.game.launchAction}
          </button>
        </div>
      </header>

      {error && (
        <div className="inline-notice" role="alert">
          <span>{error}</span>
          <button onClick={() => setError("")}>{en.shell.dismiss}</button>
        </div>
      )}

      <div className="game-page__workspace">
        <nav className="game-page__tabs" aria-label={en.game.workspace}>
          {GAME_TABS.map((tab) => (
            <button
              key={tab.id}
              aria-current={activeTab === tab.id ? "page" : undefined}
              onClick={() => setActiveTab(tab.id)}
            >
              <span aria-hidden="true">{activeTab === tab.id ? "■" : "□"}</span>
              {tab.label}
            </button>
          ))}
        </nav>

        <section className="game-page__panel" aria-live="polite">
          {activeTab === "overview" && (
            <>
              <header className="panel-heading">
                <h2>{en.game.overviewTitle}</h2>
                <p>{en.game.overviewDescription}</p>
              </header>
              <dl className="game-page__facts">
                <div>
                  <dt>{en.library.region}</dt>
                  <dd>{title.region}</dd>
                </div>
                <div>
                  <dt>{en.library.version}</dt>
                  <dd>{title.version}</dd>
                </div>
                <div>
                  <dt>{en.library.playTime}</dt>
                  <dd>
                    {hoursFromMinutes(title.playTimeMinutes)} {en.library.hours}
                  </dd>
                </div>
                <div>
                  <dt>{en.library.lastPlayed}</dt>
                  <dd>{title.lastPlayed || en.library.never}</dd>
                </div>
                <div className="game-page__fact-wide">
                  <dt>{en.library.path}</dt>
                  <dd>{title.path}</dd>
                </div>
              </dl>
              <section className="game-operations">
                <h2>{en.game.quickActions}</h2>
                <div className="game-operations__list">
                  <button onClick={() => setActiveTab("mods")}>
                    {en.game.tabs.mods}
                  </button>
                  <button onClick={() => setActiveTab("graphic-packs")}>
                    {en.game.tabs.graphicPacks}
                  </button>
                  <button onClick={() => setActiveTab("saves")}>
                    {en.game.tabs.saves}
                  </button>
                  <button onClick={() => setActiveTab("settings")}>
                    {en.game.tabs.settings}
                  </button>
                </div>
              </section>
            </>
          )}

          {activeTab === "mods" && (
            <GameManagerPanel
              title={en.game.modsTitle}
              description={en.game.modsDescription}
              note={en.game.modsNote}
              actionLabel={en.game.openCemod}
              onOpen={managerAction}
            />
          )}
          {activeTab === "graphic-packs" && (
            <GameManagerPanel
              title={en.game.packsTitle}
              description={en.game.packsDescription}
              actionLabel={en.game.openPacks}
              onOpen={managerAction}
            />
          )}
          {activeTab === "saves" && (
            <GameManagerPanel
              title={en.game.savesTitle}
              description={en.game.savesDescription}
              actionLabel={en.game.openSaves}
              onOpen={managerAction}
            />
          )}
          {activeTab === "settings" && (
            <GameManagerPanel
              title={en.game.settingsTitle}
              description={en.game.settingsDescription}
              actionLabel={en.game.openSettings}
              onOpen={managerAction}
            />
          )}
          {activeTab === "play" && (
            <section className="game-launch-panel">
              <header className="panel-heading">
                <h2>{en.game.playTitle}</h2>
                <p>{en.game.playDescription}</p>
              </header>
              <dl className="launch-checklist">
                <div>
                  <dt>{en.game.titleIdentity}</dt>
                  <dd>{title.titleId}</dd>
                </div>
                <div>
                  <dt>{en.game.contentVersion}</dt>
                  <dd>{title.version}</dd>
                </div>
                <div>
                  <dt>{en.library.path}</dt>
                  <dd>{title.path}</dd>
                </div>
              </dl>
              <button className="button-primary" onClick={() => void launch()}>
                {en.game.launchAction}
              </button>
            </section>
          )}
        </section>
      </div>
    </section>
  );
}

function GameManagerPanel({
  title,
  description,
  note,
  actionLabel,
  onOpen,
}: {
  title: string;
  description: string;
  note?: string;
  actionLabel: string;
  onOpen: () => void;
}) {
  return (
    <section className="game-manager-panel">
      <header className="panel-heading">
        <h2>{title}</h2>
        <p>{description}</p>
      </header>
      {note && <p className="game-manager-panel__note">{note}</p>}
      <button className="button-primary" onClick={onOpen}>
        {actionLabel}
      </button>
    </section>
  );
}
