//! Durable-data migration contracts for the CemuExtend Rust rewrite.
//!
//! This crate defines the planned `inspect`, `copy`, `apply`, and `rollback`
//! operations. It performs no filesystem mutation and does not yet serialize
//! the JSON report.

use core::fmt;
use std::path::PathBuf;

/// A migration feature exposed by the planned command-line tool.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(usize)]
pub enum MigrationCapability {
    /// Read-only source inspection.
    Inspect,
    /// Copying data into the isolated preview directory.
    Copy,
    /// Atomic migration with an original-data backup.
    Apply,
    /// Restoration from a migration backup.
    Rollback,
    /// Machine-readable JSON reporting.
    JsonReport,
}

const CAPABILITY_COUNT: usize = 5;

/// The implementation status of a migration capability.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CapabilityState {
    /// The Rust rewrite has not implemented the capability.
    #[default]
    NotImplemented,
    /// An implementation exists, but is unavailable in the current runtime.
    Unavailable,
    /// The current migration engine can provide the capability.
    Available,
}

/// An immutable snapshot of migration capabilities.
///
/// Keep the returned value when constructing or updating this snapshot; the
/// type models an immutable report and discarding it would drop the change.
#[must_use = "MigrationCapabilities is an immutable snapshot; keep the returned value."]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MigrationCapabilities {
    states: [CapabilityState; CAPABILITY_COUNT],
}

impl MigrationCapabilities {
    /// Returns a report in which every migration feature is unimplemented.
    pub const fn none() -> Self {
        Self {
            states: [CapabilityState::NotImplemented; CAPABILITY_COUNT],
        }
    }

    /// Returns a copy of this report with one capability state changed.
    pub const fn with_state(
        mut self,
        capability: MigrationCapability,
        state: CapabilityState,
    ) -> Self {
        self.states[capability as usize] = state;
        self
    }

    /// Returns the state of one capability.
    pub const fn state(self, capability: MigrationCapability) -> CapabilityState {
        self.states[capability as usize]
    }
}

impl Default for MigrationCapabilities {
    fn default() -> Self {
        Self::none()
    }
}

/// Migration operation selected by the command-line interface.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MigrationMode {
    /// Inspect the source and report required changes without writing files.
    Inspect,
    /// Copy source data into the isolated Rust preview directory.
    Copy,
    /// Apply the final migration after creating a recoverable backup.
    Apply,
    /// Restore the original data from a previous migration backup.
    Rollback,
}

/// One migration operation and its explicitly resolved paths.
#[derive(Clone, Eq, PartialEq)]
pub struct MigrationRequest {
    /// Selected operation.
    pub mode: MigrationMode,
    /// Existing CemuExtend data directory.
    pub source: PathBuf,
    /// Rust preview or final destination directory.
    pub destination: PathBuf,
}

impl fmt::Debug for MigrationRequest {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("MigrationRequest")
            .field("mode", &self.mode)
            .field("source_component_count", &self.source.components().count())
            .field("source_is_absolute", &self.source.is_absolute())
            .field(
                "destination_component_count",
                &self.destination.components().count(),
            )
            .field("destination_is_absolute", &self.destination.is_absolute())
            .finish()
    }
}

/// Machine-readable outcome category for a migration report.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MigrationOutcome {
    /// Inspection completed and no writes were attempted.
    Inspected,
    /// The requested mutation completed successfully.
    Applied,
    /// The request made no changes because it was already satisfied.
    NoChanges,
}

/// Redacted action category safe for machine-readable reports.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MigrationActionCode {
    /// The source layout was inspected without mutation.
    SourceInspected,
    /// One data item was copied into the preview location.
    DataCopied,
    /// A recoverable original-data backup was created.
    BackupCreated,
    /// One destination item was atomically replaced.
    DataReplaced,
    /// Original data was atomically restored from a backup.
    BackupRestored,
}

/// Redacted warning category safe for machine-readable reports.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MigrationWarningCode {
    /// Unknown XML content was retained unchanged.
    UnknownXmlPreserved,
    /// Optional data could not be migrated without reducing core compatibility.
    OptionalDataSkipped,
    /// The request was already satisfied and made no changes.
    AlreadyApplied,
}

/// Structured report intended for future JSON serialization.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MigrationReport {
    /// Overall operation outcome.
    pub outcome: MigrationOutcome,
    /// Redacted actions in deterministic execution order.
    pub actions: Vec<MigrationActionCode>,
    /// Redacted non-fatal compatibility or data-quality warnings.
    pub warnings: Vec<MigrationWarningCode>,
}

/// Boundary implemented by the future durable-data migration engine.
pub trait MigrationEngine {
    /// Migration-specific validation or filesystem failure.
    type Error;

    /// Reports migration operations available in this build.
    fn capabilities(&self) -> MigrationCapabilities;

    /// Executes an explicitly selected migration request.
    ///
    /// Implementations must preserve unknown XML and be idempotent. `Copy`
    /// must leave the source unchanged and atomically populate an isolated
    /// preview destination. `Apply` must create a recoverable backup before
    /// atomically replacing data. `Rollback` must validate its backup and
    /// restore atomically without deleting that backup until success. All
    /// mutating modes must reject equal, broad, unresolved, or unauthorized
    /// source and destination paths before the first write.
    fn execute(&mut self, request: &MigrationRequest) -> Result<MigrationReport, Self::Error>;
}

#[cfg(test)]
mod tests {
    use super::{
        CapabilityState, MigrationCapabilities, MigrationCapability, MigrationMode,
        MigrationRequest,
    };
    use std::path::PathBuf;

    #[test]
    fn default_report_does_not_claim_migration_support() {
        let report = MigrationCapabilities::default();

        assert_eq!(
            report.state(MigrationCapability::Inspect),
            CapabilityState::NotImplemented
        );
        assert_eq!(
            report.state(MigrationCapability::Rollback),
            CapabilityState::NotImplemented
        );
    }

    #[test]
    fn migration_request_debug_redacts_paths() {
        let request = MigrationRequest {
            mode: MigrationMode::Copy,
            source: PathBuf::from("source/secret-source"),
            destination: PathBuf::from("destination/private-destination"),
        };

        let debug = format!("{request:?}");

        assert!(debug.contains("MigrationRequest {"));
        assert!(debug.contains("mode: Copy"));
        assert!(debug.contains("source_component_count: 2"));
        assert!(debug.contains("destination_component_count: 2"));
        assert!(debug.contains("source_is_absolute: false"));
        assert!(debug.contains("destination_is_absolute: false"));
        assert!(!debug.contains("secret-source"));
        assert!(!debug.contains("private-destination"));
        assert!(!debug.contains("source/"));
    }
}
