//! Emits the canonical link-state trace for the synthetic RPX/RPL ADDR16 data fixture.

use std::collections::BTreeMap;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::Path;
use std::process::ExitCode;

use cex_compat::{
    DecimalI64, DecimalU64, GenericEvent, HexU32, MemoryHashAlgorithm, MemoryHashEvent,
    Sha256Digest, StopReason, TerminalEvent, TraceCategory, TraceEntry, TraceEvent, TraceSource,
    TraceValue, TraceWriter,
};
use cex_system::{
    CafeRelocationKind, MAX_RPX_IMAGE_SIZE, ParsedRpl, ParsedRpx, RplModuleName, RpxRplLinkPhase,
    RpxRplLinkProof, RpxRplRelocationProof, commit_rpx_rpl_link, parse_rpl, parse_rpx,
    plan_rpx_rpl_link,
};
use sha2::{Digest, Sha256};

const READ_CHUNK: usize = 8 * 1024;
const MAX_TRACE_BYTES: usize = 2 * 1024 * 1024;
const PROVIDER_CONTRACT_NAME: &str = "linkmod.rpl";
const LINKED_MAP_RANGE: &str = "rpx-rpl-addr16-data-linked-map-v1";
const TERMINAL_DETAIL: &str = "rpx-rpl-addr16-data-link-state-v1";
const HELP: &str =
    "Usage: cex-rpl-data-link-contract-trace --main FILE --provider FILE [--trace-output FILE|-]\n";

fn main() -> ExitCode {
    match run(std::env::args_os()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::from(error.exit_code())
        }
    }
}

fn run(arguments: impl IntoIterator<Item = OsString>) -> Result<(), ContractTraceError> {
    match Options::parse(arguments)? {
        Invocation::Help => write_stdout(HELP.as_bytes()),
        Invocation::Run(options) => run_contract_trace(&options),
    }
}

fn run_contract_trace(options: &Options) -> Result<(), ContractTraceError> {
    validate_trace_destination(&options.main, &options.provider, &options.trace_output)?;
    let main_bytes = read_bounded(Path::new(&options.main), MAX_RPX_IMAGE_SIZE)?;
    let provider_bytes = read_bounded(Path::new(&options.provider), MAX_RPX_IMAGE_SIZE)?;
    let main = parse_rpx(&main_bytes).map_err(|_| ContractTraceError::Contract)?;
    let provider = parse_rpl(&provider_bytes).map_err(|_| ContractTraceError::Contract)?;
    let trace = build_trace(&main_bytes, main, &provider_bytes, provider)?;
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
    ) -> Result<Invocation, ContractTraceError> {
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
                        Err(ContractTraceError::Usage)
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
                _ => return Err(ContractTraceError::Usage),
            }
        }

        Ok(Invocation::Run(Options {
            main: main.ok_or(ContractTraceError::Usage)?,
            provider: provider.ok_or(ContractTraceError::Usage)?,
            trace_output,
        }))
    }
}

fn next_value(
    arguments: &mut impl Iterator<Item = OsString>,
) -> Result<OsString, ContractTraceError> {
    arguments.next().ok_or(ContractTraceError::Usage)
}

fn read_bounded(path: &Path, maximum: usize) -> Result<Vec<u8>, ContractTraceError> {
    let mut input = File::open(path).map_err(|_| ContractTraceError::Input)?;
    read_bounded_from_reader(&mut input, maximum)
}

fn read_bounded_from_reader(
    reader: &mut impl Read,
    maximum: usize,
) -> Result<Vec<u8>, ContractTraceError> {
    let limit = maximum.checked_add(1).ok_or(ContractTraceError::Input)?;
    let mut bytes = Vec::new();
    let mut chunk = [0_u8; READ_CHUNK];
    loop {
        if bytes.len() == limit {
            return Err(ContractTraceError::Input);
        }
        let read_limit = (limit - bytes.len()).min(chunk.len());
        let read = match reader.read(&mut chunk[..read_limit]) {
            Ok(read) => read,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(_) => return Err(ContractTraceError::Input),
        };
        if read == 0 {
            break;
        }
        reserve_for_append(&mut bytes, read, limit)?;
        bytes.extend_from_slice(&chunk[..read]);
    }
    if bytes.len() > maximum {
        Err(ContractTraceError::Input)
    } else {
        Ok(bytes)
    }
}

fn reserve_for_append(
    bytes: &mut Vec<u8>,
    additional: usize,
    limit: usize,
) -> Result<(), ContractTraceError> {
    let required = bytes
        .len()
        .checked_add(additional)
        .ok_or(ContractTraceError::Input)?;
    if required > limit {
        return Err(ContractTraceError::Input);
    }
    if required > bytes.capacity() {
        let target = required
            .max(READ_CHUNK.max(bytes.capacity().saturating_mul(2)))
            .min(limit);
        bytes
            .try_reserve_exact(target - bytes.len())
            .map_err(|_| ContractTraceError::Input)?;
    }
    Ok(())
}

fn build_trace(
    main_bytes: &[u8],
    main: ParsedRpx,
    provider_bytes: &[u8],
    provider: ParsedRpl,
) -> Result<Vec<u8>, ContractTraceError> {
    let counts = ContractCounts::from_modules(&main, &provider)?;
    let provider_name =
        RplModuleName::new(PROVIDER_CONTRACT_NAME).map_err(|_| ContractTraceError::Harness)?;
    let proof = commit_rpx_rpl_link(
        plan_rpx_rpl_link(main, provider_name, provider)
            .map_err(|_| ContractTraceError::Contract)?,
    )
    .map_err(|_| ContractTraceError::Contract)?;
    build_trace_from_proof(
        counts,
        ImageHashes {
            main: sha256_digest(main_bytes)?,
            provider: sha256_digest(provider_bytes)?,
        },
        &proof,
    )
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ContractCounts {
    main_imports: u64,
    main_relocations: u64,
    provider_exports: u64,
    provider_relocations: u64,
}

impl ContractCounts {
    fn from_modules(main: &ParsedRpx, provider: &ParsedRpl) -> Result<Self, ContractTraceError> {
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

fn to_u64(value: usize) -> Result<u64, ContractTraceError> {
    u64::try_from(value).map_err(|_| ContractTraceError::Harness)
}

fn build_trace_from_proof(
    counts: ContractCounts,
    hashes: ImageHashes,
    proof: &RpxRplLinkProof,
) -> Result<Vec<u8>, ContractTraceError> {
    let local = proof.local_relocation();
    let [import_ha, import_lo] = proof.import_relocations() else {
        return Err(ContractTraceError::Contract);
    };
    validate_relocations(local, import_ha, import_lo, proof)?;

    let mut trace = TraceWriter::new(Vec::new());
    for entry in [
        validation_entry(counts, hashes),
        relocation_entry(1, "rpx-rpl-local-relocation", local),
        relocation_entry(2, "rpx-rpl-import-relocation", import_ha),
        relocation_entry(3, "rpx-rpl-import-relocation", import_lo),
        memory_hash_entry(proof)?,
        completed_entry(),
    ] {
        trace
            .write_entry(&entry)
            .map_err(|_| ContractTraceError::Harness)?;
    }
    let bytes = trace.finish().map_err(|_| ContractTraceError::Harness)?;
    if bytes.len() > MAX_TRACE_BYTES {
        Err(ContractTraceError::Harness)
    } else {
        Ok(bytes)
    }
}

fn validate_relocations(
    local: &RpxRplRelocationProof,
    import_ha: &RpxRplRelocationProof,
    import_lo: &RpxRplRelocationProof,
    proof: &RpxRplLinkProof,
) -> Result<(), ContractTraceError> {
    let all = proof.relocations();
    if all.len() != 3
        || all[0] != *local
        || all[1] != *import_ha
        || all[2] != *import_lo
        || proof.mapped_page_count() != 12
        || proof.mapped_byte_count() != 0xc000
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
        || import_ha.after() != 0x0000_1001
        || import_ha.resolved_symbol() != 0x1000_8000
        || import_ha.addend() != 4
        || import_lo.phase() != RpxRplLinkPhase::Import
        || import_lo.kind() != CafeRelocationKind::Addr16Lo
        || import_lo.width_bytes() != 2
        || import_lo.site() != 0x0200_0006
        || import_lo.before() != 0
        || import_lo.after() != 0x0000_8004
        || import_lo.resolved_symbol() != 0x1000_8000
        || import_lo.addend() != 4
        || relocation_value(import_ha) != 0x1000_8004
        || relocation_value(import_lo) != 0x1000_8004
    {
        return Err(ContractTraceError::Contract);
    }
    Ok(())
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

fn memory_hash_entry(proof: &RpxRplLinkProof) -> Result<TraceEntry, ContractTraceError> {
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

fn completed_entry() -> TraceEntry {
    TraceEntry::new(
        0,
        5,
        TraceSource::TestHarness,
        None,
        TraceCategory::Terminal,
        TraceEvent::Terminal(TerminalEvent {
            reason: StopReason::TestCompleted,
            guest_exit_code: None,
            detail_code: Some(TERMINAL_DETAIL.to_owned()),
        }),
    )
}

fn proof_digest(bytes: [u8; 32]) -> Result<Sha256Digest, ContractTraceError> {
    let mut text = String::with_capacity(Sha256Digest::HEX_LENGTH);
    for byte in bytes {
        use std::fmt::Write as _;
        write!(&mut text, "{byte:02x}").expect("writing to String cannot fail");
    }
    Sha256Digest::parse(text).map_err(|_| ContractTraceError::Harness)
}

fn sha256_digest(bytes: &[u8]) -> Result<Sha256Digest, ContractTraceError> {
    proof_digest(Sha256::digest(bytes).into())
}

fn validate_trace_destination(
    main: &OsStr,
    provider: &OsStr,
    destination: &OsStr,
) -> Result<(), ContractTraceError> {
    if destination == OsStr::new("-") {
        return Ok(());
    }
    let destination = Path::new(destination);
    match fs::symlink_metadata(destination) {
        Ok(_)
            if paths_alias(Path::new(main), destination)
                || paths_alias(Path::new(provider), destination) =>
        {
            Err(ContractTraceError::InputOutputAlias)
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Ok(_) | Err(_) => Err(ContractTraceError::Output),
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

fn write_stdout(bytes: &[u8]) -> Result<(), ContractTraceError> {
    let stdout = io::stdout();
    let mut stdout = stdout.lock();
    stdout
        .write_all(bytes)
        .and_then(|()| stdout.flush())
        .map_err(|_| ContractTraceError::Output)
}

fn publish_trace(destination: &OsStr, bytes: &[u8]) -> Result<(), ContractTraceError> {
    if bytes.len() > MAX_TRACE_BYTES {
        return Err(ContractTraceError::Harness);
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
        .map_err(|_| ContractTraceError::Output)?;
    let published = staged
        .persist_noclobber(destination)
        .map_err(|_| ContractTraceError::Output)?;
    published
        .sync_all()
        .map_err(|_| ContractTraceError::Output)?;
    File::open(parent)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| ContractTraceError::Output)
}

fn private_tempfile_in(parent: &Path) -> Result<tempfile::NamedTempFile, ContractTraceError> {
    let mut builder = tempfile::Builder::new();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        builder.permissions(fs::Permissions::from_mode(0o600));
    }
    let staged = builder
        .tempfile_in(parent)
        .map_err(|_| ContractTraceError::Output)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        staged
            .as_file()
            .set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| ContractTraceError::Output)?;
    }
    Ok(staged)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ContractTraceError {
    Usage,
    Input,
    Output,
    InputOutputAlias,
    Contract,
    Harness,
}

impl ContractTraceError {
    const fn exit_code(self) -> u8 {
        match self {
            Self::Contract => 1,
            Self::Usage | Self::Input | Self::Output | Self::InputOutputAlias | Self::Harness => 2,
        }
    }
}

impl std::fmt::Display for ContractTraceError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            Self::Usage => "cex-rpl-data-link-contract-trace: invalid arguments",
            Self::Input => "cex-rpl-data-link-contract-trace: failed to read fixture",
            Self::Output => "cex-rpl-data-link-contract-trace: failed to publish trace",
            Self::InputOutputAlias => "cex-rpl-data-link-contract-trace: trace destination aliases fixture",
            Self::Contract => "cex-rpl-data-link-contract-trace: fixtures do not satisfy ADDR16 data link contract",
            Self::Harness => "cex-rpl-data-link-contract-trace: failed to produce canonical trace",
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
    fn trace_is_deterministic_and_has_the_six_canonical_records() {
        let main = synthetic_rpx_rpl_data_main_fixture().unwrap();
        let provider = synthetic_rpx_rpl_data_provider_fixture().unwrap();
        let first = build_trace(
            &main,
            parse_rpx(&main).unwrap(),
            &provider,
            parse_rpl(&provider).unwrap(),
        )
        .unwrap();
        let second = build_trace(
            &main,
            parse_rpx(&main).unwrap(),
            &provider,
            parse_rpl(&provider).unwrap(),
        )
        .unwrap();
        assert_eq!(first, second);
        let entries = TraceReader::new(BufReader::new(Cursor::new(first)))
            .read_all()
            .unwrap();
        assert_eq!(entries.len(), 6);
        assert_eq!(
            entries
                .iter()
                .map(|entry| (entry.key().guest_cycle, entry.key().sequence))
                .collect::<Vec<_>>(),
            vec![(0, 0), (0, 1), (0, 2), (0, 3), (0, 4), (0, 5)]
        );
        let TraceEvent::Event(validation) = &entries[0].event else {
            panic!("validation event expected")
        };
        assert_eq!(validation.name, "rpx-rpl-addr16-data-validated");
        assert_eq!(
            validation.fields.get("main_image_sha256"),
            Some(&TraceValue::Sha256(sha256_digest(&main).unwrap()))
        );
        assert_eq!(
            validation.fields.get("provider_image_sha256"),
            Some(&TraceValue::Sha256(sha256_digest(&provider).unwrap()))
        );
        let TraceEvent::MemoryHash(memory) = &entries[4].event else {
            panic!("memory hash expected")
        };
        assert_eq!(memory.range, LINKED_MAP_RANGE);
        assert_eq!(memory.byte_length, DecimalU64::from(0xc000));
    }

    #[test]
    fn arguments_bounds_and_destination_aliases_are_strict_and_redacted() {
        let path = OsString::from("/private/ADDR16_DATA_SENTINEL");
        assert_eq!(
            Options::parse([OsString::from("trace"), path.clone()]),
            Err(ContractTraceError::Usage)
        );
        assert!(
            !format!("{:?}", ContractTraceError::Usage).contains(path.to_string_lossy().as_ref())
        );
        let mut exact = Cursor::new(vec![0; 8]);
        assert_eq!(read_bounded_from_reader(&mut exact, 8).unwrap().len(), 8);
        let mut oversized = Cursor::new(vec![0; 9]);
        assert_eq!(
            read_bounded_from_reader(&mut oversized, 8),
            Err(ContractTraceError::Input)
        );
        let directory = tempfile::tempdir().unwrap();
        let main = directory.path().join("main.rpx");
        fs::write(&main, b"keep").unwrap();
        assert_eq!(
            validate_trace_destination(main.as_os_str(), OsStr::new("provider"), main.as_os_str()),
            Err(ContractTraceError::InputOutputAlias)
        );
        assert_eq!(
            publish_trace(main.as_os_str(), b"new"),
            Err(ContractTraceError::Output)
        );
        assert_eq!(fs::read(main).unwrap(), b"keep");
    }
}
