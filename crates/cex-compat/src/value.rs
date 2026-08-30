use std::{fmt, str::FromStr};

use serde::{Deserialize, Deserializer, Serialize, Serializer, de};

use crate::{CompatError, Result};

macro_rules! decimal_wrapper {
    ($name:ident, $inner:ty, $doc:literal, $field_doc:literal) => {
        #[derive(Clone, Copy, Debug, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]
        #[doc = $doc]
        pub struct $name(#[doc = $field_doc] pub $inner);

        impl From<$inner> for $name {
            fn from(value: $inner) -> Self {
                Self(value)
            }
        }

        impl Serialize for $name {
            fn serialize<S>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error>
            where
                S: Serializer,
            {
                serializer.serialize_str(&self.0.to_string())
            }
        }

        impl<'de> Deserialize<'de> for $name {
            fn deserialize<D>(deserializer: D) -> std::result::Result<Self, D::Error>
            where
                D: Deserializer<'de>,
            {
                let text = String::deserialize(deserializer)?;
                if text != "0" && (text.starts_with('0') || text.starts_with("-0")) {
                    return Err(de::Error::custom(
                        "decimal values must not contain leading zeroes",
                    ));
                }
                text.parse::<$inner>().map(Self).map_err(de::Error::custom)
            }
        }
    };
}

decimal_wrapper!(
    DecimalU64,
    u64,
    "An unsigned integer encoded in JSON as a canonical base-10 string.",
    "The native unsigned value represented by the portable decimal string."
);
decimal_wrapper!(
    DecimalI64,
    i64,
    "A signed integer encoded in JSON as a canonical base-10 string.",
    "The native signed value represented by the portable decimal string."
);

macro_rules! hex_wrapper {
    ($name:ident, $inner:ty, $digits:expr, $doc:literal, $field_doc:literal) => {
        #[derive(Clone, Copy, Debug, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]
        #[doc = $doc]
        pub struct $name(#[doc = $field_doc] pub $inner);

        impl From<$inner> for $name {
            fn from(value: $inner) -> Self {
                Self(value)
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(formatter, concat!("0x{:0", $digits, "x}"), self.0)
            }
        }

        impl Serialize for $name {
            fn serialize<S>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error>
            where
                S: Serializer,
            {
                serializer.serialize_str(&self.to_string())
            }
        }

        impl<'de> Deserialize<'de> for $name {
            fn deserialize<D>(deserializer: D) -> std::result::Result<Self, D::Error>
            where
                D: Deserializer<'de>,
            {
                let text = String::deserialize(deserializer)?;
                let expected_len = 2 + $digits;
                if text.len() != expected_len
                    || !text.starts_with("0x")
                    || !text[2..]
                        .bytes()
                        .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
                {
                    return Err(de::Error::custom(format!(
                        "expected fixed-width lowercase hexadecimal with {expected_len} characters"
                    )));
                }
                <$inner>::from_str_radix(&text[2..], 16)
                    .map(Self)
                    .map_err(de::Error::custom)
            }
        }
    };
}

hex_wrapper!(
    HexU32,
    u32,
    8,
    "A 32-bit value encoded in JSON as eight lowercase hexadecimal digits with a `0x` prefix.",
    "The native 32-bit value represented by the fixed-width hexadecimal string."
);
hex_wrapper!(
    HexU64,
    u64,
    16,
    "A 64-bit value encoded in JSON as sixteen lowercase hexadecimal digits with a `0x` prefix.",
    "The native 64-bit value represented by the fixed-width hexadecimal string."
);

#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
/// A validated lowercase hexadecimal SHA-256 digest used by the oracle.
pub struct Sha256Digest(String);

impl Sha256Digest {
    /// Number of lowercase hexadecimal characters in a SHA-256 digest.
    pub const HEX_LENGTH: usize = 64;

    /// Validates and stores a canonical lowercase hexadecimal SHA-256 digest.
    pub fn parse(value: impl Into<String>) -> Result<Self> {
        let value = value.into();
        if value.len() != Self::HEX_LENGTH
            || !value
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
        {
            return Err(CompatError::InvalidTrace(
                "SHA-256 digests must be 64 lowercase hexadecimal characters".to_owned(),
            ));
        }
        Ok(Self(value))
    }

    /// Returns the canonical lowercase hexadecimal digest text.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for Sha256Digest {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl FromStr for Sha256Digest {
    type Err = CompatError;

    fn from_str(value: &str) -> Result<Self> {
        Self::parse(value)
    }
}

impl Serialize for Sha256Digest {
    fn serialize<S>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_str(&self.0)
    }
}

impl<'de> Deserialize<'de> for Sha256Digest {
    fn deserialize<D>(deserializer: D) -> std::result::Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let value = String::deserialize(deserializer)?;
        Self::parse(value).map_err(de::Error::custom)
    }
}

/// Scalars accepted in deterministic event fields.
///
/// Large integers use canonical decimal strings. Guest values and IEEE raw
/// bits use fixed-width hexadecimal. There is intentionally no byte-array,
/// float, object, or arbitrary-JSON variant.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", content = "value", rename_all = "snake_case")]
pub enum TraceValue {
    /// A bounded, path-free and URL-free text value.
    Text(String),
    /// A deterministic boolean value.
    Boolean(bool),
    /// A signed integer represented by a canonical decimal string.
    Signed(DecimalI64),
    /// An unsigned integer represented by a canonical decimal string.
    Unsigned(DecimalU64),
    /// A fixed-width 32-bit guest value.
    Hex32(HexU32),
    /// A fixed-width 64-bit value, including raw IEEE bit patterns.
    Hex64(HexU64),
    /// A canonical SHA-256 digest.
    Sha256(Sha256Digest),
    /// A stable sentinel replacing a secret field's original value.
    Redacted,
}
