//! Emits the canonical Rust execution trace for the synthetic near REL24 call fixture.

use std::collections::BTreeMap;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::Path;
use std::process::ExitCode;

use cex_compat::{
    ArchitecturalFaultEvent, CpuStateEvent, DecimalI64, DecimalU64, FaultKind as TraceFaultKind,
    GenericEvent, HexU32, HexU64, MemoryHashAlgorithm, MemoryHashEvent, ReservationEvent,
    Sha256Digest, StopReason as TraceStopReason, TerminalEvent, TraceCategory, TraceEntry,
    TraceEvent, TraceSource, TraceValue, TraceWriter,
};
use cex_cpu::{FaultKind as CpuFaultKind, StopReason};
use cex_system::{
    CafeRelocationKind, HeadlessRun, HeadlessSystem, MAX_RPX_IMAGE_SIZE, ParsedRpl, ParsedRpx,
    RplModuleName, RpxRplHeadlessRun, RpxRplLinkPhase, RpxRplLinkProof, RpxRplRelocationProof,
    parse_rpl, parse_rpx,
};
use sha2::{Digest, Sha256};

const READ_CHUNK: usize = 8 * 1024;
const PROVIDER_CONTRACT_NAME: &str = "linkmod.rpl";
const LINKED_MAP_RANGE: &str = "rpx-rpl-rel24-linked-map-v1";
const EXECUTED_MAP_RANGE: &str = "rpx-rpl-rel24-executed-map-v1";
const TERMINAL_DETAIL: &str = "rpx-rpl-rel24-execution-v1";
const FINAL_CYCLE: u64 = 4;
const LINKED_PAGE_COUNT: u64 = 5;
const EXECUTED_PAGE_COUNT: u64 = 21;
const PAGE_SIZE: u64 = 4 * 1024;
const HELP: &str =
    "Usage: cex-rpl-call-execution-trace --main FILE --provider FILE [--trace-output FILE|-]\n";

fn main() -> ExitCode {
    match run(std::env::args_os()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::from(error.exit_code())
        }
    }
}

fn run(arguments: impl IntoIterator<Item = OsString>) -> Result<(), ExecutionTraceError> {
    match Options::parse(arguments)? {
        Invocation::Help => {
            let stdout = io::stdout();
            let mut stdout = stdout.lock();
            stdout
                .write_all(HELP.as_bytes())
                .and_then(|()| stdout.flush())
                .map_err(|_| ExecutionTraceError::Output)
        }
        Invocation::Run(options) => run_execution_trace(&options),
    }
}

fn run_execution_trace(options: &Options) -> Result<(), ExecutionTraceError> {
    validate_trace_destination(&options.main, &options.provider, &options.trace_output)?;
    let main_bytes = read_bounded(Path::new(&options.main), MAX_RPX_IMAGE_SIZE)?;
    let provider_bytes = read_bounded(Path::new(&options.provider), MAX_RPX_IMAGE_SIZE)?;
    let main = parse_rpx(&main_bytes).map_err(|_| ExecutionTraceError::Contract)?;
    let provider = parse_rpl(&provider_bytes).map_err(|_| ExecutionTraceError::Contract)?;
    let counts = ContractCounts::from_modules(&main, &provider)?;
    validate_module_contract(&main, &provider)?;
    let provider_name =
        RplModuleName::new(PROVIDER_CONTRACT_NAME).map_err(|_| ExecutionTraceError::Harness)?;
    let execution = HeadlessSystem::default()
        .run_rpx_rpl(&main_bytes, provider_name, &provider_bytes)
        .map_err(|_| ExecutionTraceError::Contract)?;
    validate_execution_contract(&execution)?;
    let hashes = ImageHashes {
        main: sha256_digest(&main_bytes)?,
        provider: sha256_digest(&provider_bytes)?,
    };
    let trace = build_trace(counts, hashes, &execution)?;
    publish_trace(&options.trace_output, &trace)
}

#[derive(Clone, Eq, PartialEq)]
struct Options {
    main: OsString,
    provider: OsString,
    trace_output: OsString,
}

impl std::fmt::Debug for Options {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Options")
            .field("main", &"<redacted>")
            .field("provider", &"<redacted>")
            .field("trace_output", &"<redacted>")
            .finish()
    }
}

#[derive(Debug)]
enum Invocation {
    Help,
    Run(Options),
}

impl Options {
    fn parse(
        arguments: impl IntoIterator<Item = OsString>,
    ) -> Result<Invocation, ExecutionTraceError> {
        let mut arguments = arguments.into_iter();
        let _program_name = arguments.next();
        let mut main = None;
        let mut provider = None;
        let mut trace_output = OsString::from("-");
        let mut output_seen = false;

        while let Some(argument) = arguments.next() {
            match argument.to_str() {
                Some("-h" | "--help") if main.is_none() && provider.is_none() && !output_seen => {
                    return if arguments.next().is_none() {
                        Ok(Invocation::Help)
                    } else {
                        Err(ExecutionTraceError::Usage)
                    };
                }
                Some("--main") if main.is_none() => main = Some(next_value(&mut arguments)?),
                Some("--provider") if provider.is_none() => {
                    provider = Some(next_value(&mut arguments)?);
                }
                Some("--trace-output") if !output_seen => {
                    trace_output = next_value(&mut arguments)?;
                    output_seen = true;
                }
                _ => return Err(ExecutionTraceError::Usage),
            }
        }

        Ok(Invocation::Run(Options {
            main: main.ok_or(ExecutionTraceError::Usage)?,
            provider: provider.ok_or(ExecutionTraceError::Usage)?,
            trace_output,
        }))
    }
}

fn next_value(
    arguments: &mut impl Iterator<Item = OsString>,
) -> Result<OsString, ExecutionTraceError> {
    arguments.next().ok_or(ExecutionTraceError::Usage)
}

fn read_bounded(path: &Path, maximum: usize) -> Result<Vec<u8>, ExecutionTraceError> {
    let mut input = File::open(path).map_err(|_| ExecutionTraceError::Input)?;
    read_bounded_from_reader(&mut input, maximum)
}

fn read_bounded_from_reader(
    reader: &mut impl Read,
    maximum: usize,
) -> Result<Vec<u8>, ExecutionTraceError> {
    let limit = maximum.checked_add(1).ok_or(ExecutionTraceError::Input)?;
    let mut bytes = Vec::new();
    let mut chunk = [0_u8; READ_CHUNK];

    loop {
        if bytes.len() == limit {
            return Err(ExecutionTraceError::Input);
        }
        let read_limit = (limit - bytes.len()).min(chunk.len());
        let read = match reader.read(&mut chunk[..read_limit]) {
            Ok(read) => read,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(_) => return Err(ExecutionTraceError::Input),
        };
        if read == 0 {
            break;
        }
        reserve_for_append(&mut bytes, read, limit)?;
        bytes.extend_from_slice(&chunk[..read]);
    }

    if bytes.len() > maximum {
        return Err(ExecutionTraceError::Input);
    }
    Ok(bytes)
}

fn reserve_for_append(
    bytes: &mut Vec<u8>,
    additional: usize,
    limit: usize,
) -> Result<(), ExecutionTraceError> {
    let required = bytes
        .len()
        .checked_add(additional)
        .ok_or(ExecutionTraceError::Input)?;
    if required > limit {
        return Err(ExecutionTraceError::Input);
    }
    if required <= bytes.capacity() {
        return Ok(());
    }

    let target = required
        .max(READ_CHUNK.max(bytes.capacity().saturating_mul(2)))
        .min(limit);
    bytes
        .try_reserve_exact(target - bytes.len())
        .map_err(|_| ExecutionTraceError::Input)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ContractCounts {
    main_imports: u64,
    main_relocations: u64,
    provider_exports: u64,
    provider_relocations: u64,
}

impl ContractCounts {
    fn from_modules(main: &ParsedRpx, provider: &ParsedRpl) -> Result<Self, ExecutionTraceError> {
        Ok(Self {
            main_imports: to_u64(main.imports().len())?,
            main_relocations: to_u64(main.relocations().len())?,
            provider_exports: to_u64(provider.exports().len())?,
            provider_relocations: to_u64(provider.relocations().len())?,
        })
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct ImageHashes {
    main: Sha256Digest,
    provider: Sha256Digest,
}

fn to_u64(value: usize) -> Result<u64, ExecutionTraceError> {
    u64::try_from(value).map_err(|_| ExecutionTraceError::Harness)
}

fn validate_module_contract(
    main: &ParsedRpx,
    provider: &ParsedRpl,
) -> Result<(), ExecutionTraceError> {
    if main.imports().len() != 1
        || main.relocations().len() != 1
        || main.relocations()[0].kind() != CafeRelocationKind::Rel24
        || provider.exports().len() != 1
        || provider.relocations().len() != 1
        || provider.relocations()[0].kind() != CafeRelocationKind::Addr32
    {
        return Err(ExecutionTraceError::Contract);
    }
    Ok(())
}

fn validate_execution_contract(run: &RpxRplHeadlessRun) -> Result<(), ExecutionTraceError> {
    let proof = &run.link_proof;
    let execution = &run.execution;
    let state = &execution.final_state;
    let local = proof.local_relocation();
    let imports = proof.import_relocations();
    if imports.len() != 1 {
        return Err(ExecutionTraceError::Contract);
    }
    let imported = &imports[0];
    let mut expected_gprs = [0_u32; 32];
    expected_gprs[1] = 0x4000_0000;
    expected_gprs[3] = 42;

    if local.phase() != RpxRplLinkPhase::Local
        || local.kind() != CafeRelocationKind::Addr32
        || local.displacement().is_some()
        || imported.phase() != RpxRplLinkPhase::Import
        || imported.kind() != CafeRelocationKind::Rel24
        || imported.displacement().is_none()
        || execution.outcome.reason != StopReason::StopSentinel
        || state.gprs != expected_gprs
        || state.fprs != [[0_u64; 2]; 32]
        || state.fpscr != 0
        || state.condition_register != 0
        || state.xer != 0
        || state.link_register != 0x0200_0004
        || state.count_register != 0
        || state.ugqr != [0_u32; 8]
        || state.reservation.is_some()
        || state.pending_fault.is_some()
        || state.instruction_pointer.get() != 0x0200_0008
        || state.instructions_retired != FINAL_CYCLE
        || state.cycles.get() != FINAL_CYCLE
        || execution.outcome.instructions_executed != FINAL_CYCLE
        || execution.outcome.cycles_elapsed != FINAL_CYCLE
        || proof.mapped_page_count() != LINKED_PAGE_COUNT
        || execution.mapped_page_count != EXECUTED_PAGE_COUNT
    {
        return Err(ExecutionTraceError::Contract);
    }
    Ok(())
}

fn build_trace(
    counts: ContractCounts,
    hashes: ImageHashes,
    run: &RpxRplHeadlessRun,
) -> Result<Vec<u8>, ExecutionTraceError> {
    validate_execution_contract(run)?;
    let proof = &run.link_proof;
    let execution = &run.execution;
    let mut trace = TraceWriter::new(Vec::new());
    for entry in [
        validation_entry(counts, hashes),
        local_relocation_entry(proof.local_relocation()),
        import_relocation_entry(single_import_relocation(proof)?)?,
        linked_memory_entry(proof)?,
        cpu_state_entry(execution),
        executed_memory_entry(execution)?,
        completed_entry(),
    ] {
        trace
            .write_entry(&entry)
            .map_err(|_| ExecutionTraceError::Harness)?;
    }
    trace.finish().map_err(|_| ExecutionTraceError::Harness)
}

fn validation_entry(counts: ContractCounts, hashes: ImageHashes) -> TraceEntry {
    let mut fields = BTreeMap::new();
    fields.insert(
        "main_image_sha256".to_owned(),
        TraceValue::Sha256(hashes.main),
    );
    fields.insert(
        "main_import_count".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(counts.main_imports)),
    );
    fields.insert(
        "main_relocation_count".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(counts.main_relocations)),
    );
    fields.insert(
        "main_relocation_kind".to_owned(),
        TraceValue::Text("rel24".to_owned()),
    );
    fields.insert(
        "provider_export_count".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(counts.provider_exports)),
    );
    fields.insert(
        "provider_image_sha256".to_owned(),
        TraceValue::Sha256(hashes.provider),
    );
    fields.insert(
        "provider_relocation_count".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(counts.provider_relocations)),
    );
    system_event(0, "rpx-rpl-rel24-call-validated", fields)
}

fn single_import_relocation(
    proof: &RpxRplLinkProof,
) -> Result<&RpxRplRelocationProof, ExecutionTraceError> {
    let imports = proof.import_relocations();
    if imports.len() != 1 {
        return Err(ExecutionTraceError::Contract);
    }
    Ok(&imports[0])
}

fn local_relocation_entry(relocation: &RpxRplRelocationProof) -> TraceEntry {
    let mut fields = BTreeMap::new();
    fields.insert(
        "addend".to_owned(),
        TraceValue::Signed(DecimalI64::from(i64::from(relocation.addend()))),
    );
    fields.insert("kind".to_owned(), TraceValue::Text("addr32".to_owned()));
    fields.insert(
        "patch_after".to_owned(),
        TraceValue::Hex32(HexU32::from(relocation.after())),
    );
    fields.insert(
        "patch_before".to_owned(),
        TraceValue::Hex32(HexU32::from(relocation.before())),
    );
    fields.insert(
        "patch_site".to_owned(),
        TraceValue::Hex32(HexU32::from(relocation.site())),
    );
    fields.insert(
        "resolved_symbol".to_owned(),
        TraceValue::Hex32(HexU32::from(relocation.resolved_symbol())),
    );
    system_event(1, "rpx-rpl-rel24-local-relocation", fields)
}

fn import_relocation_entry(
    relocation: &RpxRplRelocationProof,
) -> Result<TraceEntry, ExecutionTraceError> {
    let displacement = relocation
        .displacement()
        .ok_or(ExecutionTraceError::Contract)?;
    let mut fields = BTreeMap::new();
    fields.insert(
        "addend".to_owned(),
        TraceValue::Signed(DecimalI64::from(i64::from(relocation.addend()))),
    );
    fields.insert(
        "displacement".to_owned(),
        TraceValue::Signed(DecimalI64::from(i64::from(displacement))),
    );
    fields.insert("kind".to_owned(), TraceValue::Text("rel24".to_owned()));
    fields.insert(
        "patch_after".to_owned(),
        TraceValue::Hex32(HexU32::from(relocation.after())),
    );
    fields.insert(
        "patch_before".to_owned(),
        TraceValue::Hex32(HexU32::from(relocation.before())),
    );
    fields.insert(
        "patch_site".to_owned(),
        TraceValue::Hex32(HexU32::from(relocation.site())),
    );
    fields.insert(
        "resolved_symbol".to_owned(),
        TraceValue::Hex32(HexU32::from(relocation.resolved_symbol())),
    );
    Ok(system_event(2, "rpx-rpl-rel24-import-relocation", fields))
}

fn system_event(sequence: u32, name: &str, fields: BTreeMap<String, TraceValue>) -> TraceEntry {
    TraceEntry::new(
        0,
        sequence,
        TraceSource::Cafe,
        None,
        TraceCategory::System,
        TraceEvent::Event(GenericEvent {
            name: name.to_owned(),
            fields,
        }),
    )
}

fn linked_memory_entry(proof: &RpxRplLinkProof) -> Result<TraceEntry, ExecutionTraceError> {
    Ok(TraceEntry::new(
        0,
        3,
        TraceSource::Memory,
        None,
        TraceCategory::Memory,
        TraceEvent::MemoryHash(MemoryHashEvent {
            range: LINKED_MAP_RANGE.to_owned(),
            guest_address: HexU32::from(proof.main_entry()),
            byte_length: DecimalU64::from(proof.mapped_byte_count()),
            algorithm: MemoryHashAlgorithm::Sha256V1,
            digest: proof_digest(proof.memory_hash())?,
        }),
    ))
}

fn cpu_state_entry(run: &HeadlessRun) -> TraceEntry {
    let state = &run.final_state;
    TraceEntry::new(
        FINAL_CYCLE,
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
            instructions_retired: DecimalU64::from(state.instructions_retired),
        })),
    )
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

fn executed_memory_entry(run: &HeadlessRun) -> Result<TraceEntry, ExecutionTraceError> {
    let byte_length = run
        .mapped_page_count
        .checked_mul(PAGE_SIZE)
        .ok_or(ExecutionTraceError::Harness)?;
    Ok(TraceEntry::new(
        FINAL_CYCLE,
        1,
        TraceSource::Memory,
        None,
        TraceCategory::Memory,
        TraceEvent::MemoryHash(MemoryHashEvent {
            range: EXECUTED_MAP_RANGE.to_owned(),
            guest_address: HexU32::from(0),
            byte_length: DecimalU64::from(byte_length),
            algorithm: MemoryHashAlgorithm::Sha256V1,
            digest: proof_digest(run.memory_hash)?,
        }),
    ))
}

fn completed_entry() -> TraceEntry {
    TraceEntry::new(
        FINAL_CYCLE,
        2,
        TraceSource::TestHarness,
        None,
        TraceCategory::Terminal,
        TraceEvent::Terminal(TerminalEvent {
            reason: TraceStopReason::TestCompleted,
            guest_exit_code: None,
            detail_code: Some(TERMINAL_DETAIL.to_owned()),
        }),
    )
}

fn proof_digest(bytes: [u8; 32]) -> Result<Sha256Digest, ExecutionTraceError> {
    let mut text = String::with_capacity(Sha256Digest::HEX_LENGTH);
    for byte in bytes {
        use std::fmt::Write as _;
        write!(&mut text, "{byte:02x}").expect("writing to String cannot fail");
    }
    Sha256Digest::parse(text).map_err(|_| ExecutionTraceError::Harness)
}

fn sha256_digest(bytes: &[u8]) -> Result<Sha256Digest, ExecutionTraceError> {
    proof_digest(Sha256::digest(bytes).into())
}

fn validate_trace_destination(
    main: &OsStr,
    provider: &OsStr,
    destination: &OsStr,
) -> Result<(), ExecutionTraceError> {
    if destination == OsStr::new("-") {
        return Ok(());
    }
    let destination_path = Path::new(destination);
    match fs::symlink_metadata(destination_path) {
        Ok(_) => {
            if paths_alias(Path::new(main), destination_path)
                || paths_alias(Path::new(provider), destination_path)
            {
                Err(ExecutionTraceError::InputOutputAlias)
            } else {
                Err(ExecutionTraceError::Output)
            }
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(_) => Err(ExecutionTraceError::Output),
    }
}

fn paths_alias(first: &Path, second: &Path) -> bool {
    match (fs::canonicalize(first), fs::canonicalize(second)) {
        (Ok(first), Ok(second)) if first == second => true,
        _ => match (fs::metadata(first), fs::metadata(second)) {
            (Ok(first), Ok(second)) => {
                first.is_file() && second.is_file() && same_file(&first, &second)
            }
            _ => false,
        },
    }
}

#[cfg(unix)]
fn same_file(first: &fs::Metadata, second: &fs::Metadata) -> bool {
    use std::os::unix::fs::MetadataExt as _;
    first.dev() == second.dev() && first.ino() == second.ino()
}

#[cfg(not(unix))]
fn same_file(_first: &fs::Metadata, _second: &fs::Metadata) -> bool {
    false
}

fn publish_trace(destination: &OsStr, bytes: &[u8]) -> Result<(), ExecutionTraceError> {
    if destination == OsStr::new("-") {
        let stdout = io::stdout();
        let mut stdout = stdout.lock();
        stdout
            .write_all(bytes)
            .and_then(|()| stdout.flush())
            .map_err(|_| ExecutionTraceError::Output)?;
        return Ok(());
    }

    let destination = Path::new(destination);
    let parent = destination
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let mut staged = private_tempfile_in(parent)?;
    staged
        .as_file_mut()
        .write_all(bytes)
        .and_then(|()| staged.as_file_mut().flush())
        .and_then(|()| staged.as_file().sync_all())
        .map_err(|_| ExecutionTraceError::Output)?;
    let published = staged
        .persist_noclobber(destination)
        .map_err(|_| ExecutionTraceError::Output)?;
    published
        .sync_all()
        .map_err(|_| ExecutionTraceError::Output)?;
    File::open(parent)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| ExecutionTraceError::Output)
}

fn private_tempfile_in(parent: &Path) -> Result<tempfile::NamedTempFile, ExecutionTraceError> {
    let mut builder = tempfile::Builder::new();
    set_private_permissions(&mut builder);
    let staged = builder
        .tempfile_in(parent)
        .map_err(|_| ExecutionTraceError::Output)?;
    enforce_private_permissions(staged.as_file())?;
    Ok(staged)
}

#[cfg(unix)]
fn set_private_permissions(builder: &mut tempfile::Builder<'_, '_>) {
    use std::os::unix::fs::PermissionsExt as _;
    builder.permissions(fs::Permissions::from_mode(0o600));
}

#[cfg(not(unix))]
fn set_private_permissions(_builder: &mut tempfile::Builder<'_, '_>) {}

#[cfg(unix)]
fn enforce_private_permissions(file: &File) -> Result<(), ExecutionTraceError> {
    use std::os::unix::fs::PermissionsExt as _;
    file.set_permissions(fs::Permissions::from_mode(0o600))
        .map_err(|_| ExecutionTraceError::Output)
}

#[cfg(not(unix))]
fn enforce_private_permissions(_file: &File) -> Result<(), ExecutionTraceError> {
    Ok(())
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ExecutionTraceError {
    Usage,
    Input,
    Output,
    InputOutputAlias,
    Contract,
    Harness,
}

impl ExecutionTraceError {
    const fn exit_code(self) -> u8 {
        match self {
            Self::Contract => 1,
            Self::Usage | Self::Input | Self::Output | Self::InputOutputAlias | Self::Harness => 2,
        }
    }
}

impl std::fmt::Display for ExecutionTraceError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            Self::Usage => "cex-rpl-call-execution-trace: invalid arguments",
            Self::Input => "cex-rpl-call-execution-trace: failed to read fixture",
            Self::Output => "cex-rpl-call-execution-trace: failed to publish trace",
            Self::InputOutputAlias => {
                "cex-rpl-call-execution-trace: trace destination aliases fixture"
            }
            Self::Contract => {
                "cex-rpl-call-execution-trace: fixtures do not satisfy execution contract"
            }
            Self::Harness => "cex-rpl-call-execution-trace: failed to produce canonical trace",
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cex_compat::TraceReader;
    use cex_system::{
        synthetic_rpx_rpl_call_main_fixture, synthetic_rpx_rpl_call_provider_fixture,
    };
    use std::io::{BufReader, Cursor};

    fn fixture_trace() -> (Vec<u8>, Vec<u8>, Vec<u8>, RpxRplHeadlessRun) {
        let main_bytes = synthetic_rpx_rpl_call_main_fixture().unwrap();
        let provider_bytes = synthetic_rpx_rpl_call_provider_fixture().unwrap();
        let main = parse_rpx(&main_bytes).unwrap();
        let provider = parse_rpl(&provider_bytes).unwrap();
        let counts = ContractCounts::from_modules(&main, &provider).unwrap();
        validate_module_contract(&main, &provider).unwrap();
        let run = HeadlessSystem::default()
            .run_rpx_rpl(
                &main_bytes,
                RplModuleName::new(PROVIDER_CONTRACT_NAME).unwrap(),
                &provider_bytes,
            )
            .unwrap();
        let hashes = ImageHashes {
            main: sha256_digest(&main_bytes).unwrap(),
            provider: sha256_digest(&provider_bytes).unwrap(),
        };
        let trace = build_trace(counts, hashes, &run).unwrap();
        (trace, main_bytes, provider_bytes, run)
    }

    #[test]
    fn arguments_are_strict_and_all_path_bearing_debug_and_errors_are_redacted() {
        let options = Options::parse(
            [
                "trace",
                "--main",
                "/private/MAIN_SENTINEL.rpx",
                "--provider",
                "/private/PROVIDER_SENTINEL.rpl",
                "--trace-output",
                "/private/OUTPUT_SENTINEL.jsonl",
            ]
            .map(OsString::from),
        )
        .unwrap();
        let summary = format!("{options:?}");
        for sentinel in ["MAIN_SENTINEL", "PROVIDER_SENTINEL", "OUTPUT_SENTINEL"] {
            assert!(!summary.contains(sentinel));
        }
        assert!(summary.contains("<redacted>"));

        let secret = OsString::from("/private/ARGUMENT_SENTINEL");
        let error = Options::parse([OsString::from("trace"), secret.clone()]).unwrap_err();
        assert_eq!(error, ExecutionTraceError::Usage);
        assert!(
            !error
                .to_string()
                .contains(secret.to_string_lossy().as_ref())
        );
        assert!(!format!("{error:?}").contains(secret.to_string_lossy().as_ref()));
        assert!(matches!(
            Options::parse(["trace", "--help", "extra"].map(OsString::from)),
            Err(ExecutionTraceError::Usage)
        ));
        assert!(matches!(
            Options::parse(
                [
                    "trace",
                    "--main",
                    "main",
                    "--help",
                    "--provider",
                    "provider"
                ]
                .map(OsString::from)
            ),
            Err(ExecutionTraceError::Usage)
        ));
    }

    #[test]
    fn bounded_reader_accepts_the_limit_and_rejects_excess_or_impossible_limits() {
        let mut exact = Cursor::new(b"12345678".as_slice());
        assert_eq!(
            read_bounded_from_reader(&mut exact, 8).unwrap(),
            b"12345678"
        );
        let mut excess = Cursor::new(b"123456789".as_slice());
        assert_eq!(
            read_bounded_from_reader(&mut excess, 8),
            Err(ExecutionTraceError::Input)
        );
        let mut impossible_limit = Cursor::new(b"".as_slice());
        assert_eq!(
            read_bounded_from_reader(&mut impossible_limit, usize::MAX),
            Err(ExecutionTraceError::Input)
        );
    }

    #[test]
    fn emits_exactly_seven_ordered_records_with_the_complete_final_state() {
        let (trace, main_bytes, provider_bytes, run) = fixture_trace();
        let entries = TraceReader::new(BufReader::new(Cursor::new(trace)))
            .read_all()
            .unwrap();
        assert_trace_layout(&entries);
        assert_validation_record(&entries[0], &main_bytes, &provider_bytes);
        assert_relocation_records(&entries[1], &entries[2], &run.link_proof);
        assert_linked_memory_record(&entries[3], &run.link_proof);
        assert_cpu_state_record(&entries[4]);
        assert_executed_memory_record(&entries[5], &run.execution);
        assert_terminal_record(&entries[6]);
    }

    fn assert_trace_layout(entries: &[TraceEntry]) {
        assert_eq!(entries.len(), 7);
        assert_eq!(
            entries
                .iter()
                .map(|entry| (entry.key().guest_cycle, entry.key().sequence))
                .collect::<Vec<_>>(),
            vec![(0, 0), (0, 1), (0, 2), (0, 3), (4, 0), (4, 1), (4, 2)]
        );
        let expected_metadata = [
            (TraceSource::Cafe, TraceCategory::System, None),
            (TraceSource::Cafe, TraceCategory::System, None),
            (TraceSource::Cafe, TraceCategory::System, None),
            (TraceSource::Memory, TraceCategory::Memory, None),
            (TraceSource::Cpu, TraceCategory::Cpu, Some(0)),
            (TraceSource::Memory, TraceCategory::Memory, None),
            (TraceSource::TestHarness, TraceCategory::Terminal, None),
        ];
        for (entry, (source, category, core)) in entries.iter().zip(expected_metadata) {
            assert_eq!(entry.schema_version, 1);
            assert_eq!(entry.source, source);
            assert_eq!(entry.category, category);
            assert_eq!(entry.core, core);
        }
    }

    fn assert_validation_record(entry: &TraceEntry, main_bytes: &[u8], provider_bytes: &[u8]) {
        let TraceEvent::Event(validation) = &entry.event else {
            panic!("record zero must validate the REL24 call");
        };
        assert_eq!(validation.name, "rpx-rpl-rel24-call-validated");
        assert_eq!(validation.fields.len(), 7);
        assert_eq!(
            validation.fields.get("main_image_sha256"),
            Some(&TraceValue::Sha256(sha256_digest(main_bytes).unwrap()))
        );
        assert_eq!(
            validation.fields.get("main_import_count"),
            Some(&TraceValue::Unsigned(DecimalU64::from(1)))
        );
        assert_eq!(
            validation.fields.get("main_relocation_count"),
            Some(&TraceValue::Unsigned(DecimalU64::from(1)))
        );
        assert_eq!(
            validation.fields.get("main_relocation_kind"),
            Some(&TraceValue::Text("rel24".to_owned()))
        );
        assert_eq!(
            validation.fields.get("provider_export_count"),
            Some(&TraceValue::Unsigned(DecimalU64::from(1)))
        );
        assert_eq!(
            validation.fields.get("provider_image_sha256"),
            Some(&TraceValue::Sha256(sha256_digest(provider_bytes).unwrap()))
        );
        assert_eq!(
            validation.fields.get("provider_relocation_count"),
            Some(&TraceValue::Unsigned(DecimalU64::from(1)))
        );
    }

    fn assert_relocation_records(
        local_entry: &TraceEntry,
        import_entry: &TraceEntry,
        proof: &RpxRplLinkProof,
    ) {
        let local = proof.local_relocation();
        let TraceEvent::Event(local_event) = &local_entry.event else {
            panic!("record one must describe the local relocation");
        };
        assert_eq!(local_event.name, "rpx-rpl-rel24-local-relocation");
        assert_eq!(local_event.fields.len(), 6);
        assert_relocation_fields(local_event, local, "addr32", None);

        let imports = proof.import_relocations();
        assert_eq!(imports.len(), 1);
        let imported = &imports[0];
        let TraceEvent::Event(import_event) = &import_entry.event else {
            panic!("record two must describe the import relocation");
        };
        assert_eq!(import_event.name, "rpx-rpl-rel24-import-relocation");
        assert_eq!(import_event.fields.len(), 7);
        assert_relocation_fields(import_event, imported, "rel24", imported.displacement());
    }

    fn assert_linked_memory_record(entry: &TraceEntry, proof: &RpxRplLinkProof) {
        let TraceEvent::MemoryHash(linked_memory) = &entry.event else {
            panic!("record three must hash linked memory");
        };
        assert_eq!(linked_memory.range, LINKED_MAP_RANGE);
        assert_eq!(
            linked_memory.guest_address,
            HexU32::from(proof.main_entry())
        );
        assert_eq!(
            linked_memory.byte_length,
            DecimalU64::from(proof.mapped_byte_count())
        );
        assert_eq!(
            linked_memory.digest,
            proof_digest(proof.memory_hash()).unwrap()
        );
        assert_eq!(linked_memory.algorithm, MemoryHashAlgorithm::Sha256V1);
    }

    fn assert_cpu_state_record(entry: &TraceEntry) {
        let TraceEvent::CpuState(state) = &entry.event else {
            panic!("record four must contain the final CPU state");
        };
        let mut expected_gprs = [HexU32::from(0); 32];
        expected_gprs[1] = HexU32::from(0x4000_0000);
        expected_gprs[3] = HexU32::from(42);
        assert_eq!(state.pc, HexU32::from(0x0200_0008));
        assert_eq!(state.gpr, expected_gprs);
        assert_eq!(state.cr, HexU32::from(0));
        assert_eq!(state.lr, HexU32::from(0x0200_0004));
        assert_eq!(state.ctr, HexU32::from(0));
        assert_eq!(state.xer, HexU32::from(0));
        assert_eq!(state.fpr_bits, [[HexU64::from(0); 2]; 32]);
        assert_eq!(state.fpscr, HexU32::from(0));
        assert_eq!(state.ugqr, [HexU32::from(0); 8]);
        assert_eq!(state.reservation, None);
        assert_eq!(state.pending_fault, None);
        assert_eq!(state.instructions_retired, DecimalU64::from(4));
        assert_eq!(entry.core, Some(0));
    }

    fn assert_executed_memory_record(entry: &TraceEntry, execution: &HeadlessRun) {
        let TraceEvent::MemoryHash(executed_memory) = &entry.event else {
            panic!("record five must hash executed memory");
        };
        assert_eq!(executed_memory.range, EXECUTED_MAP_RANGE);
        assert_eq!(executed_memory.guest_address, HexU32::from(0));
        assert_eq!(executed_memory.byte_length, DecimalU64::from(86_016));
        assert_eq!(
            executed_memory.digest,
            proof_digest(execution.memory_hash).unwrap()
        );
        assert_eq!(executed_memory.algorithm, MemoryHashAlgorithm::Sha256V1);
    }

    fn assert_terminal_record(entry: &TraceEntry) {
        let TraceEvent::Terminal(terminal) = &entry.event else {
            panic!("record six must terminate the trace");
        };
        assert_eq!(terminal.reason, TraceStopReason::TestCompleted);
        assert_eq!(terminal.guest_exit_code, None);
        assert_eq!(terminal.detail_code.as_deref(), Some(TERMINAL_DETAIL));
    }

    fn assert_relocation_fields(
        event: &GenericEvent,
        relocation: &RpxRplRelocationProof,
        kind: &str,
        displacement: Option<i32>,
    ) {
        assert_eq!(
            event.fields.get("addend"),
            Some(&TraceValue::Signed(DecimalI64::from(i64::from(
                relocation.addend()
            ))))
        );
        assert_eq!(
            event.fields.get("kind"),
            Some(&TraceValue::Text(kind.to_owned()))
        );
        assert_eq!(
            event.fields.get("patch_after"),
            Some(&TraceValue::Hex32(HexU32::from(relocation.after())))
        );
        assert_eq!(
            event.fields.get("patch_before"),
            Some(&TraceValue::Hex32(HexU32::from(relocation.before())))
        );
        assert_eq!(
            event.fields.get("patch_site"),
            Some(&TraceValue::Hex32(HexU32::from(relocation.site())))
        );
        assert_eq!(
            event.fields.get("resolved_symbol"),
            Some(&TraceValue::Hex32(HexU32::from(
                relocation.resolved_symbol()
            )))
        );
        match displacement {
            Some(displacement) => assert_eq!(
                event.fields.get("displacement"),
                Some(&TraceValue::Signed(DecimalI64::from(i64::from(
                    displacement
                ))))
            ),
            None => assert!(!event.fields.contains_key("displacement")),
        }
    }

    #[test]
    fn repeated_fixture_execution_produces_identical_canonical_bytes() {
        let (first, _, _, first_run) = fixture_trace();
        let (second, _, _, second_run) = fixture_trace();
        assert_eq!(first, second);
        assert_eq!(first_run, second_run);
    }

    #[test]
    fn execution_contract_rejects_any_noncanonical_final_state() {
        let (_, _, _, mut run) = fixture_trace();
        run.execution.final_state.gprs[4] = 1;
        assert_eq!(
            validate_execution_contract(&run),
            Err(ExecutionTraceError::Contract)
        );
    }

    #[test]
    fn existing_and_alias_destinations_are_rejected_without_clobbering() {
        let directory = tempfile::tempdir().unwrap();
        let main = directory.path().join("main.rpx");
        let provider = directory.path().join("provider.rpl");
        let destination = directory.path().join("trace.jsonl");
        fs::write(&main, b"main").unwrap();
        fs::write(&provider, b"provider").unwrap();
        fs::write(&destination, b"original").unwrap();

        assert_eq!(
            validate_trace_destination(main.as_os_str(), provider.as_os_str(), main.as_os_str()),
            Err(ExecutionTraceError::InputOutputAlias)
        );
        assert_eq!(
            validate_trace_destination(
                main.as_os_str(),
                provider.as_os_str(),
                destination.as_os_str()
            ),
            Err(ExecutionTraceError::Output)
        );
        assert_eq!(
            publish_trace(destination.as_os_str(), b"replacement"),
            Err(ExecutionTraceError::Output)
        );
        assert_eq!(fs::read(&destination).unwrap(), b"original");
    }

    #[cfg(unix)]
    #[test]
    fn published_file_is_private() {
        use std::os::unix::fs::PermissionsExt as _;

        let directory = tempfile::tempdir().unwrap();
        let destination = directory.path().join("trace.jsonl");
        publish_trace(destination.as_os_str(), b"trace\n").unwrap();
        assert_eq!(
            fs::metadata(destination).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }

    #[test]
    fn exit_codes_separate_contract_mismatches_from_io_and_harness_errors() {
        assert_eq!(ExecutionTraceError::Contract.exit_code(), 1);
        for error in [
            ExecutionTraceError::Usage,
            ExecutionTraceError::Input,
            ExecutionTraceError::Output,
            ExecutionTraceError::InputOutputAlias,
            ExecutionTraceError::Harness,
        ] {
            assert_eq!(error.exit_code(), 2);
        }
    }
}
