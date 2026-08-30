//! Deterministic ELF32 fixtures for imported data addressed with `ADDR16` pairs.
//!
//! The generated Cafe images contain no host data.  Each factory allocates a
//! fresh buffer, and writes its CRC table only after all section bytes are final.

const ELF_HEADER_SIZE: usize = 0x34;
const SECTION_TABLE_OFFSET: usize = 0x40;
const SECTION_HEADER_SIZE: usize = 0x28;
const FILE_INFO_SIZE: usize = 0x60;
const MAIN_SECTION_COUNT: usize = 10;
const PROVIDER_SECTION_COUNT: usize = 10;
const MAIN_IMAGE_SIZE: usize = 0x314;
const PROVIDER_IMAGE_SIZE: usize = 0x300;

const SHT_PROGBITS: u32 = 1;
const SHT_SYMTAB: u32 = 2;
const SHT_STRTAB: u32 = 3;
const SHT_RELA: u32 = 4;
const SHT_RPL_EXPORTS: u32 = 0x8000_0001;
const SHT_RPL_IMPORTS: u32 = 0x8000_0002;
const SHT_RPL_CRCS: u32 = 0x8000_0003;
const SHT_RPL_FILE_INFO: u32 = 0x8000_0004;
const SHF_WRITE: u32 = 1;
const SHF_ALLOC: u32 = 2;
const SHF_EXECINSTR: u32 = 4;
const R_PPC_ADDR32: u32 = 1;
const R_PPC_ADDR16_LO: u32 = 4;
const R_PPC_ADDR16_HA: u32 = 6;
const MAIN_TEXT_ADDRESS: u32 = 0x0200_0000;
const MAIN_DATA_ADDRESS: u32 = 0x1000_0000;
const MAIN_IMPORT_ADDRESS: u32 = 0xc000_0000;
const PROVIDER_TEXT_ADDRESS: u32 = 0x0200_0000;
const PROVIDER_DATA_ADDRESS: u32 = 0x1000_0000;
const PROVIDER_EXPORT_ADDRESS: u32 = 0xc000_0000;

const MAIN_TEXT: &[u8] = &[
    0x3c, 0x80, 0x00, 0x00, // lis r4, 0
    0x38, 0x84, 0x00, 0x00, // addi r4, r4, 0
    0x80, 0x64, 0x00, 0x00, // lwz r3, 0(r4)
    0x00, 0x00, 0x00, 0x00, // deterministic stop word
];
const PROVIDER_TEXT: &[u8] = &[0x00, 0x00, 0x00, 0x00];
const PROVIDER_DATA: &[u8] = &[0xc0, 0xde, 0xc0, 0xde, 0x00, 0x00, 0x00, 0x2a];
const MAIN_IMPORT: &[u8] = b"\0\0\0\0\0\0\0\0linkmod.rpl\0";
const SYMBOL_NAMES: &[u8] = b"\0answer\0";
const MAIN_SECTION_NAMES: &[u8] = b"\0.text\0.data\0.dimport_linkmod\0.symtab\0.strtab\0.rela.text\0.shstrtab\0.crcs\0.fileinfo\0";
const PROVIDER_SECTION_NAMES: &[u8] =
    b"\0.text\0.data\0.dexports\0.symtab\0.strtab\0.rela.dexports\0.shstrtab\0.crcs\0.fileinfo\0";

/// Failure to reserve storage for a generated imported-data fixture.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum RplDataFixtureError {
    /// The requested fixture buffer could not be allocated.
    AllocationFailed { requested: usize },
}

/// Return the deterministic main RPX that addresses imported data with `ADDR16`.
pub(crate) fn main_rpx_data_fixture() -> Result<Vec<u8>, RplDataFixtureError> {
    build_main_image()
}

/// Return the deterministic RPL provider that exports the imported data object.
pub(crate) fn provider_rpl_data_fixture() -> Result<Vec<u8>, RplDataFixtureError> {
    build_provider_image()
}

fn build_main_image() -> Result<Vec<u8>, RplDataFixtureError> {
    let text = section_table_end(MAIN_SECTION_COUNT);
    let data = text + MAIN_TEXT.len();
    let import = data + 4;
    let symtab = import + MAIN_IMPORT.len();
    let strtab = symtab + 32;
    let rela = align_up(strtab + SYMBOL_NAMES.len(), 4);
    let shstrtab = rela + 24;
    let crc = align_up(shstrtab + MAIN_SECTION_NAMES.len(), 4);
    let file_info = crc + (MAIN_SECTION_COUNT * 4);
    debug_assert_eq!(file_info + FILE_INFO_SIZE, MAIN_IMAGE_SIZE);
    let offsets = MainOffsets {
        text,
        data,
        import,
        symtab,
        strtab,
        rela,
        shstrtab,
        crc,
        file_info,
    };
    let sections = main_sections(&offsets);
    let mut image = empty_image(MAIN_IMAGE_SIZE)?;
    write_elf_header(&mut image, MAIN_SECTION_COUNT, 7, MAIN_TEXT_ADDRESS);
    write_sections(&mut image, &sections);
    image[text..text + MAIN_TEXT.len()].copy_from_slice(MAIN_TEXT);
    image[import..import + MAIN_IMPORT.len()].copy_from_slice(MAIN_IMPORT);
    write_symbol(
        &mut image,
        symtab + 16,
        Symbol::new(1, MAIN_IMPORT_ADDRESS + 8, 8, 0x11, 3),
    );
    image[strtab..strtab + SYMBOL_NAMES.len()].copy_from_slice(SYMBOL_NAMES);
    write_rela(
        &mut image,
        rela,
        Relocation::new(MAIN_TEXT_ADDRESS + 2, 1, R_PPC_ADDR16_HA, 4),
    );
    write_rela(
        &mut image,
        rela + 12,
        Relocation::new(MAIN_TEXT_ADDRESS + 6, 1, R_PPC_ADDR16_LO, 4),
    );
    image[shstrtab..shstrtab + MAIN_SECTION_NAMES.len()].copy_from_slice(MAIN_SECTION_NAMES);
    write_file_info(
        &mut image[file_info..file_info + FILE_INFO_SIZE],
        FileInfo::main(),
    );
    write_crc_table(&mut image, &sections, crc);
    Ok(image)
}

fn build_provider_image() -> Result<Vec<u8>, RplDataFixtureError> {
    let text = section_table_end(PROVIDER_SECTION_COUNT);
    let data = text + PROVIDER_TEXT.len();
    let export = data + PROVIDER_DATA.len();
    let symtab = align_up(export + 23, 4);
    let strtab = symtab + 32;
    let rela = align_up(strtab + SYMBOL_NAMES.len(), 4);
    let shstrtab = rela + 12;
    let crc = align_up(shstrtab + PROVIDER_SECTION_NAMES.len(), 4);
    let file_info = crc + (PROVIDER_SECTION_COUNT * 4);
    debug_assert_eq!(file_info + FILE_INFO_SIZE, PROVIDER_IMAGE_SIZE);
    let offsets = ProviderOffsets {
        text,
        data,
        export,
        symtab,
        strtab,
        rela,
        shstrtab,
        crc,
        file_info,
    };
    let sections = provider_sections(&offsets);
    let mut image = empty_image(PROVIDER_IMAGE_SIZE)?;
    write_elf_header(&mut image, PROVIDER_SECTION_COUNT, 7, 0);
    write_sections(&mut image, &sections);
    image[text..text + PROVIDER_TEXT.len()].copy_from_slice(PROVIDER_TEXT);
    image[data..data + PROVIDER_DATA.len()].copy_from_slice(PROVIDER_DATA);
    write_export_descriptor(&mut image, export);
    write_symbol(
        &mut image,
        symtab + 16,
        Symbol::new(1, PROVIDER_DATA_ADDRESS, 8, 0x11, 2),
    );
    image[strtab..strtab + SYMBOL_NAMES.len()].copy_from_slice(SYMBOL_NAMES);
    write_rela(
        &mut image,
        rela,
        Relocation::new(PROVIDER_EXPORT_ADDRESS + 8, 1, R_PPC_ADDR32, 0),
    );
    image[shstrtab..shstrtab + PROVIDER_SECTION_NAMES.len()]
        .copy_from_slice(PROVIDER_SECTION_NAMES);
    write_file_info(
        &mut image[file_info..file_info + FILE_INFO_SIZE],
        FileInfo::provider(),
    );
    write_crc_table(&mut image, &sections, crc);
    Ok(image)
}

fn empty_image(size: usize) -> Result<Vec<u8>, RplDataFixtureError> {
    let mut image = Vec::new();
    image
        .try_reserve_exact(size)
        .map_err(|_| RplDataFixtureError::AllocationFailed { requested: size })?;
    image.resize(size, 0);
    Ok(image)
}

fn section_table_end(count: usize) -> usize {
    SECTION_TABLE_OFFSET + (count * SECTION_HEADER_SIZE)
}

fn write_elf_header(image: &mut [u8], count: usize, names: usize, entry: u32) {
    image[..16].copy_from_slice(&[
        0x7f, b'E', b'L', b'F', 1, 2, 1, 0xca, 0xfe, 0, 0, 0, 0, 0, 0, 0,
    ]);
    write_u16(image, 16, 0xfe01);
    write_u16(image, 18, 20);
    write_u32(image, 20, 1);
    write_u32(image, 24, entry);
    write_u32(
        image,
        32,
        u32::try_from(SECTION_TABLE_OFFSET).expect("fixture section table offset fits ELF32"),
    );
    write_u16(
        image,
        40,
        u16::try_from(ELF_HEADER_SIZE).expect("fixture ELF header size fits u16"),
    );
    write_u16(
        image,
        46,
        u16::try_from(SECTION_HEADER_SIZE).expect("fixture section header size fits u16"),
    );
    write_u16(
        image,
        48,
        u16::try_from(count).expect("fixture section count fits u16"),
    );
    write_u16(
        image,
        50,
        u16::try_from(names).expect("fixture section name index fits u16"),
    );
}

#[derive(Clone, Copy)]
struct Section {
    template: SectionTemplate,
    offset: usize,
    size: usize,
}

impl Section {
    const NULL: Self = Self {
        template: SectionTemplate::NULL,
        offset: 0,
        size: 0,
    };

    const fn at(template: SectionTemplate, offset: usize, size: usize) -> Self {
        Self {
            template,
            offset,
            size,
        }
    }
}

#[derive(Clone, Copy)]
struct SectionTemplate {
    name: u32,
    kind: u32,
    flags: u32,
    address: u32,
    link: u32,
    info: u32,
    align: u32,
    entry_size: u32,
}

impl SectionTemplate {
    const NULL: Self = Self {
        name: 0,
        kind: 0,
        flags: 0,
        address: 0,
        link: 0,
        info: 0,
        align: 0,
        entry_size: 0,
    };

    const fn new(name: u32, kind: u32) -> Self {
        Self {
            name,
            kind,
            ..Self::NULL
        }
    }

    const fn in_memory(mut self, flags: u32, address: u32) -> Self {
        self.flags = flags;
        self.address = address;
        self
    }

    const fn linked(mut self, link: u32, info: u32) -> Self {
        self.link = link;
        self.info = info;
        self
    }

    const fn laid_out(mut self, align: u32, entry_size: u32) -> Self {
        self.align = align;
        self.entry_size = entry_size;
        self
    }
}

const MAIN_TEXT_SECTION: SectionTemplate = SectionTemplate::new(1, SHT_PROGBITS)
    .in_memory(SHF_ALLOC | SHF_EXECINSTR, MAIN_TEXT_ADDRESS)
    .laid_out(4, 0);
const MAIN_DATA_SECTION: SectionTemplate = SectionTemplate::new(7, SHT_PROGBITS)
    .in_memory(SHF_ALLOC | SHF_WRITE, MAIN_DATA_ADDRESS)
    .laid_out(4, 0);
const MAIN_IMPORT_SECTION: SectionTemplate = SectionTemplate::new(13, SHT_RPL_IMPORTS)
    .in_memory(SHF_ALLOC, MAIN_IMPORT_ADDRESS)
    .laid_out(4, 0);
const MAIN_SYMTAB_SECTION: SectionTemplate = SectionTemplate::new(30, SHT_SYMTAB)
    .linked(5, 1)
    .laid_out(4, 16);
const MAIN_STRTAB_SECTION: SectionTemplate = SectionTemplate::new(38, SHT_STRTAB).laid_out(1, 0);
const MAIN_RELA_SECTION: SectionTemplate = SectionTemplate::new(46, SHT_RELA)
    .linked(4, 1)
    .laid_out(4, 12);
const MAIN_SHSTRTAB_SECTION: SectionTemplate = SectionTemplate::new(57, SHT_STRTAB).laid_out(1, 0);
const MAIN_CRC_SECTION: SectionTemplate = SectionTemplate::new(67, SHT_RPL_CRCS).laid_out(4, 4);
const MAIN_FILE_INFO_SECTION: SectionTemplate =
    SectionTemplate::new(73, SHT_RPL_FILE_INFO).laid_out(4, 0);

const PROVIDER_TEXT_SECTION: SectionTemplate = SectionTemplate::new(1, SHT_PROGBITS)
    .in_memory(SHF_ALLOC | SHF_EXECINSTR, PROVIDER_TEXT_ADDRESS)
    .laid_out(4, 0);
const PROVIDER_DATA_SECTION: SectionTemplate = SectionTemplate::new(7, SHT_PROGBITS)
    .in_memory(SHF_ALLOC | SHF_WRITE, PROVIDER_DATA_ADDRESS)
    .laid_out(4, 0);
const PROVIDER_EXPORT_SECTION: SectionTemplate = SectionTemplate::new(13, SHT_RPL_EXPORTS)
    .in_memory(SHF_ALLOC, PROVIDER_EXPORT_ADDRESS)
    .laid_out(4, 0);
const PROVIDER_SYMTAB_SECTION: SectionTemplate = SectionTemplate::new(23, SHT_SYMTAB)
    .linked(5, 1)
    .laid_out(4, 16);
const PROVIDER_STRTAB_SECTION: SectionTemplate =
    SectionTemplate::new(31, SHT_STRTAB).laid_out(1, 0);
const PROVIDER_RELA_SECTION: SectionTemplate = SectionTemplate::new(39, SHT_RELA)
    .linked(4, 3)
    .laid_out(4, 12);
const PROVIDER_SHSTRTAB_SECTION: SectionTemplate =
    SectionTemplate::new(54, SHT_STRTAB).laid_out(1, 0);
const PROVIDER_CRC_SECTION: SectionTemplate = SectionTemplate::new(64, SHT_RPL_CRCS).laid_out(4, 4);
const PROVIDER_FILE_INFO_SECTION: SectionTemplate =
    SectionTemplate::new(70, SHT_RPL_FILE_INFO).laid_out(4, 0);

struct MainOffsets {
    text: usize,
    data: usize,
    import: usize,
    symtab: usize,
    strtab: usize,
    rela: usize,
    shstrtab: usize,
    crc: usize,
    file_info: usize,
}

fn main_sections(offsets: &MainOffsets) -> [Section; MAIN_SECTION_COUNT] {
    [
        Section::NULL,
        Section::at(MAIN_TEXT_SECTION, offsets.text, MAIN_TEXT.len()),
        Section::at(MAIN_DATA_SECTION, offsets.data, 4),
        Section::at(MAIN_IMPORT_SECTION, offsets.import, MAIN_IMPORT.len()),
        Section::at(MAIN_SYMTAB_SECTION, offsets.symtab, 32),
        Section::at(MAIN_STRTAB_SECTION, offsets.strtab, SYMBOL_NAMES.len()),
        Section::at(MAIN_RELA_SECTION, offsets.rela, 24),
        Section::at(
            MAIN_SHSTRTAB_SECTION,
            offsets.shstrtab,
            MAIN_SECTION_NAMES.len(),
        ),
        Section::at(MAIN_CRC_SECTION, offsets.crc, MAIN_SECTION_COUNT * 4),
        Section::at(MAIN_FILE_INFO_SECTION, offsets.file_info, FILE_INFO_SIZE),
    ]
}

struct ProviderOffsets {
    text: usize,
    data: usize,
    export: usize,
    symtab: usize,
    strtab: usize,
    rela: usize,
    shstrtab: usize,
    crc: usize,
    file_info: usize,
}

fn provider_sections(offsets: &ProviderOffsets) -> [Section; PROVIDER_SECTION_COUNT] {
    [
        Section::NULL,
        Section::at(PROVIDER_TEXT_SECTION, offsets.text, PROVIDER_TEXT.len()),
        Section::at(PROVIDER_DATA_SECTION, offsets.data, PROVIDER_DATA.len()),
        Section::at(PROVIDER_EXPORT_SECTION, offsets.export, 23),
        Section::at(PROVIDER_SYMTAB_SECTION, offsets.symtab, 32),
        Section::at(PROVIDER_STRTAB_SECTION, offsets.strtab, SYMBOL_NAMES.len()),
        Section::at(PROVIDER_RELA_SECTION, offsets.rela, 12),
        Section::at(
            PROVIDER_SHSTRTAB_SECTION,
            offsets.shstrtab,
            PROVIDER_SECTION_NAMES.len(),
        ),
        Section::at(
            PROVIDER_CRC_SECTION,
            offsets.crc,
            PROVIDER_SECTION_COUNT * 4,
        ),
        Section::at(
            PROVIDER_FILE_INFO_SECTION,
            offsets.file_info,
            FILE_INFO_SIZE,
        ),
    ]
}

fn write_sections(image: &mut [u8], sections: &[Section]) {
    for (index, section) in sections.iter().enumerate() {
        let offset = SECTION_TABLE_OFFSET + (index * SECTION_HEADER_SIZE);
        let template = section.template;
        write_u32(image, offset, template.name);
        write_u32(image, offset + 4, template.kind);
        write_u32(image, offset + 8, template.flags);
        write_u32(image, offset + 12, template.address);
        write_u32(
            image,
            offset + 16,
            u32::try_from(section.offset).expect("fixture section offset fits ELF32"),
        );
        write_u32(
            image,
            offset + 20,
            u32::try_from(section.size).expect("fixture section size fits ELF32"),
        );
        write_u32(image, offset + 24, template.link);
        write_u32(image, offset + 28, template.info);
        write_u32(image, offset + 32, template.align);
        write_u32(image, offset + 36, template.entry_size);
    }
}

#[derive(Clone, Copy)]
struct Symbol {
    name: u32,
    value: u32,
    size: u32,
    info: u8,
    section: u16,
}

impl Symbol {
    const fn new(name: u32, value: u32, size: u32, info: u8, section: u16) -> Self {
        Self {
            name,
            value,
            size,
            info,
            section,
        }
    }
}

fn write_symbol(bytes: &mut [u8], offset: usize, symbol: Symbol) {
    write_u32(bytes, offset, symbol.name);
    write_u32(bytes, offset + 4, symbol.value);
    write_u32(bytes, offset + 8, symbol.size);
    bytes[offset + 12] = symbol.info;
    write_u16(bytes, offset + 14, symbol.section);
}

#[derive(Clone, Copy)]
struct Relocation {
    target: u32,
    symbol: u32,
    kind: u32,
    addend: i32,
}

impl Relocation {
    const fn new(target: u32, symbol: u32, kind: u32, addend: i32) -> Self {
        Self {
            target,
            symbol,
            kind,
            addend,
        }
    }
}

fn write_rela(bytes: &mut [u8], offset: usize, relocation: Relocation) {
    write_u32(bytes, offset, relocation.target);
    write_u32(
        bytes,
        offset + 4,
        (relocation.symbol << 8) | relocation.kind,
    );
    write_u32(bytes, offset + 8, relocation.addend.cast_unsigned());
}

fn write_export_descriptor(bytes: &mut [u8], offset: usize) {
    write_u32(bytes, offset, 1);
    write_u32(bytes, offset + 4, 0);
    write_u32(bytes, offset + 8, 0);
    write_u32(bytes, offset + 12, 16);
    bytes[offset + 16..offset + 23].copy_from_slice(b"answer\0");
}

#[derive(Clone, Copy)]
struct FileInfo {
    text_region_size: u32,
    data_region_size: u32,
    flags: u32,
}

impl FileInfo {
    const fn main() -> Self {
        Self {
            text_region_size: 0x1000,
            data_region_size: 0x7000,
            flags: 2,
        }
    }

    const fn provider() -> Self {
        Self {
            text_region_size: 0x1000,
            data_region_size: 0x1000,
            flags: 0,
        }
    }
}

fn write_file_info(bytes: &mut [u8], info: FileInfo) {
    write_u32(bytes, 0, 0xcafe_0402);
    write_u32(bytes, 4, info.text_region_size);
    write_u32(bytes, 8, 0x1000);
    write_u32(bytes, 12, info.data_region_size);
    write_u32(bytes, 16, 0x1000);
    write_u32(bytes, 20, 0x1000);
    write_u32(bytes, 0x34, info.flags);
    write_u16(bytes, 0x58, u16::MAX);
}

fn write_crc_table(image: &mut [u8], sections: &[Section], crc: usize) {
    for (index, section) in sections.iter().enumerate() {
        let checksum = if matches!(section.template.kind, 0 | SHT_RPL_CRCS) {
            0
        } else {
            crc32_ieee(&image[section.offset..section.offset + section.size])
        };
        write_u32(image, crc + (index * 4), checksum);
    }
}

const fn align_up(value: usize, alignment: usize) -> usize {
    (value + (alignment - 1)) & !(alignment - 1)
}

fn write_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_be_bytes());
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
}

fn crc32_ieee(bytes: &[u8]) -> u32 {
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fixtures_are_equal_but_fresh_allocations() {
        assert_fresh_allocations(main_rpx_data_fixture);
        assert_fresh_allocations(provider_rpl_data_fixture);
    }

    #[test]
    fn main_fixture_has_the_addr16_imported_data_contract() {
        let image = main_rpx_data_fixture().expect("fixture allocation");
        assert_eq!(image.len(), MAIN_IMAGE_SIZE);
        assert_header(&image, MAIN_SECTION_COUNT, 7, MAIN_TEXT_ADDRESS);
        let text = section(&image, 1);
        let data = section(&image, 2);
        let import = section(&image, 3);
        let symbol = section(&image, 4);
        let relocation = section(&image, 6);
        assert_section(
            text,
            SHT_PROGBITS,
            SHF_ALLOC | SHF_EXECINSTR,
            MAIN_TEXT_ADDRESS,
            16,
        );
        assert_section(
            data,
            SHT_PROGBITS,
            SHF_ALLOC | SHF_WRITE,
            MAIN_DATA_ADDRESS,
            4,
        );
        assert_section(import, SHT_RPL_IMPORTS, SHF_ALLOC, MAIN_IMPORT_ADDRESS, 20);
        assert_eq!(payload(&image, text), MAIN_TEXT);
        assert_eq!(payload(&image, data), &[0, 0, 0, 0]);
        assert_eq!(payload(&image, import), MAIN_IMPORT);
        assert_symbol(
            &image,
            symbol.offset + 16,
            MAIN_IMPORT_ADDRESS + 8,
            8,
            0x11,
            3,
        );
        assert_eq!(
            (relocation.link, relocation.info, relocation.entry_size),
            (4, 1, 12)
        );
        assert_rela(
            &image,
            relocation.offset,
            MAIN_TEXT_ADDRESS + 2,
            R_PPC_ADDR16_HA,
            4,
        );
        assert_rela(
            &image,
            relocation.offset + 12,
            MAIN_TEXT_ADDRESS + 6,
            R_PPC_ADDR16_LO,
            4,
        );
        assert_eq!(
            &payload(&image, section(&image, 7))[46..57],
            b".rela.text\0"
        );
        assert_file_info(&image, section(&image, 9), 0x7000, 2);
        assert_crcs(&image);
    }

    #[test]
    fn provider_fixture_has_the_addr16_imported_data_contract() {
        let image = provider_rpl_data_fixture().expect("fixture allocation");
        assert_eq!(image.len(), PROVIDER_IMAGE_SIZE);
        assert_header(&image, PROVIDER_SECTION_COUNT, 7, 0);
        let text = section(&image, 1);
        let data = section(&image, 2);
        let export = section(&image, 3);
        let symbol = section(&image, 4);
        let relocation = section(&image, 6);
        assert_section(
            text,
            SHT_PROGBITS,
            SHF_ALLOC | SHF_EXECINSTR,
            PROVIDER_TEXT_ADDRESS,
            4,
        );
        assert_section(
            data,
            SHT_PROGBITS,
            SHF_ALLOC | SHF_WRITE,
            PROVIDER_DATA_ADDRESS,
            8,
        );
        assert_section(
            export,
            SHT_RPL_EXPORTS,
            SHF_ALLOC,
            PROVIDER_EXPORT_ADDRESS,
            23,
        );
        assert_eq!(payload(&image, text), PROVIDER_TEXT);
        assert_eq!(payload(&image, data), PROVIDER_DATA);
        assert_eq!(read_u32(&image, export.offset), 1);
        assert_eq!(read_u32(&image, export.offset + 4), 0);
        assert_eq!(read_u32(&image, export.offset + 8), 0);
        assert_eq!(read_u32(&image, export.offset + 12), 16);
        assert_eq!(&payload(&image, export)[16..], b"answer\0");
        assert_symbol(
            &image,
            symbol.offset + 16,
            PROVIDER_DATA_ADDRESS,
            8,
            0x11,
            2,
        );
        assert_rela(
            &image,
            relocation.offset,
            PROVIDER_EXPORT_ADDRESS + 8,
            R_PPC_ADDR32,
            0,
        );
        assert_file_info(&image, section(&image, 9), 0x1000, 0);
        assert_crcs(&image);
    }

    #[derive(Clone, Copy)]
    struct ReadSection {
        kind: u32,
        flags: u32,
        address: u32,
        offset: usize,
        size: usize,
        link: u32,
        info: u32,
        entry_size: u32,
    }

    fn section(image: &[u8], index: usize) -> ReadSection {
        let header = SECTION_TABLE_OFFSET + (index * SECTION_HEADER_SIZE);
        ReadSection {
            kind: read_u32(image, header + 4),
            flags: read_u32(image, header + 8),
            address: read_u32(image, header + 12),
            offset: usize::try_from(read_u32(image, header + 16))
                .expect("fixture section offset fits usize"),
            size: usize::try_from(read_u32(image, header + 20))
                .expect("fixture section size fits usize"),
            link: read_u32(image, header + 24),
            info: read_u32(image, header + 28),
            entry_size: read_u32(image, header + 36),
        }
    }

    fn payload(image: &[u8], section: ReadSection) -> &[u8] {
        &image[section.offset..section.offset + section.size]
    }

    fn assert_header(image: &[u8], count: usize, names: u16, entry: u32) {
        assert_eq!(&image[..9], &[0x7f, b'E', b'L', b'F', 1, 2, 1, 0xca, 0xfe]);
        assert_eq!(read_u16(image, 16), 0xfe01);
        assert_eq!(read_u16(image, 18), 20);
        assert_eq!(read_u32(image, 24), entry);
        assert_eq!(
            read_u16(image, 48),
            u16::try_from(count).expect("fixture section count fits u16")
        );
        assert_eq!(read_u16(image, 50), names);
    }

    fn assert_section(section: ReadSection, kind: u32, flags: u32, address: u32, size: usize) {
        assert_eq!(
            (section.kind, section.flags, section.address, section.size),
            (kind, flags, address, size)
        );
    }

    fn assert_symbol(image: &[u8], offset: usize, value: u32, size: u32, info: u8, section: u16) {
        assert_eq!(read_u32(image, offset), 1);
        assert_eq!(read_u32(image, offset + 4), value);
        assert_eq!(read_u32(image, offset + 8), size);
        assert_eq!(image[offset + 12], info);
        assert_eq!(read_u16(image, offset + 14), section);
    }

    fn assert_rela(image: &[u8], offset: usize, target: u32, kind: u32, addend: i32) {
        assert_eq!(read_u32(image, offset), target);
        assert_eq!(read_u32(image, offset + 4), (1 << 8) | kind);
        assert_eq!(read_u32(image, offset + 8), addend.cast_unsigned());
    }

    fn assert_file_info(image: &[u8], section: ReadSection, data_size: u32, flags: u32) {
        assert_eq!(read_u32(image, section.offset), 0xcafe_0402);
        assert_eq!(read_u32(image, section.offset + 4), 0x1000);
        assert_eq!(read_u32(image, section.offset + 8), 0x1000);
        assert_eq!(read_u32(image, section.offset + 12), data_size);
        assert_eq!(read_u32(image, section.offset + 16), 0x1000);
        assert_eq!(read_u32(image, section.offset + 20), 0x1000);
        assert_eq!(read_u32(image, section.offset + 0x34), flags);
    }

    fn assert_crcs(image: &[u8]) {
        let count = usize::from(read_u16(image, 48));
        let crc = section(image, count - 2);
        for index in 0..count {
            let checked = section(image, index);
            let expected = if matches!(checked.kind, 0 | SHT_RPL_CRCS) {
                0
            } else {
                crc32_ieee(payload(image, checked))
            };
            assert_eq!(read_u32(image, crc.offset + (index * 4)), expected);
        }
    }

    fn assert_fresh_allocations(build: fn() -> Result<Vec<u8>, RplDataFixtureError>) {
        let first = build().expect("fixture allocation");
        let second = build().expect("fixture allocation");
        assert_eq!(first, second);
        assert_ne!(first.as_ptr(), second.as_ptr());
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
}
