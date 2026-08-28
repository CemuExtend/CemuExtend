import { useEffect, useState } from "react";
import type { Bootstrap, ImplementedToolWindowRole } from "../bridge/contracts";
import { CemuIcon, type CemuIconName } from "../components/CemuIcon";
import {
  MenuBar,
  StatusBar,
  ToolButton,
  type MenuGroup,
} from "../components/WindowChrome";
import { GamePage } from "../features/titles/GamePage";
import { Library } from "../features/titles/Library";
import { en } from "../i18n/en";
import { translate, translateFormat } from "../i18n/runtime";
import type { OpenToolHandler, ToolContext } from "../platform/native/tools";
import { invoke } from "../bridge/native";
import {
  getReferencePreviewScreen,
  mainRouteForScreen,
} from "../dev/referencePreview";
import { EmbeddedToolWorkspace } from "./EmbeddedToolWorkspace";
import {
  DEFAULT_WORKSPACE_ROLE,
  workspaceRouteForRole,
  type WorkspaceRoute,
} from "./workspaceRegistry";
import {
  developerNavigation,
  helpNavigation,
  primaryNavigation,
  routeLabel,
  type AppRoute,
  type MainRoute,
} from "./navigation";

const ROUTE_ICONS: Record<MainRoute, CemuIconName> = {
  library: "library",
  mods: "mods",
  downloads: "download",
  controllers: "controller",
  accounts: "account",
  settings: "settings",
  developer: "tools",
  help: "help",
};

export function AppShell({ bootstrap }: { bootstrap: Bootstrap }) {
  const previewGameId = import.meta.env.DEV
    ? (new URLSearchParams(window.location.search).get("game") ??
      ((getReferencePreviewScreen()?.index ?? -1) >= 4 &&
      ((getReferencePreviewScreen()?.index ?? -1) <= 9 ||
        (getReferencePreviewScreen()?.index ?? -1) === 75 ||
        (getReferencePreviewScreen()?.index ?? -1) === 76)
        ? "00050000101C9400"
        : undefined))
    : undefined;
  const previewRoute = getReferencePreviewScreen();
  const masterLibrary = previewRoute?.index === 0;
  const [route, setRoute] = useState<AppRoute>(
    previewRoute
      ? (mainRouteForScreen(previewRoute.index) ?? "library")
      : previewGameId
        ? "game"
        : "library",
  );
  const [activeGameId, setActiveGameId] = useState<string | undefined>(
    previewGameId,
  );
  const [workspaceRoles, setWorkspaceRoles] = useState(DEFAULT_WORKSPACE_ROLE);
  const [toolContexts, setToolContexts] = useState<
    Partial<Record<string, ToolContext>>
  >({});
  const [notice, setNotice] = useState("");
  const [activeAccountName, setActiveAccountName] = useState(
    bootstrap.activeAccountName,
  );
  const [activatedNativeRole, setActivatedNativeRole] = useState<
    ImplementedToolWindowRole | "main-library" | null
  >("main-library");
  const activeNativeRole =
    route === "library" || route === "game"
      ? "main-library"
      : workspaceRoles[route];

  useEffect(() => {
    let current = true;
    setActivatedNativeRole(null);
    void invoke("workspace.activate", { role: activeNativeRole })
      .then(() => {
        if (!current) return;
        setActivatedNativeRole(activeNativeRole);
        setNotice("");
      })
      .catch((reason: unknown) => {
        if (!current) return;
        setNotice(reason instanceof Error ? reason.message : String(reason));
      });
    return () => {
      current = false;
    };
  }, [activeNativeRole]);

  useEffect(() => {
    setActiveAccountName(bootstrap.activeAccountName);
  }, [bootstrap.activeAccountName]);

  useEffect(() => {
    const updateActiveAccount = (event: Event) => {
      const name = (event as CustomEvent<unknown>).detail;
      if (typeof name === "string") setActiveAccountName(name);
    };
    window.addEventListener("cemu-active-account-changed", updateActiveAccount);
    return () =>
      window.removeEventListener(
        "cemu-active-account-changed",
        updateActiveAccount,
      );
  }, []);

  const handleOpenTool: OpenToolHandler = (role, context) => {
    const workspace = workspaceRouteForRole(role);
    if (!workspace) {
      setNotice(`The ${role} view is available only as a modal dialog.`);
      return;
    }
    setWorkspaceRoles((current) => ({ ...current, [workspace]: role }));
    setToolContexts((current) => ({ ...current, [role]: context }));
    setNotice("");
    setRoute(workspace);
  };

  const navigate = (nextRoute: MainRoute) => setRoute(nextRoute);
  const openGame = (titleId: string) => {
    setActiveGameId(titleId);
    setRoute("game");
  };
  const toolbarRoutes = [
    ...primaryNavigation,
    developerNavigation,
    helpNavigation,
  ];
  const menuGroups: MenuGroup[] = [
    {
      label: "File",
      commands: [
        {
          label: "Load…",
          onSelect: () => handleOpenTool("title-manager"),
        },
        {
          label: "Install game title, update or DLC…",
          onSelect: () => handleOpenTool("title-manager"),
        },
        { separator: true },
        {
          label: "Game paths…",
          onSelect: () => handleOpenTool("general-settings"),
        },
        { separator: true },
        { label: "Exit", onSelect: () => void invoke("window.close") },
      ],
    },
    {
      label: "Options",
      commands: [
        {
          label: "Graphic packs",
          onSelect: () => handleOpenTool("graphic-packs"),
        },
        {
          label: "General settings",
          onSelect: () => handleOpenTool("general-settings"),
        },
        {
          label: "Input settings",
          onSelect: () => handleOpenTool("input-settings"),
        },
        {
          label: "Hotkey settings",
          onSelect: () => handleOpenTool("hotkey-settings"),
        },
        {
          label: "Active accounts",
          onSelect: () => handleOpenTool("account-manager"),
        },
      ],
    },
    {
      label: "Tools",
      commands: [
        {
          label: "Memory searcher",
          onSelect: () => handleOpenTool("memory-searcher"),
        },
        {
          label: "Title Manager",
          onSelect: () => handleOpenTool("title-manager"),
        },
        {
          label: "Download Manager",
          onSelect: () => handleOpenTool("update-manager"),
        },
        {
          label: "Emulated USB Devices",
          onSelect: () => handleOpenTool("emulated-usb-devices"),
        },
      ],
    },
    {
      label: "CPU",
      commands: [
        {
          label: "PPC debugger",
          onSelect: () => handleOpenTool("ppc-debugger"),
        },
        {
          label: "PPC threads",
          onSelect: () => handleOpenTool("ppc-threads"),
        },
      ],
    },
    {
      label: "NFC",
      commands: [
        {
          label: "Emulated USB devices…",
          onSelect: () => handleOpenTool("emulated-usb-devices"),
        },
      ],
    },
    {
      label: "Debug",
      commands: [
        {
          label: "Open logging window",
          onSelect: () => handleOpenTool("logging"),
        },
        {
          label: "View PPC threads",
          onSelect: () => handleOpenTool("ppc-threads"),
        },
        {
          label: "View PPC debugger",
          onSelect: () => handleOpenTool("ppc-debugger"),
        },
        {
          label: "View audio debugger",
          onSelect: () => handleOpenTool("audio-debugger"),
        },
        {
          label: "View texture cache info",
          onSelect: () => handleOpenTool("texture-relations"),
        },
      ],
    },
    {
      label: "Help",
      commands: [
        {
          label: "Check for updates",
          onSelect: () => handleOpenTool("update-manager"),
        },
        { label: "About CemuExtend", onSelect: () => handleOpenTool("about") },
      ],
    },
  ];

  return (
    <div className="app-shell app-window">
      <MenuBar groups={menuGroups} />
      <div className="toolbar shell-toolbar">
        <div className="toolbar-left">
          <ToolButton
            icon="library"
            onClick={() => handleOpenTool("general-settings")}
          >
            Add Game
          </ToolButton>
          <ToolButton
            icon="download"
            onClick={() => handleOpenTool("title-manager")}
          >
            Import
          </ToolButton>
          <ToolButton
            icon="folder"
            onClick={() => handleOpenTool("general-settings")}
          >
            Folders
          </ToolButton>
          <i className="toolbar-separator" aria-hidden="true" />
          {toolbarRoutes.map((item) => (
            <ToolButton
              key={item.route}
              icon={ROUTE_ICONS[item.route]}
              active={route === item.route}
              onClick={() => navigate(item.route)}
            >
              {item.label}
            </ToolButton>
          ))}
        </div>
        <div className="toolbar-right">
          {route === "library" && (
            <label className="searchbox">
              <CemuIcon name="search" />
              <input
                aria-label={en.library.searchLabel}
                placeholder={
                  masterLibrary ? "Search" : en.library.searchPlaceholder
                }
                onChange={(event) =>
                  window.dispatchEvent(
                    new CustomEvent("cemu-library-search", {
                      detail: event.target.value,
                    }),
                  )
                }
              />
            </label>
          )}
          {masterLibrary && (
            <button className="profile-button">Profiles ▾</button>
          )}
          {previewRoute?.index === 16 && (
            <details className="job-center" open>
              <summary>
                <CemuIcon name="download" />
                {en.shell.jobs} · 3
              </summary>
              <div className="job-center__panel">
                <>
                  <strong>Active jobs</strong>
                  <p>Installing title update · 64%</p>
                  <progress max="100" value="64" />
                  <p>Refreshing graphic packs · 38%</p>
                  <progress max="100" value="38" />
                  <p>Verifying content · queued</p>
                </>
              </div>
            </details>
          )}
          <button
            className="profile-button"
            aria-label={translateFormat("User profile: {name}", {
              name: activeAccountName || "—",
            })}
            title={activeAccountName || undefined}
            onClick={() => navigate("accounts")}
          >
            <CemuIcon name="account" />
            <span data-i18n-ignore>{activeAccountName || "—"}</span>
          </button>
        </div>
      </div>

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
        ) : activatedNativeRole === activeNativeRole ? (
          <EmbeddedToolWorkspace
            route={route as WorkspaceRoute}
            role={workspaceRoles[route as WorkspaceRoute]}
            windowId={bootstrap.windowId}
            context={toolContexts[workspaceRoles[route as WorkspaceRoute]]}
            onSelectRole={async (role) => handleOpenTool(role)}
          />
        ) : (
          <div className="workspace-loading" role="status" aria-live="polite">
            <span className="spinner" aria-hidden="true" />
            <span>Loading {routeLabel(route)}…</span>
          </div>
        )}
      </main>
      <StatusBar
        left={
          masterLibrary ? (
            <>
              <CemuIcon name="check" />
              {translateFormat("Library scan complete · {count} titles found", {
                count: 38,
              })}
            </>
          ) : (
            <>
              <CemuIcon name="check" /> {translate("Ready")} ·{" "}
              {translate(routeLabel(route))}
            </>
          )
        }
        right={
          masterLibrary
            ? "Total playtime: 186h"
            : `${bootstrap.platform} · CEF host`
        }
      />
    </div>
  );
}
