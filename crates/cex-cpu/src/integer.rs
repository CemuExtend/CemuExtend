pub(crate) fn sign_extend_i16(value: i16) -> u32 {
    i32::from(value).cast_unsigned()
}

pub(crate) fn sign_extend_u16(value: u16) -> u32 {
    i32::from(value.cast_signed()).cast_unsigned()
}

pub(crate) fn truncate_u32_to_u8(value: u32) -> u8 {
    u8::try_from(value & u32::from(u8::MAX)).expect("masked value fits in u8")
}

pub(crate) fn truncate_u32_to_u16(value: u32) -> u16 {
    u16::try_from(value & u32::from(u16::MAX)).expect("masked value fits in u16")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extension_and_truncation_preserve_powerpc_bit_patterns() {
        assert_eq!(sign_extend_i16(i16::MIN), 0xffff_8000);
        assert_eq!(sign_extend_u16(0x8000), 0xffff_8000);
        assert_eq!(truncate_u32_to_u8(0x1234_abcd), 0xcd);
        assert_eq!(truncate_u32_to_u16(0x1234_abcd), 0xabcd);
    }
}
