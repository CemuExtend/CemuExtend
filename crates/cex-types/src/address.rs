use core::fmt;

use crate::BeU32;

/// Size of the 32-bit guest virtual address space, in bytes.
pub const GUEST_ADDRESS_SPACE_SIZE: u64 = 1_u64 << 32;

#[allow(clippy::cast_lossless)]
const fn address_space_offset(address: GuestAddress) -> u64 {
    address.get() as u64
}

/// A byte address in the 32-bit guest virtual address space.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]
#[cfg_attr(feature = "serde", derive(serde::Deserialize, serde::Serialize))]
#[cfg_attr(feature = "serde", serde(transparent))]
pub struct GuestAddress(u32);

impl GuestAddress {
    /// The null/lowest guest address.
    pub const ZERO: Self = Self(0);

    /// The highest representable guest address.
    pub const MAX: Self = Self(u32::MAX);

    /// Constructs a guest address.
    #[must_use]
    pub const fn new(value: u32) -> Self {
        Self(value)
    }

    /// Returns the raw 32-bit virtual address.
    #[must_use]
    pub const fn get(self) -> u32 {
        self.0
    }

    /// Decodes a big-endian guest pointer field.
    #[must_use]
    pub const fn from_be(value: BeU32) -> Self {
        Self(value.get())
    }

    /// Encodes this address as a big-endian guest pointer field.
    #[must_use]
    pub const fn to_be(self) -> BeU32 {
        BeU32::new(self.0)
    }

    /// Adds a byte offset without wrapping the guest address space.
    #[must_use]
    pub const fn checked_add(self, offset: u32) -> Option<Self> {
        match self.0.checked_add(offset) {
            Some(value) => Some(Self(value)),
            None => None,
        }
    }

    /// Subtracts a byte offset without wrapping the guest address space.
    #[must_use]
    pub const fn checked_sub(self, offset: u32) -> Option<Self> {
        match self.0.checked_sub(offset) {
            Some(value) => Some(Self(value)),
            None => None,
        }
    }

    /// Applies a signed byte offset without wrapping the guest address space.
    #[must_use]
    pub fn checked_offset(self, offset: i64) -> Option<Self> {
        let value = i128::from(self.0) + i128::from(offset);
        u32::try_from(value).ok().map(Self)
    }

    /// Returns whether the address is aligned to a non-zero power of two.
    #[must_use]
    pub const fn is_aligned(self, alignment: u32) -> bool {
        alignment.is_power_of_two() && self.0 & (alignment - 1) == 0
    }

    /// Rounds down to a power-of-two byte alignment.
    pub const fn align_down(self, alignment: u32) -> Result<Self, GuestAddressError> {
        if !alignment.is_power_of_two() {
            return Err(GuestAddressError::InvalidAlignment { alignment });
        }
        Ok(Self(self.0 & !(alignment - 1)))
    }

    /// Rounds up to a power-of-two byte alignment without wrapping.
    pub const fn checked_align_up(self, alignment: u32) -> Result<Self, GuestAddressError> {
        if !alignment.is_power_of_two() {
            return Err(GuestAddressError::InvalidAlignment { alignment });
        }
        let mask = alignment - 1;
        match self.0.checked_add(mask) {
            Some(value) => Ok(Self(value & !mask)),
            None => Err(GuestAddressError::AlignmentOverflow {
                address: self,
                alignment,
            }),
        }
    }
}

impl From<u32> for GuestAddress {
    fn from(value: u32) -> Self {
        Self::new(value)
    }
}

impl From<GuestAddress> for u32 {
    fn from(value: GuestAddress) -> Self {
        value.get()
    }
}

impl From<BeU32> for GuestAddress {
    fn from(value: BeU32) -> Self {
        Self::from_be(value)
    }
}

impl From<GuestAddress> for BeU32 {
    fn from(value: GuestAddress) -> Self {
        value.to_be()
    }
}

impl TryFrom<u64> for GuestAddress {
    type Error = GuestAddressError;

    fn try_from(value: u64) -> Result<Self, Self::Error> {
        u32::try_from(value)
            .map(Self)
            .map_err(|_| GuestAddressError::AddressOutOfRange { value })
    }
}

impl TryFrom<usize> for GuestAddress {
    type Error = GuestAddressError;

    fn try_from(value: usize) -> Result<Self, Self::Error> {
        let value = u64::try_from(value)
            .map_err(|_| GuestAddressError::AddressOutOfRange { value: u64::MAX })?;
        Self::try_from(value)
    }
}

impl fmt::Display for GuestAddress {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "0x{:08x}", self.0)
    }
}

/// A validated half-open byte range in guest virtual memory.
///
/// The end is stored conceptually as a `u64`, allowing a range to end exactly
/// at `0x1_0000_0000` and allowing the full 4 GiB address space to be expressed.
#[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Deserialize, serde::Serialize))]
#[cfg_attr(
    feature = "serde",
    serde(try_from = "GuestRangeSerde", into = "GuestRangeSerde")
)]
pub struct GuestRange {
    start: GuestAddress,
    length: u64,
}

impl GuestRange {
    /// Validates and constructs a half-open guest range.
    pub const fn new(start: GuestAddress, length: u64) -> Result<Self, GuestAddressError> {
        match address_space_offset(start).checked_add(length) {
            Some(end) if end <= GUEST_ADDRESS_SPACE_SIZE => Ok(Self { start, length }),
            _ => Err(GuestAddressError::RangeOutOfBounds { start, length }),
        }
    }

    /// Returns a range spanning all guest virtual addresses.
    #[must_use]
    pub const fn full() -> Self {
        Self {
            start: GuestAddress::ZERO,
            length: GUEST_ADDRESS_SPACE_SIZE,
        }
    }

    /// Returns the first address in the range.
    #[must_use]
    pub const fn start(self) -> GuestAddress {
        self.start
    }

    /// Returns the range length in bytes.
    #[must_use]
    pub const fn len(self) -> u64 {
        self.length
    }

    /// Returns whether this range contains no bytes.
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.length == 0
    }

    /// Returns the exclusive end as an address-space offset.
    ///
    /// The result may be exactly [`GUEST_ADDRESS_SPACE_SIZE`], which is not a
    /// valid [`GuestAddress`].
    #[must_use]
    pub const fn end_exclusive(self) -> u64 {
        address_space_offset(self.start) + self.length
    }

    /// Returns the exclusive end when it is representable as a guest address.
    #[must_use]
    pub fn end_address(self) -> Option<GuestAddress> {
        GuestAddress::try_from(self.end_exclusive()).ok()
    }

    /// Returns the last included address, or `None` for an empty range.
    #[must_use]
    pub fn last_address(self) -> Option<GuestAddress> {
        if self.is_empty() {
            None
        } else {
            GuestAddress::try_from(self.end_exclusive() - 1).ok()
        }
    }

    /// Returns whether `address` names a byte in this range.
    #[must_use]
    pub const fn contains(self, address: GuestAddress) -> bool {
        let address = address_space_offset(address);
        address >= address_space_offset(self.start) && address < self.end_exclusive()
    }

    /// Returns whether every byte in `other` is contained by this range.
    #[must_use]
    pub const fn contains_range(self, other: Self) -> bool {
        address_space_offset(other.start) >= address_space_offset(self.start)
            && other.end_exclusive() <= self.end_exclusive()
    }

    /// Returns whether this range and `other` share at least one byte.
    #[must_use]
    pub const fn overlaps(self, other: Self) -> bool {
        !self.is_empty()
            && !other.is_empty()
            && address_space_offset(self.start) < other.end_exclusive()
            && address_space_offset(other.start) < self.end_exclusive()
    }

    /// Returns the byte offset of an address in this range.
    #[must_use]
    pub const fn offset_of(self, address: GuestAddress) -> Option<u64> {
        if self.contains(address) {
            Some(address_space_offset(address) - address_space_offset(self.start))
        } else {
            None
        }
    }
}

#[cfg(feature = "serde")]
#[derive(serde::Deserialize, serde::Serialize)]
struct GuestRangeSerde {
    start: GuestAddress,
    length: u64,
}

#[cfg(feature = "serde")]
impl TryFrom<GuestRangeSerde> for GuestRange {
    type Error = GuestAddressError;

    fn try_from(value: GuestRangeSerde) -> Result<Self, Self::Error> {
        Self::new(value.start, value.length)
    }
}

#[cfg(feature = "serde")]
impl From<GuestRange> for GuestRangeSerde {
    fn from(value: GuestRange) -> Self {
        Self {
            start: value.start,
            length: value.length,
        }
    }
}

/// Validation errors for guest addresses and ranges.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GuestAddressError {
    /// A wider integer cannot be represented by a 32-bit guest address.
    AddressOutOfRange {
        /// Rejected integer value.
        value: u64,
    },
    /// A range extends beyond the 4 GiB guest address space.
    RangeOutOfBounds {
        /// First address requested.
        start: GuestAddress,
        /// Requested byte length.
        length: u64,
    },
    /// Alignment was zero or not a power of two.
    InvalidAlignment {
        /// Rejected alignment.
        alignment: u32,
    },
    /// Rounding an address up would wrap past `u32::MAX`.
    AlignmentOverflow {
        /// Address that was being rounded.
        address: GuestAddress,
        /// Requested alignment.
        alignment: u32,
    },
}

impl fmt::Display for GuestAddressError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::AddressOutOfRange { value } => {
                write!(
                    formatter,
                    "0x{value:x} is outside the 32-bit guest address space"
                )
            }
            Self::RangeOutOfBounds { start, length } => write!(
                formatter,
                "guest range starting at {start} with length 0x{length:x} exceeds 4 GiB"
            ),
            Self::InvalidAlignment { alignment } => write!(
                formatter,
                "guest alignment {alignment} is not a non-zero power of two"
            ),
            Self::AlignmentOverflow { address, alignment } => write!(
                formatter,
                "aligning guest address {address} to {alignment} bytes would overflow"
            ),
        }
    }
}

impl core::error::Error for GuestAddressError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn address_arithmetic_never_wraps() {
        assert_eq!(
            GuestAddress::new(7).checked_add(5),
            Some(GuestAddress::new(12))
        );
        assert_eq!(GuestAddress::MAX.checked_add(1), None);
        assert_eq!(GuestAddress::new(7).checked_sub(8), None);
        assert_eq!(GuestAddress::ZERO.checked_offset(-1), None);
        assert_eq!(GuestAddress::MAX.checked_offset(1), None);
        assert_eq!(
            GuestAddress::new(7).checked_offset(-7),
            Some(GuestAddress::ZERO)
        );
        assert_eq!(GuestAddress::new(7).checked_offset(i64::MAX), None);
        assert_eq!(GuestAddress::new(7).checked_offset(i64::MIN), None);
    }

    #[test]
    fn alignment_is_explicit_and_checked() {
        let address = GuestAddress::new(0x1235);
        assert!(!address.is_aligned(4));
        assert_eq!(address.align_down(4), Ok(GuestAddress::new(0x1234)));
        assert_eq!(address.checked_align_up(4), Ok(GuestAddress::new(0x1238)));
        assert_eq!(
            address.align_down(0),
            Err(GuestAddressError::InvalidAlignment { alignment: 0 })
        );
        assert_eq!(
            GuestAddress::MAX.checked_align_up(4),
            Err(GuestAddressError::AlignmentOverflow {
                address: GuestAddress::MAX,
                alignment: 4,
            })
        );
    }

    #[test]
    fn range_supports_the_entire_four_gibibyte_space() {
        let full = GuestRange::full();
        assert_eq!(full.len(), 0x1_0000_0000);
        assert_eq!(full.end_exclusive(), GUEST_ADDRESS_SPACE_SIZE);
        assert_eq!(full.end_address(), None);
        assert_eq!(full.last_address(), Some(GuestAddress::MAX));
        assert!(full.contains(GuestAddress::ZERO));
        assert!(full.contains(GuestAddress::MAX));
    }

    #[test]
    fn range_rejects_only_values_past_address_space_end() {
        assert_eq!(
            GuestRange::new(GuestAddress::MAX, 1),
            Ok(GuestRange {
                start: GuestAddress::MAX,
                length: 1,
            })
        );
        assert_eq!(
            GuestRange::new(GuestAddress::MAX, 2),
            Err(GuestAddressError::RangeOutOfBounds {
                start: GuestAddress::MAX,
                length: 2,
            })
        );
        assert_eq!(
            GuestRange::new(GuestAddress::new(1), u64::MAX),
            Err(GuestAddressError::RangeOutOfBounds {
                start: GuestAddress::new(1),
                length: u64::MAX,
            })
        );
    }

    #[test]
    fn range_validation_matches_wide_checked_arithmetic() {
        let starts = [0, 1, 0x7fff_ffff, 0xffff_fffe, u32::MAX];
        let lengths = [
            0,
            1,
            2,
            u64::from(u32::MAX),
            GUEST_ADDRESS_SPACE_SIZE,
            u64::MAX,
        ];
        for start in starts {
            for length in lengths {
                let expected_valid = u64::from(start)
                    .checked_add(length)
                    .is_some_and(|end| end <= GUEST_ADDRESS_SPACE_SIZE);
                assert_eq!(
                    GuestRange::new(GuestAddress::new(start), length).is_ok(),
                    expected_valid,
                    "start={start:#x}, length={length:#x}"
                );
            }
        }
    }

    #[test]
    fn half_open_range_queries_handle_edges_and_empty_ranges() {
        let range = GuestRange::new(GuestAddress::new(10), 5).expect("valid range");
        assert!(!range.contains(GuestAddress::new(9)));
        assert!(range.contains(GuestAddress::new(10)));
        assert!(range.contains(GuestAddress::new(14)));
        assert!(!range.contains(GuestAddress::new(15)));
        assert_eq!(range.offset_of(GuestAddress::new(14)), Some(4));

        let inside = GuestRange::new(GuestAddress::new(11), 3).expect("valid range");
        let touching = GuestRange::new(GuestAddress::new(15), 2).expect("valid range");
        let empty = GuestRange::new(GuestAddress::new(12), 0).expect("valid range");
        assert!(range.contains_range(inside));
        assert!(range.contains_range(empty));
        assert!(range.overlaps(inside));
        assert!(!range.overlaps(touching));
        assert!(!range.overlaps(empty));
    }

    #[test]
    fn wider_address_conversion_checks_bounds() {
        assert_eq!(
            GuestAddress::try_from(u64::from(u32::MAX)),
            Ok(GuestAddress::MAX)
        );
        assert_eq!(
            GuestAddress::try_from(GUEST_ADDRESS_SPACE_SIZE),
            Err(GuestAddressError::AddressOutOfRange {
                value: GUEST_ADDRESS_SPACE_SIZE,
            })
        );
    }

    #[test]
    fn address_big_endian_conversion_is_explicit() {
        let address = GuestAddress::new(0x1234_5678);
        assert_eq!(address.to_be().to_be_bytes(), [0x12, 0x34, 0x56, 0x78]);
        assert_eq!(GuestAddress::from_be(address.to_be()), address);
    }

    #[cfg(feature = "serde")]
    #[test]
    fn serde_revalidates_guest_ranges() {
        let invalid = r#"{"start":4294967295,"length":2}"#;
        assert!(serde_json::from_str::<GuestRange>(invalid).is_err());

        let valid = GuestRange::new(GuestAddress::new(0xffff_ffff), 1).expect("valid range");
        let json = serde_json::to_string(&valid).expect("serializes");
        assert_eq!(
            serde_json::from_str::<GuestRange>(&json).expect("validates"),
            valid
        );
    }
}
