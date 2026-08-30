//! Emits the canonical execution trace for the synthetic RPX/RPL ADDR16 data fixture.

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
const MAX_TRACE_BYTES: usize = 2 * 1024 * 1024;
const PROVIDER_CONTRACT_NAME: &str = "linkmod.rpl";
const LINKED_MAP_RANGE: &str = "rpx-rpl-addr16-data-linked-map-v1";
const EXECUTED_MAP_RANGE: &str = "rpx-rpl-addr16-data-executed-map-v1";
const TERMINAL_DETAIL: &str = "rpx-rpl-addr16-data-execution-v1";
const FINAL_CYCLE: u64 = 4;
const PAGE_SIZE: u64 = 4 * 1024;
const HELP: &str =
    "Usage: cex-rpl-data-execution-trace --main FILE --provider FILE [--trace-output FILE|-]\n";

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
        Invocation::Help => write_stdout(HELP.as_bytes()),
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
    let run = HeadlessSystem::default()
        .run_rpx_rpl(&main_bytes, provider_name, &provider_bytes)
        .map_err(|_| ExecutionTraceError::Contract)?;
    validate_execution_contract(&run)?;
    let trace = build_trace(
        counts,
        ImageHashes {
            main: sha256_digest(&main_bytes)?,
            provider: sha256_digest(&provider_bytes)?,
        },
        &run,
    )?;
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

#[derive(Debug, Eq, PartialEq)]
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
        Err(ExecutionTraceError::Input)
    } else {
        Ok(bytes)
    }
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
    if required > bytes.capacity() {
        let target = required
            .max(READ_CHUNK.max(bytes.capacity().saturating_mul(2)))
            .min(limit);
        bytes
            .try_reserve_exact(target - bytes.len())
            .map_err(|_| ExecutionTraceError::Input)?;
    }
    Ok(())
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
        || main.relocations().len() != 2
        || main.relocations()[0].kind() != CafeRelocationKind::Addr16Ha
        || main.relocations()[1].kind() != CafeRelocationKind::Addr16Lo
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
    let [import_ha, import_lo] = proof.import_relocations() else {
        return Err(ExecutionTraceError::Contract);
    };
    let mut gprs = [0_u32; 32];
    gprs[1] = 0x4000_0000;
    gprs[3] = 42;
    gprs[4] = 0x1000_8004;
    let all = proof.relocations();
    if all.len() != 3
        || all[0] != *local
        || all[1] != *import_ha
        || all[2] != *import_lo
        || proof.mapped_page_count() != 12
        || proof.mapped_byte_count() != 0xc000
        || execution.mapped_page_count != 28
        || local.phase() != RpxRplLinkPhase::Local
        || local.kind() != CafeRelocationKind::Addr32
        || local.width_bytes() != 4
        || local.site() != 0x1000_9008
        || local.before() != 0
        || local.after() != 0x1000_8000
        || local.resolved_symbol() != 0x1000_8000
        || local.addend() != 0
        || import_ha.phase() != RpxRplLinkPhase::Import
        || import_ha.kind() != CafeRelocationKind::Addr16Ha
        || import_ha.width_bytes() != 2
        || import_ha.site() != 0x0200_0002
        || import_ha.before() != 0
        || import_ha.after() != 0x1001
        || import_ha.resolved_symbol() != 0x1000_8000
        || import_ha.addend() != 4
        || import_lo.phase() != RpxRplLinkPhase::Import
        || import_lo.kind() != CafeRelocationKind::Addr16Lo
        || import_lo.width_bytes() != 2
        || import_lo.site() != 0x0200_0006
        || import_lo.before() != 0
        || import_lo.after() != 0x8004
        || import_lo.resolved_symbol() != 0x1000_8000
        || import_lo.addend() != 4
        || relocation_value(import_ha) != 0x1000_8004
        || relocation_value(import_lo) != 0x1000_8004
        || execution.outcome.reason != StopReason::StopSentinel
        || state.gprs != gprs
        || state.fprs != [[0_u64; 2]; 32]
        || state.fpscr != 0
        || state.condition_register != 0
        || state.link_register != 0
        || state.count_register != 0
        || state.xer != 0
        || state.ugqr != [0; 8]
        || state.reservation.is_some()
        || state.pending_fault.is_some()
        || state.instruction_pointer.get() != 0x0200_0010
        || state.instructions_retired != FINAL_CYCLE
        || state.cycles.get() != FINAL_CYCLE
        || execution.outcome.instructions_executed != FINAL_CYCLE
        || execution.outcome.cycles_elapsed != FINAL_CYCLE
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
    let [import_ha, import_lo] = proof.import_relocations() else {
        return Err(ExecutionTraceError::Contract);
    };
    let mut trace = TraceWriter::new(Vec::new());
    for entry in [
        validation_entry(counts, hashes),
        relocation_entry(1, "rpx-rpl-local-relocation", proof.local_relocation()),
        relocation_entry(2, "rpx-rpl-import-relocation", import_ha),
        relocation_entry(3, "rpx-rpl-import-relocation", import_lo),
        linked_memory_entry(proof)?,
        cpu_state_entry(&run.execution),
        executed_memory_entry(&run.execution)?,
        completed_entry(),
    ] {
        trace
            .write_entry(&entry)
            .map_err(|_| ExecutionTraceError::Harness)?;
    }
    let bytes = trace.finish().map_err(|_| ExecutionTraceError::Harness)?;
    if bytes.len() > MAX_TRACE_BYTES {
        Err(ExecutionTraceError::Harness)
    } else {
        Ok(bytes)
    }
}

fn validation_entry(counts: ContractCounts, hashes: ImageHashes) -> TraceEntry {
    let mut fields = BTreeMap::new();
    fields.insert(
        "main_image_sha256".to_owned(),
        TraceValue::Sha256(hashes.main),
    );
    fields.insert(
        "main_import_count".to_owned(),
        unsigned(counts.main_imports),
    );
    fields.insert(
        "main_relocation_count".to_owned(),
        unsigned(counts.main_relocations),
    );
    fields.insert(
        "main_relocation_0_kind".to_owned(),
        TraceValue::Text("addr16_ha".to_owned()),
    );
    fields.insert(
        "main_relocation_1_kind".to_owned(),
        TraceValue::Text("addr16_lo".to_owned()),
    );
    fields.insert(
        "provider_export_count".to_owned(),
        unsigned(counts.provider_exports),
    );
    fields.insert(
        "provider_image_sha256".to_owned(),
        TraceValue::Sha256(hashes.provider),
    );
    fields.insert(
        "provider_relocation_count".to_owned(),
        unsigned(counts.provider_relocations),
    );
    system_event(0, "rpx-rpl-addr16-data-validated", fields)
}

fn relocation_entry(sequence: u32, name: &str, relocation: &RpxRplRelocationProof) -> TraceEntry {
    let mut fields = BTreeMap::new();
    fields.insert(
        "addend".to_owned(),
        TraceValue::Signed(DecimalI64::from(i64::from(relocation.addend()))),
    );
    fields.insert(
        "kind".to_owned(),
        TraceValue::Text(relocation_kind(relocation.kind()).to_owned()),
    );
    fields.insert("patch_before".to_owned(), hex(relocation.before()));
    fields.insert("patch_site".to_owned(), hex(relocation.site()));
    fields.insert("patch_value".to_owned(), hex(relocation.after()));
    fields.insert(
        "relocation_value".to_owned(),
        hex(relocation_value(relocation)),
    );
    fields.insert(
        "resolved_symbol".to_owned(),
        hex(relocation.resolved_symbol()),
    );
    fields.insert(
        "width_bytes".to_owned(),
        unsigned(u64::try_from(relocation.width_bytes()).expect("relocation width fits u64")),
    );
    system_event(sequence, name, fields)
}

fn relocation_value(relocation: &RpxRplRelocationProof) -> u32 {
    relocation
        .resolved_symbol()
        .wrapping_add(relocation.addend().cast_unsigned())
}

fn relocation_kind(kind: CafeRelocationKind) -> &'static str {
    match kind {
        CafeRelocationKind::Addr32 => "addr32",
        CafeRelocationKind::Addr16Ha => "addr16_ha",
        CafeRelocationKind::Addr16Lo => "addr16_lo",
        CafeRelocationKind::Rel24 => "rel24",
    }
}

fn unsigned(value: u64) -> TraceValue {
    TraceValue::Unsigned(DecimalU64::from(value))
}
fn hex(value: u32) -> TraceValue {
    TraceValue::Hex32(HexU32::from(value))
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
        4,
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
    let destination = Path::new(destination);
    match fs::symlink_metadata(destination) {
        Ok(_)
            if paths_alias(Path::new(main), destination)
                || paths_alias(Path::new(provider), destination) =>
        {
            Err(ExecutionTraceError::InputOutputAlias)
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Ok(_) | Err(_) => Err(ExecutionTraceError::Output),
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

fn write_stdout(bytes: &[u8]) -> Result<(), ExecutionTraceError> {
    let stdout = io::stdout();
    let mut stdout = stdout.lock();
    stdout
        .write_all(bytes)
        .and_then(|()| stdout.flush())
        .map_err(|_| ExecutionTraceError::Output)
}

fn publish_trace(destination: &OsStr, bytes: &[u8]) -> Result<(), ExecutionTraceError> {
    if bytes.len() > MAX_TRACE_BYTES {
        return Err(ExecutionTraceError::Harness);
    }
    if destination == OsStr::new("-") {
        return write_stdout(bytes);
    }
    let destination = Path::new(destination);
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
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
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        builder.permissions(fs::Permissions::from_mode(0o600));
    }
    let staged = builder
        .tempfile_in(parent)
        .map_err(|_| ExecutionTraceError::Output)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        staged
            .as_file()
            .set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| ExecutionTraceError::Output)?;
    }
    Ok(staged)
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
            Self::Usage => "cex-rpl-data-execution-trace: invalid arguments",
            Self::Input => "cex-rpl-data-execution-trace: failed to read fixture",
            Self::Output => "cex-rpl-data-execution-trace: failed to publish trace",
            Self::InputOutputAlias => "cex-rpl-data-execution-trace: trace destination aliases fixture",
            Self::Contract => "cex-rpl-data-execution-trace: fixtures do not satisfy ADDR16 data execution contract",
            Self::Harness => "cex-rpl-data-execution-trace: failed to produce canonical trace",
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cex_compat::TraceReader;
    use cex_system::{
        synthetic_rpx_rpl_data_main_fixture, synthetic_rpx_rpl_data_provider_fixture,
    };
    use std::io::{BufReader, Cursor};

    #[test]
    fn exact_eight_record_trace_is_deterministic_and_derives_input_hashes() {
        let main = synthetic_rpx_rpl_data_main_fixture().unwrap();
        let provider = synthetic_rpx_rpl_data_provider_fixture().unwrap();
        let execute = || {
            let parsed_main = parse_rpx(&main).unwrap();
            let parsed_provider = parse_rpl(&provider).unwrap();
            let run = HeadlessSystem::default()
                .run_rpx_rpl(
                    &main,
                    RplModuleName::new(PROVIDER_CONTRACT_NAME).unwrap(),
                    &provider,
                )
                .unwrap();
            build_trace(
                ContractCounts::from_modules(&parsed_main, &parsed_provider).unwrap(),
                ImageHashes {
                    main: sha256_digest(&main).unwrap(),
                    provider: sha256_digest(&provider).unwrap(),
                },
                &run,
            )
            .unwrap()
        };
        let first = execute();
        assert_eq!(first, execute());
        let entries = TraceReader::new(BufReader::new(Cursor::new(first)))
            .read_all()
            .unwrap();
        assert_eq!(
            entries
                .iter()
                .map(|entry| (entry.key().guest_cycle, entry.key().sequence))
                .collect::<Vec<_>>(),
            vec![
                (0, 0),
                (0, 1),
                (0, 2),
                (0, 3),
                (0, 4),
                (4, 0),
                (4, 1),
                (4, 2)
            ]
        );
        let TraceEvent::Event(validation) = &entries[0].event else {
            panic!("validation expected")
        };
        assert_eq!(validation.name, "rpx-rpl-addr16-data-validated");
        assert_eq!(
            validation.fields.get("main_image_sha256"),
            Some(&TraceValue::Sha256(sha256_digest(&main).unwrap()))
        );
        let TraceEvent::CpuState(cpu) = &entries[5].event else {
            panic!("CPU state expected")
        };
        assert_eq!(cpu.pc, HexU32::from(0x0200_0010));
        assert_eq!(cpu.gpr[4], HexU32::from(0x1000_8004));
        let TraceEvent::MemoryHash(memory) = &entries[6].event else {
            panic!("execution map expected")
        };
        assert_eq!(memory.range, EXECUTED_MAP_RANGE);
        assert_eq!(memory.byte_length, DecimalU64::from(0x1c000));
    }

    #[test]
    fn strict_inputs_are_bounded_redacted_and_never_clobber() {
        let secret = OsString::from("/private/ADDR16_DATA_SENTINEL");
        assert_eq!(
            Options::parse([OsString::from("trace"), secret.clone()]),
            Err(ExecutionTraceError::Usage)
        );
        assert!(
            !format!("{:?}", ExecutionTraceError::Usage)
                .contains(secret.to_string_lossy().as_ref())
        );
        let mut excess = Cursor::new(vec![0; 9]);
        assert_eq!(
            read_bounded_from_reader(&mut excess, 8),
            Err(ExecutionTraceError::Input)
        );
        let directory = tempfile::tempdir().unwrap();
        let main = directory.path().join("main.rpx");
        fs::write(&main, b"preserve").unwrap();
        assert_eq!(
            validate_trace_destination(main.as_os_str(), OsStr::new("provider"), main.as_os_str()),
            Err(ExecutionTraceError::InputOutputAlias)
        );
        assert_eq!(
            publish_trace(main.as_os_str(), b"replacement"),
            Err(ExecutionTraceError::Output)
        );
        assert_eq!(fs::read(main).unwrap(), b"preserve");
    }
}
