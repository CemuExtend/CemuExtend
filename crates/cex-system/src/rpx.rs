//! Strict, resource-bounded parsing for the first supported Cafe RPX slice.
//!
//! The parser deliberately accepts only uncompressed ELF32 big-endian images
//! whose section kinds are needed by the initial Rust loader. Unsupported RPX
//! features fail closed instead of being partially interpreted.

use std::{
    collections::{BTreeMap, BTreeSet},
    fmt,
    mem::size_of,
};

use thiserror::Error;

const ELF_HEADER_SIZE: u64 = 52;
const SECTION_HEADER_SIZE: u64 = 40;
const MAX_SECTIONS: u16 = 512;
const MAX_ALIGNMENT: u32 = 64 * 1024;
// Guest memory models all 4 GiB as a half-open range, so an exact exclusive
// end of 2^32 is intentionally valid. This does not preserve the legacy
// external validator's stricter UINT32_MAX exclusive-end restriction.
const GUEST_ADDRESS_SPACE_SIZE: u64 = 1_u64 << 32;

const SHT_NULL: u32 = 0;
const SHT_PROGBITS: u32 = 1;
const SHT_SYMTAB: u32 = 2;
const SHT_STRTAB: u32 = 3;
const SHT_RELA: u32 = 4;
const SHT_NOBITS: u32 = 8;
const SHT_DYNSYM: u32 = 11;
const SHT_RPL_EXPORTS: u32 = 0x8000_0001;
const SHT_RPL_IMPORTS: u32 = 0x8000_0002;
const SHT_RPL_CRCS: u32 = 0x8000_0003;
const SHT_RPL_FILEINFO: u32 = 0x8000_0004;

const SHF_WRITE: u32 = 1;
const SHF_ALLOC: u32 = 2;
const SHF_EXECINSTR: u32 = 4;
const SHF_SUPPORTED: u32 = SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR;
const SHF_RPL_COMPRESSED: u32 = 0x0800_0000;

const FILEINFO_SIZE: u32 = 0x60;
const FILEINFO_MAGIC: u32 = 0xcafe_0402;
const FILEINFO_RPX_FLAG: u32 = 1 << 1;
const MAX_RPL_RECORDS: usize = 4096;
const MAX_RPL_NAME_SIZE: usize = 1024;

/// Maximum complete RPX image accepted by [`parse_rpx`].
pub const MAX_RPX_IMAGE_SIZE: usize = 64 * 1024 * 1024;

/// The module kind asserted by the Cafe FILEINFO record.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CafeModuleKind {
    /// A main executable module.
    Rpx,
    /// A relocatable library module.
    Rpl,
}

/// The only symbol classes exposed by the minimal RPL linker slice.
#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum CafeSymbolKind {
    /// An executable function symbol.
    Function,
    /// A data object symbol.
    Data,
}

/// Relocation operations accepted by the minimal RPL linker slice.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CafeRelocationKind {
    /// Write the complete 32-bit symbol value plus addend.
    Addr32,
    /// Apply a 24-bit relative branch relocation to one aligned instruction.
    Rel24,
}

/// A validated RPL symbol-table entry.
#[derive(Clone, Eq, PartialEq)]
pub struct CafeSymbol {
    table_section_index: usize,
    symbol_index: usize,
    name: String,
    value: u32,
    size: u32,
    section_index: usize,
    kind: CafeSymbolKind,
}

impl fmt::Debug for CafeSymbol {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("CafeSymbol")
            .field("table_section_index", &self.table_section_index)
            .field("symbol_index", &self.symbol_index)
            .field("value", &self.value)
            .field("size", &self.size)
            .field("section_index", &self.section_index)
            .field("kind", &self.kind)
            .finish_non_exhaustive()
    }
}

impl CafeSymbol {
    /// Return the ELF section-table index of the containing symbol table.
    pub const fn table_section_index(&self) -> usize {
        self.table_section_index
    }
    /// Return the zero-based index within the containing symbol table.
    pub const fn symbol_index(&self) -> usize {
        self.symbol_index
    }
    /// Return the validated UTF-8 symbol name.
    pub fn name(&self) -> &str {
        &self.name
    }
    /// Return the symbol value interpreted as a guest virtual address.
    pub const fn value(&self) -> u32 {
        self.value
    }
    /// Return the declared symbol size in bytes.
    pub const fn size(&self) -> u32 {
        self.size
    }
    /// Return the ELF section-table index referenced by the symbol.
    pub const fn section_index(&self) -> usize {
        self.section_index
    }
    /// Return the validated symbol class.
    pub const fn kind(&self) -> CafeSymbolKind {
        self.kind
    }
}

/// One validated imported module record.
#[derive(Clone, Eq, PartialEq)]
pub struct CafeImport {
    section_index: usize,
    module_name: String,
    kind: CafeSymbolKind,
}

impl fmt::Debug for CafeImport {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("CafeImport")
            .field("section_index", &self.section_index)
            .field("kind", &self.kind)
            .finish_non_exhaustive()
    }
}
impl CafeImport {
    /// Return the ELF section-table index containing the import record.
    pub const fn section_index(&self) -> usize {
        self.section_index
    }
    /// Return the validated imported module name.
    pub fn module_name(&self) -> &str {
        &self.module_name
    }
    /// Return the symbol class imported from the module.
    pub const fn kind(&self) -> CafeSymbolKind {
        self.kind
    }
}

/// One validated exported address/name descriptor.
#[derive(Clone, Eq, PartialEq)]
pub struct CafeExport {
    section_index: usize,
    descriptor_index: usize,
    name: String,
    address: u32,
    kind: CafeSymbolKind,
}
impl fmt::Debug for CafeExport {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("CafeExport")
            .field("section_index", &self.section_index)
            .field("address", &self.address)
            .field("kind", &self.kind)
            .finish_non_exhaustive()
    }
}
impl CafeExport {
    /// Return the ELF section-table index containing the export descriptor.
    pub const fn section_index(&self) -> usize {
        self.section_index
    }
    /// Return the validated exported symbol name.
    pub fn name(&self) -> &str {
        &self.name
    }
    /// Return the exported guest virtual address.
    pub const fn address(&self) -> u32 {
        self.address
    }
    /// Return the exported symbol class.
    pub const fn kind(&self) -> CafeSymbolKind {
        self.kind
    }
}

/// One validated `SHT_RELA` record.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CafeRelocation {
    section_index: usize,
    target_section_index: usize,
    symbol_table_section_index: usize,
    offset: u32,
    symbol_index: usize,
    addend: i32,
    kind: CafeRelocationKind,
}
impl CafeRelocation {
    /// Return the ELF section-table index containing the relocation.
    pub const fn section_index(&self) -> usize {
        self.section_index
    }
    /// Return the ELF section-table index modified by the relocation.
    pub const fn target_section_index(&self) -> usize {
        self.target_section_index
    }
    /// Return the ELF section-table index of the referenced symbol table.
    pub const fn symbol_table_section_index(&self) -> usize {
        self.symbol_table_section_index
    }
    /// Return the guest virtual address modified by the relocation.
    pub const fn offset(&self) -> u32 {
        self.offset
    }
    /// Return the referenced symbol-table index.
    pub const fn symbol_index(&self) -> usize {
        self.symbol_index
    }
    /// Return the signed relocation addend.
    pub const fn addend(&self) -> i32 {
        self.addend
    }
    /// Return the validated relocation operation.
    pub const fn kind(&self) -> CafeRelocationKind {
        self.kind
    }
}

/// A validated, owned Cafe RPL image description.
#[derive(Clone, Eq, PartialEq)]
pub struct ParsedRpl {
    entry_point: u32,
    sections: Vec<RpxSection>,
    file_info: RpxFileInfo,
    symbols: Vec<CafeSymbol>,
    imports: Vec<CafeImport>,
    exports: Vec<CafeExport>,
    relocations: Vec<CafeRelocation>,
}
impl fmt::Debug for ParsedRpl {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("ParsedRpl")
            .field("entry_point", &self.entry_point)
            .field("section_count", &self.sections.len())
            .field("symbol_count", &self.symbols.len())
            .field("import_count", &self.imports.len())
            .field("export_count", &self.exports.len())
            .field("relocation_count", &self.relocations.len())
            .field("file_info", &self.file_info)
            .finish()
    }
}
impl ParsedRpl {
    /// Return the validated entry point, which is always zero for this RPL slice.
    pub const fn entry_point(&self) -> u32 {
        self.entry_point
    }
    /// Return all sections in ELF section-table order.
    pub fn sections(&self) -> &[RpxSection] {
        &self.sections
    }
    /// Return the validated terminal FILEINFO record.
    pub const fn file_info(&self) -> &RpxFileInfo {
        &self.file_info
    }
    /// Return the FILEINFO module kind.
    pub const fn module_kind(&self) -> CafeModuleKind {
        CafeModuleKind::Rpl
    }
    /// Return all validated symbol-table entries except the null symbol.
    pub fn symbols(&self) -> &[CafeSymbol] {
        &self.symbols
    }
    /// Return all validated imported-module records.
    pub fn imports(&self) -> &[CafeImport] {
        &self.imports
    }
    /// Return all validated export descriptors.
    pub fn exports(&self) -> &[CafeExport] {
        &self.exports
    }
    /// Return all validated ADDR32 and REL24 relocations.
    pub fn relocations(&self) -> &[CafeRelocation] {
        &self.relocations
    }
}

/// A validated, owned Cafe RPX image description.
///
/// Construction is restricted to [`parse_rpx`], so callers can rely on the
/// invariants documented by the getters.
#[derive(Clone, Eq, PartialEq)]
pub struct ParsedRpx {
    entry_point: u32,
    sections: Vec<RpxSection>,
    file_info: RpxFileInfo,
    symbols: Vec<CafeSymbol>,
    imports: Vec<CafeImport>,
    exports: Vec<CafeExport>,
    relocations: Vec<CafeRelocation>,
}

impl fmt::Debug for ParsedRpx {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("ParsedRpx")
            .field("entry_point", &self.entry_point)
            .field("section_count", &self.sections.len())
            .field("symbol_count", &self.symbols.len())
            .field("import_count", &self.imports.len())
            .field("export_count", &self.exports.len())
            .field("relocation_count", &self.relocations.len())
            .field("file_info", &self.file_info)
            .finish()
    }
}

impl ParsedRpx {
    /// Return the aligned entry point, which is backed by a complete instruction
    /// in an allocated executable section.
    pub const fn entry_point(&self) -> u32 {
        self.entry_point
    }

    /// Return all sections in ELF section-table order.
    pub fn sections(&self) -> &[RpxSection] {
        &self.sections
    }

    /// Return the validated terminal FILEINFO record.
    pub const fn file_info(&self) -> &RpxFileInfo {
        &self.file_info
    }

    /// Return whether FILEINFO marks this module as the main RPX rather than an RPL.
    pub const fn is_rpx(&self) -> bool {
        self.file_info.is_rpx()
    }

    /// Return the FILEINFO module kind.
    pub const fn module_kind(&self) -> CafeModuleKind {
        CafeModuleKind::Rpx
    }

    /// Return all validated symbol-table entries except the mandatory null symbol.
    pub fn symbols(&self) -> &[CafeSymbol] {
        &self.symbols
    }

    /// Return all validated imported-module records.
    pub fn imports(&self) -> &[CafeImport] {
        &self.imports
    }

    /// Return all validated export descriptors.
    pub fn exports(&self) -> &[CafeExport] {
        &self.exports
    }

    /// Return all validated ADDR32 and REL24 relocations.
    pub fn relocations(&self) -> &[CafeRelocation] {
        &self.relocations
    }
}

/// One validated RPX section with owned, uncompressed stored bytes.
#[derive(Clone, Eq, PartialEq)]
pub struct RpxSection {
    index: usize,
    name: String,
    section_type: u32,
    flags: u32,
    virtual_address: u32,
    file_offset: u32,
    alignment: u32,
    mapping_region: Option<RpxMappingRegion>,
    data: Vec<u8>,
}

impl fmt::Debug for RpxSection {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpxSection")
            .field("index", &self.index)
            .field("section_type", &self.section_type)
            .field("flags", &self.flags)
            .field("virtual_address", &self.virtual_address)
            .field("file_offset", &self.file_offset)
            .field("alignment", &self.alignment)
            .field("mapping_region", &self.mapping_region)
            .field("data_len", &self.data.len())
            .finish_non_exhaustive()
    }
}

impl RpxSection {
    /// Return the section-table index.
    pub const fn index(&self) -> usize {
        self.index
    }

    /// Return the validated UTF-8 section name.
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Return the numeric ELF/Cafe section type.
    pub const fn section_type(&self) -> u32 {
        self.section_type
    }

    /// Return the numeric ELF section flags.
    pub const fn flags(&self) -> u32 {
        self.flags
    }

    /// Return the section's guest virtual address.
    ///
    /// Non-allocated sections always return zero.
    pub const fn virtual_address(&self) -> u32 {
        self.virtual_address
    }

    /// Return the section payload's offset in the original file.
    pub const fn file_offset(&self) -> u32 {
        self.file_offset
    }

    /// Return the validated section alignment, where zero means unspecified.
    pub const fn alignment(&self) -> u32 {
        self.alignment
    }

    /// Return the FILEINFO mapping region selected during validation.
    ///
    /// Empty and non-allocated sections do not participate in the guest map
    /// and therefore return [`None`].
    pub const fn mapping_region(&self) -> Option<RpxMappingRegion> {
        self.mapping_region
    }

    /// Return owned raw section bytes through an immutable view.
    pub fn data(&self) -> &[u8] {
        &self.data
    }

    /// Return whether the section participates in the guest virtual-address map.
    pub const fn is_allocated(&self) -> bool {
        self.flags & SHF_ALLOC != 0
    }

    /// Return whether the section requests guest write permission.
    pub const fn is_writable(&self) -> bool {
        self.flags & SHF_WRITE != 0
    }

    /// Return whether the section requests guest execute permission.
    pub const fn is_executable(&self) -> bool {
        self.flags & SHF_EXECINSTR != 0
    }
}

/// Validated mapping metadata from the terminal Cafe FILEINFO section.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RpxFileInfo {
    magic: u32,
    text_region_size: u32,
    text_alignment: u32,
    data_region_size: u32,
    data_alignment: u32,
    loader_region_size: u32,
    trampoline_adjustment: u32,
    loader_adjustment: u32,
    flags: u32,
}

impl RpxFileInfo {
    /// Return the validated Cafe FILEINFO magic.
    pub const fn magic(&self) -> u32 {
        self.magic
    }

    /// Return the declared text-region size.
    pub const fn text_region_size(&self) -> u32 {
        self.text_region_size
    }

    /// Return the text-region alignment, where zero means unspecified.
    pub const fn text_alignment(&self) -> u32 {
        self.text_alignment
    }

    /// Return the declared data-region size.
    pub const fn data_region_size(&self) -> u32 {
        self.data_region_size
    }

    /// Return the data-region alignment, where zero means unspecified.
    pub const fn data_alignment(&self) -> u32 {
        self.data_alignment
    }

    /// Return the declared loader-region size.
    pub const fn loader_region_size(&self) -> u32 {
        self.loader_region_size
    }

    /// Return the reserved prefix within the text region.
    pub const fn trampoline_adjustment(&self) -> u32 {
        self.trampoline_adjustment
    }

    /// Return the reserved suffix within the loader region.
    pub const fn loader_adjustment(&self) -> u32 {
        self.loader_adjustment
    }

    /// Return the numeric Cafe FILEINFO flags.
    pub const fn flags(&self) -> u32 {
        self.flags
    }

    /// Return whether the FILEINFO flags identify a main RPX image.
    pub const fn is_rpx(&self) -> bool {
        self.flags & FILEINFO_RPX_FLAG != 0
    }

    /// Return the module kind asserted by the FILEINFO flags.
    pub const fn module_kind(&self) -> CafeModuleKind {
        if self.is_rpx() {
            CafeModuleKind::Rpx
        } else {
            CafeModuleKind::Rpl
        }
    }
}

/// Header fields whose exact values define the supported Cafe ELF32 slice.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpxHeaderField {
    /// ELF object class (`ELFCLASS32`).
    Class,
    /// ELF byte order (`ELFDATA2MSB`).
    DataEncoding,
    /// ELF identification version.
    IdentificationVersion,
    /// Cafe identification bytes.
    CafeIdentification,
    /// Cafe executable object type.
    ObjectType,
    /// PowerPC machine identifier.
    Machine,
    /// ELF object version.
    ObjectVersion,
    /// ELF header size.
    HeaderSize,
    /// Program-header table offset.
    ProgramHeaderOffset,
    /// Program-header entry count.
    ProgramHeaderCount,
    /// Section-header entry size.
    SectionHeaderSize,
    /// Section-header entry count.
    SectionCount,
    /// Section-name string-table index.
    SectionNameIndex,
}

impl fmt::Display for RpxHeaderField {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Class => "class",
            Self::DataEncoding => "data encoding",
            Self::IdentificationVersion => "identification version",
            Self::CafeIdentification => "Cafe identification",
            Self::ObjectType => "object type",
            Self::Machine => "machine",
            Self::ObjectVersion => "object version",
            Self::HeaderSize => "header size",
            Self::ProgramHeaderOffset => "program-header offset",
            Self::ProgramHeaderCount => "program-header count",
            Self::SectionHeaderSize => "section-header size",
            Self::SectionCount => "section count",
            Self::SectionNameIndex => "section-name index",
        })
    }
}

/// RPX features intentionally outside the initial parser slice.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpxUnsupportedFeature {
    /// A symbol table section.
    SymbolTable,
    /// A relocation section.
    Relocation,
    /// An ELF NOBITS section.
    NoBits,
    /// A Cafe exports section.
    Exports,
    /// A Cafe imports section.
    Imports,
    /// Cafe section compression.
    Compression,
    /// Another unsupported section type.
    SectionType(u32),
    /// Unsupported section-flag bits.
    SectionFlags(u32),
}

impl fmt::Display for RpxUnsupportedFeature {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::SymbolTable => formatter.write_str("symbol table"),
            Self::Relocation => formatter.write_str("relocation"),
            Self::NoBits => formatter.write_str("NOBITS"),
            Self::Exports => formatter.write_str("Cafe exports"),
            Self::Imports => formatter.write_str("Cafe imports"),
            Self::Compression => formatter.write_str("Cafe compression"),
            Self::SectionType(value) => write!(formatter, "section type 0x{value:08x}"),
            Self::SectionFlags(value) => write!(formatter, "section flags 0x{value:08x}"),
        }
    }
}

/// FILEINFO fields checked by the bounded mapping-metadata validator.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpxFileInfoField {
    /// FILEINFO magic.
    Magic,
    /// Text-region size.
    TextRegionSize,
    /// Text-region alignment.
    TextAlignment,
    /// Data-region size.
    DataRegionSize,
    /// Data-region alignment.
    DataAlignment,
    /// Loader-region size.
    LoaderRegionSize,
    /// Text trampoline adjustment.
    TrampolineAdjustment,
    /// Loader-region adjustment.
    LoaderAdjustment,
    /// Module-kind flags.
    Flags,
}

impl fmt::Display for RpxFileInfoField {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Magic => "magic",
            Self::TextRegionSize => "text-region size",
            Self::TextAlignment => "text alignment",
            Self::DataRegionSize => "data-region size",
            Self::DataAlignment => "data alignment",
            Self::LoaderRegionSize => "loader-region size",
            Self::TrampolineAdjustment => "trampoline adjustment",
            Self::LoaderAdjustment => "loader adjustment",
            Self::Flags => "module flags",
        })
    }
}

/// FILEINFO mapping region selected for one allocated, non-empty section.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpxMappingRegion {
    /// Executable program text.
    Text,
    /// Writable program data.
    Data,
    /// Read-only loader metadata.
    Loader,
}

impl fmt::Display for RpxMappingRegion {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Text => "text",
            Self::Data => "data",
            Self::Loader => "loader",
        })
    }
}

/// A deterministic RPX validation failure.
///
/// Variants contain only bounded numeric metadata. Input bytes, file paths, and
/// section names are never retained or rendered by this type.
#[derive(Clone, Debug, Error, Eq, PartialEq)]
pub enum RpxError {
    /// The fixed ELF header is incomplete.
    #[error("RPX header is truncated: image has {actual} bytes")]
    TruncatedHeader {
        /// Complete input length.
        actual: usize,
    },
    /// The input exceeds the hard complete-image limit.
    #[error("RPX image size {actual} exceeds the {maximum}-byte limit")]
    ImageTooLarge {
        /// Complete input length.
        actual: usize,
        /// Configured maximum length.
        maximum: usize,
    },
    /// The ELF magic is absent.
    #[error("RPX ELF magic is invalid")]
    InvalidMagic,
    /// A fixed Cafe ELF header field is outside the supported slice.
    #[error("RPX {field} is invalid")]
    InvalidHeader {
        /// Invalid fixed field.
        field: RpxHeaderField,
    },
    /// The section-header table is outside the image or overlaps the ELF header.
    #[error("RPX section table is outside the image or overlaps the ELF header")]
    InvalidSectionTable,
    /// A section payload is outside the complete image.
    #[error("RPX section {section_index} payload range is outside the image")]
    InvalidSectionRange {
        /// Section-table index.
        section_index: usize,
    },
    /// A section payload overlaps the fixed ELF header.
    #[error("RPX section {section_index} payload overlaps the ELF header")]
    SectionOverlapsHeader {
        /// Section-table index.
        section_index: usize,
    },
    /// A section payload overlaps the section-header table.
    #[error("RPX section {section_index} payload overlaps the section table")]
    SectionOverlapsTable {
        /// Section-table index.
        section_index: usize,
    },
    /// Two non-empty stored section payloads overlap.
    #[error("RPX section payloads {first_index} and {second_index} overlap")]
    SectionPayloadOverlap {
        /// Earlier section-table index.
        first_index: usize,
        /// Later section-table index.
        second_index: usize,
    },
    /// Section zero is not the required all-zero ELF NULL record.
    #[error("RPX NULL section {section_index} is invalid")]
    InvalidNullSection {
        /// Section-table index.
        section_index: usize,
    },
    /// A metadata section requests guest memory permissions.
    #[error(
        "RPX metadata section {section_index} of type 0x{section_type:08x} has invalid flags 0x{flags:08x}"
    )]
    InvalidMetadataFlags {
        /// Section-table index.
        section_index: usize,
        /// Numeric metadata section type.
        section_type: u32,
        /// Rejected numeric permission flags.
        flags: u32,
    },
    /// The image requests an intentionally unsupported RPX feature.
    #[error("RPX section {section_index} uses unsupported {feature}")]
    Unsupported {
        /// Section-table index.
        section_index: usize,
        /// Unsupported semantic feature.
        feature: RpxUnsupportedFeature,
    },
    /// A section alignment is neither zero nor a bounded power of two.
    #[error("RPX section {section_index} has invalid alignment {alignment}")]
    InvalidAlignment {
        /// Section-table index.
        section_index: usize,
        /// Declared alignment.
        alignment: u32,
    },
    /// A section virtual address does not satisfy its declared alignment.
    #[error("RPX section {section_index} virtual address is misaligned")]
    MisalignedVirtualAddress {
        /// Section-table index.
        section_index: usize,
    },
    /// A section requests writable and executable permissions together.
    #[error("RPX section {section_index} violates write-xor-execute")]
    WriteExecute {
        /// Section-table index.
        section_index: usize,
    },
    /// A non-allocated section has a non-zero guest virtual address.
    #[error("RPX non-allocated section {section_index} has a virtual address")]
    NonAllocatedAddress {
        /// Section-table index.
        section_index: usize,
    },
    /// An allocated section extends beyond the 32-bit guest address space.
    #[error("RPX allocated section {section_index} virtual range overflows")]
    VirtualAddressOverflow {
        /// Section-table index.
        section_index: usize,
    },
    /// Two non-empty allocated virtual ranges overlap.
    #[error("RPX allocated section ranges {first_index} and {second_index} overlap")]
    VirtualAddressOverlap {
        /// Earlier section-table index.
        first_index: usize,
        /// Later section-table index.
        second_index: usize,
    },
    /// The selected section-name string table is malformed.
    #[error("RPX section-name string table is invalid")]
    InvalidStringTable,
    /// A section name is out of bounds, unterminated, invalid UTF-8, or exceeds
    /// the aggregate name-allocation bound.
    #[error("RPX section {section_index} has an invalid section-name reference")]
    InvalidSectionName {
        /// Section-table index.
        section_index: usize,
    },
    /// CRC and FILEINFO sections are missing, duplicated, or not terminal.
    #[error("RPX must contain exactly one terminal CRC section followed by FILEINFO")]
    InvalidSectionOrder,
    /// The CRC table is not exactly one big-endian word per section.
    #[error("RPX CRC table size {actual} does not match expected size {expected}")]
    InvalidCrcTableSize {
        /// Stored CRC table size.
        actual: u32,
        /// Required CRC table size.
        expected: u32,
    },
    /// A CRC table word does not match the corresponding raw section bytes.
    #[error("RPX section {section_index} CRC does not match")]
    CrcMismatch {
        /// Section-table index.
        section_index: usize,
        /// CRC table value.
        expected: u32,
        /// Computed IEEE CRC-32 value.
        actual: u32,
    },
    /// The terminal FILEINFO payload is shorter than the fixed initial record.
    #[error("RPX FILEINFO size {actual} is smaller than {minimum}")]
    FileInfoTooSmall {
        /// Stored FILEINFO size.
        actual: u32,
        /// Required initial record size.
        minimum: u32,
    },
    /// A FILEINFO field is outside the supported mapping bounds.
    #[error("RPX FILEINFO {field} has invalid value {value}")]
    InvalidFileInfo {
        /// Invalid FILEINFO field.
        field: RpxFileInfoField,
        /// Rejected bounded numeric value.
        value: u32,
    },
    /// A declared mapping region extends beyond the 32-bit guest address space.
    #[error("RPX {region} region for section {section_index} overflows guest address space")]
    RegionAddressOverflow {
        /// First affected section-table index.
        section_index: usize,
        /// FILEINFO mapping region selected for the section.
        region: RpxMappingRegion,
    },
    /// An allocated section is not contained by its usable FILEINFO region.
    #[error("RPX section {section_index} is outside its declared {region} region")]
    SectionOutsideRegion {
        /// Section-table index.
        section_index: usize,
        /// FILEINFO mapping region selected for the section.
        region: RpxMappingRegion,
    },
    /// The entry point is unaligned or lacks a complete allocated executable word.
    #[error("RPX entry point is not a complete aligned executable instruction")]
    InvalidEntryPoint,
    /// The host allocator rejected a bounded output allocation.
    #[error("host allocation failed while owning {requested} validated RPX bytes")]
    AllocationFailed {
        /// Bounded requested allocation length.
        requested: usize,
    },
    /// An RPL structural record has an invalid bounded field or layout.
    #[error("RPL section {section_index} has invalid {reason}")]
    InvalidRplRecord {
        /// Section-table index containing the record.
        section_index: usize,
        /// Bounded reason for the rejection.
        reason: RplRecordError,
    },
}

/// Bounded reasons for rejecting an RPL structural record.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RplRecordError {
    /// A record uses an unsupported fixed entry size.
    EntrySize,
    /// A record count is outside the parser's resource bounds.
    RecordCount,
    /// A linked section is missing or has the wrong type.
    SectionLink,
    /// A section information field is outside its valid range.
    SectionInfo,
    /// A referenced symbol-table index is invalid.
    SymbolIndex,
    /// A symbol has an unsupported type or mismatches its section.
    SymbolKind,
    /// A symbol value and size extend outside their target section.
    SymbolRange,
    /// A relocation uses an unsupported operation.
    RelocationType,
    /// A relocation target extends outside its target section.
    RelocationRange,
    /// A REL24 relocation target is not aligned to a four-byte instruction.
    RelocationAlignment,
    /// A structural section payload has an invalid size.
    PayloadSize,
    /// A string offset is invalid or references an empty name.
    NameReference,
    /// A structural record name exceeds the supported bound.
    NameTooLong,
    /// A structural record name is not valid UTF-8.
    NameEncoding,
    /// A structural record duplicates a name of the same class.
    DuplicateName,
    /// An export address is targeted by a relocation.
    ExportRelocation,
    /// A structural section has unsupported section-header fields.
    SectionHeaderFields,
    /// The mandatory first symbol-table entry is not all zeroes.
    NullSymbol,
    /// A symbol has an unsupported binding.
    SymbolBinding,
    /// A symbol has unsupported visibility or reserved attributes.
    SymbolOther,
}
impl fmt::Display for RplRecordError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::EntrySize => "entry size",
            Self::RecordCount => "record count",
            Self::SectionLink => "section link",
            Self::SectionInfo => "section info",
            Self::SymbolIndex => "symbol index",
            Self::SymbolKind => "symbol kind",
            Self::SymbolRange => "symbol range",
            Self::RelocationType => "relocation type",
            Self::RelocationRange => "relocation range",
            Self::RelocationAlignment => "relocation alignment",
            Self::PayloadSize => "payload size",
            Self::NameReference => "name reference",
            Self::NameTooLong => "name length",
            Self::NameEncoding => "name encoding",
            Self::DuplicateName => "duplicate name",
            Self::ExportRelocation => "export relocation",
            Self::SectionHeaderFields => "section header fields",
            Self::NullSymbol => "null symbol",
            Self::SymbolBinding => "symbol binding",
            Self::SymbolOther => "symbol visibility",
        })
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct RawSection {
    name_offset: u32,
    section_type: u32,
    flags: u32,
    virtual_address: u32,
    file_offset: u32,
    size: u32,
    link: u32,
    info: u32,
    alignment: u32,
    entry_size: u32,
}

impl RawSection {
    fn from_bytes(bytes: &[u8], offset: usize) -> Self {
        Self {
            name_offset: read_u32(bytes, offset),
            section_type: read_u32(bytes, offset + 4),
            flags: read_u32(bytes, offset + 8),
            virtual_address: read_u32(bytes, offset + 12),
            file_offset: read_u32(bytes, offset + 16),
            size: read_u32(bytes, offset + 20),
            link: read_u32(bytes, offset + 24),
            info: read_u32(bytes, offset + 28),
            alignment: read_u32(bytes, offset + 32),
            entry_size: read_u32(bytes, offset + 36),
        }
    }

    const fn is_all_zero(self) -> bool {
        self.name_offset == 0
            && self.section_type == 0
            && self.flags == 0
            && self.virtual_address == 0
            && self.file_offset == 0
            && self.size == 0
            && self.link == 0
            && self.info == 0
            && self.alignment == 0
            && self.entry_size == 0
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ByteRange {
    start: u64,
    end: u64,
}

impl ByteRange {
    fn bounded(start: u64, size: u64, limit: u64) -> Option<Self> {
        let end = start.checked_add(size)?;
        (end <= limit).then_some(Self { start, end })
    }

    const fn overlaps(self, other: Self) -> bool {
        self.start < other.end && other.start < self.end
    }
}

/// Parse and fully validate the initial, uncompressed Cafe ELF32 RPX slice.
pub fn parse_rpx(bytes: &[u8]) -> Result<ParsedRpx, RpxError> {
    let module = parse_cafe_module(bytes, CafeModuleKind::Rpx)?;
    Ok(ParsedRpx {
        entry_point: module.entry_point,
        sections: module.sections,
        file_info: module.file_info,
        symbols: module.symbols,
        imports: module.imports,
        exports: module.exports,
        relocations: module.relocations,
    })
}

/// Parse and fully validate the minimal, uncompressed Cafe ELF32 RPL slice.
///
/// This first slice requires the ELF entry point to be zero.
pub fn parse_rpl(bytes: &[u8]) -> Result<ParsedRpl, RpxError> {
    let module = parse_cafe_module(bytes, CafeModuleKind::Rpl)?;
    Ok(ParsedRpl {
        entry_point: module.entry_point,
        sections: module.sections,
        file_info: module.file_info,
        symbols: module.symbols,
        imports: module.imports,
        exports: module.exports,
        relocations: module.relocations,
    })
}

struct ParsedCafeModule {
    entry_point: u32,
    sections: Vec<RpxSection>,
    file_info: RpxFileInfo,
    symbols: Vec<CafeSymbol>,
    imports: Vec<CafeImport>,
    exports: Vec<CafeExport>,
    relocations: Vec<CafeRelocation>,
}

fn parse_cafe_module(bytes: &[u8], kind: CafeModuleKind) -> Result<ParsedCafeModule, RpxError> {
    if bytes.len() < usize::try_from(ELF_HEADER_SIZE).expect("ELF header size fits usize") {
        return Err(RpxError::TruncatedHeader {
            actual: bytes.len(),
        });
    }
    if bytes.len() > MAX_RPX_IMAGE_SIZE {
        return Err(RpxError::ImageTooLarge {
            actual: bytes.len(),
            maximum: MAX_RPX_IMAGE_SIZE,
        });
    }
    validate_header(bytes)?;
    let image_size = u64::try_from(bytes.len()).map_err(|_| RpxError::ImageTooLarge {
        actual: bytes.len(),
        maximum: MAX_RPX_IMAGE_SIZE,
    })?;
    let entry_point = read_u32(bytes, 24);
    let section_table_offset = u64::from(read_u32(bytes, 32));
    let section_count = read_u16(bytes, 48);
    let name_section_index = usize::from(read_u16(bytes, 50));
    let section_table_size = SECTION_HEADER_SIZE
        .checked_mul(u64::from(section_count))
        .ok_or(RpxError::InvalidSectionTable)?;
    let section_table = ByteRange::bounded(section_table_offset, section_table_size, image_size)
        .ok_or(RpxError::InvalidSectionTable)?;
    let elf_header = ByteRange {
        start: 0,
        end: ELF_HEADER_SIZE,
    };
    if section_table.overlaps(elf_header) {
        return Err(RpxError::InvalidSectionTable);
    }

    let count = usize::from(section_count);
    let mut raw_sections = Vec::new();
    raw_sections
        .try_reserve_exact(count)
        .map_err(|_| RpxError::AllocationFailed {
            requested: count
                .checked_mul(size_of::<RawSection>())
                .unwrap_or(MAX_RPX_IMAGE_SIZE),
        })?;
    let table_start =
        usize::try_from(section_table.start).map_err(|_| RpxError::InvalidSectionTable)?;
    for index in 0..count {
        let offset = table_start
            .checked_add(index.checked_mul(40).ok_or(RpxError::InvalidSectionTable)?)
            .ok_or(RpxError::InvalidSectionTable)?;
        raw_sections.push(RawSection::from_bytes(bytes, offset));
    }
    validate_rpl_sections(bytes, &raw_sections, elf_header, section_table)?;
    let name_table = validate_section_names(bytes, &raw_sections, name_section_index)?;
    validate_rpl_name_lengths(name_table, &raw_sections)?;
    validate_terminal_sections(&raw_sections)?;
    let crc_index = raw_sections.len() - 2;
    let crc_section = raw_sections[crc_index];
    let expected_crc_size =
        u32::from(section_count)
            .checked_mul(4)
            .ok_or(RpxError::InvalidCrcTableSize {
                actual: crc_section.size,
                expected: u32::MAX,
            })?;
    if crc_section.size != expected_crc_size {
        return Err(RpxError::InvalidCrcTableSize {
            actual: crc_section.size,
            expected: expected_crc_size,
        });
    }
    let file_info = parse_file_info_for_kind(bytes, raw_sections[raw_sections.len() - 1], kind)?;
    validate_mapping_regions(&raw_sections, &file_info)?;
    validate_crcs(bytes, &raw_sections, crc_index)?;
    match kind {
        CafeModuleKind::Rpx => validate_entry_point(entry_point, &raw_sections)?,
        CafeModuleKind::Rpl if entry_point != 0 => return Err(RpxError::InvalidEntryPoint),
        CafeModuleKind::Rpl => {}
    }
    let (symbols, imports, exports, relocations) = parse_rpl_records(bytes, &raw_sections)?;
    let sections = own_section_vec(bytes, raw_sections, name_table)?;
    Ok(ParsedCafeModule {
        entry_point,
        sections,
        file_info,
        symbols,
        imports,
        exports,
        relocations,
    })
}

fn validate_rpl_sections(
    bytes: &[u8],
    sections: &[RawSection],
    elf_header: ByteRange,
    section_table: ByteRange,
) -> Result<(), RpxError> {
    if !sections[0].is_all_zero() {
        return Err(RpxError::InvalidNullSection { section_index: 0 });
    }
    let image_size = u64::try_from(bytes.len()).map_err(|_| RpxError::ImageTooLarge {
        actual: bytes.len(),
        maximum: MAX_RPX_IMAGE_SIZE,
    })?;
    let mut payload_ranges = Vec::new();
    payload_ranges
        .try_reserve_exact(sections.len())
        .map_err(|_| RpxError::AllocationFailed {
            requested: sections
                .len()
                .checked_mul(size_of::<(usize, ByteRange)>())
                .unwrap_or(MAX_RPX_IMAGE_SIZE),
        })?;
    let mut virtual_ranges = Vec::new();
    virtual_ranges
        .try_reserve_exact(sections.len())
        .map_err(|_| RpxError::AllocationFailed {
            requested: sections
                .len()
                .checked_mul(size_of::<(usize, ByteRange)>())
                .unwrap_or(MAX_RPX_IMAGE_SIZE),
        })?;
    for (index, section) in sections.iter().copied().enumerate() {
        validate_supported_rpl_section(index, section)?;
        validate_section_flags_and_address(index, section, &mut virtual_ranges)?;
        let payload = ByteRange::bounded(
            u64::from(section.file_offset),
            u64::from(section.size),
            image_size,
        )
        .ok_or(RpxError::InvalidSectionRange {
            section_index: index,
        })?;
        if section.size == 0 {
            continue;
        }
        if payload.overlaps(elf_header) {
            return Err(RpxError::SectionOverlapsHeader {
                section_index: index,
            });
        }
        if payload.overlaps(section_table) {
            return Err(RpxError::SectionOverlapsTable {
                section_index: index,
            });
        }
        if let Some((other, _)) = payload_ranges
            .iter()
            .find(|(_, range)| payload.overlaps(*range))
        {
            return Err(RpxError::SectionPayloadOverlap {
                first_index: *other,
                second_index: index,
            });
        }
        payload_ranges.push((index, payload));
    }
    Ok(())
}

fn validate_supported_rpl_section(index: usize, section: RawSection) -> Result<(), RpxError> {
    match section.section_type {
        SHT_NULL if index != 0 => {
            return Err(RpxError::InvalidNullSection {
                section_index: index,
            });
        }
        SHT_NULL | SHT_PROGBITS | SHT_STRTAB | SHT_SYMTAB | SHT_RELA | SHT_RPL_EXPORTS
        | SHT_RPL_IMPORTS | SHT_RPL_CRCS | SHT_RPL_FILEINFO => {}
        SHT_NOBITS => {
            return Err(RpxError::Unsupported {
                section_index: index,
                feature: RpxUnsupportedFeature::NoBits,
            });
        }
        SHT_DYNSYM => {
            return Err(RpxError::Unsupported {
                section_index: index,
                feature: RpxUnsupportedFeature::SymbolTable,
            });
        }
        other => {
            return Err(RpxError::Unsupported {
                section_index: index,
                feature: RpxUnsupportedFeature::SectionType(other),
            });
        }
    }
    if section.flags & SHF_RPL_COMPRESSED != 0 {
        return Err(RpxError::Unsupported {
            section_index: index,
            feature: RpxUnsupportedFeature::Compression,
        });
    }
    let unsupported = section.flags & !SHF_SUPPORTED;
    if unsupported != 0 {
        return Err(RpxError::Unsupported {
            section_index: index,
            feature: RpxUnsupportedFeature::SectionFlags(unsupported),
        });
    }
    let invalid_metadata_flags = if matches!(
        section.section_type,
        SHT_STRTAB | SHT_RPL_CRCS | SHT_RPL_FILEINFO
    ) {
        section.flags != 0
    } else if matches!(section.section_type, SHT_SYMTAB | SHT_RELA) {
        section.flags & (SHF_WRITE | SHF_EXECINSTR) != 0
    } else {
        false
    };
    if invalid_metadata_flags {
        return Err(RpxError::InvalidMetadataFlags {
            section_index: index,
            section_type: section.section_type,
            flags: section.flags,
        });
    }
    if matches!(section.section_type, SHT_RPL_IMPORTS | SHT_RPL_EXPORTS)
        && (section.link != 0 || section.info != 0 || section.entry_size != 0)
    {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::SectionHeaderFields,
        });
    }
    Ok(())
}

fn validate_rpl_name_lengths(table: &[u8], sections: &[RawSection]) -> Result<(), RpxError> {
    for (index, section) in sections.iter().enumerate() {
        let name =
            section_name_bytes(table, section.name_offset).ok_or(RpxError::InvalidSectionName {
                section_index: index,
            })?;
        if name.len() > MAX_RPL_NAME_SIZE {
            return Err(RpxError::InvalidSectionName {
                section_index: index,
            });
        }
    }
    Ok(())
}

type RplRecords = (
    Vec<CafeSymbol>,
    Vec<CafeImport>,
    Vec<CafeExport>,
    Vec<CafeRelocation>,
);

fn parse_rpl_records(bytes: &[u8], sections: &[RawSection]) -> Result<RplRecords, RpxError> {
    let mut symbols = Vec::new();
    let mut imports = Vec::new();
    let mut exports = Vec::new();
    let mut export_names = BTreeSet::new();
    let mut relocations = Vec::new();
    for (index, section) in sections.iter().copied().enumerate() {
        match section.section_type {
            SHT_RPL_IMPORTS => parse_import(bytes, index, section, &mut imports)?,
            SHT_RPL_EXPORTS => {
                parse_exports(bytes, index, section, &mut exports, &mut export_names)?;
            }
            SHT_SYMTAB => parse_symbols(bytes, sections, index, section, &mut symbols)?,
            SHT_RELA => parse_relocations(bytes, sections, index, section, &mut relocations)?,
            _ => {}
        }
    }
    validate_export_relocations(sections, &exports, &symbols, &relocations)?;
    Ok((symbols, imports, exports, relocations))
}

fn reserve_record<T>(records: &mut Vec<T>, additional: usize) -> Result<(), RpxError> {
    records
        .try_reserve_exact(additional)
        .map_err(|_| RpxError::AllocationFailed {
            requested: additional
                .checked_mul(size_of::<T>())
                .unwrap_or(MAX_RPX_IMAGE_SIZE),
        })
}

fn owned_rpl_name(data: &[u8], offset: u32, section_index: usize) -> Result<String, RpxError> {
    let raw = section_name_bytes(data, offset).ok_or(RpxError::InvalidRplRecord {
        section_index,
        reason: RplRecordError::NameReference,
    })?;
    if raw.len() > MAX_RPL_NAME_SIZE {
        return Err(RpxError::InvalidRplRecord {
            section_index,
            reason: RplRecordError::NameTooLong,
        });
    }
    let value = std::str::from_utf8(raw).map_err(|_| RpxError::InvalidRplRecord {
        section_index,
        reason: RplRecordError::NameEncoding,
    })?;
    let mut owned = String::new();
    owned
        .try_reserve_exact(value.len())
        .map_err(|_| RpxError::AllocationFailed {
            requested: value.len(),
        })?;
    owned.push_str(value);
    Ok(owned)
}

fn parse_import(
    bytes: &[u8],
    index: usize,
    section: RawSection,
    imports: &mut Vec<CafeImport>,
) -> Result<(), RpxError> {
    if imports.len() >= MAX_RPL_RECORDS {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::RecordCount,
        });
    }
    let data = section_payload(bytes, section);
    if data.len() < 9 {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::PayloadSize,
        });
    }
    let module_name = owned_rpl_name(data, 8, index)?;
    if module_name.is_empty() {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::NameReference,
        });
    }
    let kind = if section.flags & SHF_EXECINSTR != 0 {
        CafeSymbolKind::Function
    } else {
        CafeSymbolKind::Data
    };
    if imports
        .iter()
        .any(|record| record.module_name == module_name && record.kind == kind)
    {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::DuplicateName,
        });
    }
    reserve_record(imports, 1)?;
    imports.push(CafeImport {
        section_index: index,
        module_name,
        kind,
    });
    Ok(())
}

fn parse_exports(
    bytes: &[u8],
    index: usize,
    section: RawSection,
    exports: &mut Vec<CafeExport>,
    export_names: &mut BTreeSet<(CafeSymbolKind, String)>,
) -> Result<(), RpxError> {
    let data = section_payload(bytes, section);
    if data.len() < 8 {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::PayloadSize,
        });
    }
    let count = usize::try_from(read_u32(data, 0)).map_err(|_| RpxError::InvalidRplRecord {
        section_index: index,
        reason: RplRecordError::RecordCount,
    })?;
    if count > MAX_RPL_RECORDS
        || exports
            .len()
            .checked_add(count)
            .is_none_or(|value| value > MAX_RPL_RECORDS)
    {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::RecordCount,
        });
    }
    let descriptors_end = count
        .checked_mul(8)
        .and_then(|v| v.checked_add(8))
        .filter(|end| *end <= data.len())
        .ok_or(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::PayloadSize,
        })?;
    reserve_record(exports, count)?;
    let kind = if section.flags & SHF_EXECINSTR != 0 {
        CafeSymbolKind::Function
    } else {
        CafeSymbolKind::Data
    };
    for record in 0..count {
        let offset = 8 + record * 8;
        let address = read_u32(data, offset);
        let name_offset = read_u32(data, offset + 4);
        if usize::try_from(name_offset)
            .ok()
            .is_none_or(|value| value < descriptors_end)
        {
            return Err(RpxError::InvalidRplRecord {
                section_index: index,
                reason: RplRecordError::NameReference,
            });
        }
        let name = owned_rpl_name(data, name_offset, index)?;
        if name.is_empty() {
            return Err(RpxError::InvalidRplRecord {
                section_index: index,
                reason: RplRecordError::NameReference,
            });
        }
        if !export_names.insert((kind, name.clone())) {
            return Err(RpxError::InvalidRplRecord {
                section_index: index,
                reason: RplRecordError::DuplicateName,
            });
        }
        exports.push(CafeExport {
            section_index: index,
            descriptor_index: record,
            name,
            address,
            kind,
        });
    }
    Ok(())
}

fn validate_export_relocations(
    sections: &[RawSection],
    exports: &[CafeExport],
    symbols: &[CafeSymbol],
    relocations: &[CafeRelocation],
) -> Result<(), RpxError> {
    let mut exports_by_location = BTreeMap::new();
    for (index, export) in exports.iter().enumerate() {
        if let Some(offset) =
            export_address_field(sections[export.section_index], export.descriptor_index)
        {
            exports_by_location
                .entry((export.section_index, offset))
                .or_insert(index);
        }
    }
    let mut symbols_by_index = BTreeMap::new();
    for (index, symbol) in symbols.iter().enumerate() {
        symbols_by_index
            .entry((symbol.table_section_index, symbol.symbol_index))
            .or_insert(index);
    }
    let mut relocation_counts = BTreeMap::new();
    for relocation in relocations {
        *relocation_counts
            .entry((relocation.target_section_index, relocation.offset))
            .or_insert(0_usize) += 1;
    }
    let direct_address_ranges = direct_export_address_ranges(sections);

    for relocation in relocations
        .iter()
        .filter(|record| sections[record.target_section_index].section_type == SHT_RPL_EXPORTS)
    {
        if relocation.kind != CafeRelocationKind::Addr32 {
            return Err(RpxError::InvalidRplRecord {
                section_index: relocation.section_index,
                reason: RplRecordError::ExportRelocation,
            });
        }
        let Some(&export_index) =
            exports_by_location.get(&(relocation.target_section_index, relocation.offset))
        else {
            return Err(RpxError::InvalidRplRecord {
                section_index: relocation.section_index,
                reason: RplRecordError::ExportRelocation,
            });
        };
        let export = &exports[export_index];
        let Some(&symbol_index) = symbols_by_index.get(&(
            relocation.symbol_table_section_index,
            relocation.symbol_index,
        )) else {
            return Err(RpxError::InvalidRplRecord {
                section_index: relocation.section_index,
                reason: RplRecordError::ExportRelocation,
            });
        };
        let symbol = &symbols[symbol_index];
        if symbol.section_index == 0
            || sections[symbol.section_index].section_type != SHT_PROGBITS
            || symbol.kind != export.kind
            || symbol.name != export.name
            || !range_inside_section(symbol.value, symbol.size, sections[symbol.section_index])
        {
            return Err(RpxError::InvalidRplRecord {
                section_index: relocation.section_index,
                reason: RplRecordError::ExportRelocation,
            });
        }
    }

    for export in exports {
        let expected_offset =
            export_address_field(sections[export.section_index], export.descriptor_index).ok_or(
                RpxError::InvalidRplRecord {
                    section_index: export.section_index,
                    reason: RplRecordError::ExportRelocation,
                },
            )?;
        let relocation_count = relocation_counts
            .get(&(export.section_index, expected_offset))
            .copied()
            .unwrap_or(0);
        let valid = match relocation_count {
            0 => address_has_kind(&direct_address_ranges, export.address, export.kind),
            1 => export.address == 0,
            _ => false,
        };
        if !valid {
            return Err(RpxError::InvalidRplRecord {
                section_index: export.section_index,
                reason: RplRecordError::ExportRelocation,
            });
        }
    }
    Ok(())
}

fn direct_export_address_ranges(sections: &[RawSection]) -> BTreeMap<(CafeSymbolKind, u64), u64> {
    let mut ranges: BTreeMap<(CafeSymbolKind, u64), u64> = BTreeMap::new();
    for section in sections
        .iter()
        .filter(|section| section.section_type == SHT_PROGBITS && section.flags & SHF_ALLOC != 0)
    {
        let kind = if section.flags & SHF_EXECINSTR != 0 {
            CafeSymbolKind::Function
        } else {
            CafeSymbolKind::Data
        };
        let start = u64::from(section.virtual_address);
        let end = start + u64::from(section.size);
        ranges
            .entry((kind, start))
            .and_modify(|existing_end| *existing_end = (*existing_end).max(end))
            .or_insert(end);
    }
    ranges
}

fn export_address_field(section: RawSection, descriptor_index: usize) -> Option<u32> {
    let relative = descriptor_index.checked_mul(8)?.checked_add(8)?;
    u32::try_from(u64::from(section.virtual_address).checked_add(u64::try_from(relative).ok()?)?)
        .ok()
}

fn address_has_kind(
    ranges: &BTreeMap<(CafeSymbolKind, u64), u64>,
    address: u32,
    kind: CafeSymbolKind,
) -> bool {
    let address = u64::from(address);
    ranges
        .range(..=(kind, address))
        .next_back()
        .is_some_and(|(&(candidate_kind, start), &end)| {
            candidate_kind == kind && address >= start && address < end
        })
}

fn parse_symbols(
    bytes: &[u8],
    sections: &[RawSection],
    index: usize,
    section: RawSection,
    symbols: &mut Vec<CafeSymbol>,
) -> Result<(), RpxError> {
    if section.entry_size != 16 || !section.size.is_multiple_of(16) {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::EntrySize,
        });
    }
    let count = usize::try_from(section.size / 16).expect("bounded symbol count fits usize");
    if !(2..=MAX_RPL_RECORDS).contains(&count)
        || symbols
            .len()
            .checked_add(count)
            .is_none_or(|value| value > MAX_RPL_RECORDS)
    {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::RecordCount,
        });
    }
    let linked = usize::try_from(section.link)
        .ok()
        .filter(|v| *v < sections.len())
        .ok_or(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::SectionLink,
        })?;
    if sections[linked].section_type != SHT_STRTAB
        || usize::try_from(section.info)
            .ok()
            .is_none_or(|value| value > count)
    {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: if sections[linked].section_type == SHT_STRTAB {
                RplRecordError::SectionInfo
            } else {
                RplRecordError::SectionLink
            },
        });
    }
    let names = section_payload(bytes, sections[linked]);
    if names.is_empty() || names[0] != 0 || names.last() != Some(&0) {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::SectionLink,
        });
    }
    reserve_record(symbols, count)?;
    let data = section_payload(bytes, section);
    if data[..16].iter().any(|byte| *byte != 0) {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::NullSymbol,
        });
    }
    for symbol_index in 1..count {
        symbols.push(parse_symbol(data, names, sections, index, symbol_index)?);
    }
    Ok(())
}

fn parse_symbol(
    data: &[u8],
    names: &[u8],
    sections: &[RawSection],
    table_section_index: usize,
    symbol_index: usize,
) -> Result<CafeSymbol, RpxError> {
    let invalid = |reason| RpxError::InvalidRplRecord {
        section_index: table_section_index,
        reason,
    };
    let offset = symbol_index * 16;
    let value = read_u32(data, offset + 4);
    let size = read_u32(data, offset + 8);
    let info = data[offset + 12];
    let target = usize::from(read_u16(data, offset + 14));
    let name = owned_rpl_name(names, read_u32(data, offset), table_section_index)?;
    if name.is_empty() {
        return Err(invalid(RplRecordError::NameReference));
    }
    if info >> 4 != 1 {
        return Err(invalid(RplRecordError::SymbolBinding));
    }
    let kind = match info & 0x0f {
        1 => CafeSymbolKind::Data,
        2 => CafeSymbolKind::Function,
        _ => return Err(invalid(RplRecordError::SymbolKind)),
    };
    if data[offset + 13] != 0 {
        return Err(invalid(RplRecordError::SymbolOther));
    }
    if target == 0 || target >= sections.len() {
        return Err(invalid(RplRecordError::SymbolIndex));
    }
    let target_section = sections[target];
    let target_kind = if target_section.flags & SHF_EXECINSTR != 0 {
        CafeSymbolKind::Function
    } else {
        CafeSymbolKind::Data
    };
    if kind != target_kind {
        return Err(invalid(RplRecordError::SymbolKind));
    }
    let range_is_valid = if target_section.section_type == SHT_RPL_IMPORTS {
        range_inside_import_payload(value, size, target_section)
    } else {
        range_inside_section(value, size, target_section)
    };
    if !range_is_valid {
        return Err(invalid(RplRecordError::SymbolRange));
    }
    Ok(CafeSymbol {
        table_section_index,
        symbol_index,
        name,
        value,
        size,
        section_index: target,
        kind,
    })
}

fn range_inside_import_payload(value: u32, size: u32, section: RawSection) -> bool {
    let section_start = u64::from(section.virtual_address);
    let Some(payload_start) = section_start.checked_add(8) else {
        return false;
    };
    let Some(section_end) = section_start.checked_add(u64::from(section.size)) else {
        return false;
    };
    let value_start = u64::from(value);
    value_start >= payload_start
        && value_start < section_end
        && value_start
            .checked_add(u64::from(size))
            .is_some_and(|end| end <= section_end)
}

fn range_inside_section(value: u32, size: u32, section: RawSection) -> bool {
    if section.flags & SHF_ALLOC == 0 {
        return false;
    }
    let start = u64::from(section.virtual_address);
    let end = start + u64::from(section.size);
    let value_start = u64::from(value);
    value_start >= start
        && value_start < end
        && value_start
            .checked_add(u64::from(size))
            .is_some_and(|v| v <= end)
}

fn parse_relocations(
    bytes: &[u8],
    sections: &[RawSection],
    index: usize,
    section: RawSection,
    relocations: &mut Vec<CafeRelocation>,
) -> Result<(), RpxError> {
    if section.entry_size != 12 || !section.size.is_multiple_of(12) {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::EntrySize,
        });
    }
    let count = usize::try_from(section.size / 12).expect("bounded relocation count fits usize");
    if count > MAX_RPL_RECORDS
        || relocations
            .len()
            .checked_add(count)
            .is_none_or(|value| value > MAX_RPL_RECORDS)
    {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::RecordCount,
        });
    }
    let symtab = usize::try_from(section.link)
        .ok()
        .filter(|v| *v < sections.len())
        .ok_or(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::SectionLink,
        })?;
    let target = usize::try_from(section.info)
        .ok()
        .filter(|v| *v < sections.len() && *v != 0)
        .ok_or(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::SectionInfo,
        })?;
    if sections[symtab].section_type != SHT_SYMTAB {
        return Err(RpxError::InvalidRplRecord {
            section_index: index,
            reason: RplRecordError::SectionLink,
        });
    }
    let symbol_count =
        usize::try_from(sections[symtab].size / 16).expect("bounded symbol count fits usize");
    reserve_record(relocations, count)?;
    let data = section_payload(bytes, section);
    for record in 0..count {
        let record_offset = record * 12;
        let offset = read_u32(data, record_offset);
        let symbol_and_type = read_u32(data, record_offset + 4);
        let relocation_type = symbol_and_type & 0xff;
        let kind = match relocation_type {
            1 => CafeRelocationKind::Addr32,
            10 => CafeRelocationKind::Rel24,
            _ => {
                return Err(RpxError::InvalidRplRecord {
                    section_index: index,
                    reason: RplRecordError::RelocationType,
                });
            }
        };
        let symbol_index =
            usize::try_from(symbol_and_type >> 8).expect("24-bit symbol index fits usize");
        if symbol_index == 0 || symbol_index >= symbol_count {
            return Err(RpxError::InvalidRplRecord {
                section_index: index,
                reason: RplRecordError::SymbolIndex,
            });
        }
        if !range_inside_section(offset, 4, sections[target]) {
            return Err(RpxError::InvalidRplRecord {
                section_index: index,
                reason: RplRecordError::RelocationRange,
            });
        }
        if kind == CafeRelocationKind::Rel24 && !offset.is_multiple_of(4) {
            return Err(RpxError::InvalidRplRecord {
                section_index: index,
                reason: RplRecordError::RelocationAlignment,
            });
        }
        relocations.push(CafeRelocation {
            section_index: index,
            target_section_index: target,
            symbol_table_section_index: symtab,
            offset,
            symbol_index,
            addend: i32::from_be_bytes(
                data[record_offset + 8..record_offset + 12]
                    .try_into()
                    .expect("four-byte addend"),
            ),
            kind,
        });
    }
    Ok(())
}

fn validate_header(bytes: &[u8]) -> Result<(), RpxError> {
    if bytes[0..4] != [0x7f, b'E', b'L', b'F'] {
        return Err(RpxError::InvalidMagic);
    }
    let byte_fields = [
        (bytes[4] == 1, RpxHeaderField::Class),
        (bytes[5] == 2, RpxHeaderField::DataEncoding),
        (bytes[6] == 1, RpxHeaderField::IdentificationVersion),
        (
            bytes[7] == 0xca && bytes[8] == 0xfe,
            RpxHeaderField::CafeIdentification,
        ),
    ];
    for (valid, field) in byte_fields {
        if !valid {
            return Err(RpxError::InvalidHeader { field });
        }
    }

    let word_fields = [
        (read_u16(bytes, 16) == 0xfe01, RpxHeaderField::ObjectType),
        (read_u16(bytes, 18) == 20, RpxHeaderField::Machine),
        (read_u32(bytes, 20) == 1, RpxHeaderField::ObjectVersion),
        (read_u16(bytes, 40) == 52, RpxHeaderField::HeaderSize),
        (
            read_u32(bytes, 28) == 0,
            RpxHeaderField::ProgramHeaderOffset,
        ),
        (read_u16(bytes, 44) == 0, RpxHeaderField::ProgramHeaderCount),
        (read_u16(bytes, 46) == 40, RpxHeaderField::SectionHeaderSize),
    ];
    for (valid, field) in word_fields {
        if !valid {
            return Err(RpxError::InvalidHeader { field });
        }
    }

    let section_count = read_u16(bytes, 48);
    if !(2..=MAX_SECTIONS).contains(&section_count) {
        return Err(RpxError::InvalidHeader {
            field: RpxHeaderField::SectionCount,
        });
    }
    if read_u16(bytes, 50) >= section_count {
        return Err(RpxError::InvalidHeader {
            field: RpxHeaderField::SectionNameIndex,
        });
    }
    Ok(())
}

#[cfg(test)]
#[allow(dead_code)]
fn validate_sections(
    bytes: &[u8],
    sections: &[RawSection],
    elf_header: ByteRange,
    section_table: ByteRange,
) -> Result<(), RpxError> {
    if !sections[0].is_all_zero() {
        return Err(RpxError::InvalidNullSection { section_index: 0 });
    }

    let image_size = u64::try_from(bytes.len()).map_err(|_| RpxError::ImageTooLarge {
        actual: bytes.len(),
        maximum: MAX_RPX_IMAGE_SIZE,
    })?;
    let mut payload_ranges: Vec<(usize, ByteRange)> = Vec::new();
    payload_ranges
        .try_reserve_exact(sections.len())
        .map_err(|_| RpxError::AllocationFailed {
            requested: sections
                .len()
                .checked_mul(size_of::<(usize, ByteRange)>())
                .unwrap_or(MAX_RPX_IMAGE_SIZE),
        })?;
    let mut virtual_ranges: Vec<(usize, ByteRange)> = Vec::new();
    virtual_ranges
        .try_reserve_exact(sections.len())
        .map_err(|_| RpxError::AllocationFailed {
            requested: sections
                .len()
                .checked_mul(size_of::<(usize, ByteRange)>())
                .unwrap_or(MAX_RPX_IMAGE_SIZE),
        })?;

    for (index, section) in sections.iter().copied().enumerate() {
        validate_supported_section(index, section)?;
        validate_section_flags_and_address(index, section, &mut virtual_ranges)?;
        let payload = ByteRange::bounded(
            u64::from(section.file_offset),
            u64::from(section.size),
            image_size,
        )
        .ok_or(RpxError::InvalidSectionRange {
            section_index: index,
        })?;
        if section.size == 0 {
            continue;
        }
        if payload.overlaps(elf_header) {
            return Err(RpxError::SectionOverlapsHeader {
                section_index: index,
            });
        }
        if payload.overlaps(section_table) {
            return Err(RpxError::SectionOverlapsTable {
                section_index: index,
            });
        }
        if let Some((other_index, _)) = payload_ranges
            .iter()
            .find(|(_, other)| payload.overlaps(*other))
        {
            return Err(RpxError::SectionPayloadOverlap {
                first_index: *other_index,
                second_index: index,
            });
        }
        payload_ranges.push((index, payload));
    }
    Ok(())
}

#[cfg(test)]
fn validate_supported_section(index: usize, section: RawSection) -> Result<(), RpxError> {
    match section.section_type {
        SHT_NULL if index != 0 => {
            return Err(RpxError::InvalidNullSection {
                section_index: index,
            });
        }
        SHT_NULL | SHT_PROGBITS | SHT_STRTAB | SHT_RPL_CRCS | SHT_RPL_FILEINFO => {}
        SHT_SYMTAB | SHT_DYNSYM => {
            return Err(RpxError::Unsupported {
                section_index: index,
                feature: RpxUnsupportedFeature::SymbolTable,
            });
        }
        SHT_RELA => {
            return Err(RpxError::Unsupported {
                section_index: index,
                feature: RpxUnsupportedFeature::Relocation,
            });
        }
        SHT_NOBITS => {
            return Err(RpxError::Unsupported {
                section_index: index,
                feature: RpxUnsupportedFeature::NoBits,
            });
        }
        SHT_RPL_EXPORTS => {
            return Err(RpxError::Unsupported {
                section_index: index,
                feature: RpxUnsupportedFeature::Exports,
            });
        }
        SHT_RPL_IMPORTS => {
            return Err(RpxError::Unsupported {
                section_index: index,
                feature: RpxUnsupportedFeature::Imports,
            });
        }
        section_type => {
            return Err(RpxError::Unsupported {
                section_index: index,
                feature: RpxUnsupportedFeature::SectionType(section_type),
            });
        }
    }
    if section.flags & SHF_RPL_COMPRESSED != 0 {
        return Err(RpxError::Unsupported {
            section_index: index,
            feature: RpxUnsupportedFeature::Compression,
        });
    }
    let unsupported_flags = section.flags & !SHF_SUPPORTED;
    if unsupported_flags != 0 {
        return Err(RpxError::Unsupported {
            section_index: index,
            feature: RpxUnsupportedFeature::SectionFlags(unsupported_flags),
        });
    }
    if matches!(
        section.section_type,
        SHT_STRTAB | SHT_RPL_CRCS | SHT_RPL_FILEINFO
    ) && section.flags != 0
    {
        return Err(RpxError::InvalidMetadataFlags {
            section_index: index,
            section_type: section.section_type,
            flags: section.flags,
        });
    }
    Ok(())
}

fn validate_section_flags_and_address(
    index: usize,
    section: RawSection,
    virtual_ranges: &mut Vec<(usize, ByteRange)>,
) -> Result<(), RpxError> {
    if !valid_alignment(section.alignment) {
        return Err(RpxError::InvalidAlignment {
            section_index: index,
            alignment: section.alignment,
        });
    }
    if section.alignment != 0 && !section.virtual_address.is_multiple_of(section.alignment) {
        return Err(RpxError::MisalignedVirtualAddress {
            section_index: index,
        });
    }
    if section.flags & (SHF_WRITE | SHF_EXECINSTR) == (SHF_WRITE | SHF_EXECINSTR) {
        return Err(RpxError::WriteExecute {
            section_index: index,
        });
    }
    if section.flags & SHF_ALLOC == 0 {
        if section.virtual_address != 0 {
            return Err(RpxError::NonAllocatedAddress {
                section_index: index,
            });
        }
        return Ok(());
    }
    if section.size == 0 {
        return Ok(());
    }

    let virtual_range = ByteRange::bounded(
        u64::from(section.virtual_address),
        u64::from(section.size),
        GUEST_ADDRESS_SPACE_SIZE,
    )
    .ok_or(RpxError::VirtualAddressOverflow {
        section_index: index,
    })?;
    if let Some((other_index, _)) = virtual_ranges
        .iter()
        .find(|(_, other)| virtual_range.overlaps(*other))
    {
        return Err(RpxError::VirtualAddressOverlap {
            first_index: *other_index,
            second_index: index,
        });
    }
    virtual_ranges.push((index, virtual_range));
    Ok(())
}

fn validate_section_names<'a>(
    bytes: &'a [u8],
    sections: &[RawSection],
    name_section_index: usize,
) -> Result<&'a [u8], RpxError> {
    let name_section = sections[name_section_index];
    if name_section.section_type != SHT_STRTAB {
        return Err(RpxError::InvalidStringTable);
    }
    let name_table = section_payload(bytes, name_section);
    if name_table.is_empty() || name_table[0] != 0 || name_table.last() != Some(&0) {
        return Err(RpxError::InvalidStringTable);
    }

    let mut aggregate_name_size = 0_usize;
    for (index, section) in sections.iter().enumerate() {
        let name = section_name_bytes(name_table, section.name_offset).ok_or(
            RpxError::InvalidSectionName {
                section_index: index,
            },
        )?;
        std::str::from_utf8(name).map_err(|_| RpxError::InvalidSectionName {
            section_index: index,
        })?;
        aggregate_name_size =
            aggregate_name_size
                .checked_add(name.len())
                .ok_or(RpxError::InvalidSectionName {
                    section_index: index,
                })?;
        if aggregate_name_size > MAX_RPX_IMAGE_SIZE {
            return Err(RpxError::InvalidSectionName {
                section_index: index,
            });
        }
    }
    Ok(name_table)
}

fn validate_terminal_sections(sections: &[RawSection]) -> Result<(), RpxError> {
    if sections.len() < 3
        || sections[sections.len() - 2].section_type != SHT_RPL_CRCS
        || sections[sections.len() - 1].section_type != SHT_RPL_FILEINFO
    {
        return Err(RpxError::InvalidSectionOrder);
    }
    let crc_count = sections
        .iter()
        .filter(|section| section.section_type == SHT_RPL_CRCS)
        .count();
    let file_info_count = sections
        .iter()
        .filter(|section| section.section_type == SHT_RPL_FILEINFO)
        .count();
    if crc_count != 1 || file_info_count != 1 {
        return Err(RpxError::InvalidSectionOrder);
    }
    Ok(())
}

fn parse_file_info_for_kind(
    bytes: &[u8],
    section: RawSection,
    kind: CafeModuleKind,
) -> Result<RpxFileInfo, RpxError> {
    if section.size < FILEINFO_SIZE {
        return Err(RpxError::FileInfoTooSmall {
            actual: section.size,
            minimum: FILEINFO_SIZE,
        });
    }
    let data = section_payload(bytes, section);
    let file_info = RpxFileInfo {
        magic: read_u32(data, 0),
        text_region_size: read_u32(data, 4),
        text_alignment: read_u32(data, 8),
        data_region_size: read_u32(data, 12),
        data_alignment: read_u32(data, 16),
        loader_region_size: read_u32(data, 20),
        trampoline_adjustment: read_u32(data, 32),
        loader_adjustment: read_u32(data, 76),
        flags: read_u32(data, 52),
    };
    if file_info.magic != FILEINFO_MAGIC {
        return Err(RpxError::InvalidFileInfo {
            field: RpxFileInfoField::Magic,
            value: file_info.magic,
        });
    }
    validate_file_info_region(RpxFileInfoField::TextRegionSize, file_info.text_region_size)?;
    validate_file_info_region(RpxFileInfoField::DataRegionSize, file_info.data_region_size)?;
    validate_file_info_region(
        RpxFileInfoField::LoaderRegionSize,
        file_info.loader_region_size,
    )?;
    validate_file_info_alignment(RpxFileInfoField::TextAlignment, file_info.text_alignment)?;
    validate_file_info_alignment(RpxFileInfoField::DataAlignment, file_info.data_alignment)?;
    if file_info.trampoline_adjustment > file_info.text_region_size {
        return Err(RpxError::InvalidFileInfo {
            field: RpxFileInfoField::TrampolineAdjustment,
            value: file_info.trampoline_adjustment,
        });
    }
    if file_info.loader_adjustment > file_info.loader_region_size {
        return Err(RpxError::InvalidFileInfo {
            field: RpxFileInfoField::LoaderAdjustment,
            value: file_info.loader_adjustment,
        });
    }
    validate_file_info_flags_for_kind(file_info.flags, kind)?;
    Ok(file_info)
}

fn validate_file_info_region(field: RpxFileInfoField, value: u32) -> Result<(), RpxError> {
    if u64::from(value) > u64::try_from(MAX_RPX_IMAGE_SIZE).expect("image limit fits u64") {
        return Err(RpxError::InvalidFileInfo { field, value });
    }
    Ok(())
}

fn validate_file_info_alignment(field: RpxFileInfoField, value: u32) -> Result<(), RpxError> {
    if !valid_alignment(value) {
        return Err(RpxError::InvalidFileInfo { field, value });
    }
    Ok(())
}

fn validate_file_info_flags(flags: u32) -> Result<(), RpxError> {
    if flags & FILEINFO_RPX_FLAG == 0 {
        return Err(RpxError::InvalidFileInfo {
            field: RpxFileInfoField::Flags,
            value: flags,
        });
    }
    Ok(())
}

fn validate_file_info_flags_for_kind(flags: u32, kind: CafeModuleKind) -> Result<(), RpxError> {
    if matches!(kind, CafeModuleKind::Rpx) {
        return validate_file_info_flags(flags);
    }
    if flags & FILEINFO_RPX_FLAG == 0 {
        Ok(())
    } else {
        Err(RpxError::InvalidFileInfo {
            field: RpxFileInfoField::Flags,
            value: flags,
        })
    }
}

const fn classify_mapping_region(section: RawSection) -> Option<RpxMappingRegion> {
    if section.size == 0 || section.flags & SHF_ALLOC == 0 {
        return None;
    }
    if matches!(
        section.section_type,
        SHT_SYMTAB | SHT_RELA | SHT_RPL_EXPORTS | SHT_RPL_IMPORTS
    ) {
        return Some(RpxMappingRegion::Loader);
    }
    if section.flags & SHF_EXECINSTR != 0 {
        return Some(RpxMappingRegion::Text);
    }
    if section.flags & SHF_WRITE != 0 {
        return Some(RpxMappingRegion::Data);
    }
    Some(RpxMappingRegion::Loader)
}

fn validate_mapping_regions(
    sections: &[RawSection],
    file_info: &RpxFileInfo,
) -> Result<(), RpxError> {
    let mut text_begin: Option<u64> = None;
    let mut data_begin: Option<u64> = None;
    let mut loader_begin: Option<u64> = None;
    for section in sections.iter().copied() {
        let Some(region) = classify_mapping_region(section) else {
            continue;
        };
        let begin = u64::from(section.virtual_address);
        let minimum = match region {
            RpxMappingRegion::Text => &mut text_begin,
            RpxMappingRegion::Data => &mut data_begin,
            RpxMappingRegion::Loader => &mut loader_begin,
        };
        *minimum = Some(minimum.map_or(begin, |current| current.min(begin)));
    }

    for (section_index, section) in sections.iter().copied().enumerate() {
        let Some(region) = classify_mapping_region(section) else {
            continue;
        };
        let region_begin = match region {
            RpxMappingRegion::Text => text_begin,
            RpxMappingRegion::Data => data_begin,
            RpxMappingRegion::Loader => loader_begin,
        }
        .expect("a classified section established its region minimum");
        let (declared_size, adjustment) = match region {
            RpxMappingRegion::Text => (
                u64::from(file_info.text_region_size),
                u64::from(file_info.trampoline_adjustment),
            ),
            RpxMappingRegion::Data => (u64::from(file_info.data_region_size), 0),
            RpxMappingRegion::Loader => (
                u64::from(file_info.loader_region_size),
                u64::from(file_info.loader_adjustment),
            ),
        };
        let usable_size = declared_size
            .checked_sub(adjustment)
            .expect("FILEINFO adjustments were validated before mapping regions");
        let overflow = RpxError::RegionAddressOverflow {
            section_index,
            region,
        };
        region_begin
            .checked_add(declared_size)
            .filter(|end| *end <= GUEST_ADDRESS_SPACE_SIZE)
            .ok_or(overflow)?;
        let region_end = region_begin
            .checked_add(usable_size)
            .filter(|end| *end <= GUEST_ADDRESS_SPACE_SIZE)
            .ok_or(RpxError::RegionAddressOverflow {
                section_index,
                region,
            })?;
        let section_begin = u64::from(section.virtual_address);
        let section_end = section_begin
            .checked_add(u64::from(section.size))
            .filter(|end| *end <= GUEST_ADDRESS_SPACE_SIZE)
            .ok_or(RpxError::SectionOutsideRegion {
                section_index,
                region,
            })?;
        if section_begin < region_begin || section_end > region_end {
            return Err(RpxError::SectionOutsideRegion {
                section_index,
                region,
            });
        }
    }
    Ok(())
}

fn validate_crcs(bytes: &[u8], sections: &[RawSection], crc_index: usize) -> Result<(), RpxError> {
    let crc_table = section_payload(bytes, sections[crc_index]);
    for (index, section) in sections.iter().copied().enumerate() {
        let word_offset = index.checked_mul(4).ok_or(RpxError::InvalidCrcTableSize {
            actual: sections[crc_index].size,
            expected: u32::MAX,
        })?;
        let expected = read_u32(crc_table, word_offset);
        let actual = if matches!(section.section_type, SHT_NULL | SHT_NOBITS | SHT_RPL_CRCS) {
            0
        } else {
            crc32(section_payload(bytes, section))
        };
        if expected != actual {
            return Err(RpxError::CrcMismatch {
                section_index: index,
                expected,
                actual,
            });
        }
    }
    Ok(())
}

fn validate_entry_point(entry_point: u32, sections: &[RawSection]) -> Result<(), RpxError> {
    if !entry_point.is_multiple_of(4) {
        return Err(RpxError::InvalidEntryPoint);
    }
    let entry_start = u64::from(entry_point);
    let entry_end = entry_start
        .checked_add(4)
        .filter(|end| *end <= GUEST_ADDRESS_SPACE_SIZE)
        .ok_or(RpxError::InvalidEntryPoint)?;
    let is_mapped = sections.iter().any(|section| {
        if section.section_type != SHT_PROGBITS
            || section.flags & (SHF_ALLOC | SHF_EXECINSTR) != (SHF_ALLOC | SHF_EXECINSTR)
        {
            return false;
        }
        let start = u64::from(section.virtual_address);
        let Some(end) = start.checked_add(u64::from(section.size)) else {
            return false;
        };
        entry_start >= start && entry_end <= end
    });
    if !is_mapped {
        return Err(RpxError::InvalidEntryPoint);
    }
    Ok(())
}

fn own_section_vec(
    bytes: &[u8],
    raw_sections: Vec<RawSection>,
    name_table: &[u8],
) -> Result<Vec<RpxSection>, RpxError> {
    let mut sections = Vec::new();
    sections
        .try_reserve_exact(raw_sections.len())
        .map_err(|_| RpxError::AllocationFailed {
            requested: raw_sections
                .len()
                .checked_mul(size_of::<RpxSection>())
                .unwrap_or(MAX_RPX_IMAGE_SIZE),
        })?;
    for (index, raw) in raw_sections.into_iter().enumerate() {
        let name_bytes = section_name_bytes(name_table, raw.name_offset).ok_or(
            RpxError::InvalidSectionName {
                section_index: index,
            },
        )?;
        let name_str =
            std::str::from_utf8(name_bytes).map_err(|_| RpxError::InvalidSectionName {
                section_index: index,
            })?;
        let mut name = String::new();
        name.try_reserve_exact(name_str.len())
            .map_err(|_| RpxError::AllocationFailed {
                requested: name_str.len(),
            })?;
        name.push_str(name_str);

        let source = section_payload(bytes, raw);
        let mut data = Vec::new();
        data.try_reserve_exact(source.len())
            .map_err(|_| RpxError::AllocationFailed {
                requested: source.len(),
            })?;
        data.extend_from_slice(source);
        sections.push(RpxSection {
            index,
            name,
            section_type: raw.section_type,
            flags: raw.flags,
            virtual_address: raw.virtual_address,
            file_offset: raw.file_offset,
            alignment: raw.alignment,
            mapping_region: classify_mapping_region(raw),
            data,
        });
    }
    Ok(sections)
}

fn section_payload(bytes: &[u8], section: RawSection) -> &[u8] {
    let start = usize::try_from(section.file_offset).expect("validated u32 offset fits usize");
    let size = usize::try_from(section.size).expect("validated u32 size fits usize");
    let end = start
        .checked_add(size)
        .expect("validated section payload range does not overflow");
    &bytes[start..end]
}

fn section_name_bytes(table: &[u8], offset: u32) -> Option<&[u8]> {
    let start = usize::try_from(offset).ok()?;
    let suffix = table.get(start..)?;
    let length = suffix.iter().position(|byte| *byte == 0)?;
    suffix.get(..length)
}

const fn valid_alignment(alignment: u32) -> bool {
    alignment == 0 || (alignment.is_power_of_two() && alignment <= MAX_ALIGNMENT)
}

fn read_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_be_bytes([bytes[offset], bytes[offset + 1]])
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = u32::MAX;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0_u32.wrapping_sub(crc & 1);
            crc = (crc >> 1) ^ (0xedb8_8320 & mask);
        }
    }
    !crc
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpx_fixture::builtin_rpx_fixture;

    const FIXTURE_SECTION_TABLE_OFFSET: usize = 0x40;
    const FIXTURE_SECTION_HEADER_SIZE: usize = 0x28;
    const FIXTURE_TEXT_OFFSET: usize = 0x108;
    const FIXTURE_NAMES_OFFSET: usize = 0x114;
    const FIXTURE_NAMES_SIZE: usize = 17;
    const FIXTURE_CRC_OFFSET: usize = 0x128;
    const FIXTURE_FILE_INFO_OFFSET: usize = 0x13c;
    const FIXTURE_FILE_INFO_SIZE: usize = 0x60;

    fn fixture_section_header(index: usize) -> usize {
        FIXTURE_SECTION_TABLE_OFFSET + index * FIXTURE_SECTION_HEADER_SIZE
    }

    fn write_fixture_u32(bytes: &mut [u8], offset: usize, value: u32) {
        bytes[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
    }

    fn refresh_fixture_file_info_crc(bytes: &mut [u8]) {
        let checksum = crc32(
            &bytes[FIXTURE_FILE_INFO_OFFSET..FIXTURE_FILE_INFO_OFFSET + FIXTURE_FILE_INFO_SIZE],
        );
        write_fixture_u32(bytes, FIXTURE_CRC_OFFSET + 16, checksum);
    }

    const EXT_SECTION_COUNT: usize = 10;
    const EXT_TABLE_OFFSET: usize = 0x40;
    const EXT_TEXT_OFFSET: usize = 0x1d0;
    const EXT_STRINGS_OFFSET: usize = 0x1d5;
    const EXT_IMPORT_OFFSET: usize = 0x1da;
    const EXT_EXPORT_OFFSET: usize = 0x1e6;
    const EXT_SYMBOL_OFFSET: usize = 0x1fa;
    const EXT_RELOCATION_OFFSET: usize = 0x21a;
    const EXT_CRC_OFFSET: usize = 0x226;
    const EXT_FILE_INFO_OFFSET: usize = 0x24e;
    const EXT_IMAGE_SIZE: usize = 0x2ae;

    #[derive(Clone, Copy)]
    struct ExtendedSection {
        section_type: u32,
        flags: u32,
        address: u32,
        offset: usize,
        size: usize,
        link: u32,
        info: u32,
        alignment: u32,
        entry_size: u32,
    }

    const EXT_SECTIONS: [ExtendedSection; 9] = [
        ExtendedSection {
            section_type: SHT_PROGBITS,
            flags: SHF_ALLOC | SHF_EXECINSTR,
            address: 0x1000,
            offset: EXT_TEXT_OFFSET,
            size: 4,
            link: 0,
            info: 0,
            alignment: 4,
            entry_size: 0,
        },
        ExtendedSection {
            section_type: SHT_STRTAB,
            flags: 0,
            address: 0,
            offset: 0x1d4,
            size: 1,
            link: 0,
            info: 0,
            alignment: 1,
            entry_size: 0,
        },
        ExtendedSection {
            section_type: SHT_STRTAB,
            flags: 0,
            address: 0,
            offset: EXT_STRINGS_OFFSET,
            size: 5,
            link: 0,
            info: 0,
            alignment: 1,
            entry_size: 0,
        },
        ExtendedSection {
            section_type: SHT_RPL_IMPORTS,
            flags: SHF_ALLOC | SHF_EXECINSTR,
            address: 0x2000,
            offset: EXT_IMPORT_OFFSET,
            size: 12,
            link: 0,
            info: 0,
            alignment: 4,
            entry_size: 0,
        },
        ExtendedSection {
            section_type: SHT_RPL_EXPORTS,
            flags: SHF_ALLOC | SHF_EXECINSTR,
            address: 0x2010,
            offset: EXT_EXPORT_OFFSET,
            size: 20,
            link: 0,
            info: 0,
            alignment: 4,
            entry_size: 0,
        },
        ExtendedSection {
            section_type: SHT_SYMTAB,
            flags: 0,
            address: 0,
            offset: EXT_SYMBOL_OFFSET,
            size: 32,
            link: 3,
            info: 1,
            alignment: 4,
            entry_size: 16,
        },
        ExtendedSection {
            section_type: SHT_RELA,
            flags: 0,
            address: 0,
            offset: EXT_RELOCATION_OFFSET,
            size: 12,
            link: 6,
            info: 1,
            alignment: 4,
            entry_size: 12,
        },
        ExtendedSection {
            section_type: SHT_RPL_CRCS,
            flags: 0,
            address: 0,
            offset: EXT_CRC_OFFSET,
            size: 40,
            link: 0,
            info: 0,
            alignment: 4,
            entry_size: 0,
        },
        ExtendedSection {
            section_type: SHT_RPL_FILEINFO,
            flags: 0,
            address: 0,
            offset: EXT_FILE_INFO_OFFSET,
            size: 96,
            link: 0,
            info: 0,
            alignment: 4,
            entry_size: 0,
        },
    ];

    fn write_extended_sections(bytes: &mut [u8]) {
        for (index, section) in EXT_SECTIONS.iter().enumerate() {
            let header = EXT_TABLE_OFFSET + (index + 1) * 40;
            let offset = u32::try_from(section.offset).expect("fixture offset");
            let size = u32::try_from(section.size).expect("fixture size");
            for (field, value) in [
                (4, section.section_type),
                (8, section.flags),
                (12, section.address),
                (16, offset),
                (20, size),
                (24, section.link),
                (28, section.info),
                (32, section.alignment),
                (36, section.entry_size),
            ] {
                write_fixture_u32(bytes, header + field, value);
            }
        }
    }

    fn write_extended_payload(bytes: &mut [u8], kind: CafeModuleKind) {
        bytes[EXT_TEXT_OFFSET..EXT_TEXT_OFFSET + 4].copy_from_slice(&[0x60, 0, 0, 0]);
        bytes[EXT_STRINGS_OFFSET..EXT_STRINGS_OFFSET + 5].copy_from_slice(b"\0foo\0");
        bytes[EXT_IMPORT_OFFSET + 8..EXT_IMPORT_OFFSET + 12].copy_from_slice(b"mod\0");
        write_fixture_u32(bytes, EXT_EXPORT_OFFSET, 1);
        write_fixture_u32(bytes, EXT_EXPORT_OFFSET + 8, 0x1000);
        write_fixture_u32(bytes, EXT_EXPORT_OFFSET + 12, 16);
        bytes[EXT_EXPORT_OFFSET + 16..EXT_EXPORT_OFFSET + 20].copy_from_slice(b"exp\0");
        write_fixture_u32(bytes, EXT_SYMBOL_OFFSET + 16, 1);
        write_fixture_u32(bytes, EXT_SYMBOL_OFFSET + 20, 0x1000);
        write_fixture_u32(bytes, EXT_SYMBOL_OFFSET + 24, 4);
        bytes[EXT_SYMBOL_OFFSET + 28] = 0x12;
        bytes[EXT_SYMBOL_OFFSET + 30..EXT_SYMBOL_OFFSET + 32].copy_from_slice(&1_u16.to_be_bytes());
        write_fixture_u32(bytes, EXT_RELOCATION_OFFSET, 0x1000);
        write_fixture_u32(bytes, EXT_RELOCATION_OFFSET + 4, (1 << 8) | 1);
        write_fixture_u32(bytes, EXT_FILE_INFO_OFFSET, FILEINFO_MAGIC);
        write_fixture_u32(bytes, EXT_FILE_INFO_OFFSET + 4, 4);
        write_fixture_u32(bytes, EXT_FILE_INFO_OFFSET + 8, 4);
        write_fixture_u32(bytes, EXT_FILE_INFO_OFFSET + 20, 0x40);
        let flags = if matches!(kind, CafeModuleKind::Rpx) {
            FILEINFO_RPX_FLAG
        } else {
            0
        };
        write_fixture_u32(bytes, EXT_FILE_INFO_OFFSET + 52, flags);
    }

    fn write_extended_checksums(bytes: &mut [u8]) {
        for index in 0..EXT_SECTION_COUNT {
            let header = EXT_TABLE_OFFSET + index * 40;
            let section_type = read_u32(bytes, header + 4);
            let offset = usize::try_from(read_u32(bytes, header + 16)).expect("fixture offset");
            let size = usize::try_from(read_u32(bytes, header + 20)).expect("fixture size");
            let checksum = if matches!(section_type, SHT_NULL | SHT_RPL_CRCS) {
                0
            } else {
                crc32(&bytes[offset..offset + size])
            };
            write_fixture_u32(bytes, EXT_CRC_OFFSET + index * 4, checksum);
        }
    }

    fn extended_cafe_fixture(kind: CafeModuleKind) -> Vec<u8> {
        let mut bytes = vec![0_u8; EXT_IMAGE_SIZE];
        bytes[0..9].copy_from_slice(&[0x7f, b'E', b'L', b'F', 1, 2, 1, 0xca, 0xfe]);
        bytes[16..18].copy_from_slice(&0xfe01_u16.to_be_bytes());
        bytes[18..20].copy_from_slice(&20_u16.to_be_bytes());
        write_fixture_u32(&mut bytes, 20, 1);
        let entry_point = if matches!(kind, CafeModuleKind::Rpx) {
            0x1000
        } else {
            0
        };
        write_fixture_u32(&mut bytes, 24, entry_point);
        write_fixture_u32(
            &mut bytes,
            32,
            u32::try_from(EXT_TABLE_OFFSET).expect("fixture table offset"),
        );
        bytes[40..42].copy_from_slice(&52_u16.to_be_bytes());
        bytes[46..48].copy_from_slice(&40_u16.to_be_bytes());
        bytes[48..50].copy_from_slice(
            &u16::try_from(EXT_SECTION_COUNT)
                .expect("fixture section count")
                .to_be_bytes(),
        );
        bytes[50..52].copy_from_slice(&2_u16.to_be_bytes());
        write_extended_sections(&mut bytes);
        write_extended_payload(&mut bytes, kind);
        write_extended_checksums(&mut bytes);
        bytes
    }

    #[test]
    fn builtin_fixture_parses_without_mutating_input() {
        let image = builtin_rpx_fixture();
        let snapshot = image.clone();

        let parsed = parse_rpx(&image).expect("the bundled RPX fixture must parse");

        assert_eq!(parsed.entry_point(), 0x0200_0000);
        assert!(parsed.is_rpx());
        assert_eq!(parsed.sections().len(), 5);
        assert!(parsed.symbols().is_empty());
        assert!(parsed.imports().is_empty());
        assert!(parsed.exports().is_empty());
        assert!(parsed.relocations().is_empty());
        assert_eq!(
            parsed.sections()[1].mapping_region(),
            Some(RpxMappingRegion::Text)
        );
        assert_eq!(parsed.sections()[2].mapping_region(), None);
        assert_eq!(image, snapshot);
    }

    #[test]
    fn extended_rpx_records_are_accepted_and_exposed() {
        let image = extended_cafe_fixture(CafeModuleKind::Rpx);
        let parsed = parse_rpx(&image).expect("extended RPX records must parse");

        assert_eq!(parsed.module_kind(), CafeModuleKind::Rpx);
        assert_eq!(parsed.symbols().len(), 1);
        assert_eq!(parsed.symbols()[0].name(), "foo");
        assert_eq!(parsed.imports().len(), 1);
        assert_eq!(parsed.imports()[0].module_name(), "mod");
        assert_eq!(parsed.exports().len(), 1);
        assert_eq!(parsed.exports()[0].name(), "exp");
        assert_eq!(parsed.relocations().len(), 1);
        assert_eq!(parsed.relocations()[0].kind(), CafeRelocationKind::Addr32);
        assert_eq!(
            parsed.sections()[4].mapping_region(),
            Some(RpxMappingRegion::Loader)
        );
        assert_eq!(
            parsed.sections()[5].mapping_region(),
            Some(RpxMappingRegion::Loader)
        );
    }

    #[test]
    fn extended_rel24_record_is_accepted_and_exposed() {
        let mut image = extended_cafe_fixture(CafeModuleKind::Rpx);
        write_fixture_u32(&mut image, EXT_RELOCATION_OFFSET + 4, (1 << 8) | 0x0a);
        write_extended_checksums(&mut image);

        let parsed = parse_rpx(&image).expect("type 10 REL24 record must parse");

        assert_eq!(parsed.relocations().len(), 1);
        assert_eq!(parsed.relocations()[0].kind(), CafeRelocationKind::Rel24);
    }

    #[test]
    fn extended_unsupported_relocation_types_are_rejected() {
        for relocation_type in [0, 4, 5, 6, 11, 251, 252, 253, 255] {
            let mut image = extended_cafe_fixture(CafeModuleKind::Rpx);
            write_fixture_u32(
                &mut image,
                EXT_RELOCATION_OFFSET + 4,
                (1 << 8) | relocation_type,
            );
            write_extended_checksums(&mut image);

            assert_eq!(
                parse_rpx(&image),
                Err(RpxError::InvalidRplRecord {
                    section_index: 7,
                    reason: RplRecordError::RelocationType,
                })
            );
        }
    }

    #[test]
    fn cafe_module_kind_mismatch_is_rejected() {
        let rpx = extended_cafe_fixture(CafeModuleKind::Rpx);
        assert_eq!(
            parse_rpl(&rpx),
            Err(RpxError::InvalidFileInfo {
                field: RpxFileInfoField::Flags,
                value: FILEINFO_RPX_FLAG,
            })
        );

        let rpl = extended_cafe_fixture(CafeModuleKind::Rpl);
        assert_eq!(
            parse_rpx(&rpl),
            Err(RpxError::InvalidFileInfo {
                field: RpxFileInfoField::Flags,
                value: 0,
            })
        );
        assert_eq!(parse_rpl(&rpl).expect("matching RPL kind").entry_point(), 0);
    }

    #[test]
    fn rpl_entry_point_must_be_zero_even_when_executable() {
        for entry_point in [1, 0x1000] {
            let mut rpl = extended_cafe_fixture(CafeModuleKind::Rpl);
            write_fixture_u32(&mut rpl, 24, entry_point);

            assert_eq!(parse_rpl(&rpl), Err(RpxError::InvalidEntryPoint));
        }

        let mut bad_crc = extended_cafe_fixture(CafeModuleKind::Rpl);
        write_fixture_u32(&mut bad_crc, 24, 1);
        bad_crc[EXT_TEXT_OFFSET] ^= 1;
        assert_eq!(
            parse_rpl(&bad_crc),
            Err(RpxError::CrcMismatch {
                section_index: 1,
                expected: read_u32(&bad_crc, EXT_CRC_OFFSET + 4),
                actual: crc32(&bad_crc[EXT_TEXT_OFFSET..EXT_TEXT_OFFSET + 4]),
            })
        );
    }

    #[test]
    fn rejects_zero_or_undersized_text_mapping_region() {
        for region_size in [0, 11] {
            let mut image = builtin_rpx_fixture();
            write_fixture_u32(&mut image, FIXTURE_FILE_INFO_OFFSET + 4, region_size);
            refresh_fixture_file_info_crc(&mut image);

            assert_eq!(
                parse_rpx(&image),
                Err(RpxError::SectionOutsideRegion {
                    section_index: 1,
                    region: RpxMappingRegion::Text,
                })
            );
        }
    }

    #[test]
    fn rejects_text_region_reduced_below_section_by_adjustment() {
        let mut image = builtin_rpx_fixture();
        write_fixture_u32(&mut image, FIXTURE_FILE_INFO_OFFSET + 4, 12);
        write_fixture_u32(&mut image, FIXTURE_FILE_INFO_OFFSET + 32, 1);
        refresh_fixture_file_info_crc(&mut image);

        assert_eq!(
            parse_rpx(&image),
            Err(RpxError::SectionOutsideRegion {
                section_index: 1,
                region: RpxMappingRegion::Text,
            })
        );
    }

    #[test]
    fn accepts_exact_region_fit_at_full_guest_address_space_endpoint() {
        const TEXT_ADDRESS: u32 = 0xffff_fff4;

        let mut image = builtin_rpx_fixture();
        write_fixture_u32(&mut image, 24, TEXT_ADDRESS);
        write_fixture_u32(&mut image, fixture_section_header(1) + 12, TEXT_ADDRESS);
        write_fixture_u32(&mut image, FIXTURE_FILE_INFO_OFFSET + 4, 12);
        refresh_fixture_file_info_crc(&mut image);

        let parsed = parse_rpx(&image)
            .expect("an exact half-open mapping fit ending at 2^32 must remain valid");
        assert_eq!(parsed.entry_point(), TEXT_ADDRESS);
        assert_eq!(parsed.file_info().text_region_size(), 12);
    }

    #[test]
    fn rejects_declared_mapping_region_past_guest_address_space() {
        const TEXT_ADDRESS: u32 = 0xffff_fff4;

        let mut image = builtin_rpx_fixture();
        write_fixture_u32(&mut image, fixture_section_header(1) + 12, TEXT_ADDRESS);

        assert_eq!(
            parse_rpx(&image),
            Err(RpxError::RegionAddressOverflow {
                section_index: 1,
                region: RpxMappingRegion::Text,
            })
        );
    }

    #[test]
    fn classifies_data_and_loader_sections_for_region_containment() {
        for (flags, region) in [
            (SHF_ALLOC | SHF_WRITE, RpxMappingRegion::Data),
            (SHF_ALLOC, RpxMappingRegion::Loader),
        ] {
            let mut image = builtin_rpx_fixture();
            write_fixture_u32(&mut image, fixture_section_header(1) + 8, flags);

            assert_eq!(
                parse_rpx(&image),
                Err(RpxError::SectionOutsideRegion {
                    section_index: 1,
                    region,
                })
            );
        }
    }

    #[test]
    fn parsed_debug_summaries_redact_names_and_payload_bytes() {
        const NAME_SENTINEL: &[u8; 5] = b"N4M3!";
        const PAYLOAD_SENTINEL: &[u8; 12] = b"PAYLOADLEAK!";

        let mut image = builtin_rpx_fixture();
        image[FIXTURE_TEXT_OFFSET..FIXTURE_TEXT_OFFSET + PAYLOAD_SENTINEL.len()]
            .copy_from_slice(PAYLOAD_SENTINEL);
        image[FIXTURE_NAMES_OFFSET + 1..FIXTURE_NAMES_OFFSET + 1 + NAME_SENTINEL.len()]
            .copy_from_slice(NAME_SENTINEL);
        let text_crc = crc32(PAYLOAD_SENTINEL);
        let names_crc =
            crc32(&image[FIXTURE_NAMES_OFFSET..FIXTURE_NAMES_OFFSET + FIXTURE_NAMES_SIZE]);
        write_fixture_u32(&mut image, FIXTURE_CRC_OFFSET + 4, text_crc);
        write_fixture_u32(&mut image, FIXTURE_CRC_OFFSET + 8, names_crc);

        let parsed = parse_rpx(&image).expect("the sentinel RPX fixture must parse");
        let image_debug = format!("{parsed:?}");
        let section_debug = format!("{:?}", parsed.sections()[1]);

        for summary in [&image_debug, &section_debug] {
            assert!(!summary.contains("N4M3!"));
            assert!(!summary.contains("PAYLOADLEAK!"));
        }
        assert!(image_debug.contains("section_count"));
        assert!(section_debug.contains("data_len"));
    }

    #[test]
    fn failure_does_not_mutate_input() {
        let mut image = builtin_rpx_fixture();
        image[0] = 0;
        let snapshot = image.clone();

        assert_eq!(parse_rpx(&image), Err(RpxError::InvalidMagic));
        assert_eq!(image, snapshot);
    }

    #[test]
    fn rejects_truncated_header_and_invalid_magic() {
        let image = builtin_rpx_fixture();
        assert_eq!(
            parse_rpx(&image[..51]),
            Err(RpxError::TruncatedHeader { actual: 51 })
        );

        let mut invalid_magic = image;
        invalid_magic[3] ^= 1;
        assert_eq!(parse_rpx(&invalid_magic), Err(RpxError::InvalidMagic));
    }

    #[test]
    fn rejects_section_table_overlapping_header() {
        let mut image = builtin_rpx_fixture();
        write_fixture_u32(&mut image, 32, 0);

        assert_eq!(parse_rpx(&image), Err(RpxError::InvalidSectionTable));
    }

    #[test]
    fn rejects_nonzero_null_section() {
        let mut image = builtin_rpx_fixture();
        write_fixture_u32(&mut image, FIXTURE_SECTION_TABLE_OFFSET, 1);

        assert_eq!(
            parse_rpx(&image),
            Err(RpxError::InvalidNullSection { section_index: 0 })
        );
    }

    #[test]
    fn rejects_compression_and_write_execute() {
        let flags_offset = fixture_section_header(1) + 8;
        let mut compressed = builtin_rpx_fixture();
        write_fixture_u32(
            &mut compressed,
            flags_offset,
            SHF_ALLOC | SHF_EXECINSTR | SHF_RPL_COMPRESSED,
        );
        assert_eq!(
            parse_rpx(&compressed),
            Err(RpxError::Unsupported {
                section_index: 1,
                feature: RpxUnsupportedFeature::Compression,
            })
        );

        let mut writable_executable = builtin_rpx_fixture();
        write_fixture_u32(
            &mut writable_executable,
            flags_offset,
            SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR,
        );
        assert_eq!(
            parse_rpx(&writable_executable),
            Err(RpxError::WriteExecute { section_index: 1 })
        );
    }

    #[test]
    fn rejects_payload_overlap_with_table_or_another_payload() {
        let mut table_overlap = builtin_rpx_fixture();
        write_fixture_u32(
            &mut table_overlap,
            fixture_section_header(1) + 16,
            u32::try_from(FIXTURE_SECTION_TABLE_OFFSET).expect("fixture offset fits u32"),
        );
        assert_eq!(
            parse_rpx(&table_overlap),
            Err(RpxError::SectionOverlapsTable { section_index: 1 })
        );

        let mut payload_overlap = builtin_rpx_fixture();
        write_fixture_u32(
            &mut payload_overlap,
            fixture_section_header(2) + 16,
            u32::try_from(FIXTURE_TEXT_OFFSET + 4).expect("fixture offset fits u32"),
        );
        assert_eq!(
            parse_rpx(&payload_overlap),
            Err(RpxError::SectionPayloadOverlap {
                first_index: 1,
                second_index: 2,
            })
        );
    }

    #[test]
    fn rejects_crc_mismatch_after_text_mutation() {
        let mut image = builtin_rpx_fixture();
        image[FIXTURE_TEXT_OFFSET] ^= 1;
        let expected = read_u32(&image, FIXTURE_CRC_OFFSET + 4);
        let actual = crc32(&image[FIXTURE_TEXT_OFFSET..FIXTURE_TEXT_OFFSET + 12]);

        assert_eq!(
            parse_rpx(&image),
            Err(RpxError::CrcMismatch {
                section_index: 1,
                expected,
                actual,
            })
        );
    }

    #[test]
    fn rejects_rpl_flag_before_stale_file_info_crc() {
        let mut image = builtin_rpx_fixture();
        write_fixture_u32(&mut image, FIXTURE_FILE_INFO_OFFSET + 52, 0);

        assert_eq!(
            parse_rpx(&image),
            Err(RpxError::InvalidFileInfo {
                field: RpxFileInfoField::Flags,
                value: 0,
            })
        );
    }

    #[test]
    fn rejects_unaligned_or_out_of_executable_range_entry() {
        let mut unaligned = builtin_rpx_fixture();
        write_fixture_u32(&mut unaligned, 24, 0x0200_0001);
        assert_eq!(parse_rpx(&unaligned), Err(RpxError::InvalidEntryPoint));

        let mut outside = builtin_rpx_fixture();
        write_fixture_u32(&mut outside, 24, 0x0200_000c);
        assert_eq!(parse_rpx(&outside), Err(RpxError::InvalidEntryPoint));
    }

    #[test]
    fn bounded_range_rejects_overflow_and_out_of_bounds() {
        assert_eq!(ByteRange::bounded(u64::MAX, 1, u64::MAX), None);
        assert_eq!(ByteRange::bounded(8, 3, 10), None);
        assert_eq!(
            ByteRange::bounded(8, 2, 10),
            Some(ByteRange { start: 8, end: 10 })
        );
    }

    #[test]
    fn half_open_range_overlap_handles_touching_edges() {
        let first = ByteRange { start: 4, end: 8 };
        assert!(!first.overlaps(ByteRange { start: 8, end: 12 }));
        assert!(first.overlaps(ByteRange { start: 7, end: 12 }));
    }

    #[test]
    fn allocated_range_may_end_at_guest_address_space_boundary() {
        let section = RawSection {
            section_type: SHT_PROGBITS,
            flags: SHF_ALLOC,
            virtual_address: 0xffff_fffc,
            size: 4,
            alignment: 4,
            ..RawSection::default()
        };
        let mut ranges = Vec::new();

        assert_eq!(
            validate_section_flags_and_address(1, section, &mut ranges),
            Ok(())
        );
        assert_eq!(
            ranges,
            vec![(
                1,
                ByteRange {
                    start: 0xffff_fffc,
                    end: GUEST_ADDRESS_SPACE_SIZE,
                }
            )]
        );
    }

    #[test]
    fn allocated_range_may_not_wrap_past_guest_address_space() {
        let section = RawSection {
            section_type: SHT_PROGBITS,
            flags: SHF_ALLOC,
            virtual_address: 0xffff_fffc,
            size: 8,
            alignment: 4,
            ..RawSection::default()
        };

        assert_eq!(
            validate_section_flags_and_address(1, section, &mut Vec::new()),
            Err(RpxError::VirtualAddressOverflow { section_index: 1 })
        );
    }

    #[test]
    fn file_info_must_identify_a_main_rpx() {
        assert_eq!(
            validate_file_info_flags(0),
            Err(RpxError::InvalidFileInfo {
                field: RpxFileInfoField::Flags,
                value: 0,
            })
        );
        assert_eq!(validate_file_info_flags(FILEINFO_RPX_FLAG), Ok(()));
    }

    #[test]
    fn null_section_is_supported_only_at_index_zero() {
        assert_eq!(validate_supported_section(0, RawSection::default()), Ok(()));
        assert_eq!(
            validate_supported_section(1, RawSection::default()),
            Err(RpxError::InvalidNullSection { section_index: 1 })
        );
    }

    #[test]
    fn metadata_sections_must_not_request_guest_permissions() {
        for (index, section_type) in [SHT_STRTAB, SHT_RPL_CRCS, SHT_RPL_FILEINFO]
            .into_iter()
            .enumerate()
        {
            let section = RawSection {
                section_type,
                flags: SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR,
                ..RawSection::default()
            };
            assert_eq!(
                validate_supported_section(index + 1, section),
                Err(RpxError::InvalidMetadataFlags {
                    section_index: index + 1,
                    section_type,
                    flags: SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR,
                })
            );
        }
    }

    #[test]
    fn ieee_crc32_matches_standard_check_value() {
        assert_eq!(crc32(b""), 0);
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
    }

    #[test]
    fn parses_minimal_rpl_structural_records() {
        let mut bytes = vec![0; 96];
        bytes[0..5].copy_from_slice(b"\0foo\0");
        bytes[8..20].copy_from_slice(b"\0\0\0\0\0\0\0\0mod\0");
        bytes[40..56].copy_from_slice(&[0, 0, 0, 1, 0, 0, 0x10, 0, 0, 0, 0, 4, 0x12, 0, 0, 1]);
        write_fixture_u32(&mut bytes, 56, 0x1000);
        write_fixture_u32(&mut bytes, 60, (1 << 8) | 1);
        write_fixture_u32(&mut bytes, 64, u32::MAX);
        write_fixture_u32(&mut bytes, 68, 1);
        write_fixture_u32(&mut bytes, 76, 0x1000);
        write_fixture_u32(&mut bytes, 80, 16);
        bytes[84..88].copy_from_slice(b"exp\0");
        let sections = vec![
            RawSection::default(),
            RawSection {
                section_type: SHT_PROGBITS,
                flags: SHF_ALLOC | SHF_EXECINSTR,
                virtual_address: 0x1000,
                size: 4,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_STRTAB,
                file_offset: 0,
                size: 5,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_RPL_IMPORTS,
                flags: SHF_EXECINSTR,
                file_offset: 8,
                size: 12,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_RPL_EXPORTS,
                flags: SHF_EXECINSTR,
                file_offset: 68,
                size: 20,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_SYMTAB,
                file_offset: 24,
                size: 32,
                link: 2,
                info: 1,
                entry_size: 16,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_RELA,
                file_offset: 56,
                size: 12,
                link: 5,
                info: 1,
                entry_size: 12,
                ..RawSection::default()
            },
        ];

        let mut imports = Vec::new();
        parse_import(&bytes, 3, sections[3], &mut imports).expect("minimal import");
        assert_eq!(imports[0].module_name(), "mod");
        let mut exports = Vec::new();
        parse_exports(&bytes, 4, sections[4], &mut exports, &mut BTreeSet::new())
            .expect("minimal export");
        assert_eq!(exports[0].name(), "exp");
        let mut symbols = Vec::new();
        parse_symbols(&bytes, &sections, 5, sections[5], &mut symbols).expect("minimal symbol");
        assert_eq!(symbols[0].name(), "foo");
        let mut relocations = Vec::new();
        parse_relocations(&bytes, &sections, 6, sections[6], &mut relocations)
            .expect("minimal ADDR32 relocation");
        assert_eq!(relocations[0].kind(), CafeRelocationKind::Addr32);
        assert_eq!(relocations[0].addend(), -1);
    }

    fn export_semantics(
        address: u32,
        symbol_kind: CafeSymbolKind,
        relocation_offsets: &[u32],
    ) -> (
        Vec<RawSection>,
        Vec<CafeExport>,
        Vec<CafeSymbol>,
        Vec<CafeRelocation>,
    ) {
        let sections = vec![
            RawSection::default(),
            RawSection {
                section_type: SHT_PROGBITS,
                flags: SHF_ALLOC | SHF_EXECINSTR,
                virtual_address: 0x1000,
                size: 4,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_RPL_EXPORTS,
                flags: SHF_ALLOC | SHF_EXECINSTR,
                virtual_address: 0x2000,
                size: 20,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_SYMTAB,
                size: 32,
                entry_size: 16,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_RELA,
                link: 3,
                info: 2,
                entry_size: 12,
                ..RawSection::default()
            },
        ];
        let exports = vec![CafeExport {
            section_index: 2,
            descriptor_index: 0,
            name: "redacted".to_owned(),
            address,
            kind: CafeSymbolKind::Function,
        }];
        let symbols = vec![CafeSymbol {
            table_section_index: 3,
            symbol_index: 1,
            name: "redacted".to_owned(),
            value: 0x1000,
            size: 4,
            section_index: 1,
            kind: symbol_kind,
        }];
        let relocations = relocation_offsets
            .iter()
            .map(|offset| CafeRelocation {
                section_index: 4,
                target_section_index: 2,
                symbol_table_section_index: 3,
                offset: *offset,
                symbol_index: 1,
                addend: 0,
                kind: CafeRelocationKind::Addr32,
            })
            .collect();
        (sections, exports, symbols, relocations)
    }

    #[test]
    fn zero_export_accepts_one_exact_local_addr32_relocation() {
        let (sections, exports, symbols, relocations) =
            export_semantics(0, CafeSymbolKind::Function, &[0x2008]);
        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Ok(())
        );
    }

    #[test]
    fn export_descriptor_rejects_rel24_relocation() {
        let (sections, exports, symbols, mut relocations) =
            export_semantics(0, CafeSymbolKind::Function, &[0x2008]);
        relocations[0].kind = CafeRelocationKind::Rel24;

        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Err(RpxError::InvalidRplRecord {
                section_index: 4,
                reason: RplRecordError::ExportRelocation,
            })
        );
    }

    #[test]
    fn export_name_may_start_at_descriptor_end() {
        let mut bytes = [0_u8; 20];
        write_fixture_u32(&mut bytes, 0, 1);
        write_fixture_u32(&mut bytes, 8, 0x1000);
        write_fixture_u32(&mut bytes, 12, 16);
        bytes[16..20].copy_from_slice(b"exp\0");
        let section = RawSection {
            section_type: SHT_RPL_EXPORTS,
            flags: SHF_EXECINSTR,
            size: 20,
            ..RawSection::default()
        };
        let mut exports = Vec::new();
        parse_exports(&bytes, 2, section, &mut exports, &mut BTreeSet::new())
            .expect("name begins at descriptor end");
        assert_eq!(exports[0].name(), "exp");
    }

    #[test]
    fn maximum_exports_and_relocations_use_bounded_indexes() {
        const NAME_WIDTH: usize = 9;

        let descriptors_end = 8 + MAX_RPL_RECORDS * 8;
        let mut bytes = vec![0_u8; descriptors_end + MAX_RPL_RECORDS * NAME_WIDTH];
        write_fixture_u32(
            &mut bytes,
            0,
            u32::try_from(MAX_RPL_RECORDS).expect("record limit fits u32"),
        );
        for record in 0..MAX_RPL_RECORDS {
            let descriptor = 8 + record * 8;
            let name_offset = descriptors_end + record * NAME_WIDTH;
            let address = if record + 1 == MAX_RPL_RECORDS {
                0x1000
            } else {
                0
            };
            write_fixture_u32(&mut bytes, descriptor, address);
            write_fixture_u32(
                &mut bytes,
                descriptor + 4,
                u32::try_from(name_offset).expect("bounded name offset fits u32"),
            );
            let name = format!("{record:08x}");
            bytes[name_offset..name_offset + name.len()].copy_from_slice(name.as_bytes());
        }
        let export_section = RawSection {
            section_type: SHT_RPL_EXPORTS,
            flags: SHF_ALLOC | SHF_EXECINSTR,
            virtual_address: 0x1_0000,
            size: u32::try_from(bytes.len()).expect("bounded export payload fits u32"),
            ..RawSection::default()
        };
        let mut exports = Vec::new();
        let mut export_names = BTreeSet::new();
        parse_exports(&bytes, 2, export_section, &mut exports, &mut export_names)
            .expect("the exact export record limit is accepted");
        assert_eq!(exports.len(), MAX_RPL_RECORDS);

        let sections = vec![
            RawSection::default(),
            RawSection {
                section_type: SHT_PROGBITS,
                flags: SHF_ALLOC | SHF_EXECINSTR,
                virtual_address: 0x1000,
                size: u32::try_from((MAX_RPL_RECORDS - 1) * 4).expect("bounded text size fits u32"),
                ..RawSection::default()
            },
            export_section,
            RawSection {
                section_type: SHT_SYMTAB,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_RELA,
                ..RawSection::default()
            },
        ];
        let symbols: Vec<_> = exports[..MAX_RPL_RECORDS - 1]
            .iter()
            .enumerate()
            .map(|(record, export)| CafeSymbol {
                table_section_index: 3,
                symbol_index: record + 1,
                name: export.name.clone(),
                value: 0x1000 + u32::try_from(record * 4).expect("bounded symbol offset"),
                size: 4,
                section_index: 1,
                kind: CafeSymbolKind::Function,
            })
            .collect();
        let relocations: Vec<_> = (0..MAX_RPL_RECORDS - 1)
            .map(|record| CafeRelocation {
                section_index: 4,
                target_section_index: 2,
                symbol_table_section_index: 3,
                offset: 0x1_0008 + u32::try_from(record * 8).expect("bounded relocation offset"),
                symbol_index: record + 1,
                addend: 0,
                kind: CafeRelocationKind::Addr32,
            })
            .collect();

        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Ok(())
        );
    }

    #[test]
    fn zero_export_without_relocation_is_rejected() {
        let (sections, exports, symbols, relocations) =
            export_semantics(0, CafeSymbolKind::Function, &[]);
        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Err(RpxError::InvalidRplRecord {
                section_index: 2,
                reason: RplRecordError::ExportRelocation
            })
        );
    }

    #[test]
    fn export_relocation_rejects_name_field_wrong_kind_and_duplicate() {
        let (sections, exports, symbols, relocations) =
            export_semantics(0, CafeSymbolKind::Function, &[0x200c]);
        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Err(RpxError::InvalidRplRecord {
                section_index: 4,
                reason: RplRecordError::ExportRelocation
            })
        );

        let (sections, exports, symbols, relocations) =
            export_semantics(0, CafeSymbolKind::Data, &[0x2008]);
        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Err(RpxError::InvalidRplRecord {
                section_index: 4,
                reason: RplRecordError::ExportRelocation
            })
        );

        let (sections, exports, symbols, relocations) =
            export_semantics(0, CafeSymbolKind::Function, &[0x2008, 0x2008]);
        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Err(RpxError::InvalidRplRecord {
                section_index: 2,
                reason: RplRecordError::ExportRelocation
            })
        );
    }

    #[test]
    fn export_relocation_rejects_wrong_symbol_table_or_index() {
        let (sections, exports, symbols, mut relocations) =
            export_semantics(0, CafeSymbolKind::Function, &[0x2008]);
        relocations[0].symbol_table_section_index = 1;
        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Err(RpxError::InvalidRplRecord {
                section_index: 4,
                reason: RplRecordError::ExportRelocation
            })
        );
        relocations[0].symbol_table_section_index = 3;
        relocations[0].symbol_index = 0;
        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Err(RpxError::InvalidRplRecord {
                section_index: 4,
                reason: RplRecordError::ExportRelocation
            })
        );
    }

    #[test]
    fn export_relocation_rejects_garbage_direct_and_name_mismatch() {
        for address in [1, 0x1000] {
            let (sections, exports, symbols, relocations) =
                export_semantics(address, CafeSymbolKind::Function, &[0x2008]);
            assert_eq!(
                validate_export_relocations(&sections, &exports, &symbols, &relocations),
                Err(RpxError::InvalidRplRecord {
                    section_index: 2,
                    reason: RplRecordError::ExportRelocation
                })
            );
        }

        let (sections, exports, mut symbols, relocations) =
            export_semantics(0, CafeSymbolKind::Function, &[0x2008]);
        symbols[0].name = "different".to_owned();
        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Err(RpxError::InvalidRplRecord {
                section_index: 4,
                reason: RplRecordError::ExportRelocation
            })
        );
    }

    #[test]
    fn directly_valid_export_address_remains_accepted() {
        let (sections, exports, symbols, relocations) =
            export_semantics(0x1000, CafeSymbolKind::Function, &[]);
        assert_eq!(
            validate_export_relocations(&sections, &exports, &symbols, &relocations),
            Ok(())
        );
    }

    #[test]
    fn import_symbol_range_excludes_header_and_end() {
        let import = RawSection {
            section_type: SHT_RPL_IMPORTS,
            virtual_address: 0x2000,
            size: 12,
            ..RawSection::default()
        };
        assert!(!range_inside_import_payload(0x2007, 1, import));
        assert!(range_inside_import_payload(0x2008, 4, import));
        assert!(!range_inside_import_payload(0x200c, 0, import));
        assert!(!range_inside_import_payload(0x2008, 5, import));
    }

    #[test]
    fn import_and_export_unused_header_fields_must_be_zero() {
        for (index, section) in [
            RawSection {
                section_type: SHT_RPL_IMPORTS,
                link: 1,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_RPL_EXPORTS,
                info: 1,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_RPL_IMPORTS,
                entry_size: 1,
                ..RawSection::default()
            },
        ]
        .into_iter()
        .enumerate()
        {
            assert_eq!(
                validate_supported_rpl_section(index + 1, section),
                Err(RpxError::InvalidRplRecord {
                    section_index: index + 1,
                    reason: RplRecordError::SectionHeaderFields
                })
            );
        }
    }

    fn canonical_symbol_fixture(info: u8, other: u8) -> (Vec<u8>, Vec<RawSection>) {
        let mut bytes = vec![0_u8; 37];
        write_fixture_u32(&mut bytes, 16, 1);
        write_fixture_u32(&mut bytes, 20, 0x1000);
        write_fixture_u32(&mut bytes, 24, 4);
        bytes[28] = info;
        bytes[29] = other;
        bytes[30..32].copy_from_slice(&1_u16.to_be_bytes());
        bytes[32..37].copy_from_slice(b"\0sym\0");
        let target_flags = SHF_ALLOC | if info & 0x0f == 2 { SHF_EXECINSTR } else { 0 };
        let sections = vec![
            RawSection::default(),
            RawSection {
                section_type: SHT_PROGBITS,
                flags: target_flags,
                virtual_address: 0x1000,
                size: 4,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_STRTAB,
                file_offset: 32,
                size: 5,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_SYMTAB,
                file_offset: 0,
                size: 32,
                link: 2,
                info: 1,
                entry_size: 16,
                ..RawSection::default()
            },
        ];
        (bytes, sections)
    }

    #[test]
    fn symtab_null_symbol_must_be_completely_zero() {
        for byte_offset in [0, 4, 8, 12, 13, 15] {
            let (mut bytes, sections) = canonical_symbol_fixture(0x12, 0);
            bytes[byte_offset] = 1;
            assert_eq!(
                parse_symbols(&bytes, &sections, 3, sections[3], &mut Vec::new()),
                Err(RpxError::InvalidRplRecord {
                    section_index: 3,
                    reason: RplRecordError::NullSymbol
                })
            );
        }
    }

    #[test]
    fn symtab_requires_global_binding_and_zero_other() {
        for info in [0x02, 0x22, 0x32] {
            let (bytes, sections) = canonical_symbol_fixture(info, 0);
            assert_eq!(
                parse_symbols(&bytes, &sections, 3, sections[3], &mut Vec::new()),
                Err(RpxError::InvalidRplRecord {
                    section_index: 3,
                    reason: RplRecordError::SymbolBinding
                })
            );
        }
        let (bytes, sections) = canonical_symbol_fixture(0x12, 1);
        assert_eq!(
            parse_symbols(&bytes, &sections, 3, sections[3], &mut Vec::new()),
            Err(RpxError::InvalidRplRecord {
                section_index: 3,
                reason: RplRecordError::SymbolOther
            })
        );
    }

    #[test]
    fn global_function_and_object_symbols_are_accepted() {
        for (info, expected) in [
            (0x12, CafeSymbolKind::Function),
            (0x11, CafeSymbolKind::Data),
        ] {
            let (bytes, sections) = canonical_symbol_fixture(info, 0);
            let mut symbols = Vec::new();
            parse_symbols(&bytes, &sections, 3, sections[3], &mut symbols)
                .expect("canonical global symbol");
            assert_eq!(symbols.len(), 1);
            assert_eq!(symbols[0].kind(), expected);
        }
    }

    #[test]
    fn zero_sized_function_and_object_symbols_at_section_end_are_rejected() {
        for info in [0x12, 0x11] {
            let (mut bytes, sections) = canonical_symbol_fixture(info, 0);
            write_fixture_u32(&mut bytes, 20, 0x1004);
            write_fixture_u32(&mut bytes, 24, 0);

            assert_eq!(
                parse_symbols(&bytes, &sections, 3, sections[3], &mut Vec::new()),
                Err(RpxError::InvalidRplRecord {
                    section_index: 3,
                    reason: RplRecordError::SymbolRange,
                })
            );
        }
    }

    #[test]
    fn rejects_rpl_record_count_name_and_layout_bounds() {
        let mut oversized_count = [0_u8; 8];
        write_fixture_u32(&mut oversized_count, 0, 4097);
        let export = RawSection {
            section_type: SHT_RPL_EXPORTS,
            size: 8,
            ..RawSection::default()
        };
        assert_eq!(
            parse_exports(
                &oversized_count,
                1,
                export,
                &mut Vec::new(),
                &mut BTreeSet::new()
            ),
            Err(RpxError::InvalidRplRecord {
                section_index: 1,
                reason: RplRecordError::RecordCount,
            })
        );

        let mut long_name = vec![b'x'; MAX_RPL_NAME_SIZE + 1];
        long_name.push(0);
        assert_eq!(
            owned_rpl_name(&long_name, 0, 2),
            Err(RpxError::InvalidRplRecord {
                section_index: 2,
                reason: RplRecordError::NameTooLong,
            })
        );

        let invalid_symtab = RawSection {
            section_type: SHT_SYMTAB,
            entry_size: 15,
            ..RawSection::default()
        };
        assert_eq!(
            parse_symbols(
                &[],
                &[RawSection::default(), invalid_symtab],
                1,
                invalid_symtab,
                &mut Vec::new()
            ),
            Err(RpxError::InvalidRplRecord {
                section_index: 1,
                reason: RplRecordError::EntrySize,
            })
        );
        let one_symbol = RawSection {
            section_type: SHT_SYMTAB,
            size: 16,
            entry_size: 16,
            ..RawSection::default()
        };
        assert_eq!(
            parse_symbols(
                &[0; 16],
                &[RawSection::default(), one_symbol],
                1,
                one_symbol,
                &mut Vec::new()
            ),
            Err(RpxError::InvalidRplRecord {
                section_index: 1,
                reason: RplRecordError::RecordCount,
            })
        );
        let invalid_rela = RawSection {
            section_type: SHT_RELA,
            entry_size: 8,
            ..RawSection::default()
        };
        assert_eq!(
            parse_relocations(
                &[],
                &[RawSection::default(), invalid_rela],
                1,
                invalid_rela,
                &mut Vec::new()
            ),
            Err(RpxError::InvalidRplRecord {
                section_index: 1,
                reason: RplRecordError::EntrySize,
            })
        );
    }

    #[test]
    fn rejects_unsupported_symbol_and_out_of_range_rpl_relocations() {
        let mut bytes = [0_u8; 12];
        write_fixture_u32(&mut bytes, 0, 0x2000);
        write_fixture_u32(&mut bytes, 4, 2);
        let sections = [
            RawSection::default(),
            RawSection {
                section_type: SHT_PROGBITS,
                flags: SHF_ALLOC,
                virtual_address: 0x1000,
                size: 4,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_SYMTAB,
                size: 32,
                entry_size: 16,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_RELA,
                size: 12,
                link: 2,
                info: 1,
                entry_size: 12,
                ..RawSection::default()
            },
        ];
        assert_eq!(
            parse_relocations(&bytes, &sections, 3, sections[3], &mut Vec::new()),
            Err(RpxError::InvalidRplRecord {
                section_index: 3,
                reason: RplRecordError::RelocationType
            })
        );
        write_fixture_u32(&mut bytes, 4, 1);
        assert_eq!(
            parse_relocations(&bytes, &sections, 3, sections[3], &mut Vec::new()),
            Err(RpxError::InvalidRplRecord {
                section_index: 3,
                reason: RplRecordError::SymbolIndex
            })
        );
        write_fixture_u32(&mut bytes, 4, (1 << 8) | 1);
        assert_eq!(
            parse_relocations(&bytes, &sections, 3, sections[3], &mut Vec::new()),
            Err(RpxError::InvalidRplRecord {
                section_index: 3,
                reason: RplRecordError::RelocationRange
            })
        );
    }

    fn relocation_fixture(
        offset: u32,
        relocation_type: u32,
        target_size: u32,
    ) -> ([u8; 12], [RawSection; 4]) {
        let mut bytes = [0_u8; 12];
        write_fixture_u32(&mut bytes, 0, offset);
        write_fixture_u32(&mut bytes, 4, (1 << 8) | relocation_type);
        let sections = [
            RawSection::default(),
            RawSection {
                section_type: SHT_PROGBITS,
                flags: SHF_ALLOC,
                virtual_address: 0x1000,
                size: target_size,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_SYMTAB,
                size: 32,
                entry_size: 16,
                ..RawSection::default()
            },
            RawSection {
                section_type: SHT_RELA,
                size: 12,
                link: 2,
                info: 1,
                entry_size: 12,
                ..RawSection::default()
            },
        ];
        (bytes, sections)
    }

    #[test]
    fn rel24_requires_an_aligned_site_after_range_validation() {
        let (bytes, sections) = relocation_fixture(0x1001, 10, 8);

        assert_eq!(
            parse_relocations(&bytes, &sections, 3, sections[3], &mut Vec::new()),
            Err(RpxError::InvalidRplRecord {
                section_index: 3,
                reason: RplRecordError::RelocationAlignment,
            })
        );
    }

    #[test]
    fn addr32_and_rel24_require_a_complete_four_byte_site() {
        for relocation_type in [1, 10] {
            for offset in [0x0ffc, 0x1001, 0x1004] {
                let (bytes, sections) = relocation_fixture(offset, relocation_type, 4);

                assert_eq!(
                    parse_relocations(&bytes, &sections, 3, sections[3], &mut Vec::new()),
                    Err(RpxError::InvalidRplRecord {
                        section_index: 3,
                        reason: RplRecordError::RelocationRange,
                    })
                );
            }
        }
    }
}
