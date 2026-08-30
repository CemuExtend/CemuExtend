//! Deterministic PowerPC execution primitives.
//!
//! The interpreter is intentionally the reference implementation. Later IML
//! optimisers and machine-code backends are expected to compare their results
//! against this crate's architectural state and stop reasons.

mod decode;
mod iml;
mod integer;
mod interpreter;
mod state;

pub use decode::{DecodeError, LoadWidth};
pub use iml::{
    ImlBlock, ImlCondition, ImlEvaluationError, ImlEvaluator, ImlInstruction, ImlOperand,
    ImlOutcome, ImlRegister,
};
pub use interpreter::{
    BudgetKind, Cpu, CpuFault, ExecutionBudget, ExecutionOutcome, FaultAccess, MemoryBus,
    StepOutcome, StopReason,
};
pub use state::{ArchitecturalFault, CpuState, FaultKind, RegisterIndexError, Reservation};

/// The synthetic headless-test sentinel. It is deliberately not a valid PPC
/// instruction, so a zero-filled executable page cannot accidentally run on.
pub const STOP_INSTRUCTION: u32 = 0;

/// The PowerPC `sc` instruction used by the first headless milestone.
pub const SYSTEM_CALL_INSTRUCTION: u32 = 0x4400_0002;
