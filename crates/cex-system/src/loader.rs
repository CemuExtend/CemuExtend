//! Loader for the deterministic synthetic PPC fixture format.

use cex_types::GuestAddress;
use thiserror::Error;

const MAGIC: &[u8; 4] = b"CEXH";
const HEADER_LEN: usize = 24;
const FORMAT_VERSION: u16 = 1;

/// Hard resource bound for the deliberately tiny synthetic image format.
pub const MAX_SYNTHETIC_CODE_SIZE: usize = 1024 * 1024;

/// Maximum complete CEXH v1 file size accepted by a bounded file reader.
pub const MAX_SYNTHETIC_IMAGE_SIZE: usize = HEADER_LEN + MAX_SYNTHETIC_CODE_SIZE;

/// Stable name accepted by the command-line runner for the bundled fixture.
pub const BUILTIN_FIXTURE_NAME: &str = "synthetic-boot";

/// A minimal, deterministic PPC program used to bootstrap the headless core.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SyntheticProgram {
    /// Guest virtual address at which `code` is mapped.
    pub load_address: GuestAddress,
    /// Initial PPC instruction pointer.
    pub entry_point: GuestAddress,
    /// Initial guest stack pointer.
    pub stack_pointer: GuestAddress,
    /// Big-endian PPC instruction bytes.
    pub code: Vec<u8>,
}

impl SyntheticProgram {
    /// Decode a versioned CEXH program container.
    pub fn decode(bytes: &[u8]) -> Result<Self, ProgramDecodeError> {
        if bytes.len() < HEADER_LEN {
            return Err(ProgramDecodeError::TruncatedHeader);
        }
        if &bytes[0..4] != MAGIC {
            return Err(ProgramDecodeError::BadMagic);
        }

        let version = u16::from_be_bytes([bytes[4], bytes[5]]);
        if version != FORMAT_VERSION {
            return Err(ProgramDecodeError::UnsupportedVersion(version));
        }
        let reserved = u16::from_be_bytes([bytes[6], bytes[7]]);
        if reserved != 0 {
            return Err(ProgramDecodeError::NonZeroReserved);
        }

        let load_address = GuestAddress::new(read_u32(bytes, 8));
        let entry_point = GuestAddress::new(read_u32(bytes, 12));
        let stack_pointer = GuestAddress::new(read_u32(bytes, 16));
        let code_len =
            usize::try_from(read_u32(bytes, 20)).map_err(|_| ProgramDecodeError::LengthOverflow)?;
        let expected_len = HEADER_LEN
            .checked_add(code_len)
            .ok_or(ProgramDecodeError::LengthOverflow)?;
        if bytes.len() != expected_len {
            return Err(ProgramDecodeError::LengthMismatch {
                declared: code_len,
                actual: bytes.len().saturating_sub(HEADER_LEN),
            });
        }
        if code_len == 0 || code_len % 4 != 0 {
            return Err(ProgramDecodeError::InvalidCodeLength(code_len));
        }
        if code_len > MAX_SYNTHETIC_CODE_SIZE {
            return Err(ProgramDecodeError::CodeTooLarge(code_len));
        }

        let load_start = u64::from(load_address.get());
        let load_end = load_start
            .checked_add(u64::try_from(code_len).map_err(|_| ProgramDecodeError::LengthOverflow)?)
            .ok_or(ProgramDecodeError::AddressOverflow)?;
        if load_end > (u64::from(u32::MAX) + 1) {
            return Err(ProgramDecodeError::AddressOverflow);
        }
        if !load_address.get().is_multiple_of(4) {
            return Err(ProgramDecodeError::UnalignedLoadAddress(load_address));
        }
        let entry_start = u64::from(entry_point.get());
        let entry_end = entry_start
            .checked_add(4)
            .ok_or(ProgramDecodeError::AddressOverflow)?;
        if entry_start < load_start || entry_end > load_end || !entry_point.get().is_multiple_of(4)
        {
            return Err(ProgramDecodeError::EntryOutsideCode);
        }

        Ok(Self {
            load_address,
            entry_point,
            stack_pointer,
            code: bytes[HEADER_LEN..].to_vec(),
        })
    }

    /// Encode the program using the stable CEXH v1 wire format.
    pub fn encode(&self) -> Result<Vec<u8>, ProgramDecodeError> {
        let code_len =
            u32::try_from(self.code.len()).map_err(|_| ProgramDecodeError::LengthOverflow)?;
        if code_len == 0 || code_len % 4 != 0 {
            return Err(ProgramDecodeError::InvalidCodeLength(self.code.len()));
        }
        if self.code.len() > MAX_SYNTHETIC_CODE_SIZE {
            return Err(ProgramDecodeError::CodeTooLarge(self.code.len()));
        }

        let mut result = Vec::with_capacity(HEADER_LEN + self.code.len());
        result.extend_from_slice(MAGIC);
        result.extend_from_slice(&FORMAT_VERSION.to_be_bytes());
        result.extend_from_slice(&0_u16.to_be_bytes());
        result.extend_from_slice(&self.load_address.get().to_be_bytes());
        result.extend_from_slice(&self.entry_point.get().to_be_bytes());
        result.extend_from_slice(&self.stack_pointer.get().to_be_bytes());
        result.extend_from_slice(&code_len.to_be_bytes());
        result.extend_from_slice(&self.code);

        // Keep encode and decode subject to exactly the same validation.
        Self::decode(&result)?;
        Ok(result)
    }
}

/// Return the source-controlled, non-private synthetic boot fixture.
pub fn builtin_fixture() -> SyntheticProgram {
    SyntheticProgram {
        load_address: GuestAddress::new(0x0100_0000),
        entry_point: GuestAddress::new(0x0100_0000),
        stack_pointer: GuestAddress::new(0x1000_0000),
        code: vec![
            0x38, 0x60, 0x00, 0x28, // addi r3, r0, 40
            0x38, 0x63, 0x00, 0x02, // addi r3, r3, 2
            0x00, 0x00, 0x00, 0x00, // deterministic stop sentinel
        ],
    }
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

/// Validation failure for the synthetic CEXH container.
#[derive(Clone, Debug, Error, Eq, PartialEq)]
pub enum ProgramDecodeError {
    /// The fixed header is incomplete.
    #[error("synthetic program header is truncated")]
    TruncatedHeader,
    /// The file does not start with the CEXH magic.
    #[error("synthetic program has invalid magic")]
    BadMagic,
    /// Only format version 1 is currently supported.
    #[error("unsupported synthetic program version {0}")]
    UnsupportedVersion(u16),
    /// Reserved bits must stay zero for forward-compatible decoding.
    #[error("synthetic program reserved header bits are non-zero")]
    NonZeroReserved,
    /// The declared payload length cannot be represented safely.
    #[error("synthetic program length overflows the host address space")]
    LengthOverflow,
    /// The declared and actual code sizes differ.
    #[error("synthetic program declares {declared} code bytes but contains {actual}")]
    LengthMismatch {
        /// Size from the header.
        declared: usize,
        /// Bytes actually following the header.
        actual: usize,
    },
    /// PPC instructions must be a non-empty sequence of 32-bit words.
    #[error("synthetic program has invalid code length {0}")]
    InvalidCodeLength(usize),
    /// Synthetic fixtures are intentionally resource-bounded.
    #[error("synthetic program code length {0} exceeds the 1 MiB limit")]
    CodeTooLarge(usize),
    /// PPC code segments begin on a 32-bit instruction boundary.
    #[error("synthetic program load address {0} is not 4-byte aligned")]
    UnalignedLoadAddress(GuestAddress),
    /// The load range would wrap guest address space.
    #[error("synthetic program load range overflows guest address space")]
    AddressOverflow,
    /// The entry point is not aligned within the code segment.
    #[error("synthetic program entry point is outside its code segment")]
    EntryOutsideCode,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bundled_fixture_round_trips() {
        let fixture = builtin_fixture();
        let encoded = fixture.encode().expect("fixture must encode");
        assert_eq!(SyntheticProgram::decode(&encoded), Ok(fixture));
    }

    #[test]
    fn rejects_trailing_bytes() {
        let mut encoded = builtin_fixture().encode().expect("fixture must encode");
        encoded.push(0);
        assert!(matches!(
            SyntheticProgram::decode(&encoded),
            Err(ProgramDecodeError::LengthMismatch { .. })
        ));
    }

    #[test]
    fn rejects_oversized_code_before_copying() {
        let mut bytes = vec![0_u8; HEADER_LEN + MAX_SYNTHETIC_CODE_SIZE + 4];
        bytes[0..4].copy_from_slice(MAGIC);
        bytes[4..6].copy_from_slice(&FORMAT_VERSION.to_be_bytes());
        bytes[8..12].copy_from_slice(&0x0100_0000_u32.to_be_bytes());
        bytes[12..16].copy_from_slice(&0x0100_0000_u32.to_be_bytes());
        bytes[16..20].copy_from_slice(&0x1000_0000_u32.to_be_bytes());
        bytes[20..24].copy_from_slice(
            &u32::try_from(MAX_SYNTHETIC_CODE_SIZE + 4)
                .expect("test length fits u32")
                .to_be_bytes(),
        );

        assert_eq!(
            SyntheticProgram::decode(&bytes),
            Err(ProgramDecodeError::CodeTooLarge(
                MAX_SYNTHETIC_CODE_SIZE + 4
            ))
        );
    }

    #[test]
    fn rejects_unaligned_code_segment() {
        let mut encoded = builtin_fixture().encode().expect("fixture must encode");
        encoded[8..12].copy_from_slice(&0x0100_0001_u32.to_be_bytes());

        assert_eq!(
            SyntheticProgram::decode(&encoded),
            Err(ProgramDecodeError::UnalignedLoadAddress(GuestAddress::new(
                0x0100_0001
            )))
        );
    }

    #[test]
    fn rejects_entry_without_a_complete_instruction() {
        let mut encoded = builtin_fixture().encode().expect("fixture must encode");
        let code_end = 0x0100_0000_u32
            .checked_add(
                u32::try_from(builtin_fixture().code.len()).expect("fixture length fits u32"),
            )
            .expect("fixture range is bounded");
        encoded[12..16].copy_from_slice(&code_end.to_be_bytes());

        assert_eq!(
            SyntheticProgram::decode(&encoded),
            Err(ProgramDecodeError::EntryOutsideCode)
        );
    }

    #[test]
    fn encode_applies_the_same_alignment_validation() {
        let mut program = builtin_fixture();
        program.load_address = GuestAddress::new(program.load_address.get() + 1);

        assert_eq!(
            program.encode(),
            Err(ProgramDecodeError::UnalignedLoadAddress(GuestAddress::new(
                0x0100_0001
            )))
        );
    }
}
