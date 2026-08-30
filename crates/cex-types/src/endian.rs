use core::cmp::Ordering;
use core::fmt;

/// An error returned when a guest scalar does not fit in the supplied buffer.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GuestValueError {
    /// The buffer was shorter than the encoded guest value.
    BufferTooSmall {
        /// Number of bytes required by the value.
        required: usize,
        /// Number of bytes provided by the caller.
        actual: usize,
    },
}

impl fmt::Display for GuestValueError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::BufferTooSmall { required, actual } => write!(
                formatter,
                "guest value requires {required} bytes, but the buffer has {actual}"
            ),
        }
    }
}

impl core::error::Error for GuestValueError {}

/// A fixed-width value that can be copied to and from guest-ordered bytes.
///
/// Implementations read or write exactly [`Self::SIZE`] bytes at the beginning
/// of a slice. A longer slice is accepted and its remaining bytes are untouched.
pub trait GuestValue: Copy + Sized {
    /// Encoded width of the value in bytes.
    const SIZE: usize;

    /// Reads a value from the first [`Self::SIZE`] bytes of `bytes`.
    fn read_from(bytes: &[u8]) -> Result<Self, GuestValueError>;

    /// Writes this value to the first [`Self::SIZE`] bytes of `bytes`.
    fn write_to(self, bytes: &mut [u8]) -> Result<(), GuestValueError>;
}

macro_rules! define_big_endian_integer {
    ($name:ident, $native:ty, $size:expr, $serde_name:literal, $doc:literal) => {
        #[doc = $doc]
        ///
        /// The representation has the size and alignment of the corresponding
        /// native integer, but its in-memory bytes are always big-endian.
        #[repr(transparent)]
        #[derive(Clone, Copy, Default, Eq, Hash, PartialEq)]
        #[cfg_attr(feature = "serde", derive(serde::Deserialize, serde::Serialize))]
        #[cfg_attr(feature = "serde", serde(from = $serde_name, into = $serde_name))]
        pub struct $name($native);

        impl $name {
            /// Encoded width in bytes.
            pub const SIZE: usize = $size;

            /// Encodes a native value for guest memory.
            #[must_use]
            pub const fn new(value: $native) -> Self {
                Self(value.to_be())
            }

            /// Decodes the stored guest value to native byte order.
            #[must_use]
            pub const fn get(self) -> $native {
                <$native>::from_be(self.0)
            }

            /// Constructs a value from its exact guest-order bytes.
            #[must_use]
            pub const fn from_be_bytes(bytes: [u8; $size]) -> Self {
                Self(<$native>::from_ne_bytes(bytes))
            }

            /// Returns the exact bytes to place in guest memory.
            #[must_use]
            pub const fn to_be_bytes(self) -> [u8; $size] {
                self.0.to_ne_bytes()
            }
        }

        impl From<$native> for $name {
            fn from(value: $native) -> Self {
                Self::new(value)
            }
        }

        impl From<$name> for $native {
            fn from(value: $name) -> Self {
                value.get()
            }
        }

        impl fmt::Debug for $name {
            fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter
                    .debug_tuple(stringify!($name))
                    .field(&self.get())
                    .finish()
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                self.get().fmt(formatter)
            }
        }

        impl Ord for $name {
            fn cmp(&self, other: &Self) -> Ordering {
                self.get().cmp(&other.get())
            }
        }

        impl PartialOrd for $name {
            fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
                Some(self.cmp(other))
            }
        }

        impl GuestValue for $name {
            const SIZE: usize = Self::SIZE;

            fn read_from(bytes: &[u8]) -> Result<Self, GuestValueError> {
                let source = bytes
                    .get(..Self::SIZE)
                    .ok_or(GuestValueError::BufferTooSmall {
                        required: Self::SIZE,
                        actual: bytes.len(),
                    })?;
                let mut encoded = [0_u8; $size];
                encoded.copy_from_slice(source);
                Ok(Self::from_be_bytes(encoded))
            }

            fn write_to(self, bytes: &mut [u8]) -> Result<(), GuestValueError> {
                let actual = bytes.len();
                let destination =
                    bytes
                        .get_mut(..Self::SIZE)
                        .ok_or(GuestValueError::BufferTooSmall {
                            required: Self::SIZE,
                            actual,
                        })?;
                destination.copy_from_slice(&self.to_be_bytes());
                Ok(())
            }
        }
    };
}

define_big_endian_integer!(BeU8, u8, 1, "u8", "An unsigned 8-bit guest integer.");
define_big_endian_integer!(BeI8, i8, 1, "i8", "A signed 8-bit guest integer.");
define_big_endian_integer!(
    BeU16,
    u16,
    2,
    "u16",
    "An unsigned 16-bit big-endian guest integer."
);
define_big_endian_integer!(
    BeI16,
    i16,
    2,
    "i16",
    "A signed 16-bit big-endian guest integer."
);
define_big_endian_integer!(
    BeU32,
    u32,
    4,
    "u32",
    "An unsigned 32-bit big-endian guest integer."
);
define_big_endian_integer!(
    BeI32,
    i32,
    4,
    "i32",
    "A signed 32-bit big-endian guest integer."
);
define_big_endian_integer!(
    BeU64,
    u64,
    8,
    "u64",
    "An unsigned 64-bit big-endian guest integer."
);
define_big_endian_integer!(
    BeI64,
    i64,
    8,
    "i64",
    "A signed 64-bit big-endian guest integer."
);

macro_rules! define_big_endian_float {
    ($name:ident, $float:ty, $bits:ty, $size:expr, $serde_name:literal, $doc:literal) => {
        #[doc = $doc]
        ///
        /// Equality and hashing compare the IEEE bit pattern. This preserves
        /// signed zero and NaN payloads, which are observable guest state. With
        /// the `serde` feature, the native-order bit pattern is serialized as
        /// an integer so every possible guest value round-trips losslessly.
        #[repr(transparent)]
        #[derive(Clone, Copy, Default, Eq, Hash, PartialEq)]
        #[cfg_attr(feature = "serde", derive(serde::Deserialize, serde::Serialize))]
        #[cfg_attr(feature = "serde", serde(from = $serde_name, into = $serde_name))]
        pub struct $name($bits);

        impl $name {
            /// Encoded width in bytes.
            pub const SIZE: usize = $size;

            /// Encodes a native floating-point value for guest memory.
            #[must_use]
            pub const fn new(value: $float) -> Self {
                Self(value.to_bits().to_be())
            }

            /// Decodes the stored guest value without changing its IEEE bits.
            #[must_use]
            pub const fn get(self) -> $float {
                <$float>::from_bits(<$bits>::from_be(self.0))
            }

            /// Constructs a value from its exact guest-order bytes.
            #[must_use]
            pub const fn from_be_bytes(bytes: [u8; $size]) -> Self {
                Self(<$bits>::from_ne_bytes(bytes))
            }

            /// Returns the exact bytes to place in guest memory.
            #[must_use]
            pub const fn to_be_bytes(self) -> [u8; $size] {
                self.0.to_ne_bytes()
            }

            /// Returns the native-order IEEE bit pattern.
            #[must_use]
            pub const fn to_bits(self) -> $bits {
                <$bits>::from_be(self.0)
            }

            /// Encodes a native-order IEEE bit pattern for guest memory.
            #[must_use]
            pub const fn from_bits(bits: $bits) -> Self {
                Self(bits.to_be())
            }
        }

        impl From<$float> for $name {
            fn from(value: $float) -> Self {
                Self::new(value)
            }
        }

        impl From<$name> for $float {
            fn from(value: $name) -> Self {
                value.get()
            }
        }

        impl From<$bits> for $name {
            fn from(bits: $bits) -> Self {
                Self::from_bits(bits)
            }
        }

        impl From<$name> for $bits {
            fn from(value: $name) -> Self {
                value.to_bits()
            }
        }

        impl fmt::Debug for $name {
            fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter
                    .debug_tuple(stringify!($name))
                    .field(&self.get())
                    .finish()
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                self.get().fmt(formatter)
            }
        }

        impl GuestValue for $name {
            const SIZE: usize = Self::SIZE;

            fn read_from(bytes: &[u8]) -> Result<Self, GuestValueError> {
                let source = bytes
                    .get(..Self::SIZE)
                    .ok_or(GuestValueError::BufferTooSmall {
                        required: Self::SIZE,
                        actual: bytes.len(),
                    })?;
                let mut encoded = [0_u8; $size];
                encoded.copy_from_slice(source);
                Ok(Self::from_be_bytes(encoded))
            }

            fn write_to(self, bytes: &mut [u8]) -> Result<(), GuestValueError> {
                let actual = bytes.len();
                let destination =
                    bytes
                        .get_mut(..Self::SIZE)
                        .ok_or(GuestValueError::BufferTooSmall {
                            required: Self::SIZE,
                            actual,
                        })?;
                destination.copy_from_slice(&self.to_be_bytes());
                Ok(())
            }
        }
    };
}

define_big_endian_float!(
    BeF32,
    f32,
    u32,
    4,
    "u32",
    "A 32-bit IEEE 754 big-endian guest value."
);
define_big_endian_float!(
    BeF64,
    f64,
    u64,
    8,
    "u64",
    "A 64-bit IEEE 754 big-endian guest value."
);

#[cfg(test)]
mod tests {
    use core::mem::{align_of, size_of};

    use super::*;

    #[test]
    fn integer_known_vectors_use_big_endian_bytes() {
        assert_eq!(BeU8::new(0xa5).to_be_bytes(), [0xa5]);
        assert_eq!(BeI8::new(-2).to_be_bytes(), [0xfe]);
        assert_eq!(BeU16::new(0x1234).to_be_bytes(), [0x12, 0x34]);
        assert_eq!(BeI16::new(-2).to_be_bytes(), [0xff, 0xfe]);
        assert_eq!(
            BeU32::new(0x1234_5678).to_be_bytes(),
            [0x12, 0x34, 0x56, 0x78]
        );
        assert_eq!(BeI32::new(-2).to_be_bytes(), [0xff, 0xff, 0xff, 0xfe]);
        assert_eq!(
            BeU64::new(0x0123_4567_89ab_cdef).to_be_bytes(),
            [0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef]
        );
        assert_eq!(
            BeI64::new(-2).to_be_bytes(),
            [0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe]
        );
    }

    #[test]
    fn integer_round_trips_cover_edges_and_generated_values() {
        for value in [0, 1, u32::MAX / 2, u32::MAX - 1, u32::MAX] {
            assert_eq!(BeU32::from_be_bytes(value.to_be_bytes()).get(), value);
            assert_eq!(BeU32::new(value).to_be_bytes(), value.to_be_bytes());
        }

        let mut value = 0x4d59_5df4_d0f3_3173_u64;
        for _ in 0..10_000 {
            value = value
                .wrapping_mul(6_364_136_223_846_793_005)
                .wrapping_add(1_442_695_040_888_963_407);
            assert_eq!(BeU64::from_be_bytes(value.to_be_bytes()).get(), value);
            assert_eq!(BeI64::new(value.cast_signed()).get(), value.cast_signed());
        }
    }

    #[test]
    fn floating_point_round_trips_every_bit_pattern_sampled() {
        assert_eq!(BeF32::new(1.0).to_be_bytes(), [0x3f, 0x80, 0x00, 0x00]);
        assert_eq!(
            BeF64::new(-1.0).to_be_bytes(),
            [0xbf, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
        );

        let important = [
            0_u64,
            1,
            (-0.0_f64).to_bits(),
            f64::INFINITY.to_bits(),
            f64::NEG_INFINITY.to_bits(),
            f64::NAN.to_bits(),
            0x7ff8_1234_5678_9abc,
            u64::MAX,
        ];
        for bits in important {
            let encoded = BeF64::from_bits(bits);
            assert_eq!(encoded.to_bits(), bits);
            assert_eq!(BeF64::from_be_bytes(encoded.to_be_bytes()).to_bits(), bits);
        }

        let mut bits = 0x853c_49e6_748f_ea9b_u64;
        for _ in 0..10_000 {
            bits ^= bits << 13;
            bits ^= bits >> 7;
            bits ^= bits << 17;
            assert_eq!(BeF64::new(f64::from_bits(bits)).to_bits(), bits);
        }
    }

    #[test]
    fn guest_value_checks_lengths_and_preserves_suffix() {
        let expected = GuestValueError::BufferTooSmall {
            required: 4,
            actual: 3,
        };
        assert_eq!(BeU32::read_from(&[0; 3]), Err(expected));

        let mut short = [0_u8; 3];
        assert_eq!(BeU32::new(7).write_to(&mut short), Err(expected));

        let mut destination = [0xaa; 6];
        BeU32::new(0x1234_5678)
            .write_to(&mut destination)
            .expect("four bytes fit");
        assert_eq!(destination, [0x12, 0x34, 0x56, 0x78, 0xaa, 0xaa]);
        assert_eq!(
            BeU32::read_from(&destination)
                .expect("four bytes available")
                .get(),
            0x1234_5678
        );
    }

    #[test]
    fn integer_order_is_native_value_order_not_encoded_host_order() {
        let mut values = [
            BeI32::new(256),
            BeI32::new(-1),
            BeI32::new(1),
            BeI32::new(0),
        ];
        values.sort();
        assert_eq!(values.map(BeI32::get), [-1, 0, 1, 256]);
    }

    #[test]
    fn abi_size_and_alignment_match_native_scalars() {
        assert_eq!(size_of::<BeU8>(), size_of::<u8>());
        assert_eq!(align_of::<BeU8>(), align_of::<u8>());
        assert_eq!(size_of::<BeU16>(), size_of::<u16>());
        assert_eq!(align_of::<BeU16>(), align_of::<u16>());
        assert_eq!(size_of::<BeU32>(), size_of::<u32>());
        assert_eq!(align_of::<BeU32>(), align_of::<u32>());
        assert_eq!(size_of::<BeU64>(), size_of::<u64>());
        assert_eq!(align_of::<BeU64>(), align_of::<u64>());
        assert_eq!(size_of::<BeF32>(), size_of::<f32>());
        assert_eq!(align_of::<BeF32>(), align_of::<f32>());
        assert_eq!(size_of::<BeF64>(), size_of::<f64>());
        assert_eq!(align_of::<BeF64>(), align_of::<f64>());
    }

    #[cfg(feature = "serde")]
    #[test]
    fn serde_uses_decoded_values() {
        let value = BeU32::new(0x1234_5678);
        let encoded = serde_json::to_string(&value).expect("serializes");
        assert_eq!(encoded, "305419896");
        assert_eq!(
            serde_json::from_str::<BeU32>(&encoded)
                .expect("deserializes")
                .get(),
            0x1234_5678
        );

        let nan_bits = 0x7ff8_1234_5678_9abc;
        let nan = BeF64::from_bits(nan_bits);
        let encoded = serde_json::to_string(&nan).expect("all float bits serialize");
        assert_eq!(
            serde_json::from_str::<BeF64>(&encoded)
                .expect("all float bits deserialize")
                .to_bits(),
            nan_bits
        );
    }
}
