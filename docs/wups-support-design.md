# WUPS/WUMS support design and code contract

Status: Task 1/7 package, inspection, and runtime-abstraction baseline. This
document is the contract for the remaining runtime tasks; it is not a claim
that the runtime described below is already present. Sections explicitly
marked **implemented in Task 1** are available now. A WUPS payload that reaches
`TrustedCemodRuntime` is deliberately rejected until `WupsPayloadRuntime`
exists; no fake lifecycle, import, patch, or module API reports success.

## 1. Source baseline and license boundary

The design was checked against these upstream revisions on 2026-07-23:

| Project | Revision | Use in this design | License boundary |
| --- | --- | --- | --- |
| Cemu | [`50b9e4ba`](https://github.com/cemu-project/Cemu/commit/50b9e4ba1d4d7cf9821a9cd416378bb94e1ba0ca) | RPL loader, PPC execution, memory, TLS, and JIT APIs | Existing Cemu/CemuExtend code |
| WiiUPluginSystem | [`0e26d9fd`](https://github.com/wiiu-env/WiiUPluginSystem/commit/0e26d9fd2d9db6c356c973a1a79000757802d883) | Public WUPS 0.9.1 headers and binary ABI | Public headers retain their own notices; no source copied into CemuExtend |
| WiiUPluginLoaderBackend | [`e3e67f8a`](https://github.com/wiiu-env/WiiUPluginLoaderBackend/commit/e3e67f8a8ebee77b351a43ff79561ecb6955dea6) | Observable lifecycle, version compatibility, and error behavior | GPL implementation is research-only and must not be copied into MPL files |
| WiiUModuleSystem | [`2f0b0303`](https://github.com/wiiu-env/WiiUModuleSystem/commit/2f0b0303509bbf22bf06747778b5640d6f1c1830) | Public WUMS 0.3.6 ABI and descriptors | Public ABI/binary format only |
| WUMSLoader | [`bc7113f1`](https://github.com/wiiu-env/WUMSLoader/commit/bc7113f1e5a290dc1335098adc74d934d2c171ff) | Observable module order and allocator behavior | GPL implementation is research-only |
| Aroma | [`3006f47f`](https://github.com/wiiu-env/Aroma/commit/3006f47f0f2288261ba4d029dd7df621e8637a8b) | Current environment composition and published memory map | Aggregator information only |
| FunctionPatcherModule | [`1a294db7`](https://github.com/wiiu-env/FunctionPatcherModule/commit/1a294db7eab9c87d561fbbd3ab08a71a565b1ef2) | API v2, descriptor v2/v3, patch handles, late RPL behavior | GPL implementation is research-only |
| MemoryMappingModule | [`b849fecf`](https://github.com/wiiu-env/MemoryMappingModule/commit/b849fecf421f9f7ce3056d8d08581ef0bb0a4d91) | Function/data exports, GX2 allocator, module dependencies | GPL implementation is research-only |
| NotificationModule | [`ef2b5fd9`](https://github.com/wiiu-env/NotificationModule/commit/ef2b5fd94b278ff5014b7f837b154bbba298a7bc) | API v2 static/dynamic notification status and callbacks | GPL implementation is research-only |
| LoggingModule | [`0f81f5e7`](https://github.com/wiiu-env/LoggingModule/commit/0f81f5e75401ce6aa4888f1c6de4938324e5d909) | `homebrew_logging` module and `WUMSLogWrite` behavior | Repository has no root license file; no code is incorporated |

The same rule applies to FunctionPatcherModule, MemoryMappingModule,
NotificationModule, LoggingModule, Aroma, and other wiiu-env repositories:
public headers, numeric ABI values, binary formats, and externally observable
behavior are specifications. CemuExtend code is an independent MPL-2.0
implementation using existing Cemu services. If a future change imports GPL
code, it must be a separately built GPL component with source/license delivery
and an explicit interface; it must not be pasted into CemuExtend.

The inspected CemuExtend baseline routes packages through
`CemodPackage::{Inspect,Load}`, catalogs them in `cemuextend.cpp`, and selects
`CemodRuntime`/`TrustedCemodRuntime`. Title start is connected from
`CafeSystem.cpp` after normal RPL linking, while title shutdown unloads CEMOD
instances before the application RPLs. `RPLLoader.cpp` already supplies
in-memory loading, linking, export lookup, entrypoint lookup, and trampoline
allocation. Guest calls use `PPCCoreCallback`; guest TLS, r2/r13 setup, and JIT
cache invalidation already have Cemu-owned implementations. Those facilities
must be reused rather than replaced by a second linker or PPC executor.

## 2. Architecture

The target ownership graph is:

```text
CemodPackage (immutable verified bytes + manifest + inspection)
  |
TrustedCemodRuntime (title/package orchestration only)
  +-- CemodElfPayloadRuntime : ITrustedPayloadInstance
  +-- WupsPayloadRuntime     : ITrustedPayloadInstance
        |
        +-- AromaCompatibilityRuntime
              +-- WupsPluginRuntime / lifecycle dispatcher
              +-- WumsModuleRuntime / ModuleExportRegistry
              +-- WupsFunctionPatchManager
              +-- WupsBackendRuntime (Storage, Config, Button Combo, Reent)
              +-- StandardAromaModuleRuntime
```

`execution_mode` selects CEX2 isolation versus trusted guest execution.
`payload.format` selects the binary and ABI loader. WUPS is therefore never an
execution mode. `TrustedCemodRuntime` may choose a factory based on
`CemodPayloadFormat`, but must not contain WUPS parsing, hooks, or module APIs.

**Implemented in Task 1:** `ITrustedPayloadInstance`,
`CemodPayloadDescriptor`, and the format-neutral `CemodPackage::PayloadBytes()`
adapter establish this boundary without changing legacy callers that populate
`CemodPackage::elf` directly.

The common instance contract is:

```cpp
class ITrustedPayloadInstance {
public:
    virtual ~ITrustedPayloadInstance() = default;
    virtual CemodPayloadFormat Format() const = 0;
    virtual uint64_t OwnerHandle() const = 0;
    virtual uint32_t Generation() const = 0;
    virtual bool OnApplicationStarts(std::string& error) = 0;
    virtual void OnReleaseForeground() = 0;
    virtual void OnAcquiredForeground() = 0;
    virtual void OnApplicationRequestsExit() = 0;
    virtual void OnApplicationEnds() = 0;
    virtual void Unload() = 0;
};
```

`Unload()` is idempotent. It first makes the generation unreachable and
revokes callbacks, then removes patches and handles, and only then releases
guest memory. Destructors call it as a last resort but normal shutdown reports
individual cleanup errors.

## 3. Package format

### 3.1 Manifest versions

Package version 1 is frozen. It must omit `payload`, `scope`, and
`permissions`; the implicit descriptor is `cemod_elf/mod.elf`. Existing
signature bytes are unchanged.

Package version 2 requires an exact two-member `payload` object:

```json
{"format":"cemod_elf","path":"mod.elf"}
```

or:

```json
{"format":"wups","path":"plugin.wps"}
```

Unknown formats and path/format mismatches fail before runtime construction.
`wups` is allowed only with `execution_mode: trusted_native`. Version 2 may
also contain:

```json
{
  "scope": {"type":"process","targets":["game","wii_u_menu"]},
  "permissions": {
    "native_memory": true,
    "function_patching": true,
    "physical_address_patching": false,
    "filesystem": {"read":true,"write":false},
    "network": false,
    "mapped_memory": true,
    "notifications": true,
    "content_redirection": false,
    "modules": ["homebrew_functionpatcher"]
  }
}
```

`scope.type` is `process` with a nonempty unique target list, or
`aroma_native` with no targets. A missing scope means the existing title
scope. Valid process names are `all`, `root_rpx`, `wii_u_menu`, `tvii`,
`e_manual`, `home_menu`, `error_display`, `mini_miiverse`, `browser`,
`miiverse`, `eshop`, `download_manager`, `game`, and `game_and_menu`. Scope
never causes Cemu to invent a process it is not emulating.

Native permissions are separate from the existing CEX2
`requested_permissions`. Importing a module does not grant permission. The
package catalog stores the complete permission set so any newly requested bit
or module invalidates an earlier approval.

### 3.2 ZIP contract

A package contains exactly one of:

```text
manifest.json + mod.elf
manifest.json + plugin.wps
```

and optionally both `public_key.ed25519` (32 raw bytes) and
`signature.ed25519` (64 raw bytes). No other entry is currently understood;
an unknown entry is treated as unknown mandatory data and rejected.

Validation is independent of central-directory order and commits no catalog or
runtime state until all checks pass. Limits are 256 entries, a 64 MiB package,
64 MiB total expansion, 64 MiB payload, and a maximum 200:1 ratio for entries
over 4096 bytes. Only stored and deflated, unencrypted entries are accepted.
The decompressor must produce exactly the ZIP-declared length and EOF.

Names are UTF-8 ZIP names and are rejected for absolute Unix/Windows/UNC
forms, backslashes, drive prefixes, NUL/control bytes, trailing directory
entries, or any `..` component. Duplicate raw names and duplicates after
lower-casing and removal of empty/`.` components are both rejected. This
prevents `./plugin.wps`, case variants, and equivalent paths from bypassing
the allow-list.

### 3.3 Signature digest

The canonical algorithm remains the version 1 algorithm. Iterate every entry
except `signature.ed25519` in unsigned UTF-8 bytewise name order and append:

```text
BE32(name byte length)
name bytes
BE64(uncompressed data length)
SHA-256(uncompressed data)
```

The signed message is `SHA-256(concatenation)`. Thus the selected payload name,
length, and SHA-256 are explicitly authenticated, as are the canonical
manifest and raw public key. Ed25519 signs this 32-byte message. The principal
continues to be `ed25519:SHA256(raw-public-key):mod_id`; an unsigned principal
continues to hash the complete package file.

**Implemented in Task 1:** all manifest, ZIP, digest, payload-count, signature,
order, limit, and atomic-failure rules above in CemuExtend and cemod-sdk.

## 4. WPS/RPL host-side inspection contract

Inspection is pure host-side parsing over immutable bytes. It allocates no
guest memory and does not use `RPLLoader`. Every guest address remains a
`uint32_t` until runtime mapping. Metadata and names are copied into host-owned
strings. Inspection yields sections, metadata, hooks, replacements, imports,
exports, relocations, required modules, process targets, TLS usage,
fixed-address use, and compatibility warnings.

The accepted outer image is ELF32, big-endian, PowerPC machine 20, RPL type
`0xfe01`, ELF version 1, Wii U OS ABI bytes `ca fe`, and WPS marker bytes `PL`.
There is no program-header table. Section entry size is 40 and section count is
5..512. The null section, section-name string table, final RPL CRC section
(`0x80000003`), and final FILEINFO section (`0x80000004`, magic
`0xcafe0402`) are mandatory.

For every section the parser checks table and file bounds with widened
arithmetic, power-of-two alignment up to 64 KiB, aligned allocated addresses,
no W+X, no header/table/file overlap, no allocated-address overlap, no 32-bit
wrap, and correct text/data/loader-memory placement. `SHT_NOBITS` contributes
expanded size without file data. `SHF_RPL_COMPRESSED` data begins with a BE32
expanded length followed by one complete zlib stream; trailing, short, excess,
or aggregate-over-limit output is invalid. CRC32 is over expanded bytes and is
checked for every non-NOBITS/non-CRC section.

Symbol tables have 16-byte entries and a bounded terminated linked string
table. Import sections are `SHT_RPL_IMPORTS` named `.fimport_<module>` or
`.dimport_<module>`, reside in loader memory, and their symbol type must be
function or object respectively. Import value, module name, symbol name, and
duplicates are validated. Export tables (`SHT_RPL_EXPORTS`) contain a bounded
count followed by address/name-offset pairs; execute flags distinguish
function from data exports.

REL/RELA entries must target a valid section, reference a valid symbol, fit the
target range at the relocation width, and use one of the current Cemu RPL/WUPS
types: 0, 1, 4, 5, 6, 10, 11, 68, 78, 251, 252, or 253. No unknown type is
silently deferred to runtime. TLS flags are recorded and range-checked.

`.wups.meta` is a NUL-delimited sequence of `key=value` records. The complete
section and each value must terminate. Duplicate keys are invalid. Required
keys are `name` and `wups`; `author`, `version`, `license`, `description`,
`buildtimestamp`, `storage_id`, and `debug` are copied. `debug` is absent,
`track_heap`, or `track_heap_with_stack_trace`. Unknown valid keys are retained.

`.wups.hooks` is mandatory and contains at most 64 aligned 8-byte
`{BE32 type, BE32 target}` records. Types 0..26 follow the current public
`wups_loader_hook_type_t` exactly: malloc pair, newlib pair, stdcpp pair,
devoptab pair, sockets pair, wrapper pair, deprecated config open/close,
deprecated storage, plugin init/deinit, five application/foreground hooks,
storage, config, button combo, WUT thread, and reent initialization. Duplicate
types, unknown types, or non-executable/alignment-invalid targets fail.

`.wups.load`, when present, consists only of 36-byte replacement records:
type (optional function, mandatory function, or legacy export), physical
address, virtual address, function-name pointer, library,
replacement-name pointer, replacement function, call-through storage pointer,
and process target. Counts, enum values, guest strings, executable/writable
ranges, alignment, library 0..66, and public process values are strict. A
legacy export is retained with an explicit runtime-compatibility warning.
Library
66 means fixed-address and must agree with physical/virtual fields. This task
only inspects descriptors; it does not apply them.

Accepted WUPS ABI strings are 0.7.1, 0.8.1, 0.8.2, 0.9.0, and 0.9.1, matching
the current backend compatibility surface. Other versions fail with plugin
name, detected version, and supported versions. Older accepted versions carry
a warning. Runtime must implement the actual per-version storage, button-combo,
hook argument, and reent behavior before allowing execution.

**Implemented in Task 1:** `WupsBinaryInspector` and the equivalent SDK parser
implement the independent inspection contract. Both parse a current public
0.9.1 plugin without executing it.

## 5. RPL loader integration

The future runtime extends the existing Cemu loader, not the host inspector:

```cpp
struct RPLLoadOptions {
    bool callEntrypoint{};
    bool registerDependency{};
    bool useApplicationAllocator{};
    bool allowWupsMarker{};
    bool allowWumsMarker{};
    RplOwnerHandle owner{};
};

RPLExternalModuleHandle RPLLoader_LoadExternalModuleFromMemory(
    std::span<const std::byte> image, std::string_view name,
    const RPLLoadOptions&, std::string& error);
bool RPLLoader_UnloadExternalModule(RPLExternalModuleHandle,
    std::string& error);
```

The handle is generation-checked and owns module registration, text/data/BSS,
TLS, trampoline blocks, dependencies, and link records. Loading has a prepare
transaction and a publish step. Failure rolls back allocations in reverse
order. External modules are not put into application dependency lists unless
requested. WPS/WUMS markers are rejected by existing title paths unless the
matching option is explicit. Normal RPL entrypoints are not WUPS/WUMS hooks and
default to not being called. Unload first broadcasts a pre-unload event,
rejects new references, removes dependants/exports, unlinks relocations, and
then frees owned memory. Existing title APIs and behavior remain unchanged.

Dynamic load events are emitted after export publication and before a caller
can execute the returned address. Pre-unload events occur while the module is
still readable/executable. FunctionPatcher consumes these events to apply and
remove module-scoped patches transactionally.

## 6. Guest calls, TLS, and reent

All plugin/module code is invoked through a Cemu PPC callback service built on
`PPCCoreCallback`; a guest address is never cast to a host function pointer.
The service accepts typed 32/64-bit integer, float, guest pointer, and bounded
by-value struct arguments and returns captured PPC ABI registers. It prepares
an aligned guest stack, LR/CTR, r2/r13, volatile registers, and the owning
module's TOC/SDA state, and works identically for Interpreter and JIT. It maps
guest exceptions, invalid instructions, and invalid return state to a
per-owner error without `OSFatal`.

Every callback token contains owner, generation, module, executable range,
expected signature, current title/process, and permitted lifecycle states.
Dispatch pins the owner under the registry lock, releases all runtime locks,
calls guest code, then reacquires and revalidates generation. Unload marks the
owner `Unloading`, closes queues, waits for pins without holding the registry
lock, and rejects later tokens.

TLS uses Cemu's RPL TLS templates and per-emulated-thread allocations. The
external-module handle owns its template/index; thread creation/destruction and
module unload add/remove instances. r2/r13 come from the linked module and
thread context, never a host address.

Reent has `(owner, generation, guest-thread)` identity. ABI 0.9.1 receives the
current get/register context callbacks. 0.8.x/0.9.0 use the documented legacy
path, including the current backend's compatibility for the pre-0.9.1 reent
issue; 0.7.1 uses its older storage/reent layout. Thread exit and plugin unload
destroy contexts only after callbacks are revoked.

## 7. WUPS lifecycle and state machine

Each plugin has one of:

```text
Parsed -> Loaded -> Linked -> RuntimeInitialized -> PluginInitialized
       -> ApplicationStarted -> ForegroundReleased -> ExitRequested
       -> ApplicationEnded -> Unloading -> Unloaded
any nonterminal state -> Failed -> Unloading -> Unloaded
```

Repeated foreground acquire/release is represented by a foreground flag and a
monotonic event serial; it does not allow arbitrary state reversal. Illegal
transitions are logged and rejected. Missing hooks are successful no-ops;
invalid addresses or guest failures are plugin failures. A failure does not
abort the title or another plugin.

For a newly linked plugin the observable initialization order is:

1. `INIT_REENT_FUNCTIONS`
2. `INIT_WUT_MALLOC`
3. `INIT_WUT_NEWLIB`
4. `INIT_WUT_STDCPP`
5. `INIT_WUT_THREAD`
6. `INIT_WUT_DEVOPTAB`
7. `INIT_WUT_SOCKETS`
8. `INIT_WRAPPER`
9. open storage and create button-combo owner context
10. `INIT_BUTTON_COMBO`, `INIT_CONFIG`, deprecated storage, current storage,
    `INIT_PLUGIN`
11. apply committed function patches
12. `APPLICATION_STARTS`

Devoptab and sockets may need per-application calls for already linked plugins,
as in the current backend. Application exit dispatches requests-exit and ends
once; application end then finishes sockets and devoptab for that application.
Plugin removal invokes deinit plugin, fini wrapper, devoptab/sockets cleanup,
revokes APIs/callbacks, restores patches, closes storage/button handles, clears
reent/TLS, and releases memory. Initializer failure performs the exact reverse
of completed transactional steps. Later implementation must verify this order
against the pinned upstream revision with conformance trace tests.

## 8. Import resolution and module exports

Imports preserve function/data kind. A mandatory unresolved or wrong-kind
symbol fails link. Optionality exists only where the public WUPS/WUMS ABI marks
it; CemuExtend must not invent optional imports. Diagnostic fields are package
ID, plugin/module name, importing module, symbol, function/data, mandatory
flag, ABI version, and resolver attempted.

Resolution order is deterministic:

1. the existing Cemu RPL dependency/export resolver and loaded Cafe OS RPLs;
2. existing Cemu HLE exports for that same Cafe module;
3. WUPS backend exports owned by `WupsBackendRuntime`;
4. WUMS exports in `ModuleExportRegistry`;
5. standard Aroma-module HLE exports;
6. explicitly loaded custom `.wms` exports.

Steps 3..6 must not shadow a valid normal Cafe export unless the public ABI
module name intentionally addresses that registry. Within WUMS/custom modules,
the first module in topological order wins only if duplicate policy explicitly
allows aliases; otherwise a same-module/same-name/same-kind duplicate is a load
error. A function and data export never satisfy one another.

`ModuleExportRegistry` publishes one immutable generation at a time. A plugin
pins module generations used by relocations. Module unload fails or first
unloads dependants according to declared dependency semantics; it never leaves
a relocated dangling address.

## 9. WUMS runtime

`.wms` uses the external RPL API with the WUMS marker option. A pure parser
validates `.wums.meta`, `.wums.hooks`, and `.wums.exports` before allocation.
The model stores identifier/version, function/data exports, mandatory/optional
dependencies, and hook addresses as copied host values.

Build a dependency graph before publication. Reject duplicate module IDs,
duplicate exports, missing mandatory dependencies, and cycles. Topological
order uses identifier as a stable tie-breaker; unload is reverse order.
Optional absent dependencies do not create edges. Reload constructs a complete
new graph and swaps only after link/lifecycle success.

Module initialization covers WUT malloc/newlib/stdcpp/devoptab/sockets/thread,
reent, wrapper, INIT, relocations-done, and all-application-starts-done in the
observable WUMSLoader order. Application requests/end hooks run plugin and
module layers in the order required by the public backend/WUMS relationship;
the `*_DONE` hooks run after every participant's corresponding ordinary hook.
Custom allocator hooks are called only through the guest callback service and
their ranges are registered to the owner. Clear-allocated-memory runs even if
later unload work fails.

## 10. FunctionPatcher contract

`WupsFunctionPatchManager` has one transaction per plugin generation. It first
resolves every applicable mandatory descriptor and validates permissions,
process target, function/data kind, 4-byte alignment, executable target,
writable patch mechanism, replacement ownership, and conflicts. Optional
unresolved descriptors are recorded but do not fail. Process matching is one
central function; an inactive Cemu process is a normal non-match, not emulated.

Named replacements resolve library plus export. Fixed replacements distinguish
physical and virtual addresses and require
`physical_address_patching`. Function patching never accepts a data export.
`real_*` storage receives an owner-scoped call-through trampoline to an
unmodified prologue. REL24 is used only in range. Out-of-range calls use a
far-call trampoline allocated through Cemu's RPL trampoline service. Relocated
instructions and branch-back sequences are validated before commit.

Patch ownership is an interval registry keyed by current process/address. A
conflict is deterministic and fails the later transaction; partial writes are
rolled back. Commit records original instructions, target module generation,
trampoline allocation, call-through storage old value, and JIT invalidation
range. Every write invalidates Cemu interpreter/JIT caches. Unload or dynamic
RPL pre-unload restores original instructions before freeing trampolines.
Dynamic module post-load re-resolves eligible descriptors in stable package ID
then descriptor order. Title end performs reverse committed order and verifies
restoration.

## 11. Backend and standard Aroma modules

All guest-visible handles encode owner and generation. Every API rechecks the
package permission at call time and returns the public error code. Unsupported
hardware behavior returns an explicit unsupported result and structured log;
it never reports dummy success.

- Storage: per-storage-ID namespace, typed/nested items, item/string/total-size
  limits, mutex-protected owner handles, corruption detection, temporary file
  plus fsync/rename atomic save, and legacy serialization migration.
- Config: owner-scoped categories/items for bool, integer range, multiple
  values, and button combos. The model exists without an open GUI. Guest
  callbacks are pinned and failure closes the interaction safely.
- Button Combo: controller mask, down/release/hold/duration, deterministic
  conflict detection, input-to-CPU callback queue, and owner cleanup.
- Reent: the thread/TLS model in section 6.
- FunctionPatcherModule: delegates to the transactional manager.
- MemoryMappingModule: Cemu guest allocations with owner, address range,
  alignment, size, CPU/GX2 intent, permissions, overlap, double-free, and JIT/GX2
  synchronization. Real hardware physical pointers are never host pointers.
- NotificationModule: GUI-thread queue, dynamic update/finish, callbacks,
  duration/keep-until-shown, owner cleanup, and log fallback headlessly.
- LoggingModule: Cemu log integration with owner/module/source context, levels,
  rate limiting, and safe UDP fallback.
- ContentRedirectionModule: normalized virtual/source paths, traversal/symlink
  containment, owner/priority/title scope, recursion prevention, read/write
  permission, and Cemu VFS integration.
- KernelModule: only operations safely representable by Cemu; exploit, IOSU,
  hardware-register, and real cache-control operations return unsupported.
- USBSerialLoggingModule: map to Cemu logging or return unsupported when the API
  specifically requires USB transport.
- SDUtils/filesystem helpers: map to configured Cemu VFS roots with traversal
  and permission checks; do not expose arbitrary host files.

Backend imports are not themselves permission grants. Required permission
inference is an inspection warning; load approval remains manifest-driven.

## 12. Resource ownership and rollback

`PayloadOwner` is `(package principal, mod_id, title ID, owner handle,
generation)`. A per-plugin ledger holds, in creation order: RPL handle,
text/data/BSS/TLS, trampolines, WUT heap, reent contexts, guest threads,
function patches, module pins, storage/config/combo handles, notifications,
mapped memory, VFS mounts, sockets, callbacks, queued callbacks, and dynamic RPL
references. WUMS modules use the same ledger type with module identity.

Creation returns RAII reservations that publish to the ledger only on commit.
Failure destroys reservations in reverse order. Unload closes public access,
drains callbacks, and then walks the ledger in reverse even if individual
destructors report failure. Cleanup is idempotent and each resource validates
owner/generation; double-free and owner mismatch are errors, never frees.

## 13. Threading and lock order

Required lock order, outer to inner, is:

```text
title lifecycle -> owner registry -> module registry -> patch registry
-> backend service registry -> individual resource/storage lock
```

The Cemu RPL loader lock is acquired only through a loader operation and never
while calling guest code. GUI, input, filesystem, notification, and socket
threads enqueue immutable owner/generation events to the emulated CPU thread.
No global/runtime lock is held across a guest callback. A callback obtains a
pin, snapshots data, releases locks, invokes PPC, then revalidates owner and
generation. Title end closes all queues before waiting for pins. Storage disk
I/O snapshots under its lock and performs file operations after releasing it.

## 14. Error handling and crash isolation

Parsers return `optional` plus a concrete error and never mutate global state.
Runtime errors use a structured record containing package, plugin/module,
state, ABI, import/patch/API context, guest PC/LR, and recoverability. User
strings are bounded before logging. One plugin transitions to Failed and is
rolled back without `OSFatal`; dependent modules/plugins are the only other
objects affected. Guest exception handling stops the callback and invalidates
that owner. Cemu process/title fatality is reserved for corruption proven to be
outside the plugin boundary, not for malformed or failing plugin code.

## 15. Permissions, CEX2, and UI

Package verification and publisher/principal/future-update trust remain CEX2
container services. The WUPS owner maps one-to-one to the package CEX2 owner,
but WUPS does not require CEX2 service imports. Title shutdown uses the same
owner to close CEX2 sessions and the WUPS ledger. Namespace keys include the
principal and plugin storage ID.

Preflight rejects statically evident permission mismatches: replacements need
function patching; physical replacements need physical-address permission;
mapped-memory/module imports need both the module declaration and operation
permission where applicable. Every API and fixed-address operation rechecks at
runtime. Denials return the public ABI error, log the owner and reason, and do
not affect other plugins.

The package UI model will expose payload format, WUPS ABI and metadata,
required modules, process targets, patch/fixed-address/TLS use, inferred versus
declared permissions, warnings, unresolved imports, load error, lifecycle
state, and configuration items. Existing enable/disable, signature, approval,
and future-update trust controls are unchanged. GUI actions enqueue lifecycle
commands rather than calling guest code on the GUI thread.

## 16. Security requirements

Every guest pointer API checks 32-bit addition overflow, range, access
permission, alignment, NUL termination, struct version/size, owner, generation,
lifetime, title/process, and callback state. Host pointers and guest addresses
have distinct types. Counts, offsets, indexes, and allocation multiplication
use checked widened arithmetic. Package and WPS parsing occurs before trust
approval can lead to allocation/execution. No raw `reinterpret_cast` turns a
guest address into a host callable pointer.

File APIs normalize paths component-wise, reject traversal and symlink escape,
and write atomically. Resource/count/size/rate limits exist per owner and
globally. Logs avoid dumping secrets or arbitrary guest buffers. JIT and
Interpreter share the same patch/cache and callback validation path.

## 17. Testing contract

Task 1 unit tests cover manifest v1/v2, both payload descriptors, missing and
multiple payloads, order independence, duplicate/normalized/unsafe/unknown ZIP
entries, bomb ratio, bad signing material, signature name/length/hash binding,
WPS header/section/compression/CRC/string/symbol/relocation/meta/hook/load/import
errors, supported/unknown ABI, TLS, and a real current WPS image. LibFuzzer
targets consume complete package files and raw WPS images.

Following tasks add deterministic fake *guest callbacks and Cemu services* for
failure injection, not fake successful runtime implementations. Tests must
cover linker rollback, wrong-kind/unresolved import, WUMS graph conflicts and
cycles, patch conflicts/restoration/dynamic RPL, lifecycle rollback, stale
callbacks, storage corruption, owner mismatch/double-free, title cleanup, and
Interpreter/JIT. Conformance plugins/modules must trace every hook and service,
and integration tests must compare the trace to this ordering across title
restart and multiple independent failures. Leak checks require every owner
ledger and callback queue to be empty after unload.

## 18. Unsupported hardware behavior

Cemu will not create Wii U processes merely to satisfy a target. Kernel
exploits, IOSU commands without a safe Cemu service, raw hardware registers,
real physical-memory mappings, real SD mounts, USB serial transport, and
kernel-only cache operations are unsupported unless explicitly mapped to an
equivalent Cemu subsystem. Unsupported is a real error/status with module,
export, owner, call site, and reason. It is not a success stub.

## 19. Backward compatibility

- Version 1 manifest interpretation and canonical signature digest are frozen.
- `mod.elf` remains the default SDK output and existing CMB1 validation/runtime
  stays intact.
- `CemodPackage::elf` remains a source adapter and is mirrored for loaded ELF
  packages; format-neutral code uses the canonical `payload` vector.
- Existing Cemu RPL load/link behavior is unchanged; all external-module
  behavior is opt-in through new APIs/options.
- Existing CEX2 principals, grants, package enablement, and title ownership do
  not depend on payload format.

## 20. Definition of complete support

WUPS support is complete only when a verified `plugin.wps` is loaded through
the external RPL API, all accepted ABI versions receive their correct hook and
backend arguments, WUT/reent/TLS and lifecycle work in Interpreter and JIT,
named/fixed/far/call-through patches restore transactionally across dynamic
RPLs, WUMS graph/lifecycle and standard module APIs work with real errors,
permissions and GUI/config/storage operate, and unload leaves no resources or
callbacks. All malformed inputs and guest pointers must fail safely and one
owner failure must remain isolated.

Task 1 meets only the container, manifest, signature, independent WPS
inspection, SDK tooling, common interface, documentation, and parser fuzz/unit
portion of that definition. RPL external-module lifetime, PPC callback runtime,
lifecycle, FunctionPatcher, WUMS, backend APIs, standard modules, GUI runtime,
and full conformance/integration execution remain dependencies of Tasks 2..7.
