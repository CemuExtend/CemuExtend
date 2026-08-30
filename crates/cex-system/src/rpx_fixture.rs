//! A deterministic, license-clean RPX fixture assembled from literal bytes.
//!
//! The image is intentionally small: it contains a three-instruction text
//! section and only the metadata an RPX loader needs to identify its layout.

const SECTION_TABLE_OFFSET: usize = 0x40;
const SECTION_HEADER_SIZE: usize = 0x28;
const SECTION_COUNT: usize = 5;
const PAYLOAD_OFFSET: usize = SECTION_TABLE_OFFSET + (SECTION_COUNT * SECTION_HEADER_SIZE);

const TEXT_OFFSET: usize = PAYLOAD_OFFSET;
const TEXT: [u8; 12] = [
    0x38, 0x60, 0x00, 0x28, // addi r3, r0, 40
    0x38, 0x63, 0x00, 0x02, // addi r3, r3, 2
    0x00, 0x00, 0x00, 0x00, // deterministic stop word
];
const SECTION_NAMES: &[u8] = b"\0.text\0.shstrtab\0";
const SECTION_NAMES_OFFSET: usize = TEXT_OFFSET + TEXT.len();
const CRC_OFFSET: usize = align_up(SECTION_NAMES_OFFSET + SECTION_NAMES.len(), 4);
const CRC_SIZE: usize = SECTION_COUNT * 4;
const FILE_INFO_OFFSET: usize = CRC_OFFSET + CRC_SIZE;
const FILE_INFO_SIZE: usize = 0x60;
const IMAGE_SIZE: usize = FILE_INFO_OFFSET + FILE_INFO_SIZE;

const ELF_HEADER_SIZE_U16: u16 = 0x34;
const SECTION_TABLE_OFFSET_U32: u32 = 0x40;
const SECTION_HEADER_SIZE_U16: u16 = 0x28;
const SECTION_COUNT_U16: u16 = 5;
const TEXT_OFFSET_U32: u32 = 0x108;
const TEXT_SIZE_U32: u32 = 12;
const SECTION_NAMES_OFFSET_U32: u32 = 0x114;
const SECTION_NAMES_SIZE_U32: u32 = 17;
const CRC_OFFSET_U32: u32 = 0x128;
const CRC_SIZE_U32: u32 = 20;
const FILE_INFO_OFFSET_U32: u32 = 0x13c;
const FILE_INFO_SIZE_U32: u32 = 0x60;

const SHT_PROGBITS: u32 = 1;
const SHT_STRTAB: u32 = 3;
const SHT_RPL_CRCS: u32 = 0x8000_0003;
const SHT_RPL_FILE_INFO: u32 = 0x8000_0004;
const SHF_ALLOC: u32 = 0x2;
const SHF_EXECINSTR: u32 = 0x4;

const fn align_up(value: usize, alignment: usize) -> usize {
    (value + (alignment - 1)) & !(alignment - 1)
}

/// Returns a fixed five-section Cafe RPX image for deterministic loader tests.
///
/// Its text starts at `0x0200_0000` and evaluates two `addi` instructions
/// before reaching a zero stop word. No host data, timestamps, assets, or
/// linker output contribute to the returned bytes.
#[must_use]
pub fn builtin_rpx_fixture() -> Vec<u8> {
    let mut image = vec![0; IMAGE_SIZE];

    write_elf_header(&mut image);
    write_section_headers(&mut image);

    image[TEXT_OFFSET..TEXT_OFFSET + TEXT.len()].copy_from_slice(&TEXT);
    image[SECTION_NAMES_OFFSET..SECTION_NAMES_OFFSET + SECTION_NAMES.len()]
        .copy_from_slice(SECTION_NAMES);

    let file_info = file_info_bytes();
    image[FILE_INFO_OFFSET..FILE_INFO_OFFSET + FILE_INFO_SIZE].copy_from_slice(&file_info);

    let mut crcs = [0; CRC_SIZE];
    write_u32(&mut crcs, 4, crc32_ieee(&TEXT));
    write_u32(&mut crcs, 8, crc32_ieee(SECTION_NAMES));
    write_u32(&mut crcs, 16, crc32_ieee(&file_info));
    image[CRC_OFFSET..CRC_OFFSET + CRC_SIZE].copy_from_slice(&crcs);

    image
}

fn write_elf_header(image: &mut [u8]) {
    image[..16].copy_from_slice(&[
        0x7f, b'E', b'L', b'F', 1, 2, 1, 0xca, 0xfe, 0, 0, 0, 0, 0, 0, 0,
    ]);
    write_u16(image, 16, 0xfe01);
    write_u16(image, 18, 20);
    write_u32(image, 20, 1);
    write_u32(image, 24, 0x0200_0000);
    write_u32(image, 32, SECTION_TABLE_OFFSET_U32);
    write_u16(image, 40, ELF_HEADER_SIZE_U16);
    write_u16(image, 46, SECTION_HEADER_SIZE_U16);
    write_u16(image, 48, SECTION_COUNT_U16);
    write_u16(image, 50, 2);
}

fn write_section_headers(image: &mut [u8]) {
    write_section_header(
        image,
        1,
        &SectionHeader {
            name: 1,
            section_type: SHT_PROGBITS,
            flags: SHF_ALLOC | SHF_EXECINSTR,
            address: 0x0200_0000,
            offset: TEXT_OFFSET_U32,
            size: TEXT_SIZE_U32,
            align: 4,
            ..SectionHeader::default()
        },
    );
    write_section_header(
        image,
        2,
        &SectionHeader {
            name: 7,
            section_type: SHT_STRTAB,
            offset: SECTION_NAMES_OFFSET_U32,
            size: SECTION_NAMES_SIZE_U32,
            align: 1,
            ..SectionHeader::default()
        },
    );
    write_section_header(
        image,
        3,
        &SectionHeader {
            section_type: SHT_RPL_CRCS,
            offset: CRC_OFFSET_U32,
            size: CRC_SIZE_U32,
            align: 4,
            entry_size: 4,
            ..SectionHeader::default()
        },
    );
    write_section_header(
        image,
        4,
        &SectionHeader {
            section_type: SHT_RPL_FILE_INFO,
            offset: FILE_INFO_OFFSET_U32,
            size: FILE_INFO_SIZE_U32,
            align: 4,
            ..SectionHeader::default()
        },
    );
}

#[derive(Default)]
struct SectionHeader {
    name: u32,
    section_type: u32,
    flags: u32,
    address: u32,
    offset: u32,
    size: u32,
    link: u32,
    info: u32,
    align: u32,
    entry_size: u32,
}

fn write_section_header(image: &mut [u8], index: usize, header: &SectionHeader) {
    let offset = SECTION_TABLE_OFFSET + (index * SECTION_HEADER_SIZE);
    write_u32(image, offset, header.name);
    write_u32(image, offset + 4, header.section_type);
    write_u32(image, offset + 8, header.flags);
    write_u32(image, offset + 12, header.address);
    write_u32(image, offset + 16, header.offset);
    write_u32(image, offset + 20, header.size);
    write_u32(image, offset + 24, header.link);
    write_u32(image, offset + 28, header.info);
    write_u32(image, offset + 32, header.align);
    write_u32(image, offset + 36, header.entry_size);
}

fn file_info_bytes() -> [u8; FILE_INFO_SIZE] {
    let mut file_info = [0; FILE_INFO_SIZE];
    write_u32(&mut file_info, 0x00, 0xcafe_0402);
    write_u32(&mut file_info, 0x04, 0x1000);
    write_u32(&mut file_info, 0x08, 0x20);
    write_u32(&mut file_info, 0x0c, 0);
    write_u32(&mut file_info, 0x10, 0x1000);
    write_u32(&mut file_info, 0x14, 0);
    write_u32(&mut file_info, 0x20, 0);
    write_u32(&mut file_info, 0x24, 0x8000);
    write_u32(&mut file_info, 0x28, 0x8000);
    write_u32(&mut file_info, 0x34, 2);
    write_u32(&mut file_info, 0x4c, 0);
    write_u16(&mut file_info, 0x58, u16::MAX);
    file_info
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
    fn fixture_is_byte_deterministic() {
        assert_eq!(builtin_rpx_fixture(), builtin_rpx_fixture());
    }

    #[test]
    fn fixture_has_stable_size_and_layout_fields() {
        let image = builtin_rpx_fixture();

        assert_eq!(image.len(), 0x19c);
        assert_eq!(
            [
                u32::from(read_u16(&image, 16)),
                u32::from(read_u16(&image, 18)),
                read_u32(&image, 20),
                read_u32(&image, 24),
                read_u32(&image, 32),
                u32::from(read_u16(&image, 40)),
                u32::from(read_u16(&image, 46)),
                u32::from(read_u16(&image, 48)),
                u32::from(read_u16(&image, 50)),
            ],
            [0xfe01, 20, 1, 0x0200_0000, 0x40, 0x34, 0x28, 5, 2]
        );
        assert_eq!(&image[TEXT_OFFSET..TEXT_OFFSET + TEXT.len()], &TEXT);
        assert_eq!(
            section_field_vector(&image),
            [
                (0, 0, 0, 0, 0),
                (
                    SHT_PROGBITS,
                    SHF_ALLOC | SHF_EXECINSTR,
                    0x0200_0000,
                    0x108,
                    12
                ),
                (SHT_STRTAB, 0, 0, 0x114, 17),
                (SHT_RPL_CRCS, 0, 0, 0x128, 20),
                (SHT_RPL_FILE_INFO, 0, 0, 0x13c, 0x60),
            ]
        );
        assert_eq!(
            [
                read_u32(&image, FILE_INFO_OFFSET),
                read_u32(&image, FILE_INFO_OFFSET + 4),
                read_u32(&image, FILE_INFO_OFFSET + 8),
                read_u32(&image, FILE_INFO_OFFSET + 16),
                read_u32(&image, FILE_INFO_OFFSET + 52),
                u32::from(read_u16(&image, FILE_INFO_OFFSET + 88)),
            ],
            [0xcafe_0402, 0x1000, 0x20, 0x1000, 2, 0xffff]
        );
    }

    #[test]
    fn crc_table_matches_raw_non_crc_section_bytes() {
        let image = builtin_rpx_fixture();
        let file_info = &image[FILE_INFO_OFFSET..FILE_INFO_OFFSET + FILE_INFO_SIZE];

        assert_eq!(crc32_ieee(b"123456789"), 0xcbf4_3926);
        assert_eq!(read_u32(&image, CRC_OFFSET), 0);
        assert_eq!(
            read_u32(&image, CRC_OFFSET + 4),
            crc32_ieee(&image[TEXT_OFFSET..TEXT_OFFSET + TEXT.len()])
        );
        assert_eq!(
            read_u32(&image, CRC_OFFSET + 8),
            crc32_ieee(&image[SECTION_NAMES_OFFSET..SECTION_NAMES_OFFSET + SECTION_NAMES.len()])
        );
        assert_eq!(read_u32(&image, CRC_OFFSET + 12), 0);
        assert_eq!(read_u32(&image, CRC_OFFSET + 16), crc32_ieee(file_info));
    }

    fn section_field_vector(image: &[u8]) -> [(u32, u32, u32, u32, u32); SECTION_COUNT] {
        core::array::from_fn(|index| {
            let offset = SECTION_TABLE_OFFSET + (index * SECTION_HEADER_SIZE);
            (
                read_u32(image, offset + 4),
                read_u32(image, offset + 8),
                read_u32(image, offset + 12),
                read_u32(image, offset + 16),
                read_u32(image, offset + 20),
            )
        })
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
