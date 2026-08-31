import type {
  AccountManagerModel,
  AudioVoiceDiagnosticPage,
  CemodManagerSnapshot,
  ChecksumModel,
  EmulatedUsbModel,
  FrontendSettings,
  GraphicPack,
  HotkeySettingsModel,
  InputSettingsModel,
  LoggingSnapshot,
  PpcThreadsModel,
  RuntimeOverlaySnapshot,
  SaveManagerModel,
  TextureDiagnosticPage,
  TitleManagerModel,
  UpdateManagerModel,
} from "../bridge/contracts";

export const previewFrontendSettings: FrontendSettings = {
  revision: 7,
  gamePaths: [
    "/games/Wii U Library",
    "/mnt/archive/long-path-layout-check/CemuExtend Games",
  ],
  startFullscreen: false,
  openPad: true,
  checkUpdates: true,
  saveScreenshots: true,
  updateChecksSupported: true,
  portableMode: false,
  titleRunning: false,
  setupCompleted: true,
  fullscreenOverride: null,
};

export const previewOverlay: RuntimeOverlaySnapshot = {
  sequence: "1",
  overlayStyle: { position: "topLeft", color: 0xffffffff, scale: 1 },
  notificationStyle: { position: "topRight", color: 0xffffffff, scale: 1 },
  visibility: {
    fps: false,
    drawCalls: false,
    cpuUsage: false,
    cpuPerCore: false,
    ramUsage: false,
    vramUsage: false,
    debug: false,
  },
  stats: {
    fps: 60,
    drawCalls: 3412,
    fastDrawCalls: 2988,
    cpuUsage: 26.42,
    cpuPerCore: [31.8, 24.05, 18.61],
    ramUsageMb: 4812,
    vramUsageMb: 6144,
    vramTotalMb: 12288,
    debugLines: [],
  },
  notices: [],
  shaderProgress: {
    generation: "1",
    visible: false,
    pipelines: false,
    current: 0,
    total: 0,
    vertexShaders: 0,
    pixelShaders: 0,
    geometryShaders: 0,
    backgroundImageAvailable: false,
  },
  keyboard: {
    generation: "1",
    active: false,
    keyboardOnly: false,
    shifted: false,
    maximumLength: 32,
    text: "",
  },
  errorDialog: {
    generation: "1",
    active: false,
    title: "",
    message: "",
    leftButton: "",
    rightButton: "",
    opacity: 1,
  },
  interaction: "passive",
};

export const previewAccounts: AccountManagerModel = {
  accounts: [
    {
      persistentId: 2147483649,
      persistentIdHex: "80000001",
      miiName: "Preview Player",
      birthYear: 1990,
      birthMonth: 7,
      birthDay: 15,
      gender: 0,
      email: "preview@example.invalid",
      country: 49,
      validOnlineAccount: true,
    },
    {
      persistentId: 2147483650,
      persistentIdHex: "80000002",
      miiName: "A Deliberately Long Account Name",
      birthYear: 2000,
      birthMonth: 12,
      birthDay: 31,
      gender: 1,
      email: "long-preview-address@example.invalid",
      country: 1,
      validOnlineAccount: false,
    },
  ],
  countries: [
    { code: 49, name: "Japan" },
    { code: 1, name: "United States" },
  ],
  nextPersistentId: 2147483651,
  hasFreeSlots: true,
  activePersistentId: 2147483649,
  titleRunning: false,
  networkSettings: [
    {
      persistentId: 2147483649,
      service: "pretendo",
      validation: {
        validAccount: true,
        otp: "ok",
        seeprom: "ok",
        missingFiles: [],
        accountError: "none",
      },
    },
    {
      persistentId: 2147483650,
      service: "offline",
      validation: {
        validAccount: false,
        otp: "missing",
        seeprom: "missing",
        missingFiles: ["otp.bin", "seeprom.bin"],
        accountError: "noAccountId",
      },
    },
  ],
  onlineEnvironment: {
    requiredFilesAvailable: true,
    otpPresent: true,
    seepromPresent: true,
    consoleCertificateAvailable: true,
  },
};

export const previewInput: InputSettingsModel = {
  generation: 3,
  profiles: ["Default", "Preview profile with a long name"],
  availableApis: ["SDL", "Keyboard"],
  players: Array.from({ length: 4 }, (_, player) => ({
    player,
    type:
      player === 0 ? "gamePad" : player === 1 ? "proController" : "disabled",
    gameProfileLocked: false,
    profileName: player === 0 ? "Default" : "",
    controllers:
      player === 0
        ? [
            {
              token: 1,
              api: "SDL",
              displayName: "Preview Wireless Controller With Long Device Name",
              connected: true,
              hasBattery: true,
              lowBattery: false,
              hasMotion: true,
              hasRumble: true,
              settings: {
                axis: { deadzone: 15, range: 100 },
                rotation: { deadzone: 12, range: 100 },
                trigger: { deadzone: 5, range: 100 },
                rumble: 80,
                motion: true,
              },
            },
          ]
        : [],
    mappings: [
      { mappingId: 1, label: "A", binding: "Button South", controllerToken: 1 },
      { mappingId: 2, label: "B", binding: "Button East", controllerToken: 1 },
      {
        mappingId: 3,
        label: "Left Stick X",
        binding: "Axis 0",
        controllerToken: 1,
      },
      { mappingId: 4, label: "Home", binding: "Unassigned" },
    ],
  })) as InputSettingsModel["players"],
};

export const previewHotkeys: HotkeySettingsModel = {
  revision: 2,
  controllerModifier: 4,
  controllerModifierLabel: "Guide",
  controller: { token: 1, displayName: "Preview Wireless Controller" },
  bindings: [
    ["toggleFullscreen", "F11"],
    ["toggleFullscreenAlternative", "Alt+Enter"],
    ["exitFullscreen", "Escape"],
    ["takeScreenshot", "F12"],
    ["toggleFastForward", "Tab"],
    ["endEmulation", "Ctrl+E"],
    ["exitApplication", "Ctrl+Q"],
  ].map(([action, label], index) => ({
    action: action as HotkeySettingsModel["bindings"][number]["action"],
    keyboardUsage: 58 + index,
    keyboardModifiers: index > 4 ? 1 : 0,
    controllerButton: index,
    controllerLabel: `${label} / Button ${index}`,
  })),
};

export const previewPacks: GraphicPack[] = [
  {
    key: "preview-quality",
    virtualPath: "Preview Adventure/Graphics",
    name: "Preview Adventure Graphics",
    description: "Resolution and shadow presets for layout verification.",
    version: 7,
    universal: false,
    enabled: true,
    activated: true,
    defaultEnabled: false,
    hasShaders: true,
    hasPatches: true,
    hasCustomVsync: false,
    supportedVersion: true,
    titleIds: ["0005000000000001"],
    presetOrder: ["Resolution", "Shadow quality"],
    presets: [
      {
        category: "Resolution",
        name: "1920×1080",
        active: true,
        visible: true,
      },
      {
        category: "Resolution",
        name: "3840×2160",
        active: false,
        visible: true,
      },
      { category: "Shadow quality", name: "High", active: true, visible: true },
    ],
  },
  {
    key: "preview-long-pack-key-for-overflow-verification",
    virtualPath:
      "Universal/Enhancements/Extremely Long Pack Path For Overflow Checks",
    name: "Universal Enhancements With a Deliberately Long Display Name",
    description:
      "A long description that must wrap without pushing controls outside the window.",
    version: 1,
    universal: true,
    enabled: false,
    activated: false,
    defaultEnabled: false,
    hasShaders: false,
    hasPatches: true,
    hasCustomVsync: true,
    supportedVersion: true,
    titleIds: [],
    presetOrder: [],
    presets: [],
  },
];

export const previewTitles: TitleManagerModel = {
  scanning: false,
  entries: [
    {
      locationUid: "1001",
      titleId: "0005000000000001",
      name: "Preview Adventure",
      version: 208,
      region: "USA",
      type: "base",
      format: "folder",
      path: "/games/Wii U Library/Preview Adventure/code/preview.rpx",
      canLaunch: true,
      canVerify: true,
      canConvert: true,
      canDelete: true,
    },
    {
      locationUid: "1002",
      titleId: "0005000e00000001",
      name: "Preview Adventure Update With Long Display Name",
      version: 208,
      region: "USA",
      type: "update",
      format: "wua",
      path: "/games/archive/preview-adventure-update-with-a-long-path.wua",
      canLaunch: false,
      canVerify: true,
      canConvert: false,
      canDelete: true,
    },
  ],
};

export const previewChecksum: ChecksumModel = {
  entries: previewTitles.entries.flatMap((entry) =>
    entry.type === "save"
      ? []
      : [
          {
            locationUid: entry.locationUid,
            titleId: entry.titleId,
            name: entry.name,
            version: entry.version,
            region: entry.region,
            type: entry.type,
            format: entry.format,
          },
        ],
  ),
};

export const previewSaves: SaveManagerModel = {
  scanning: false,
  accounts: [
    { persistentId: "80000001", name: "Preview Player" },
    { persistentId: "80000002", name: "Long Secondary Account Name" },
  ],
  titles: [
    {
      titleId: "0005000000000001",
      name: "Preview Adventure",
      saves: [
        {
          persistentId: "80000001",
          state: "directory",
          accountName: "Preview Player",
        },
        {
          persistentId: "80000002",
          state: "missing",
          accountName: "Long Secondary Account Name",
        },
      ],
    },
  ],
};

const permissionNames = [
  "Read game memory",
  "Write game memory",
  "Patch executable code",
  "Access title storage",
  "Access shared storage",
  "Network access",
  "Controller input",
  "Overlay output",
  "System clock",
  "Native host calls",
  "Background execution",
];

export const previewCemod: CemodManagerSnapshot = {
  generation: "42",
  selectedTitleId: "0005000000000001",
  cancelled: false,
  packages: [
    {
      packageKey: "preview-package-exact-key",
      titleIds: ["0005000000000001"],
      modId: "preview.cemod.layout",
      principal: "preview-author",
      modIdentity: "preview.cemod.layout@1.2.3",
      packageDigest:
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      pluginName: "Preview CemuMod With a Long Package Name",
      author: "Preview Author",
      version: "1.2.3",
      description:
        "Representative exact-package permission data for complete visual verification.",
      scope: "title",
      status: "approved",
      approvalReason: "Exact digest approved",
      warnings: ["Preview warning with wrapping content."],
      permissions: permissionNames.map((name, index) => ({
        name,
        bit: (1n << BigInt(index)).toString(),
        requested: index < 8,
        granted: index < 5,
        dangerous: index === 1 || index === 2 || index === 9,
        manifestMismatch: index === 7,
      })),
      requestedPermissions: "255",
      grantedPermissions: "31",
      approved: true,
      signedPackage: true,
      trustedNative: false,
      wups: true,
      headless: false,
      runtimeAvailable: true,
      valid: true,
      enabled: true,
    },
  ],
};

export const previewLogging: LoggingSnapshot = {
  entries: Array.from({ length: 18 }, (_, index) => ({
    sequence: String(index + 1),
    level: index % 9 === 8 ? "error" : index % 5 === 4 ? "warning" : "info",
    category: index % 2 ? "WebFrontend" : "CemuExtend.Runtime.Diagnostics",
    message:
      index === 3
        ? "A deliberately long log entry validates wrapping without horizontal page overflow: /games/preview/a/very/long/path/that/must/remain/readable/content.rpx"
        : `Preview diagnostic message ${index + 1}`,
  })),
  firstAvailableSequence: "1",
  nextSequence: "19",
  droppedEntries: "0",
  retainedBytes: "4096",
  truncated: false,
};

export const previewUsb: EmulatedUsbModel = {
  generation: "4",
  emulatedDevices: [
    {
      id: "skylanders",
      name: "Skylanders Portal",
      vendorId: 5168,
      productId: 336,
      enabled: true,
      connected: true,
    },
    {
      id: "infinity",
      name: "Disney Infinity Base",
      vendorId: 3695,
      productId: 305,
      enabled: false,
      connected: false,
    },
    {
      id: "dimensions",
      name: "LEGO Dimensions Toy Pad",
      vendorId: 3695,
      productId: 374,
      enabled: true,
      connected: true,
    },
  ],
  attachedDevices: [
    {
      id: "usb-preview-device-with-long-identity",
      vendorId: 5168,
      productId: 336,
      interfaceIndex: 0,
      interfaceSubClass: 0,
      protocol: 0,
      maxPacketSizeRx: 64,
      maxPacketSizeTx: 64,
      opened: true,
    },
  ],
};

export const previewThreads: PpcThreadsModel = {
  generation: "9",
  available: true,
  diagnostic: "",
  threads: Array.from({ length: 8 }, (_, index) => ({
    address: (0x10010000 + index * 0x100).toString(16),
    identity: `preview-thread-${index}`,
    entryPoint: "10000000",
    stackLow: "20000000",
    stackHigh: "20010000",
    instructionPointer: (0x10000000 + index * 4).toString(16),
    linkRegister: "10000100",
    state: index === 2 ? "waiting" : index === 3 ? "suspended" : "running",
    requestedAffinity: 7,
    effectiveAffinity: index % 2 ? 3 : 7,
    basePriority: 16,
    effectivePriority: 16 + index,
    wakeUpTime: "0",
    totalCycles: String(123456789 + index * 10000),
    name:
      index === 0
        ? "Main PPC thread with a deliberately long diagnostic name"
        : `Worker ${index}`,
    gpr: ["00000001", "00000002", "00000003", "00000004", "00000005"],
    cancelRequested: false,
    suspensionOwnedByFacade: index === 3,
    waitingMutex:
      index === 2
        ? { address: "20000040", owner: "10010100", lockCount: 1 }
        : undefined,
  })) as PpcThreadsModel["threads"],
};

export const previewTextures: TextureDiagnosticPage = {
  generation: "5",
  offset: 0,
  total: 3,
  truncated: false,
  available: true,
  diagnostic: "",
  rows: [
    {
      id: "texture-1",
      kind: "texture",
      active: true,
      updatedOnGpu: true,
      depthFormat: false,
      dimension: "2D",
      format: "RGBA8_UNORM",
      width: 1920,
      height: 1080,
      depth: 1,
      pitch: 2048,
      tileMode: 4,
      firstSlice: 0,
      sliceCount: 1,
      firstMip: 0,
      mipCount: 10,
      ageMilliseconds: 12,
      alternativeViewCount: 1,
      resolutionOverridden: true,
      effectiveWidth: 3840,
      effectiveHeight: 2160,
      effectiveDepth: 1,
    },
    {
      id: "view-1",
      parentId: "texture-1",
      kind: "view",
      active: true,
      updatedOnGpu: false,
      depthFormat: false,
      dimension: "2D array view with long descriptor",
      format: "RGBA8_SRGB",
      width: 1920,
      height: 1080,
      depth: 1,
      pitch: 2048,
      tileMode: 4,
      firstSlice: 0,
      sliceCount: 1,
      firstMip: 0,
      mipCount: 1,
      ageMilliseconds: 0,
      alternativeViewCount: 0,
      resolutionOverridden: false,
      effectiveWidth: 1920,
      effectiveHeight: 1080,
      effectiveDepth: 1,
    },
    {
      id: "texture-2",
      kind: "texture",
      active: false,
      updatedOnGpu: false,
      depthFormat: true,
      dimension: "2D",
      format: "D24_S8",
      width: 1280,
      height: 720,
      depth: 1,
      pitch: 1280,
      tileMode: 2,
      firstSlice: 0,
      sliceCount: 1,
      firstMip: 0,
      mipCount: 1,
      ageMilliseconds: 241,
      alternativeViewCount: 0,
      resolutionOverridden: false,
      effectiveWidth: 1280,
      effectiveHeight: 720,
      effectiveDepth: 1,
    },
  ],
};

export const previewAudio: AudioVoiceDiagnosticPage = {
  generation: "6",
  offset: 0,
  total: 12,
  available: true,
  diagnostic: "",
  rows: Array.from({ length: 12 }, (_, index) => ({
    id: `voice-${index}`,
    index,
    format: index % 2 ? "PCM16" : "ADPCM",
    currentOffset: 1024 + index * 128,
    loopOffset: 512,
    endOffset: 8192,
    looping: index % 3 === 0,
    volume: 32767 - index * 512,
    volumeDelta: index,
    sourceRatio: 0x10000 + index * 0x100,
    lowPassEnabled: index % 2 === 0,
    biquadEnabled: index % 4 === 0,
    deviceMix:
      index === 2 ? "TV L/R + GamePad L/R + long diagnostic route" : "TV L/R",
  })),
};

export const previewUpdates: UpdateManagerModel = { titleRunning: false };
