//! Sparse, permission-checked storage for the Wii U's 32-bit guest address space.
//!
//! Mapping address space is cheap: backing pages are allocated only when a write
//! makes a page non-zero. The implementation intentionally uses safe Rust and
//! does not expose host pointers. A later JIT layer can provide a separate,
//! carefully audited executable-memory backend.

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use std::collections::BTreeMap;

use bitflags::bitflags;
use cex_types::{BeU16, BeU32, BeU64, GUEST_ADDRESS_SPACE_SIZE, GuestAddress, GuestValue};
use sha2::{Digest, Sha256};
use thiserror::Error;

/// Size of a guest memory page in bytes.
pub const PAGE_SIZE: u64 = 4 * 1024;

/// Maximum allocation performed by [`GuestMemory::read_vec`].
///
/// Larger reads should use [`GuestMemory::read`] with a caller-managed,
/// explicitly resource-bounded buffer.
pub const MAX_OWNED_READ_LEN: usize = 16 * 1024 * 1024;

/// Maximum encoded width accepted by [`GuestMemory::read_value`] and
/// [`GuestMemory::write_value`].
///
/// Guest scalar access is intentionally limited to 128 bits. Wider values
/// should use [`GuestMemory::read`] or [`GuestMemory::write`] with a
/// caller-managed buffer.
pub const MAX_GUEST_VALUE_SIZE: usize = 16;

const PAGE_BYTES: usize = 4 * 1024;
const PAGE_BYTES_U32: u32 = 4 * 1024;

bitflags! {
    /// Permissions attached to a mapped guest range.
    #[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq)]
    pub struct Permissions: u8 {
        /// The range is mapped but cannot be accessed by the guest.
        const NONE = 0;
        /// The guest may read from the range.
        const READ = 1 << 0;
        /// The guest may write to the range.
        const WRITE = 1 << 1;
        /// The CPU may fetch instructions from the range.
        const EXECUTE = 1 << 2;
    }
}

/// The operation whose memory access is being validated.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum AccessType {
    /// Data read.
    Read,
    /// Data write.
    Write,
    /// Instruction fetch.
    Execute,
    /// Address-space management such as unmapping or protection changes.
    Manage,
}

impl AccessType {
    const fn required_permission(self) -> Permissions {
        match self {
            Self::Read => Permissions::READ,
            Self::Write => Permissions::WRITE,
            Self::Execute => Permissions::EXECUTE,
            Self::Manage => Permissions::empty(),
        }
    }
}

/// A canonical description of a contiguous mapped range.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MemoryMapping {
    /// First byte in the mapped range.
    pub start: GuestAddress,
    /// Length of the range in bytes.
    pub len: u64,
    /// Permissions applying to the entire range.
    pub permissions: Permissions,
}

/// A deterministic guest-memory access or address-space management fault.
#[derive(Clone, Copy, Debug, Error, Eq, PartialEq)]
pub enum MemoryFault {
    /// A range operation was requested with no bytes.
    #[error("guest memory ranges must not be empty")]
    ZeroLength,

    /// The range's first byte did not meet the required alignment.
    #[error("guest address {address:?} is not aligned to {alignment} bytes")]
    UnalignedAddress {
        /// Address supplied by the caller.
        address: GuestAddress,
        /// Required alignment in bytes.
        alignment: u64,
    },

    /// The range length did not meet the required alignment.
    #[error("guest memory length {length} is not aligned to {alignment} bytes")]
    UnalignedLength {
        /// Length supplied by the caller.
        length: u64,
        /// Required alignment in bytes.
        alignment: u64,
    },

    /// The requested range wrapped or extended beyond the 4 GiB guest space.
    #[error("guest range starting at {start:?} with length {len} exceeds the address space")]
    AddressOverflow {
        /// First byte supplied by the caller.
        start: GuestAddress,
        /// Length supplied by the caller.
        len: u64,
    },

    /// A new mapping overlaps an existing mapping.
    #[error("guest mapping overlaps mapped byte at {address:?}")]
    Overlap {
        /// First overlapping guest byte.
        address: GuestAddress,
    },

    /// An operation reached a page that is not mapped.
    #[error("{access:?} access reached unmapped guest byte {address:?}")]
    Unmapped {
        /// First unmapped byte reached by the operation.
        address: GuestAddress,
        /// Operation being performed.
        access: AccessType,
    },

    /// A mapped range did not grant the permission required by an operation.
    #[error("{access:?} access denied at guest byte {address:?}; mapping has {permissions:?}")]
    PermissionDenied {
        /// First denied guest byte.
        address: GuestAddress,
        /// Operation being performed.
        access: AccessType,
        /// Permissions on the containing mapping.
        permissions: Permissions,
    },

    /// A [`GuestValue`] implementation rejected an exactly sized byte buffer.
    #[error("guest value codec rejected an exactly sized buffer")]
    InvalidGuestValue,

    /// A guest value exceeded the maximum width supported by scalar access.
    #[error("guest value width of {requested} bytes exceeds the {maximum}-byte scalar limit")]
    GuestValueTooLarge {
        /// Encoded width declared by the [`GuestValue`] implementation.
        requested: usize,
        /// Maximum width accepted by typed scalar access.
        maximum: usize,
    },

    /// An owned convenience read exceeded its hard allocation limit.
    #[error("owned guest read of {requested} bytes exceeds the {maximum}-byte limit")]
    OwnedReadTooLarge {
        /// Requested output allocation in bytes.
        requested: usize,
        /// Maximum allocation accepted by [`GuestMemory::read_vec`].
        maximum: usize,
    },

    /// The host allocator rejected an otherwise bounded owned buffer.
    #[error("host allocation failed for an owned guest-memory buffer of {len} bytes")]
    HostAllocationFailed {
        /// Requested output allocation in bytes.
        len: usize,
    },
}

#[derive(Clone, Copy, Debug)]
struct Mapping {
    end_page: u32,
    permissions: Permissions,
}

#[derive(Clone, Copy, Debug)]
struct ByteRange {
    start: u64,
    end: u64,
}

impl ByteRange {
    fn start_page(self) -> u32 {
        page_index(self.start)
    }

    fn end_page(self) -> u32 {
        page_index(self.end)
    }
}

fn page_index(byte_offset: u64) -> u32 {
    u32::try_from(byte_offset / PAGE_SIZE)
        .expect("a validated guest byte offset has a page index that fits in u32")
}

fn page_offset(byte_offset: u64) -> usize {
    usize::try_from(byte_offset % PAGE_SIZE).expect("an offset within a guest page fits in usize")
}

fn guest_address(byte_offset: u64) -> GuestAddress {
    GuestAddress::new(
        u32::try_from(byte_offset)
            .expect("an address visited within a validated guest range fits in u32"),
    )
}

fn chunk_len_u64(chunk_len: usize) -> u64 {
    u64::try_from(chunk_len).expect("a chunk no larger than one guest page fits in u64")
}

/// Sparse storage and page permissions for the 4 GiB guest address space.
#[derive(Debug, Default)]
pub struct GuestMemory {
    // Key and end are page numbers. `end_page` is exclusive and may equal
    // GUEST_ADDRESS_SPACE_SIZE / PAGE_SIZE.
    mappings: BTreeMap<u32, Mapping>,
    // Only logically non-zero pages are resident, which makes the content hash
    // independent of whether a caller previously wrote zeroes.
    pages: BTreeMap<u32, Box<[u8; PAGE_BYTES]>>,
}

impl GuestMemory {
    /// Creates an empty address space.
    pub fn new() -> Self {
        Self::default()
    }

    /// Maps a page-aligned range without allocating backing storage.
    pub fn map(
        &mut self,
        start: GuestAddress,
        len: u64,
        permissions: Permissions,
    ) -> Result<(), MemoryFault> {
        let range = Self::page_range(start, len)?;
        let start_page = range.start_page();
        let end_page = range.end_page();

        if let Some((&existing_start, mapping)) = self.mappings.range(..=start_page).next_back()
            && mapping.end_page > start_page
        {
            let overlap = start_page.max(existing_start);
            return Err(MemoryFault::Overlap {
                address: Self::page_address(overlap),
            });
        }

        if let Some((&existing_start, _)) = self.mappings.range(start_page..).next()
            && existing_start < end_page
        {
            return Err(MemoryFault::Overlap {
                address: Self::page_address(existing_start),
            });
        }

        self.mappings.insert(
            start_page,
            Mapping {
                end_page,
                permissions,
            },
        );
        self.coalesce_mappings();
        Ok(())
    }

    /// Removes a fully mapped, page-aligned range and discards its contents.
    ///
    /// Validation happens before mutation, so a range containing a hole leaves
    /// the address space unchanged.
    pub fn unmap(&mut self, start: GuestAddress, len: u64) -> Result<(), MemoryFault> {
        let range = Self::page_range(start, len)?;
        self.validate_access(range, AccessType::Manage)?;
        let start_page = range.start_page();
        let end_page = range.end_page();
        let overlapping = self.overlapping_mappings(start_page, end_page);

        for (mapping_start, mapping) in overlapping {
            self.mappings.remove(&mapping_start);
            if mapping_start < start_page {
                self.mappings.insert(
                    mapping_start,
                    Mapping {
                        end_page: start_page,
                        permissions: mapping.permissions,
                    },
                );
            }
            if mapping.end_page > end_page {
                self.mappings.insert(
                    end_page,
                    Mapping {
                        end_page: mapping.end_page,
                        permissions: mapping.permissions,
                    },
                );
            }
        }

        self.pages
            .retain(|page, _| *page < start_page || *page >= end_page);
        self.coalesce_mappings();
        Ok(())
    }

    /// Replaces permissions on a fully mapped, page-aligned range.
    ///
    /// Validation happens before mutation, so a range containing a hole leaves
    /// existing protections unchanged.
    pub fn protect(
        &mut self,
        start: GuestAddress,
        len: u64,
        permissions: Permissions,
    ) -> Result<(), MemoryFault> {
        let range = Self::page_range(start, len)?;
        self.validate_access(range, AccessType::Manage)?;
        let start_page = range.start_page();
        let end_page = range.end_page();
        let overlapping = self.overlapping_mappings(start_page, end_page);

        for (mapping_start, mapping) in overlapping {
            self.mappings.remove(&mapping_start);
            if mapping_start < start_page {
                self.mappings.insert(
                    mapping_start,
                    Mapping {
                        end_page: start_page,
                        permissions: mapping.permissions,
                    },
                );
            }

            let protected_start = mapping_start.max(start_page);
            let protected_end = mapping.end_page.min(end_page);
            self.mappings.insert(
                protected_start,
                Mapping {
                    end_page: protected_end,
                    permissions,
                },
            );

            if mapping.end_page > end_page {
                self.mappings.insert(
                    end_page,
                    Mapping {
                        end_page: mapping.end_page,
                        permissions: mapping.permissions,
                    },
                );
            }
        }

        self.coalesce_mappings();
        Ok(())
    }

    /// Checks that every byte in a non-empty range is mapped and permits an access.
    pub fn check_access(
        &self,
        start: GuestAddress,
        len: u64,
        access: AccessType,
    ) -> Result<(), MemoryFault> {
        let range = Self::byte_range(start, len)?;
        self.validate_access(range, access)
    }

    /// Reads bytes from guest memory after validating the complete range.
    pub fn read(&self, start: GuestAddress, output: &mut [u8]) -> Result<(), MemoryFault> {
        self.read_with_access(start, output, AccessType::Read)
    }

    /// Returns an owned byte vector read from guest memory.
    ///
    /// The guest range and permissions are checked before allocation. This
    /// convenience method is capped at [`MAX_OWNED_READ_LEN`]; callers needing
    /// larger reads must provide a suitably bounded buffer to [`Self::read`].
    pub fn read_vec(&self, start: GuestAddress, len: usize) -> Result<Vec<u8>, MemoryFault> {
        let range = Self::buffer_range(start, len)?;
        self.validate_access(range, AccessType::Read)?;
        if len > MAX_OWNED_READ_LEN {
            return Err(MemoryFault::OwnedReadTooLarge {
                requested: len,
                maximum: MAX_OWNED_READ_LEN,
            });
        }
        let mut output = Vec::new();
        output
            .try_reserve_exact(len)
            .map_err(|_| MemoryFault::HostAllocationFailed { len })?;
        output.resize(len, 0);
        self.read(start, &mut output)?;
        Ok(output)
    }

    /// Writes bytes after validating the complete range.
    ///
    /// A cross-page fault never leaves a partially written prefix behind.
    pub fn write(&mut self, start: GuestAddress, input: &[u8]) -> Result<(), MemoryFault> {
        let range = Self::buffer_range(start, input.len())?;
        self.validate_access(range, AccessType::Write)?;

        let mut cursor = range.start;
        let mut input_offset = 0;
        while cursor < range.end {
            let page_index = page_index(cursor);
            let page_offset = page_offset(cursor);
            let chunk_len =
                (PAGE_BYTES - page_offset).min(input.len().saturating_sub(input_offset));
            let source = &input[input_offset..input_offset + chunk_len];

            if self.pages.contains_key(&page_index) || source.iter().any(|byte| *byte != 0) {
                let page = self
                    .pages
                    .entry(page_index)
                    .or_insert_with(|| Box::new([0; PAGE_BYTES]));
                page[page_offset..page_offset + chunk_len].copy_from_slice(source);
                if page.iter().all(|byte| *byte == 0) {
                    self.pages.remove(&page_index);
                }
            }

            cursor += chunk_len_u64(chunk_len);
            input_offset += chunk_len;
        }
        Ok(())
    }

    /// Reads a fixed-size guest-endian value from a data-readable range.
    ///
    /// Values wider than [`MAX_GUEST_VALUE_SIZE`] are rejected before the guest
    /// range is inspected or a temporary buffer is allocated.
    pub fn read_value<T: GuestValue>(&self, start: GuestAddress) -> Result<T, MemoryFault> {
        self.read_value_with_access(start, AccessType::Read)
    }

    /// Writes a fixed-size guest-endian value to a data-writable range.
    ///
    /// Values wider than [`MAX_GUEST_VALUE_SIZE`] are rejected before the codec
    /// or guest range is inspected or a temporary buffer is allocated.
    pub fn write_value<T: GuestValue>(
        &mut self,
        start: GuestAddress,
        value: T,
    ) -> Result<(), MemoryFault> {
        let mut bytes = Self::guest_value_buffer::<T>()?;
        value
            .write_to(&mut bytes)
            .map_err(|_| MemoryFault::InvalidGuestValue)?;
        self.write(start, &bytes)
    }

    /// Reads one byte from a data-readable range.
    pub fn read_u8(&self, start: GuestAddress) -> Result<u8, MemoryFault> {
        let mut byte = [0];
        self.read(start, &mut byte)?;
        Ok(byte[0])
    }

    /// Reads a big-endian `u16` from a data-readable range.
    pub fn read_u16(&self, start: GuestAddress) -> Result<u16, MemoryFault> {
        self.read_value::<BeU16>(start).map(BeU16::get)
    }

    /// Reads a big-endian `u32` from a data-readable range.
    pub fn read_u32(&self, start: GuestAddress) -> Result<u32, MemoryFault> {
        self.read_value::<BeU32>(start).map(BeU32::get)
    }

    /// Reads a big-endian `u64` from a data-readable range.
    pub fn read_u64(&self, start: GuestAddress) -> Result<u64, MemoryFault> {
        self.read_value::<BeU64>(start).map(BeU64::get)
    }

    /// Writes one byte to a data-writable range.
    pub fn write_u8(&mut self, start: GuestAddress, value: u8) -> Result<(), MemoryFault> {
        self.write(start, &[value])
    }

    /// Writes a big-endian `u16` to a data-writable range.
    pub fn write_u16(&mut self, start: GuestAddress, value: u16) -> Result<(), MemoryFault> {
        self.write_value(start, BeU16::new(value))
    }

    /// Writes a big-endian `u32` to a data-writable range.
    pub fn write_u32(&mut self, start: GuestAddress, value: u32) -> Result<(), MemoryFault> {
        self.write_value(start, BeU32::new(value))
    }

    /// Writes a big-endian `u64` to a data-writable range.
    pub fn write_u64(&mut self, start: GuestAddress, value: u64) -> Result<(), MemoryFault> {
        self.write_value(start, BeU64::new(value))
    }

    /// Fetches a big-endian `u32` using execute permission, without requiring read permission.
    pub fn fetch_u32(&self, start: GuestAddress) -> Result<u32, MemoryFault> {
        self.read_value_with_access::<BeU32>(start, AccessType::Execute)
            .map(BeU32::get)
    }

    /// Returns canonical, ascending mappings. Adjacent equal protections are coalesced.
    pub fn mappings(&self) -> impl ExactSizeIterator<Item = MemoryMapping> + '_ {
        self.mappings
            .iter()
            .map(|(&start_page, mapping)| MemoryMapping {
                start: Self::page_address(start_page),
                len: u64::from(mapping.end_page - start_page) * PAGE_SIZE,
                permissions: mapping.permissions,
            })
    }

    /// Returns the number of logically mapped pages, including zero-filled pages.
    pub fn mapped_page_count(&self) -> u64 {
        self.mappings
            .iter()
            .map(|(&start, mapping)| u64::from(mapping.end_page - start))
            .sum()
    }

    /// Returns the number of physically resident, non-zero backing pages.
    pub fn resident_page_count(&self) -> usize {
        self.pages.len()
    }

    /// Hashes the logical memory state in a stable, allocation-history-independent format.
    ///
    /// The digest covers mapping boundaries, permissions, addresses of non-zero
    /// pages, and their complete contents. It intentionally does not depend on
    /// host endianness or collection insertion order.
    pub fn deterministic_hash(&self) -> [u8; 32] {
        let mut hash = Sha256::new();
        hash.update(b"CemuExtend guest memory v1\0");

        for (&start_page, mapping) in &self.mappings {
            hash.update(b"M");
            hash.update(start_page.to_be_bytes());
            hash.update(mapping.end_page.to_be_bytes());
            hash.update([mapping.permissions.bits()]);
        }

        for (&page_index, contents) in &self.pages {
            hash.update(b"P");
            hash.update(page_index.to_be_bytes());
            hash.update(contents.as_slice());
        }

        hash.finalize().into()
    }

    fn read_value_with_access<T: GuestValue>(
        &self,
        start: GuestAddress,
        access: AccessType,
    ) -> Result<T, MemoryFault> {
        let mut bytes = Self::guest_value_buffer::<T>()?;
        self.read_with_access(start, &mut bytes, access)?;
        T::read_from(&bytes).map_err(|_| MemoryFault::InvalidGuestValue)
    }

    fn guest_value_buffer<T: GuestValue>() -> Result<Vec<u8>, MemoryFault> {
        if T::SIZE == 0 {
            return Err(MemoryFault::ZeroLength);
        }
        if T::SIZE > MAX_GUEST_VALUE_SIZE {
            return Err(MemoryFault::GuestValueTooLarge {
                requested: T::SIZE,
                maximum: MAX_GUEST_VALUE_SIZE,
            });
        }

        let mut bytes = Vec::new();
        bytes
            .try_reserve_exact(T::SIZE)
            .map_err(|_| MemoryFault::HostAllocationFailed { len: T::SIZE })?;
        bytes.resize(T::SIZE, 0);
        Ok(bytes)
    }

    fn read_with_access(
        &self,
        start: GuestAddress,
        output: &mut [u8],
        access: AccessType,
    ) -> Result<(), MemoryFault> {
        let range = Self::buffer_range(start, output.len())?;
        self.validate_access(range, access)?;

        let mut cursor = range.start;
        let mut output_offset = 0;
        while cursor < range.end {
            let page_index = page_index(cursor);
            let page_offset = page_offset(cursor);
            let chunk_len =
                (PAGE_BYTES - page_offset).min(output.len().saturating_sub(output_offset));
            let target = &mut output[output_offset..output_offset + chunk_len];
            if let Some(page) = self.pages.get(&page_index) {
                target.copy_from_slice(&page[page_offset..page_offset + chunk_len]);
            } else {
                target.fill(0);
            }
            cursor += chunk_len_u64(chunk_len);
            output_offset += chunk_len;
        }
        Ok(())
    }

    fn validate_access(&self, range: ByteRange, access: AccessType) -> Result<(), MemoryFault> {
        let required = access.required_permission();
        let mut cursor = range.start;
        while cursor < range.end {
            let page = page_index(cursor);
            let Some((&mapping_start, mapping)) = self.mappings.range(..=page).next_back() else {
                return Err(MemoryFault::Unmapped {
                    address: guest_address(cursor),
                    access,
                });
            };
            if page < mapping_start || page >= mapping.end_page {
                return Err(MemoryFault::Unmapped {
                    address: guest_address(cursor),
                    access,
                });
            }
            if !mapping.permissions.contains(required) {
                return Err(MemoryFault::PermissionDenied {
                    address: guest_address(cursor),
                    access,
                    permissions: mapping.permissions,
                });
            }
            cursor = range.end.min(u64::from(mapping.end_page) * PAGE_SIZE);
        }
        Ok(())
    }

    fn page_range(start: GuestAddress, len: u64) -> Result<ByteRange, MemoryFault> {
        if u64::from(start.get()) % PAGE_SIZE != 0 {
            return Err(MemoryFault::UnalignedAddress {
                address: start,
                alignment: PAGE_SIZE,
            });
        }
        if !len.is_multiple_of(PAGE_SIZE) {
            return Err(MemoryFault::UnalignedLength {
                length: len,
                alignment: PAGE_SIZE,
            });
        }
        Self::byte_range(start, len)
    }

    fn buffer_range(start: GuestAddress, len: usize) -> Result<ByteRange, MemoryFault> {
        let len = u64::try_from(len).map_err(|_| MemoryFault::AddressOverflow {
            start,
            len: u64::MAX,
        })?;
        Self::byte_range(start, len)
    }

    fn byte_range(start: GuestAddress, len: u64) -> Result<ByteRange, MemoryFault> {
        if len == 0 {
            return Err(MemoryFault::ZeroLength);
        }
        let start_u64 = u64::from(start.get());
        let Some(end) = start_u64.checked_add(len) else {
            return Err(MemoryFault::AddressOverflow { start, len });
        };
        if end > GUEST_ADDRESS_SPACE_SIZE {
            return Err(MemoryFault::AddressOverflow { start, len });
        }
        Ok(ByteRange {
            start: start_u64,
            end,
        })
    }

    fn overlapping_mappings(&self, start_page: u32, end_page: u32) -> Vec<(u32, Mapping)> {
        self.mappings
            .iter()
            .filter_map(|(&mapping_start, &mapping)| {
                (mapping_start < end_page && mapping.end_page > start_page)
                    .then_some((mapping_start, mapping))
            })
            .collect()
    }

    fn coalesce_mappings(&mut self) {
        let mut canonical = Vec::<(u32, Mapping)>::with_capacity(self.mappings.len());
        for (&start, &mapping) in &self.mappings {
            if let Some((_, previous)) = canonical.last_mut()
                && previous.end_page == start
                && previous.permissions == mapping.permissions
            {
                previous.end_page = mapping.end_page;
                continue;
            }
            canonical.push((start, mapping));
        }
        self.mappings.clear();
        self.mappings.extend(canonical);
    }

    fn page_address(page: u32) -> GuestAddress {
        GuestAddress::new(
            page.checked_mul(PAGE_BYTES_U32)
                .expect("a mapped guest page starts within the u32 address space"),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    struct MaliciousGuestValue;

    impl GuestValue for MaliciousGuestValue {
        const SIZE: usize = usize::MAX;

        fn read_from(_: &[u8]) -> Result<Self, cex_types::GuestValueError> {
            panic!("oversized guest value codec must not be called")
        }

        fn write_to(self, _: &mut [u8]) -> Result<(), cex_types::GuestValueError> {
            panic!("oversized guest value codec must not be called")
        }
    }

    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    struct OversizedGuestValue;

    impl GuestValue for OversizedGuestValue {
        const SIZE: usize = MAX_GUEST_VALUE_SIZE + 1;

        fn read_from(_: &[u8]) -> Result<Self, cex_types::GuestValueError> {
            panic!("oversized guest value codec must not be called")
        }

        fn write_to(self, _: &mut [u8]) -> Result<(), cex_types::GuestValueError> {
            panic!("oversized guest value codec must not be called")
        }
    }

    fn address(value: u32) -> GuestAddress {
        GuestAddress::new(value)
    }

    #[test]
    fn exact_top_page_is_valid_but_wrapping_range_is_not() {
        let mut memory = GuestMemory::new();
        let top_page = address(u32::MAX - PAGE_BYTES_U32 + 1);
        memory
            .map(top_page, PAGE_SIZE, Permissions::READ | Permissions::WRITE)
            .unwrap();
        memory
            .write_u32(address(u32::MAX - 3), 0x1234_5678)
            .unwrap();
        assert_eq!(memory.read_u32(address(u32::MAX - 3)).unwrap(), 0x1234_5678);

        assert_eq!(
            memory.read_u32(address(u32::MAX - 2)),
            Err(MemoryFault::AddressOverflow {
                start: address(u32::MAX - 2),
                len: 4,
            })
        );
    }

    #[test]
    fn mapping_requires_non_empty_page_aligned_ranges() {
        let mut memory = GuestMemory::new();
        assert_eq!(
            memory.map(address(1), PAGE_SIZE, Permissions::READ),
            Err(MemoryFault::UnalignedAddress {
                address: address(1),
                alignment: PAGE_SIZE,
            })
        );
        assert_eq!(
            memory.map(address(0), 1, Permissions::READ),
            Err(MemoryFault::UnalignedLength {
                length: 1,
                alignment: PAGE_SIZE,
            })
        );
        assert_eq!(
            memory.map(address(0), 0, Permissions::READ),
            Err(MemoryFault::ZeroLength)
        );
    }

    #[test]
    fn map_rejects_overlap_but_accepts_adjacency() {
        let mut memory = GuestMemory::new();
        memory
            .map(address(0x2000), PAGE_SIZE * 2, Permissions::READ)
            .unwrap();
        assert_eq!(
            memory.map(address(0x1000), PAGE_SIZE * 2, Permissions::WRITE),
            Err(MemoryFault::Overlap {
                address: address(0x2000),
            })
        );
        memory
            .map(address(0x4000), PAGE_SIZE, Permissions::READ)
            .unwrap();
        assert_eq!(memory.mappings().len(), 1);
    }

    #[test]
    fn reads_are_zero_filled_and_backing_is_sparse() {
        let mut memory = GuestMemory::new();
        memory
            .map(
                address(0),
                GUEST_ADDRESS_SPACE_SIZE,
                Permissions::READ | Permissions::WRITE,
            )
            .unwrap();
        assert_eq!(
            memory.mapped_page_count(),
            GUEST_ADDRESS_SPACE_SIZE / PAGE_SIZE
        );
        assert_eq!(memory.resident_page_count(), 0);
        assert_eq!(memory.read_u64(address(0x8000_0000)).unwrap(), 0);
        memory.write_u8(address(0x8000_0000), 1).unwrap();
        assert_eq!(memory.resident_page_count(), 1);
        memory.write_u8(address(0x8000_0000), 0).unwrap();
        assert_eq!(memory.resident_page_count(), 0);
    }

    #[test]
    fn cross_page_write_is_transactional_on_permission_failure() {
        let mut memory = GuestMemory::new();
        memory
            .map(
                address(0),
                PAGE_SIZE,
                Permissions::READ | Permissions::WRITE,
            )
            .unwrap();
        memory
            .map(address(PAGE_BYTES_U32), PAGE_SIZE, Permissions::READ)
            .unwrap();

        let start = address(PAGE_BYTES_U32 - 2);
        assert_eq!(
            memory.write(start, &[1, 2, 3, 4]),
            Err(MemoryFault::PermissionDenied {
                address: address(PAGE_BYTES_U32),
                access: AccessType::Write,
                permissions: Permissions::READ,
            })
        );
        assert_eq!(memory.read(start, &mut [0; 2]), Ok(()));
        assert_eq!(memory.read_vec(start, 2).unwrap(), [0, 0]);
    }

    #[test]
    fn execute_only_memory_can_be_fetched_but_not_data_read() {
        let mut memory = GuestMemory::new();
        memory
            .map(
                address(0x1000),
                PAGE_SIZE,
                Permissions::WRITE | Permissions::EXECUTE,
            )
            .unwrap();
        memory.write_u32(address(0x1000), 0x6000_0000).unwrap();
        memory
            .protect(address(0x1000), PAGE_SIZE, Permissions::EXECUTE)
            .unwrap();

        assert_eq!(memory.fetch_u32(address(0x1000)).unwrap(), 0x6000_0000);
        assert_eq!(
            memory.read_u32(address(0x1000)),
            Err(MemoryFault::PermissionDenied {
                address: address(0x1000),
                access: AccessType::Read,
                permissions: Permissions::EXECUTE,
            })
        );
    }

    #[test]
    fn big_endian_typed_access_crosses_a_page_boundary() {
        let mut memory = GuestMemory::new();
        memory
            .map(
                address(0),
                PAGE_SIZE * 2,
                Permissions::READ | Permissions::WRITE,
            )
            .unwrap();
        let start = address(PAGE_BYTES_U32 - 2);
        memory.write_u32(start, 0x0102_0304).unwrap();
        assert_eq!(memory.read_vec(start, 4).unwrap(), [1, 2, 3, 4]);
        assert_eq!(memory.read_u32(start).unwrap(), 0x0102_0304);
    }

    #[test]
    fn partial_unmap_splits_a_mapping_and_discards_contents() {
        let mut memory = GuestMemory::new();
        memory
            .map(
                address(0),
                PAGE_SIZE * 3,
                Permissions::READ | Permissions::WRITE,
            )
            .unwrap();
        memory.write_u8(address(PAGE_BYTES_U32), 7).unwrap();
        memory.unmap(address(PAGE_BYTES_U32), PAGE_SIZE).unwrap();

        assert_eq!(memory.mappings().len(), 2);
        assert_eq!(memory.resident_page_count(), 0);
        assert_eq!(
            memory.read_u8(address(PAGE_BYTES_U32)),
            Err(MemoryFault::Unmapped {
                address: address(PAGE_BYTES_U32),
                access: AccessType::Read,
            })
        );
    }

    #[test]
    fn failed_unmap_and_protect_are_transactional() {
        let mut memory = GuestMemory::new();
        memory
            .map(address(0), PAGE_SIZE, Permissions::READ)
            .unwrap();
        memory
            .map(address(PAGE_BYTES_U32 * 2), PAGE_SIZE, Permissions::READ)
            .unwrap();
        let before = memory.deterministic_hash();

        assert!(matches!(
            memory.unmap(address(0), PAGE_SIZE * 3),
            Err(MemoryFault::Unmapped { .. })
        ));
        assert_eq!(memory.deterministic_hash(), before);
        assert!(matches!(
            memory.protect(address(0), PAGE_SIZE * 3, Permissions::WRITE),
            Err(MemoryFault::Unmapped { .. })
        ));
        assert_eq!(memory.deterministic_hash(), before);
    }

    #[test]
    fn deterministic_hash_ignores_mapping_and_zero_write_history() {
        let mut one_mapping = GuestMemory::new();
        one_mapping
            .map(
                address(0),
                PAGE_SIZE * 2,
                Permissions::READ | Permissions::WRITE,
            )
            .unwrap();

        let mut adjacent_mappings = GuestMemory::new();
        adjacent_mappings
            .map(
                address(0),
                PAGE_SIZE,
                Permissions::READ | Permissions::WRITE,
            )
            .unwrap();
        adjacent_mappings
            .map(
                address(PAGE_BYTES_U32),
                PAGE_SIZE,
                Permissions::READ | Permissions::WRITE,
            )
            .unwrap();
        adjacent_mappings.write_u32(address(0x100), 0).unwrap();

        assert_eq!(
            one_mapping.deterministic_hash(),
            adjacent_mappings.deterministic_hash()
        );
        adjacent_mappings.write_u32(address(0x100), 1).unwrap();
        assert_ne!(
            one_mapping.deterministic_hash(),
            adjacent_mappings.deterministic_hash()
        );
    }

    #[test]
    fn owned_read_validates_overflow_before_allocating() {
        let memory = GuestMemory::new();
        let reported_len = u64::try_from(usize::MAX).unwrap_or(u64::MAX);
        assert_eq!(
            memory.read_vec(GuestAddress::MAX, usize::MAX),
            Err(MemoryFault::AddressOverflow {
                start: GuestAddress::MAX,
                len: reported_len,
            })
        );
    }

    #[test]
    fn malicious_guest_value_read_is_rejected_before_range_or_allocation_work() {
        let memory = GuestMemory::new();
        let before = memory.deterministic_hash();
        let expected = Err(MemoryFault::GuestValueTooLarge {
            requested: usize::MAX,
            maximum: MAX_GUEST_VALUE_SIZE,
        });

        assert_eq!(
            memory.read_value::<MaliciousGuestValue>(GuestAddress::MAX),
            expected
        );
        assert_eq!(
            memory.read_value::<MaliciousGuestValue>(GuestAddress::MAX),
            expected
        );
        assert_eq!(memory.deterministic_hash(), before);
    }

    #[test]
    fn oversized_guest_value_write_is_rejected_without_mutating_memory() {
        let mut memory = GuestMemory::new();
        memory
            .map(
                address(0),
                PAGE_SIZE,
                Permissions::READ | Permissions::WRITE,
            )
            .unwrap();
        memory.write_u32(address(0), 0x1234_5678).unwrap();
        let before = memory.deterministic_hash();
        let expected = Err(MemoryFault::GuestValueTooLarge {
            requested: MAX_GUEST_VALUE_SIZE + 1,
            maximum: MAX_GUEST_VALUE_SIZE,
        });

        assert_eq!(
            memory.write_value(GuestAddress::MAX, OversizedGuestValue),
            expected
        );
        assert_eq!(
            memory.write_value(GuestAddress::MAX, OversizedGuestValue),
            expected
        );
        assert_eq!(memory.deterministic_hash(), before);
        assert_eq!(memory.read_u32(address(0)).unwrap(), 0x1234_5678);
    }
}
