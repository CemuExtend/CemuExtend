//! Deterministic ELF32 fixtures for the minimal RPX-to-RPL `REL24` call path.
//!
//! The source-generated images are bounded, contain no host-derived data, and
//! return fresh owned bytes on every call.

const ELF_HEADER_SIZE: usize = 0x34;
const SECTION_TABLE_OFFSET: usize = 0x40;
const SECTION_HEADER_SIZE: usize = 0x28;
const FILE_INFO_SIZE: usize = 0x60;
const MAIN_SECTION_COUNT: usize = 10;
const PROVIDER_SECTION_COUNT: usize = 9;
const MAIN_IMAGE_SIZE: usize = 0x304;
const PROVIDER_IMAGE_SIZE: usize = 0x2cc;

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
const R_PPC_REL24: u32 = 10;
const MAIN_TEXT_ADDRESS: u32 = 0x0200_0000;
const MAIN_DATA_ADDRESS: u32 = 0x1000_0000;
const MAIN_IMPORT_ADDRESS: u32 = 0xc000_0000;
const PROVIDER_TEXT_ADDRESS: u32 = 0x0200_0000;
const PROVIDER_EXPORT_ADDRESS: u32 = 0xc000_0000;
const MAIN_TEXT: &[u8] = &[
    0x48, 0x00, 0x00, 0x01, // bl 0 preimage
    0x00, 0x00, 0x00, 0x00, // deterministic stop word
    0x00, 0x00, 0x00, 0x00, // deterministic guard word
];
const PROVIDER_TEXT: &[u8] = &[0x38, 0x60, 0x00, 0x2a, 0x4e, 0x80, 0x00, 0x20];
const MAIN_IMPORT: &[u8] = b"\0\0\0\0\0\0\0\0linkmod.rpl\0";
const SYMBOL_NAMES: &[u8] = b"\0answer\0";
const MAIN_SECTION_NAMES: &[u8] = b"\0.text\0.data\0.fimport_linkmod\0.symtab\0.strtab\0.rela.text\0.shstrtab\0.crcs\0.fileinfo\0";
const PROVIDER_SECTION_NAMES: &[u8] =
    b"\0.text\0.fexports\0.symtab\0.strtab\0.rela.fexports\0.shstrtab\0.crcs\0.fileinfo\0";

/// Failure to reserve storage for a generated call fixture.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum RplCallFixtureError {
    /// The requested fixture buffer could not be allocated.
    AllocationFailed { requested: usize },
}

/// Return the deterministic main RPX that calls imported `answer` with `REL24`.
pub(crate) fn main_rpx_call_fixture() -> Result<Vec<u8>, RplCallFixtureError> {
    build_main_image()
}

/// Return the deterministic RPL provider that exports `answer`.
pub(crate) fn provider_rpl_call_fixture() -> Result<Vec<u8>, RplCallFixtureError> {
    build_provider_image()
}

fn build_main_image() -> Result<Vec<u8>, RplCallFixtureError> {
    let text = SECTION_TABLE_OFFSET + (MAIN_SECTION_COUNT * SECTION_HEADER_SIZE);
    let data = text + MAIN_TEXT.len();
    let import = data + 4;
    let symtab = import + MAIN_IMPORT.len();
    let strtab = symtab + 32;
    let rela = align_up(strtab + SYMBOL_NAMES.len(), 4);
    let shstrtab = rela + 12;
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
    write_symbol(&mut image, symtab + 16, 1, MAIN_IMPORT_ADDRESS + 8, 0x12, 3);
    image[strtab..strtab + SYMBOL_NAMES.len()].copy_from_slice(SYMBOL_NAMES);
    write_rela(&mut image, rela, MAIN_TEXT_ADDRESS, 1, R_PPC_REL24);
    image[shstrtab..shstrtab + MAIN_SECTION_NAMES.len()].copy_from_slice(MAIN_SECTION_NAMES);
    write_file_info(&mut image[file_info..file_info + FILE_INFO_SIZE], 2, 0x1000);
    write_crc_table(&mut image, &sections, crc);
    Ok(image)
}

fn build_provider_image() -> Result<Vec<u8>, RplCallFixtureError> {
    let text = SECTION_TABLE_OFFSET + (PROVIDER_SECTION_COUNT * SECTION_HEADER_SIZE);
    let export = text + PROVIDER_TEXT.len();
    let symtab = align_up(export + 23, 4);
    let strtab = symtab + 32;
    let rela = align_up(strtab + SYMBOL_NAMES.len(), 4);
    let shstrtab = rela + 12;
    let crc = align_up(shstrtab + PROVIDER_SECTION_NAMES.len(), 4);
    let file_info = crc + (PROVIDER_SECTION_COUNT * 4);
    debug_assert_eq!(file_info + FILE_INFO_SIZE, PROVIDER_IMAGE_SIZE);
    let offsets = ProviderOffsets {
        text,
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
    write_elf_header(&mut image, PROVIDER_SECTION_COUNT, 6, 0);
    write_sections(&mut image, &sections);
    image[text..text + PROVIDER_TEXT.len()].copy_from_slice(PROVIDER_TEXT);
    write_u32(&mut image, export, 1);
    write_u32(&mut image, export + 12, 16);
    image[export + 16..export + 23].copy_from_slice(b"answer\0");
    write_symbol(&mut image, symtab + 16, 1, PROVIDER_TEXT_ADDRESS, 0x12, 1);
    image[strtab..strtab + SYMBOL_NAMES.len()].copy_from_slice(SYMBOL_NAMES);
    write_rela(
        &mut image,
        rela,
        PROVIDER_EXPORT_ADDRESS + 8,
        1,
        R_PPC_ADDR32,
    );
    image[shstrtab..shstrtab + PROVIDER_SECTION_NAMES.len()]
        .copy_from_slice(PROVIDER_SECTION_NAMES);
    write_file_info(&mut image[file_info..file_info + FILE_INFO_SIZE], 0, 0);
    write_crc_table(&mut image, &sections, crc);
    Ok(image)
}

fn empty_image(size: usize) -> Result<Vec<u8>, RplCallFixtureError> {
    let mut image = Vec::new();
    image
        .try_reserve_exact(size)
        .map_err(|_| RplCallFixtureError::AllocationFailed { requested: size })?;
    image.resize(size, 0);
    Ok(image)
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
    const fn new(name: u32, kind: u32, offset: usize, size: usize) -> Self {
        Self {
            name,
            kind,
            ..Self::NULL
        }
        .with_location(offset, size)
    }
    const fn with_location(mut self, offset: usize, size: usize) -> Self {
        self.offset = offset;
        self.size = size;
        self
    }
    const fn with_memory(mut self, flags: u32, address: u32) -> Self {
        self.flags = flags;
        self.address = address;
        self
    }
    const fn with_link(mut self, link: u32, info: u32) -> Self {
        self.link = link;
        self.info = info;
        self
    }
    const fn with_layout(mut self, align: u32, entry_size: u32) -> Self {
        self.align = align;
        self.entry_size = entry_size;
        self
    }
}
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
fn main_sections(o: &MainOffsets) -> [Section; MAIN_SECTION_COUNT] {
    [
        Section::NULL,
        Section::new(1, SHT_PROGBITS, o.text, MAIN_TEXT.len())
            .with_memory(SHF_ALLOC | SHF_EXECINSTR, MAIN_TEXT_ADDRESS)
            .with_layout(4, 0),
        Section::new(7, SHT_PROGBITS, o.data, 4)
            .with_memory(SHF_ALLOC | SHF_WRITE, MAIN_DATA_ADDRESS)
            .with_layout(4, 0),
        Section::new(13, SHT_RPL_IMPORTS, o.import, MAIN_IMPORT.len())
            .with_memory(SHF_ALLOC | SHF_EXECINSTR, MAIN_IMPORT_ADDRESS)
            .with_layout(4, 0),
        Section::new(30, SHT_SYMTAB, o.symtab, 32)
            .with_link(5, 1)
            .with_layout(4, 16),
        Section::new(38, SHT_STRTAB, o.strtab, SYMBOL_NAMES.len()).with_layout(1, 0),
        Section::new(46, SHT_RELA, o.rela, 12)
            .with_link(4, 1)
            .with_layout(4, 12),
        Section::new(57, SHT_STRTAB, o.shstrtab, MAIN_SECTION_NAMES.len()).with_layout(1, 0),
        Section::new(67, SHT_RPL_CRCS, o.crc, MAIN_SECTION_COUNT * 4).with_layout(4, 4),
        Section::new(73, SHT_RPL_FILE_INFO, o.file_info, FILE_INFO_SIZE).with_layout(4, 0),
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
fn provider_sections(o: &ProviderOffsets) -> [Section; PROVIDER_SECTION_COUNT] {
    [
        Section::NULL,
        Section::new(1, SHT_PROGBITS, o.text, PROVIDER_TEXT.len())
            .with_memory(SHF_ALLOC | SHF_EXECINSTR, PROVIDER_TEXT_ADDRESS)
            .with_layout(4, 0),
        Section::new(7, SHT_RPL_EXPORTS, o.export, 23)
            .with_memory(SHF_ALLOC | SHF_EXECINSTR, PROVIDER_EXPORT_ADDRESS)
            .with_layout(4, 0),
        Section::new(17, SHT_SYMTAB, o.symtab, 32)
            .with_link(4, 1)
            .with_layout(4, 16),
        Section::new(25, SHT_STRTAB, o.strtab, SYMBOL_NAMES.len()).with_layout(1, 0),
        Section::new(33, SHT_RELA, o.rela, 12)
            .with_link(3, 2)
            .with_layout(4, 12),
        Section::new(49, SHT_STRTAB, o.shstrtab, PROVIDER_SECTION_NAMES.len()).with_layout(1, 0),
        Section::new(59, SHT_RPL_CRCS, o.crc, PROVIDER_SECTION_COUNT * 4).with_layout(4, 4),
        Section::new(65, SHT_RPL_FILE_INFO, o.file_info, FILE_INFO_SIZE).with_layout(4, 0),
    ]
}

fn write_sections(image: &mut [u8], sections: &[Section]) {
    for (index, value) in sections.iter().enumerate() {
        let offset = SECTION_TABLE_OFFSET + (index * SECTION_HEADER_SIZE);
        write_u32(image, offset, value.name);
        write_u32(image, offset + 4, value.kind);
        write_u32(image, offset + 8, value.flags);
        write_u32(image, offset + 12, value.address);
        write_u32(
            image,
            offset + 16,
            u32::try_from(value.offset).expect("fixture section offset fits ELF32"),
        );
        write_u32(
            image,
            offset + 20,
            u32::try_from(value.size).expect("fixture section size fits ELF32"),
        );
        write_u32(image, offset + 24, value.link);
        write_u32(image, offset + 28, value.info);
        write_u32(image, offset + 32, value.align);
        write_u32(image, offset + 36, value.entry_size);
    }
}
fn write_symbol(
    bytes: &mut [u8],
    offset: usize,
    name: u32,
    value: u32,
    info: u8,
    section_index: u16,
) {
    write_u32(bytes, offset, name);
    write_u32(bytes, offset + 4, value);
    write_u32(bytes, offset + 8, 0);
    bytes[offset + 12] = info;
    write_u16(bytes, offset + 14, section_index);
}
fn write_rela(bytes: &mut [u8], offset: usize, target: u32, symbol: u32, kind: u32) {
    write_u32(bytes, offset, target);
    write_u32(bytes, offset + 4, (symbol << 8) | kind);
    write_u32(bytes, offset + 8, 0);
}
fn write_file_info(bytes: &mut [u8], flags: u32, data_region_size: u32) {
    write_u32(bytes, 0, 0xcafe_0402);
    write_u32(bytes, 4, 0x1000);
    write_u32(bytes, 8, 0x1000);
    write_u32(bytes, 12, data_region_size);
    write_u32(bytes, 16, 0x1000);
    write_u32(bytes, 20, 0x1000);
    write_u32(bytes, 0x34, flags);
    write_u16(bytes, 0x58, u16::MAX);
}
fn write_crc_table(image: &mut [u8], sections: &[Section], crc: usize) {
    for (index, value) in sections.iter().enumerate() {
        let checksum = if matches!(value.kind, 0 | SHT_RPL_CRCS) {
            0
        } else {
            crc32_ieee(&image[value.offset..value.offset + value.size])
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
        assert_fresh_allocations(main_rpx_call_fixture);
        assert_fresh_allocations(provider_rpl_call_fixture);
    }

    #[test]
    fn main_fixture_has_the_rel24_call_contract() {
        let image = main_rpx_call_fixture().expect("fixture allocation");
        assert_eq!(image.len(), MAIN_IMAGE_SIZE);
        assert_header(&image, MAIN_SECTION_COUNT, 7, MAIN_TEXT_ADDRESS);
        let text = read_section(&image, 1);
        let data = read_section(&image, 2);
        let import = read_section(&image, 3);
        let symbol = read_section(&image, 4);
        let relocation = read_section(&image, 6);
        let file_info = read_section(&image, 9);
        assert_eq!(
            (text.kind, text.flags, text.address, text.size),
            (
                SHT_PROGBITS,
                SHF_ALLOC | SHF_EXECINSTR,
                MAIN_TEXT_ADDRESS,
                12
            )
        );
        assert_eq!(payload(&image, text), MAIN_TEXT);
        assert_eq!(payload(&image, data), &[0, 0, 0, 0]);
        assert_eq!(payload(&image, import), MAIN_IMPORT);
        assert_eq!(read_u32(&image, symbol.offset + 16), 1);
        assert_eq!(
            read_u32(&image, symbol.offset + 20),
            MAIN_IMPORT_ADDRESS + 8
        );
        assert_eq!(image[symbol.offset + 28], 0x12);
        assert_eq!(read_u16(&image, symbol.offset + 30), 3);
        assert_eq!(
            (relocation.link, relocation.info, relocation.entry_size),
            (4, 1, 12)
        );
        assert_eq!(read_u32(&image, relocation.offset), MAIN_TEXT_ADDRESS);
        assert_eq!(
            read_u32(&image, relocation.offset + 4),
            (1 << 8) | R_PPC_REL24
        );
        assert_eq!(read_u32(&image, relocation.offset + 8), 0);
        assert_eq!(
            &payload(&image, read_section(&image, 7))[46..57],
            b".rela.text\0"
        );
        assert_file_info(&image, file_info, 2, 0x1000);
        assert_crcs(&image);
    }

    #[test]
    fn provider_fixture_preserves_the_provider_contract() {
        let image = provider_rpl_call_fixture().expect("fixture allocation");
        assert_eq!(image.len(), PROVIDER_IMAGE_SIZE);
        assert_header(&image, PROVIDER_SECTION_COUNT, 6, 0);
        let text = read_section(&image, 1);
        let export = read_section(&image, 2);
        let symbol = read_section(&image, 3);
        let relocation = read_section(&image, 5);
        let file_info = read_section(&image, 8);
        assert_eq!(payload(&image, text), PROVIDER_TEXT);
        assert_eq!(read_u32(&image, export.offset), 1);
        assert_eq!(&payload(&image, export)[16..], b"answer\0");
        assert_eq!(read_u32(&image, symbol.offset + 20), PROVIDER_TEXT_ADDRESS);
        assert_eq!(image[symbol.offset + 28], 0x12);
        assert_eq!(
            read_u32(&image, relocation.offset),
            PROVIDER_EXPORT_ADDRESS + 8
        );
        assert_eq!(
            read_u32(&image, relocation.offset + 4),
            (1 << 8) | R_PPC_ADDR32
        );
        assert_file_info(&image, file_info, 0, 0);
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
    fn read_section(image: &[u8], index: usize) -> ReadSection {
        let h = SECTION_TABLE_OFFSET + (index * SECTION_HEADER_SIZE);
        ReadSection {
            kind: read_u32(image, h + 4),
            flags: read_u32(image, h + 8),
            address: read_u32(image, h + 12),
            offset: usize::try_from(read_u32(image, h + 16))
                .expect("fixture section offset fits usize"),
            size: usize::try_from(read_u32(image, h + 20))
                .expect("fixture section size fits usize"),
            link: read_u32(image, h + 24),
            info: read_u32(image, h + 28),
            entry_size: read_u32(image, h + 36),
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
    fn assert_file_info(image: &[u8], section: ReadSection, flags: u32, data_region_size: u32) {
        assert_eq!(read_u32(image, section.offset), 0xcafe_0402);
        assert_eq!(read_u32(image, section.offset + 12), data_region_size);
        assert_eq!(read_u32(image, section.offset + 0x34), flags);
    }
    fn assert_crcs(image: &[u8]) {
        let count = usize::from(read_u16(image, 48));
        let crc = read_section(image, count - 2);
        for index in 0..count {
            let section = read_section(image, index);
            let expected = if matches!(section.kind, 0 | SHT_RPL_CRCS) {
                0
            } else {
                crc32_ieee(payload(image, section))
            };
            assert_eq!(read_u32(image, crc.offset + (index * 4)), expected);
        }
    }
    fn assert_fresh_allocations(build: fn() -> Result<Vec<u8>, RplCallFixtureError>) {
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
