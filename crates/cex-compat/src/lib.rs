//! Deterministic compatibility traces and private-fixture metadata.
//!
//! Trace output is deliberately narrower than a general logging format. It
//! excludes host time, paths, pointers, thread identifiers, arbitrary JSON,
//! and raw guest bytes so that two emulator implementations can be compared
//! safely and reproducibly.

mod diff;
mod error;
mod manifest;
mod redaction;
mod trace;
mod value;

pub use diff::{TraceDiff, TraceMismatch, TraceMismatchKind, diff_traces};
pub use error::{CompatError, Result};
pub use manifest::{
    ArtifactFingerprint, Baseline, CompatManifest, ExpectedCheckpoint, ExpectedOutcome,
    FIXTURE_MANIFEST_SCHEMA_VERSION, FixtureKind, FixtureMetadata, FixtureStatus, MAX_FIXTURES,
    MAX_MANIFEST_BYTES,
};
pub use redaction::RedactionPolicy;
pub use trace::{
    ArchitecturalFaultEvent, CheckpointEvent, CpuStateEvent, EventFields, FaultKind, GenericEvent,
    MAX_CANONICAL_LINE_BYTES, MAX_EVENT_FIELDS, MAX_TRACE_BYTES, MAX_TRACE_ENTRIES,
    MemoryHashAlgorithm, MemoryHashEvent, ReservationEvent, StopReason, TRACE_SCHEMA_VERSION,
    TerminalEvent, TraceCategory, TraceEntry, TraceEvent, TraceKey, TraceReader, TraceSource,
    TraceWriter, canonical_json_line,
};
pub use value::{DecimalI64, DecimalU64, HexU32, HexU64, Sha256Digest, TraceValue};
