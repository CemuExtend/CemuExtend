//! Deterministic ELF32 fixtures for the minimal RPX-to-RPL link slice.
//!
//! These images deliberately contain only the import/export and `R_PPC_ADDR32`
//! relocation needed to link `answer`.  They are source-generated so no host
//! linker, path, timestamp, or build output affects their bytes.

const ELF_HEADER_SIZE: usize = 0x34;
const SECTION_TABLE_OFFSET: usize = 0x40;
const SECTION_HEADER_SIZE: usize = 0x28;
const FILE_INFO_SIZE: usize = 0x60;

const SHT_PROGBITS: u32 = 1;
const SHT_SYMTAB: u32 = 2;
const SHT_STRTAB: u32 = 3;
const SHT_RELA: u32 = 4;
const SHT_RPL_EXPORTS: u32 = 0x8000_0001;
const SHT_RPL_IMPORTS: u32 = 0x8000_0002;
const SHT_RPL_CRCS: u32 = 0x8000_0003;
const SHT_RPL_FILE_INFO: u32 = 0x8000_0004;

const SHF_WRITE: u32 = 0x1;
const SHF_ALLOC: u32 = 0x2;
const SHF_EXECINSTR: u32 = 0x4;
const R_PPC_ADDR32: u32 = 1;

const MAIN_TEXT_ADDRESS: u32 = 0x0200_0000;
const MAIN_DATA_ADDRESS: u32 = 0x1000_0000;
const MAIN_IMPORT_ADDRESS: u32 = 0xc000_0000;
const PROVIDER_TEXT_ADDRESS: u32 = 0x0200_0000;
const PROVIDER_EXPORT_ADDRESS: u32 = 0xc000_0000;

const MAIN_SECTION_COUNT: usize = 10;
const PROVIDER_SECTION_COUNT: usize = 9;
const MAIN_IMAGE_SIZE: usize = 0x304;
const PROVIDER_IMAGE_SIZE: usize = 0x2cc;

const MAIN_TEXT: &[u8] = &[
    0x38, 0x60, 0x00, 0x28, // addi r3, r0, 40
    0x38, 0x63, 0x00, 0x02, // addi r3, r3, 2
    0x00, 0x00, 0x00, 0x00, // deterministic stop word
];
const PROVIDER_TEXT: &[u8] = &[
    0x38, 0x60, 0x00, 0x2a, // addi r3, r0, 42
    0x4e, 0x80, 0x00, 0x20, // blr
];
const MAIN_IMPORT: &[u8] = b"\0\0\0\0\0\0\0\0linkmod.rpl\0";
const SYMBOL_NAMES: &[u8] = b"\0answer\0";
const MAIN_SECTION_NAMES: &[u8] = b"\0.text\0.data\0.fimport_linkmod\0.symtab\0.strtab\0.rela.data\0.shstrtab\0.crcs\0.fileinfo\0";
const PROVIDER_SECTION_NAMES: &[u8] =
    b"\0.text\0.fexports\0.symtab\0.strtab\0.rela.fexports\0.shstrtab\0.crcs\0.fileinfo\0";

/// Failure to reserve storage for a generated fixture.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum RplLinkFixtureError {
    /// The requested fixture buffer could not be allocated.
    AllocationFailed { requested: usize },
}

/// Return the deterministic main RPX which imports `answer` from `linkmod.rpl`.
pub(crate) fn main_rpx_link_fixture() -> Result<Vec<u8>, RplLinkFixtureError> {
    build_main_image()
}

/// Return the deterministic provider RPL which exports `answer`.
pub(crate) fn provider_rpl_link_fixture() -> Result<Vec<u8>, RplLinkFixtureError> {
    build_provider_image()
}

fn build_main_image() -> Result<Vec<u8>, RplLinkFixtureError> {
    let section_table_end = SECTION_TABLE_OFFSET + (MAIN_SECTION_COUNT * SECTION_HEADER_SIZE);
    let text_offset = section_table_end;
    let data_offset = text_offset + MAIN_TEXT.len();
    let import_offset = data_offset + 4;
    let symtab_offset = import_offset + MAIN_IMPORT.len();
    let strtab_offset = symtab_offset + 32;
    let rela_offset = align_up(strtab_offset + SYMBOL_NAMES.len(), 4);
    let shstrtab_offset = rela_offset + 12;
    let crc_offset = align_up(shstrtab_offset + MAIN_SECTION_NAMES.len(), 4);
    let file_info_offset = crc_offset + (MAIN_SECTION_COUNT * 4);
    debug_assert_eq!(file_info_offset + FILE_INFO_SIZE, MAIN_IMAGE_SIZE);

    let mut image = empty_image(MAIN_IMAGE_SIZE)?;
    write_elf_header(&mut image, MAIN_SECTION_COUNT, 7, MAIN_TEXT_ADDRESS);
    let offsets = MainOffsets {
        text: text_offset,
        data: data_offset,
        import: import_offset,
        symtab: symtab_offset,
        strtab: strtab_offset,
        rela: rela_offset,
        shstrtab: shstrtab_offset,
        crc: crc_offset,
        file_info: file_info_offset,
    };
    let sections = main_sections(&offsets);
    write_sections(&mut image, &sections);
    image[text_offset..text_offset + MAIN_TEXT.len()].copy_from_slice(MAIN_TEXT);
    image[import_offset..import_offset + MAIN_IMPORT.len()].copy_from_slice(MAIN_IMPORT);
    write_symbol(
        &mut image,
        symtab_offset + 16,
        1,
        MAIN_IMPORT_ADDRESS + 8,
        0x12,
        0,
        3,
    );
    image[strtab_offset..strtab_offset + SYMBOL_NAMES.len()].copy_from_slice(SYMBOL_NAMES);
    write_rela(
        &mut image,
        rela_offset,
        MAIN_DATA_ADDRESS,
        1,
        R_PPC_ADDR32,
        0,
    );
    image[shstrtab_offset..shstrtab_offset + MAIN_SECTION_NAMES.len()]
        .copy_from_slice(MAIN_SECTION_NAMES);
    write_file_info(
        &mut image[file_info_offset..file_info_offset + FILE_INFO_SIZE],
        2,
        0x1000,
    );
    write_crc_table(&mut image, &sections, crc_offset);
    Ok(image)
}

fn build_provider_image() -> Result<Vec<u8>, RplLinkFixtureError> {
    let section_table_end = SECTION_TABLE_OFFSET + (PROVIDER_SECTION_COUNT * SECTION_HEADER_SIZE);
    let text_offset = section_table_end;
    let export_offset = text_offset + PROVIDER_TEXT.len();
    let symtab_offset = align_up(export_offset + 23, 4);
    let strtab_offset = symtab_offset + 32;
    let rela_offset = align_up(strtab_offset + SYMBOL_NAMES.len(), 4);
    let shstrtab_offset = rela_offset + 12;
    let crc_offset = align_up(shstrtab_offset + PROVIDER_SECTION_NAMES.len(), 4);
    let file_info_offset = crc_offset + (PROVIDER_SECTION_COUNT * 4);
    debug_assert_eq!(file_info_offset + FILE_INFO_SIZE, PROVIDER_IMAGE_SIZE);

    let mut image = empty_image(PROVIDER_IMAGE_SIZE)?;
    write_elf_header(&mut image, PROVIDER_SECTION_COUNT, 6, 0);
    let offsets = ProviderOffsets {
        text: text_offset,
        export: export_offset,
        symtab: symtab_offset,
        strtab: strtab_offset,
        rela: rela_offset,
        shstrtab: shstrtab_offset,
        crc: crc_offset,
        file_info: file_info_offset,
    };
    let sections = provider_sections(&offsets);
    write_sections(&mut image, &sections);
    image[text_offset..text_offset + PROVIDER_TEXT.len()].copy_from_slice(PROVIDER_TEXT);
    write_u32(&mut image, export_offset, 1);
    write_u32(&mut image, export_offset + 12, 16);
    image[export_offset + 16..export_offset + 23].copy_from_slice(b"answer\0");
    write_symbol(
        &mut image,
        symtab_offset + 16,
        1,
        PROVIDER_TEXT_ADDRESS,
        0x12,
        0,
        1,
    );
    image[strtab_offset..strtab_offset + SYMBOL_NAMES.len()].copy_from_slice(SYMBOL_NAMES);
    write_rela(
        &mut image,
        rela_offset,
        PROVIDER_EXPORT_ADDRESS + 8,
        1,
        R_PPC_ADDR32,
        0,
    );
    image[shstrtab_offset..shstrtab_offset + PROVIDER_SECTION_NAMES.len()]
        .copy_from_slice(PROVIDER_SECTION_NAMES);
    write_file_info(
        &mut image[file_info_offset..file_info_offset + FILE_INFO_SIZE],
        0,
        0,
    );
    write_crc_table(&mut image, &sections, crc_offset);
    Ok(image)
}

fn empty_image(size: usize) -> Result<Vec<u8>, RplLinkFixtureError> {
    let mut image = Vec::new();
    image
        .try_reserve_exact(size)
        .map_err(|_| RplLinkFixtureError::AllocationFailed { requested: size })?;
    image.resize(size, 0);
    Ok(image)
}

fn write_elf_header(image: &mut [u8], section_count: usize, name_index: usize, entry: u32) {
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
        u32::try_from(SECTION_TABLE_OFFSET).expect("section table fits ELF32"),
    );
    write_u16(
        image,
        40,
        u16::try_from(ELF_HEADER_SIZE).expect("ELF header fits u16"),
    );
    write_u16(
        image,
        46,
        u16::try_from(SECTION_HEADER_SIZE).expect("section header fits u16"),
    );
    write_u16(
        image,
        48,
        u16::try_from(section_count).expect("section count fits u16"),
    );
    write_u16(
        image,
        50,
        u16::try_from(name_index).expect("section name index fits u16"),
    );
}

#[derive(Clone, Copy)]
struct Section {
    name: u32,
    kind: u32,
    flags: u32,
    address: u32,
    offset: usize,
    size: usize,
    link: u32,
    info: u32,
    align: u32,
    entry_size: u32,
}

impl Section {
    const NULL: Self = Self {
        name: 0,
        kind: 0,
        flags: 0,
        address: 0,
        offset: 0,
        size: 0,
        link: 0,
        info: 0,
        align: 0,
        entry_size: 0,
    };

    const fn at(template: SectionTemplate, offset: usize, size: usize) -> Self {
        Self {
            name: template.name,
            kind: template.kind,
            flags: template.flags,
            address: template.address,
            offset,
            size,
            link: template.link,
            info: template.info,
            align: template.align,
            entry_size: template.entry_size,
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

const MAIN_TEXT_SECTION: SectionTemplate = SectionTemplate {
    name: 1,
    kind: SHT_PROGBITS,
    flags: SHF_ALLOC | SHF_EXECINSTR,
    address: MAIN_TEXT_ADDRESS,
    link: 0,
    info: 0,
    align: 4,
    entry_size: 0,
};
const MAIN_DATA_SECTION: SectionTemplate = SectionTemplate {
    name: 7,
    kind: SHT_PROGBITS,
    flags: SHF_ALLOC | SHF_WRITE,
    address: MAIN_DATA_ADDRESS,
    link: 0,
    info: 0,
    align: 4,
    entry_size: 0,
};
const MAIN_IMPORT_SECTION: SectionTemplate = SectionTemplate {
    name: 13,
    kind: SHT_RPL_IMPORTS,
    flags: SHF_ALLOC | SHF_EXECINSTR,
    address: MAIN_IMPORT_ADDRESS,
    link: 0,
    info: 0,
    align: 4,
    entry_size: 0,
};
const MAIN_SYMTAB_SECTION: SectionTemplate = SectionTemplate {
    name: 30,
    kind: SHT_SYMTAB,
    flags: 0,
    address: 0,
    link: 5,
    info: 1,
    align: 4,
    entry_size: 16,
};
const MAIN_STRTAB_SECTION: SectionTemplate = SectionTemplate {
    name: 38,
    kind: SHT_STRTAB,
    flags: 0,
    address: 0,
    link: 0,
    info: 0,
    align: 1,
    entry_size: 0,
};
const MAIN_RELA_SECTION: SectionTemplate = SectionTemplate {
    name: 46,
    kind: SHT_RELA,
    flags: 0,
    address: 0,
    link: 4,
    info: 2,
    align: 4,
    entry_size: 12,
};
const MAIN_SHSTRTAB_SECTION: SectionTemplate = SectionTemplate {
    name: 57,
    kind: SHT_STRTAB,
    flags: 0,
    address: 0,
    link: 0,
    info: 0,
    align: 1,
    entry_size: 0,
};
const MAIN_CRC_SECTION: SectionTemplate = SectionTemplate {
    name: 67,
    kind: SHT_RPL_CRCS,
    flags: 0,
    address: 0,
    link: 0,
    info: 0,
    align: 4,
    entry_size: 4,
};
const MAIN_FILE_INFO_SECTION: SectionTemplate = SectionTemplate {
    name: 73,
    kind: SHT_RPL_FILE_INFO,
    flags: 0,
    address: 0,
    link: 0,
    info: 0,
    align: 4,
    entry_size: 0,
};

const PROVIDER_TEXT_SECTION: SectionTemplate = SectionTemplate {
    name: 1,
    kind: SHT_PROGBITS,
    flags: SHF_ALLOC | SHF_EXECINSTR,
    address: PROVIDER_TEXT_ADDRESS,
    link: 0,
    info: 0,
    align: 4,
    entry_size: 0,
};
const PROVIDER_EXPORT_SECTION: SectionTemplate = SectionTemplate {
    name: 7,
    kind: SHT_RPL_EXPORTS,
    flags: SHF_ALLOC | SHF_EXECINSTR,
    address: PROVIDER_EXPORT_ADDRESS,
    link: 0,
    info: 0,
    align: 4,
    entry_size: 0,
};
const PROVIDER_SYMTAB_SECTION: SectionTemplate = SectionTemplate {
    name: 17,
    kind: SHT_SYMTAB,
    flags: 0,
    address: 0,
    link: 4,
    info: 1,
    align: 4,
    entry_size: 16,
};
const PROVIDER_STRTAB_SECTION: SectionTemplate = SectionTemplate {
    name: 25,
    kind: SHT_STRTAB,
    flags: 0,
    address: 0,
    link: 0,
    info: 0,
    align: 1,
    entry_size: 0,
};
const PROVIDER_RELA_SECTION: SectionTemplate = SectionTemplate {
    name: 33,
    kind: SHT_RELA,
    flags: 0,
    address: 0,
    link: 3,
    info: 2,
    align: 4,
    entry_size: 12,
};
const PROVIDER_SHSTRTAB_SECTION: SectionTemplate = SectionTemplate {
    name: 49,
    kind: SHT_STRTAB,
    flags: 0,
    address: 0,
    link: 0,
    info: 0,
    align: 1,
    entry_size: 0,
};
const PROVIDER_CRC_SECTION: SectionTemplate = SectionTemplate {
    name: 59,
    kind: SHT_RPL_CRCS,
    flags: 0,
    address: 0,
    link: 0,
    info: 0,
    align: 4,
    entry_size: 4,
};
const PROVIDER_FILE_INFO_SECTION: SectionTemplate = SectionTemplate {
    name: 65,
    kind: SHT_RPL_FILE_INFO,
    flags: 0,
    address: 0,
    link: 0,
    info: 0,
    align: 4,
    entry_size: 0,
};

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
        Section::at(MAIN_RELA_SECTION, offsets.rela, 12),
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
        write_u32(image, offset, section.name);
        write_u32(image, offset + 4, section.kind);
        write_u32(image, offset + 8, section.flags);
        write_u32(image, offset + 12, section.address);
        write_u32(
            image,
            offset + 16,
            u32::try_from(section.offset).expect("fixture offset fits ELF32"),
        );
        write_u32(
            image,
            offset + 20,
            u32::try_from(section.size).expect("fixture size fits ELF32"),
        );
        write_u32(image, offset + 24, section.link);
        write_u32(image, offset + 28, section.info);
        write_u32(image, offset + 32, section.align);
        write_u32(image, offset + 36, section.entry_size);
    }
}

fn write_symbol(
    bytes: &mut [u8],
    offset: usize,
    name: u32,
    value: u32,
    info: u8,
    other: u8,
    section: u16,
) {
    write_u32(bytes, offset, name);
    write_u32(bytes, offset + 4, value);
    write_u32(bytes, offset + 8, 0);
    bytes[offset + 12] = info;
    bytes[offset + 13] = other;
    write_u16(bytes, offset + 14, section);
}

fn write_rela(
    bytes: &mut [u8],
    offset: usize,
    target: u32,
    symbol: u32,
    relocation: u32,
    addend: u32,
) {
    write_u32(bytes, offset, target);
    write_u32(bytes, offset + 4, (symbol << 8) | relocation);
    write_u32(bytes, offset + 8, addend);
}

fn write_file_info(bytes: &mut [u8], flags: u32, data_region_size: u32) {
    write_u32(bytes, 0x00, 0xcafe_0402);
    write_u32(bytes, 0x04, 0x1000);
    write_u32(bytes, 0x08, 0x1000);
    write_u32(bytes, 0x0c, data_region_size);
    write_u32(bytes, 0x10, 0x1000);
    write_u32(bytes, 0x14, 0x1000);
    write_u32(bytes, 0x34, flags);
    write_u16(bytes, 0x58, u16::MAX);
}

fn write_crc_table(image: &mut [u8], sections: &[Section], crc_offset: usize) {
    for (index, section) in sections.iter().enumerate() {
        let crc = if matches!(section.kind, 0 | SHT_RPL_CRCS) {
            0
        } else {
            crc32_ieee(&image[section.offset..section.offset + section.size])
        };
        write_u32(image, crc_offset + (index * 4), crc);
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
    fn fixtures_are_deterministic_without_a_hash_fixture() {
        assert_eq!(
            main_rpx_link_fixture().unwrap(),
            main_rpx_link_fixture().unwrap()
        );
        assert_eq!(
            provider_rpl_link_fixture().unwrap(),
            provider_rpl_link_fixture().unwrap()
        );
    }

    #[test]
    fn main_fixture_has_the_approved_link_layout() {
        let image = main_rpx_link_fixture().unwrap();
        assert_eq!(image.len(), MAIN_IMAGE_SIZE);
        assert_layout(&image, MAIN_SECTION_COUNT, 7, 0x1000);
        assert_eq!(read_u32(&image, 24), MAIN_TEXT_ADDRESS);
        assert_eq!(section(&image, 1).address, MAIN_TEXT_ADDRESS);
        assert_eq!(section(&image, 2).address, MAIN_DATA_ADDRESS);
        assert_eq!(section(&image, 3).address, MAIN_IMPORT_ADDRESS);
        assert_eq!(section(&image, 3).flags, SHF_ALLOC | SHF_EXECINSTR);
        assert_eq!(section(&image, 3).link, 0);
        assert_eq!(section(&image, 3).info, 0);
        assert_eq!(section(&image, 3).entry_size, 0);
        assert_eq!(payload(&image, section(&image, 1)), MAIN_TEXT);
        assert_eq!(&payload(&image, section(&image, 3))[8..], b"linkmod.rpl\0");
        assert_eq!(
            read_u32(&image, section(&image, 4).offset + 20),
            MAIN_IMPORT_ADDRESS + 8
        );
        assert_eq!(read_u16(&image, section(&image, 4).offset + 30), 3);
        assert_eq!(
            read_u32(&image, section(&image, 6).offset),
            MAIN_DATA_ADDRESS
        );
        assert_eq!(read_u32(&image, section(&image, 6).offset + 4), 0x101);
        assert_eq!(read_u32(&image, section(&image, 9).offset + 0x0c), 0x1000);
        assert_eq!(read_u32(&image, section(&image, 9).offset + 0x34), 2);
    }

    #[test]
    fn provider_fixture_has_the_approved_link_layout() {
        let image = provider_rpl_link_fixture().unwrap();
        assert_eq!(image.len(), PROVIDER_IMAGE_SIZE);
        assert_layout(&image, PROVIDER_SECTION_COUNT, 6, 0);
        assert_eq!(read_u32(&image, 24), 0);
        assert_eq!(section(&image, 1).address, PROVIDER_TEXT_ADDRESS);
        assert_eq!(section(&image, 2).address, PROVIDER_EXPORT_ADDRESS);
        assert_eq!(section(&image, 2).flags, SHF_ALLOC | SHF_EXECINSTR);
        assert_eq!(section(&image, 2).link, 0);
        assert_eq!(section(&image, 2).info, 0);
        assert_eq!(section(&image, 2).entry_size, 0);
        assert_eq!(payload(&image, section(&image, 1)), PROVIDER_TEXT);
        assert_eq!(read_u32(&image, section(&image, 2).offset), 1);
        assert_eq!(section(&image, 2).size, 23);
        let export_payload = payload(&image, section(&image, 2));
        let export_name_offset = read_u32(export_payload, 12) as usize;
        assert_eq!(export_name_offset, 16);
        assert_eq!(&export_payload[export_name_offset..], b"answer\0");
        assert_eq!(read_u32(&image, section(&image, 3).offset + 16), 1);
        assert_eq!(payload(&image, section(&image, 4)), SYMBOL_NAMES);
        assert_eq!(
            read_u32(&image, section(&image, 3).offset + 20),
            PROVIDER_TEXT_ADDRESS
        );
        assert_eq!(image[section(&image, 3).offset + 28], 0x12);
        assert_eq!(
            read_u32(&image, section(&image, 5).offset),
            PROVIDER_EXPORT_ADDRESS + 8
        );
        assert_eq!(read_u32(&image, section(&image, 5).offset + 4), 0x101);
        assert_eq!(read_u32(&image, section(&image, 8).offset + 0x0c), 0);
        assert_eq!(read_u32(&image, section(&image, 8).offset + 0x34), 0);
    }

    #[test]
    fn crc_tables_cover_raw_non_crc_section_bytes() {
        for image in [
            main_rpx_link_fixture().unwrap(),
            provider_rpl_link_fixture().unwrap(),
        ] {
            let count = usize::from(read_u16(&image, 48));
            let crc = section(&image, count - 2);
            for index in 0..count {
                let current = section(&image, index);
                let expected = if matches!(current.kind, 0 | SHT_RPL_CRCS) {
                    0
                } else {
                    crc32_ieee(payload(&image, current))
                };
                assert_eq!(read_u32(&image, crc.offset + (index * 4)), expected);
            }
        }
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

    fn assert_layout(
        image: &[u8],
        expected_count: usize,
        name_index: u16,
        expected_data_region_size: u32,
    ) {
        assert_eq!(&image[..9], &[0x7f, b'E', b'L', b'F', 1, 2, 1, 0xca, 0xfe]);
        assert_eq!(read_u16(image, 16), 0xfe01);
        assert_eq!(read_u16(image, 18), 20);
        assert_eq!(
            read_u16(image, 48),
            u16::try_from(expected_count).expect("expected count fits u16")
        );
        assert_eq!(read_u16(image, 50), name_index);
        let table_end = SECTION_TABLE_OFFSET + (expected_count * SECTION_HEADER_SIZE);
        let mut payloads = Vec::new();
        for index in 1..expected_count {
            let current = section(image, index);
            assert_eq!(current.offset % 4, 0);
            assert!(current.offset >= table_end);
            assert!(current.offset + current.size <= image.len());
            for &(begin, end) in &payloads {
                assert!(current.offset >= end || current.offset + current.size <= begin);
            }
            payloads.push((current.offset, current.offset + current.size));
        }
        let file_info = section(image, expected_count - 1);
        assert_eq!(read_u32(image, file_info.offset + 4), 0x1000);
        assert_eq!(read_u32(image, file_info.offset + 8), 0x1000);
        assert_eq!(
            read_u32(image, file_info.offset + 12),
            expected_data_region_size
        );
        assert_eq!(read_u32(image, file_info.offset + 16), 0x1000);
        assert_eq!(read_u32(image, file_info.offset + 20), 0x1000);
    }

    fn section(image: &[u8], index: usize) -> ReadSection {
        let offset = SECTION_TABLE_OFFSET + (index * SECTION_HEADER_SIZE);
        ReadSection {
            kind: read_u32(image, offset + 4),
            flags: read_u32(image, offset + 8),
            address: read_u32(image, offset + 12),
            offset: read_u32(image, offset + 16) as usize,
            size: read_u32(image, offset + 20) as usize,
            link: read_u32(image, offset + 24),
            info: read_u32(image, offset + 28),
            entry_size: read_u32(image, offset + 36),
        }
    }

    fn payload(image: &[u8], section: ReadSection) -> &[u8] {
        &image[section.offset..section.offset + section.size]
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
