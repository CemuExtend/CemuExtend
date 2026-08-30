use core::fmt;

use crate::BeU64;

/// A monotonically increasing guest CPU-cycle timestamp.
///
/// Arithmetic is explicit and never wraps implicitly. Callers must choose
/// checked or saturating behavior at the point where time is advanced.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]
#[cfg_attr(feature = "serde", derive(serde::Deserialize, serde::Serialize))]
#[cfg_attr(feature = "serde", serde(transparent))]
pub struct GuestCycle(u64);

impl GuestCycle {
    /// The initial guest timestamp.
    pub const ZERO: Self = Self(0);

    /// The highest representable guest timestamp.
    pub const MAX: Self = Self(u64::MAX);

    /// Constructs a guest-cycle timestamp.
    #[must_use]
    pub const fn new(value: u64) -> Self {
        Self(value)
    }

    /// Returns the raw cycle count.
    #[must_use]
    pub const fn get(self) -> u64 {
        self.0
    }

    /// Decodes a big-endian guest cycle field.
    #[must_use]
    pub const fn from_be(value: BeU64) -> Self {
        Self(value.get())
    }

    /// Encodes this timestamp as a big-endian guest value.
    #[must_use]
    pub const fn to_be(self) -> BeU64 {
        BeU64::new(self.0)
    }

    /// Advances by `cycles`, returning `None` rather than wrapping.
    #[must_use]
    pub const fn checked_add(self, cycles: u64) -> Option<Self> {
        match self.0.checked_add(cycles) {
            Some(value) => Some(Self(value)),
            None => None,
        }
    }

    /// Rewinds by `cycles`, returning `None` rather than wrapping.
    #[must_use]
    pub const fn checked_sub(self, cycles: u64) -> Option<Self> {
        match self.0.checked_sub(cycles) {
            Some(value) => Some(Self(value)),
            None => None,
        }
    }

    /// Advances by `cycles`, saturating at [`Self::MAX`].
    #[must_use]
    pub const fn saturating_add(self, cycles: u64) -> Self {
        Self(self.0.saturating_add(cycles))
    }

    /// Returns elapsed cycles when `self` is not earlier than `earlier`.
    #[must_use]
    pub const fn elapsed_since(self, earlier: Self) -> Option<u64> {
        self.0.checked_sub(earlier.0)
    }
}

impl From<u64> for GuestCycle {
    fn from(value: u64) -> Self {
        Self::new(value)
    }
}

impl From<GuestCycle> for u64 {
    fn from(value: GuestCycle) -> Self {
        value.get()
    }
}

impl From<BeU64> for GuestCycle {
    fn from(value: BeU64) -> Self {
        Self::from_be(value)
    }
}

impl From<GuestCycle> for BeU64 {
    fn from(value: GuestCycle) -> Self {
        value.to_be()
    }
}

impl fmt::Display for GuestCycle {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cycle_arithmetic_is_explicit_at_boundaries() {
        assert_eq!(
            GuestCycle::new(10).checked_add(5),
            Some(GuestCycle::new(15))
        );
        assert_eq!(GuestCycle::MAX.checked_add(1), None);
        assert_eq!(GuestCycle::ZERO.checked_sub(1), None);
        assert_eq!(GuestCycle::MAX.saturating_add(1), GuestCycle::MAX);
    }

    #[test]
    fn elapsed_time_rejects_reversed_timestamps() {
        assert_eq!(
            GuestCycle::new(15).elapsed_since(GuestCycle::new(10)),
            Some(5)
        );
        assert_eq!(GuestCycle::new(10).elapsed_since(GuestCycle::new(15)), None);
    }
}
