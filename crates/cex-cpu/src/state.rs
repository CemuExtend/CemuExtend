use cex_types::{GuestAddress, GuestCycle};

/// A stable description of the most recent architectural fault.
///
/// Host error details stay in [`crate::CpuFault`]; this compact record is part
/// of the canonical CPU state used by differential traces.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ArchitecturalFault {
    /// Address of the instruction whose execution faulted.
    pub instruction_pointer: GuestAddress,
    /// Memory address involved in the fault, when one is applicable.
    pub address: Option<GuestAddress>,
    /// Stable architectural classification of the fault.
    pub kind: FaultKind,
}

/// Stable architectural fault classifications used in CPU-state traces.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FaultKind {
    /// Instruction memory could not be fetched.
    InstructionFetch,
    /// Guest data could not be read.
    DataRead,
    /// Guest data could not be written.
    DataWrite,
    /// An instruction or memory address violated its alignment requirement.
    Alignment,
    /// The interpreter does not support the fetched instruction.
    UnsupportedInstruction,
    /// A deterministic execution counter could not be incremented.
    CounterOverflow,
}

/// State retained by a PowerPC load-reserve instruction.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Reservation {
    /// Guest address of the reserved word.
    pub address: GuestAddress,
    /// Word observed when the reservation was established.
    pub value: u32,
}

/// Error returned when a general-purpose register index is out of range.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RegisterIndexError {
    /// Rejected register index.
    pub index: u8,
}

/// Canonical architectural state for one emulated PowerPC core.
///
/// Floating-point registers are stored as raw bits. This avoids host floating
/// point canonicalisation and gives future paired-single work a deterministic
/// comparison surface.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CpuState {
    /// Address of the next instruction to execute.
    pub instruction_pointer: GuestAddress,
    /// The 32 general-purpose registers, indexed by architectural register number.
    pub gprs: [u32; 32],
    /// Two raw 64-bit lanes matching Cemu's paired `FPR_t` representation.
    pub fprs: [[u64; 2]; 32],
    /// Floating-point status and control register.
    pub fpscr: u32,
    /// Eight four-bit condition-register fields in architectural bit order.
    pub condition_register: u32,
    /// Fixed-point exception register.
    pub xer: u32,
    /// Link register used for subroutine returns and indirect branches.
    pub link_register: u32,
    /// Count register used by counted branches.
    pub count_register: u32,
    /// Quantisation registers used by paired-single load and store instructions.
    pub ugqr: [u32; 8],
    /// Active load-reserve state, if any.
    pub reservation: Option<Reservation>,
    /// Most recent fault that has not been cleared by a retired instruction.
    pub pending_fault: Option<ArchitecturalFault>,
    /// Total deterministic guest cycles retired by this state.
    pub cycles: GuestCycle,
    /// Total guest instructions retired by this state.
    pub instructions_retired: u64,
}

impl CpuState {
    /// Creates zero-initialised architectural state at `entry`.
    #[must_use]
    pub fn new(entry: GuestAddress) -> Self {
        Self {
            instruction_pointer: entry,
            gprs: [0; 32],
            fprs: [[0; 2]; 32],
            fpscr: 0,
            condition_register: 0,
            xer: 0,
            link_register: 0,
            count_register: 0,
            ugqr: [0; 8],
            reservation: None,
            pending_fault: None,
            cycles: GuestCycle::new(0),
            instructions_retired: 0,
        }
    }

    /// Returns the value of a general-purpose register, or `None` for an invalid index.
    #[must_use]
    pub fn gpr(&self, index: u8) -> Option<u32> {
        self.gprs.get(usize::from(index)).copied()
    }

    /// Sets a general-purpose register.
    ///
    /// Returns [`RegisterIndexError`] when `index` is not in `0..32`.
    pub fn set_gpr(&mut self, index: u8, value: u32) -> Result<(), RegisterIndexError> {
        let slot = self
            .gprs
            .get_mut(usize::from(index))
            .ok_or(RegisterIndexError { index })?;
        *slot = value;
        Ok(())
    }

    /// Returns one condition-register bit using PowerPC's most-significant-bit numbering.
    ///
    /// Returns `None` when `bit_index` is not in `0..32`.
    #[must_use]
    pub fn condition_bit(&self, bit_index: u8) -> Option<bool> {
        (bit_index < 32).then(|| (self.condition_register & (0x8000_0000_u32 >> bit_index)) != 0)
    }

    pub(crate) fn gpr_unchecked(&self, index: u8) -> u32 {
        debug_assert!(index < 32);
        self.gprs[usize::from(index)]
    }

    pub(crate) fn set_gpr_unchecked(&mut self, index: u8, value: u32) {
        debug_assert!(index < 32);
        self.gprs[usize::from(index)] = value;
    }

    pub(crate) fn condition_bit_unchecked(&self, bit_index: u8) -> bool {
        debug_assert!(bit_index < 32);
        (self.condition_register & (0x8000_0000_u32 >> bit_index)) != 0
    }

    pub(crate) fn set_condition_field(&mut self, field: u8, nibble: u8) {
        debug_assert!(field < 8);
        let shift = 28 - u32::from(field) * 4;
        let mask = 0xf_u32 << shift;
        self.condition_register =
            (self.condition_register & !mask) | (u32::from(nibble & 0xf) << shift);
    }

    pub(crate) fn set_cr0_from_result(&mut self, value: u32) {
        let comparison = match value.cast_signed().cmp(&0) {
            std::cmp::Ordering::Less => 0b1000,
            std::cmp::Ordering::Greater => 0b0100,
            std::cmp::Ordering::Equal => 0b0010,
        };
        self.set_condition_field(0, comparison | self.summary_overflow_bit());
    }

    pub(crate) fn summary_overflow_bit(&self) -> u8 {
        u8::from((self.xer & 0x8000_0000) != 0)
    }

    pub(crate) fn retire_instruction(&mut self) {
        debug_assert!(self.instructions_retired < u64::MAX);
        debug_assert!(self.cycles.get() < u64::MAX);
        self.instructions_retired += 1;
        self.cycles = GuestCycle::new(self.cycles.get() + 1);
        self.pending_fault = None;
    }
}

impl Default for CpuState {
    fn default() -> Self {
        Self::new(GuestAddress::ZERO)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn condition_bits_use_powerpc_msb_numbering() {
        let state = CpuState {
            condition_register: 0x8100_0000,
            ..CpuState::default()
        };

        assert_eq!(state.condition_bit(0), Some(true));
        assert_eq!(state.condition_bit(7), Some(true));
        assert_eq!(state.condition_bit(1), Some(false));
        assert_eq!(state.condition_bit(32), None);
    }

    #[test]
    fn setting_one_condition_field_preserves_the_others() {
        let mut state = CpuState {
            condition_register: 0x1234_5678,
            ..CpuState::default()
        };

        state.set_condition_field(2, 0xf);

        assert_eq!(state.condition_register, 0x12f4_5678);
    }
}
