//! Public API coverage for the deterministic RPX-to-RPL imported-call slice.

use cex_system::{
    CafeRelocationKind, RplModuleName, RpxRplLinkPhase, commit_rpx_rpl_link, parse_rpl, parse_rpx,
    plan_rpx_rpl_link, synthetic_rpx_rpl_call_main_fixture,
    synthetic_rpx_rpl_call_provider_fixture,
};

fn call_proof() -> cex_system::RpxRplLinkProof {
    let main_bytes = synthetic_rpx_rpl_call_main_fixture().expect("main fixture allocation");
    let provider_bytes =
        synthetic_rpx_rpl_call_provider_fixture().expect("provider fixture allocation");
    let plan = plan_rpx_rpl_link(
        parse_rpx(&main_bytes).expect("main fixture parses"),
        RplModuleName::new("linkmod.rpl").expect("fixture provider name is valid"),
        parse_rpl(&provider_bytes).expect("provider fixture parses"),
    )
    .expect("fixture shape is supported");

    commit_rpx_rpl_link(plan).expect("fixture link commits")
}

/// Fixtures are equal across calls while retaining independent owned storage.
#[test]
fn public_rpx_rpl_call_fixtures_are_fresh_and_describe_rel24_addr32() {
    let main_first = synthetic_rpx_rpl_call_main_fixture().expect("main fixture allocation");
    let main_second = synthetic_rpx_rpl_call_main_fixture().expect("main fixture allocation");
    let provider_first =
        synthetic_rpx_rpl_call_provider_fixture().expect("provider fixture allocation");
    let provider_second =
        synthetic_rpx_rpl_call_provider_fixture().expect("provider fixture allocation");

    assert_eq!(main_first, main_second);
    assert_ne!(main_first.as_ptr(), main_second.as_ptr());
    assert_eq!(provider_first, provider_second);
    assert_ne!(provider_first.as_ptr(), provider_second.as_ptr());

    let main = parse_rpx(&main_first).expect("main fixture parses");
    let provider = parse_rpl(&provider_first).expect("provider fixture parses");
    assert_eq!(main.relocations().len(), 1);
    assert_eq!(main.relocations()[0].kind(), CafeRelocationKind::Rel24);
    assert_eq!(provider.relocations().len(), 1);
    assert_eq!(provider.relocations()[0].kind(), CafeRelocationKind::Addr32);
}

/// Repeated planning and committing preserves the complete public relocation proof.
#[test]
fn public_rpx_rpl_call_link_proof_is_deterministic_and_complete() {
    let first = call_proof();
    let second = call_proof();

    assert_eq!(first, second);
    assert_eq!(first.memory_hash(), second.memory_hash());
    assert_eq!(first.mapped_page_count(), 5);
    assert_eq!(first.mapped_byte_count(), 0x5000);

    let relocations = first.relocations();
    assert_eq!(relocations[0].phase(), RpxRplLinkPhase::Local);
    assert_eq!(relocations[0].kind(), CafeRelocationKind::Addr32);
    assert_eq!(relocations[0].site(), 0x1000_2008);
    assert_eq!(relocations[0].before(), 0);
    assert_eq!(relocations[0].after(), 0x0200_2000);
    assert_eq!(relocations[0].resolved_symbol(), 0x0200_2000);
    assert_eq!(relocations[0].addend(), 0);
    assert_eq!(relocations[0].displacement(), None);

    assert_eq!(relocations[1].phase(), RpxRplLinkPhase::Import);
    assert_eq!(relocations[1].kind(), CafeRelocationKind::Rel24);
    assert_eq!(relocations[1].site(), 0x0200_0000);
    assert_eq!(relocations[1].before(), 0x4800_0001);
    assert_eq!(relocations[1].after(), 0x4800_2001);
    assert_eq!(relocations[1].resolved_symbol(), 0x0200_2000);
    assert_eq!(relocations[1].addend(), 0);
    assert_eq!(relocations[1].displacement(), Some(0x2000));

    assert_eq!(first.local_patch_site(), relocations[0].site());
    assert_eq!(first.local_patch_value(), relocations[0].after());
    assert_eq!(first.import_patch_site(), relocations[1].site());
    assert_eq!(first.import_patch_value(), relocations[1].after());
}
