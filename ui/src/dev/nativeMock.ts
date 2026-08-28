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
import {
  getReferencePreviewScreen,
  overlayModeForScreen,
} from "./referencePreview";
import { titleCover } from "../assets/titleCovers";

let previewTheme: "light" | "dark" = "dark";

function createPreviewOverlay(): RuntimeOverlaySnapshot {
  const previewScreen = getReferencePreviewScreen();
  const mode =
    new URLSearchParams(window.location.search).get("overlay") ??
    (previewScreen ? overlayModeForScreen(previewScreen.index) : undefined);
  const snapshot = structuredClone(previewOverlay);
  snapshot.overlayStyle.scale = 100;
  snapshot.notificationStyle.scale = 100;
  if (mode === "stats") {
    snapshot.visibility = {
      fps: true,
      drawCalls: true,
      cpuUsage: true,
      cpuPerCore: true,
      ramUsage: true,
      vramUsage: true,
      debug: false,
    };
    snapshot.stats.debugLines = [];
    snapshot.notices = [];
  } else if (mode === "notifications") {
    snapshot.notices = [
      {
        id: "controller-connected",
        kind: "controller",
        text: "Controller 1 connected",
        remainingMs: 0,
      },
      {
        id: "shader-cache",
        kind: "shader",
        text: "Shader cache updated",
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
  const previewScreen = getReferencePreviewScreen();
  const requestedRole = search.get("role");
  const requestedTheme = search.get("theme");
  if (requestedTheme === "light" || requestedTheme === "dark")
    previewTheme = requestedTheme;
  const windowRole =
    previewScreen?.role ??
    (windowRoles.includes(requestedRole as (typeof windowRoles)[number])
      ? (requestedRole as ActiveWindowRole)
      : "main-library");
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
    activeAccountName:
      previewAccounts.accounts.find(
        (account) =>
          account.persistentId === previewAccounts.activePersistentId,
      )?.miiName ?? "",
    theme: previewTheme,
    themeRevision: "1",
    language: search.get("language") ?? "system",
    languageRevision: "1",
    shuttingDown: previewScreen?.index === 79,
    context,
  };
}

const previewTitles: Title[] = [
  ["00050000101C9400", "Open Air Quest", "EU", 208, 2892, "Today", "open_air"],
  ["00050000101D4E5F", "Kart Party U", "US", 32, 1902, "Yesterday", "kart"],
  [
    "00050000101AA22B",
    "Splatter Arena",
    "JP",
    288,
    1584,
    "2026-08-20",
    "splatter",
  ],
  [
    "000500001010CC33",
    "Builder Blocks",
    "EU",
    16,
    1146,
    "2026-08-16",
    "builder",
  ],
  ["000500001055CC77", "Wind Sailor HD", "EU", 80, 1104, "2026-08-12", "wind"],
  ["0005000010442266", "Captain Cosmo", "US", 32, 422, "2026-08-07", "cosmo"],
  [
    "0005000010DEAD22",
    "Starship Raiders",
    "JP",
    24,
    221,
    "2026-07-30",
    "starship",
  ],
  ["0005000010F1E2D3", "Mystic Garden", "EU", 48, 132, "2026-07-21", "garden"],
  [
    "0005000010AABB11",
    "Homebrew Channel",
    "ALL",
    1,
    84,
    "2026-07-18",
    "homebrew",
  ],
  ["0005000010CCDD22", "NUS Downloader", "ALL", 4, 46, "2026-07-12", "nus"],
  ["0005000010EEFF33", "System Tools", "ALL", 2, 32, "2026-07-05", "system"],
  ["000500001012AB34", "Media Studio", "USA", 12, 18, "2026-06-28", "media"],
].map(
  ([titleId, name, region, version, playTimeMinutes, lastPlayed, cover]) => ({
    titleId: String(titleId),
    name: String(name),
    path: `/games/${String(name).replaceAll(" ", "_")}`,
    region: String(region),
    version: Number(version),
    playTimeMinutes: Number(playTimeMinutes),
    lastPlayed: String(lastPlayed),
    iconDataUrl: titleCover(String(cover)),
  }),
);

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
  const previewScreen = getReferencePreviewScreen();
  if (previewScreen)
    document.documentElement.dataset.previewScreen = String(
      previewScreen.index,
    ).padStart(2, "0");
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
      window.setTimeout(() => dispatch("language.changed", result), 0);
    } else if (request.method === "overlay.getSnapshot")
      result = createPreviewOverlay();
    else if (request.method === "titles.list") {
      const screenIndex = getReferencePreviewScreen()?.index;
      result =
        screenIndex === 2
          ? []
          : screenIndex === 1
            ? previewTitles
            : previewTitles.slice(0, 8);
    } else if (request.method === "titles.icon") {
      const titleId = (request.params as { titleId: string }).titleId;
      result = {
        titleId,
        iconDataUrl:
          previewTitles.find((title) => title.titleId === titleId)
            ?.iconDataUrl ?? null,
      };
    } else if (request.method === "settings.getFrontend")
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
    } else if (request.method === "workspace.activate") {
      result = {};
    } else if (request.method === "about.get") {
      result = {
        name: "CemuExtend",
        version: "preview",
        commit: "preview-build",
        buildDate: "Preview data",
        frontend: "cef-react",
        browserEngine: "Browser preview",
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
