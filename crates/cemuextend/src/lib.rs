//! Command-line entrypoint for the first deterministic Rust vertical slice.

use std::collections::BTreeMap;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::Path;

use cex_compat::{
    ArchitecturalFaultEvent, CpuStateEvent, FaultKind as TraceFaultKind, GenericEvent, HexU32,
    HexU64, MemoryHashAlgorithm, MemoryHashEvent, ReservationEvent, Sha256Digest,
    StopReason as TraceStopReason, TerminalEvent, TraceCategory, TraceEntry, TraceEvent,
    TraceSource, TraceValue, TraceWriter,
};
use cex_cpu::{BudgetKind, FaultKind as CpuFaultKind, StopReason};
use cex_system::{
    BUILTIN_FIXTURE_NAME, BUILTIN_RPX_FIXTURE_NAME, HeadlessRun, HeadlessSystem,
    MAX_RPX_IMAGE_SIZE, MAX_SYNTHETIC_IMAGE_SIZE, ProgramDecodeError, SyntheticProgram,
    builtin_fixture, builtin_rpx_fixture,
};
use thiserror::Error;

const DEFAULT_BUDGET: u64 = 10_000;
const FIXTURE_READ_CHUNK: usize = 8 * 1024;

/// Stable CLI help for the synthetic headless milestone.
pub const HELP: &str = "Cemu Rust headless compatibility runner\n\
\n\
Usage:\n\
  Cemu --headless-fixture <synthetic-boot|synthetic-rpx-boot|CEXH_FILE|RPX_FILE> [OPTIONS]\n\
\n\
Options:\n\
  --trace-output <FILE|->       Canonical JSONL destination (default: -)\n\
  --instruction-budget <COUNT>  Maximum retired instructions (default: 10000)\n\
  --cycle-budget <COUNT>        Maximum guest cycles (default: 10000)\n\
  -h, --help                    Print this help\n\
  -V, --version                 Print the version\n\
\n\
The RPX slice accepts only self-contained, uncompressed main RPX images;\n\
imports, relocations, compression, and RPL modules are unsupported.\n";

/// Parse and run the Cemu command, returning its process exit code.
pub fn run_cli<I, T>(args: I) -> Result<u8, CliError>
where
    I: IntoIterator<Item = T>,
    T: Into<OsString>,
{
    match Options::parse(args)? {
        Invocation::Help => {
            print!("{HELP}");
            Ok(0)
        }
        Invocation::Version => {
            println!("Cemu {}", env!("CARGO_PKG_VERSION"));
            Ok(0)
        }
        Invocation::Run(options) => run_headless(&options),
    }
}

fn run_headless(options: &Options) -> Result<u8, CliError> {
    validate_trace_destination(options)?;

    let artifact = match load_artifact(&options.fixture) {
        Ok(artifact) => artifact,
        Err(error) => {
            let trace = failure_trace("fixture-load-failed")?;
            publish_trace(&options.trace_output, &trace)?;
            return Err(error);
        }
    };
    let system = HeadlessSystem::with_budget(options.instruction_budget, options.cycle_budget)?;
    let source_label = artifact.source_label();
    let event_name = artifact.event_name();
    let run = match artifact.run(&system) {
        Ok(run) => run,
        Err(error) => {
            let trace = failure_trace("headless-execution-failed")?;
            publish_trace(&options.trace_output, &trace)?;
            return Err(error.into());
        }
    };

    let mut trace = TraceWriter::new(Vec::new());
    write_success_trace(&mut trace, &run, source_label, event_name)?;
    let trace = trace.finish()?;
    publish_trace(&options.trace_output, &trace)?;
    Ok(exit_code(&run))
}

fn validate_trace_destination(options: &Options) -> Result<(), CliError> {
    if options.trace_output == OsStr::new("-") {
        return Ok(());
    }
    let output = Path::new(&options.trace_output);
    if !is_builtin_fixture(&options.fixture) && output == Path::new(&options.fixture) {
        return Err(CliError::TraceEqualsFixture);
    }
    match fs::symlink_metadata(output) {
        Ok(_) => Err(CliError::TraceDestinationExists),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(CliError::TraceOutput(error)),
    }
}

fn publish_trace(destination: &OsStr, bytes: &[u8]) -> Result<(), CliError> {
    if destination == OsStr::new("-") {
        let stdout = io::stdout();
        let mut output = stdout.lock();
        output.write_all(bytes).map_err(CliError::TraceOutput)?;
        output.flush().map_err(CliError::TraceOutput)?;
        return Ok(());
    }

    let destination = Path::new(destination);
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let mut staged = tempfile::NamedTempFile::new_in(parent).map_err(CliError::TraceOutput)?;
    staged
        .as_file_mut()
        .write_all(bytes)
        .map_err(CliError::TraceOutput)?;
    staged
        .as_file_mut()
        .flush()
        .map_err(CliError::TraceOutput)?;
    staged.as_file().sync_all().map_err(CliError::TraceOutput)?;

    let published = staged.persist_noclobber(destination).map_err(|error| {
        if error.error.kind() == io::ErrorKind::AlreadyExists {
            CliError::TraceDestinationExists
        } else {
            CliError::TraceOutput(error.error)
        }
    })?;
    published.sync_all().map_err(CliError::TraceOutput)?;
    File::open(parent)
        .and_then(|directory| directory.sync_all())
        .map_err(CliError::TraceOutput)?;
    Ok(())
}

fn is_builtin_fixture(source: &OsStr) -> bool {
    source == OsStr::new(BUILTIN_FIXTURE_NAME) || source == OsStr::new(BUILTIN_RPX_FIXTURE_NAME)
}

enum LoadedArtifact {
    BuiltinCexh(SyntheticProgram),
    ExternalCexh(SyntheticProgram),
    BuiltinRpx(Vec<u8>),
    ExternalRpx(Vec<u8>),
}

impl LoadedArtifact {
    const fn source_label(&self) -> &'static str {
        match self {
            Self::BuiltinCexh(_) => "synthetic-cexh-v1",
            Self::ExternalCexh(_) => "external-cexh-v1",
            Self::BuiltinRpx(_) => "synthetic-rpx-v1",
            Self::ExternalRpx(_) => "external-rpx-v1",
        }
    }

    const fn event_name(&self) -> &'static str {
        match self {
            Self::BuiltinCexh(_) | Self::ExternalCexh(_) => "synthetic-fixture-loaded",
            Self::BuiltinRpx(_) | Self::ExternalRpx(_) => "rpx-fixture-loaded",
        }
    }

    fn run(self, system: &HeadlessSystem) -> Result<HeadlessRun, cex_system::HeadlessError> {
        match self {
            Self::BuiltinCexh(program) | Self::ExternalCexh(program) => system.run(&program),
            Self::BuiltinRpx(image) | Self::ExternalRpx(image) => system.run_rpx(&image),
        }
    }
}

fn load_artifact(source: &OsStr) -> Result<LoadedArtifact, CliError> {
    if source == OsStr::new(BUILTIN_FIXTURE_NAME) {
        let bytes = builtin_fixture().encode()?;
        return Ok(LoadedArtifact::BuiltinCexh(SyntheticProgram::decode(
            &bytes,
        )?));
    }
    if source == OsStr::new(BUILTIN_RPX_FIXTURE_NAME) {
        return Ok(LoadedArtifact::BuiltinRpx(builtin_rpx_fixture()));
    }

    let bytes = read_bounded(Path::new(source), MAX_RPX_IMAGE_SIZE)?;
    if bytes.starts_with(b"\x7fELF") {
        return Ok(LoadedArtifact::ExternalRpx(bytes));
    }
    if bytes.len() > MAX_SYNTHETIC_IMAGE_SIZE {
        return Err(CliError::SyntheticFixtureTooLarge);
    }
    Ok(LoadedArtifact::ExternalCexh(SyntheticProgram::decode(
        &bytes,
    )?))
}

fn read_bounded(path: &Path, maximum: usize) -> Result<Vec<u8>, CliError> {
    let mut file = File::open(path).map_err(CliError::FixtureIo)?;
    read_bounded_from_reader(&mut file, maximum)
}

fn read_bounded_from_reader(reader: &mut impl Read, maximum: usize) -> Result<Vec<u8>, CliError> {
    let limit = bounded_limit(maximum)?;
    let mut bytes = Vec::new();
    let mut chunk = [0u8; FIXTURE_READ_CHUNK];

    loop {
        if bytes.len() == limit {
            return Err(CliError::FixtureTooLarge);
        }

        let read_limit = (limit - bytes.len()).min(chunk.len());
        let read = match reader.read(&mut chunk[..read_limit]) {
            Ok(read) => read,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(CliError::FixtureIo(error)),
        };

        if read == 0 {
            break;
        }

        reserve_for_append(&mut bytes, read, limit)?;
        bytes.extend_from_slice(&chunk[..read]);
    }

    if bytes.len() > maximum {
        return Err(CliError::FixtureTooLarge);
    }
    Ok(bytes)
}

fn bounded_limit(maximum: usize) -> Result<usize, CliError> {
    maximum
        .checked_add(1)
        .ok_or(CliError::FixtureAllocationFailed { requested: maximum })
}

fn reserve_for_append(
    bytes: &mut Vec<u8>,
    additional: usize,
    limit: usize,
) -> Result<(), CliError> {
    if let Some(target_capacity) = planned_capacity(
        bytes.len(),
        bytes.capacity(),
        additional,
        FIXTURE_READ_CHUNK,
        limit,
    )? {
        bytes
            .try_reserve_exact(target_capacity - bytes.len())
            .map_err(|_| CliError::FixtureAllocationFailed {
                requested: target_capacity,
            })?;
    }
    Ok(())
}

fn planned_capacity(
    len: usize,
    capacity: usize,
    additional: usize,
    chunk_size: usize,
    limit: usize,
) -> Result<Option<usize>, CliError> {
    let required = len
        .checked_add(additional)
        .ok_or(CliError::FixtureAllocationFailed {
            requested: usize::MAX,
        })?;

    if required <= capacity {
        return Ok(None);
    }
    if required > limit {
        return Err(CliError::FixtureTooLarge);
    }

    let doubled_capacity = capacity.saturating_mul(2);
    let target = required.max(chunk_size.max(doubled_capacity)).min(limit);
    Ok(Some(target))
}

fn failure_trace(detail_code: &str) -> Result<Vec<u8>, CliError> {
    let mut trace = TraceWriter::new(Vec::new());
    trace.write_entry(&TraceEntry::new(
        0,
        0,
        TraceSource::TestHarness,
        None,
        TraceCategory::Terminal,
        TraceEvent::Terminal(TerminalEvent {
            reason: TraceStopReason::HostError,
            guest_exit_code: None,
            detail_code: Some(detail_code.to_owned()),
        }),
    ))?;
    Ok(trace.finish()?)
}

fn write_success_trace<W: Write>(
    trace: &mut TraceWriter<W>,
    run: &HeadlessRun,
    source_label: &str,
    event_name: &str,
) -> Result<(), CliError> {
    let cycle = run.final_state.cycles.get();
    let mut fields = BTreeMap::new();
    fields.insert(
        "fixture".to_owned(),
        TraceValue::Text(source_label.to_owned()),
    );
    fields.insert(
        "program_sha256".to_owned(),
        TraceValue::Sha256(digest(run.program_hash)?),
    );
    trace.write_entry(&TraceEntry::new(
        0,
        0,
        TraceSource::TestHarness,
        None,
        TraceCategory::System,
        TraceEvent::Event(GenericEvent {
            name: event_name.to_owned(),
            fields,
        }),
    ))?;

    let state = &run.final_state;
    trace.write_entry(&TraceEntry::new(
        cycle,
        0,
        TraceSource::Cpu,
        Some(0),
        TraceCategory::Cpu,
        TraceEvent::CpuState(Box::new(CpuStateEvent {
            pc: HexU32::from(state.instruction_pointer.get()),
            gpr: state.gprs.map(HexU32::from),
            cr: HexU32::from(state.condition_register),
            lr: HexU32::from(state.link_register),
            ctr: HexU32::from(state.count_register),
            xer: HexU32::from(state.xer),
            fpr_bits: state.fprs.map(|lanes| lanes.map(HexU64::from)),
            fpscr: HexU32::from(state.fpscr),
            ugqr: state.ugqr.map(HexU32::from),
            reservation: state.reservation.map(|reservation| ReservationEvent {
                address: HexU32::from(reservation.address.get()),
                value: HexU32::from(reservation.value),
            }),
            pending_fault: state.pending_fault.map(|fault| ArchitecturalFaultEvent {
                instruction_pointer: HexU32::from(fault.instruction_pointer.get()),
                address: fault.address.map(|address| HexU32::from(address.get())),
                kind: trace_fault_kind(fault.kind),
            }),
            instructions_retired: state.instructions_retired.into(),
        })),
    ))?;
    trace.write_entry(&TraceEntry::new(
        cycle,
        1,
        TraceSource::Memory,
        None,
        TraceCategory::Memory,
        TraceEvent::MemoryHash(MemoryHashEvent {
            range: "memory-map-permissions-and-nonzero-pages-v1".to_owned(),
            guest_address: HexU32::from(0),
            byte_length: run
                .mapped_page_count
                .saturating_mul(cex_memory_page_size())
                .into(),
            algorithm: MemoryHashAlgorithm::Sha256V1,
            digest: digest(run.memory_hash)?,
        }),
    ))?;

    let (reason, guest_exit_code, detail_code) = terminal_for(run);
    trace.write_entry(&TraceEntry::new(
        cycle,
        2,
        TraceSource::TestHarness,
        None,
        TraceCategory::Terminal,
        TraceEvent::Terminal(TerminalEvent {
            reason,
            guest_exit_code,
            detail_code,
        }),
    ))?;
    Ok(())
}

fn terminal_for(run: &HeadlessRun) -> (TraceStopReason, Option<HexU32>, Option<String>) {
    match run.outcome.reason {
        StopReason::StopSentinel => (
            TraceStopReason::TestCompleted,
            None,
            Some("stop-sentinel".to_owned()),
        ),
        StopReason::SystemCall { number } => (
            TraceStopReason::GuestExit,
            run.final_state.gpr(3).map(HexU32::from),
            Some(format!("system-call-{number}")),
        ),
        StopReason::BudgetExhausted {
            kind: BudgetKind::Instructions,
        } => (
            TraceStopReason::InstructionLimit,
            None,
            Some("instruction-budget".to_owned()),
        ),
        StopReason::BudgetExhausted {
            kind: BudgetKind::Cycles,
        } => (
            TraceStopReason::CycleLimit,
            None,
            Some("cycle-budget".to_owned()),
        ),
    }
}

const fn cex_memory_page_size() -> u64 {
    4 * 1024
}

const fn trace_fault_kind(kind: CpuFaultKind) -> TraceFaultKind {
    match kind {
        CpuFaultKind::InstructionFetch => TraceFaultKind::InstructionFetch,
        CpuFaultKind::DataRead => TraceFaultKind::DataRead,
        CpuFaultKind::DataWrite => TraceFaultKind::DataWrite,
        CpuFaultKind::Alignment => TraceFaultKind::Alignment,
        CpuFaultKind::UnsupportedInstruction => TraceFaultKind::UnsupportedInstruction,
        CpuFaultKind::CounterOverflow => TraceFaultKind::CounterOverflow,
    }
}

fn exit_code(run: &HeadlessRun) -> u8 {
    match run.outcome.reason {
        StopReason::StopSentinel | StopReason::SystemCall { .. } => 0,
        StopReason::BudgetExhausted { .. } => 2,
    }
}

fn digest(bytes: [u8; 32]) -> Result<Sha256Digest, CliError> {
    let mut text = String::with_capacity(64);
    for byte in bytes {
        use std::fmt::Write as _;
        write!(&mut text, "{byte:02x}").expect("writing to String cannot fail");
    }
    Ok(Sha256Digest::parse(text)?)
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Options {
    fixture: OsString,
    trace_output: OsString,
    instruction_budget: u64,
    cycle_budget: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
enum Invocation {
    Help,
    Version,
    Run(Options),
}

impl Options {
    fn parse<I, T>(args: I) -> Result<Invocation, CliError>
    where
        I: IntoIterator<Item = T>,
        T: Into<OsString>,
    {
        let mut args = args.into_iter().map(Into::into);
        let _program_name = args.next();
        let mut fixture = None;
        let mut trace_output = OsString::from("-");
        let mut instruction_budget = DEFAULT_BUDGET;
        let mut cycle_budget = DEFAULT_BUDGET;

        while let Some(argument) = args.next() {
            match argument.to_str() {
                Some("-h" | "--help") => return Ok(Invocation::Help),
                Some("-V" | "--version") => return Ok(Invocation::Version),
                Some("--headless-fixture") => {
                    fixture = Some(next_value(&mut args, "--headless-fixture")?);
                }
                Some("--trace-output") => {
                    trace_output = next_value(&mut args, "--trace-output")?;
                }
                Some("--instruction-budget") => {
                    let value = next_value(&mut args, "--instruction-budget")?;
                    instruction_budget = parse_budget(&value, "--instruction-budget")?;
                }
                Some("--cycle-budget") => {
                    let value = next_value(&mut args, "--cycle-budget")?;
                    cycle_budget = parse_budget(&value, "--cycle-budget")?;
                }
                _ => return Err(CliError::UnknownArgument),
            }
        }

        Ok(Invocation::Run(Options {
            fixture: fixture.ok_or(CliError::MissingFixture)?,
            trace_output,
            instruction_budget,
            cycle_budget,
        }))
    }
}

fn next_value(
    args: &mut impl Iterator<Item = OsString>,
    option: &'static str,
) -> Result<OsString, CliError> {
    args.next().ok_or(CliError::MissingValue(option))
}

fn parse_budget(value: &OsString, option: &'static str) -> Result<u64, CliError> {
    let parsed = value
        .to_str()
        .and_then(|text| text.parse::<u64>().ok())
        .filter(|value| *value != 0)
        .ok_or(CliError::InvalidBudget(option))?;
    Ok(parsed)
}

/// Command-line, I/O, fixture, trace, or headless execution failure.
#[derive(Debug, Error)]
pub enum CliError {
    /// A required fixture selector was omitted.
    #[error("--headless-fixture is required; use --help for usage")]
    MissingFixture,
    /// An option requiring a value appeared at the end of argv.
    #[error("{0} requires a value")]
    MissingValue(&'static str),
    /// An unsupported option or positional argument was provided.
    #[error("unknown argument; use --help for usage")]
    UnknownArgument,
    /// Execution bounds must be non-zero decimal u64 values.
    #[error("{0} must be a non-zero decimal integer")]
    InvalidBudget(&'static str),
    /// Reading an explicitly selected fixture failed.
    #[error("failed to read the selected synthetic fixture: {0}")]
    FixtureIo(io::Error),
    /// A selected fixture exceeded the bounded CEXH input size.
    #[error("selected fixture exceeds the RPX input size limit")]
    FixtureTooLarge,
    /// A bounded fixture read could not obtain enough host memory.
    #[error("failed to allocate {requested} bytes while reading the selected fixture")]
    FixtureAllocationFailed {
        /// Requested output allocation in bytes.
        requested: usize,
    },
    /// A non-RPX file exceeded the smaller CEXH format bound.
    #[error("synthetic fixture exceeds the CEXH size limit")]
    SyntheticFixtureTooLarge,
    /// Creating or writing the selected trace destination failed.
    #[error("failed to create the trace destination: {0}")]
    TraceOutput(io::Error),
    /// Existing destinations are never replaced by a compatibility run.
    #[error("trace destination already exists; choose a different --trace-output")]
    TraceDestinationExists,
    /// A trace must never be published over its input fixture.
    #[error("trace destination must not be the input fixture")]
    TraceEqualsFixture,
    /// CEXH validation failed.
    #[error(transparent)]
    Program(#[from] ProgramDecodeError),
    /// Guest-system validation, mapping, or execution failed.
    #[error(transparent)]
    Headless(#[from] cex_system::HeadlessError),
    /// Canonical trace validation or output failed.
    #[error(transparent)]
    Trace(#[from] cex_compat::CompatError),
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parser_requires_fixture() {
        assert!(matches!(
            Options::parse(["Cemu"]),
            Err(CliError::MissingFixture)
        ));
    }

    #[test]
    fn parser_rejects_zero_budget() {
        assert!(matches!(
            Options::parse([
                "Cemu",
                "--headless-fixture",
                BUILTIN_FIXTURE_NAME,
                "--instruction-budget",
                "0"
            ]),
            Err(CliError::InvalidBudget("--instruction-budget"))
        ));
    }

    #[test]
    fn parser_rejects_unknown_argument_without_echoing_it() {
        let sentinel = "--api-key=super-secret-sentinel";
        let error = Options::parse(["Cemu", sentinel]).unwrap_err();

        assert!(matches!(&error, CliError::UnknownArgument));

        let display = error.to_string();
        let debug = format!("{error:?}");
        assert!(!display.contains(sentinel));
        assert!(!debug.contains(sentinel));
    }

    #[test]
    fn trace_destination_exists_does_not_echo_the_path() {
        let file = tempfile::Builder::new()
            .prefix("cemuextend-secret-sentinel-")
            .tempfile()
            .unwrap();
        let path = file.path().to_owned();

        let error = validate_trace_destination(&Options {
            fixture: OsString::from(BUILTIN_FIXTURE_NAME),
            trace_output: path.as_os_str().to_os_string(),
            instruction_budget: DEFAULT_BUDGET,
            cycle_budget: DEFAULT_BUDGET,
        })
        .unwrap_err();

        assert!(matches!(&error, CliError::TraceDestinationExists));

        let path_text = path.to_string_lossy();
        let display = error.to_string();
        let debug = format!("{error:?}");
        assert!(!display.contains(path_text.as_ref()));
        assert!(!debug.contains(path_text.as_ref()));
        assert!(!display.contains("sentinel"));
        assert!(!debug.contains("sentinel"));
    }

    #[test]
    fn external_fixture_reader_enforces_its_explicit_bound() {
        let mut file = tempfile::NamedTempFile::new().expect("temporary fixture must be created");
        file.write_all(b"123456789")
            .expect("temporary fixture must be written");

        assert!(matches!(
            read_bounded(file.path(), 8),
            Err(CliError::FixtureTooLarge)
        ));
    }

    #[test]
    fn bounded_reader_enforces_its_explicit_bound_without_allocating_unboundedly() {
        let mut reader = io::Cursor::new(b"123456789".as_slice());

        assert!(matches!(
            read_bounded_from_reader(&mut reader, 8),
            Err(CliError::FixtureTooLarge)
        ));
    }

    #[test]
    fn planned_capacity_grows_geometrically_and_caps_at_limit() {
        assert_eq!(planned_capacity(0, 0, 1, 8, 100).unwrap(), Some(8));
        assert_eq!(planned_capacity(8, 8, 1, 8, 100).unwrap(), Some(16));
        assert_eq!(planned_capacity(16, 16, 1, 8, 20).unwrap(), Some(20));
        assert_eq!(planned_capacity(16, 32, 1, 8, 100).unwrap(), None);
    }

    #[test]
    fn planned_capacity_rejects_over_limit_and_overflow() {
        assert!(matches!(
            planned_capacity(9, 8, 12, 8, 20),
            Err(CliError::FixtureTooLarge)
        ));
        assert!(matches!(
            planned_capacity(usize::MAX, usize::MAX, 1, 8, usize::MAX),
            Err(CliError::FixtureAllocationFailed { requested })
                if requested == usize::MAX
        ));
    }

    #[test]
    fn bounded_reader_handles_multiple_chunks_and_exact_cap() {
        let maximum = FIXTURE_READ_CHUNK * 2 + 37;
        let payload = vec![0x5a; maximum];
        let mut reader = io::Cursor::new(payload.clone());

        let bytes = read_bounded_from_reader(&mut reader, maximum).unwrap();

        assert_eq!(bytes, payload);
    }

    #[test]
    fn bounded_reader_rejects_payloads_one_byte_over_the_cap_after_multiple_chunks() {
        let maximum = FIXTURE_READ_CHUNK * 2 + 37;
        let payload = vec![0x5a; maximum + 1];
        let mut reader = io::Cursor::new(payload);

        assert!(matches!(
            read_bounded_from_reader(&mut reader, maximum),
            Err(CliError::FixtureTooLarge)
        ));
    }

    #[test]
    fn bounded_limit_rejects_an_unrepresentable_maximum() {
        assert!(matches!(
            bounded_limit(usize::MAX),
            Err(CliError::FixtureAllocationFailed { requested })
                if requested == usize::MAX
        ));
    }

    #[test]
    fn both_builtin_selectors_are_not_treated_as_host_paths() {
        assert!(is_builtin_fixture(OsStr::new(BUILTIN_FIXTURE_NAME)));
        assert!(is_builtin_fixture(OsStr::new(BUILTIN_RPX_FIXTURE_NAME)));
    }
}
