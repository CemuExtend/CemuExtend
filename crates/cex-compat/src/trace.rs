use std::{
    collections::BTreeMap,
    fmt,
    io::{BufRead, Write},
};

use serde::{Deserialize, Serialize};

use crate::{
    CompatError, DecimalU64, HexU32, HexU64, RedactionPolicy, Result, Sha256Digest, TraceValue,
};

/// Version of the canonical JSONL trace wire schema accepted by this crate.
pub const TRACE_SCHEMA_VERSION: u16 = 1;
/// Largest encoded JSONL record, including its required trailing LF, in bytes.
pub const MAX_CANONICAL_LINE_BYTES: usize = 1024 * 1024;
/// Maximum number of records accepted by a canonical trace reader or writer.
///
/// This bounds allocations for traces made up of many small records while
/// leaving room for instruction-level fixture traces.
pub const MAX_TRACE_ENTRIES: usize = 65_536;
/// Maximum aggregate canonical JSONL size accepted by a trace reader or writer.
///
/// The limit applies to bytes read, including each record's trailing newline,
/// and bounds memory use even when individual lines are within the one-MiB
/// [`MAX_CANONICAL_LINE_BYTES`] limit.
pub const MAX_TRACE_BYTES: usize = 64 * 1024 * 1024;
/// Largest number of fields permitted in one generic event payload.
pub const MAX_EVENT_FIELDS: usize = 256;
const MAX_EVENT_TEXT_BYTES: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
/// Total ordering key for records in a canonical trace.
///
/// At a given guest cycle, sequences start at zero and increase contiguously;
/// a later guest cycle restarts the sequence at zero.
pub struct TraceKey {
    /// Guest CPU-cycle coordinate of the record.
    pub guest_cycle: u64,
    /// Deterministic intra-cycle record position.
    pub sequence: u32,
}

impl fmt::Display for TraceKey {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{}:{}", self.guest_cycle, self.sequence)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
/// Producer subsystem attributed to a trace record.
pub enum TraceSource {
    /// Espresso CPU emulation.
    Cpu,
    /// Guest-memory subsystem.
    Memory,
    /// Café OS service layer.
    Cafe,
    /// IOSU service layer.
    Iosu,
    /// GX2 graphics subsystem.
    Gx2,
    /// Audio subsystem.
    Audio,
    /// Online-service emulation.
    Online,
    /// Emulator frontend.
    Frontend,
    /// A host-originated diagnostic; payloads remain guest-stable.
    Host,
    /// The deterministic test harness.
    TestHarness,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
/// Semantic class of an event, used to enforce its allowed payload type.
pub enum TraceCategory {
    /// Register and architectural CPU-state snapshots.
    Cpu,
    /// Hashes of declared guest-memory ranges.
    Memory,
    /// Named state-hash synchronization points.
    Checkpoint,
    /// Inter-process communication observations.
    Ipc,
    /// GX2 observations.
    Gx2,
    /// Audio observations.
    Audio,
    /// Guest system-service observations.
    System,
    /// Online-service observations.
    Online,
    /// Frontend observations.
    Frontend,
    /// The unique record that closes a trace.
    Terminal,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// Hash algorithm identifier used by memory and state checkpoints.
pub enum MemoryHashAlgorithm {
    /// Version 1 SHA-256 domain used by this compatibility schema.
    #[serde(rename = "sha256-v1")]
    Sha256V1,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// Architecturally visible CPU state at a deterministic trace point.
pub struct CpuStateEvent {
    /// Guest program counter.
    pub pc: HexU32,
    /// Thirty-two guest general-purpose registers in architectural order.
    pub gpr: [HexU32; 32],
    /// Guest condition register.
    pub cr: HexU32,
    /// Guest link register.
    pub lr: HexU32,
    /// Guest count register.
    pub ctr: HexU32,
    /// Guest fixed-point exception register.
    pub xer: HexU32,
    /// Two raw 64-bit lanes per FPR, matching the paired-single architecture.
    pub fpr_bits: [[HexU64; 2]; 32],
    /// Guest floating-point status and control register.
    pub fpscr: HexU32,
    /// Eight guest graphics quantization registers.
    pub ugqr: [HexU32; 8],
    /// Current load-reserve/store-conditional reservation, if any.
    pub reservation: Option<ReservationEvent>,
    /// Pending architecturally observable fault, if any.
    pub pending_fault: Option<ArchitecturalFaultEvent>,
    /// Number of guest instructions retired at this snapshot.
    pub instructions_retired: DecimalU64,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// Reservation state that governs the next store-conditional operation.
pub struct ReservationEvent {
    /// Guest address covered by the reservation.
    pub address: HexU32,
    /// Value observed when the reservation was established.
    pub value: HexU32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
/// Architecture-defined reason a CPU state records a pending fault.
pub enum FaultKind {
    /// Fault while fetching an instruction.
    InstructionFetch,
    /// Fault while reading guest data.
    DataRead,
    /// Fault while writing guest data.
    DataWrite,
    /// Alignment requirement was violated.
    Alignment,
    /// The emulator does not implement the decoded instruction.
    UnsupportedInstruction,
    /// A counter overflow raised an architectural condition.
    CounterOverflow,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// Guest-visible fault details captured with a CPU-state event.
pub struct ArchitecturalFaultEvent {
    /// Guest instruction pointer at which the fault was observed.
    pub instruction_pointer: HexU32,
    /// Guest data address involved in the fault, when applicable.
    pub address: Option<HexU32>,
    /// Architecture-defined fault classification.
    pub kind: FaultKind,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// Digest of a named guest-memory range at a trace point.
pub struct MemoryHashEvent {
    /// Stable logical name, never a host or guest filesystem path.
    pub range: String,
    /// First guest address included in the hashed range.
    pub guest_address: HexU32,
    /// Number of guest bytes included in the digest.
    pub byte_length: DecimalU64,
    /// Algorithm that produced [`Self::digest`].
    pub algorithm: MemoryHashAlgorithm,
    /// Canonical digest of the selected range.
    pub digest: Sha256Digest,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// Named deterministic state-hash synchronization point.
pub struct CheckpointEvent {
    /// Stable checkpoint name shared with fixture expectations.
    pub name: String,
    /// Algorithm that produced [`Self::state_digest`].
    pub algorithm: MemoryHashAlgorithm,
    /// State digest emitted at this trace position.
    pub state_digest: Sha256Digest,
}

/// Sorted generic-event payload mapping stable field names to safe scalar values.
///
/// Producers must not place raw guest or user-controlled text, PII, or secrets
/// in event fields (including message/value-like fields). Emit only allowlisted
/// typed data, stable hashes, or [`TraceValue::Redacted`] values instead.
pub type EventFields = BTreeMap<String, TraceValue>;

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// Extensible, guest-stable observation for categories without a fixed payload.
pub struct GenericEvent {
    /// Stable event name, never a host path or endpoint.
    pub name: String,
    #[serde(default)]
    /// Sorted deterministic payload fields after redaction; see [`EventFields`]
    /// for the producer privacy contract.
    pub fields: EventFields,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
/// Reason the trace producer stopped executing the guest.
pub enum StopReason {
    /// The guest exited normally.
    GuestExit,
    /// The deterministic test harness reached its completion condition.
    TestCompleted,
    /// The configured instruction-retirement budget was exhausted.
    InstructionLimit,
    /// The configured guest-cycle budget was exhausted.
    CycleLimit,
    /// An exception could not be handled by the guest/emulator contract.
    UnhandledException,
    /// Guest execution encountered an invalid instruction.
    InvalidInstruction,
    /// A host-side failure stopped execution without serializing host details.
    HostError,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// The final event in a trace, which closes its ordering and reader contract.
pub struct TerminalEvent {
    /// Why execution stopped.
    pub reason: StopReason,
    #[serde(skip_serializing_if = "Option::is_none")]
    /// Guest exit code when [`Self::reason`] is [`StopReason::GuestExit`].
    pub guest_exit_code: Option<HexU32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    /// Stable implementation-neutral detail label, when one is available.
    pub detail_code: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
/// Payload carried by a trace record and constrained by its category.
pub enum TraceEvent {
    /// Architectural CPU state.
    CpuState(Box<CpuStateEvent>),
    /// Guest-memory range digest.
    MemoryHash(MemoryHashEvent),
    /// Named state-digest checkpoint.
    Checkpoint(CheckpointEvent),
    /// Extensible generic subsystem event.
    Event(GenericEvent),
    /// Unique terminal stop event.
    Terminal(TerminalEvent),
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// One canonical JSONL record in the deterministic trace interchange format.
pub struct TraceEntry {
    /// Version of the wire schema used to serialize this record.
    pub schema_version: u16,
    /// Deterministic guest-cycle coordinate, encoded as a decimal string.
    pub guest_cycle: DecimalU64,
    /// Contiguous zero-based position within [`Self::guest_cycle`].
    pub sequence: u32,
    /// Subsystem that produced the record.
    pub source: TraceSource,
    /// Emulated Espresso core number. `null` means the event is not core-bound.
    pub core: Option<u8>,
    /// Category that determines which [`TraceEvent`] variant is valid.
    pub category: TraceCategory,
    /// Event payload for the category.
    pub event: TraceEvent,
}

impl TraceEntry {
    /// Creates a schema-versioned entry for a deterministic guest-cycle point.
    pub fn new(
        guest_cycle: u64,
        sequence: u32,
        source: TraceSource,
        core: Option<u8>,
        category: TraceCategory,
        event: TraceEvent,
    ) -> Self {
        Self {
            schema_version: TRACE_SCHEMA_VERSION,
            guest_cycle: guest_cycle.into(),
            sequence,
            source,
            core,
            category,
            event,
        }
    }

    /// Returns the key used for trace ordering and first-difference comparison.
    pub fn key(&self) -> TraceKey {
        TraceKey {
            guest_cycle: self.guest_cycle.0,
            sequence: self.sequence,
        }
    }

    fn validate(&self, require_redacted: bool, policy: &RedactionPolicy) -> Result<()> {
        if self.schema_version != TRACE_SCHEMA_VERSION {
            return Err(CompatError::UnsupportedTraceSchema {
                expected: TRACE_SCHEMA_VERSION,
                found: self.schema_version,
            });
        }
        if self.core.is_some_and(|core| core > 2) {
            return Err(CompatError::InvalidTrace(
                "core must be null or an Espresso core in the range 0..=2".to_owned(),
            ));
        }

        let category_matches = matches!(
            (&self.category, &self.event),
            (TraceCategory::Cpu, TraceEvent::CpuState(_))
                | (TraceCategory::Memory, TraceEvent::MemoryHash(_))
                | (TraceCategory::Checkpoint, TraceEvent::Checkpoint(_))
                | (
                    TraceCategory::Ipc
                        | TraceCategory::Gx2
                        | TraceCategory::Audio
                        | TraceCategory::System
                        | TraceCategory::Online
                        | TraceCategory::Frontend,
                    TraceEvent::Event(_)
                )
                | (TraceCategory::Terminal, TraceEvent::Terminal(_))
        );
        if !category_matches {
            return Err(CompatError::InvalidTrace(format!(
                "category {:?} does not match event kind",
                self.category
            )));
        }

        match &self.event {
            TraceEvent::CpuState(_) => {
                if self.source != TraceSource::Cpu || self.core.is_none() {
                    return Err(CompatError::InvalidTrace(
                        "CPU state records require source=cpu and a core".to_owned(),
                    ));
                }
            }
            TraceEvent::MemoryHash(event) => validate_label("memory range", &event.range)?,
            TraceEvent::Checkpoint(event) => validate_label("checkpoint", &event.name)?,
            TraceEvent::Event(event) => {
                validate_label("event", &event.name)?;
                if event.fields.len() > MAX_EVENT_FIELDS {
                    return Err(CompatError::InvalidTrace(format!(
                        "event {:?} has {} fields; the maximum is {}",
                        event.name,
                        event.fields.len(),
                        MAX_EVENT_FIELDS
                    )));
                }
                let mut text_bytes = 0_usize;
                for (name, value) in &event.fields {
                    validate_label("event field", name)?;
                    if policy.is_forbidden_nondeterministic_field(name) {
                        return Err(CompatError::InvalidTrace(format!(
                            "event field {name:?} is host-dependent; encode a guest-stable identifier instead"
                        )));
                    }
                    if require_redacted
                        && policy.requires_redaction(name, value)
                        && *value != TraceValue::Redacted
                    {
                        return Err(CompatError::InvalidTrace(format!(
                            "sensitive event field {name:?} was not redacted"
                        )));
                    }
                    validate_trace_value(name, value)?;
                    if let TraceValue::Text(text) = value {
                        if RedactionPolicy::is_forbidden_text(text) {
                            return Err(CompatError::InvalidTrace(format!(
                                "text field {name:?} contains a URL, URI, or credential-bearing query"
                            )));
                        }
                        text_bytes = text_bytes.saturating_add(text.len());
                    }
                }
                if text_bytes > MAX_EVENT_TEXT_BYTES {
                    return Err(CompatError::InvalidTrace(format!(
                        "event {:?} contains more than {} bytes of text",
                        event.name, MAX_EVENT_TEXT_BYTES
                    )));
                }
            }
            TraceEvent::Terminal(event) => {
                if let Some(detail_code) = &event.detail_code {
                    validate_label("terminal detail code", detail_code)?;
                }
            }
        }
        Ok(())
    }
}

fn validate_label(kind: &str, label: &str) -> Result<()> {
    if label.is_empty() || label.len() > 128 {
        return Err(CompatError::InvalidTrace(format!(
            "{kind} names must contain 1..=128 bytes"
        )));
    }
    if !label
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.' | b':'))
    {
        return Err(CompatError::InvalidTrace(format!(
            "{kind} name {label:?} contains a path separator, whitespace, or control character"
        )));
    }
    Ok(())
}

fn validate_trace_value(name: &str, value: &TraceValue) -> Result<()> {
    let TraceValue::Text(text) = value else {
        return Ok(());
    };
    if text.len() > 4096 || text.chars().any(char::is_control) {
        return Err(CompatError::InvalidTrace(format!(
            "text field {name:?} must be at most 4096 bytes and contain no control characters"
        )));
    }
    let looks_like_absolute_path = text.starts_with('/')
        || text.starts_with("\\\\")
        || (text.len() >= 3
            && text.as_bytes()[1] == b':'
            && matches!(text.as_bytes()[2], b'\\' | b'/'));
    if looks_like_absolute_path {
        return Err(CompatError::InvalidTrace(format!(
            "text field {name:?} contains an absolute host path"
        )));
    }
    Ok(())
}

/// Validates, redacts, and encodes one entry as canonical LF-terminated JSONL.
///
/// The result is suitable for an oracle trace only after callers preserve key
/// ordering and append one terminal event.
pub fn canonical_json_line(entry: &TraceEntry) -> Result<Vec<u8>> {
    let policy = RedactionPolicy::default();
    let mut redacted = entry.clone();
    redacted.validate(false, &policy)?;
    policy.redact_entry(&mut redacted);
    redacted.validate(true, &policy)?;
    canonical_json_line_redacted(&redacted)
}

fn canonical_json_line_redacted(entry: &TraceEntry) -> Result<Vec<u8>> {
    let mut line =
        serde_json::to_vec(entry).map_err(|source| CompatError::Json { line: 0, source })?;
    line.push(b'\n');
    if line.len() > MAX_CANONICAL_LINE_BYTES {
        return Err(CompatError::TraceLineTooLong {
            line: 0,
            max_bytes: MAX_CANONICAL_LINE_BYTES,
        });
    }
    Ok(line)
}

/// Streaming canonical-trace encoder that enforces aggregate limits, order,
/// and terminal closure.
pub struct TraceWriter<W> {
    inner: W,
    entries_written: usize,
    trace_bytes: usize,
    previous: Option<TraceKey>,
    terminal_seen: bool,
    poisoned: bool,
    policy: RedactionPolicy,
}

impl<W: Write> TraceWriter<W> {
    /// Creates a writer with the default conservative redaction policy.
    pub fn new(inner: W) -> Self {
        Self {
            inner,
            entries_written: 0,
            trace_bytes: 0,
            previous: None,
            terminal_seen: false,
            poisoned: false,
            policy: RedactionPolicy::default(),
        }
    }

    /// Creates a writer using the supplied redaction policy.
    pub fn with_redaction_policy(inner: W, policy: RedactionPolicy) -> Self {
        Self {
            inner,
            entries_written: 0,
            trace_bytes: 0,
            previous: None,
            terminal_seen: false,
            poisoned: false,
            policy,
        }
    }

    /// Validates, redacts, and writes the next ordered entry.
    ///
    /// Once a terminal entry is written, further calls fail so a finished
    /// trace has exactly one final stop event. An I/O failure poisons the
    /// writer because the sink may already contain a partial record.
    pub fn write_entry(&mut self, entry: &TraceEntry) -> Result<()> {
        if self.poisoned {
            return Err(CompatError::WriterPoisoned);
        }
        if self.terminal_seen {
            return Err(CompatError::RecordAfterTerminal);
        }
        entry.validate(false, &self.policy)?;
        let key = entry.key();
        validate_order(self.previous, key)?;

        let mut redacted = entry.clone();
        self.policy.redact_entry(&mut redacted);
        redacted.validate(true, &self.policy)?;
        let line = canonical_json_line_redacted(&redacted)?;

        // Complete canonical serialization and enforce both aggregate limits
        // before allowing any bytes from this record to reach the sink.
        ensure_entry_budget(self.entries_written, false)?;
        let trace_bytes = checked_trace_size(self.trace_bytes, line.len())?;
        if let Err(error) = self.inner.write_all(&line) {
            self.poisoned = true;
            return Err(error.into());
        }

        self.entries_written += 1;
        self.trace_bytes = trace_bytes;
        self.previous = Some(key);
        self.terminal_seen = matches!(entry.event, TraceEvent::Terminal(_));
        Ok(())
    }

    /// Flushes the stream and returns its inner writer after terminal closure.
    ///
    /// A writer poisoned by an earlier I/O failure cannot be finished.
    pub fn finish(mut self) -> Result<W> {
        if self.poisoned {
            return Err(CompatError::WriterPoisoned);
        }
        if !self.terminal_seen {
            return Err(CompatError::MissingTerminal);
        }
        self.inner.flush()?;
        Ok(self.inner)
    }
}

/// Streaming validator and parser for bounded canonical JSONL traces.
pub struct TraceReader<R> {
    inner: R,
    line_number: usize,
    entries_read: usize,
    trace_bytes: usize,
    previous: Option<TraceKey>,
    terminal_seen: bool,
    eof_seen: bool,
    policy: RedactionPolicy,
}

impl<R: BufRead> TraceReader<R> {
    /// Creates a reader that applies the default redaction and format policy.
    pub fn new(inner: R) -> Self {
        Self {
            inner,
            line_number: 0,
            entries_read: 0,
            trace_bytes: 0,
            previous: None,
            terminal_seen: false,
            eof_seen: false,
            policy: RedactionPolicy::default(),
        }
    }

    /// Reads and validates the next canonical record, or `None` at valid EOF.
    ///
    /// A call after the terminal record checks that no trailing bytes exist.
    pub fn next_entry(&mut self) -> Result<Option<TraceEntry>> {
        if self.eof_seen {
            return Ok(None);
        }
        if self.terminal_seen {
            if !self.inner.fill_buf()?.is_empty() {
                return Err(CompatError::RecordAfterTerminal);
            }
            self.eof_seen = true;
            return Ok(None);
        }
        // Reject before allocating or consuming another line. A trace whose
        // MAXth record was terminal takes the branch above so trailing bytes
        // are still checked without accepting another record.
        ensure_entry_budget(self.entries_read, false)?;

        let mut line = Vec::new();
        let bytes_read = read_until_bounded(
            &mut self.inner,
            b'\n',
            MAX_CANONICAL_LINE_BYTES + 1,
            &mut line,
        )?;
        if line.len() > MAX_CANONICAL_LINE_BYTES {
            return Err(CompatError::TraceLineTooLong {
                line: self.line_number + 1,
                max_bytes: MAX_CANONICAL_LINE_BYTES,
            });
        }
        self.trace_bytes = checked_trace_size(self.trace_bytes, line.len())?;
        if bytes_read == 0 {
            self.eof_seen = true;
            if self.terminal_seen {
                return Ok(None);
            }
            return Err(CompatError::MissingTerminal);
        }
        self.line_number += 1;
        if line.last() != Some(&b'\n') || line.ends_with(b"\r\n") {
            return Err(CompatError::NonCanonical {
                line: self.line_number,
            });
        }
        let json = &line[..line.len() - 1];
        let entry: TraceEntry =
            serde_json::from_slice(json).map_err(|source| CompatError::Json {
                line: self.line_number,
                source,
            })?;
        entry.validate(true, &self.policy)?;
        if canonical_json_line_redacted(&entry)? != line {
            return Err(CompatError::NonCanonical {
                line: self.line_number,
            });
        }
        validate_order(self.previous, entry.key())?;
        self.previous = Some(entry.key());
        self.terminal_seen = matches!(entry.event, TraceEvent::Terminal(_));
        self.entries_read += 1;
        Ok(Some(entry))
    }

    /// Reads the complete trace while enforcing byte, record, order, and closure limits.
    pub fn read_all(mut self) -> Result<Vec<TraceEntry>> {
        let mut entries = Vec::new();
        loop {
            // Keep the post-terminal call so `next_entry` can reject trailing
            // bytes, but do not read another record once the entry budget is
            // exhausted.
            ensure_entry_budget(entries.len(), self.terminal_seen)?;
            match self.next_entry()? {
                Some(entry) => entries.push(entry),
                None => break,
            }
        }
        if !self.terminal_seen {
            return Err(CompatError::MissingTerminal);
        }
        Ok(entries)
    }
}

fn read_until_bounded<R: BufRead>(
    reader: &mut R,
    delimiter: u8,
    limit: usize,
    output: &mut Vec<u8>,
) -> std::io::Result<usize> {
    let mut bytes_read = 0;

    loop {
        let consumed = {
            let buffer = reader.fill_buf()?;
            if buffer.is_empty() || bytes_read == limit {
                return Ok(bytes_read);
            }

            let available = (limit - bytes_read).min(buffer.len());
            let consumed = buffer[..available]
                .iter()
                .position(|&byte| byte == delimiter)
                .map_or(available, |position| position + 1);
            output.extend_from_slice(&buffer[..consumed]);
            consumed
        };
        reader.consume(consumed);
        bytes_read += consumed;

        if output.last() == Some(&delimiter) {
            return Ok(bytes_read);
        }
    }
}

fn checked_trace_size(current: usize, next_line: usize) -> Result<usize> {
    let next = current
        .checked_add(next_line)
        .ok_or(CompatError::TraceTooLarge {
            max_bytes: MAX_TRACE_BYTES,
        })?;
    if next > MAX_TRACE_BYTES {
        return Err(CompatError::TraceTooLarge {
            max_bytes: MAX_TRACE_BYTES,
        });
    }
    Ok(next)
}

fn ensure_entry_budget(retained: usize, terminal_seen: bool) -> Result<()> {
    if retained >= MAX_TRACE_ENTRIES && !terminal_seen {
        return Err(CompatError::TraceTooManyEntries {
            max_entries: MAX_TRACE_ENTRIES,
        });
    }
    Ok(())
}

fn validate_order(previous: Option<TraceKey>, current: TraceKey) -> Result<()> {
    if let Some(previous) = previous {
        let expected_sequence = match current.guest_cycle.cmp(&previous.guest_cycle) {
            std::cmp::Ordering::Equal => previous.sequence.checked_add(1),
            std::cmp::Ordering::Greater => Some(0),
            std::cmp::Ordering::Less => None,
        };
        if expected_sequence != Some(current.sequence) {
            return Err(CompatError::OutOfOrder {
                previous: previous.to_string(),
                current: current.to_string(),
            });
        }
    } else if current.sequence != 0 {
        return Err(CompatError::InvalidTrace(
            "the first record at a guest cycle must use sequence 0".to_owned(),
        ));
    }
    Ok(())
}

#[cfg(test)]
mod budget_tests {
    use std::io::{Cursor, Error};

    use super::*;

    fn terminal(cycle: u64) -> TraceEntry {
        TraceEntry::new(
            cycle,
            0,
            TraceSource::TestHarness,
            None,
            TraceCategory::Terminal,
            TraceEvent::Terminal(TerminalEvent {
                reason: StopReason::TestCompleted,
                guest_exit_code: None,
                detail_code: Some("budget_test".to_owned()),
            }),
        )
    }

    fn event(cycle: u64) -> TraceEntry {
        TraceEntry::new(
            cycle,
            0,
            TraceSource::TestHarness,
            None,
            TraceCategory::System,
            TraceEvent::Event(GenericEvent {
                name: "budget_test".to_owned(),
                fields: BTreeMap::new(),
            }),
        )
    }

    #[test]
    fn aggregate_byte_budget_has_a_deterministic_boundary() {
        assert!(matches!(
            checked_trace_size(MAX_TRACE_BYTES - 1, 1),
            Ok(size) if size == MAX_TRACE_BYTES
        ));
        assert!(matches!(
            checked_trace_size(MAX_TRACE_BYTES, 1),
            Err(CompatError::TraceTooLarge {
                max_bytes: MAX_TRACE_BYTES
            })
        ));
        assert!(matches!(
            checked_trace_size(usize::MAX, 1),
            Err(CompatError::TraceTooLarge {
                max_bytes: MAX_TRACE_BYTES
            })
        ));
    }

    #[test]
    fn retained_entry_budget_allows_only_terminal_completion_at_limit() {
        assert!(ensure_entry_budget(MAX_TRACE_ENTRIES - 1, false).is_ok());
        assert!(ensure_entry_budget(MAX_TRACE_ENTRIES, true).is_ok());
        assert!(matches!(
            ensure_entry_budget(MAX_TRACE_ENTRIES, false),
            Err(CompatError::TraceTooManyEntries {
                max_entries: MAX_TRACE_ENTRIES
            })
        ));
    }

    #[test]
    fn read_all_reports_cumulative_limits_without_large_allocations() {
        let terminal = terminal(1);
        let line = canonical_json_line(&terminal).unwrap();

        let mut entry_limited = TraceReader::new(Cursor::new(line.clone()));
        entry_limited.entries_read = MAX_TRACE_ENTRIES;
        assert!(matches!(
            entry_limited.next_entry(),
            Err(CompatError::TraceTooManyEntries {
                max_entries: MAX_TRACE_ENTRIES
            })
        ));
        assert_eq!(entry_limited.inner.position(), 0);

        let mut byte_limited = TraceReader::new(Cursor::new(line));
        byte_limited.trace_bytes = MAX_TRACE_BYTES;
        assert!(matches!(
            byte_limited.read_all(),
            Err(CompatError::TraceTooLarge {
                max_bytes: MAX_TRACE_BYTES
            })
        ));
    }

    #[test]
    fn writer_accepts_exact_aggregate_boundaries() {
        let entry = terminal(1);
        let line = canonical_json_line(&entry).unwrap();
        let mut writer = TraceWriter::new(Vec::new());
        writer.entries_written = MAX_TRACE_ENTRIES - 1;
        writer.trace_bytes = MAX_TRACE_BYTES - line.len();

        writer.write_entry(&entry).unwrap();

        assert_eq!(writer.entries_written, MAX_TRACE_ENTRIES);
        assert_eq!(writer.trace_bytes, MAX_TRACE_BYTES);
        assert_eq!(writer.inner, line);
    }

    #[test]
    fn writer_rejects_aggregate_limits_without_writing_a_partial_record() {
        let entry = terminal(1);
        let line = canonical_json_line(&entry).unwrap();
        let prefix = b"existing trace\n".to_vec();
        let mut byte_limited = TraceWriter::new(prefix.clone());
        byte_limited.trace_bytes = MAX_TRACE_BYTES - line.len() + 1;
        assert!(matches!(
            byte_limited.write_entry(&entry),
            Err(CompatError::TraceTooLarge {
                max_bytes: MAX_TRACE_BYTES
            })
        ));
        assert_eq!(byte_limited.inner, prefix);
        assert_eq!(byte_limited.trace_bytes, MAX_TRACE_BYTES - line.len() + 1);
        assert!(!byte_limited.poisoned);

        let mut entry_limited = TraceWriter::new(prefix.clone());
        entry_limited.entries_written = MAX_TRACE_ENTRIES;
        assert!(matches!(
            entry_limited.write_entry(&entry),
            Err(CompatError::TraceTooManyEntries {
                max_entries: MAX_TRACE_ENTRIES
            })
        ));
        assert_eq!(entry_limited.inner, prefix);
        assert_eq!(entry_limited.entries_written, MAX_TRACE_ENTRIES);
        assert!(!entry_limited.poisoned);
    }

    struct FailingWriter {
        bytes: Vec<u8>,
        bytes_before_error: usize,
    }

    impl Write for FailingWriter {
        fn write(&mut self, buffer: &[u8]) -> std::io::Result<usize> {
            if self.bytes_before_error == 0 {
                return Err(Error::other("injected write failure"));
            }
            let written = self.bytes_before_error.min(buffer.len());
            self.bytes.extend_from_slice(&buffer[..written]);
            self.bytes_before_error -= written;
            Ok(written)
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn writer_is_poisoned_after_a_partial_line_write() {
        let mut writer = TraceWriter::new(FailingWriter {
            bytes: Vec::new(),
            bytes_before_error: 1,
        });

        assert!(matches!(
            writer.write_entry(&event(1)),
            Err(CompatError::Io(_))
        ));
        assert_eq!(writer.inner.bytes.len(), 1);
        assert_eq!(writer.entries_written, 0);
        assert_eq!(writer.trace_bytes, 0);
        assert_eq!(writer.previous, None);
        assert!(!writer.terminal_seen);
        assert!(writer.poisoned);

        assert!(matches!(
            writer.write_entry(&event(1)),
            Err(CompatError::WriterPoisoned)
        ));
        assert_eq!(writer.inner.bytes.len(), 1);
        assert!(matches!(writer.finish(), Err(CompatError::WriterPoisoned)));
    }
}
