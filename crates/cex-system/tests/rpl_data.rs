//! Public end-to-end coverage for imported data addressed by an `ADDR16` pair.

use cex_cpu::{BudgetKind, StopReason};
use cex_system::{
    CafeRelocationKind, CafeSymbolKind, HeadlessError, HeadlessSystem, RplModuleName, RpxError,
    RpxRplLinkPhase, commit_rpx_rpl_link, parse_rpl, parse_rpx, plan_rpx_rpl_link,
    synthetic_rpx_rpl_data_main_fixture, synthetic_rpx_rpl_data_provider_fixture,
};
use cex_types::GuestAddress;

fn provider_name() -> RplModuleName {
    RplModuleName::new("linkmod.rpl").expect("fixture provider name is canonical")
}

fn data_fixture() -> (Vec<u8>, Vec<u8>) {
    (
        synthetic_rpx_rpl_data_main_fixture().expect("main fixture must build"),
        synthetic_rpx_rpl_data_provider_fixture().expect("provider fixture must build"),
    )
}

#[test]
fn public_addr16_data_fixtures_are_fresh_and_preserve_relocation_order() {
    let (main_first, provider_first) = data_fixture();
    let (main_second, provider_second) = data_fixture();

    assert_eq!(main_first, main_second);
    assert_ne!(main_first.as_ptr(), main_second.as_ptr());
    assert_eq!(provider_first, provider_second);
    assert_ne!(provider_first.as_ptr(), provider_second.as_ptr());

    let main = parse_rpx(&main_first).expect("main fixture must parse");
    let provider = parse_rpl(&provider_first).expect("provider fixture must parse");
    assert_eq!(main.relocations().len(), 2);
    assert_eq!(main.relocations()[0].kind(), CafeRelocationKind::Addr16Ha);
    assert_eq!(main.relocations()[0].offset(), 0x0200_0002);
    assert_eq!(main.relocations()[0].addend(), 4);
    assert_eq!(main.relocations()[1].kind(), CafeRelocationKind::Addr16Lo);
    assert_eq!(main.relocations()[1].offset(), 0x0200_0006);
    assert_eq!(main.relocations()[1].addend(), 4);
    assert_eq!(main.imports().len(), 1);
    assert_eq!(main.imports()[0].kind(), CafeSymbolKind::Data);
    assert_eq!(provider.relocations().len(), 1);
    assert_eq!(provider.relocations()[0].kind(), CafeRelocationKind::Addr32);
    assert_eq!(provider.relocations()[0].offset(), 0xc000_0008);
    assert_eq!(provider.symbols().len(), 1);
    assert_eq!(provider.symbols()[0].kind(), CafeSymbolKind::Data);
    assert_eq!(provider.exports().len(), 1);
    assert_eq!(provider.exports()[0].kind(), CafeSymbolKind::Data);
}

#[test]
fn linked_addr16_data_load_has_exact_repeatable_proof_and_state() {
    let (main, provider) = data_fixture();
    let committed = commit_rpx_rpl_link(
        plan_rpx_rpl_link(
            parse_rpx(&main).expect("main fixture must parse"),
            provider_name(),
            parse_rpl(&provider).expect("provider fixture must parse"),
        )
        .expect("ADDR16 data link must plan"),
    )
    .expect("ADDR16 data link must commit");
    let first = HeadlessSystem::default()
        .run_rpx_rpl(&main, provider_name(), &provider)
        .expect("ADDR16 data fixture must execute");
    let second = HeadlessSystem::default()
        .run_rpx_rpl(&main, provider_name(), &provider)
        .expect("repeated ADDR16 data fixture must execute");

    assert_eq!(first, second);
    assert_eq!(first.execution.program_hash, second.execution.program_hash);
    assert_eq!(first.execution.memory_hash, second.execution.memory_hash);
    assert_eq!(
        first.link_proof.memory_hash(),
        second.link_proof.memory_hash()
    );
    assert_eq!(first.link_proof, committed);
    assert_eq!(first.link_proof.main_entry(), 0x0200_0000);

    let relocations = first.link_proof.relocations();
    assert_eq!(relocations.len(), 3);
    assert_eq!(relocations[0].phase(), RpxRplLinkPhase::Local);
    assert_eq!(relocations[0].kind(), CafeRelocationKind::Addr32);
    assert_eq!(relocations[0].site(), 0x1000_9008);
    assert_eq!(relocations[0].before(), 0);
    assert_eq!(relocations[0].after(), 0x1000_8000);
    assert_eq!(relocations[0].width_bytes(), 4);
    assert_eq!(relocations[0].resolved_symbol(), 0x1000_8000);
    assert_eq!(relocations[0].addend(), 0);
    assert_eq!(relocations[0].displacement(), None);
    assert_eq!(relocations[1].phase(), RpxRplLinkPhase::Import);
    assert_eq!(relocations[1].kind(), CafeRelocationKind::Addr16Ha);
    assert_eq!(relocations[1].site(), 0x0200_0002);
    assert_eq!(relocations[1].before(), 0);
    assert_eq!(relocations[1].after(), 0x1001);
    assert_eq!(relocations[1].width_bytes(), 2);
    assert_eq!(relocations[1].resolved_symbol(), 0x1000_8000);
    assert_eq!(relocations[1].addend(), 4);
    assert_eq!(relocations[1].displacement(), None);
    assert_eq!(relocations[2].phase(), RpxRplLinkPhase::Import);
    assert_eq!(relocations[2].kind(), CafeRelocationKind::Addr16Lo);
    assert_eq!(relocations[2].site(), 0x0200_0006);
    assert_eq!(relocations[2].before(), 0);
    assert_eq!(relocations[2].after(), 0x8004);
    assert_eq!(relocations[2].width_bytes(), 2);
    assert_eq!(relocations[2].resolved_symbol(), 0x1000_8000);
    assert_eq!(relocations[2].addend(), 4);
    assert_eq!(relocations[2].displacement(), None);
    assert_eq!(first.link_proof.local_relocation(), &relocations[0]);
    assert_eq!(first.link_proof.import_relocations(), &relocations[1..]);
    assert_eq!(first.link_proof.mapped_page_count(), 12);
    assert_eq!(first.link_proof.mapped_byte_count(), 0xc000);

    let execution = &first.execution;
    let state = &execution.final_state;
    assert_eq!(execution.mapped_page_count, 28);
    assert_eq!(state.gpr(1), Some(0x4000_0000));
    assert_eq!(state.gpr(3), Some(42));
    assert_eq!(state.gpr(4), Some(0x1000_8004));
    assert_eq!(state.instruction_pointer, GuestAddress::new(0x0200_0010));
    assert_eq!(state.link_register, 0);
    assert_eq!(state.count_register, 0);
    assert_eq!(state.condition_register, 0);
    assert_eq!(state.xer, 0);
    assert_eq!(state.instructions_retired, 4);
    assert_eq!(state.cycles.get(), 4);
    assert_eq!(execution.outcome.reason, StopReason::StopSentinel);
    assert_eq!(execution.outcome.instructions_executed, 4);
    assert_eq!(execution.outcome.cycles_elapsed, 4);
}

#[test]
fn addr16_data_budget_and_malformed_inputs_cannot_publish_false_success() {
    let (main, provider) = data_fixture();
    let budgeted = HeadlessSystem::with_budget(3, 3)
        .expect("non-zero budget is valid")
        .run_rpx_rpl(&main, provider_name(), &provider)
        .expect("budget exhaustion is a normal outcome");
    assert_eq!(
        budgeted.execution.outcome.reason,
        StopReason::BudgetExhausted {
            kind: BudgetKind::Instructions,
        }
    );
    assert_ne!(budgeted.execution.outcome.reason, StopReason::StopSentinel);
    assert_eq!(budgeted.execution.outcome.instructions_executed, 3);

    let both_malformed = HeadlessSystem::default().run_rpx_rpl(&[0], provider_name(), &[0, 0]);
    assert!(matches!(
        both_malformed,
        Err(HeadlessError::Rpx(RpxError::TruncatedHeader { actual: 1 }))
    ));
    let malformed_provider = HeadlessSystem::default().run_rpx_rpl(&main, provider_name(), &[0]);
    assert!(matches!(
        malformed_provider,
        Err(HeadlessError::Rpx(RpxError::TruncatedHeader { actual: 1 }))
    ));
}
