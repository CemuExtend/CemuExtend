//! Public API coverage for the bounded synthetic RPX-to-RPL link slice.

use cex_system::{
    CafeModuleKind, CafeRelocationKind, CafeSymbolKind, RplModuleName, RpxError, RpxFileInfoField,
    RpxRplPlanError, commit_rpx_rpl_link, parse_rpl, parse_rpx, plan_rpx_rpl_link,
    synthetic_rpx_rpl_link_main_fixture, synthetic_rpx_rpl_link_provider_fixture,
};

fn linked_proof() -> cex_system::RpxRplLinkProof {
    let main_bytes = synthetic_rpx_rpl_link_main_fixture().expect("main fixture allocation");
    let provider_bytes =
        synthetic_rpx_rpl_link_provider_fixture().expect("provider fixture allocation");
    let plan = plan_rpx_rpl_link(
        parse_rpx(&main_bytes).expect("main fixture parses"),
        RplModuleName::new("linkmod.rpl").expect("fixture provider name is valid"),
        parse_rpl(&provider_bytes).expect("provider fixture parses"),
    )
    .expect("fixture shape is supported");

    commit_rpx_rpl_link(plan).expect("fixture link commits")
}

#[test]
fn public_rpx_rpl_link_slice_is_deterministic_and_complete() {
    let main_bytes = synthetic_rpx_rpl_link_main_fixture().expect("main fixture allocation");
    let provider_bytes =
        synthetic_rpx_rpl_link_provider_fixture().expect("provider fixture allocation");
    let main = parse_rpx(&main_bytes).expect("main fixture parses");
    let provider = parse_rpl(&provider_bytes).expect("provider fixture parses");

    assert_eq!(main.module_kind(), CafeModuleKind::Rpx);
    assert_eq!(provider.module_kind(), CafeModuleKind::Rpl);
    assert_eq!(main.sections().len(), 10);
    assert_eq!(provider.sections().len(), 9);
    assert_eq!(main.sections()[1].name(), ".text");
    assert_eq!(main.sections()[2].name(), ".data");
    assert_eq!(provider.sections()[1].name(), ".text");
    assert_eq!(provider.sections()[2].name(), ".fexports");
    assert_eq!(main.entry_point(), 0x0200_0000);
    assert_eq!(main.imports().len(), 1);
    assert_eq!(main.imports()[0].module_name(), "linkmod.rpl");
    assert_eq!(main.imports()[0].kind(), CafeSymbolKind::Function);
    assert_eq!(main.relocations()[0].kind(), CafeRelocationKind::Addr32);
    assert_eq!(provider.exports()[0].name(), "answer");
    assert_eq!(provider.exports()[0].kind(), CafeSymbolKind::Function);
    assert_eq!(provider.symbols()[0].name(), "answer");

    let first = linked_proof();
    let second = linked_proof();
    assert_eq!(first, second);
    let local = first.local_relocation();
    let imports = first.import_relocations();
    assert_eq!(imports.len(), 1);
    let import = &imports[0];
    assert_eq!(local.site(), 0x1000_2008);
    assert_eq!(import.site(), 0x1000_0000);
    assert_eq!(local.after(), 0x0200_2000);
    assert_eq!(import.after(), 0x0200_2000);
    assert_eq!(first.mapped_page_count(), 5);
    assert_eq!(first.mapped_byte_count(), 5 * 4096);
    assert_eq!(first.main_sha256(), second.main_sha256());
    assert_eq!(first.provider_sha256(), second.provider_sha256());
    assert_eq!(first.memory_hash(), second.memory_hash());
    assert_eq!(
        first.memory_hash(),
        [
            0x6f, 0x0b, 0x03, 0xa9, 0xb6, 0x9c, 0x89, 0x94, 0x43, 0x30, 0x51, 0x58, 0xc5, 0x48,
            0x83, 0x38, 0x74, 0x12, 0x91, 0xc8, 0xd0, 0x98, 0x53, 0x8b, 0x72, 0xd2, 0x49, 0x60,
            0x06, 0x12, 0x46, 0xae,
        ]
    );
}

#[test]
fn invalid_name_and_module_kind_are_redacted() {
    let raw_name = "../secret-provider.rpl";
    let name_error = RplModuleName::new(raw_name).expect_err("paths are forbidden");
    let formatted_name_error = format!("{name_error:?} {name_error}");
    assert!(!formatted_name_error.contains(raw_name));

    let main_bytes = synthetic_rpx_rpl_link_main_fixture().expect("main fixture allocation");
    let kind_error = parse_rpl(&main_bytes).expect_err("RPX cannot parse as RPL");
    assert!(matches!(
        &kind_error,
        RpxError::InvalidFileInfo {
            field: RpxFileInfoField::Flags,
            ..
        }
    ));
    let formatted_kind_error = format!("{kind_error:?} {kind_error}");
    assert!(!formatted_kind_error.contains(raw_name));

    let provider_bytes =
        synthetic_rpx_rpl_link_provider_fixture().expect("provider fixture allocation");
    let unresolved = plan_rpx_rpl_link(
        parse_rpx(&main_bytes).expect("main fixture parses"),
        RplModuleName::new("other-provider.rpl").expect("bounded wrong name"),
        parse_rpl(&provider_bytes).expect("provider fixture parses"),
    )
    .expect_err("wrong provider name is rejected");
    assert!(matches!(
        &unresolved,
        RpxRplPlanError::UnresolvedImport { .. }
    ));
    let formatted_unresolved = format!("{unresolved:?} {unresolved}");
    assert!(!formatted_unresolved.contains("other-provider.rpl"));
    assert!(!formatted_unresolved.contains("linkmod.rpl"));
}
