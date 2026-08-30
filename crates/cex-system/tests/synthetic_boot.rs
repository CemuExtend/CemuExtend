//! Integration tests for the synthetic boot fixture and headless system execution.

use cex_cpu::{BudgetKind, StopReason};
use cex_system::{
    HeadlessError, HeadlessSystem, MAX_SYNTHETIC_CODE_SIZE, ProgramDecodeError, RpxError,
    RpxFileInfoField, RpxMappingRegion, SyntheticProgram, builtin_fixture, builtin_rpx_fixture,
    parse_rpx,
};
use sha2::{Digest, Sha256};

#[test]
fn mapping_region_is_available_through_the_public_api() {
    assert_eq!(RpxMappingRegion::Text.to_string(), "text");
}

#[test]
fn encoded_fixture_boots_to_a_deterministic_stop() {
    let encoded = builtin_fixture()
        .encode()
        .expect("bundled fixture must encode");
    let decoded = SyntheticProgram::decode(&encoded).expect("bundled fixture must validate");
    let first = HeadlessSystem::default()
        .run(&decoded)
        .expect("bundled fixture must run");
    let second = HeadlessSystem::default()
        .run(&decoded)
        .expect("bundled fixture must repeat");

    assert_eq!(first, second);
    assert_eq!(first.final_state.gpr(3), Some(42));
    assert_eq!(first.outcome.reason, StopReason::StopSentinel);
}

#[test]
fn both_execution_budgets_are_enforced() {
    let mut loop_program = builtin_fixture();
    loop_program.code = 0x4800_0000_u32.to_be_bytes().to_vec();

    let instruction_limited = HeadlessSystem::with_budget(2, 10)
        .expect("budgets are valid")
        .run(&loop_program)
        .expect("a budget is a normal stop");
    assert_eq!(
        instruction_limited.outcome.reason,
        StopReason::BudgetExhausted {
            kind: BudgetKind::Instructions
        }
    );

    let cycle_limited = HeadlessSystem::with_budget(10, 2)
        .expect("budgets are valid")
        .run(&loop_program)
        .expect("a budget is a normal stop");
    assert_eq!(
        cycle_limited.outcome.reason,
        StopReason::BudgetExhausted {
            kind: BudgetKind::Cycles
        }
    );
}

#[test]
fn direct_struct_construction_cannot_bypass_the_code_limit() {
    let mut oversized = builtin_fixture();
    oversized.code = vec![0; MAX_SYNTHETIC_CODE_SIZE + 4];

    assert!(matches!(
        HeadlessSystem::default().run(&oversized),
        Err(HeadlessError::Program(ProgramDecodeError::CodeTooLarge(size)))
            if size == MAX_SYNTHETIC_CODE_SIZE + 4
    ));
}

#[test]
fn bundled_rpx_boots_to_42_repeatably() {
    let image = builtin_rpx_fixture();
    let parsed = parse_rpx(&image).expect("bundled RPX must pass the public parser");
    let first = HeadlessSystem::default()
        .run_rpx(&image)
        .expect("bundled RPX must run");
    let second = HeadlessSystem::default()
        .run_rpx(&image)
        .expect("bundled RPX must repeat");

    assert!(parsed.is_rpx());
    assert_eq!(parsed.entry_point(), 0x0200_0000);
    assert_eq!(first, second);
    assert_eq!(first.final_state.gpr(1), Some(0x4000_0000));
    assert_eq!(first.final_state.gpr(3), Some(42));
    assert_eq!(first.final_state.instructions_retired, 3);
    assert_eq!(first.outcome.reason, StopReason::StopSentinel);
    assert_eq!(first.mapped_page_count, 17);
    assert_eq!(first.program_hash, <[u8; 32]>::from(Sha256::digest(&image)));
}

#[test]
fn malformed_rpx_is_rejected_before_execution() {
    assert!(matches!(
        HeadlessSystem::default().run_rpx(b"not an RPX"),
        Err(HeadlessError::Rpx(RpxError::TruncatedHeader { actual: 10 }))
    ));
}

#[test]
fn rpl_flag_is_explicitly_rejected() {
    let mut image = builtin_rpx_fixture();
    let file_info_offset =
        usize::try_from(section_u32(&image, 4, 16)).expect("FILEINFO offset must fit in usize");
    write_u32(&mut image, file_info_offset + 0x34, 0);

    assert!(matches!(
        parse_rpx(&image),
        Err(RpxError::InvalidFileInfo {
            field: RpxFileInfoField::Flags,
            value: 0,
        })
    ));
}

#[test]
fn allocated_metadata_is_not_mapped_as_guest_program_data() {
    let mut image = builtin_rpx_fixture();
    let name_table_header = section_header_offset(&image, 2);
    write_u32(&mut image, name_table_header + 8, 0x3);
    write_u32(&mut image, name_table_header + 12, 0x0200_0010);

    assert!(matches!(
        parse_rpx(&image),
        Err(RpxError::InvalidMetadataFlags {
            section_index: 2,
            section_type: 3,
            flags: 3,
        })
    ));
}

#[test]
fn sections_that_share_a_page_cannot_escalate_to_write_execute() {
    let image = rpx_with_data_section_at(0x0200_0010);
    parse_rpx(&image).expect("section byte ranges are valid before page permissions are merged");

    assert!(matches!(
        HeadlessSystem::default().run_rpx(&image),
        Err(HeadlessError::RpxPageWriteExecute(address))
            if address.get() == 0x0200_0000
    ));
}

#[test]
fn rpx_stack_overlap_is_rejected_before_mapping() {
    let image = rpx_with_data_section_at(0x3fff_0000);
    parse_rpx(&image).expect("stack location is a headless mapping policy");

    assert!(matches!(
        HeadlessSystem::default().run_rpx(&image),
        Err(HeadlessError::RpxStackOverlap(address))
            if address.get() == 0x3fff_0000
    ));
}

fn rpx_with_data_section_at(data_address: u32) -> Vec<u8> {
    const TEXT: &[u8] = &[
        0x38, 0x60, 0x00, 0x28, 0x38, 0x63, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    ];
    const DATA: &[u8] = &[1, 2, 3, 4];
    const NAMES: &[u8] = b"\0.text\0.shstrtab\0.data\0";
    const TEXT_OFFSET: usize = 0x130;
    const DATA_OFFSET: usize = 0x13c;
    const NAMES_OFFSET: usize = 0x140;
    const CRC_OFFSET: usize = 0x158;
    const FILE_INFO_OFFSET: usize = 0x170;
    const TEXT_OFFSET_U32: u32 = 0x130;
    const DATA_OFFSET_U32: u32 = 0x13c;
    const NAMES_OFFSET_U32: u32 = 0x140;
    const CRC_OFFSET_U32: u32 = 0x158;
    const FILE_INFO_OFFSET_U32: u32 = 0x170;

    let builtin = builtin_rpx_fixture();
    let builtin_file_info_offset =
        usize::try_from(section_u32(&builtin, 4, 16)).expect("FILEINFO offset must fit in usize");
    let mut image = vec![0; FILE_INFO_OFFSET + 0x60];
    image[..52].copy_from_slice(&builtin[..52]);
    write_u16(&mut image, 48, 6);
    write_u16(&mut image, 50, 3);
    write_section_header(
        &mut image,
        1,
        [1, 1, 0x6, 0x0200_0000, TEXT_OFFSET_U32, 12, 0, 0, 4, 0],
    );
    write_section_header(
        &mut image,
        2,
        [17, 1, 0x3, data_address, DATA_OFFSET_U32, 4, 0, 0, 4, 0],
    );
    write_section_header(
        &mut image,
        3,
        [7, 3, 0, 0, NAMES_OFFSET_U32, 23, 0, 0, 1, 0],
    );
    write_section_header(
        &mut image,
        4,
        [0, 0x8000_0003, 0, 0, CRC_OFFSET_U32, 24, 0, 0, 4, 4],
    );
    write_section_header(
        &mut image,
        5,
        [0, 0x8000_0004, 0, 0, FILE_INFO_OFFSET_U32, 0x60, 0, 0, 4, 0],
    );
    image[TEXT_OFFSET..TEXT_OFFSET + TEXT.len()].copy_from_slice(TEXT);
    image[DATA_OFFSET..DATA_OFFSET + DATA.len()].copy_from_slice(DATA);
    image[NAMES_OFFSET..NAMES_OFFSET + NAMES.len()].copy_from_slice(NAMES);
    image[FILE_INFO_OFFSET..FILE_INFO_OFFSET + 0x60]
        .copy_from_slice(&builtin[builtin_file_info_offset..builtin_file_info_offset + 0x60]);
    write_u32(&mut image, FILE_INFO_OFFSET + 0x0c, 0x1000);
    write_u32(&mut image, CRC_OFFSET + 4, crc32_ieee(TEXT));
    write_u32(&mut image, CRC_OFFSET + 8, crc32_ieee(DATA));
    write_u32(&mut image, CRC_OFFSET + 12, crc32_ieee(NAMES));
    let file_info_crc = crc32_ieee(&image[FILE_INFO_OFFSET..FILE_INFO_OFFSET + 0x60]);
    write_u32(&mut image, CRC_OFFSET + 20, file_info_crc);
    image
}

fn write_section_header(image: &mut [u8], section_index: usize, fields: [u32; 10]) {
    let offset = section_header_offset(image, section_index);
    for (field_index, value) in fields.into_iter().enumerate() {
        write_u32(image, offset + (field_index * 4), value);
    }
}

fn section_header_offset(image: &[u8], section_index: usize) -> usize {
    let section_table_offset =
        usize::try_from(read_u32(image, 32)).expect("section table offset must fit in usize");
    section_table_offset + (section_index * 40)
}

fn section_u32(image: &[u8], section_index: usize, field_offset: usize) -> u32 {
    read_u32(
        image,
        section_header_offset(image, section_index) + field_offset,
    )
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
}

fn write_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_be_bytes());
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
