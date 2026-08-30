//! Strict, resource-bounded parsing for the first supported Cafe RPX slice.
//!
//! The parser deliberately accepts only uncompressed ELF32 big-endian images
//! whose section kinds are needed by the initial Rust loader. Unsupported RPX
//! features fail closed instead of being partially interpreted.

use std::{fmt, mem::size_of};

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

/// Maximum complete RPX image accepted by [`parse_rpx`].
pub const MAX_RPX_IMAGE_SIZE: usize = 64 * 1024 * 1024;

/// A validated, owned Cafe RPX image description.
///
/// Construction is restricted to [`parse_rpx`], so callers can rely on the
/// invariants documented by the getters.
#[derive(Clone, Eq, PartialEq)]
pub struct ParsedRpx {
    entry_point: u32,
    sections: Vec<RpxSection>,
    file_info: RpxFileInfo,
}

impl fmt::Debug for ParsedRpx {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("ParsedRpx")
            .field("entry_point", &self.entry_point)
            .field("section_count", &self.sections.len())
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
    let name_section_index = read_u16(bytes, 50);
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

    let mut raw_sections = Vec::new();
    let section_count_usize = usize::from(section_count);
    raw_sections
        .try_reserve_exact(section_count_usize)
        .map_err(|_| RpxError::AllocationFailed {
            requested: section_count_usize
                .checked_mul(size_of::<RawSection>())
                .unwrap_or(MAX_RPX_IMAGE_SIZE),
        })?;
    let table_start =
        usize::try_from(section_table.start).map_err(|_| RpxError::InvalidSectionTable)?;
    for index in 0..section_count_usize {
        let entry_offset = index
            .checked_mul(usize::try_from(SECTION_HEADER_SIZE).expect("section size fits usize"))
            .and_then(|relative| table_start.checked_add(relative))
            .ok_or(RpxError::InvalidSectionTable)?;
        raw_sections.push(RawSection::from_bytes(bytes, entry_offset));
    }

    validate_sections(bytes, &raw_sections, elf_header, section_table)?;
    let name_table = validate_section_names(bytes, &raw_sections, usize::from(name_section_index))?;
    validate_terminal_sections(&raw_sections)?;

    let crc_index = raw_sections.len() - 2;
    let file_info_index = raw_sections.len() - 1;
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

    let file_info = parse_file_info(bytes, raw_sections[file_info_index])?;
    validate_mapping_regions(&raw_sections, &file_info)?;
    validate_crcs(bytes, &raw_sections, crc_index)?;
    validate_entry_point(entry_point, &raw_sections)?;
    own_sections(bytes, raw_sections, name_table, entry_point, file_info)
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

fn parse_file_info(bytes: &[u8], section: RawSection) -> Result<RpxFileInfo, RpxError> {
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
    validate_file_info_flags(file_info.flags)?;
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

const fn classify_mapping_region(section: RawSection) -> Option<RpxMappingRegion> {
    if section.size == 0 || section.flags & SHF_ALLOC == 0 {
        return None;
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
        if section.flags & (SHF_ALLOC | SHF_EXECINSTR) != (SHF_ALLOC | SHF_EXECINSTR) {
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

fn own_sections(
    bytes: &[u8],
    raw_sections: Vec<RawSection>,
    name_table: &[u8],
    entry_point: u32,
    file_info: RpxFileInfo,
) -> Result<ParsedRpx, RpxError> {
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
            data,
        });
    }
    Ok(ParsedRpx {
        entry_point,
        sections,
        file_info,
    })
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

    #[test]
    fn builtin_fixture_parses_without_mutating_input() {
        let image = builtin_rpx_fixture();
        let snapshot = image.clone();

        let parsed = parse_rpx(&image).expect("the bundled RPX fixture must parse");

        assert_eq!(parsed.entry_point(), 0x0200_0000);
        assert!(parsed.is_rpx());
        assert_eq!(parsed.sections().len(), 5);
        assert_eq!(image, snapshot);
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
}
