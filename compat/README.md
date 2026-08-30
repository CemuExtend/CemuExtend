# Compatibility oracle data

`cex-compat` defines the interchange format used to compare the frozen C++
oracle with the Rust implementation. Trace files are canonical JSONL. Every
line has this fixed envelope order:

1. `schema_version`
2. `guest_cycle` (a decimal string, so the full `u64` range is portable)
3. `sequence` (strictly increasing within a cycle)
4. `source`, `core`, and `category`
5. `event`

Entries are strictly ordered by `(guest_cycle, sequence)`. The final entry is
always a terminal event with an explicit stop reason. Writers emit compact
UTF-8 JSON followed by LF and readers reject alternate key order, whitespace,
CRLF, schema mismatches, duplicate/out-of-order keys, trailing records, and
lines larger than 1 MiB. `read_all` additionally retains at most 65,536 entries
and accepts at most 64 MiB of encoded JSONL, including newline bytes.

Guest addresses, registers, and IEEE floating-point raw bits use fixed-width
lowercase hexadecimal strings. Memory and checkpoint records name a stable
logical range and state the versioned hash algorithm (`sha256-v1`). Event maps
are `BTreeMap`s so field order cannot depend on a language hash map.

The whole-guest-memory `sha256-v1` digest has the following exact preimage.
Begin with the ASCII bytes `CemuExtend guest memory v1` and one NUL byte. Add
mappings in ascending order as byte `M`, `start_page` encoded BE32, `end_page`
encoded BE32, and the permission bits as one byte. Then add each nonzero
resident page in ascending order as byte `P`, `page_index` encoded BE32, and
exactly 4096 raw page bytes. SHA-256 is calculated over that complete byte
sequence.

Checkpoint `state_digest` preimage versioning is reserved but is not emitted
by the current milestone. A fixture stays `metadata_only` until the pinned C++
oracle and Rust producer share a documented checkpoint preimage; a
`metadata_only` fixture cannot contain an `expected` block.

## Privacy and determinism

Trace records never carry raw guest memory. The writer replaces credential,
account, device, certificate, token, cookie, and key fields with the typed
`redacted` value. Readers reject canonical-looking files in which those fields
or secret-looking values were not redacted. Host wall-clock values, paths,
URLs/endpoints/hostnames, pointers, process IDs, and thread IDs are rejected;
use guest-cycle and logical identifiers instead.

Producers must not pass raw guest- or user-controlled text, PII, or secrets to
generic event `message`/`value` fields (or any other opaque text field).
Automatic secret detection is intentionally incomplete. Producers must emit
only allowlisted typed values, stable hashes, or typed `redacted` payloads.

`fixtures/manifest.schema.json` describes metadata that may be checked in.
Manifests contain only logical names, title/version metadata, byte lengths,
SHA-256 fingerprints, expected stop reasons, and expected checkpoint hashes.
They have no field capable of holding fixture bytes, local paths, URLs,
certificates, console keys, or credentials. Commercial titles, system apps,
console dumps, certificates, PNIDs, and keys remain outside the repository.
