use core::fmt;

use cex_memory::MemoryFault;
use cex_types::GuestAddress;

use crate::integer::{sign_extend_u16, truncate_u32_to_u8, truncate_u32_to_u16};
use crate::{CpuState, LoadWidth, MemoryBus};

/// Register operand addressable by the intermediate machine language (IML).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ImlRegister {
    /// Architectural general-purpose register by index.
    Gpr(u8),
    /// Block-local temporary register by index.
    Temporary(u16),
}

/// Value consumed by an IML instruction.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ImlOperand {
    /// Value read from a register.
    Register(ImlRegister),
    /// Constant 32-bit value.
    Immediate(u32),
}

/// Comparison operation available to IML control-flow instructions.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ImlCondition {
    /// Values are equal.
    Equal,
    /// Values are not equal.
    NotEqual,
    /// The signed left value is less than the signed right value.
    SignedLessThan,
    /// The signed left value is greater than the signed right value.
    SignedGreaterThan,
    /// The unsigned left value is less than the unsigned right value.
    UnsignedLessThan,
    /// The unsigned left value is greater than the unsigned right value.
    UnsignedGreaterThan,
}

/// One operation in the portable intermediate machine language.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ImlInstruction {
    /// Copies a value into a register.
    Move {
        /// Register receiving the value.
        destination: ImlRegister,
        /// Value to copy.
        value: ImlOperand,
    },
    /// Adds two operands with 32-bit wrapping semantics.
    Add {
        /// Register receiving the result.
        destination: ImlRegister,
        /// Left operand.
        left: ImlOperand,
        /// Right operand.
        right: ImlOperand,
    },
    /// Subtracts the right operand from the left with 32-bit wrapping semantics.
    Subtract {
        /// Register receiving the result.
        destination: ImlRegister,
        /// Left operand.
        left: ImlOperand,
        /// Right operand.
        right: ImlOperand,
    },
    /// Computes the bitwise AND of two operands.
    And {
        /// Register receiving the result.
        destination: ImlRegister,
        /// Left operand.
        left: ImlOperand,
        /// Right operand.
        right: ImlOperand,
    },
    /// Computes the bitwise OR of two operands.
    Or {
        /// Register receiving the result.
        destination: ImlRegister,
        /// Left operand.
        left: ImlOperand,
        /// Right operand.
        right: ImlOperand,
    },
    /// Computes the bitwise exclusive OR of two operands.
    Xor {
        /// Register receiving the result.
        destination: ImlRegister,
        /// Left operand.
        left: ImlOperand,
        /// Right operand.
        right: ImlOperand,
    },
    /// Loads an integer from guest memory.
    Load {
        /// Register receiving the loaded value.
        destination: ImlRegister,
        /// Guest byte address to read.
        address: ImlOperand,
        /// Width of the memory access.
        width: LoadWidth,
        /// Whether a sub-word value is sign-extended.
        signed: bool,
    },
    /// Stores an integer to guest memory.
    Store {
        /// Guest byte address to write.
        address: ImlOperand,
        /// Value to store; excess high bits are discarded for sub-word stores.
        value: ImlOperand,
        /// Width of the memory access.
        width: LoadWidth,
    },
    /// Writes one four-bit condition-register field from a comparison.
    SetConditionField {
        /// Condition-register field index in `0..8`.
        field: u8,
        /// Comparison to evaluate.
        condition: ImlCondition,
        /// Left comparison operand.
        left: ImlOperand,
        /// Right comparison operand.
        right: ImlOperand,
    },
    /// Unconditionally exits the block at a guest target address.
    Branch {
        /// Guest address selected as the next instruction pointer.
        target: GuestAddress,
    },
    /// Exits the block at a guest target address when a comparison succeeds.
    BranchIf {
        /// Comparison to evaluate.
        condition: ImlCondition,
        /// Left comparison operand.
        left: ImlOperand,
        /// Right comparison operand.
        right: ImlOperand,
        /// Guest address selected when the comparison succeeds.
        target: GuestAddress,
    },
    /// Exits the block with a system-call outcome.
    SystemCall,
    /// Exits the block with a synthetic stop outcome.
    Stop,
}

/// Straight-line sequence of IML instructions translated from guest code.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ImlBlock {
    /// Guest address of the first translated instruction.
    pub guest_start: GuestAddress,
    /// Number of block-local temporary registers required during evaluation.
    pub temporary_count: u16,
    /// Operations evaluated in source order.
    pub instructions: Vec<ImlInstruction>,
}

impl ImlBlock {
    /// Creates an IML block and declares its required temporary-register count.
    #[must_use]
    pub fn new(
        guest_start: GuestAddress,
        temporary_count: u16,
        instructions: Vec<ImlInstruction>,
    ) -> Self {
        Self {
            guest_start,
            temporary_count,
            instructions,
        }
    }
}

/// Control-flow result produced after evaluating an IML block.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ImlOutcome {
    /// All instructions completed without exiting the block.
    Completed,
    /// Evaluation selected a new guest instruction pointer.
    Branch(GuestAddress),
    /// Evaluation reached a guest system call.
    SystemCall {
        /// System-call number read from general-purpose register zero.
        number: u32,
    },
    /// Evaluation reached the synthetic stop operation.
    Stop,
}

/// Deterministic failure encountered by the portable IML evaluator.
#[derive(Debug, Eq, PartialEq)]
pub enum ImlEvaluationError {
    /// A general-purpose register index was outside `0..32`.
    InvalidGeneralPurposeRegister(u8),
    /// A temporary-register index exceeded the block's declared count.
    InvalidTemporary(u16),
    /// A condition-register field index was outside `0..8`.
    InvalidConditionField(u8),
    /// A guest memory address did not meet the access-width alignment.
    UnalignedAddress {
        /// Misaligned guest address.
        address: GuestAddress,
        /// Required access width in bytes.
        width: u8,
    },
    /// The backing guest-memory implementation rejected an access.
    Memory {
        /// Guest address that could not be accessed.
        address: GuestAddress,
        /// Detailed fault reported by the memory implementation.
        source: MemoryFault,
    },
}

impl fmt::Display for ImlEvaluationError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidGeneralPurposeRegister(register) => {
                write!(formatter, "invalid IML GPR r{register}")
            }
            Self::InvalidTemporary(temporary) => {
                write!(formatter, "invalid IML temporary t{temporary}")
            }
            Self::InvalidConditionField(field) => {
                write!(formatter, "invalid IML condition-register field cr{field}")
            }
            Self::UnalignedAddress { address, width } => write!(
                formatter,
                "unaligned {width}-byte IML memory access at 0x{:08x}",
                address.get()
            ),
            Self::Memory { address, source } => {
                write!(
                    formatter,
                    "IML memory fault at 0x{:08x}: {source}",
                    address.get()
                )
            }
        }
    }
}

impl std::error::Error for ImlEvaluationError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Memory { source, .. } => Some(source),
            _ => None,
        }
    }
}

/// A safe, deliberately literal IML evaluator used as an oracle while the
/// optimiser and platform code generators are brought up.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ImlEvaluator {
    temporaries: Vec<u32>,
}

impl ImlEvaluator {
    /// Creates an evaluator with no allocated temporary-register storage.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            temporaries: Vec::new(),
        }
    }

    /// Evaluates one block against architectural state and guest memory.
    pub fn evaluate(
        &mut self,
        block: &ImlBlock,
        state: &mut CpuState,
        memory: &mut impl MemoryBus,
    ) -> Result<ImlOutcome, ImlEvaluationError> {
        self.temporaries.clear();
        self.temporaries
            .resize(usize::from(block.temporary_count), 0);

        for instruction in &block.instructions {
            if let Some(outcome) = self.evaluate_instruction(instruction, state, memory)? {
                return Ok(outcome);
            }
        }
        Ok(ImlOutcome::Completed)
    }

    fn evaluate_instruction(
        &mut self,
        instruction: &ImlInstruction,
        state: &mut CpuState,
        memory: &mut impl MemoryBus,
    ) -> Result<Option<ImlOutcome>, ImlEvaluationError> {
        let outcome = match *instruction {
            ImlInstruction::Move { destination, value } => {
                let value = self.read_operand(state, value)?;
                self.write_register(state, destination, value)?;
                None
            }
            ImlInstruction::Add {
                destination,
                left,
                right,
            } => {
                self.evaluate_binary(state, destination, left, right, u32::wrapping_add)?;
                None
            }
            ImlInstruction::Subtract {
                destination,
                left,
                right,
            } => {
                self.evaluate_binary(state, destination, left, right, u32::wrapping_sub)?;
                None
            }
            ImlInstruction::And {
                destination,
                left,
                right,
            } => {
                self.evaluate_binary(state, destination, left, right, |left, right| left & right)?;
                None
            }
            ImlInstruction::Or {
                destination,
                left,
                right,
            } => {
                self.evaluate_binary(state, destination, left, right, |left, right| left | right)?;
                None
            }
            ImlInstruction::Xor {
                destination,
                left,
                right,
            } => {
                self.evaluate_binary(state, destination, left, right, |left, right| left ^ right)?;
                None
            }
            ImlInstruction::Load {
                destination,
                address,
                width,
                signed,
            } => {
                let address = GuestAddress::new(self.read_operand(state, address)?);
                let value = load(memory, address, width, signed)?;
                self.write_register(state, destination, value)?;
                None
            }
            ImlInstruction::Store {
                address,
                value,
                width,
            } => {
                let address = GuestAddress::new(self.read_operand(state, address)?);
                store(memory, address, width, self.read_operand(state, value)?)?;
                None
            }
            ImlInstruction::SetConditionField {
                field,
                condition,
                left,
                right,
            } => {
                self.set_condition_field(state, field, condition, left, right)?;
                None
            }
            ImlInstruction::Branch { target } => Some(ImlOutcome::Branch(target)),
            ImlInstruction::BranchIf {
                condition,
                left,
                right,
                target,
            } => compare(
                condition,
                self.read_operand(state, left)?,
                self.read_operand(state, right)?,
            )
            .then_some(ImlOutcome::Branch(target)),
            ImlInstruction::SystemCall => Some(ImlOutcome::SystemCall {
                number: state.gpr_unchecked(0),
            }),
            ImlInstruction::Stop => Some(ImlOutcome::Stop),
        };
        Ok(outcome)
    }

    fn evaluate_binary(
        &mut self,
        state: &mut CpuState,
        destination: ImlRegister,
        left: ImlOperand,
        right: ImlOperand,
        operation: fn(u32, u32) -> u32,
    ) -> Result<(), ImlEvaluationError> {
        let value = operation(
            self.read_operand(state, left)?,
            self.read_operand(state, right)?,
        );
        self.write_register(state, destination, value)
    }

    fn set_condition_field(
        &self,
        state: &mut CpuState,
        field: u8,
        condition: ImlCondition,
        left: ImlOperand,
        right: ImlOperand,
    ) -> Result<(), ImlEvaluationError> {
        if field >= 8 {
            return Err(ImlEvaluationError::InvalidConditionField(field));
        }
        let result = compare(
            condition,
            self.read_operand(state, left)?,
            self.read_operand(state, right)?,
        );
        let nibble = if result { 0b0010 } else { 0 };
        state.set_condition_field(field, nibble | state.summary_overflow_bit());
        Ok(())
    }

    fn read_operand(
        &self,
        state: &CpuState,
        operand: ImlOperand,
    ) -> Result<u32, ImlEvaluationError> {
        match operand {
            ImlOperand::Immediate(value) => Ok(value),
            ImlOperand::Register(register) => self.read_register(state, register),
        }
    }

    fn read_register(
        &self,
        state: &CpuState,
        register: ImlRegister,
    ) -> Result<u32, ImlEvaluationError> {
        match register {
            ImlRegister::Gpr(register) if register < 32 => Ok(state.gpr_unchecked(register)),
            ImlRegister::Gpr(register) => {
                Err(ImlEvaluationError::InvalidGeneralPurposeRegister(register))
            }
            ImlRegister::Temporary(temporary) => self
                .temporaries
                .get(usize::from(temporary))
                .copied()
                .ok_or(ImlEvaluationError::InvalidTemporary(temporary)),
        }
    }

    fn write_register(
        &mut self,
        state: &mut CpuState,
        register: ImlRegister,
        value: u32,
    ) -> Result<(), ImlEvaluationError> {
        match register {
            ImlRegister::Gpr(register) if register < 32 => {
                state.set_gpr_unchecked(register, value);
                Ok(())
            }
            ImlRegister::Gpr(register) => {
                Err(ImlEvaluationError::InvalidGeneralPurposeRegister(register))
            }
            ImlRegister::Temporary(temporary) => {
                let slot = self
                    .temporaries
                    .get_mut(usize::from(temporary))
                    .ok_or(ImlEvaluationError::InvalidTemporary(temporary))?;
                *slot = value;
                Ok(())
            }
        }
    }
}

fn check_alignment(address: GuestAddress, width: LoadWidth) -> Result<(), ImlEvaluationError> {
    let width = width.bytes();
    if address.get().is_multiple_of(u32::from(width)) {
        Ok(())
    } else {
        Err(ImlEvaluationError::UnalignedAddress { address, width })
    }
}

fn load(
    memory: &mut impl MemoryBus,
    address: GuestAddress,
    width: LoadWidth,
    signed: bool,
) -> Result<u32, ImlEvaluationError> {
    check_alignment(address, width)?;
    match width {
        LoadWidth::Byte => memory.read_u8(address).map(u32::from),
        LoadWidth::HalfWord => memory.read_u16(address).map(|value| {
            if signed {
                sign_extend_u16(value)
            } else {
                u32::from(value)
            }
        }),
        LoadWidth::Word => memory.read_u32(address),
    }
    .map_err(|source| ImlEvaluationError::Memory { address, source })
}

fn store(
    memory: &mut impl MemoryBus,
    address: GuestAddress,
    width: LoadWidth,
    value: u32,
) -> Result<(), ImlEvaluationError> {
    check_alignment(address, width)?;
    match width {
        LoadWidth::Byte => memory.write_u8(address, truncate_u32_to_u8(value)),
        LoadWidth::HalfWord => memory.write_u16(address, truncate_u32_to_u16(value)),
        LoadWidth::Word => memory.write_u32(address, value),
    }
    .map_err(|source| ImlEvaluationError::Memory { address, source })
}

fn compare(condition: ImlCondition, left: u32, right: u32) -> bool {
    match condition {
        ImlCondition::Equal => left == right,
        ImlCondition::NotEqual => left != right,
        ImlCondition::SignedLessThan => left.cast_signed() < right.cast_signed(),
        ImlCondition::SignedGreaterThan => left.cast_signed() > right.cast_signed(),
        ImlCondition::UnsignedLessThan => left < right,
        ImlCondition::UnsignedGreaterThan => left > right,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cex_memory::{GuestMemory, Permissions};

    #[test]
    fn evaluates_arithmetic_and_control_flow_without_host_code_generation() {
        let block = ImlBlock::new(
            GuestAddress::new(0x1000),
            1,
            vec![
                ImlInstruction::Move {
                    destination: ImlRegister::Temporary(0),
                    value: ImlOperand::Immediate(40),
                },
                ImlInstruction::Add {
                    destination: ImlRegister::Gpr(3),
                    left: ImlOperand::Register(ImlRegister::Temporary(0)),
                    right: ImlOperand::Immediate(2),
                },
                ImlInstruction::Stop,
            ],
        );
        let mut evaluator = ImlEvaluator::new();
        let mut state = CpuState::default();
        let mut memory = GuestMemory::new();

        let outcome = evaluator.evaluate(&block, &mut state, &mut memory).unwrap();

        assert_eq!(outcome, ImlOutcome::Stop);
        assert_eq!(state.gpr(3), Some(42));
    }

    #[test]
    fn evaluates_big_endian_loads_and_stores() {
        let data = GuestAddress::new(0x2000);
        let block = ImlBlock::new(
            GuestAddress::new(0x1000),
            0,
            vec![
                ImlInstruction::Store {
                    address: ImlOperand::Immediate(data.get()),
                    value: ImlOperand::Immediate(0x1234_abcd),
                    width: LoadWidth::Word,
                },
                ImlInstruction::Load {
                    destination: ImlRegister::Gpr(4),
                    address: ImlOperand::Immediate(data.get() + 2),
                    width: LoadWidth::HalfWord,
                    signed: false,
                },
            ],
        );
        let mut memory = GuestMemory::new();
        memory
            .map(data, 0x1000, Permissions::READ | Permissions::WRITE)
            .unwrap();
        let mut state = CpuState::default();

        assert_eq!(
            ImlEvaluator::new()
                .evaluate(&block, &mut state, &mut memory)
                .unwrap(),
            ImlOutcome::Completed
        );
        assert_eq!(state.gpr(4), Some(0xabcd));
    }

    #[test]
    fn rejects_invalid_virtual_registers_deterministically() {
        let block = ImlBlock::new(
            GuestAddress::ZERO,
            0,
            vec![ImlInstruction::Move {
                destination: ImlRegister::Temporary(2),
                value: ImlOperand::Immediate(1),
            }],
        );

        let error = ImlEvaluator::new()
            .evaluate(&block, &mut CpuState::default(), &mut GuestMemory::new())
            .unwrap_err();

        assert_eq!(error, ImlEvaluationError::InvalidTemporary(2));
    }
}
