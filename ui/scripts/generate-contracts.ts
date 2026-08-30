import { mkdir } from "node:fs/promises";

type WireField = {
  kind: string;
  value?: boolean;
  requiredFields?: string[];
  optionalFields?: string[];
  undefinedNormalization?: string;
};

type ContractManifest = {
  schemaVersion: number;
  wireProtocol: {
    encoding: string;
    request: {
      requiredFields: string[];
      unknownFields: string;
      fields: Record<string, WireField>;
    };
    successResponse: {
      requiredFields: string[];
      unknownFields: string;
      fields: Record<string, WireField>;
    };
    errorResponse: {
      requiredFields: string[];
      unknownFields: string;
      fields: Record<string, WireField>;
    };
    eventEnvelope: {
      requiredFields: string[];
      unknownFields: string;
      fields: Record<string, WireField>;
    };
    limits: {
      maxRequestBytes: number;
      maxIdentifierBytes: number;
      rememberedRequestIds: number;
    };
  };
  typeBindings: {
    status: string;
    jsonSchemaMigration: string;
    references: Record<string, string>;
  };
  events: Array<{ name: string; payloadTypeRef: string }>;
  windowRoles: string[];
  implementedWindowRoles: string[];
  methods: Array<{ name: string; params: string; result: string }>;
};

const manifest = (await Bun.file(
  new URL("../contracts/rpc.json", import.meta.url),
).json()) as ContractManifest;

function requireUnique(values: string[], label: string): void {
  if (new Set(values).size !== values.length)
    throw new Error(`${label} must be unique`);
}

function requireFields(
  envelope: {
    requiredFields: string[];
    unknownFields: string;
    fields: Record<string, WireField>;
  },
  expected: string[],
  label: string,
): void {
  if (
    envelope.requiredFields.length !== expected.length ||
    expected.some(
      (field, index) =>
        envelope.requiredFields[index] !== field || !envelope.fields[field],
    )
  )
    throw new Error(`${label} envelope does not match the pinned wire shape`);
  if (envelope.unknownFields !== "ignored")
    throw new Error(`${label} envelope must ignore unknown fields`);
}

if (manifest.schemaVersion !== 1)
  throw new Error(`unsupported RPC schema version: ${manifest.schemaVersion}`);
if (manifest.wireProtocol?.encoding !== "json")
  throw new Error("RPC wire encoding must be JSON");
requireFields(
  manifest.wireProtocol.request,
  ["id", "method", "params"],
  "request",
);
requireFields(
  manifest.wireProtocol.successResponse,
  ["id", "ok", "result"],
  "success response",
);
requireFields(
  manifest.wireProtocol.errorResponse,
  ["id", "ok", "error"],
  "error response",
);
requireFields(
  manifest.wireProtocol.eventEnvelope,
  ["type", "sequence", "payload"],
  "event",
);
if (
  manifest.wireProtocol.request.fields.params.kind !== "object" ||
  manifest.wireProtocol.request.fields.params.undefinedNormalization !==
    "emptyObject"
)
  throw new Error("undefined RPC params must normalize to an empty object");
if (
  manifest.wireProtocol.successResponse.fields.ok.value !== true ||
  manifest.wireProtocol.errorResponse.fields.ok.value !== false
)
  throw new Error("RPC response discriminants must remain boolean literals");
const errorField = manifest.wireProtocol.errorResponse.fields.error;
if (
  errorField.kind !== "object" ||
  errorField.requiredFields?.join(",") !== "code,message" ||
  errorField.optionalFields?.join(",") !== "details"
)
  throw new Error(
    "RPC error must require code/message and permit optional details",
  );
for (const [name, value] of Object.entries(manifest.wireProtocol.limits)) {
  if (!Number.isSafeInteger(value) || value <= 0)
    throw new Error(`invalid RPC wire limit ${name}`);
}
if (manifest.typeBindings?.status !== "legacy-typescript-expressions")
  throw new Error(
    "payload bindings must identify the legacy TypeScript format",
  );
if (manifest.typeBindings.jsonSchemaMigration !== "deferred")
  throw new Error("JSON Schema migration status must be explicit");
const typeReferences = manifest.typeBindings.references;
if (!typeReferences || Object.keys(typeReferences).length === 0)
  throw new Error("RPC contract must define payload type references");
for (const [name, expression] of Object.entries(typeReferences)) {
  if (!/^[A-Z][A-Za-z0-9]*$/.test(name) || !expression.trim())
    throw new Error(`invalid payload type reference: ${name}`);
}
if (!Array.isArray(manifest.events) || manifest.events.length === 0)
  throw new Error("RPC contract must define events");
const eventNames = new Set<string>();
for (const event of manifest.events) {
  if (!/^[a-z][a-zA-Z]*(?:\.[a-z][a-zA-Z]*)+$/.test(event.name))
    throw new Error(`invalid RPC event: ${event.name}`);
  if (eventNames.has(event.name))
    throw new Error(`duplicate RPC event: ${event.name}`);
  if (!Object.hasOwn(typeReferences, event.payloadTypeRef))
    throw new Error(
      `unknown payload type reference for ${event.name}: ${event.payloadTypeRef}`,
    );
  eventNames.add(event.name);
}
if (!Array.isArray(manifest.methods) || manifest.methods.length === 0)
  throw new Error("RPC contract must define methods");
if (
  !Array.isArray(manifest.windowRoles) ||
  !manifest.windowRoles.includes("main-library")
)
  throw new Error("RPC contract must define main-library");
if (
  !Array.isArray(manifest.implementedWindowRoles) ||
  manifest.implementedWindowRoles.some(
    (role) => role === "main-library" || !manifest.windowRoles.includes(role),
  )
)
  throw new Error("implemented window roles must be known tool roles");
requireUnique(manifest.windowRoles, "window roles");
requireUnique(manifest.implementedWindowRoles, "implemented window roles");
const methodNames = new Set<string>();
for (const method of manifest.methods) {
  if (!/^[a-z][a-zA-Z]*(?:\.[a-z][a-zA-Z]*)+$/.test(method.name))
    throw new Error(`invalid RPC method: ${method.name}`);
  if (!method.params || !method.result)
    throw new Error(`RPC method types are required: ${method.name}`);
  if (methodNames.has(method.name))
    throw new Error(`duplicate RPC method: ${method.name}`);
  methodNames.add(method.name);
}

const methods = manifest.methods.map((method) => method.name);
const events = manifest.events.map((event) => event.name);
const generatedHeader =
  "// Generated by scripts/generate-contracts.ts. Do not edit.\n";
await mkdir(new URL("../src/generated/", import.meta.url), { recursive: true });
await Bun.write(
  new URL("../src/generated/roles.ts", import.meta.url),
  `${generatedHeader}export const windowRoles = ${JSON.stringify(manifest.windowRoles)} as const;\nexport const implementedWindowRoles = ${JSON.stringify(manifest.implementedWindowRoles)} as const;\n`,
);
await Bun.write(
  new URL("../src/generated/methods.ts", import.meta.url),
  `${generatedHeader}export const rpcMethods = ${JSON.stringify(methods)} as const;\nexport type RpcMethod = typeof rpcMethods[number];\n`,
);
await Bun.write(
  new URL("../src/generated/protocol.ts", import.meta.url),
  `${generatedHeader}export const rpcSchemaVersion = ${manifest.schemaVersion} as const;\nexport const rpcWireLimits = ${JSON.stringify(manifest.wireProtocol.limits)} as const;\n`,
);
await Bun.write(
  new URL("../src/generated/events.ts", import.meta.url),
  `${generatedHeader}import type { CemodManagerSnapshot, ImplementedToolWindowRole, LoggingSnapshot, RuntimeOverlaySnapshot, TitleLaunchState, UsbDevicesChangedPayload } from "../bridge/contracts";\nimport type { EmptyResult } from "./contracts";\nexport const rpcEvents = ${JSON.stringify(events)} as const;\nexport type RpcEventName = typeof rpcEvents[number];\nexport type RpcEventContract = {\n${manifest.events.map((event) => `  ${JSON.stringify(event.name)}: { payload: ${typeReferences[event.payloadTypeRef]} };`).join("\n")}\n};\nexport type RpcEvent = { [Name in RpcEventName]: { type: Name; sequence: string; payload: RpcEventContract[Name]["payload"] } }[RpcEventName];\n`,
);
await Bun.write(
  new URL("../src/generated/contracts.ts", import.meta.url),
  `${generatedHeader}import type { AboutInfo, Account, AccountManagerModel, AccountNetworkService, AccountUpdate, AudioVoiceDiagnosticPage, Bootstrap, CapturedInputButton, CemodApprovalUpdate, CemodManagerResult, ChecksumModel, DiagnosticPageRequest, EmulatedControllerType, EmulatedUsbDeviceId, EmulatedUsbModel, FrontendSettings, FrontendSettingsApplyResult, FrontendSettingsUpdate, GraphicPack, GraphicPackInstallRequest, GraphicPackMutation, GuestAddress, HotkeySettingsApplyResult, HotkeySettingsModel, HotkeySettingsUpdate, ImplementedToolWindowRole, InputDeviceCandidate, InputSettingsModel, LoggingSnapshot, ManagedContentDeletePlanView, MemorySearchPage, MemorySearchSession, MemorySearchStatus, MemoryTypedValue, NativeDestination, NativeSelection, PhysicalControllerSettings, PpcDebuggerControl, PpcDebuggerSnapshot, PpcThreadCommand, PpcThreadCommandResult, PpcThreadsModel, RuntimeOverlaySnapshot, SaveEntryState, SaveIdentity, SaveImportInspection, SaveManagerModel, TextureDiagnosticPage, Title, TitleInstallPlanView, TitleInstallSelection, TitleLaunchResult, TitleManagerModel, UpdateManagerModel, WuaConversionPlanView } from "../bridge/contracts";\nexport type EmptyResult = Record<string, never>;\nexport type RpcContract = {\n${manifest.methods.map((method) => `  ${JSON.stringify(method.name)}: { params: ${method.params}; result: ${method.result} };`).join("\n")}\n};\nexport type RpcMethod = keyof RpcContract;\n`,
);

const rustStrings = (values: string[]) =>
  values.map((value) => `    ${JSON.stringify(value)},`).join("\n");
const rustIntegerLiteral = (value: number) => {
  if (!Number.isSafeInteger(value) || value < 0)
    throw new Error(`invalid Rust integer literal: ${value}`);
  return value.toString().replace(/\B(?=(\d{3})+(?!\d))/g, "_");
};
const rustEvents = manifest.events
  .map(
    (event) =>
      `    EventContract { name: ${JSON.stringify(event.name)}, payload_type_ref: ${JSON.stringify(event.payloadTypeRef)} },`,
  )
  .join("\n");
await mkdir(new URL("../../crates/cex-contracts/src/", import.meta.url), {
  recursive: true,
});
await Bun.write(
  new URL("../../crates/cex-contracts/src/generated.rs", import.meta.url),
  `// Generated by ui/scripts/generate-contracts.ts. Do not edit.\n\n/// Version of the shared RPC contract schema.\npub const SCHEMA_VERSION: u32 = ${rustIntegerLiteral(manifest.schemaVersion)};\n/// Maximum encoded request size accepted by the native transport.\npub const MAX_REQUEST_BYTES: usize = ${rustIntegerLiteral(manifest.wireProtocol.limits.maxRequestBytes)};\n/// Maximum UTF-8 byte length of a transport identifier.\npub const MAX_IDENTIFIER_BYTES: usize = ${rustIntegerLiteral(manifest.wireProtocol.limits.maxIdentifierBytes)};\n/// Number of request identifiers retained for duplicate detection.\npub const REMEMBERED_REQUEST_IDS: usize = ${rustIntegerLiteral(manifest.wireProtocol.limits.rememberedRequestIds)};\n/// All window roles declared by the shared contract.\npub const WINDOW_ROLES: [&str; ${rustIntegerLiteral(manifest.windowRoles.length)}] = [\n${rustStrings(manifest.windowRoles)}\n];\n/// Window roles implemented as detached tool windows.\npub const IMPLEMENTED_WINDOW_ROLES: [&str; ${rustIntegerLiteral(manifest.implementedWindowRoles.length)}] = [\n${rustStrings(manifest.implementedWindowRoles)}\n];\n/// RPC methods accepted by the native bridge.\npub const RPC_METHODS: [&str; ${rustIntegerLiteral(methods.length)}] = [\n${rustStrings(methods)}\n];\n/// Native-to-frontend events and their manifest payload references.\npub const RPC_EVENTS: [EventContract; ${rustIntegerLiteral(manifest.events.length)}] = [\n${rustEvents}\n];\n`,
);
