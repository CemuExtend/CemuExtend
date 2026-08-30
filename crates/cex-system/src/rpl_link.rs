//! Transactional planning and commit for the first bounded RPX-to-RPL link slice.
//!
//! The supported shape is deliberately small: one main RPX, one named provider
//! RPL, one import, one export, and one relocation in each module.  Provider
//! export descriptors use `ADDR32`; main imports use either `ADDR32` data
//! patches or near `REL24` function branches.  A plan owns every byte and
//! computed patch value, so committing never consults caller-owned state and
//! never publishes partially initialized memory.

use std::{collections::BTreeMap, error::Error, fmt};

use cex_memory::{GuestMemory, MemoryFault, PAGE_SIZE, Permissions};
use cex_types::GuestAddress;
use sha2::{Digest, Sha256};

use crate::rpx::{
    CafeRelocation, CafeRelocationKind, CafeSymbol, CafeSymbolKind, ParsedRpl, ParsedRpx,
    RpxFileInfo, RpxMappingRegion, RpxSection,
};

const CODE_POOL_START: u64 = 0x0200_0000;
const CODE_POOL_END: u64 = 0x1000_0000;
const DATA_POOL_START: u64 = 0x1000_0000;
const DATA_POOL_END: u64 = 0x4000_0000;
const MAX_MODULE_NAME_BYTES: usize = 63;
const MAX_LINK_BYTES: u64 = 64 * 1024 * 1024;

/// A validated module name used for exact import-provider matching.
///
/// The first slice accepts only an unambiguous canonical spelling: a non-empty
/// lowercase ASCII basename followed by exactly one `.rpl` extension.  This
/// deliberately rejects aliases which C++ basename/first-dot/lowercase
/// normalization could otherwise collapse together.  Debug and display
/// formatting are intentionally redacted.
#[derive(Clone, Eq, Hash, PartialEq)]
pub struct RplModuleName(String);

impl RplModuleName {
    /// Validate and own one exact RPL module name.
    pub fn new(name: &str) -> Result<Self, RplModuleNameError> {
        if name.is_empty() {
            return Err(RplModuleNameError::Empty);
        }
        if name.len() > MAX_MODULE_NAME_BYTES {
            return Err(RplModuleNameError::TooLong {
                maximum: MAX_MODULE_NAME_BYTES,
            });
        }
        let Some(basename) = name.strip_suffix(".rpl") else {
            return Err(RplModuleNameError::InvalidCharacter);
        };
        if basename.is_empty()
            || basename.bytes().any(|byte| {
                !(byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'_' | b'-'))
            })
        {
            return Err(RplModuleNameError::InvalidCharacter);
        }

        let mut owned = String::new();
        owned
            .try_reserve_exact(name.len())
            .map_err(|_| RplModuleNameError::AllocationFailed {
                requested: name.len(),
            })?;
        owned.push_str(name);
        Ok(Self(owned))
    }

    fn matches(&self, candidate: &str) -> bool {
        self.0 == candidate
    }
}

impl TryFrom<&str> for RplModuleName {
    type Error = RplModuleNameError;

    fn try_from(value: &str) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl fmt::Debug for RplModuleName {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("RplModuleName(<redacted>)")
    }
}

impl fmt::Display for RplModuleName {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("<redacted RPL module name>")
    }
}

/// Failure to construct a bounded [`RplModuleName`].
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum RplModuleNameError {
    /// The name contains no bytes.
    Empty,
    /// The name exceeds the public bound.
    TooLong {
        /// Maximum accepted byte count.
        maximum: usize,
    },
    /// The name is not an unambiguous lowercase ASCII `.rpl` file name.
    InvalidCharacter,
    /// The host allocator rejected the bounded name allocation.
    AllocationFailed {
        /// Requested byte count.
        requested: usize,
    },
}

impl fmt::Display for RplModuleNameError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Empty => formatter.write_str("RPL module name is empty"),
            Self::TooLong { maximum } => {
                write!(formatter, "RPL module name exceeds {maximum} bytes")
            }
            Self::InvalidCharacter => {
                formatter.write_str("RPL module name contains an unsupported character")
            }
            Self::AllocationFailed { requested } => {
                write!(
                    formatter,
                    "failed to allocate {requested} module-name bytes"
                )
            }
        }
    }
}

impl fmt::Debug for RplModuleNameError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, formatter)
    }
}

impl Error for RplModuleNameError {}

/// Which input module caused a bounded planning failure.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpxRplLinkModule {
    /// The main executable.
    Main,
    /// The single provider library.
    Provider,
}

/// Which record class had an unsupported count in the first slice.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpxRplLinkRecord {
    /// Imported-module records.
    Import,
    /// Export descriptors.
    Export,
    /// Symbol-table entries other than the null symbol.
    Symbol,
    /// Relocation records.
    Relocation,
}

/// Deterministic placement pool or final mapping class.
#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum RpxRplLinkRegion {
    /// Executable text.
    Text,
    /// Writable data.
    Data,
    /// Read-only loader metadata.
    Loader,
}

/// Phase in which a planned patch is applied.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpxRplLinkPhase {
    /// Provider-local symbol resolution and export relocation.
    Local,
    /// Main import resolution and data or near-branch relocation.
    Import,
}

#[derive(Clone, Copy)]
enum FileInfoAdjustment {
    Trampoline,
    Loader,
}

/// A bounded, redacted failure produced before guest memory is mutated.
#[derive(Clone, Eq, PartialEq)]
pub enum RpxRplPlanError {
    /// The exact first-slice record count was not present.
    UnsupportedRecordCount {
        /// Affected module.
        module: RpxRplLinkModule,
        /// Affected record class.
        record: RpxRplLinkRecord,
        /// Observed bounded count.
        actual: usize,
        /// Required count.
        expected: usize,
    },
    /// A nonzero FILEINFO adjustment requires placement semantics outside the
    /// first bounded slice.
    UnsupportedTrampolineAdjustment {
        /// Affected module.
        module: RpxRplLinkModule,
    },
    /// A nonzero FILEINFO loader adjustment requires placement semantics
    /// outside the first bounded slice.
    UnsupportedLoaderAdjustment {
        /// Affected module.
        module: RpxRplLinkModule,
    },
    /// Declared regions or owned section payloads exceed 64 MiB in aggregate.
    AggregateLimitExceeded,
    /// A deterministic placement pool cannot contain the next region.
    AddressPoolExhausted {
        /// Exhausted pool.
        region: RpxRplLinkRegion,
    },
    /// Guest address translation overflowed.
    AddressOverflow {
        /// Affected region.
        region: RpxRplLinkRegion,
    },
    /// A parsed section has no corresponding deterministic region placement.
    InvalidRegionPlacement {
        /// Section-table index.
        section_index: usize,
        /// Required region.
        region: RpxRplLinkRegion,
    },
    /// Two planned mappings overlap.
    MappingConflict {
        /// First byte of the conflicting page.
        address: u32,
    },
    /// Page-granular permission union would be writable and executable.
    PageWriteExecute {
        /// First byte of the affected page.
        address: u32,
    },
    /// The provider name does not exactly match the main import record.
    UnresolvedImport {
        /// Main import section index.
        section_index: usize,
        /// Requested import kind.
        kind: CafeSymbolKind,
    },
    /// Import and export symbol classes differ.
    ImportKindMismatch {
        /// Main import section index.
        section_index: usize,
    },
    /// More than one symbol can satisfy a required indexed lookup.
    AmbiguousSymbol {
        /// Symbol-table section index.
        table_section_index: usize,
        /// Symbol index within that table.
        symbol_index: usize,
    },
    /// A relocation references no retained parsed symbol.
    MissingSymbol {
        /// Symbol-table section index.
        table_section_index: usize,
        /// Symbol index within that table.
        symbol_index: usize,
    },
    /// The provider export has no exact local descriptor relocation.
    ExportDescriptorNotRelocated {
        /// Export section index.
        section_index: usize,
    },
    /// The first slice requires the provider descriptor to begin at zero and
    /// receive its value exclusively from the local relocation phase.
    PreRelocatedExportUnsupported {
        /// Export section index.
        section_index: usize,
    },
    /// A provider local symbol cannot define the sole export.
    InvalidExportAddress {
        /// Export section index.
        section_index: usize,
    },
    /// A relocation does not use its operation's supported target class.
    InvalidRelocationTarget {
        /// Relocation section index.
        section_index: usize,
        /// Target section index.
        target_section_index: usize,
    },
    /// A near branch relocation site is not instruction aligned.
    Rel24SiteUnaligned {
        /// Mapped guest patch address.
        address: u32,
    },
    /// A near branch relocation does not target a relative branch instruction.
    Rel24InvalidInstruction {
        /// Mapped guest patch address.
        address: u32,
        /// Original instruction word.
        instruction: u32,
    },
    /// The modular near branch displacement is not a multiple of four.
    Rel24DisplacementUnaligned {
        /// Mapped guest patch address.
        address: u32,
        /// Signed modular displacement.
        displacement: i32,
    },
    /// The modular displacement cannot be represented by a near `REL24`
    /// branch.  This bounded slice does not synthesize a trampoline.
    Rel24OutOfRange {
        /// Mapped guest patch address.
        address: u32,
        /// Signed modular displacement.
        displacement: i32,
    },
    /// Two planned patches overlap.
    PatchConflict {
        /// First conflicting guest byte.
        address: u32,
    },
    /// A bounded host allocation failed while owning the complete plan.
    AllocationFailed {
        /// Requested byte or element count.
        requested: usize,
    },
}

impl fmt::Display for RpxRplPlanError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnsupportedRecordCount {
                module,
                record,
                actual,
                expected,
            } => write!(
                formatter,
                "{module:?} has {actual} {record:?} records; first slice requires {expected}"
            ),
            Self::UnsupportedTrampolineAdjustment { module } => write!(
                formatter,
                "{module:?} has a nonzero trampoline adjustment; first slice requires zero"
            ),
            Self::UnsupportedLoaderAdjustment { module } => write!(
                formatter,
                "{module:?} has a nonzero loader adjustment; first slice requires zero"
            ),
            Self::AggregateLimitExceeded => {
                formatter.write_str("RPX/RPL link aggregate exceeds 64 MiB")
            }
            Self::AddressPoolExhausted { region } => {
                write!(formatter, "{region:?} placement pool is exhausted")
            }
            Self::AddressOverflow { region } => {
                write!(formatter, "{region:?} guest address translation overflowed")
            }
            Self::InvalidRegionPlacement {
                section_index,
                region,
            } => write!(
                formatter,
                "section {section_index} has no valid {region:?} placement"
            ),
            Self::MappingConflict { address } => {
                write!(formatter, "planned mappings conflict at 0x{address:08x}")
            }
            Self::PageWriteExecute { address } => {
                write!(
                    formatter,
                    "planned page 0x{address:08x} is writable and executable"
                )
            }
            Self::UnresolvedImport {
                section_index,
                kind,
            } => write!(
                formatter,
                "main import in section {section_index} has no exact {kind:?} provider"
            ),
            Self::ImportKindMismatch { section_index } => {
                write!(
                    formatter,
                    "main import kind mismatches section {section_index} export"
                )
            }
            Self::AmbiguousSymbol {
                table_section_index,
                symbol_index,
            } => write!(
                formatter,
                "symbol {symbol_index} in table {table_section_index} is ambiguous"
            ),
            Self::MissingSymbol {
                table_section_index,
                symbol_index,
            } => write!(
                formatter,
                "symbol {symbol_index} is absent from table {table_section_index}"
            ),
            Self::ExportDescriptorNotRelocated { section_index } => write!(
                formatter,
                "provider export descriptor in section {section_index} lacks a local relocation"
            ),
            Self::PreRelocatedExportUnsupported { section_index } => write!(
                formatter,
                "provider export descriptor in section {section_index} is already populated"
            ),
            Self::InvalidExportAddress { section_index } => write!(
                formatter,
                "provider export descriptor in section {section_index} has no exact local symbol"
            ),
            error @ (Self::InvalidRelocationTarget { .. }
            | Self::Rel24SiteUnaligned { .. }
            | Self::Rel24InvalidInstruction { .. }
            | Self::Rel24DisplacementUnaligned { .. }
            | Self::Rel24OutOfRange { .. }
            | Self::PatchConflict { .. }) => fmt_relocation_plan_error(error, formatter),
            Self::AllocationFailed { requested } => {
                write!(
                    formatter,
                    "failed to allocate bounded link plan storage for {requested}"
                )
            }
        }
    }
}

fn fmt_relocation_plan_error(
    error: &RpxRplPlanError,
    formatter: &mut fmt::Formatter<'_>,
) -> fmt::Result {
    match error {
        RpxRplPlanError::InvalidRelocationTarget {
            section_index,
            target_section_index,
        } => write!(
            formatter,
            "relocation section {section_index} has unsupported target {target_section_index}"
        ),
        RpxRplPlanError::Rel24SiteUnaligned { address } => {
            write!(
                formatter,
                "REL24 site 0x{address:08x} is not 4-byte aligned"
            )
        }
        RpxRplPlanError::Rel24InvalidInstruction {
            address,
            instruction,
        } => write!(
            formatter,
            "REL24 site 0x{address:08x} has invalid instruction 0x{instruction:08x}"
        ),
        RpxRplPlanError::Rel24DisplacementUnaligned {
            address,
            displacement,
        } => write!(
            formatter,
            "REL24 site 0x{address:08x} has unaligned displacement {displacement}"
        ),
        RpxRplPlanError::Rel24OutOfRange {
            address,
            displacement,
        } => write!(
            formatter,
            "REL24 site 0x{address:08x} has out-of-range displacement {displacement}"
        ),
        RpxRplPlanError::PatchConflict { address } => {
            write!(formatter, "planned patches conflict at 0x{address:08x}")
        }
        _ => unreachable!("only relocation errors are delegated"),
    }
}

impl fmt::Debug for RpxRplPlanError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, formatter)
    }
}

impl Error for RpxRplPlanError {}

#[derive(Clone, Copy)]
struct RegionPlacement {
    original_base: u32,
    mapped_base: u32,
    range_start: u32,
    range_len: u64,
}

#[derive(Clone, Copy, Default)]
struct ModulePlacement {
    text: Option<RegionPlacement>,
    data: Option<RegionPlacement>,
    loader: Option<RegionPlacement>,
}

impl ModulePlacement {
    const fn get(self, region: RpxMappingRegion) -> Option<RegionPlacement> {
        match region {
            RpxMappingRegion::Text => self.text,
            RpxMappingRegion::Data => self.data,
            RpxMappingRegion::Loader => self.loader,
        }
    }
}

struct PlannedSection {
    destination: u32,
    bytes: Vec<u8>,
}

#[derive(Clone, Copy)]
struct PlannedRange {
    start: u32,
    len: u64,
    region: RpxRplLinkRegion,
}

#[derive(Clone, Copy)]
struct PlannedPatch {
    phase: RpxRplLinkPhase,
    kind: CafeRelocationKind,
    site: u32,
    before: u32,
    after: u32,
    resolved_symbol: u32,
    addend: i32,
    displacement: Option<i32>,
}

#[derive(Clone, Copy)]
struct PatchSite {
    address: u32,
    before: u32,
}

#[derive(Clone, Copy)]
struct PatchSiteSpec {
    kind: CafeRelocationKind,
    region: RpxMappingRegion,
    width: u64,
}

/// Fully validated and owned RPX/RPL placement and relocation work.
///
/// Fields are private so callers cannot invalidate the transaction between
/// planning and commit.
pub struct RpxRplLinkPlan {
    sections: Vec<PlannedSection>,
    ranges: Vec<PlannedRange>,
    local_patch: PlannedPatch,
    import_patch: PlannedPatch,
    main_entry: u32,
    mapped_page_count: u64,
    mapped_byte_count: u64,
    main_hash: [u8; 32],
    provider_hash: [u8; 32],
}

impl fmt::Debug for RpxRplLinkPlan {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpxRplLinkPlan")
            .field("section_count", &self.sections.len())
            .field("range_count", &self.ranges.len())
            .field("main_entry", &self.main_entry)
            .field("local_patch_site", &self.local_patch.site)
            .field("import_patch_site", &self.import_patch.site)
            .finish_non_exhaustive()
    }
}

/// Validate, place, resolve, and own a main RPX plus one provider RPL.
pub fn plan_rpx_rpl_link(
    main: ParsedRpx,
    provider_name: RplModuleName,
    provider: ParsedRpl,
) -> Result<RpxRplLinkPlan, RpxRplPlanError> {
    let result = plan_rpx_rpl_link_borrowed(&main, &provider_name, &provider);
    drop((main, provider_name, provider));
    result
}

fn plan_rpx_rpl_link_borrowed(
    main: &ParsedRpx,
    provider_name: &RplModuleName,
    provider: &ParsedRpl,
) -> Result<RpxRplLinkPlan, RpxRplPlanError> {
    validate_shape(main, provider)?;
    validate_aggregate(main, provider)?;

    let main_hash = hash_main(main);
    let provider_hash = hash_provider(provider);
    let mut code_cursor = CODE_POOL_START;
    let mut data_cursor = DATA_POOL_START;
    let mut ranges = new_vec(6)?;
    let main_placement = place_module(
        main.sections(),
        main.file_info(),
        &mut code_cursor,
        &mut data_cursor,
        &mut ranges,
    )?;
    let provider_placement = place_module(
        provider.sections(),
        provider.file_info(),
        &mut code_cursor,
        &mut data_cursor,
        &mut ranges,
    )?;
    let (mapped_page_count, mapped_byte_count) = validate_ranges(&ranges)?;

    let mut sections = new_vec(
        main.sections()
            .len()
            .saturating_add(provider.sections().len()),
    )?;
    own_allocated_sections(main.sections(), main_placement, &mut sections)?;
    own_allocated_sections(provider.sections(), provider_placement, &mut sections)?;

    let main_entry = translate_address(main.sections(), main_placement, main.entry_point())?;
    let local_patch = plan_local_patch(provider, provider_placement)?;
    let import_patch = plan_import_patch(
        main,
        main_placement,
        provider,
        provider_name,
        local_patch.after,
    )?;
    validate_patch_conflict(local_patch, import_patch)?;

    Ok(RpxRplLinkPlan {
        sections,
        ranges,
        local_patch,
        import_patch,
        main_entry,
        mapped_page_count,
        mapped_byte_count,
        main_hash,
        provider_hash,
    })
}

fn validate_shape(main: &ParsedRpx, provider: &ParsedRpl) -> Result<(), RpxRplPlanError> {
    exact_count(
        RpxRplLinkModule::Main,
        RpxRplLinkRecord::Import,
        main.imports().len(),
        1,
    )?;
    exact_count(
        RpxRplLinkModule::Main,
        RpxRplLinkRecord::Export,
        main.exports().len(),
        0,
    )?;
    exact_count(
        RpxRplLinkModule::Main,
        RpxRplLinkRecord::Symbol,
        main.symbols().len(),
        1,
    )?;
    exact_count(
        RpxRplLinkModule::Main,
        RpxRplLinkRecord::Relocation,
        main.relocations().len(),
        1,
    )?;
    exact_count(
        RpxRplLinkModule::Provider,
        RpxRplLinkRecord::Import,
        provider.imports().len(),
        0,
    )?;
    exact_count(
        RpxRplLinkModule::Provider,
        RpxRplLinkRecord::Export,
        provider.exports().len(),
        1,
    )?;
    validate_unrelocated_export(
        provider.exports()[0].section_index(),
        provider.exports()[0].address(),
    )?;
    exact_count(
        RpxRplLinkModule::Provider,
        RpxRplLinkRecord::Symbol,
        provider.symbols().len(),
        1,
    )?;
    exact_count(
        RpxRplLinkModule::Provider,
        RpxRplLinkRecord::Relocation,
        provider.relocations().len(),
        1,
    )?;
    validate_adjustments(RpxRplLinkModule::Main, main.file_info())?;
    validate_adjustments(RpxRplLinkModule::Provider, provider.file_info())
}

fn validate_adjustments(
    module: RpxRplLinkModule,
    file_info: &RpxFileInfo,
) -> Result<(), RpxRplPlanError> {
    validate_adjustment(
        module,
        FileInfoAdjustment::Trampoline,
        file_info.trampoline_adjustment(),
    )?;
    validate_adjustment(
        module,
        FileInfoAdjustment::Loader,
        file_info.loader_adjustment(),
    )
}

fn validate_adjustment(
    module: RpxRplLinkModule,
    adjustment: FileInfoAdjustment,
    value: u32,
) -> Result<(), RpxRplPlanError> {
    if value != 0 {
        return Err(match adjustment {
            FileInfoAdjustment::Trampoline => {
                RpxRplPlanError::UnsupportedTrampolineAdjustment { module }
            }
            FileInfoAdjustment::Loader => RpxRplPlanError::UnsupportedLoaderAdjustment { module },
        });
    }
    Ok(())
}

fn exact_count(
    module: RpxRplLinkModule,
    record: RpxRplLinkRecord,
    actual: usize,
    expected: usize,
) -> Result<(), RpxRplPlanError> {
    if actual != expected {
        return Err(RpxRplPlanError::UnsupportedRecordCount {
            module,
            record,
            actual,
            expected,
        });
    }
    Ok(())
}

fn validate_aggregate(main: &ParsedRpx, provider: &ParsedRpl) -> Result<(), RpxRplPlanError> {
    let payload_bytes = main
        .sections()
        .iter()
        .chain(provider.sections())
        .try_fold(0_u64, |total, section| {
            total.checked_add(u64::try_from(section.data().len()).ok()?)
        })
        .ok_or(RpxRplPlanError::AggregateLimitExceeded)?;
    let region_bytes = [main.file_info(), provider.file_info()]
        .into_iter()
        .try_fold(0_u64, |total, info| {
            total
                .checked_add(u64::from(info.text_region_size()))?
                .checked_add(u64::from(info.data_region_size()))?
                .checked_add(u64::from(info.loader_region_size()))
        })
        .ok_or(RpxRplPlanError::AggregateLimitExceeded)?;
    if payload_bytes > MAX_LINK_BYTES || region_bytes > MAX_LINK_BYTES {
        return Err(RpxRplPlanError::AggregateLimitExceeded);
    }
    Ok(())
}

fn place_module(
    sections: &[RpxSection],
    file_info: &RpxFileInfo,
    code_cursor: &mut u64,
    data_cursor: &mut u64,
    ranges: &mut Vec<PlannedRange>,
) -> Result<ModulePlacement, RpxRplPlanError> {
    let mut placement = ModulePlacement::default();
    if let Some(original_base) = original_region_base(sections, RpxMappingRegion::Text) {
        let adjustment = u64::from(file_info.trampoline_adjustment());
        let placed = allocate_region(
            code_cursor,
            CODE_POOL_END,
            file_info.text_region_size(),
            file_info.text_alignment(),
            adjustment,
            PAGE_SIZE,
            RpxRplLinkRegion::Text,
        )?;
        placement.text = Some(RegionPlacement {
            original_base,
            ..placed
        });
        ranges.push(PlannedRange {
            start: placed.range_start,
            len: placed.range_len,
            region: RpxRplLinkRegion::Text,
        });
    }
    if let Some(original_base) = original_region_base(sections, RpxMappingRegion::Data) {
        let placed = allocate_region(
            data_cursor,
            DATA_POOL_END,
            file_info.data_region_size(),
            file_info.data_alignment(),
            0,
            0,
            RpxRplLinkRegion::Data,
        )?;
        placement.data = Some(RegionPlacement {
            original_base,
            ..placed
        });
        ranges.push(PlannedRange {
            start: placed.range_start,
            len: placed.range_len,
            region: RpxRplLinkRegion::Data,
        });
    }
    if let Some(original_base) = original_region_base(sections, RpxMappingRegion::Loader) {
        let placed = allocate_region(
            data_cursor,
            DATA_POOL_END,
            file_info.loader_region_size(),
            0,
            0,
            0,
            RpxRplLinkRegion::Loader,
        )?;
        placement.loader = Some(RegionPlacement {
            original_base,
            ..placed
        });
        ranges.push(PlannedRange {
            start: placed.range_start,
            len: placed.range_len,
            region: RpxRplLinkRegion::Loader,
        });
    }
    Ok(placement)
}

fn allocate_region(
    cursor: &mut u64,
    pool_end: u64,
    declared_size: u32,
    declared_alignment: u32,
    mapped_adjustment: u64,
    cursor_extra: u64,
    region: RpxRplLinkRegion,
) -> Result<RegionPlacement, RpxRplPlanError> {
    let alignment = PAGE_SIZE.max(u64::from(declared_alignment.max(1)));
    let range_start =
        align_up(*cursor, alignment).ok_or(RpxRplPlanError::AddressPoolExhausted { region })?;
    let range_len = align_up(u64::from(declared_size), PAGE_SIZE)
        .filter(|len| *len != 0)
        .ok_or(RpxRplPlanError::AddressPoolExhausted { region })?;
    let allocation_len = align_up(
        u64::from(declared_size)
            .checked_add(cursor_extra)
            .ok_or(RpxRplPlanError::AddressPoolExhausted { region })?,
        PAGE_SIZE,
    )
    .ok_or(RpxRplPlanError::AddressPoolExhausted { region })?;
    let next = range_start
        .checked_add(allocation_len)
        .filter(|end| *end <= pool_end)
        .ok_or(RpxRplPlanError::AddressPoolExhausted { region })?;
    let mapped_base = range_start
        .checked_add(mapped_adjustment)
        .ok_or(RpxRplPlanError::AddressOverflow { region })?;
    let mapped_base =
        u32::try_from(mapped_base).map_err(|_| RpxRplPlanError::AddressOverflow { region })?;
    *cursor = next;
    Ok(RegionPlacement {
        original_base: 0,
        mapped_base,
        range_start: u32::try_from(range_start)
            .map_err(|_| RpxRplPlanError::AddressOverflow { region })?,
        range_len,
    })
}

fn original_region_base(sections: &[RpxSection], region: RpxMappingRegion) -> Option<u32> {
    sections
        .iter()
        .filter(|section| section.mapping_region() == Some(region))
        .map(RpxSection::virtual_address)
        .min()
}

fn validate_ranges(ranges: &[PlannedRange]) -> Result<(u64, u64), RpxRplPlanError> {
    let mut pages = BTreeMap::<u32, Permissions>::new();
    let mut mapped_bytes = 0_u64;
    for range in ranges {
        mapped_bytes = mapped_bytes
            .checked_add(range.len)
            .ok_or(RpxRplPlanError::AggregateLimitExceeded)?;
        if mapped_bytes > MAX_LINK_BYTES {
            return Err(RpxRplPlanError::AggregateLimitExceeded);
        }
        let first = u64::from(range.start) / PAGE_SIZE;
        let count = range.len / PAGE_SIZE;
        for raw_page in first..first + count {
            let page = u32::try_from(raw_page).map_err(|_| RpxRplPlanError::AddressOverflow {
                region: range.region,
            })?;
            let permission = final_permissions(range.region);
            if let Some(existing) = pages.insert(page, permission) {
                let address = u32::try_from(raw_page * PAGE_SIZE).map_err(|_| {
                    RpxRplPlanError::AddressOverflow {
                        region: range.region,
                    }
                })?;
                if (existing | permission).contains(Permissions::WRITE | Permissions::EXECUTE) {
                    return Err(RpxRplPlanError::PageWriteExecute { address });
                }
                return Err(RpxRplPlanError::MappingConflict { address });
            }
        }
    }
    let page_count =
        u64::try_from(pages.len()).map_err(|_| RpxRplPlanError::AggregateLimitExceeded)?;
    Ok((page_count, mapped_bytes))
}

fn own_allocated_sections(
    source: &[RpxSection],
    placement: ModulePlacement,
    output: &mut Vec<PlannedSection>,
) -> Result<(), RpxRplPlanError> {
    for section in source
        .iter()
        .filter(|section| section.mapping_region().is_some())
    {
        let destination = translate_section(section, placement)?;
        let bytes = owned_bytes(section.data())?;
        output.push(PlannedSection { destination, bytes });
    }
    Ok(())
}

fn owned_bytes(source: &[u8]) -> Result<Vec<u8>, RpxRplPlanError> {
    let mut bytes = new_vec(source.len())?;
    bytes.extend_from_slice(source);
    Ok(bytes)
}

fn new_vec<T>(capacity: usize) -> Result<Vec<T>, RpxRplPlanError> {
    let mut output = Vec::new();
    output
        .try_reserve_exact(capacity)
        .map_err(|_| RpxRplPlanError::AllocationFailed {
            requested: capacity,
        })?;
    Ok(output)
}

fn translate_section(
    section: &RpxSection,
    placement: ModulePlacement,
) -> Result<u32, RpxRplPlanError> {
    let region = section
        .mapping_region()
        .ok_or(RpxRplPlanError::InvalidRegionPlacement {
            section_index: section.index(),
            region: RpxRplLinkRegion::Loader,
        })?;
    let placed = placement
        .get(region)
        .ok_or(RpxRplPlanError::InvalidRegionPlacement {
            section_index: section.index(),
            region: public_region(region),
        })?;
    let delta = section
        .virtual_address()
        .checked_sub(placed.original_base)
        .ok_or(RpxRplPlanError::InvalidRegionPlacement {
            section_index: section.index(),
            region: public_region(region),
        })?;
    placed
        .mapped_base
        .checked_add(delta)
        .ok_or(RpxRplPlanError::AddressOverflow {
            region: public_region(region),
        })
}

fn translate_address(
    sections: &[RpxSection],
    placement: ModulePlacement,
    address: u32,
) -> Result<u32, RpxRplPlanError> {
    let section = sections
        .iter()
        .filter(|section| section.mapping_region() == Some(RpxMappingRegion::Text))
        .find(|section| {
            let start = u64::from(section.virtual_address());
            u64::try_from(section.data().len())
                .ok()
                .and_then(|len| start.checked_add(len))
                .is_some_and(|end| u64::from(address) >= start && u64::from(address) < end)
        })
        .ok_or(RpxRplPlanError::AddressOverflow {
            region: RpxRplLinkRegion::Text,
        })?;
    let region = section
        .mapping_region()
        .ok_or(RpxRplPlanError::InvalidRegionPlacement {
            section_index: section.index(),
            region: RpxRplLinkRegion::Text,
        })?;
    let mapped = translate_section(section, placement)?;
    mapped
        .checked_add(address - section.virtual_address())
        .ok_or(RpxRplPlanError::AddressOverflow {
            region: public_region(region),
        })
}

fn translate_patch_site(
    sections: &[RpxSection],
    placement: ModulePlacement,
    relocation: &CafeRelocation,
    spec: PatchSiteSpec,
) -> Result<PatchSite, RpxRplPlanError> {
    let target = sections
        .get(relocation.target_section_index())
        .filter(|section| section.index() == relocation.target_section_index())
        .ok_or(RpxRplPlanError::InvalidRelocationTarget {
            section_index: relocation.section_index(),
            target_section_index: relocation.target_section_index(),
        })?;
    if relocation.kind() != spec.kind || target.mapping_region() != Some(spec.region) {
        return Err(RpxRplPlanError::InvalidRelocationTarget {
            section_index: relocation.section_index(),
            target_section_index: relocation.target_section_index(),
        });
    }
    let offset = relocation
        .offset()
        .checked_sub(target.virtual_address())
        .ok_or(RpxRplPlanError::InvalidRelocationTarget {
            section_index: relocation.section_index(),
            target_section_index: relocation.target_section_index(),
        })?;
    let end = u64::from(offset) + spec.width;
    let target_len = u64::try_from(target.data().len()).map_err(|_| {
        RpxRplPlanError::InvalidRelocationTarget {
            section_index: relocation.section_index(),
            target_section_index: relocation.target_section_index(),
        }
    })?;
    if end > target_len {
        return Err(RpxRplPlanError::InvalidRelocationTarget {
            section_index: relocation.section_index(),
            target_section_index: relocation.target_section_index(),
        });
    }
    let address = translate_section(target, placement)?
        .checked_add(offset)
        .ok_or(RpxRplPlanError::AddressOverflow {
            region: public_region(spec.region),
        })?;
    let offset = usize::try_from(offset).map_err(|_| RpxRplPlanError::InvalidRelocationTarget {
        section_index: relocation.section_index(),
        target_section_index: relocation.target_section_index(),
    })?;
    let before = target
        .data()
        .get(offset..offset + 4)
        .and_then(|bytes| <[u8; 4]>::try_from(bytes).ok())
        .map(u32::from_be_bytes)
        .ok_or(RpxRplPlanError::InvalidRelocationTarget {
            section_index: relocation.section_index(),
            target_section_index: relocation.target_section_index(),
        })?;
    Ok(PatchSite { address, before })
}

fn unique_symbol<'a>(
    symbols: &'a [CafeSymbol],
    relocation: &CafeRelocation,
) -> Result<&'a CafeSymbol, RpxRplPlanError> {
    let mut matches = symbols.iter().filter(|symbol| {
        symbol.table_section_index() == relocation.symbol_table_section_index()
            && symbol.symbol_index() == relocation.symbol_index()
    });
    let Some(symbol) = matches.next() else {
        return Err(RpxRplPlanError::MissingSymbol {
            table_section_index: relocation.symbol_table_section_index(),
            symbol_index: relocation.symbol_index(),
        });
    };
    if matches.next().is_some() {
        return Err(RpxRplPlanError::AmbiguousSymbol {
            table_section_index: relocation.symbol_table_section_index(),
            symbol_index: relocation.symbol_index(),
        });
    }
    Ok(symbol)
}

fn plan_local_patch(
    provider: &ParsedRpl,
    placement: ModulePlacement,
) -> Result<PlannedPatch, RpxRplPlanError> {
    let export = &provider.exports()[0];
    let relocation = &provider.relocations()[0];
    let export_section = provider
        .sections()
        .get(export.section_index())
        .filter(|section| section.index() == export.section_index())
        .ok_or(RpxRplPlanError::ExportDescriptorNotRelocated {
            section_index: export.section_index(),
        })?;
    let expected_offset = export_section.virtual_address().checked_add(8).ok_or(
        RpxRplPlanError::ExportDescriptorNotRelocated {
            section_index: export.section_index(),
        },
    )?;
    if relocation.target_section_index() != export.section_index()
        || relocation.offset() != expected_offset
    {
        return Err(RpxRplPlanError::ExportDescriptorNotRelocated {
            section_index: export.section_index(),
        });
    }
    let site = translate_patch_site(
        provider.sections(),
        placement,
        relocation,
        PatchSiteSpec {
            kind: CafeRelocationKind::Addr32,
            region: RpxMappingRegion::Loader,
            width: 4,
        },
    )?;
    let symbol = unique_symbol(provider.symbols(), relocation)?;
    if symbol.section_index() == 0
        || symbol.kind() != export.kind()
        || symbol.name() != export.name()
    {
        return Err(RpxRplPlanError::InvalidExportAddress {
            section_index: export.section_index(),
        });
    }
    let symbol_section = provider
        .sections()
        .get(symbol.section_index())
        .filter(|section| section.index() == symbol.section_index())
        .ok_or(RpxRplPlanError::InvalidExportAddress {
            section_index: export.section_index(),
        })?;
    let symbol_offset = symbol
        .value()
        .checked_sub(symbol_section.virtual_address())
        .ok_or(RpxRplPlanError::InvalidExportAddress {
            section_index: export.section_index(),
        })?;
    let symbol_region =
        symbol_section
            .mapping_region()
            .ok_or(RpxRplPlanError::InvalidExportAddress {
                section_index: export.section_index(),
            })?;
    let resolved = translate_section(symbol_section, placement)?
        .checked_add(symbol_offset)
        .ok_or(RpxRplPlanError::AddressOverflow {
            region: public_region(symbol_region),
        })?;
    let after = addr32_value(resolved, relocation.addend());
    Ok(PlannedPatch {
        phase: RpxRplLinkPhase::Local,
        kind: CafeRelocationKind::Addr32,
        site: site.address,
        before: site.before,
        after,
        resolved_symbol: resolved,
        addend: relocation.addend(),
        displacement: None,
    })
}

fn validate_unrelocated_export(
    section_index: usize,
    raw_address: u32,
) -> Result<(), RpxRplPlanError> {
    if raw_address != 0 {
        return Err(RpxRplPlanError::PreRelocatedExportUnsupported { section_index });
    }
    Ok(())
}

fn plan_import_patch(
    main: &ParsedRpx,
    placement: ModulePlacement,
    provider: &ParsedRpl,
    provider_name: &RplModuleName,
    resolved_export: u32,
) -> Result<PlannedPatch, RpxRplPlanError> {
    let import = &main.imports()[0];
    if !provider_name.matches(import.module_name()) {
        return Err(RpxRplPlanError::UnresolvedImport {
            section_index: import.section_index(),
            kind: import.kind(),
        });
    }
    let export = &provider.exports()[0];
    if import.kind() != export.kind() {
        return Err(RpxRplPlanError::ImportKindMismatch {
            section_index: import.section_index(),
        });
    }
    let relocation = &main.relocations()[0];
    let symbol = unique_symbol(main.symbols(), relocation)?;
    if symbol.section_index() != import.section_index()
        || symbol.kind() != import.kind()
        || symbol.name() != export.name()
    {
        return Err(RpxRplPlanError::UnresolvedImport {
            section_index: import.section_index(),
            kind: import.kind(),
        });
    }
    let spec = match relocation.kind() {
        CafeRelocationKind::Addr32 => PatchSiteSpec {
            kind: CafeRelocationKind::Addr32,
            region: RpxMappingRegion::Data,
            width: 4,
        },
        CafeRelocationKind::Rel24 if import.kind() == CafeSymbolKind::Function => PatchSiteSpec {
            kind: CafeRelocationKind::Rel24,
            region: RpxMappingRegion::Text,
            width: 4,
        },
        CafeRelocationKind::Rel24 => {
            return Err(RpxRplPlanError::InvalidRelocationTarget {
                section_index: relocation.section_index(),
                target_section_index: relocation.target_section_index(),
            });
        }
    };
    let site = translate_patch_site(main.sections(), placement, relocation, spec)?;
    let (after, displacement) = match relocation.kind() {
        CafeRelocationKind::Addr32 => (addr32_value(resolved_export, relocation.addend()), None),
        CafeRelocationKind::Rel24 => {
            let (after, displacement) = rel24_value(
                site.before,
                resolved_export,
                relocation.addend(),
                site.address,
            )?;
            (after, Some(displacement))
        }
    };
    Ok(PlannedPatch {
        phase: RpxRplLinkPhase::Import,
        kind: relocation.kind(),
        site: site.address,
        before: site.before,
        after,
        resolved_symbol: resolved_export,
        addend: relocation.addend(),
        displacement,
    })
}

fn validate_patch_conflict(
    local: PlannedPatch,
    import: PlannedPatch,
) -> Result<(), RpxRplPlanError> {
    let local_end = u64::from(local.site) + 4;
    let import_end = u64::from(import.site) + 4;
    if u64::from(local.site) < import_end && u64::from(import.site) < local_end {
        return Err(RpxRplPlanError::PatchConflict {
            address: local.site.max(import.site),
        });
    }
    Ok(())
}

const fn addr32_value(symbol: u32, addend: i32) -> u32 {
    symbol.wrapping_add(addend.cast_unsigned())
}

fn rel24_value(
    before: u32,
    symbol: u32,
    addend: i32,
    site: u32,
) -> Result<(u32, i32), RpxRplPlanError> {
    if site & 3 != 0 {
        return Err(RpxRplPlanError::Rel24SiteUnaligned { address: site });
    }
    if before >> 26 != 18 || before & 2 != 0 {
        return Err(RpxRplPlanError::Rel24InvalidInstruction {
            address: site,
            instruction: before,
        });
    }
    let displacement = symbol
        .wrapping_add(addend.cast_unsigned())
        .wrapping_sub(site)
        .cast_signed();
    if displacement & 3 != 0 {
        return Err(RpxRplPlanError::Rel24DisplacementUnaligned {
            address: site,
            displacement,
        });
    }
    if !(-0x0200_0000..=0x01ff_fffc).contains(&displacement) {
        return Err(RpxRplPlanError::Rel24OutOfRange {
            address: site,
            displacement,
        });
    }
    let after = (before & 0xfc00_0003) | (displacement.cast_unsigned() & 0x03ff_fffc);
    Ok((after, displacement))
}

const fn public_region(region: RpxMappingRegion) -> RpxRplLinkRegion {
    match region {
        RpxMappingRegion::Text => RpxRplLinkRegion::Text,
        RpxMappingRegion::Data => RpxRplLinkRegion::Data,
        RpxMappingRegion::Loader => RpxRplLinkRegion::Loader,
    }
}

fn final_permissions(region: RpxRplLinkRegion) -> Permissions {
    match region {
        RpxRplLinkRegion::Text => Permissions::READ | Permissions::EXECUTE,
        RpxRplLinkRegion::Data => Permissions::READ | Permissions::WRITE,
        RpxRplLinkRegion::Loader => Permissions::READ,
    }
}

fn align_up(value: u64, alignment: u64) -> Option<u64> {
    value
        .checked_add(alignment - 1)
        .map(|sum| sum & !(alignment - 1))
}

fn hash_main(module: &ParsedRpx) -> [u8; 32] {
    hash_module(
        &HashModuleParts {
            kind: b"RPX",
            entry: module.entry_point(),
            sections: module.sections(),
            file_info: module.file_info(),
            symbols: module.symbols(),
            relocations: module.relocations(),
        },
        module
            .imports()
            .iter()
            .map(|record| (record.section_index(), record.module_name(), record.kind())),
        module.exports().iter().map(|record| {
            (
                record.section_index(),
                record.name(),
                record.address(),
                record.kind(),
            )
        }),
    )
}

fn hash_provider(module: &ParsedRpl) -> [u8; 32] {
    hash_module(
        &HashModuleParts {
            kind: b"RPL",
            entry: module.entry_point(),
            sections: module.sections(),
            file_info: module.file_info(),
            symbols: module.symbols(),
            relocations: module.relocations(),
        },
        module
            .imports()
            .iter()
            .map(|record| (record.section_index(), record.module_name(), record.kind())),
        module.exports().iter().map(|record| {
            (
                record.section_index(),
                record.name(),
                record.address(),
                record.kind(),
            )
        }),
    )
}

struct HashModuleParts<'a> {
    kind: &'a [u8],
    entry: u32,
    sections: &'a [RpxSection],
    file_info: &'a RpxFileInfo,
    symbols: &'a [CafeSymbol],
    relocations: &'a [CafeRelocation],
}

fn hash_module<'a, I, E>(parts: &HashModuleParts<'_>, imports: I, exports: E) -> [u8; 32]
where
    I: IntoIterator<Item = (usize, &'a str, CafeSymbolKind)>,
    E: IntoIterator<Item = (usize, &'a str, u32, CafeSymbolKind)>,
{
    let HashModuleParts {
        kind,
        entry,
        sections,
        file_info,
        symbols,
        relocations,
    } = *parts;
    let mut hash = Sha256::new();
    hash.update(b"CemuExtend parsed Cafe module v2\0");
    hash.update(kind);
    hash.update(entry.to_be_bytes());
    hash.update(file_info.magic().to_be_bytes());
    hash.update(file_info.text_region_size().to_be_bytes());
    hash.update(file_info.text_alignment().to_be_bytes());
    hash.update(file_info.data_region_size().to_be_bytes());
    hash.update(file_info.data_alignment().to_be_bytes());
    hash.update(file_info.loader_region_size().to_be_bytes());
    hash.update(file_info.trampoline_adjustment().to_be_bytes());
    hash.update(file_info.loader_adjustment().to_be_bytes());
    hash.update(file_info.flags().to_be_bytes());
    for section in sections {
        hash.update(b"S");
        hash_usize(&mut hash, section.index());
        hash.update(section.section_type().to_be_bytes());
        hash.update(section.flags().to_be_bytes());
        hash.update(section.virtual_address().to_be_bytes());
        hash.update(section.alignment().to_be_bytes());
        hash_usize(&mut hash, section.data().len());
        hash.update(section.data());
    }
    for symbol in symbols {
        hash.update(b"Y");
        hash_usize(&mut hash, symbol.table_section_index());
        hash_usize(&mut hash, symbol.symbol_index());
        hash_usize(&mut hash, symbol.name().len());
        hash.update(symbol.name().as_bytes());
        hash.update(symbol.value().to_be_bytes());
        hash.update(symbol.size().to_be_bytes());
        hash_usize(&mut hash, symbol.section_index());
        hash.update([symbol_kind_byte(symbol.kind())]);
    }
    for (section, name, record_kind) in imports {
        hash.update(b"I");
        hash_usize(&mut hash, section);
        hash_usize(&mut hash, name.len());
        hash.update(name.as_bytes());
        hash.update([symbol_kind_byte(record_kind)]);
    }
    for (section, name, address, record_kind) in exports {
        hash.update(b"E");
        hash_usize(&mut hash, section);
        hash_usize(&mut hash, name.len());
        hash.update(name.as_bytes());
        hash.update(address.to_be_bytes());
        hash.update([symbol_kind_byte(record_kind)]);
    }
    for relocation in relocations {
        hash.update(b"R");
        hash_usize(&mut hash, relocation.section_index());
        hash_usize(&mut hash, relocation.target_section_index());
        hash_usize(&mut hash, relocation.symbol_table_section_index());
        hash.update(relocation.offset().to_be_bytes());
        hash_usize(&mut hash, relocation.symbol_index());
        hash.update(relocation.addend().to_be_bytes());
        hash.update([relocation_kind_byte(relocation.kind())]);
    }
    hash.finalize().into()
}

fn hash_usize(hash: &mut Sha256, value: usize) {
    hash.update((value as u64).to_be_bytes());
}

const fn symbol_kind_byte(kind: CafeSymbolKind) -> u8 {
    match kind {
        CafeSymbolKind::Function => 1,
        CafeSymbolKind::Data => 2,
    }
}

const fn relocation_kind_byte(kind: CafeRelocationKind) -> u8 {
    match kind {
        CafeRelocationKind::Addr32 => 1,
        CafeRelocationKind::Rel24 => 10,
    }
}

/// Commit failure after a complete link plan has been accepted.
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum RpxRplCommitError {
    /// Sparse guest-memory mapping, access, or protection failed.
    Memory(MemoryFault),
    /// Guest memory no longer contains the planned patch preimage.
    PreimageMismatch {
        /// Patch phase.
        phase: RpxRplLinkPhase,
        /// Guest patch address.
        address: u32,
    },
    /// A staged patch did not read back as planned.
    ProofMismatch {
        /// Patch phase.
        phase: RpxRplLinkPhase,
        /// Guest patch address.
        address: u32,
    },
}

impl fmt::Display for RpxRplCommitError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Memory(fault) => write!(formatter, "guest memory commit failed: {fault}"),
            Self::PreimageMismatch { phase, address } => {
                write!(
                    formatter,
                    "{phase:?} patch preimage mismatched at 0x{address:08x}"
                )
            }
            Self::ProofMismatch { phase, address } => {
                write!(
                    formatter,
                    "{phase:?} patch verification failed at 0x{address:08x}"
                )
            }
        }
    }
}

impl fmt::Debug for RpxRplCommitError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, formatter)
    }
}

impl Error for RpxRplCommitError {}

impl From<MemoryFault> for RpxRplCommitError {
    fn from(value: MemoryFault) -> Self {
        Self::Memory(value)
    }
}

/// Commit a validated plan into fresh memory and return only numeric/hash proof.
pub fn commit_rpx_rpl_link(plan: RpxRplLinkPlan) -> Result<RpxRplLinkProof, RpxRplCommitError> {
    let RpxRplLinkPlan {
        sections,
        ranges,
        local_patch,
        import_patch,
        main_entry,
        mapped_page_count,
        mapped_byte_count,
        main_hash,
        provider_hash,
    } = plan;
    let mut memory = GuestMemory::new();
    for range in &ranges {
        memory.map(
            GuestAddress::new(range.start),
            range.len,
            Permissions::READ | Permissions::WRITE,
        )?;
    }
    for section in &sections {
        memory.write(GuestAddress::new(section.destination), &section.bytes)?;
    }
    for patch in patches_in_commit_order(local_patch, import_patch) {
        apply_and_verify(&mut memory, patch)?;
    }
    for range in &ranges {
        memory.protect(
            GuestAddress::new(range.start),
            range.len,
            final_permissions(range.region),
        )?;
    }
    let memory_hash = memory.deterministic_hash();
    Ok(RpxRplLinkProof {
        main_entry,
        relocations: [
            RpxRplRelocationProof::from_patch(local_patch),
            RpxRplRelocationProof::from_patch(import_patch),
        ],
        mapped_page_count,
        mapped_byte_count,
        memory_hash,
        main_hash,
        provider_hash,
    })
}

/// Keep the C++ phase contract explicit: provider-local relocations become
/// visible before any main-module import relocation consumes their results.
fn patches_in_commit_order(local: PlannedPatch, import: PlannedPatch) -> [PlannedPatch; 2] {
    debug_assert_eq!(local.phase, RpxRplLinkPhase::Local);
    debug_assert_eq!(import.phase, RpxRplLinkPhase::Import);
    [local, import]
}

fn apply_and_verify(
    memory: &mut GuestMemory,
    patch: PlannedPatch,
) -> Result<(), RpxRplCommitError> {
    let address = GuestAddress::new(patch.site);
    if memory.read_u32(address)? != patch.before {
        return Err(RpxRplCommitError::PreimageMismatch {
            phase: patch.phase,
            address: patch.site,
        });
    }
    memory.write_u32(address, patch.after)?;
    if memory.read_u32(address)? != patch.after {
        return Err(RpxRplCommitError::ProofMismatch {
            phase: patch.phase,
            address: patch.site,
        });
    }
    Ok(())
}

/// Immutable numeric evidence for one committed relocation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RpxRplRelocationProof {
    phase: RpxRplLinkPhase,
    kind: CafeRelocationKind,
    site: u32,
    before: u32,
    after: u32,
    resolved_symbol: u32,
    addend: i32,
    displacement: Option<i32>,
}

impl RpxRplRelocationProof {
    const fn from_patch(patch: PlannedPatch) -> Self {
        Self {
            phase: patch.phase,
            kind: patch.kind,
            site: patch.site,
            before: patch.before,
            after: patch.after,
            resolved_symbol: patch.resolved_symbol,
            addend: patch.addend,
            displacement: patch.displacement,
        }
    }

    /// Return the phase in which this relocation was committed.
    pub const fn phase(&self) -> RpxRplLinkPhase {
        self.phase
    }

    /// Return the relocation operation.
    pub const fn kind(&self) -> CafeRelocationKind {
        self.kind
    }

    /// Return the guest patch address.
    pub const fn site(&self) -> u32 {
        self.site
    }

    /// Return the instruction or word observed before the patch.
    pub const fn before(&self) -> u32 {
        self.before
    }

    /// Return the instruction or word observed after the patch.
    pub const fn after(&self) -> u32 {
        self.after
    }

    /// Return the mapped symbol address used by the relocation formula.
    pub const fn resolved_symbol(&self) -> u32 {
        self.resolved_symbol
    }

    /// Return the signed relocation addend.
    pub const fn addend(&self) -> i32 {
        self.addend
    }

    /// Return the signed modular branch displacement for `REL24`, or `None`
    /// for `ADDR32`.
    pub const fn displacement(&self) -> Option<i32> {
        self.displacement
    }
}

/// Immutable numeric and hash evidence from a successful transactional commit.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RpxRplLinkProof {
    main_entry: u32,
    relocations: [RpxRplRelocationProof; 2],
    mapped_page_count: u64,
    mapped_byte_count: u64,
    memory_hash: [u8; 32],
    main_hash: [u8; 32],
    provider_hash: [u8; 32],
}

impl RpxRplLinkProof {
    /// Return the relocated main entry point.
    pub const fn main_entry(&self) -> u32 {
        self.main_entry
    }

    /// Return the complete bounded relocation proof in commit order.
    pub const fn relocations(&self) -> &[RpxRplRelocationProof; 2] {
        &self.relocations
    }

    /// Return the provider-local export descriptor patch site.
    pub const fn local_patch_site(&self) -> u32 {
        self.relocations[0].site
    }

    /// Return the provider-local export descriptor patch value.
    pub const fn local_patch_value(&self) -> u32 {
        self.relocations[0].after
    }

    /// Return the main import relocation patch site.
    pub const fn import_patch_site(&self) -> u32 {
        self.relocations[1].site
    }

    /// Return the main import relocation patch value.
    pub const fn import_patch_value(&self) -> u32 {
        self.relocations[1].after
    }

    /// Return the number of mapped logical pages.
    pub const fn mapped_page_count(&self) -> u64 {
        self.mapped_page_count
    }

    /// Return the number of mapped logical bytes.
    pub const fn mapped_byte_count(&self) -> u64 {
        self.mapped_byte_count
    }

    /// Return the deterministic hash of final ranges, permissions, and bytes.
    pub const fn memory_hash(&self) -> [u8; 32] {
        self.memory_hash
    }

    /// Return SHA-256 of the canonical parsed main-module representation.
    pub const fn main_sha256(&self) -> [u8; 32] {
        self.main_hash
    }

    /// Return SHA-256 of the canonical parsed provider-module representation.
    pub const fn provider_sha256(&self) -> [u8; 32] {
        self.provider_hash
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{parse_rpl, parse_rpx, rpl_link_fixture};

    fn fixture_u16(bytes: &[u8], offset: usize) -> u16 {
        u16::from_be_bytes([bytes[offset], bytes[offset + 1]])
    }

    fn fixture_u32(bytes: &[u8], offset: usize) -> u32 {
        u32::from_be_bytes([
            bytes[offset],
            bytes[offset + 1],
            bytes[offset + 2],
            bytes[offset + 3],
        ])
    }

    fn fixture_crc32(bytes: &[u8]) -> u32 {
        let mut crc = u32::MAX;
        for &byte in bytes {
            crc ^= u32::from(byte);
            for _ in 0..8 {
                crc = if crc & 1 == 0 {
                    crc >> 1
                } else {
                    (crc >> 1) ^ 0xedb8_8320
                };
            }
        }
        !crc
    }

    fn set_fixture_adjustment(image: &mut [u8], field_offset: usize, value: u32) {
        let section_table = usize::try_from(fixture_u32(image, 0x20)).expect("ELF32 offset fits");
        let section_size = usize::from(fixture_u16(image, 0x2e));
        let section_count = usize::from(fixture_u16(image, 0x30));
        let file_info_index = section_count - 1;
        let file_info_header = section_table + file_info_index * section_size;
        let file_info_offset =
            usize::try_from(fixture_u32(image, file_info_header + 16)).expect("ELF32 offset fits");
        image[file_info_offset + field_offset..file_info_offset + field_offset + 4]
            .copy_from_slice(&value.to_be_bytes());

        let file_info_size =
            usize::try_from(fixture_u32(image, file_info_header + 20)).expect("ELF32 size fits");
        let checksum = fixture_crc32(&image[file_info_offset..file_info_offset + file_info_size]);
        let crc_header = section_table + (section_count - 2) * section_size;
        let crc_offset =
            usize::try_from(fixture_u32(image, crc_header + 16)).expect("ELF32 offset fits");
        image[crc_offset + file_info_index * 4..crc_offset + file_info_index * 4 + 4]
            .copy_from_slice(&checksum.to_be_bytes());
    }

    fn link_plan() -> RpxRplLinkPlan {
        let main = rpl_link_fixture::main_rpx_link_fixture().expect("main fixture allocation");
        let provider =
            rpl_link_fixture::provider_rpl_link_fixture().expect("provider fixture allocation");
        plan_rpx_rpl_link(
            parse_rpx(&main).expect("main parses before provider"),
            RplModuleName::new("linkmod.rpl").expect("bounded module name"),
            parse_rpl(&provider).expect("provider parses after main"),
        )
        .expect("complete plan")
    }

    fn linked_proof() -> RpxRplLinkProof {
        commit_rpx_rpl_link(link_plan()).expect("transactional commit")
    }

    fn test_patch(phase: RpxRplLinkPhase, site: u32, before: u32, after: u32) -> PlannedPatch {
        PlannedPatch {
            phase,
            kind: CafeRelocationKind::Addr32,
            site,
            before,
            after,
            resolved_symbol: after,
            addend: 0,
            displacement: None,
        }
    }

    #[test]
    fn links_main_before_provider_at_exact_addresses() {
        let proof = linked_proof();
        assert_eq!(proof.main_entry(), 0x0200_0000);
        assert_eq!(proof.local_patch_site(), 0x1000_2008);
        assert_eq!(proof.local_patch_value(), 0x0200_2000);
        assert_eq!(proof.import_patch_site(), 0x1000_0000);
        assert_eq!(proof.import_patch_value(), 0x0200_2000);
        assert_eq!(proof.mapped_page_count(), 5);
        assert_eq!(proof.mapped_byte_count(), 5 * PAGE_SIZE);
    }

    #[test]
    fn repeated_links_have_identical_numeric_and_hash_proof() {
        assert_eq!(linked_proof(), linked_proof());
    }

    #[test]
    fn addr32_uses_cpp_wrapping_s_plus_a_semantics() {
        assert_eq!(addr32_value(u32::MAX, 1), 0);
        assert_eq!(addr32_value(0, -1), u32::MAX);
        assert_eq!(addr32_value(0x9000_0000, i32::MAX), 0x0fff_ffff);
        assert_eq!(addr32_value(0x1000_0000, i32::MIN), 0x9000_0000);
    }

    #[test]
    fn rel24_accepts_exact_positive_and_negative_range_edges() {
        let before = 0x4800_0001;
        assert_eq!(
            rel24_value(before, 0x11ff_fffc, 0, 0x1000_0000),
            Ok((0x49ff_fffd, 0x01ff_fffc))
        );
        assert_eq!(
            rel24_value(before, 0x0e00_0000, 0, 0x1000_0000),
            Ok((0x4a00_0001, -0x0200_0000))
        );
    }

    #[test]
    fn rel24_encodes_negative_displacement_and_preserves_opcode_and_lk() {
        assert_eq!(
            rel24_value(0x4800_0001, 0x0fff_fffc, 0, 0x1000_0000),
            Ok((0x4bff_fffd, -4))
        );
    }

    #[test]
    fn rel24_rejects_site_and_displacement_misalignment() {
        assert_eq!(
            rel24_value(0x4800_0000, 0x1000_0000, 0, 0x1000_0002),
            Err(RpxRplPlanError::Rel24SiteUnaligned {
                address: 0x1000_0002
            })
        );
        assert_eq!(
            rel24_value(0x4800_0000, 0x1000_0001, 0, 0x1000_0000),
            Err(RpxRplPlanError::Rel24DisplacementUnaligned {
                address: 0x1000_0000,
                displacement: 1,
            })
        );
    }

    #[test]
    fn rel24_rejects_non_branch_opcode_and_absolute_addressing() {
        assert!(matches!(
            rel24_value(0x6000_0000, 0, 0, 0),
            Err(RpxRplPlanError::Rel24InvalidInstruction { .. })
        ));
        assert!(matches!(
            rel24_value(0x4800_0002, 0, 0, 0),
            Err(RpxRplPlanError::Rel24InvalidInstruction { .. })
        ));
    }

    #[test]
    fn rel24_uses_modular_addends_and_rejects_far_branches() {
        assert_eq!(
            rel24_value(0x4800_0001, u32::MAX, 1, 0),
            Ok((0x4800_0001, 0))
        );
        assert_eq!(
            rel24_value(0x4800_0000, 0x8000_0000, i32::MIN, 0),
            Ok((0x4800_0000, 0))
        );
        assert_eq!(
            rel24_value(0x4800_0000, 0x1200_0000, 0, 0x1000_0000),
            Err(RpxRplPlanError::Rel24OutOfRange {
                address: 0x1000_0000,
                displacement: 0x0200_0000,
            })
        );
        assert_eq!(
            rel24_value(0x4800_0000, 0x0dff_fffc, 0, 0x1000_0000),
            Err(RpxRplPlanError::Rel24OutOfRange {
                address: 0x1000_0000,
                displacement: -0x0200_0004,
            })
        );
    }

    #[test]
    fn commit_rejects_a_patch_preimage_mismatch_before_writing() {
        let mut memory = GuestMemory::new();
        memory
            .map(
                GuestAddress::new(0x1000),
                PAGE_SIZE,
                Permissions::READ | Permissions::WRITE,
            )
            .expect("test page maps");
        memory
            .write_u32(GuestAddress::new(0x1000), 0x6000_0000)
            .expect("test preimage writes");
        let patch = test_patch(RpxRplLinkPhase::Import, 0x1000, 0x4800_0000, 0x4800_0001);
        assert_eq!(
            apply_and_verify(&mut memory, patch),
            Err(RpxRplCommitError::PreimageMismatch {
                phase: RpxRplLinkPhase::Import,
                address: 0x1000,
            })
        );
        assert_eq!(memory.read_u32(GuestAddress::new(0x1000)), Ok(0x6000_0000));
    }

    #[test]
    fn addr32_proof_remains_compatible_and_exposes_bounded_records() {
        let proof = linked_proof();
        let relocations = proof.relocations();
        assert_eq!(relocations.len(), 2);
        assert_eq!(relocations[0].phase(), RpxRplLinkPhase::Local);
        assert_eq!(relocations[0].kind(), CafeRelocationKind::Addr32);
        assert_eq!(relocations[0].before(), 0);
        assert_eq!(relocations[0].after(), proof.local_patch_value());
        assert_eq!(relocations[0].site(), proof.local_patch_site());
        assert_eq!(relocations[0].resolved_symbol(), 0x0200_2000);
        assert_eq!(relocations[0].addend(), 0);
        assert_eq!(relocations[0].displacement(), None);
        assert_eq!(relocations[1].phase(), RpxRplLinkPhase::Import);
        assert_eq!(relocations[1].kind(), CafeRelocationKind::Addr32);
        assert_eq!(relocations[1].before(), 0);
        assert_eq!(relocations[1].after(), proof.import_patch_value());
        assert_eq!(relocations[1].site(), proof.import_patch_site());
        assert_eq!(relocations[1].resolved_symbol(), 0x0200_2000);
        assert_eq!(relocations[1].addend(), 0);
        assert_eq!(relocations[1].displacement(), None);
    }

    #[test]
    fn relocation_hash_discriminators_are_stable() {
        assert_eq!(relocation_kind_byte(CafeRelocationKind::Addr32), 1);
        assert_eq!(relocation_kind_byte(CafeRelocationKind::Rel24), 10);
    }

    #[test]
    fn rejects_a_prepopulated_provider_export_before_planning_a_patch() {
        assert_eq!(validate_unrelocated_export(2, 0), Ok(()));
        assert_eq!(
            validate_unrelocated_export(2, 0x0200_0000),
            Err(RpxRplPlanError::PreRelocatedExportUnsupported { section_index: 2 })
        );
    }

    #[test]
    fn final_permissions_and_page_ranges_cover_the_complete_plan() {
        assert_eq!(
            final_permissions(RpxRplLinkRegion::Text),
            Permissions::READ | Permissions::EXECUTE
        );
        assert_eq!(
            final_permissions(RpxRplLinkRegion::Data),
            Permissions::READ | Permissions::WRITE
        );
        assert_eq!(
            final_permissions(RpxRplLinkRegion::Loader),
            Permissions::READ
        );

        let plan = link_plan();
        let expected = [
            (0x0200_0000, RpxRplLinkRegion::Text),
            (0x1000_0000, RpxRplLinkRegion::Data),
            (0x1000_1000, RpxRplLinkRegion::Loader),
            (0x0200_2000, RpxRplLinkRegion::Text),
            (0x1000_2000, RpxRplLinkRegion::Loader),
        ];
        assert_eq!(plan.ranges.len(), expected.len());
        for (range, (start, region)) in plan.ranges.iter().zip(expected) {
            assert_eq!(
                (range.start, range.len, range.region),
                (start, PAGE_SIZE, region)
            );
            assert_eq!(u64::from(range.start) % PAGE_SIZE, 0);
        }
        assert_eq!(plan.mapped_page_count, 5);
        assert_eq!(plan.mapped_byte_count, 5 * PAGE_SIZE);
    }

    #[test]
    fn module_name_and_errors_never_format_raw_input() {
        let name = RplModuleName::new("private-provider.rpl").expect("valid name");
        assert!(!format!("{name:?} {name}").contains("private-provider"));
        assert_eq!(
            RplModuleName::new("../provider.rpl"),
            Err(RplModuleNameError::InvalidCharacter)
        );
    }

    #[test]
    fn rejects_ambiguous_or_noncanonical_module_name_aliases() {
        for alias in [
            ".",
            "..",
            ".linkmod.rpl",
            "linkmod.",
            "link.mod.rpl",
            "linkmod.rpx",
            "path/linkmod.rpl",
            "path\\linkmod.rpl",
            "link\nmod.rpl",
            "línkmod.rpl",
            "LINKMOD.rpl",
            "linkmod.RPL",
        ] {
            assert_eq!(
                RplModuleName::new(alias),
                Err(RplModuleNameError::InvalidCharacter)
            );
        }
        assert!(RplModuleName::new("linkmod.rpl").is_ok());
    }

    #[test]
    fn rejects_each_nonzero_adjustment_for_each_module_without_exposing_value() {
        for (module, adjustment) in [
            (RpxRplLinkModule::Main, FileInfoAdjustment::Trampoline),
            (RpxRplLinkModule::Main, FileInfoAdjustment::Loader),
            (RpxRplLinkModule::Provider, FileInfoAdjustment::Trampoline),
            (RpxRplLinkModule::Provider, FileInfoAdjustment::Loader),
        ] {
            let error = validate_adjustment(module, adjustment, u32::MAX)
                .expect_err("nonzero adjustment must fail closed");
            let expected = match adjustment {
                FileInfoAdjustment::Trampoline => {
                    RpxRplPlanError::UnsupportedTrampolineAdjustment { module }
                }
                FileInfoAdjustment::Loader => {
                    RpxRplPlanError::UnsupportedLoaderAdjustment { module }
                }
            };
            assert_eq!(error, expected);
            assert!(!format!("{error:?} {error}").contains("4294967295"));
        }
    }

    #[test]
    fn planner_rejects_each_adjustment_field_in_main_and_provider() {
        for (module, adjustment, field_offset) in [
            (RpxRplLinkModule::Main, FileInfoAdjustment::Trampoline, 32),
            (RpxRplLinkModule::Main, FileInfoAdjustment::Loader, 76),
            (
                RpxRplLinkModule::Provider,
                FileInfoAdjustment::Trampoline,
                32,
            ),
            (RpxRplLinkModule::Provider, FileInfoAdjustment::Loader, 76),
        ] {
            let mut main =
                rpl_link_fixture::main_rpx_link_fixture().expect("main fixture allocation");
            let mut provider =
                rpl_link_fixture::provider_rpl_link_fixture().expect("provider fixture allocation");
            match module {
                RpxRplLinkModule::Main => set_fixture_adjustment(&mut main, field_offset, 1),
                RpxRplLinkModule::Provider => {
                    set_fixture_adjustment(&mut provider, field_offset, 1);
                }
            }
            let error = plan_rpx_rpl_link(
                parse_rpx(&main).expect("adjusted main remains structurally valid"),
                RplModuleName::new("linkmod.rpl").expect("canonical provider name"),
                parse_rpl(&provider).expect("adjusted provider remains structurally valid"),
            )
            .expect_err("nonzero adjustment must be rejected during shape validation");
            let expected = match adjustment {
                FileInfoAdjustment::Trampoline => {
                    RpxRplPlanError::UnsupportedTrampolineAdjustment { module }
                }
                FileInfoAdjustment::Loader => {
                    RpxRplPlanError::UnsupportedLoaderAdjustment { module }
                }
            };
            assert_eq!(error, expected);
        }
    }

    #[test]
    fn commit_patch_order_is_local_then_import() {
        let plan = link_plan();
        let ordered = patches_in_commit_order(plan.local_patch, plan.import_patch);
        assert_eq!(
            ordered.map(|patch| patch.phase),
            [RpxRplLinkPhase::Local, RpxRplLinkPhase::Import]
        );
        assert_eq!(
            ordered.map(|patch| patch.site),
            [plan.local_patch.site, plan.import_patch.site]
        );
    }

    #[test]
    fn wrong_provider_name_fails_before_commit_and_returns_no_proof() {
        let main = rpl_link_fixture::main_rpx_link_fixture().expect("main fixture allocation");
        let provider =
            rpl_link_fixture::provider_rpl_link_fixture().expect("provider fixture allocation");
        let result = plan_rpx_rpl_link(
            parse_rpx(&main).expect("main parse"),
            RplModuleName::new("wrong.rpl").expect("bounded module name"),
            parse_rpl(&provider).expect("provider parse"),
        );
        assert!(matches!(
            &result,
            Err(RpxRplPlanError::UnresolvedImport { .. })
        ));
        assert!(
            result
                .ok()
                .and_then(|plan| commit_rpx_rpl_link(plan).ok())
                .is_none()
        );
    }

    #[test]
    fn helper_guards_conflicts_and_pool_overflow() {
        let patch = test_patch(RpxRplLinkPhase::Local, 0x1000, 0, 0);
        let overlap = test_patch(RpxRplLinkPhase::Import, 0x1002, 0, 0);
        assert!(matches!(
            validate_patch_conflict(patch, overlap),
            Err(RpxRplPlanError::PatchConflict { .. })
        ));

        assert!(matches!(
            exact_count(RpxRplLinkModule::Provider, RpxRplLinkRecord::Export, 2, 1),
            Err(RpxRplPlanError::UnsupportedRecordCount {
                actual: 2,
                expected: 1,
                ..
            })
        ));

        let conflicting_ranges = [
            PlannedRange {
                start: 0x0200_0000,
                len: PAGE_SIZE,
                region: RpxRplLinkRegion::Text,
            },
            PlannedRange {
                start: 0x0200_0000,
                len: PAGE_SIZE,
                region: RpxRplLinkRegion::Data,
            },
        ];
        assert!(matches!(
            validate_ranges(&conflicting_ranges),
            Err(RpxRplPlanError::PageWriteExecute {
                address: 0x0200_0000
            })
        ));

        let mut cursor = CODE_POOL_END - PAGE_SIZE;
        assert!(matches!(
            allocate_region(
                &mut cursor,
                CODE_POOL_END,
                u32::try_from(PAGE_SIZE).expect("page size fits a guest address"),
                0,
                0,
                PAGE_SIZE,
                RpxRplLinkRegion::Text
            ),
            Err(RpxRplPlanError::AddressPoolExhausted { .. })
        ));
    }
}
