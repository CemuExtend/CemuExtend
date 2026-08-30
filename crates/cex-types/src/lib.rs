//! Guest-visible scalar and address types for the CemuExtend core.
//!
//! The Wii U guest is big-endian while supported hosts may not be. The endian
//! wrappers in this crate store bytes in guest order and only expose explicit
//! native-value and byte-array conversions. [`GuestValue`] provides checked
//! byte-slice I/O, avoiding alignment assumptions and pointer casts.
//!
//! # Safety policy
//!
//! This crate forbids unsafe Rust. Types intended for guest ABI fields have a
//! documented representation and tests for their size and alignment. A Rust
//! aggregate still needs an explicit guest-layout definition; `repr(C)` alone
//! is not a substitute for checking the Cafe OS ABI layout.

#![forbid(unsafe_code)]
#![warn(missing_docs)]

mod address;
mod cycle;
mod endian;

pub use address::{GUEST_ADDRESS_SPACE_SIZE, GuestAddress, GuestAddressError, GuestRange};
pub use cycle::GuestCycle;
pub use endian::{
    BeF32, BeF64, BeI8, BeI16, BeI32, BeI64, BeU8, BeU16, BeU32, BeU64, GuestValue, GuestValueError,
};
