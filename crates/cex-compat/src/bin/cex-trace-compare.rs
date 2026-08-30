//! Compare two bounded canonical compatibility traces.
//!
//! The command deliberately exposes only path-free diagnostics so it can be
//! used by fixture runners without disclosing host filesystem layout.

use std::{
    env,
    ffi::{OsStr, OsString},
    fmt,
    fs::File,
    io::{self, BufReader, Write},
    path::{Path, PathBuf},
    process,
};

use cex_compat::{MAX_CANONICAL_LINE_BYTES, TraceDiff, TraceReader, diff_traces};
use serde::Serialize;
use tempfile::NamedTempFile;

const USAGE: &str = "usage: cex-trace-compare --expected FILE --actual FILE [--json-report FILE|-]";
const REPORT_SCHEMA_VERSION: u16 = 1;
const MAX_REPORT_BYTES: usize = 2 * MAX_CANONICAL_LINE_BYTES;

/// Parsed command-line arguments, kept separate from user-visible errors.
struct Arguments {
    expected: PathBuf,
    actual: PathBuf,
    json_report: Option<ReportDestination>,
}

/// Destination selected for the optional JSON comparison report.
enum ReportDestination {
    /// Send the report to standard output.
    StandardOutput,
    /// Publish a staged report without replacing an existing path.
    Path(PathBuf),
}

/// A path-free failure category and its corresponding stable process status.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum CompareError {
    /// The supplied options do not match the command contract.
    Usage,
    /// The expected input could not be opened or validated as a trace.
    ExpectedTrace,
    /// The actual input could not be opened or validated as a trace.
    ActualTrace,
    /// The report could not be serialized or safely published.
    Report,
}

impl fmt::Display for CompareError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Usage => formatter.write_str(USAGE),
            Self::ExpectedTrace => formatter.write_str("unable to read or validate expected trace"),
            Self::ActualTrace => formatter.write_str("unable to read or validate actual trace"),
            Self::Report => formatter.write_str("unable to write comparison report"),
        }
    }
}

impl std::error::Error for CompareError {}

/// Stable, compact JSON representation of one comparison result.
#[derive(Serialize)]
struct JsonReport<'a> {
    schema_version: u16,
    r#match: bool,
    compared_entries: usize,
    mismatch: &'a Option<cex_compat::TraceMismatch>,
}

/// A preallocated writer that refuses bytes beyond the report allocation cap.
struct BoundedReportWriter {
    bytes: Vec<u8>,
}

impl BoundedReportWriter {
    fn new() -> Result<Self, CompareError> {
        let mut bytes = Vec::new();
        bytes
            .try_reserve_exact(MAX_REPORT_BYTES)
            .map_err(|_| CompareError::Report)?;
        Ok(Self { bytes })
    }

    fn finish(mut self) -> Result<Vec<u8>, CompareError> {
        if self.bytes.len() >= MAX_REPORT_BYTES {
            return Err(CompareError::Report);
        }
        self.bytes.push(b'\n');
        Ok(self.bytes)
    }
}

impl Write for BoundedReportWriter {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> {
        let remaining = MAX_REPORT_BYTES.saturating_sub(self.bytes.len());
        if buffer.len() > remaining {
            return Err(io::Error::new(
                io::ErrorKind::WriteZero,
                "comparison report exceeds its byte limit",
            ));
        }
        self.bytes.extend_from_slice(buffer);
        Ok(buffer.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn main() {
    let exit_code = match run(env::args_os()) {
        Ok(exit_code) => exit_code,
        Err(error) => {
            eprintln!("{error}");
            2
        }
    };
    process::exit(exit_code);
}

fn run(arguments: impl IntoIterator<Item = OsString>) -> Result<i32, CompareError> {
    let arguments = parse_arguments(arguments)?;
    let expected = read_trace(&arguments.expected, CompareError::ExpectedTrace)?;
    let actual = read_trace(&arguments.actual, CompareError::ActualTrace)?;
    let diff = diff_traces(&expected, &actual);

    if let Some(destination) = &arguments.json_report {
        let report = serialize_report(&diff)?;
        write_report(destination, &report)?;
    }

    Ok(i32::from(!diff.is_match()))
}

fn parse_arguments(
    arguments: impl IntoIterator<Item = OsString>,
) -> Result<Arguments, CompareError> {
    let mut arguments = arguments.into_iter();
    let _program = arguments.next();
    let mut expected = None;
    let mut actual = None;
    let mut json_report = None;

    while let Some(option) = arguments.next() {
        let value = arguments.next().ok_or(CompareError::Usage)?;
        if option == OsStr::new("--expected") && expected.is_none() {
            expected = Some(PathBuf::from(value));
        } else if option == OsStr::new("--actual") && actual.is_none() {
            actual = Some(PathBuf::from(value));
        } else if option == OsStr::new("--json-report") && json_report.is_none() {
            json_report = Some(if value == OsStr::new("-") {
                ReportDestination::StandardOutput
            } else {
                ReportDestination::Path(PathBuf::from(value))
            });
        } else {
            return Err(CompareError::Usage);
        }
    }

    match (expected, actual) {
        (Some(expected), Some(actual)) => Ok(Arguments {
            expected,
            actual,
            json_report,
        }),
        _ => Err(CompareError::Usage),
    }
}

fn read_trace(
    path: &Path,
    failure: CompareError,
) -> Result<Vec<cex_compat::TraceEntry>, CompareError> {
    let file = File::open(path).map_err(|_| failure)?;
    TraceReader::new(BufReader::new(file))
        .read_all()
        .map_err(|_| failure)
}

fn serialize_report(diff: &TraceDiff) -> Result<Vec<u8>, CompareError> {
    let mut report = BoundedReportWriter::new()?;
    serde_json::to_writer(
        &mut report,
        &JsonReport {
            schema_version: REPORT_SCHEMA_VERSION,
            r#match: diff.is_match(),
            compared_entries: diff.compared_entries,
            mismatch: &diff.mismatch,
        },
    )
    .map_err(|_| CompareError::Report)?;
    report.finish()
}

fn write_report(destination: &ReportDestination, report: &[u8]) -> Result<(), CompareError> {
    match destination {
        ReportDestination::StandardOutput => {
            let mut stdout = io::stdout().lock();
            stdout.write_all(report).map_err(|_| CompareError::Report)?;
            stdout.flush().map_err(|_| CompareError::Report)
        }
        ReportDestination::Path(path) => publish_report(path, report),
    }
}

/// Publishes a synced staged report without ever replacing a destination.
///
/// The initial Linux target stages a randomly named mode-0600 file in the
/// destination directory, then uses [`NamedTempFile::persist_noclobber`] for
/// no-replacement publication. This path-based tempfile API makes no universal
/// atomicity or pathname-substitution guarantee beyond that platform contract.
/// The report parent is a caller-owned safety boundary; an attacker-controlled
/// parent directory is outside this command's security guarantees.
fn publish_report(path: &Path, report: &[u8]) -> Result<(), CompareError> {
    let parent = report_parent(path);
    let mut staged = NamedTempFile::new_in(parent).map_err(|_| CompareError::Report)?;
    if staged.as_file_mut().write_all(report).is_err() || staged.as_file().sync_all().is_err() {
        return Err(CompareError::Report);
    }

    staged
        .persist_noclobber(path)
        .map_err(|_| CompareError::Report)?;
    // Do not remove `path` after this point: another actor may already have
    // observed it. A failed directory sync is reported as a failure, while
    // the complete report remains available rather than being overwritten.
    if sync_directory(parent).is_err() {
        return Err(CompareError::Report);
    }

    Ok(())
}

fn report_parent(path: &Path) -> &Path {
    path.parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."))
}

/// Syncs the report directory after publication for the current Linux x86_64 milestone.
///
/// If a future Windows build cannot open and sync a directory, a complete
/// report may already exist but the command still exits with status 2. A
/// Windows durability adapter is therefore a platform-milestone gate rather
/// than a best-effort fallback.
fn sync_directory(path: &Path) -> io::Result<()> {
    File::open(path)?.sync_all()
}

#[cfg(test)]
mod tests {
    use std::{
        fs,
        io::Cursor,
        path::PathBuf,
        process,
        sync::atomic::{AtomicU64, Ordering},
    };

    use cex_compat::{
        HexU32, StopReason, TerminalEvent, TraceCategory, TraceEntry, TraceEvent, TraceMismatch,
        TraceMismatchKind, TraceSource, TraceWriter,
    };

    use super::{
        CompareError, MAX_REPORT_BYTES, ReportDestination, TraceDiff, parse_arguments,
        publish_report, serialize_report,
    };

    static TEST_COUNTER: AtomicU64 = AtomicU64::new(0);

    fn terminal(cycle: u64, exit_code: u32) -> TraceEntry {
        TraceEntry::new(
            cycle,
            0,
            TraceSource::TestHarness,
            None,
            TraceCategory::Terminal,
            TraceEvent::Terminal(TerminalEvent {
                reason: StopReason::TestCompleted,
                guest_exit_code: Some(HexU32(exit_code)),
                detail_code: Some("synthetic_complete".to_owned()),
            }),
        )
    }

    fn trace(entry: &TraceEntry) -> Vec<u8> {
        let mut writer = TraceWriter::new(Vec::new());
        writer.write_entry(entry).unwrap();
        writer.finish().unwrap()
    }

    fn temporary_path(label: &str) -> PathBuf {
        let sequence = TEST_COUNTER.fetch_add(1, Ordering::Relaxed);
        std::env::temp_dir().join(format!(
            "cex-trace-compare-{label}-{}-{sequence}.json",
            process::id()
        ))
    }

    #[test]
    fn argument_errors_do_not_disclose_paths() {
        let secret_path = "/private/expected-secret.trace";
        let result = parse_arguments([
            "cex-trace-compare".into(),
            "--expected".into(),
            secret_path.into(),
        ]);
        let Err(error) = result else {
            panic!("missing required argument must fail");
        };

        assert_eq!(error, CompareError::Usage);
        assert!(!error.to_string().contains(secret_path));
        assert!(!format!("{error:?}").contains(secret_path));
    }

    #[test]
    fn report_is_compact_and_has_a_stable_schema() {
        let report = serialize_report(&TraceDiff {
            compared_entries: 1,
            mismatch: None,
        })
        .unwrap();

        assert_eq!(
            report,
            b"{\"schema_version\":1,\"match\":true,\"compared_entries\":1,\"mismatch\":null}\n"
        );
    }

    #[test]
    fn report_rejects_a_trace_diff_larger_than_its_byte_limit() {
        let diff = TraceDiff {
            compared_entries: 0,
            mismatch: Some(TraceMismatch {
                kind: TraceMismatchKind::DivergentValue,
                expected_index: 0,
                actual_index: 0,
                expected_key: None,
                actual_key: None,
                path: "x".repeat(MAX_REPORT_BYTES),
                expected: None,
                actual: None,
                hint: "synthetic oversized report".to_owned(),
            }),
        };

        assert_eq!(serialize_report(&diff).unwrap_err(), CompareError::Report);
    }

    #[test]
    fn identical_and_different_traces_produce_expected_reports() {
        let expected = trace(&terminal(1, 0));
        let actual = trace(&terminal(1, 0));
        let expected_path = temporary_path("expected");
        let actual_path = temporary_path("actual");
        fs::write(&expected_path, &expected).unwrap();
        fs::write(&actual_path, &actual).unwrap();
        let identical_status = super::run([
            "cex-trace-compare".into(),
            "--expected".into(),
            expected_path.clone().into_os_string(),
            "--actual".into(),
            actual_path.clone().into_os_string(),
        ])
        .unwrap();
        assert_eq!(identical_status, 0);

        let expected_entries = cex_compat::TraceReader::new(Cursor::new(expected))
            .read_all()
            .unwrap();
        let actual_entries = cex_compat::TraceReader::new(Cursor::new(actual))
            .read_all()
            .unwrap();
        let identical = cex_compat::diff_traces(&expected_entries, &actual_entries);
        assert!(identical.is_match());

        let different_actual = trace(&terminal(1, 1));
        fs::write(&actual_path, &different_actual).unwrap();
        let mismatch_status = super::run([
            "cex-trace-compare".into(),
            "--expected".into(),
            expected_path.clone().into_os_string(),
            "--actual".into(),
            actual_path.clone().into_os_string(),
        ])
        .unwrap();
        assert_eq!(mismatch_status, 1);
        let different_entries = cex_compat::TraceReader::new(Cursor::new(different_actual))
            .read_all()
            .unwrap();
        let different = cex_compat::diff_traces(&expected_entries, &different_entries);
        assert!(!different.is_match());
        let report: serde_json::Value =
            serde_json::from_slice(&serialize_report(&different).unwrap()).unwrap();
        assert_eq!(report["schema_version"], 1);
        assert_eq!(report["match"], false);
        assert!(report["mismatch"].is_object());
        fs::remove_file(expected_path).unwrap();
        fs::remove_file(actual_path).unwrap();
    }

    #[test]
    fn invalid_schema_is_reported_without_disclosing_its_path() {
        let invalid = temporary_path("invalid-schema");
        let bytes = String::from_utf8(trace(&terminal(1, 0)))
            .unwrap()
            .replacen("\"schema_version\":1", "\"schema_version\":2", 1)
            .into_bytes();
        fs::write(&invalid, bytes).unwrap();

        let error = super::read_trace(&invalid, CompareError::ExpectedTrace).unwrap_err();
        assert_eq!(error, CompareError::ExpectedTrace);
        assert!(
            !error
                .to_string()
                .contains(invalid.to_string_lossy().as_ref())
        );
        assert!(!format!("{error:?}").contains(invalid.to_string_lossy().as_ref()));
        fs::remove_file(invalid).unwrap();
    }

    #[test]
    fn existing_report_is_preserved_and_staging_is_cleaned_up() {
        let directory = tempfile::tempdir().unwrap();
        let report_path = directory.path().join("existing-report.json");
        fs::write(&report_path, b"already here\n").unwrap();

        let error = publish_report(&report_path, b"replacement\n").unwrap_err();
        assert_eq!(error, CompareError::Report);
        assert_eq!(fs::read(&report_path).unwrap(), b"already here\n");
        assert_eq!(fs::read_dir(directory.path()).unwrap().count(), 1);
    }

    #[cfg(unix)]
    #[test]
    fn published_report_has_owner_only_permissions() {
        use std::os::unix::fs::PermissionsExt;

        let directory = tempfile::tempdir().unwrap();
        let report_path = directory.path().join("private-report.json");
        publish_report(&report_path, b"{}\n").unwrap();

        assert_eq!(
            fs::metadata(report_path).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }

    #[cfg(unix)]
    #[test]
    fn dangling_report_symlink_is_rejected_without_creating_its_target() {
        use std::os::unix::fs::symlink;

        let directory = tempfile::tempdir().unwrap();
        let report_path = directory.path().join("report.json");
        let outside_sentinel = directory.path().join("outside-sentinel.json");
        symlink(&outside_sentinel, &report_path).unwrap();

        let error = publish_report(&report_path, b"{}\n").unwrap_err();
        assert_eq!(error, CompareError::Report);
        assert!(
            fs::symlink_metadata(&report_path)
                .unwrap()
                .file_type()
                .is_symlink()
        );
        assert_eq!(fs::read_link(&report_path).unwrap(), outside_sentinel);
        assert!(!outside_sentinel.exists());
        assert_eq!(fs::read_dir(directory.path()).unwrap().count(), 1);
    }

    #[test]
    fn report_destination_dash_selects_standard_output() {
        let arguments = parse_arguments([
            "cex-trace-compare".into(),
            "--actual".into(),
            "actual.trace".into(),
            "--expected".into(),
            "expected.trace".into(),
            "--json-report".into(),
            "-".into(),
        ])
        .unwrap();

        assert!(matches!(
            arguments.json_report,
            Some(ReportDestination::StandardOutput)
        ));
    }
}
