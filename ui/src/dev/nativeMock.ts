import type {
  ActiveWindowRole,
  Bootstrap,
  RpcRequest,
  RpcResponse,
  RuntimeOverlaySnapshot,
  Title,
} from "../bridge/contracts";
import { windowRoles } from "../generated/roles";
import {
  previewAccounts,
  previewAudio,
  previewCemod,
  previewChecksum,
  previewFrontendSettings,
  previewHotkeys,
  previewInput,
  previewLogging,
  previewOverlay,
  previewPacks,
  previewSaves,
  previewTextures,
  previewThreads,
  previewTitles as previewTitleManager,
  previewUpdates,
  previewUsb,
} from "./previewData";

let previewTheme: "light" | "dark" = "light";

function createPreviewOverlay(): RuntimeOverlaySnapshot {
  const mode = new URLSearchParams(window.location.search).get("overlay");
  const snapshot = structuredClone(previewOverlay);
  snapshot.overlayStyle.scale = 100;
  snapshot.notificationStyle.scale = 100;
  if (mode === "stats") {
    snapshot.overlayStyle.scale = 200;
    snapshot.visibility = {
      fps: true,
      drawCalls: true,
      cpuUsage: true,
      cpuPerCore: true,
      ramUsage: true,
      vramUsage: true,
      debug: true,
    };
    snapshot.stats.debugLines = [
      {
        label: "Long diagnostic label used for wrapping verification",
        value:
          "A deliberately long runtime value that must stay inside the render surface",
      },
    ];
    snapshot.notices = [
      {
        id: "preview-notice",
        kind: "controller",
        text: "A deliberately long controller notification that must wrap without leaving the render surface.",
        remainingMs: 0,
      },
    ];
  } else if (mode === "keyboard") {
    snapshot.keyboard = {
      generation: "2",
      active: true,
      keyboardOnly: false,
      shifted: false,
      maximumLength: 64,
      text: "Preview keyboard layout",
    };
    snapshot.interaction = "softwareKeyboard";
  } else if (mode === "error") {
    snapshot.errorDialog = {
      generation: "2",
      active: true,
      title: "The current operation could not be completed",
      message:
        "This deliberately long diagnostic message verifies wrapping at narrow render-surface widths.",
      leftButton: "Cancel",
      rightButton: "Try again",
      opacity: 1,
    };
    snapshot.interaction = "errorDialog";
  } else if (mode === "shader") {
    snapshot.shaderProgress = {
      generation: "2",
      visible: true,
      pipelines: false,
      current: 984,
      total: 2048,
      vertexShaders: 520,
      pixelShaders: 432,
      geometryShaders: 32,
      backgroundImageAvailable: false,
    };
  }
  return snapshot;
}

function createPreviewBootstrap(): Bootstrap {
  const search = new URLSearchParams(window.location.search);
  const requestedRole = search.get("role");
  const requestedTheme = search.get("theme");
  if (requestedTheme === "light" || requestedTheme === "dark")
    previewTheme = requestedTheme;
  const windowRole = windowRoles.includes(
    requestedRole as (typeof windowRoles)[number],
  )
    ? (requestedRole as ActiveWindowRole)
    : "main-library";
  const context =
    windowRole === "cemod-permissions"
      ? {
          titleId: "0005000000000001",
          packageKey: previewCemod.packages[0].packageKey,
          generation: previewCemod.generation,
        }
      : undefined;
  return {
    windowId: "1",
    windowRole,
    appVersion: "preview",
    platform: "browser preview",
    theme: previewTheme,
    themeRevision: "1",
    language: search.get("language") ?? "system",
    languageRevision: "1",
    shuttingDown: false,
    context,
  };
}

const previewTitles: Title[] = [
  {
    titleId: "0005000000000001",
    name: "Preview Adventure",
    path: "/preview/adventure",
    region: "USA",
    version: 1,
    playTimeMinutes: 0,
    lastPlayed: null,
  },
  {
    titleId: "0005000000000002",
    name: "Preview Title With a Deliberately Long Name",
    path: "/preview/long-title-layout-test",
    region: "EUR",
    version: 1,
    playTimeMinutes: 0,
    lastPlayed: null,
  },
  {
    titleId: "0005000000000003",
    name: "Preview Racing",
    path: "/preview/racing",
    region: "JPN",
    version: 1,
    playTimeMinutes: 0,
    lastPlayed: null,
  },
  {
    titleId: "0005000000000004",
    name: "Preview Platformer",
    path: "/preview/platformer",
    region: "USA",
    version: 1,
    playTimeMinutes: 0,
    lastPlayed: null,
  },
];

let eventSequence = 100;

function dispatch(type: string, payload: unknown): void {
  window.__cemuDispatchEvent?.({
    type,
    sequence: String(++eventSequence),
    payload,
  });
}

export function installDevNativeMock(): void {
  const previewRequested =
    new URLSearchParams(window.location.search).get("preview") === "1";
  if (window.cemuInvoke && !previewRequested) return;
  const bootstrap = createPreviewBootstrap();
  if (bootstrap.windowRole === "runtime-overlay")
    document.documentElement.dataset.runtimeOverlay = "active";
  window.__CEMU_BOOTSTRAP__ = bootstrap;
  window.cemuInvoke = async (json: string): Promise<string> => {
    const request = JSON.parse(json) as RpcRequest;
    let result: unknown = {};
    if (request.method === "system.bootstrap")
      result = { ...bootstrap, theme: previewTheme };
    else if (request.method === "theme.get")
      result = { theme: previewTheme, revision: String(eventSequence) };
    else if (request.method === "theme.set") {
      previewTheme = (request.params as { theme: "light" | "dark" }).theme;
      const revision = String(++eventSequence);
      result = { theme: previewTheme, revision };
      window.setTimeout(
        () => dispatch("theme.changed", { theme: previewTheme, revision }),
        0,
      );
    } else if (request.method === "language.get")
      result = {
        language: bootstrap.language,
        revision: bootstrap.languageRevision,
      };
    else if (request.method === "language.set") {
      bootstrap.language = (request.params as { language: string }).language;
      bootstrap.languageRevision = String(++eventSequence);
      result = {
        language: bootstrap.language,
        revision: bootstrap.languageRevision,
      };
      window.setTimeout(
        () => dispatch("language.changed", result),
        0,
      );
    } else if (request.method === "overlay.getSnapshot")
      result = createPreviewOverlay();
    else if (request.method === "titles.list") result = previewTitles;
    else if (request.method === "settings.getFrontend")
      result = previewFrontendSettings;
    else if (request.method === "settings.applyFrontend")
      result = {
        ok: true,
        error: "none",
        snapshot: previewFrontendSettings,
        diagnostic: "",
      };
    else if (request.method === "accounts.getModel") result = previewAccounts;
    else if (request.method === "input.getModel") result = previewInput;
    else if (
      request.method.startsWith("input.") &&
      request.method !== "input.enumerate"
    )
      result = previewInput;
    else if (request.method === "input.enumerate")
      result = [
        {
          token: 2,
          api: "SDL",
          displayName: "Additional Preview Controller",
          connected: true,
        },
      ];
    else if (request.method === "hotkeys.get") result = previewHotkeys;
    else if (request.method === "hotkeys.apply")
      result = {
        ok: true,
        error: "none",
        snapshot: previewHotkeys,
        diagnostic: "",
      };
    else if (request.method === "graphicPacks.list") result = previewPacks;
    else if (
      request.method === "graphicPacks.setEnabled" ||
      request.method === "graphicPacks.setPreset" ||
      request.method === "graphicPacks.reload" ||
      request.method === "graphicPacks.save" ||
      request.method === "graphicPacks.refresh"
    )
      result = {
        changed: true,
        titleRunning: false,
        requiresRestart: false,
        applied: true,
        reloaded: true,
        diagnostic: "",
        info: previewPacks[0],
      };
    else if (request.method === "titleManager.getModel")
      result = previewTitleManager;
    else if (request.method === "checksum.getModel") result = previewChecksum;
    else if (request.method === "save.getModel") result = previewSaves;
    else if (request.method === "updates.getModel") result = previewUpdates;
    else if (request.method === "logging.getSnapshot") result = previewLogging;
    else if (request.method === "logging.clear")
      result = { clearedThroughSequence: "18" };
    else if (
      request.method === "usb.getModel" ||
      request.method === "usb.setEnabled"
    )
      result = previewUsb;
    else if (request.method === "diagnostics.ppcThreadsSnapshot")
      result = previewThreads;
    else if (request.method === "diagnostics.ppcThreadCommand")
      result = { applied: true, diagnostic: "" };
    else if (request.method === "diagnostics.getTextureRelations")
      result = previewTextures;
    else if (request.method === "diagnostics.getAudioVoices")
      result = previewAudio;
    else if (request.method === "cemod.discover") {
      const jobId = String(eventSequence + 1);
      result = { jobId };
      window.setTimeout(
        () =>
          dispatch("cemod.snapshot", {
            jobId,
            ok: true,
            error: "none",
            diagnostic: "",
            snapshot: previewCemod,
          }),
        40,
      );
    } else if (request.method === "titles.launch") {
      const titleId = (request.params as { titleId: string }).titleId;
      result = { status: "started", titleId };
      window.setTimeout(
        () => dispatch("titles.launchState", { status: "started", titleId }),
        80,
      );
    } else if (request.method === "window.open") {
      const { requestId } = request.params as { requestId: string };
      window.setTimeout(
        () =>
          dispatch("window.opened", {
            requestId,
            windowId: String(eventSequence + 1),
          }),
        80,
      );
    } else if (request.method === "about.get") {
      result = {
        name: "CemuExtend",
        version: "preview",
        commit: "preview-build",
        buildDate: "Preview data",
        frontend: "WebView React UI",
        webviewEngine: "Browser preview",
        originalAuthors: ["Preview author"],
        libraries: [
          {
            name: "React",
            license: "MIT",
            url: "https://react.dev/",
          },
        ],
        links: [],
      };
    } else if (request.method === "ppcDebugger.snapshot") {
      result = {
        generation: "1",
        available: true,
        trapped: true,
        instructionPointer: "10000008",
        linkRegister: "10000100",
        gpr: Array.from({ length: 32 }, (_, index) =>
          (0x1000 + index * 4).toString(16).padStart(8, "0"),
        ),
        instructions: Array.from({ length: 18 }, (_, index) => {
          const address = (0x10000000 + index * 4)
            .toString(16)
            .padStart(8, "0");
          return {
            address,
            opcode: "60000000",
            mnemonic: index === 2 ? "blr" : "nop",
            operands: "",
            current: index === 2,
            breakpoint: index === 7,
          };
        }),
        breakpoints: [
          {
            identity: "preview-breakpoint",
            address: "1000001c",
            enabled: true,
            logging: false,
          },
        ],
        breakpointCapReached: false,
        diagnostic: "",
      };
    }
    const response: RpcResponse = { id: request.id, ok: true, result };
    return JSON.stringify(response);
  };
}
