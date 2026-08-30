use core::fmt;

use crate::integer::truncate_u32_to_u16;
use crate::{STOP_INSTRUCTION, SYSTEM_CALL_INSTRUCTION};

/// Width of an integer memory operation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LoadWidth {
    /// An eight-bit memory access.
    Byte,
    /// A 16-bit memory access.
    HalfWord,
    /// A 32-bit memory access.
    Word,
}

impl LoadWidth {
    /// Returns the access width in bytes.
    #[must_use]
    pub const fn bytes(self) -> u8 {
        match self {
            Self::Byte => 1,
            Self::HalfWord => 2,
            Self::Word => 4,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum LogicalOperation {
    And,
    Or,
    Xor,
    Nor,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum ArithmeticOperation {
    Add,
    SubtractFrom,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Instruction {
    Stop,
    SystemCall,
    AddImmediate {
        destination: u8,
        base: u8,
        immediate: i16,
        shifted: bool,
    },
    MultiplyLowImmediate {
        destination: u8,
        left: u8,
        immediate: i16,
    },
    SubtractFromImmediateCarrying {
        destination: u8,
        left: u8,
        immediate: i16,
    },
    AddImmediateCarrying {
        destination: u8,
        left: u8,
        immediate: i16,
        record: bool,
    },
    LogicalImmediate {
        operation: LogicalOperation,
        destination: u8,
        source: u8,
        immediate: u16,
        shifted: bool,
        record: bool,
    },
    CompareImmediate {
        field: u8,
        signed: bool,
        left: u8,
        immediate: u16,
    },
    ArithmeticRegister {
        operation: ArithmeticOperation,
        destination: u8,
        left: u8,
        right: u8,
        record: bool,
    },
    LogicalRegister {
        operation: LogicalOperation,
        destination: u8,
        left: u8,
        right: u8,
        record: bool,
    },
    CompareRegister {
        field: u8,
        signed: bool,
        left: u8,
        right: u8,
    },
    Load {
        width: LoadWidth,
        signed: bool,
        destination: u8,
        base: u8,
        displacement: i16,
        update: bool,
    },
    Store {
        width: LoadWidth,
        source: u8,
        base: u8,
        displacement: i16,
        update: bool,
    },
    Branch {
        displacement: i32,
        absolute: bool,
        link: bool,
    },
    BranchConditional {
        branch_options: u8,
        condition_bit: u8,
        displacement: i32,
        absolute: bool,
        link: bool,
    },
    BranchToLinkRegister {
        branch_options: u8,
        condition_bit: u8,
        link: bool,
    },
    BranchToCountRegister {
        branch_options: u8,
        condition_bit: u8,
        link: bool,
    },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
/// A deterministic failure to decode one PowerPC instruction word.
pub enum DecodeError {
    /// The opcode or extended opcode is not implemented by the interpreter.
    UnsupportedOpcode {
        /// The original big-endian PowerPC instruction word.
        instruction: u32,
    },
    /// The opcode is known, but reserved or constrained bits are invalid.
    IllegalEncoding {
        /// The original big-endian PowerPC instruction word.
        instruction: u32,
    },
}

impl fmt::Display for DecodeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let instruction = match self {
            Self::UnsupportedOpcode { instruction } | Self::IllegalEncoding { instruction } => {
                instruction
            }
        };
        match self {
            Self::UnsupportedOpcode { .. } => {
                write!(formatter, "unsupported PowerPC opcode 0x{instruction:08x}")
            }
            Self::IllegalEncoding { .. } => {
                write!(formatter, "illegal PowerPC instruction 0x{instruction:08x}")
            }
        }
    }
}

impl std::error::Error for DecodeError {}

#[must_use = "decoding failures must become deterministic CPU faults"]
pub(crate) fn decode(instruction: u32) -> Result<Instruction, DecodeError> {
    if instruction == STOP_INSTRUCTION {
        return Ok(Instruction::Stop);
    }
    if instruction == SYSTEM_CALL_INSTRUCTION {
        return Ok(Instruction::SystemCall);
    }

    let primary = instruction >> 26;
    let rt = register(instruction, 21);
    let ra = register(instruction, 16);
    let immediate = truncate_u32_to_u16(instruction);
    let signed_immediate = immediate.cast_signed();

    let decoded = match primary {
        7 => Instruction::MultiplyLowImmediate {
            destination: rt,
            left: ra,
            immediate: signed_immediate,
        },
        8 => Instruction::SubtractFromImmediateCarrying {
            destination: rt,
            left: ra,
            immediate: signed_immediate,
        },
        10 | 11 => {
            if ((instruction >> 21) & 1) != 0 {
                return Err(DecodeError::IllegalEncoding { instruction });
            }
            Instruction::CompareImmediate {
                field: extract_u8(instruction, 23, 7),
                signed: primary == 11,
                left: ra,
                immediate,
            }
        }
        12 | 13 => Instruction::AddImmediateCarrying {
            destination: rt,
            left: ra,
            immediate: signed_immediate,
            record: primary == 13,
        },
        14 | 15 => Instruction::AddImmediate {
            destination: rt,
            base: ra,
            immediate: signed_immediate,
            shifted: primary == 15,
        },
        16 => Instruction::BranchConditional {
            branch_options: extract_u8(instruction, 21, 0x1f),
            condition_bit: extract_u8(instruction, 16, 0x1f),
            displacement: sign_extend_branch_displacement(instruction, 16),
            absolute: instruction & 2 != 0,
            link: instruction & 1 != 0,
        },
        18 => Instruction::Branch {
            displacement: sign_extend_branch_displacement(instruction, 26),
            absolute: instruction & 2 != 0,
            link: instruction & 1 != 0,
        },
        19 => decode_branch_register(instruction)?,
        24..=29 => {
            let operation = match primary {
                24 | 25 => LogicalOperation::Or,
                26 | 27 => LogicalOperation::Xor,
                28 | 29 => LogicalOperation::And,
                _ => unreachable!(),
            };
            Instruction::LogicalImmediate {
                operation,
                destination: ra,
                source: rt,
                immediate,
                shifted: primary & 1 != 0,
                record: primary >= 28,
            }
        }
        31 => decode_extended(instruction)?,
        32..=45 => decode_load_store(primary, rt, ra, signed_immediate)?,
        _ => return Err(DecodeError::UnsupportedOpcode { instruction }),
    };

    Ok(decoded)
}

fn decode_extended(instruction: u32) -> Result<Instruction, DecodeError> {
    let rt = register(instruction, 21);
    let ra = register(instruction, 16);
    let rb = register(instruction, 11);
    let record = instruction & 1 != 0;
    let xo = (instruction >> 1) & 0x3ff;

    match xo {
        0 | 32 => {
            if ((instruction >> 21) & 1) != 0 || record {
                return Err(DecodeError::IllegalEncoding { instruction });
            }
            Ok(Instruction::CompareRegister {
                field: extract_u8(instruction, 23, 7),
                signed: xo == 0,
                left: ra,
                right: rb,
            })
        }
        28 | 316 | 444 | 124 => {
            let operation = match xo {
                28 => LogicalOperation::And,
                316 => LogicalOperation::Xor,
                444 => LogicalOperation::Or,
                124 => LogicalOperation::Nor,
                _ => unreachable!(),
            };
            Ok(Instruction::LogicalRegister {
                operation,
                destination: ra,
                left: rt,
                right: rb,
                record,
            })
        }
        _ => {
            // XO-form arithmetic uses bit 10 as OE, leaving a nine-bit XO.
            let arithmetic_xo = (instruction >> 1) & 0x1ff;
            if ((instruction >> 10) & 1) != 0 {
                return Err(DecodeError::UnsupportedOpcode { instruction });
            }
            let operation = match arithmetic_xo {
                266 => ArithmeticOperation::Add,
                40 => ArithmeticOperation::SubtractFrom,
                _ => return Err(DecodeError::UnsupportedOpcode { instruction }),
            };
            Ok(Instruction::ArithmeticRegister {
                operation,
                destination: rt,
                left: ra,
                right: rb,
                record,
            })
        }
    }
}

fn decode_branch_register(instruction: u32) -> Result<Instruction, DecodeError> {
    let branch_options = extract_u8(instruction, 21, 0x1f);
    let condition_bit = extract_u8(instruction, 16, 0x1f);
    // BH is a hint and is deliberately ignored, but reserved bits must be zero.
    if instruction & 0x0000_e000 != 0 {
        return Err(DecodeError::IllegalEncoding { instruction });
    }
    let link = instruction & 1 != 0;
    match (instruction >> 1) & 0x3ff {
        16 => Ok(Instruction::BranchToLinkRegister {
            branch_options,
            condition_bit,
            link,
        }),
        528 => {
            // bcctr uses CTR as its target, so BO[2] must suppress the CTR
            // decrement/test that is available to ordinary conditional branches.
            if branch_options & 0b00100 == 0 {
                return Err(DecodeError::IllegalEncoding { instruction });
            }
            Ok(Instruction::BranchToCountRegister {
                branch_options,
                condition_bit,
                link,
            })
        }
        _ => Err(DecodeError::UnsupportedOpcode { instruction }),
    }
}

fn decode_load_store(
    primary: u32,
    register: u8,
    base: u8,
    displacement: i16,
) -> Result<Instruction, DecodeError> {
    let update = primary & 1 != 0;
    if update && base == 0 {
        return Err(DecodeError::IllegalEncoding {
            instruction: (primary << 26)
                | (u32::from(register) << 21)
                | (u32::from(base) << 16)
                | u32::from(displacement.cast_unsigned()),
        });
    }
    if update && matches!(primary, 33 | 35 | 41 | 43) && register == base {
        return Err(DecodeError::IllegalEncoding {
            instruction: encode_d(primary, register, base, displacement),
        });
    }

    match primary {
        32 | 33 => Ok(Instruction::Load {
            width: LoadWidth::Word,
            signed: false,
            destination: register,
            base,
            displacement,
            update,
        }),
        34 | 35 => Ok(Instruction::Load {
            width: LoadWidth::Byte,
            signed: false,
            destination: register,
            base,
            displacement,
            update,
        }),
        36 | 37 => Ok(Instruction::Store {
            width: LoadWidth::Word,
            source: register,
            base,
            displacement,
            update,
        }),
        38 | 39 => Ok(Instruction::Store {
            width: LoadWidth::Byte,
            source: register,
            base,
            displacement,
            update,
        }),
        40 | 41 => Ok(Instruction::Load {
            width: LoadWidth::HalfWord,
            signed: false,
            destination: register,
            base,
            displacement,
            update,
        }),
        42 | 43 => Ok(Instruction::Load {
            width: LoadWidth::HalfWord,
            signed: true,
            destination: register,
            base,
            displacement,
            update,
        }),
        44 | 45 => Ok(Instruction::Store {
            width: LoadWidth::HalfWord,
            source: register,
            base,
            displacement,
            update,
        }),
        _ => unreachable!(),
    }
}

fn register(instruction: u32, shift: u32) -> u8 {
    extract_u8(instruction, shift, 0x1f)
}

fn extract_u8(instruction: u32, shift: u32, mask: u32) -> u8 {
    u8::try_from((instruction >> shift) & mask)
        .expect("masked PowerPC instruction field fits in u8")
}

fn sign_extend_branch_displacement(instruction: u32, bits: u32) -> i32 {
    let mask = if bits == 26 { 0x03ff_fffc } else { 0x0000_fffc };
    let shift = 32 - bits;
    ((instruction & mask) << shift).cast_signed() >> shift
}

fn encode_d(primary: u32, rt: u8, ra: u8, displacement: i16) -> u32 {
    (primary << 26)
        | (u32::from(rt) << 21)
        | (u32::from(ra) << 16)
        | u32::from(displacement.cast_unsigned())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_synthetic_program() {
        assert_eq!(
            decode(0x3860_0028),
            Ok(Instruction::AddImmediate {
                destination: 3,
                base: 0,
                immediate: 40,
                shifted: false,
            })
        );
        assert_eq!(
            decode(0x3863_0002),
            Ok(Instruction::AddImmediate {
                destination: 3,
                base: 3,
                immediate: 2,
                shifted: false,
            })
        );
        assert_eq!(decode(0), Ok(Instruction::Stop));
    }

    #[test]
    fn branch_displacements_are_signed_and_aligned() {
        assert_eq!(
            decode(0x4bff_fffc),
            Ok(Instruction::Branch {
                displacement: -4,
                absolute: false,
                link: false,
            })
        );
        assert_eq!(
            decode(0x4182_fff8),
            Ok(Instruction::BranchConditional {
                branch_options: 12,
                condition_bit: 2,
                displacement: -8,
                absolute: false,
                link: false,
            })
        );
    }

    #[test]
    fn unsupported_words_are_stable_errors() {
        assert_eq!(
            decode(0xffff_ffff),
            Err(DecodeError::UnsupportedOpcode {
                instruction: 0xffff_ffff,
            })
        );
    }

    #[test]
    fn every_update_load_rejects_base_equal_to_destination() {
        for instruction in [0x8463_0004, 0x8c63_0004, 0xa463_0004, 0xac63_0004] {
            assert_eq!(
                decode(instruction),
                Err(DecodeError::IllegalEncoding { instruction })
            );
        }
    }

    #[test]
    fn branch_to_count_register_must_not_decrement_its_target() {
        let illegal = 0x4c00_0420;
        assert_eq!(
            decode(illegal),
            Err(DecodeError::IllegalEncoding {
                instruction: illegal,
            })
        );

        assert_eq!(
            decode(0x4e80_0420),
            Ok(Instruction::BranchToCountRegister {
                branch_options: 20,
                condition_bit: 0,
                link: false,
            })
        );
    }
}
