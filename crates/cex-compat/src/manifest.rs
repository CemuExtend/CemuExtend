use std::{
    collections::BTreeSet,
    io::{self, Write},
};

use serde::{Deserialize, Serialize};

use crate::{CompatError, DecimalU64, MemoryHashAlgorithm, Result, Sha256Digest, StopReason};

/// Schema version for the checked-in, privacy-preserving fixture manifest.
pub const FIXTURE_MANIFEST_SCHEMA_VERSION: u16 = 1;
/// Largest accepted fixture-manifest document, in bytes.
pub const MAX_MANIFEST_BYTES: usize = 1024 * 1024;
/// Largest number of fixtures accepted in one oracle manifest.
pub const MAX_FIXTURES: usize = 1024;
const MAX_CHECKPOINTS_PER_FIXTURE: usize = 4096;

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
/// Canonical metadata for private compatibility fixtures and their oracle output.
///
/// The manifest identifies artifacts without embedding paths, bytes, keys, or
/// network locations, allowing it to be committed and compared safely.
pub struct CompatManifest {
    /// Version of the manifest wire schema.
    pub schema_version: u16,
    /// Source revision from which the expected oracle data was captured.
    pub baseline: Baseline,
    /// Fixture records sorted strictly by their stable logical identifier.
    pub fixtures: Vec<FixtureMetadata>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
/// Pinned source identity for compatibility expectations.
pub struct Baseline {
    /// Repository label, not a checkout path or remote URL.
    pub repository: String,
    /// Lowercase hexadecimal source revision used to capture the oracle.
    pub revision: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
/// Classifies the kind of guest artifact represented by a fixture record.
pub enum FixtureKind {
    /// A deliberately small executable program used for a targeted behavior.
    SyntheticProgram,
    /// A synthetic RPX image used to exercise title-loading behavior.
    SyntheticRpx,
    /// A non-commercial homebrew artifact.
    Homebrew,
    /// A retail title artifact identified without distributing its contents.
    Title,
    /// A Wii U system application artifact.
    SystemApplication,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
/// Declares whether a fixture has a committed trace oracle.
pub enum FixtureStatus {
    /// Artifact fingerprint is real, but no oracle trace has been recorded yet.
    MetadataOnly,
    /// Expected trace and checkpoint hashes were captured from the pinned oracle.
    Validated,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
/// Non-sensitive identity and optional oracle expectation for one fixture.
pub struct FixtureMetadata {
    /// Stable logical identifier, used as the manifest sort key.
    pub id: String,
    /// Guest-artifact class used to select and report the fixture.
    pub kind: FixtureKind,
    /// Whether this fixture has an expected trace oracle.
    pub status: FixtureStatus,
    #[serde(skip_serializing_if = "Option::is_none")]
    /// Canonical 16-hex-digit title identifier, if applicable.
    pub title_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    /// Guest-visible artifact version label, if applicable.
    pub version: Option<String>,
    /// Artifact identity that lets a runner select the correct private input.
    pub artifact: ArtifactFingerprint,
    #[serde(skip_serializing_if = "Option::is_none")]
    /// Terminal and checkpoint expectations captured from the pinned oracle.
    pub expected: Option<ExpectedOutcome>,
}

/// Identification data only. There is deliberately no bytes, path, URL,
/// certificate, console-key, or credential field in this type.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ArtifactFingerprint {
    /// Stable artifact label; never a host path, URL, or title key.
    pub logical_name: String,
    /// Exact artifact length encoded as a canonical decimal string.
    pub byte_length: DecimalU64,
    /// SHA-256 of the private artifact bytes.
    pub sha256: Sha256Digest,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
/// Expected end state and checkpoints emitted by the pinned compatibility oracle.
pub struct ExpectedOutcome {
    /// Reason the oracle trace terminated.
    pub terminal_reason: StopReason,
    /// Guest cycle of the terminal oracle record.
    pub final_guest_cycle: DecimalU64,
    /// SHA-256 of the complete canonical oracle JSONL trace.
    pub trace_sha256: Sha256Digest,
    #[serde(default)]
    /// Ordered state-hash observations that localize behavioral divergence.
    pub checkpoints: Vec<ExpectedCheckpoint>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
/// One named state digest expected at a deterministic guest cycle.
pub struct ExpectedCheckpoint {
    /// Stable checkpoint label shared with its trace event.
    pub name: String,
    /// Guest cycle at which the checkpoint is observed.
    pub guest_cycle: DecimalU64,
    /// Hash algorithm used to calculate [`Self::state_digest`].
    pub algorithm: MemoryHashAlgorithm,
    /// Oracle state digest at [`Self::guest_cycle`].
    pub state_digest: Sha256Digest,
}

impl CompatManifest {
    /// Parses, bounds-checks, and validates a manifest from canonical JSON bytes.
    pub fn from_json(bytes: &[u8]) -> Result<Self> {
        if bytes.len() > MAX_MANIFEST_BYTES {
            return Err(manifest_too_large_error());
        }
        let manifest: Self = serde_json::from_slice(bytes).map_err(|source| CompatError::Json {
            line: source.line(),
            source,
        })?;
        manifest.validate()?;
        Ok(manifest)
    }

    /// Validates and renders this manifest as newline-terminated pretty JSON.
    ///
    /// This is the checked-in representation of fixture metadata, not a trace
    /// serialization format.
    pub fn to_pretty_json(&self) -> Result<Vec<u8>> {
        self.validate()?;
        let mut writer = BoundedVecWriter::new(MAX_MANIFEST_BYTES);
        if let Err(source) = serde_json::to_writer_pretty(&mut writer, self) {
            if writer.limit_exceeded() {
                return Err(manifest_too_large_error());
            }
            return Err(CompatError::Json { line: 0, source });
        }
        if let Err(source) = writer.write_all(b"\n") {
            if writer.limit_exceeded() {
                return Err(manifest_too_large_error());
            }
            return Err(CompatError::Json {
                line: 0,
                source: serde_json::Error::io(source),
            });
        }
        Ok(writer.into_inner())
    }

    /// Checks schema, ordering, privacy, and oracle-consistency invariants.
    pub fn validate(&self) -> Result<()> {
        if self.schema_version != FIXTURE_MANIFEST_SCHEMA_VERSION {
            let schema_version = self.schema_version;
            return Err(CompatError::InvalidManifest(format!(
                "unsupported schema version {schema_version}; expected {FIXTURE_MANIFEST_SCHEMA_VERSION}"
            )));
        }
        validate_label("baseline repository", &self.baseline.repository)?;
        if !(8..=40).contains(&self.baseline.revision.len())
            || !self
                .baseline
                .revision
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
        {
            return Err(CompatError::InvalidManifest(
                "baseline revision must be 8..=40 lowercase hexadecimal characters".to_owned(),
            ));
        }

        if self.fixtures.len() > MAX_FIXTURES {
            return Err(CompatError::InvalidManifest(format!(
                "manifest has more than {MAX_FIXTURES} fixtures"
            )));
        }
        let mut previous_id: Option<&str> = None;
        let mut ids = BTreeSet::new();
        for fixture in &self.fixtures {
            validate_fixture(fixture, &mut previous_id, &mut ids)?;
        }
        Ok(())
    }
}

fn manifest_too_large_error() -> CompatError {
    CompatError::InvalidManifest(format!(
        "manifest exceeds the {MAX_MANIFEST_BYTES} byte limit"
    ))
}

fn bounded_output_len(current_len: usize, additional_len: usize, max_len: usize) -> Option<usize> {
    current_len
        .checked_add(additional_len)
        .filter(|required_len| *required_len <= max_len)
}

fn bounded_reserve_capacity(current_capacity: usize, required_len: usize, max_len: usize) -> usize {
    current_capacity
        .saturating_mul(2)
        .max(required_len)
        .min(max_len)
}

struct BoundedVecWriter {
    bytes: Vec<u8>,
    max_len: usize,
    limit_exceeded: bool,
}

impl BoundedVecWriter {
    fn new(max_len: usize) -> Self {
        Self {
            bytes: Vec::new(),
            max_len,
            limit_exceeded: false,
        }
    }

    fn limit_exceeded(&self) -> bool {
        self.limit_exceeded
    }

    fn into_inner(self) -> Vec<u8> {
        self.bytes
    }
}

impl Write for BoundedVecWriter {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let Some(required_len) = bounded_output_len(self.bytes.len(), bytes.len(), self.max_len)
        else {
            self.limit_exceeded = true;
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "manifest output exceeds its byte limit",
            ));
        };

        if required_len > self.bytes.capacity() {
            let target_capacity =
                bounded_reserve_capacity(self.bytes.capacity(), required_len, self.max_len);
            let additional_capacity = target_capacity - self.bytes.len();
            self.bytes
                .try_reserve_exact(additional_capacity)
                .map_err(|_| {
                    io::Error::new(
                        io::ErrorKind::OutOfMemory,
                        "failed to allocate the manifest output buffer",
                    )
                })?;
        }
        self.bytes.extend_from_slice(bytes);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn validate_fixture<'a>(
    fixture: &'a FixtureMetadata,
    previous_id: &mut Option<&'a str>,
    ids: &mut BTreeSet<&'a str>,
) -> Result<()> {
    let fixture_id = &fixture.id;
    validate_label("fixture id", &fixture.id)?;
    if let Some(previous) = *previous_id
        && fixture.id.as_str() <= previous
    {
        return Err(CompatError::InvalidManifest(
            "fixtures must be sorted by id and contain no duplicates".to_owned(),
        ));
    }
    *previous_id = Some(&fixture.id);
    if !ids.insert(&fixture.id) {
        return Err(CompatError::InvalidManifest(format!(
            "duplicate fixture id {fixture_id:?}"
        )));
    }
    validate_label("artifact logical name", &fixture.artifact.logical_name)?;
    if let Some(title_id) = &fixture.title_id
        && (title_id.len() != 16
            || !title_id
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)))
    {
        return Err(CompatError::InvalidManifest(format!(
            "fixture {fixture_id:?} has a non-canonical title id"
        )));
    }
    if let Some(version) = &fixture.version {
        validate_label("fixture version", version)?;
    }
    validate_fixture_status(fixture)?;
    if let Some(expected) = &fixture.expected {
        validate_expected_outcome(fixture, expected)?;
    }
    Ok(())
}

fn validate_fixture_status(fixture: &FixtureMetadata) -> Result<()> {
    let fixture_id = &fixture.id;
    match (fixture.status, fixture.expected.is_some()) {
        (FixtureStatus::Validated, false) => Err(CompatError::InvalidManifest(format!(
            "validated fixture {fixture_id:?} must declare expected oracle results"
        ))),
        (FixtureStatus::MetadataOnly, true) => Err(CompatError::InvalidManifest(format!(
            "metadata-only fixture {fixture_id:?} must not claim expected oracle results"
        ))),
        _ => Ok(()),
    }
}

fn validate_expected_outcome(fixture: &FixtureMetadata, expected: &ExpectedOutcome) -> Result<()> {
    let fixture_id = &fixture.id;
    if expected.checkpoints.is_empty() || expected.checkpoints.len() > MAX_CHECKPOINTS_PER_FIXTURE {
        return Err(CompatError::InvalidManifest(format!(
            "fixture {fixture_id:?} must declare 1..={MAX_CHECKPOINTS_PER_FIXTURE} expected checkpoints"
        )));
    }
    let mut previous_cycle = None;
    let mut checkpoint_names = BTreeSet::new();
    for checkpoint in &expected.checkpoints {
        let checkpoint_name = &checkpoint.name;
        validate_label("checkpoint", &checkpoint.name)?;
        if !checkpoint_names.insert(&checkpoint.name) {
            return Err(CompatError::InvalidManifest(format!(
                "fixture {fixture_id:?} repeats checkpoint {checkpoint_name:?}"
            )));
        }
        if previous_cycle.is_some_and(|cycle| checkpoint.guest_cycle.0 <= cycle) {
            return Err(CompatError::InvalidManifest(format!(
                "fixture {fixture_id:?} checkpoints must have strictly increasing guest cycles"
            )));
        }
        if checkpoint.guest_cycle.0 > expected.final_guest_cycle.0 {
            return Err(CompatError::InvalidManifest(format!(
                "fixture {fixture_id:?} has a checkpoint after its final guest cycle"
            )));
        }
        previous_cycle = Some(checkpoint.guest_cycle.0);
    }
    Ok(())
}

fn validate_label(kind: &str, value: &str) -> Result<()> {
    if value.is_empty()
        || value.len() > 128
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.' | b':'))
    {
        return Err(CompatError::InvalidManifest(format!(
            "{kind} must be a 1..=128 byte logical label without paths or whitespace"
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bounded_writer_accepts_exact_limit() {
        let mut writer = BoundedVecWriter::new(4);

        writer.write_all(b"test").unwrap();

        assert!(!writer.limit_exceeded());
        assert_eq!(writer.into_inner(), b"test");
    }

    #[test]
    fn bounded_writer_rejects_overflow_without_partial_write() {
        let mut writer = BoundedVecWriter::new(4);
        writer.write_all(b"abc").unwrap();

        let error = writer.write_all(b"de").unwrap_err();

        assert_eq!(error.kind(), io::ErrorKind::InvalidData);
        assert!(writer.limit_exceeded());
        assert_eq!(writer.into_inner(), b"abc");
    }

    #[test]
    fn bounded_length_rejects_integer_overflow() {
        assert_eq!(bounded_output_len(usize::MAX, 1, usize::MAX), None);
    }

    #[test]
    fn reserve_growth_saturates_and_stays_bounded() {
        assert_eq!(bounded_reserve_capacity(3, 4, 4), 4);
        assert_eq!(
            bounded_reserve_capacity(usize::MAX - 1, usize::MAX, usize::MAX),
            usize::MAX
        );
    }
}
