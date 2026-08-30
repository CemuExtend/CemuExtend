//! Emits the canonical trace for the checked-in synthetic Cafe RPX contract.

use std::collections::BTreeMap;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::Path;
use std::process::ExitCode;

use cex_compat::{
    DecimalU64, GenericEvent, HexU32, Sha256Digest, StopReason, TerminalEvent, TraceCategory,
    TraceEntry, TraceEvent, TraceSource, TraceValue, TraceWriter,
};
use cex_system::{
    MAX_RPX_IMAGE_SIZE, ParsedRpx, RpxMappingRegion, RpxSection, builtin_rpx_fixture, parse_rpx,
};
use sha2::{Digest, Sha256};

const READ_CHUNK: usize = 8 * 1024;
const SYNTHETIC_FIXTURE: &str = "synthetic-rpx-v1";
const TEXT_SECTION_INDEX: usize = 1;
const TEXT_SECTION_TYPE: u32 = 1;
const TEXT_SECTION_FLAGS: u32 = 0x6;
const TEXT_SECTION_ADDRESS: u32 = 0x0200_0000;
const TEXT_SECTION_LENGTH: usize = 12;

const HELP: &str = "Usage: cex-rpx-contract-trace --fixture FILE [--trace-output FILE|-]\n";

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
    validate_trace_destination(&options.fixture, &options.trace_output)?;
    let image = read_bounded(Path::new(&options.fixture), MAX_RPX_IMAGE_SIZE)?;
    let parsed = parse_rpx(&image).map_err(|_| ContractTraceError::Contract)?;
    validate_builtin_contract(&image, &parsed)?;
    let trace = build_trace(&image, &parsed)?;
    publish_trace(&options.trace_output, &trace)
}

#[derive(Clone, Eq, PartialEq)]
struct Options {
    fixture: OsString,
    trace_output: OsString,
}

impl std::fmt::Debug for Options {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Options")
            .field("fixture", &"<redacted>")
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
        let mut fixture = None;
        let mut trace_output = OsString::from("-");

        while let Some(argument) = arguments.next() {
            match argument.to_str() {
                Some("-h" | "--help") => return Ok(Invocation::Help),
                Some("--fixture") if fixture.is_none() => {
                    fixture = Some(next_value(&mut arguments)?);
                }
                Some("--trace-output") => {
                    trace_output = next_value(&mut arguments)?;
                }
                _ => return Err(ContractTraceError::Usage),
            }
        }

        Ok(Invocation::Run(Options {
            fixture: fixture.ok_or(ContractTraceError::Usage)?,
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

fn validate_builtin_contract(image: &[u8], parsed: &ParsedRpx) -> Result<(), ContractTraceError> {
    if image != builtin_rpx_fixture()
        || !parsed.is_rpx()
        || parsed.entry_point() != TEXT_SECTION_ADDRESS
        || parsed.sections().len() != 5
        || parsed.file_info().flags() != 2
    {
        return Err(ContractTraceError::Contract);
    }
    let section = parsed
        .sections()
        .get(TEXT_SECTION_INDEX)
        .ok_or(ContractTraceError::Contract)?;
    if section.index() != TEXT_SECTION_INDEX
        || section.name() != ".text"
        || section.section_type() != TEXT_SECTION_TYPE
        || section.flags() != TEXT_SECTION_FLAGS
        || section.virtual_address() != TEXT_SECTION_ADDRESS
        || section.data().len() != TEXT_SECTION_LENGTH
        || section.mapping_region() != Some(RpxMappingRegion::Text)
        || !section.is_allocated()
        || !section.is_executable()
        || section.is_writable()
    {
        return Err(ContractTraceError::Contract);
    }
    Ok(())
}

fn build_trace(image: &[u8], parsed: &ParsedRpx) -> Result<Vec<u8>, ContractTraceError> {
    let section = parsed
        .sections()
        .get(TEXT_SECTION_INDEX)
        .ok_or(ContractTraceError::Contract)?;
    let mut trace = TraceWriter::new(Vec::new());
    trace
        .write_entry(&contract_validated_entry(image, parsed)?)
        .map_err(|_| ContractTraceError::Harness)?;
    trace
        .write_entry(&section_mapping_entry(section)?)
        .map_err(|_| ContractTraceError::Harness)?;
    trace
        .write_entry(&completed_entry())
        .map_err(|_| ContractTraceError::Harness)?;
    trace.finish().map_err(|_| ContractTraceError::Harness)
}

fn contract_validated_entry(
    image: &[u8],
    parsed: &ParsedRpx,
) -> Result<TraceEntry, ContractTraceError> {
    let mut fields = BTreeMap::new();
    fields.insert(
        "entry_point".to_owned(),
        TraceValue::Hex32(HexU32::from(parsed.entry_point())),
    );
    fields.insert(
        "file_info_flags".to_owned(),
        TraceValue::Hex32(HexU32::from(parsed.file_info().flags())),
    );
    fields.insert(
        "fixture".to_owned(),
        TraceValue::Text(SYNTHETIC_FIXTURE.to_owned()),
    );
    fields.insert(
        "image_bytes".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(
            u64::try_from(image.len()).map_err(|_| ContractTraceError::Harness)?,
        )),
    );
    fields.insert(
        "program_sha256".to_owned(),
        TraceValue::Sha256(sha256_digest(image)?),
    );
    fields.insert(
        "section_count".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(
            u64::try_from(parsed.sections().len()).map_err(|_| ContractTraceError::Harness)?,
        )),
    );
    Ok(TraceEntry::new(
        0,
        0,
        TraceSource::Cafe,
        None,
        TraceCategory::System,
        TraceEvent::Event(GenericEvent {
            name: "rpx-contract-validated".to_owned(),
            fields,
        }),
    ))
}

fn section_mapping_entry(section: &RpxSection) -> Result<TraceEntry, ContractTraceError> {
    let region = section
        .mapping_region()
        .ok_or(ContractTraceError::Contract)?;
    let mut fields = BTreeMap::new();
    fields.insert(
        "byte_length".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(
            u64::try_from(section.data().len()).map_err(|_| ContractTraceError::Harness)?,
        )),
    );
    fields.insert(
        "flags".to_owned(),
        TraceValue::Hex32(HexU32::from(section.flags())),
    );
    fields.insert(
        "guest_address".to_owned(),
        TraceValue::Hex32(HexU32::from(section.virtual_address())),
    );
    fields.insert("region".to_owned(), TraceValue::Text(region.to_string()));
    fields.insert(
        "section_index".to_owned(),
        TraceValue::Unsigned(DecimalU64::from(
            u64::try_from(section.index()).map_err(|_| ContractTraceError::Harness)?,
        )),
    );
    fields.insert(
        "section_type".to_owned(),
        TraceValue::Hex32(HexU32::from(section.section_type())),
    );
    Ok(TraceEntry::new(
        0,
        1,
        TraceSource::Cafe,
        None,
        TraceCategory::System,
        TraceEvent::Event(GenericEvent {
            name: "rpx-section-mapping".to_owned(),
            fields,
        }),
    ))
}

fn completed_entry() -> TraceEntry {
    TraceEntry::new(
        0,
        2,
        TraceSource::TestHarness,
        None,
        TraceCategory::Terminal,
        TraceEvent::Terminal(TerminalEvent {
            reason: StopReason::TestCompleted,
            guest_exit_code: None,
            detail_code: Some("rpx-parse-map-contract-v1".to_owned()),
        }),
    )
}

fn sha256_digest(bytes: &[u8]) -> Result<Sha256Digest, ContractTraceError> {
    let digest = Sha256::digest(bytes);
    let mut text = String::with_capacity(Sha256Digest::HEX_LENGTH);
    for byte in digest {
        use std::fmt::Write as _;
        write!(&mut text, "{byte:02x}").expect("writing to String cannot fail");
    }
    Sha256Digest::parse(text).map_err(|_| ContractTraceError::Harness)
}

fn validate_trace_destination(
    fixture: &OsStr,
    destination: &OsStr,
) -> Result<(), ContractTraceError> {
    if destination == OsStr::new("-") {
        return Ok(());
    }
    let fixture = Path::new(fixture);
    let destination = Path::new(destination);
    match fs::symlink_metadata(destination) {
        Ok(_) => {
            if paths_alias(fixture, destination) {
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
                first.is_file() && second.is_file() && same_linux_file(&first, &second)
            }
            _ => false,
        },
    }
}

#[cfg(unix)]
fn same_linux_file(first: &fs::Metadata, second: &fs::Metadata) -> bool {
    use std::os::unix::fs::MetadataExt as _;
    first.dev() == second.dev() && first.ino() == second.ino()
}

#[cfg(not(unix))]
fn same_linux_file(_first: &fs::Metadata, _second: &fs::Metadata) -> bool {
    false
}

fn publish_trace(destination: &OsStr, bytes: &[u8]) -> Result<(), ContractTraceError> {
    if destination == OsStr::new("-") {
        let stdout = io::stdout();
        let mut stdout = stdout.lock();
        stdout
            .write_all(bytes)
            .map_err(|_| ContractTraceError::Output)?;
        return stdout.flush().map_err(|_| ContractTraceError::Output);
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
            Self::Usage => "cex-rpx-contract-trace: invalid arguments",
            Self::Input => "cex-rpx-contract-trace: failed to read fixture",
            Self::Output => "cex-rpx-contract-trace: failed to publish trace",
            Self::InputOutputAlias => "cex-rpx-contract-trace: trace destination aliases fixture",
            Self::Contract => {
                "cex-rpx-contract-trace: fixture does not satisfy synthetic RPX contract"
            }
            Self::Harness => "cex-rpx-contract-trace: failed to produce canonical trace",
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cex_compat::TraceReader;
    use std::io::{BufReader, Cursor};

    #[test]
    fn builtin_fixture_passes_the_strict_contract() {
        let image = builtin_rpx_fixture();
        let parsed = parse_rpx(&image).expect("builtin fixture must parse");

        validate_builtin_contract(&image, &parsed).expect("builtin contract must validate");
    }

    #[test]
    fn mapping_trace_rejects_an_unclassified_section() {
        let image = builtin_rpx_fixture();
        let parsed = parse_rpx(&image).expect("builtin fixture must parse");
        let name_table = &parsed.sections()[2];

        assert_eq!(name_table.mapping_region(), None);
        assert!(matches!(
            section_mapping_entry(name_table),
            Err(ContractTraceError::Contract)
        ));
    }

    #[test]
    fn option_debug_output_redacts_paths() {
        let fixture_sentinel = "/private/RPX_FIXTURE_SENTINEL.rpx";
        let output_sentinel = "/private/RPX_TRACE_SENTINEL.jsonl";
        let options = Options {
            fixture: OsString::from(fixture_sentinel),
            trace_output: OsString::from(output_sentinel),
        };

        for summary in [
            format!("{options:?}"),
            format!("{:?}", Invocation::Run(options)),
        ] {
            assert!(!summary.contains(fixture_sentinel));
            assert!(!summary.contains(output_sentinel));
            assert!(summary.contains("<redacted>"));
        }
    }

    #[test]
    fn hash_covers_the_complete_fixture_bytes() {
        let image = builtin_rpx_fixture();
        let digest = sha256_digest(&image).expect("fixture must hash");
        let mut changed = image.clone();
        let last = changed.last_mut().expect("fixture is nonempty");
        *last ^= 1;

        assert_ne!(
            digest,
            sha256_digest(&changed).expect("changed fixture must hash")
        );
        assert_eq!(digest.as_str().len(), Sha256Digest::HEX_LENGTH);
    }

    #[test]
    fn emits_exactly_three_canonical_records() {
        let image = builtin_rpx_fixture();
        let parsed = parse_rpx(&image).expect("builtin fixture must parse");
        let trace = build_trace(&image, &parsed).expect("trace must build");
        let entries = TraceReader::new(BufReader::new(Cursor::new(trace)))
            .read_all()
            .expect("trace must parse");

        assert_eq!(entries.len(), 3);
        assert_eq!(entries[0].source, TraceSource::Cafe);
        assert_eq!(entries[0].category, TraceCategory::System);
        assert_eq!(entries[0].key().sequence, 0);
        let TraceEvent::Event(validated) = &entries[0].event else {
            panic!("first record must be a generic event");
        };
        assert_eq!(validated.name, "rpx-contract-validated");
        assert_eq!(validated.fields.len(), 6);
        assert_eq!(
            validated.fields.get("entry_point"),
            Some(&TraceValue::Hex32(HexU32::from(TEXT_SECTION_ADDRESS)))
        );
        assert_eq!(
            validated.fields.get("file_info_flags"),
            Some(&TraceValue::Hex32(HexU32::from(2)))
        );
        assert_eq!(
            validated.fields.get("fixture"),
            Some(&TraceValue::Text(SYNTHETIC_FIXTURE.to_owned()))
        );
        assert_eq!(
            validated.fields.get("image_bytes"),
            Some(&TraceValue::Unsigned(DecimalU64::from(
                u64::try_from(image.len()).expect("fixture length fits in u64"),
            )))
        );
        assert_eq!(
            validated.fields.get("program_sha256"),
            Some(&TraceValue::Sha256(
                sha256_digest(&image).expect("fixture must hash")
            ))
        );
        assert_eq!(
            validated.fields.get("section_count"),
            Some(&TraceValue::Unsigned(DecimalU64::from(5)))
        );
        assert_eq!(entries[1].key().sequence, 1);
        let TraceEvent::Event(mapping) = &entries[1].event else {
            panic!("second record must be a generic event");
        };
        assert_eq!(mapping.name, "rpx-section-mapping");
        assert_eq!(mapping.fields.len(), 6);
        assert_eq!(
            mapping.fields.get("byte_length"),
            Some(&TraceValue::Unsigned(DecimalU64::from(
                u64::try_from(TEXT_SECTION_LENGTH).expect("section length fits in u64"),
            )))
        );
        assert_eq!(
            mapping.fields.get("flags"),
            Some(&TraceValue::Hex32(HexU32::from(TEXT_SECTION_FLAGS)))
        );
        assert_eq!(
            mapping.fields.get("guest_address"),
            Some(&TraceValue::Hex32(HexU32::from(TEXT_SECTION_ADDRESS)))
        );
        assert_eq!(
            mapping.fields.get("section_index"),
            Some(&TraceValue::Unsigned(DecimalU64::from(
                u64::try_from(TEXT_SECTION_INDEX).expect("section index fits in u64"),
            )))
        );
        assert_eq!(
            mapping.fields.get("region"),
            Some(&TraceValue::Text("text".to_owned()))
        );
        assert_eq!(entries[2].key().sequence, 2);
        let TraceEvent::Terminal(terminal) = &entries[2].event else {
            panic!("third record must be terminal");
        };
        assert_eq!(terminal.reason, StopReason::TestCompleted);
        assert_eq!(terminal.guest_exit_code, None);
        assert_eq!(
            terminal.detail_code.as_deref(),
            Some("rpx-parse-map-contract-v1")
        );
    }

    #[test]
    fn errors_do_not_echo_fixture_paths() {
        let secret = OsString::from("/private/rpx-contract-secret.rpx");
        let error = Options::parse([OsString::from("trace"), secret.clone()])
            .expect_err("positional fixture must fail");

        assert_eq!(error, ContractTraceError::Usage);
        assert!(
            !error
                .to_string()
                .contains(secret.to_string_lossy().as_ref())
        );
        assert!(!format!("{error:?}").contains(secret.to_string_lossy().as_ref()));
    }

    #[test]
    fn bounded_reader_rejects_one_byte_over_the_limit() {
        let mut reader = Cursor::new(b"123456789".as_slice());

        assert_eq!(
            read_bounded_from_reader(&mut reader, 8),
            Err(ContractTraceError::Input)
        );
    }

    #[test]
    fn bounded_reader_accepts_the_exact_limit() {
        let mut reader = Cursor::new(b"12345678".as_slice());

        assert_eq!(
            read_bounded_from_reader(&mut reader, 8),
            Ok(b"12345678".to_vec())
        );
    }

    #[test]
    fn publication_never_clobbers_an_existing_trace() {
        let directory = tempfile::tempdir().expect("temporary directory must be created");
        let destination = directory.path().join("existing.jsonl");
        fs::write(&destination, b"preserve").expect("existing trace must be written");

        assert_eq!(
            publish_trace(destination.as_os_str(), b"replacement\n"),
            Err(ContractTraceError::Output)
        );
        assert_eq!(
            fs::read(&destination).expect("trace must remain readable"),
            b"preserve"
        );
    }

    #[test]
    fn existing_fixture_cannot_be_its_own_trace_destination() {
        let fixture = tempfile::NamedTempFile::new().expect("temporary fixture must be created");

        assert_eq!(
            validate_trace_destination(fixture.path().as_os_str(), fixture.path().as_os_str()),
            Err(ContractTraceError::InputOutputAlias)
        );
    }
}
