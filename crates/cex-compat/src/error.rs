use std::io;

use thiserror::Error;

/// Result used by trace serialization, parsing, and manifest validation.
pub type Result<T> = std::result::Result<T, CompatError>;

#[derive(Debug, Error)]
/// Error produced when data cannot satisfy the deterministic compatibility contract.
pub enum CompatError {
    /// Reading or writing the JSONL stream failed.
    #[error("trace I/O failed: {0}")]
    Io(#[from] io::Error),

    /// A JSON document could not be parsed at the indicated one-based line.
    #[error("invalid JSON on line {line}: {source}")]
    Json {
        /// One-based JSONL line number, or zero for a non-line-oriented encode.
        line: usize,
        /// The underlying JSON parser error.
        #[source]
        source: serde_json::Error,
    },

    /// A JSONL record differs from the byte-for-byte canonical representation.
    #[error("trace line {line} is not in canonical JSONL form")]
    NonCanonical {
        /// One-based line number that failed canonicalization.
        line: usize,
    },

    /// A single JSONL record exceeds the compatibility format's size cap.
    #[error("trace line {line} exceeds the maximum canonical line size of {max_bytes} bytes")]
    TraceLineTooLong {
        /// One-based line number, or zero when encoding an in-memory entry.
        line: usize,
        /// Maximum permitted encoded line length in bytes, including LF.
        max_bytes: usize,
    },

    /// The complete JSONL stream exceeds the bounded-reader size cap.
    #[error("trace exceeds the maximum size of {max_bytes} bytes")]
    TraceTooLarge {
        /// Maximum permitted aggregate trace length in bytes.
        max_bytes: usize,
    },

    /// The trace has more records than the bounded reader may retain.
    #[error("trace contains more than the maximum of {max_entries} entries")]
    TraceTooManyEntries {
        /// Maximum permitted record count.
        max_entries: usize,
    },

    /// A record uses a schema version unsupported by this compatibility build.
    #[error("unsupported trace schema version {found}; this build only accepts version {expected}")]
    UnsupportedTraceSchema {
        /// Schema version this build can interpret.
        expected: u16,
        /// Schema version declared by the received record.
        found: u16,
    },

    /// Record keys were not strictly ordered by cycle and contiguous sequence.
    #[error(
        "trace key {current} is not strictly after the previous key {previous}; guest cycles must be monotonic and sequence numbers unique"
    )]
    OutOfOrder {
        /// Previously accepted `(guest_cycle, sequence)` key.
        previous: String,
        /// Received key that did not immediately follow the previous key.
        current: String,
    },

    /// A record violates a semantic trace invariant.
    #[error("trace record is invalid: {0}")]
    /// Human-readable invariant violation.
    InvalidTrace(String),

    /// The stream ended before it emitted exactly one terminal event.
    #[error("trace ended without a terminal stop event")]
    MissingTerminal,

    /// A producer attempted to append a record after the terminal event.
    #[error("a trace record was written after the terminal stop event")]
    RecordAfterTerminal,

    /// A previous write failed and may have left a partial record in the sink.
    #[error("trace writer cannot continue after an earlier I/O failure")]
    WriterPoisoned,

    /// Fixture metadata is malformed or violates oracle-manifest invariants.
    #[error("fixture manifest is invalid: {0}")]
    /// Human-readable manifest validation failure.
    InvalidManifest(String),
}
