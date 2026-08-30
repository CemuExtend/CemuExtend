//! Integration tests for the synthetic boot fixture and headless system execution.

use cex_cpu::{BudgetKind, StopReason};
use cex_system::{
    HeadlessError, HeadlessSystem, MAX_SYNTHETIC_CODE_SIZE, ProgramDecodeError, SyntheticProgram,
    builtin_fixture,
};

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
