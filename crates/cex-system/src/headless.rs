//! Deterministic composition of loader, memory, and the PPC interpreter.

use cex_cpu::{Cpu, CpuFault, CpuState, ExecutionBudget, ExecutionOutcome};
use cex_memory::{GuestMemory, MemoryFault, PAGE_SIZE, Permissions};
use cex_types::GuestAddress;
use sha2::{Digest, Sha256};
use thiserror::Error;

use crate::{ProgramDecodeError, SyntheticProgram};

const STACK_RESERVE: u64 = 64 * 1024;
const DEFAULT_BUDGET: u64 = 10_000;

/// Stateless deterministic headless runtime.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HeadlessSystem {
    budget: ExecutionBudget,
}

impl HeadlessSystem {
    /// Construct a runner with independent instruction and guest-cycle limits.
    pub fn with_budget(max_instructions: u64, max_cycles: u64) -> Result<Self, HeadlessError> {
        if max_instructions == 0 || max_cycles == 0 {
            return Err(HeadlessError::ZeroBudget);
        }
        Ok(Self {
            budget: ExecutionBudget::new(max_instructions, max_cycles),
        })
    }

    /// Validate, map, and execute one synthetic program.
    ///
    /// Mapping is transactional from the caller's point of view: all work is
    /// performed in a fresh address space and no partially initialized system
    /// is returned if validation, protection, or execution fails.
    pub fn run(&self, program: &SyntheticProgram) -> Result<HeadlessRun, HeadlessError> {
        // `SyntheticProgram` fields are public for fixture ergonomics. Re-run
        // the complete wire validation here so callers cannot bypass size,
        // range, or entry-point constraints by constructing the struct directly.
        let canonical_image = program.encode()?;
        let program = SyntheticProgram::decode(&canonical_image)?;
        let mut memory = GuestMemory::new();
        map_code(&mut memory, &program)?;
        map_stack(&mut memory, program.stack_pointer)?;

        let mut state = CpuState::new(program.entry_point);
        state.gprs[1] = program.stack_pointer.get();
        let mut cpu = Cpu::with_state(state);
        let outcome = cpu.run_with_budget(&mut memory, self.budget)?;
        let final_state = cpu.into_state();

        Ok(HeadlessRun {
            final_state,
            outcome,
            memory_hash: memory.deterministic_hash(),
            mapped_page_count: memory.mapped_page_count(),
            program_hash: Sha256::digest(&canonical_image).into(),
        })
    }
}

impl Default for HeadlessSystem {
    fn default() -> Self {
        Self {
            budget: ExecutionBudget::new(DEFAULT_BUDGET, DEFAULT_BUDGET),
        }
    }
}

/// Canonical results retained after a successful bounded execution.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HeadlessRun {
    /// Final architectural register and cycle state.
    pub final_state: CpuState,
    /// Explicit halt, syscall, or budget outcome.
    pub outcome: ExecutionOutcome,
    /// Deterministic digest of mapped ranges, permissions, and contents.
    pub memory_hash: [u8; 32],
    /// Number of logical pages represented by the memory digest.
    pub mapped_page_count: u64,
    /// SHA-256 of the complete canonical CEXH artifact.
    pub program_hash: [u8; 32],
}

fn map_code(memory: &mut GuestMemory, program: &SyntheticProgram) -> Result<(), HeadlessError> {
    if program.code.is_empty() || !program.code.len().is_multiple_of(4) {
        return Err(HeadlessError::InvalidCodeLength(program.code.len()));
    }
    let page_mask = PAGE_SIZE - 1;
    let load = u64::from(program.load_address.get());
    let mapping_start = load & !page_mask;
    let offset = load - mapping_start;
    let occupied = offset
        .checked_add(u64::try_from(program.code.len()).map_err(|_| HeadlessError::AddressOverflow)?)
        .ok_or(HeadlessError::AddressOverflow)?;
    let mapping_len = occupied
        .checked_add(page_mask)
        .ok_or(HeadlessError::AddressOverflow)?
        & !page_mask;
    let mapping_start =
        GuestAddress::try_from(mapping_start).map_err(|_| HeadlessError::AddressOverflow)?;

    memory.map(
        mapping_start,
        mapping_len,
        Permissions::READ | Permissions::WRITE,
    )?;
    memory.write(program.load_address, &program.code)?;
    memory.protect(
        mapping_start,
        mapping_len,
        Permissions::READ | Permissions::EXECUTE,
    )?;
    Ok(())
}

fn map_stack(memory: &mut GuestMemory, stack_pointer: GuestAddress) -> Result<(), HeadlessError> {
    let raw = u64::from(stack_pointer.get());
    if raw < STACK_RESERVE || raw % PAGE_SIZE != 0 {
        return Err(HeadlessError::InvalidStackPointer(stack_pointer));
    }
    let start =
        GuestAddress::try_from(raw - STACK_RESERVE).map_err(|_| HeadlessError::AddressOverflow)?;
    memory.map(start, STACK_RESERVE, Permissions::READ | Permissions::WRITE)?;
    Ok(())
}

/// Validation, mapping, or execution failure in the headless system.
#[derive(Debug, Error)]
pub enum HeadlessError {
    /// Both execution bounds must allow at least one step.
    #[error("instruction and cycle budgets must both be non-zero")]
    ZeroBudget,
    /// Code must contain complete PPC words.
    #[error("synthetic program has invalid code length {0}")]
    InvalidCodeLength(usize),
    /// The stack pointer must be a page-aligned exclusive stack end.
    #[error("invalid synthetic stack pointer {0}")]
    InvalidStackPointer(GuestAddress),
    /// Host arithmetic could not represent a guest mapping.
    #[error("synthetic program mapping overflows guest address space")]
    AddressOverflow,
    /// Synthetic program wire validation failed at the execution boundary.
    #[error(transparent)]
    Program(#[from] ProgramDecodeError),
    /// Guest memory mapping or access failed.
    #[error("failed to initialize guest memory: {0}")]
    Memory(#[from] MemoryFault),
    /// PPC execution faulted before reaching an explicit stop.
    #[error("PPC execution failed: {0}")]
    Cpu(#[from] CpuFault),
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::builtin_fixture;
    use cex_cpu::{BudgetKind, StopReason};

    #[test]
    fn bundled_fixture_returns_42_and_stops() {
        let run = HeadlessSystem::default()
            .run(&builtin_fixture())
            .expect("public fixture must execute");

        assert_eq!(run.final_state.gpr(3), Some(42));
        assert_eq!(run.final_state.instructions_retired, 3);
        assert_eq!(run.outcome.reason, StopReason::StopSentinel);
        assert_eq!(run.outcome.instructions_executed, 3);
    }

    #[test]
    fn looping_program_stops_at_the_instruction_bound() {
        let mut program = builtin_fixture();
        program.code = 0x4800_0000_u32.to_be_bytes().to_vec(); // b .
        let run = HeadlessSystem::with_budget(4, 10)
            .expect("non-zero budgets are valid")
            .run(&program)
            .expect("bounded loop must be a normal outcome");

        assert_eq!(
            run.outcome.reason,
            StopReason::BudgetExhausted {
                kind: BudgetKind::Instructions
            }
        );
        assert_eq!(run.outcome.instructions_executed, 4);
    }
}
