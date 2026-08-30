use core::fmt;

use cex_memory::{GuestMemory, MemoryFault};
use cex_types::GuestAddress;

use crate::decode::{
    ArithmeticOperation, DecodeError, Instruction, LoadWidth, LogicalOperation, decode,
};
use crate::integer::{sign_extend_i16, sign_extend_u16, truncate_u32_to_u8, truncate_u32_to_u16};
use crate::state::{ArchitecturalFault, CpuState, FaultKind};

const XER_CA: u32 = 0x2000_0000;

/// Memory operations needed by the reference interpreter.
///
/// Keeping this interface small makes it possible to run CPU unit tests with a
/// deterministic in-memory bus while production callers pass [`GuestMemory`]
/// directly.
pub trait MemoryBus {
    /// Fetches one big-endian instruction word from executable memory.
    fn fetch_u32(&mut self, address: GuestAddress) -> Result<u32, MemoryFault>;
    /// Reads one byte from guest memory.
    fn read_u8(&mut self, address: GuestAddress) -> Result<u8, MemoryFault>;
    /// Reads one big-endian 16-bit value from guest memory.
    fn read_u16(&mut self, address: GuestAddress) -> Result<u16, MemoryFault>;
    /// Reads one big-endian 32-bit value from guest memory.
    fn read_u32(&mut self, address: GuestAddress) -> Result<u32, MemoryFault>;
    /// Writes one byte to guest memory.
    fn write_u8(&mut self, address: GuestAddress, value: u8) -> Result<(), MemoryFault>;
    /// Writes one big-endian 16-bit value to guest memory.
    fn write_u16(&mut self, address: GuestAddress, value: u16) -> Result<(), MemoryFault>;
    /// Writes one big-endian 32-bit value to guest memory.
    fn write_u32(&mut self, address: GuestAddress, value: u32) -> Result<(), MemoryFault>;
}

impl MemoryBus for GuestMemory {
    fn fetch_u32(&mut self, address: GuestAddress) -> Result<u32, MemoryFault> {
        GuestMemory::fetch_u32(self, address)
    }

    fn read_u8(&mut self, address: GuestAddress) -> Result<u8, MemoryFault> {
        GuestMemory::read_u8(self, address)
    }

    fn read_u16(&mut self, address: GuestAddress) -> Result<u16, MemoryFault> {
        GuestMemory::read_u16(self, address)
    }

    fn read_u32(&mut self, address: GuestAddress) -> Result<u32, MemoryFault> {
        GuestMemory::read_u32(self, address)
    }

    fn write_u8(&mut self, address: GuestAddress, value: u8) -> Result<(), MemoryFault> {
        GuestMemory::write_u8(self, address, value)
    }

    fn write_u16(&mut self, address: GuestAddress, value: u16) -> Result<(), MemoryFault> {
        GuestMemory::write_u16(self, address, value)
    }

    fn write_u32(&mut self, address: GuestAddress, value: u32) -> Result<(), MemoryFault> {
        GuestMemory::write_u32(self, address, value)
    }
}

/// Kind of memory operation that caused a CPU fault.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FaultAccess {
    /// Instruction fetch from executable memory.
    InstructionFetch,
    /// Read from guest data memory.
    DataRead,
    /// Write to guest data memory.
    DataWrite,
}

/// Host-visible failure encountered while executing guest code.
#[derive(Debug, Eq, PartialEq)]
pub enum CpuFault {
    /// The backing guest-memory implementation rejected an access.
    Memory {
        /// Address of the instruction being executed.
        instruction_pointer: GuestAddress,
        /// Guest memory address that could not be accessed.
        address: GuestAddress,
        /// Kind of memory operation that failed.
        access: FaultAccess,
        /// Detailed fault reported by the memory implementation.
        source: MemoryFault,
    },
    /// An instruction or memory operand did not meet its alignment requirement.
    UnalignedAccess {
        /// Address of the instruction being executed.
        instruction_pointer: GuestAddress,
        /// Misaligned guest address.
        address: GuestAddress,
        /// Required access width in bytes.
        width: u8,
        /// Kind of access requiring alignment.
        access: FaultAccess,
    },
    /// The fetched instruction could not be decoded.
    Decode {
        /// Address containing the rejected instruction.
        instruction_pointer: GuestAddress,
        /// Detailed decoder error.
        source: DecodeError,
    },
    /// A deterministic execution counter was already at its maximum value.
    CounterOverflow {
        /// Address of the instruction that could not be retired.
        instruction_pointer: GuestAddress,
        /// Counter that could not be incremented.
        counter: BudgetKind,
    },
}

impl fmt::Display for CpuFault {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Memory {
                instruction_pointer,
                address,
                access,
                source,
            } => write!(
                formatter,
                "memory {access:?} fault at 0x{:08x} while executing 0x{:08x}: {source}",
                address.get(),
                instruction_pointer.get()
            ),
            Self::UnalignedAccess {
                instruction_pointer,
                address,
                width,
                access,
            } => write!(
                formatter,
                "unaligned {width}-byte {access:?} at 0x{:08x} while executing 0x{:08x}",
                address.get(),
                instruction_pointer.get()
            ),
            Self::Decode {
                instruction_pointer,
                source,
            } => write!(
                formatter,
                "decode fault at 0x{:08x}: {source}",
                instruction_pointer.get()
            ),
            Self::CounterOverflow {
                instruction_pointer,
                counter,
            } => write!(
                formatter,
                "{counter:?} counter exhausted before instruction at 0x{:08x}",
                instruction_pointer.get()
            ),
        }
    }
}

impl std::error::Error for CpuFault {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Memory { source, .. } => Some(source),
            Self::Decode { source, .. } => Some(source),
            Self::UnalignedAccess { .. } | Self::CounterOverflow { .. } => None,
        }
    }
}

/// Reason the interpreter stopped without a host execution fault.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StopReason {
    /// The synthetic test-completion sentinel was executed.
    StopSentinel,
    /// A PowerPC system-call instruction was executed.
    SystemCall {
        /// System-call number read from general-purpose register zero.
        number: u32,
    },
    /// A configured deterministic execution limit was reached.
    BudgetExhausted {
        /// Limit that stopped execution.
        kind: BudgetKind,
    },
}

/// Deterministic counter used to limit interpreter execution.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BudgetKind {
    /// Number of instructions executed by the current run.
    Instructions,
    /// Number of deterministic guest cycles elapsed during the current run.
    Cycles,
}

/// Result of executing exactly one instruction.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StepOutcome {
    /// Execution may continue at the updated instruction pointer.
    Continue,
    /// The instruction completed and requested a clean stop.
    Stopped(StopReason),
}

/// Upper bounds for one interpreter run.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ExecutionBudget {
    /// Maximum instructions to execute before stopping.
    pub max_instructions: u64,
    /// Maximum deterministic guest cycles to consume before stopping.
    pub max_cycles: u64,
}

impl ExecutionBudget {
    /// Creates a budget with independent instruction and cycle limits.
    #[must_use]
    pub const fn new(max_instructions: u64, max_cycles: u64) -> Self {
        Self {
            max_instructions,
            max_cycles,
        }
    }

    /// Creates an instruction-only budget with no practical cycle limit.
    #[must_use]
    pub const fn instructions(max_instructions: u64) -> Self {
        Self::new(max_instructions, u64::MAX)
    }
}

/// Summary of a cleanly stopped interpreter run.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ExecutionOutcome {
    /// Reason execution stopped.
    pub reason: StopReason,
    /// Instructions executed during this run, including a stopping instruction.
    pub instructions_executed: u64,
    /// Deterministic guest cycles consumed during this run.
    pub cycles_elapsed: u64,
}

struct InstructionExecution {
    next: GuestAddress,
    outcome: StepOutcome,
}

impl InstructionExecution {
    const fn new(next: GuestAddress) -> Self {
        Self {
            next,
            outcome: StepOutcome::Continue,
        }
    }
}

/// Deterministic reference PowerPC interpreter and its architectural state.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Cpu {
    state: CpuState,
}

impl Cpu {
    /// Creates a CPU with zero-initialised state at `entry`.
    #[must_use]
    pub fn new(entry: GuestAddress) -> Self {
        Self::with_state(CpuState::new(entry))
    }

    /// Creates a CPU from an existing architectural state snapshot.
    #[must_use]
    pub const fn with_state(state: CpuState) -> Self {
        Self { state }
    }

    /// Borrows the current architectural state.
    #[must_use]
    pub const fn state(&self) -> &CpuState {
        &self.state
    }

    /// Mutably borrows the current architectural state.
    pub fn state_mut(&mut self) -> &mut CpuState {
        &mut self.state
    }

    /// Consumes the interpreter and returns its architectural state.
    pub fn into_state(self) -> CpuState {
        self.state
    }

    /// Fetches, decodes, and executes one guest instruction.
    pub fn step(&mut self, memory: &mut impl MemoryBus) -> Result<StepOutcome, CpuFault> {
        let instruction_pointer = self.state.instruction_pointer;
        let instruction = self.fetch_instruction(memory, instruction_pointer)?;
        let sequential = GuestAddress::new(instruction_pointer.get().wrapping_add(4));
        let execution =
            self.execute_instruction(memory, instruction_pointer, sequential, instruction)?;

        self.state.instruction_pointer = execution.next;
        self.state.retire_instruction();
        Ok(execution.outcome)
    }

    fn fetch_instruction(
        &mut self,
        memory: &mut impl MemoryBus,
        instruction_pointer: GuestAddress,
    ) -> Result<Instruction, CpuFault> {
        if !instruction_pointer.is_aligned(4) {
            self.record_fault(
                instruction_pointer,
                Some(instruction_pointer),
                FaultKind::Alignment,
            );
            return Err(CpuFault::UnalignedAccess {
                instruction_pointer,
                address: instruction_pointer,
                width: 4,
                access: FaultAccess::InstructionFetch,
            });
        }
        self.ensure_counter_capacity(instruction_pointer)?;
        let raw = memory.fetch_u32(instruction_pointer).map_err(|source| {
            self.record_fault(
                instruction_pointer,
                Some(instruction_pointer),
                FaultKind::InstructionFetch,
            );
            CpuFault::Memory {
                instruction_pointer,
                address: instruction_pointer,
                access: FaultAccess::InstructionFetch,
                source,
            }
        })?;
        decode(raw).map_err(|source| {
            self.record_fault(
                instruction_pointer,
                Some(instruction_pointer),
                FaultKind::UnsupportedInstruction,
            );
            CpuFault::Decode {
                instruction_pointer,
                source,
            }
        })
    }

    fn execute_instruction(
        &mut self,
        memory: &mut impl MemoryBus,
        instruction_pointer: GuestAddress,
        sequential: GuestAddress,
        instruction: Instruction,
    ) -> Result<InstructionExecution, CpuFault> {
        let mut execution = InstructionExecution::new(sequential);
        match instruction {
            Instruction::Stop => {
                execution.outcome = StepOutcome::Stopped(StopReason::StopSentinel);
            }
            Instruction::SystemCall => {
                execution.outcome = StepOutcome::Stopped(StopReason::SystemCall {
                    number: self.state.gpr_unchecked(0),
                });
            }
            instruction @ (Instruction::AddImmediate { .. }
            | Instruction::MultiplyLowImmediate { .. }
            | Instruction::SubtractFromImmediateCarrying { .. }
            | Instruction::AddImmediateCarrying { .. }
            | Instruction::LogicalImmediate { .. }
            | Instruction::CompareImmediate { .. }) => self.execute_immediate(instruction),
            instruction @ (Instruction::ArithmeticRegister { .. }
            | Instruction::LogicalRegister { .. }
            | Instruction::CompareRegister { .. }) => self.execute_register(instruction),
            instruction @ (Instruction::Load { .. } | Instruction::Store { .. }) => {
                self.execute_memory(memory, instruction_pointer, instruction)?;
            }
            instruction @ (Instruction::Branch { .. }
            | Instruction::BranchConditional { .. }
            | Instruction::BranchToLinkRegister { .. }
            | Instruction::BranchToCountRegister { .. }) => {
                execution.next = self.execute_branch(instruction_pointer, sequential, instruction);
            }
        }
        Ok(execution)
    }

    fn execute_immediate(&mut self, instruction: Instruction) {
        match instruction {
            Instruction::AddImmediate {
                destination,
                base,
                immediate,
                shifted,
            } => {
                let left = if base == 0 {
                    0
                } else {
                    self.state.gpr_unchecked(base)
                };
                let mut right = sign_extend_i16(immediate);
                if shifted {
                    right = right.wrapping_shl(16);
                }
                self.state
                    .set_gpr_unchecked(destination, left.wrapping_add(right));
            }
            Instruction::MultiplyLowImmediate {
                destination,
                left,
                immediate,
            } => {
                let value = self
                    .state
                    .gpr_unchecked(left)
                    .wrapping_mul(sign_extend_i16(immediate));
                self.state.set_gpr_unchecked(destination, value);
            }
            Instruction::SubtractFromImmediateCarrying {
                destination,
                left,
                immediate,
            } => {
                let left = self.state.gpr_unchecked(left);
                let right = sign_extend_i16(immediate);
                let (partial, carry_one) = (!left).overflowing_add(right);
                let (value, carry_two) = partial.overflowing_add(1);
                self.set_carry(carry_one || carry_two);
                self.state.set_gpr_unchecked(destination, value);
            }
            Instruction::AddImmediateCarrying {
                destination,
                left,
                immediate,
                record,
            } => {
                let (value, carry) = self
                    .state
                    .gpr_unchecked(left)
                    .overflowing_add(sign_extend_i16(immediate));
                self.set_carry(carry);
                self.state.set_gpr_unchecked(destination, value);
                if record {
                    self.state.set_cr0_from_result(value);
                }
            }
            Instruction::LogicalImmediate {
                operation,
                destination,
                source,
                immediate,
                shifted,
                record,
            } => {
                let mut right = u32::from(immediate);
                if shifted {
                    right <<= 16;
                }
                let value = logical(operation, self.state.gpr_unchecked(source), right);
                self.state.set_gpr_unchecked(destination, value);
                if record {
                    self.state.set_cr0_from_result(value);
                }
            }
            Instruction::CompareImmediate {
                field,
                signed,
                left,
                immediate,
            } => {
                let left = self.state.gpr_unchecked(left);
                let right = if signed {
                    sign_extend_u16(immediate)
                } else {
                    u32::from(immediate)
                };
                self.compare(field, signed, left, right);
            }
            _ => unreachable!("non-immediate instruction routed to immediate executor"),
        }
    }

    fn execute_register(&mut self, instruction: Instruction) {
        match instruction {
            Instruction::ArithmeticRegister {
                operation,
                destination,
                left,
                right,
                record,
            } => {
                let left = self.state.gpr_unchecked(left);
                let right = self.state.gpr_unchecked(right);
                let value = match operation {
                    ArithmeticOperation::Add => left.wrapping_add(right),
                    ArithmeticOperation::SubtractFrom => right.wrapping_sub(left),
                };
                self.state.set_gpr_unchecked(destination, value);
                if record {
                    self.state.set_cr0_from_result(value);
                }
            }
            Instruction::LogicalRegister {
                operation,
                destination,
                left,
                right,
                record,
            } => {
                let value = logical(
                    operation,
                    self.state.gpr_unchecked(left),
                    self.state.gpr_unchecked(right),
                );
                self.state.set_gpr_unchecked(destination, value);
                if record {
                    self.state.set_cr0_from_result(value);
                }
            }
            Instruction::CompareRegister {
                field,
                signed,
                left,
                right,
            } => self.compare(
                field,
                signed,
                self.state.gpr_unchecked(left),
                self.state.gpr_unchecked(right),
            ),
            _ => unreachable!("non-register instruction routed to register executor"),
        }
    }

    fn execute_memory(
        &mut self,
        memory: &mut impl MemoryBus,
        instruction_pointer: GuestAddress,
        instruction: Instruction,
    ) -> Result<(), CpuFault> {
        match instruction {
            Instruction::Load {
                width,
                signed,
                destination,
                base,
                displacement,
                update,
            } => {
                let address = self.effective_address(base, displacement);
                self.require_alignment(instruction_pointer, address, width, FaultAccess::DataRead)?;
                let value = self.load(memory, instruction_pointer, address, width, signed)?;
                self.state.set_gpr_unchecked(destination, value);
                if update {
                    self.state.set_gpr_unchecked(base, address.get());
                }
            }
            Instruction::Store {
                width,
                source,
                base,
                displacement,
                update,
            } => {
                let address = self.effective_address(base, displacement);
                self.require_alignment(
                    instruction_pointer,
                    address,
                    width,
                    FaultAccess::DataWrite,
                )?;
                self.store(
                    memory,
                    instruction_pointer,
                    address,
                    width,
                    self.state.gpr_unchecked(source),
                )?;
                if update {
                    self.state.set_gpr_unchecked(base, address.get());
                }
            }
            _ => unreachable!("non-memory instruction routed to memory executor"),
        }
        Ok(())
    }

    fn execute_branch(
        &mut self,
        instruction_pointer: GuestAddress,
        sequential: GuestAddress,
        instruction: Instruction,
    ) -> GuestAddress {
        let mut next = sequential;
        match instruction {
            Instruction::Branch {
                displacement,
                absolute,
                link,
            } => {
                if link {
                    self.state.link_register = sequential.get();
                }
                next = branch_target(instruction_pointer, displacement, absolute);
            }
            Instruction::BranchConditional {
                branch_options,
                condition_bit,
                displacement,
                absolute,
                link,
            } => {
                if link {
                    self.state.link_register = sequential.get();
                }
                if self.branch_condition(branch_options, condition_bit) {
                    next = branch_target(instruction_pointer, displacement, absolute);
                }
            }
            Instruction::BranchToLinkRegister {
                branch_options,
                condition_bit,
                link,
            } => {
                let target = self.state.link_register & !3;
                if link {
                    self.state.link_register = sequential.get();
                }
                if self.branch_condition(branch_options, condition_bit) {
                    next = GuestAddress::new(target);
                }
            }
            Instruction::BranchToCountRegister {
                branch_options,
                condition_bit,
                link,
            } => {
                let target = self.state.count_register & !3;
                if link {
                    self.state.link_register = sequential.get();
                }
                if self.branch_condition(branch_options, condition_bit) {
                    next = GuestAddress::new(target);
                }
            }
            _ => unreachable!("non-branch instruction routed to branch executor"),
        }
        next
    }

    /// Runs until a clean stop or `max_instructions` have been executed.
    pub fn run(
        &mut self,
        memory: &mut impl MemoryBus,
        max_instructions: u64,
    ) -> Result<ExecutionOutcome, CpuFault> {
        self.run_with_budget(memory, ExecutionBudget::instructions(max_instructions))
    }

    /// Runs until a clean stop or either deterministic budget is exhausted.
    pub fn run_with_budget(
        &mut self,
        memory: &mut impl MemoryBus,
        budget: ExecutionBudget,
    ) -> Result<ExecutionOutcome, CpuFault> {
        let mut instructions = 0_u64;
        let mut cycles = 0_u64;
        loop {
            if instructions >= budget.max_instructions {
                return Ok(ExecutionOutcome {
                    reason: StopReason::BudgetExhausted {
                        kind: BudgetKind::Instructions,
                    },
                    instructions_executed: instructions,
                    cycles_elapsed: cycles,
                });
            }
            if cycles >= budget.max_cycles {
                return Ok(ExecutionOutcome {
                    reason: StopReason::BudgetExhausted {
                        kind: BudgetKind::Cycles,
                    },
                    instructions_executed: instructions,
                    cycles_elapsed: cycles,
                });
            }

            let step_outcome = self.step(memory)?;
            instructions += 1;
            cycles += 1;
            if let StepOutcome::Stopped(reason) = step_outcome {
                return Ok(ExecutionOutcome {
                    reason,
                    instructions_executed: instructions,
                    cycles_elapsed: cycles,
                });
            }
        }
    }

    fn compare(&mut self, field: u8, signed: bool, left: u32, right: u32) {
        let nibble = if signed {
            match left.cast_signed().cmp(&right.cast_signed()) {
                std::cmp::Ordering::Less => 0b1000,
                std::cmp::Ordering::Greater => 0b0100,
                std::cmp::Ordering::Equal => 0b0010,
            }
        } else {
            match left.cmp(&right) {
                std::cmp::Ordering::Less => 0b1000,
                std::cmp::Ordering::Greater => 0b0100,
                std::cmp::Ordering::Equal => 0b0010,
            }
        } | self.state.summary_overflow_bit();
        self.state.set_condition_field(field, nibble);
    }

    fn branch_condition(&mut self, branch_options: u8, condition_bit: u8) -> bool {
        let count_ok = if branch_options & 0b00100 != 0 {
            true
        } else {
            self.state.count_register = self.state.count_register.wrapping_sub(1);
            let nonzero = self.state.count_register != 0;
            nonzero ^ (branch_options & 0b00010 != 0)
        };
        let condition_ok = branch_options & 0b10000 != 0
            || self.state.condition_bit_unchecked(condition_bit) == (branch_options & 0b01000 != 0);
        count_ok && condition_ok
    }

    fn effective_address(&self, base: u8, displacement: i16) -> GuestAddress {
        let base = if base == 0 {
            0
        } else {
            self.state.gpr_unchecked(base)
        };
        GuestAddress::new(base.wrapping_add(sign_extend_i16(displacement)))
    }

    fn require_alignment(
        &mut self,
        instruction_pointer: GuestAddress,
        address: GuestAddress,
        width: LoadWidth,
        access: FaultAccess,
    ) -> Result<(), CpuFault> {
        let width = width.bytes();
        if address.get().is_multiple_of(u32::from(width)) {
            return Ok(());
        }
        self.record_fault(instruction_pointer, Some(address), FaultKind::Alignment);
        Err(CpuFault::UnalignedAccess {
            instruction_pointer,
            address,
            width,
            access,
        })
    }

    fn load(
        &mut self,
        memory: &mut impl MemoryBus,
        instruction_pointer: GuestAddress,
        address: GuestAddress,
        width: LoadWidth,
        signed: bool,
    ) -> Result<u32, CpuFault> {
        let result = match width {
            LoadWidth::Byte => memory.read_u8(address).map(u32::from),
            LoadWidth::HalfWord => memory.read_u16(address).map(|value| {
                if signed {
                    sign_extend_u16(value)
                } else {
                    u32::from(value)
                }
            }),
            LoadWidth::Word => memory.read_u32(address),
        };
        result.map_err(|source| {
            self.record_fault(instruction_pointer, Some(address), FaultKind::DataRead);
            CpuFault::Memory {
                instruction_pointer,
                address,
                access: FaultAccess::DataRead,
                source,
            }
        })
    }

    fn store(
        &mut self,
        memory: &mut impl MemoryBus,
        instruction_pointer: GuestAddress,
        address: GuestAddress,
        width: LoadWidth,
        value: u32,
    ) -> Result<(), CpuFault> {
        let result = match width {
            LoadWidth::Byte => memory.write_u8(address, truncate_u32_to_u8(value)),
            LoadWidth::HalfWord => memory.write_u16(address, truncate_u32_to_u16(value)),
            LoadWidth::Word => memory.write_u32(address, value),
        };
        result.map_err(|source| {
            self.record_fault(instruction_pointer, Some(address), FaultKind::DataWrite);
            CpuFault::Memory {
                instruction_pointer,
                address,
                access: FaultAccess::DataWrite,
                source,
            }
        })
    }

    fn set_carry(&mut self, carry: bool) {
        if carry {
            self.state.xer |= XER_CA;
        } else {
            self.state.xer &= !XER_CA;
        }
    }

    fn ensure_counter_capacity(
        &mut self,
        instruction_pointer: GuestAddress,
    ) -> Result<(), CpuFault> {
        let counter = if self.state.instructions_retired == u64::MAX {
            Some(BudgetKind::Instructions)
        } else if self.state.cycles.get() == u64::MAX {
            Some(BudgetKind::Cycles)
        } else {
            None
        };
        if let Some(counter) = counter {
            self.record_fault(instruction_pointer, None, FaultKind::CounterOverflow);
            Err(CpuFault::CounterOverflow {
                instruction_pointer,
                counter,
            })
        } else {
            Ok(())
        }
    }

    fn record_fault(
        &mut self,
        instruction_pointer: GuestAddress,
        address: Option<GuestAddress>,
        kind: FaultKind,
    ) {
        self.state.pending_fault = Some(ArchitecturalFault {
            instruction_pointer,
            address,
            kind,
        });
    }
}

fn logical(operation: LogicalOperation, left: u32, right: u32) -> u32 {
    match operation {
        LogicalOperation::And => left & right,
        LogicalOperation::Or => left | right,
        LogicalOperation::Xor => left ^ right,
        LogicalOperation::Nor => !(left | right),
    }
}

fn branch_target(current: GuestAddress, displacement: i32, absolute: bool) -> GuestAddress {
    let base = if absolute { 0 } else { current.get() };
    GuestAddress::new(base.wrapping_add(displacement.cast_unsigned()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::SYSTEM_CALL_INSTRUCTION;
    use cex_memory::Permissions;

    const CODE: GuestAddress = GuestAddress::new(0x1000);

    fn memory_with_words(words: &[u32]) -> GuestMemory {
        let mut memory = GuestMemory::new();
        memory
            .map(
                CODE,
                0x1000,
                Permissions::READ | Permissions::WRITE | Permissions::EXECUTE,
            )
            .unwrap();
        for (index, word) in words.iter().copied().enumerate() {
            memory
                .write_u32(
                    GuestAddress::new(CODE.get() + u32::try_from(index * 4).unwrap()),
                    word,
                )
                .unwrap();
        }
        memory
    }

    #[test]
    fn executes_the_first_synthetic_program() {
        let mut memory = memory_with_words(&[0x3860_0028, 0x3863_0002, 0]);
        let mut cpu = Cpu::new(CODE);

        let outcome = cpu.run(&mut memory, 16).unwrap();

        assert_eq!(outcome.reason, StopReason::StopSentinel);
        assert_eq!(outcome.instructions_executed, 3);
        assert_eq!(cpu.state().gpr(3), Some(42));
        assert_eq!(cpu.state().instruction_pointer, GuestAddress::new(0x100c));
    }

    #[test]
    fn load_store_are_big_endian_and_alignment_is_strict() {
        // stw r3,12(r4); lhz r5,12(r4); stop
        let mut memory = memory_with_words(&[0x9064_000c, 0xa0a4_000c, 0]);
        let data = GuestAddress::new(0x2000);
        memory
            .map(data, 0x1000, Permissions::READ | Permissions::WRITE)
            .unwrap();
        let mut cpu = Cpu::new(CODE);
        cpu.state_mut().set_gpr(3, 0x1234_abcd).unwrap();
        cpu.state_mut().set_gpr(4, data.get()).unwrap();

        cpu.run(&mut memory, 8).unwrap();

        assert_eq!(
            memory.read_u32(GuestAddress::new(0x200c)).unwrap(),
            0x1234_abcd
        );
        assert_eq!(cpu.state().gpr(5), Some(0x1234));

        cpu.state_mut().instruction_pointer = CODE;
        cpu.state_mut().set_gpr(4, data.get() + 1).unwrap();
        let fault = cpu.step(&mut memory).unwrap_err();
        assert!(matches!(fault, CpuFault::UnalignedAccess { width: 4, .. }));
    }

    #[test]
    fn subword_stores_truncate_and_signed_halfword_loads_extend() {
        // stb r3,0(r4); sth r3,2(r4); lha r5,2(r4); stop
        let mut memory = memory_with_words(&[0x9864_0000, 0xb064_0002, 0xa8a4_0002, 0]);
        let data = GuestAddress::new(0x2000);
        memory
            .map(data, 0x1000, Permissions::READ | Permissions::WRITE)
            .unwrap();
        let mut cpu = Cpu::new(CODE);
        cpu.state_mut().set_gpr(3, 0x1234_abcd).unwrap();
        cpu.state_mut().set_gpr(4, data.get()).unwrap();

        cpu.run(&mut memory, 8).unwrap();

        assert_eq!(memory.read_u8(data).unwrap(), 0xcd);
        assert_eq!(
            memory.read_u16(GuestAddress::new(data.get() + 2)).unwrap(),
            0xabcd
        );
        assert_eq!(cpu.state().gpr(5), Some(0xffff_abcd));
    }

    #[test]
    fn compare_and_conditional_branch_use_cr_bit_numbering() {
        // cmpwi cr0,r3,42; beq +8; addi r4,r0,1; addi r4,r0,2; stop
        let mut memory =
            memory_with_words(&[0x2c03_002a, 0x4182_0008, 0x3880_0001, 0x3880_0002, 0]);
        let mut cpu = Cpu::new(CODE);
        cpu.state_mut().set_gpr(3, 42).unwrap();

        cpu.run(&mut memory, 10).unwrap();

        assert_eq!(cpu.state().gpr(4), Some(2));
        assert_eq!(cpu.state().condition_bit(2), Some(true));
    }

    #[test]
    fn branch_loop_stops_at_the_instruction_budget() {
        let mut memory = memory_with_words(&[0x4800_0000]);
        let mut cpu = Cpu::new(CODE);

        let outcome = cpu.run(&mut memory, 5).unwrap();

        assert_eq!(
            outcome.reason,
            StopReason::BudgetExhausted {
                kind: BudgetKind::Instructions,
            }
        );
        assert_eq!(outcome.instructions_executed, 5);
        assert_eq!(cpu.state().instruction_pointer, CODE);
    }

    #[test]
    fn unsupported_instruction_fault_does_not_advance_state() {
        let mut memory = memory_with_words(&[0xffff_ffff]);
        let mut cpu = Cpu::new(CODE);

        let fault = cpu.step(&mut memory).unwrap_err();

        assert!(matches!(fault, CpuFault::Decode { .. }));
        assert_eq!(cpu.state().instruction_pointer, CODE);
        assert_eq!(cpu.state().instructions_retired, 0);
        assert_eq!(
            cpu.state().pending_fault.unwrap().kind,
            FaultKind::UnsupportedInstruction
        );
    }

    #[test]
    fn system_call_exposes_r0_and_advances_deterministically() {
        let mut memory = memory_with_words(&[SYSTEM_CALL_INSTRUCTION]);
        let mut cpu = Cpu::new(CODE);
        cpu.state_mut().set_gpr(0, 0x42).unwrap();

        assert_eq!(
            cpu.step(&mut memory).unwrap(),
            StepOutcome::Stopped(StopReason::SystemCall { number: 0x42 })
        );
        assert_eq!(cpu.state().instruction_pointer, GuestAddress::new(0x1004));
        assert_eq!(cpu.state().instructions_retired, 1);
    }

    #[test]
    fn exhausted_counters_fault_before_mutating_architectural_state() {
        let mut memory = memory_with_words(&[0x3860_0001]);
        let mut state = CpuState::new(CODE);
        state.instructions_retired = u64::MAX;
        let mut cpu = Cpu::with_state(state);

        assert_eq!(
            cpu.step(&mut memory).unwrap_err(),
            CpuFault::CounterOverflow {
                instruction_pointer: CODE,
                counter: BudgetKind::Instructions,
            }
        );
        assert_eq!(cpu.state().gpr(3), Some(0));
        assert_eq!(cpu.state().instruction_pointer, CODE);
    }

    #[test]
    fn unaligned_instruction_pointer_faults_before_fetch_or_retirement() {
        let mut memory = memory_with_words(&[0, 0]);
        let entry = GuestAddress::new(CODE.get() + 2);
        let mut cpu = Cpu::new(entry);

        assert_eq!(
            cpu.step(&mut memory).unwrap_err(),
            CpuFault::UnalignedAccess {
                instruction_pointer: entry,
                address: entry,
                width: 4,
                access: FaultAccess::InstructionFetch,
            }
        );
        assert_eq!(cpu.state().instruction_pointer, entry);
        assert_eq!(cpu.state().instructions_retired, 0);
        assert_eq!(cpu.state().cycles.get(), 0);
        assert_eq!(
            cpu.state().pending_fault,
            Some(ArchitecturalFault {
                instruction_pointer: entry,
                address: Some(entry),
                kind: FaultKind::Alignment,
            })
        );
    }
}
