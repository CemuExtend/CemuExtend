//! Minimal read-only virtual filesystem for synthetic fixtures.

use std::collections::BTreeMap;
use thiserror::Error;

/// Maximum guest path length in UTF-8 bytes accepted by the VFS.
///
/// Cafe/IOSU reserves [`FSA_PATH_SIZE_MAX`](../../src/Cafe/IOSU/fsa/fsa_types.h)
/// bytes (640, including the terminating nul), so a guest path can contain at
/// most 639 UTF-8 bytes. Keeping this limit here preserves that ABI boundary
/// while allowing [`VfsError::InvalidPath`] to retain the original input for
/// ordinary validation failures.
pub const MAX_GUEST_PATH_BYTES: usize = 639;

/// Filesystem surface used by early Cafe/IOSU services.
pub trait VirtualFileSystem {
    /// Read a guest-visible file completely.
    fn read(&self, guest_path: &str) -> Result<Vec<u8>, VfsError>;
}

/// Deterministic read-only VFS with no access to host paths.
#[derive(Clone, Debug, Default)]
pub struct InMemoryVfs {
    files: BTreeMap<String, Vec<u8>>,
}

impl InMemoryVfs {
    /// Construct an empty filesystem.
    pub const fn new() -> Self {
        Self {
            files: BTreeMap::new(),
        }
    }

    /// Install a synthetic file before the guest starts.
    pub fn insert(
        &mut self,
        guest_path: &str,
        contents: impl Into<Vec<u8>>,
    ) -> Result<(), VfsError> {
        let path = normalize_guest_path(guest_path)?;
        self.files.insert(path, contents.into());
        Ok(())
    }
}

impl VirtualFileSystem for InMemoryVfs {
    fn read(&self, guest_path: &str) -> Result<Vec<u8>, VfsError> {
        let path = normalize_guest_path(guest_path)?;
        self.files
            .get(&path)
            .cloned()
            .ok_or(VfsError::NotFound(path))
    }
}

/// Normalize `/`-separated guest paths identically on every host platform.
pub fn normalize_guest_path(guest_path: &str) -> Result<String, VfsError> {
    if guest_path.len() > MAX_GUEST_PATH_BYTES {
        return Err(VfsError::PathTooLong {
            bytes: guest_path.len(),
            maximum: MAX_GUEST_PATH_BYTES,
        });
    }
    if guest_path.is_empty()
        || guest_path.starts_with('/')
        || guest_path.ends_with('/')
        || guest_path.contains('\\')
        || guest_path.chars().any(char::is_control)
    {
        return Err(VfsError::InvalidPath(guest_path.to_owned()));
    }

    let mut normalized = Vec::new();
    for segment in guest_path.split('/') {
        match segment {
            "" | ".." => return Err(VfsError::InvalidPath(guest_path.to_owned())),
            "." => {}
            value => normalized.push(value),
        }
    }
    if normalized.is_empty() {
        return Err(VfsError::InvalidPath(guest_path.to_owned()));
    }
    Ok(normalized.join("/"))
}

/// VFS path validation or lookup failure.
#[derive(Clone, Debug, Error, Eq, PartialEq)]
pub enum VfsError {
    /// A guest supplied an absolute, empty, or traversing path.
    ///
    /// For compatibility this retains the raw input, which is bounded to
    /// [`MAX_GUEST_PATH_BYTES`] by [`normalize_guest_path`] before cloning.
    #[error("invalid guest path {0:?}")]
    InvalidPath(String),
    /// A guest path exceeded the ABI-compatible byte limit.
    #[error("guest path is {bytes} bytes (maximum {maximum})")]
    PathTooLong {
        /// Actual UTF-8 byte length of the supplied guest path before validation.
        bytes: usize,
        /// Maximum guest path length allowed by the ABI-compatible limit.
        maximum: usize,
    },
    /// The path is absent from the synthetic filesystem.
    #[error("guest file {0:?} was not found")]
    NotFound(String),
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_mount_escape_and_ambiguous_separators() {
        for path in [
            "../secret",
            "/etc/passwd",
            "a/../../secret",
            "a\\..\\secret",
            "a//b",
            "a/",
            "",
        ] {
            assert!(normalize_guest_path(path).is_err(), "accepted {path:?}");
        }
    }

    #[test]
    fn reads_only_preinstalled_files() {
        let mut vfs = InMemoryVfs::new();
        vfs.insert("vol/content/main.cexh", b"fixture".to_vec())
            .expect("safe path should be accepted");

        assert_eq!(vfs.read("vol/./content/main.cexh"), Ok(b"fixture".to_vec()));
        assert_eq!(
            vfs.read("vol/content/missing.cexh"),
            Err(VfsError::NotFound("vol/content/missing.cexh".to_owned()))
        );
    }

    #[test]
    fn enforces_guest_path_byte_limit_before_validation() {
        let exact = "a".repeat(MAX_GUEST_PATH_BYTES);
        assert_eq!(
            normalize_guest_path(&exact),
            Ok(exact.clone()),
            "the exact ABI payload limit should remain valid"
        );

        let too_long = "a".repeat(MAX_GUEST_PATH_BYTES + 1);
        assert_eq!(
            normalize_guest_path(&too_long),
            Err(VfsError::PathTooLong {
                bytes: MAX_GUEST_PATH_BYTES + 1,
                maximum: MAX_GUEST_PATH_BYTES,
            })
        );

        let invalid_and_too_long = format!("{exact}/..");
        assert_eq!(
            normalize_guest_path(&invalid_and_too_long),
            Err(VfsError::PathTooLong {
                bytes: invalid_and_too_long.len(),
                maximum: MAX_GUEST_PATH_BYTES,
            }),
            "length rejection must precede invalid-path cloning"
        );
    }
}
