//! Emits the canonical trace for the synthetic RPX-to-RPL link contract.

use std::collections::BTreeMap;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::Path;
use std::process::ExitCode;

use cex_compat::{
    DecimalU64, GenericEvent, HexU32, MemoryHashAlgorithm, MemoryHashEvent, Sha256Digest,
    StopReason, TerminalEvent, TraceCategory, TraceEntry, TraceEvent, TraceSource, TraceValue,
    TraceWriter,
};
use cex_system::{
    MAX_RPX_IMAGE_SIZE, ParsedRpl, ParsedRpx, RplModuleName, RpxRplLinkProof, commit_rpx_rpl_link,
    parse_rpl, parse_rpx, plan_rpx_rpl_link,
};
use sha2::{Digest, Sha256};

const READ_CHUNK: usize = 8 * 1024;
const PROVIDER_CONTRACT_NAME: &str = "linkmod.rpl";
const LINKED_MAP_RANGE: &str = "rpx-rpl-linked-map-v1";
const TERMINAL_DETAIL: &str = "rpx-rpl-link-state-v1";
const HELP: &str =
    "Usage: cex-rpl-link-contract-trace --main FILE --provider FILE [--trace-output FILE|-]\n";

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
        Invocation::Help => {
            let stdout = io::stdout();
            let mut stdout = stdout.lock();
            stdout
                .write_all(HELP.as_bytes())
                .and_then(|()| stdout.flush())
                .map_err(|_| ContractTraceError::Output)
        }
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

#[derive(Debug)]
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
        return Err(ContractTraceError::Input);
    }
    Ok(bytes)
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
    if required <= bytes.capacity() {
        return Ok(());
    }
    let target = required
        .max(READ_CHUNK.max(bytes.capacity().saturating_mul(2)))
        .min(limit);
    bytes
        .try_reserve_exact(target - bytes.len())
        .map_err(|_| ContractTraceError::Input)
}

fn build_trace(
    main_bytes: &[u8],
    main: ParsedRpx,
    provider_bytes: &[u8],
    provider: ParsedRpl,
) -> Result<Vec<u8>, ContractTraceError> {
    let counts = ContractCounts::from_modules(&main, &provider)?;
    let image_hashes = ImageHashes {
        main: sha256_digest(main_bytes)?,
        provider: sha256_digest(provider_bytes)?,
    };
    let provider_name =
        RplModuleName::new(PROVIDER_CONTRACT_NAME).map_err(|_| ContractTraceError::Harness)?;
    let plan = plan_rpx_rpl_link(main, provider_name, provider)
        .map_err(|_| ContractTraceError::Contract)?;
    let proof = commit_rpx_rpl_link(plan).map_err(|_| ContractTraceError::Contract)?;
    build_trace_from_proof(counts, image_hashes, &proof)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ContractCounts {
    main_imports: u64,
    main_relocations: u64,
    provider_exports: u64,
    provider_relocations: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct ImageHashes {
    main: Sha256Digest,
    provider: Sha256Digest,
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

fn to_u64(value: usize) -> Result<u64, ContractTraceError> {
    u64::try_from(value).map_err(|_| ContractTraceError::Harness)
}

fn build_trace_from_proof(
    counts: ContractCounts,
    image_hashes: ImageHashes,
    proof: &RpxRplLinkProof,
) -> Result<Vec<u8>, ContractTraceError> {
    let mut trace = TraceWriter::new(Vec::new());
    for entry in [
        validation_entry(counts, image_hashes),
        local_relocation_entry(proof),
        import_relocation_entry(proof),
        memory_hash_entry(proof)?,
        completed_entry(),
    ] {
        trace
            .write_entry(&entry)
            .map_err(|_| ContractTraceError::Harness)?;
    }
    trace.finish().map_err(|_| ContractTraceError::Harness)
}

fn validation_entry(counts: ContractCounts, image_hashes: ImageHashes) -> TraceEntry {
    let mut fields = BTreeMap::new();
    fields.insert(
        "main_import_count".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(counts.main_imports)),
    );
    fields.insert(
        "main_relocation_count".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(counts.main_relocations)),
    );
    fields.insert(
        "main_image_sha256".to_owned(),
        TraceValue::Sha256(image_hashes.main),
    );
    fields.insert(
        "provider_export_count".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(counts.provider_exports)),
    );
    fields.insert(
        "provider_relocation_count".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(counts.provider_relocations)),
    );
    fields.insert(
        "provider_image_sha256".to_owned(),
        TraceValue::Sha256(image_hashes.provider),
    );
    system_event(0, "rpx-rpl-link-validated", fields)
}

fn local_relocation_entry(proof: &RpxRplLinkProof) -> TraceEntry {
    let mut fields = BTreeMap::new();
    fields.insert(
        "patch_site".to_owned(),
        TraceValue::Hex32(HexU32::from(proof.local_patch_site())),
    );
    fields.insert(
        "patch_value".to_owned(),
        TraceValue::Hex32(HexU32::from(proof.local_patch_value())),
    );
    system_event(1, "rpx-rpl-local-relocation", fields)
}

fn import_relocation_entry(proof: &RpxRplLinkProof) -> TraceEntry {
    let mut fields = BTreeMap::new();
    fields.insert(
        "patch_before".to_owned(),
        TraceValue::Hex32(HexU32::from(0)),
    );
    fields.insert(
        "patch_site".to_owned(),
        TraceValue::Hex32(HexU32::from(proof.import_patch_site())),
    );
    fields.insert(
        "patch_value".to_owned(),
        TraceValue::Hex32(HexU32::from(proof.import_patch_value())),
    );
    fields.insert(
        "resolved_export".to_owned(),
        TraceValue::Hex32(HexU32::from(proof.local_patch_value())),
    );
    system_event(2, "rpx-rpl-import-relocation", fields)
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

fn completed_entry() -> TraceEntry {
    TraceEntry::new(
        0,
        4,
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
    let destination_path = Path::new(destination);
    match fs::symlink_metadata(destination_path) {
        Ok(_) => {
            if paths_alias(Path::new(main), destination_path)
                || paths_alias(Path::new(provider), destination_path)
            {
                Err(ContractTraceError::InputOutputAlias)
            } else {
                Err(ContractTraceError::Output)
            }
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(_) => Err(ContractTraceError::Output),
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

fn publish_trace(destination: &OsStr, bytes: &[u8]) -> Result<(), ContractTraceError> {
    if destination == OsStr::new("-") {
        let stdout = io::stdout();
        let mut stdout = stdout.lock();
        stdout
            .write_all(bytes)
            .and_then(|()| stdout.flush())
            .map_err(|_| ContractTraceError::Output)?;
        return Ok(());
    }
    let destination = Path::new(destination);
    let parent = destination
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let mut staged =
        tempfile::NamedTempFile::new_in(parent).map_err(|_| ContractTraceError::Output)?;
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
            Self::Usage => "cex-rpl-link-contract-trace: invalid arguments",
            Self::Input => "cex-rpl-link-contract-trace: failed to read fixture",
            Self::Output => "cex-rpl-link-contract-trace: failed to publish trace",
            Self::InputOutputAlias => {
                "cex-rpl-link-contract-trace: trace destination aliases fixture"
            }
            Self::Contract => "cex-rpl-link-contract-trace: fixtures do not satisfy link contract",
            Self::Harness => "cex-rpl-link-contract-trace: failed to produce canonical trace",
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cex_compat::TraceReader;
    use cex_system::{
        synthetic_rpx_rpl_link_main_fixture, synthetic_rpx_rpl_link_provider_fixture,
    };
    use std::io::{BufReader, Cursor};

    const MAIN_IMAGE_SHA256: &str =
        "13011586d955f3d3a0ce6668b1893f1a68a20f0435678e7ccd2fd85f0ea8374b";
    const PROVIDER_IMAGE_SHA256: &str =
        "9529541572c1eb5d83b2cf89f02f6b6daace501a9a8ac15e2f715a5229b68cec";
    const LINKED_MEMORY_SHA256: &str =
        "6f0b03a9b69c899443305158c5488338741291c8d098538b72d24960061246ae";

    fn linked_trace() -> (Vec<u8>, RpxRplLinkProof) {
        let main_bytes = synthetic_rpx_rpl_link_main_fixture().unwrap();
        let provider_bytes = synthetic_rpx_rpl_link_provider_fixture().unwrap();
        let main = parse_rpx(&main_bytes).unwrap();
        let provider = parse_rpl(&provider_bytes).unwrap();
        let counts = ContractCounts::from_modules(&main, &provider).unwrap();
        let image_hashes = ImageHashes {
            main: sha256_digest(&main_bytes).unwrap(),
            provider: sha256_digest(&provider_bytes).unwrap(),
        };
        let plan = plan_rpx_rpl_link(
            main,
            RplModuleName::new(PROVIDER_CONTRACT_NAME).unwrap(),
            provider,
        )
        .unwrap();
        let proof = commit_rpx_rpl_link(plan).unwrap();
        (
            build_trace_from_proof(counts, image_hashes, &proof).unwrap(),
            proof,
        )
    }

    fn assert_pinned_digests(
        proof: &RpxRplLinkProof,
        validation: &GenericEvent,
        memory: &MemoryHashEvent,
    ) {
        assert_eq!(
            sha256_digest(&synthetic_rpx_rpl_link_main_fixture().unwrap())
                .unwrap()
                .as_str(),
            MAIN_IMAGE_SHA256
        );
        assert_eq!(
            sha256_digest(&synthetic_rpx_rpl_link_provider_fixture().unwrap())
                .unwrap()
                .as_str(),
            PROVIDER_IMAGE_SHA256
        );
        assert_eq!(
            proof_digest(proof.memory_hash()).unwrap().as_str(),
            LINKED_MEMORY_SHA256
        );
        assert_eq!(
            validation.fields.get("main_image_sha256"),
            Some(&TraceValue::Sha256(
                Sha256Digest::parse(MAIN_IMAGE_SHA256).unwrap()
            ))
        );
        assert_eq!(
            validation.fields.get("provider_image_sha256"),
            Some(&TraceValue::Sha256(
                Sha256Digest::parse(PROVIDER_IMAGE_SHA256).unwrap()
            ))
        );
        assert_eq!(
            memory.digest,
            Sha256Digest::parse(LINKED_MEMORY_SHA256).unwrap()
        );
    }

    #[test]
    fn arguments_are_strict_and_debug_redacts_every_path() {
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
        assert_eq!(error, ContractTraceError::Usage);
        assert!(
            !error
                .to_string()
                .contains(secret.to_string_lossy().as_ref())
        );
        assert!(!format!("{error:?}").contains(secret.to_string_lossy().as_ref()));
    }

    #[test]
    fn bounded_reader_accepts_the_limit_and_rejects_one_byte_more() {
        let mut exact = Cursor::new(b"12345678".as_slice());
        assert_eq!(
            read_bounded_from_reader(&mut exact, 8).unwrap(),
            b"12345678"
        );
        let mut excess = Cursor::new(b"123456789".as_slice());
        assert_eq!(
            read_bounded_from_reader(&mut excess, 8),
            Err(ContractTraceError::Input)
        );
    }

    #[test]
    fn exit_codes_distinguish_contract_mismatch_from_harness_failures() {
        assert_eq!(ContractTraceError::Contract.exit_code(), 1);
        for error in [
            ContractTraceError::Usage,
            ContractTraceError::Input,
            ContractTraceError::Output,
            ContractTraceError::InputOutputAlias,
            ContractTraceError::Harness,
        ] {
            assert_eq!(error.exit_code(), 2);
        }
    }

    #[test]
    fn emits_exactly_five_deterministic_canonical_records() {
        let (first, proof) = linked_trace();
        let (second, repeated_proof) = linked_trace();
        assert_eq!(first, second);
        assert_eq!(proof, repeated_proof);

        let entries = TraceReader::new(BufReader::new(Cursor::new(first)))
            .read_all()
            .unwrap();
        assert_eq!(entries.len(), 5);
        for (sequence, entry) in entries.iter().enumerate() {
            assert_eq!(entry.schema_version, 1);
            assert_eq!(entry.key().guest_cycle, 0);
            assert_eq!(entry.key().sequence, u32::try_from(sequence).unwrap());
        }
        assert_eq!(proof.main_entry(), 0x0200_0000);
        assert_eq!(proof.local_patch_site(), 0x1000_2008);
        assert_eq!(proof.local_patch_value(), 0x0200_2000);
        assert_eq!(proof.import_patch_site(), 0x1000_0000);
        assert_eq!(proof.import_patch_value(), 0x0200_2000);
        assert_eq!(proof.mapped_page_count(), 5);
        assert_eq!(proof.mapped_byte_count(), 0x5000);

        let TraceEvent::Event(validation) = &entries[0].event else {
            panic!("first record must validate inputs");
        };
        assert_eq!(validation.name, "rpx-rpl-link-validated");
        assert_eq!(validation.fields.len(), 6);
        assert_eq!(
            validation.fields.get("main_import_count"),
            Some(&TraceValue::Unsigned(DecimalU64::from(1)))
        );
        assert_eq!(
            validation.fields.get("provider_export_count"),
            Some(&TraceValue::Unsigned(DecimalU64::from(1)))
        );

        let TraceEvent::Event(local) = &entries[1].event else {
            panic!("second record must be local relocation");
        };
        assert_eq!(local.name, "rpx-rpl-local-relocation");
        assert_eq!(
            local.fields.get("patch_site"),
            Some(&TraceValue::Hex32(HexU32::from(0x1000_2008)))
        );
        assert_eq!(
            local.fields.get("patch_value"),
            Some(&TraceValue::Hex32(HexU32::from(0x0200_2000)))
        );
        let TraceEvent::Event(import) = &entries[2].event else {
            panic!("third record must be import relocation");
        };
        assert_eq!(import.name, "rpx-rpl-import-relocation");
        assert_eq!(
            import.fields.get("patch_before"),
            Some(&TraceValue::Hex32(HexU32::from(0)))
        );
        assert_eq!(
            import.fields.get("patch_site"),
            Some(&TraceValue::Hex32(HexU32::from(0x1000_0000)))
        );
        assert_eq!(
            import.fields.get("patch_value"),
            Some(&TraceValue::Hex32(HexU32::from(0x0200_2000)))
        );
        assert_eq!(
            import.fields.get("resolved_export"),
            Some(&TraceValue::Hex32(HexU32::from(0x0200_2000)))
        );

        let TraceEvent::MemoryHash(memory) = &entries[3].event else {
            panic!("fourth record must hash linked memory");
        };
        assert_eq!(memory.range, LINKED_MAP_RANGE);
        assert_eq!(memory.guest_address, HexU32::from(0x0200_0000));
        assert_eq!(memory.byte_length, DecimalU64::from(0x5000));
        assert_pinned_digests(&proof, validation, memory);

        let TraceEvent::Terminal(terminal) = &entries[4].event else {
            panic!("fifth record must terminate the trace");
        };
        assert_eq!(terminal.reason, StopReason::TestCompleted);
        assert_eq!(terminal.detail_code.as_deref(), Some(TERMINAL_DETAIL));
    }
}
