//! Compatibility boundary for CEMOD packages and WUPS modules.
//!
//! The baseline accepts CEMOD package formats v1 through v4. Version 4 adds
//! Web UI assets and their network policy. This crate records those contracts
//! without implementing parsing, signature validation, module loading, or
//! lifecycle execution.

use core::fmt;
use std::num::NonZeroU64;
use std::path::PathBuf;

use cex_types::{GuestAddress, GuestRange};

/// A CEMOD/WUPS feature tracked by the compatibility plan.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(usize)]
pub enum CemodCapability {
    /// CEMOD package format version 1.
    PackageV1,
    /// CEMOD package format version 2.
    PackageV2,
    /// CEMOD package format version 3.
    PackageV3,
    /// CEMOD package format version 4, including Web UI metadata.
    PackageV4,
    /// CEX2 executable format.
    Cex2,
    /// CMB1 metadata format.
    Cmb1,
    /// WUPS ABI 0.7.1.
    WupsAbi071,
    /// WUPS ABI 0.8.1.
    WupsAbi081,
    /// WUPS ABI 0.8.2.
    WupsAbi082,
    /// WUPS ABI 0.9.0.
    WupsAbi090,
    /// WUPS ABI 0.9.1.
    WupsAbi091,
    /// Package signature digest validation.
    SignatureValidation,
    /// Package permission enforcement.
    PermissionPolicy,
    /// Module lifecycle dispatch.
    Lifecycle,
    /// CEMOD Web UI asset and overlay hosting.
    WebUi,
    /// CEMOD Web UI network policy enforcement.
    WebUiNetworkPolicy,
    /// TCPGecko compatibility.
    TcpGecko,
    /// Generation-scoped WUPS guest-resource ownership and revocation.
    WupsResourceOwnership,
}

const CAPABILITY_COUNT: usize = 18;

/// The implementation status of a CEMOD/WUPS capability.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CapabilityState {
    /// The Rust rewrite has not implemented the capability.
    #[default]
    NotImplemented,
    /// An implementation exists, but is unavailable in the current runtime.
    Unavailable,
    /// The current runtime can provide the capability.
    Available,
}

/// An immutable snapshot of CEMOD/WUPS capabilities.
///
/// Keep the returned report when calling `none()` or `with_state()` because
/// each builder produces a new capability snapshot.
#[must_use = "CemodCapabilities is an immutable snapshot; keep the returned report."]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CemodCapabilities {
    states: [CapabilityState; CAPABILITY_COUNT],
}

impl CemodCapabilities {
    /// Returns a report in which every CEMOD/WUPS feature is unimplemented.
    pub const fn none() -> Self {
        Self {
            states: [CapabilityState::NotImplemented; CAPABILITY_COUNT],
        }
    }

    /// Returns a copy of this report with one capability state changed.
    pub const fn with_state(mut self, capability: CemodCapability, state: CapabilityState) -> Self {
        self.states[capability as usize] = state;
        self
    }

    /// Returns the state of one capability.
    pub const fn state(self, capability: CemodCapability) -> CapabilityState {
        self.states[capability as usize]
    }
}

impl Default for CemodCapabilities {
    fn default() -> Self {
        Self::none()
    }
}

/// CEMOD package format declared by a package manifest.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PackageVersion {
    /// Package format version 1.
    V1,
    /// Package format version 2.
    V2,
    /// Package format version 3.
    V3,
    /// Package format version 4 with Web UI asset declarations.
    V4,
}

/// Supported WUPS ABI families in the compatibility baseline.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WupsAbiVersion {
    /// WUPS ABI version 0.7.1.
    V071,
    /// WUPS ABI version 0.8.1.
    V081,
    /// WUPS ABI version 0.8.2.
    V082,
    /// WUPS ABI version 0.9.0.
    V090,
    /// WUPS ABI version 0.9.1.
    V091,
}

/// A package selected for validation or activation.
#[derive(Clone, Eq, PartialEq)]
pub struct PackageCandidate {
    /// Local package path supplied by the trusted host filesystem layer.
    pub path: PathBuf,
}

impl fmt::Debug for PackageCandidate {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("PackageCandidate")
            .field("path_component_count", &self.path.components().count())
            .field("path_is_absolute", &self.path.is_absolute())
            .finish()
    }
}

/// Package identifier parsed from a manifest.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct PackageId(String);

impl PackageId {
    /// Validates the manifest-compatible package identifier grammar.
    pub fn new(value: String) -> Result<Self, InvalidPackageId> {
        if value.is_empty() {
            return Err(InvalidPackageId::Empty);
        }
        if value.len() > 128 {
            return Err(InvalidPackageId::TooLong { bytes: value.len() });
        }
        if !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
        {
            return Err(InvalidPackageId::InvalidCharacter);
        }
        Ok(Self(value))
    }

    /// Returns the validated identifier.
    #[must_use]
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

/// Error returned for an invalid manifest package identifier.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum InvalidPackageId {
    /// Package identifiers must contain at least one byte.
    Empty,
    /// Package identifier exceeded the baseline 128-byte limit.
    TooLong {
        /// Actual UTF-8 byte length.
        bytes: usize,
    },
    /// Package identifier contained a character outside ASCII alphanumeric, `.`, `_`, and `-`.
    InvalidCharacter,
}

/// Non-authorizing metadata describing a verified package artifact.
///
/// This descriptor is safe to display but is not accepted as proof that a
/// package was validated.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VerifiedPackageDescriptor {
    /// Stable package identifier read from the verified manifest.
    pub id: PackageId,
    /// Verified package format.
    pub version: PackageVersion,
    /// SHA-256 digest of the exact immutable package artifact.
    pub sha256: [u8; 32],
}

/// Opaque verified artifact owned by a concrete package runtime.
///
/// Implementations must retain the exact immutable bytes used during
/// validation, or pin an equivalent immutable artifact addressed by the
/// descriptor digest. Activation must never re-open the candidate path.
pub trait VerifiedPackage: Send {
    /// Returns non-authorizing metadata for diagnostics and UI display.
    fn descriptor(&self) -> &VerifiedPackageDescriptor;
}

/// Identity and generation of the module that owns a guest resource.
///
/// A new generation must be allocated whenever the package is reloaded so a
/// stale callback or handle cannot operate on resources from the new instance.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct ResourceOwner {
    /// Package owning the resource.
    package: PackageId,
    /// Monotonically changing module-load generation.
    generation: NonZeroU64,
}

impl ResourceOwner {
    /// Constructs an owner whose generation is statically non-zero.
    #[must_use]
    pub const fn new(package: PackageId, generation: NonZeroU64) -> Self {
        Self {
            package,
            generation,
        }
    }

    /// Returns the owning package.
    #[must_use]
    pub const fn package(&self) -> &PackageId {
        &self.package
    }

    /// Returns the non-zero module-load generation.
    #[must_use]
    pub const fn generation(&self) -> NonZeroU64 {
        self.generation
    }
}

/// Lifecycle state assigned to a WUPS-owned guest resource.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ResourceState {
    /// Registered during setup but not visible to normal API consumers.
    Provisional,
    /// Successfully published and available to its owner.
    Live,
    /// New leases and guest access have been rejected.
    Revoked,
    /// Existing host-side leases are being drained before memory is cleared.
    Draining,
}

/// Policy controlling which guest pointers an API entry may accept.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ApiPointerPolicy {
    /// Pointer must resolve to an allocation owned by the calling generation.
    Heap,
    /// Pointer must resolve to an explicitly mapped guest-memory resource.
    Mapped,
    /// Pointer is a validated guest callback entry point.
    Callback,
}

/// Operation requested through a WUPS API entry point.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ResourceAccess {
    /// Read guest-owned bytes.
    Read,
    /// Modify guest-owned bytes.
    Write,
    /// Invoke a guest callback entry point.
    Execute,
}

/// R/W/X permissions recorded when a guest resource is registered.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ResourcePermissions(u8);

impl ResourcePermissions {
    /// No permitted operation.
    pub const NONE: Self = Self(0);
    /// Read-only access.
    pub const READ: Self = Self(1);
    /// Write-only access.
    pub const WRITE: Self = Self(2);
    /// Execute-only callback access.
    pub const EXECUTE: Self = Self(4);
    /// Read and write access.
    pub const READ_WRITE: Self = Self(Self::READ.0 | Self::WRITE.0);
    /// Read and execute access, used by immutable executable sections.
    pub const READ_EXECUTE: Self = Self(Self::READ.0 | Self::EXECUTE.0);
    /// Returns whether this mask permits the requested operation.
    #[must_use]
    pub const fn allows(self, access: ResourceAccess) -> bool {
        let required = match access {
            ResourceAccess::Read => Self::READ.0,
            ResourceAccess::Write => Self::WRITE.0,
            ResourceAccess::Execute => Self::EXECUTE.0,
        };
        self.0 & required != 0
    }
}

/// Error returned when an empty guest range is used as a WUPS resource.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EmptyResourceRange;

/// A validated, non-empty range in the 4 GiB guest address space.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct GuestResourceRange(GuestRange);

impl GuestResourceRange {
    /// Rejects an empty range before it reaches the ownership registry.
    pub const fn new(range: GuestRange) -> Result<Self, EmptyResourceRange> {
        if range.is_empty() {
            Err(EmptyResourceRange)
        } else {
            Ok(Self(range))
        }
    }

    /// Returns the checked guest range.
    #[must_use]
    pub const fn get(self) -> GuestRange {
        self.0
    }
}

/// Description of a resource before it enters the provisional state.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResourceRegistration {
    /// Package generation that exclusively owns the range.
    pub owner: ResourceOwner,
    /// Exact guest range reserved for the resource.
    pub range: GuestResourceRange,
    /// Pointer validation policy used for API access.
    pub pointer_policy: ApiPointerPolicy,
    /// Guest read, write, and execute permissions.
    pub permissions: ResourcePermissions,
}

/// Fail-closed ownership boundary for guest resources created by WUPS modules.
///
/// Implementations must reject overflowed or empty ranges, cross-owner overlap,
/// owner/generation mismatches, and pointer-policy violations. Teardown order is
/// always revoke, drain all leases, then zero and free the exact-base resource.
/// None of these operations has a permissive default implementation.
pub trait WupsResourceRegistry {
    /// Registry validation or lifecycle failure.
    type Error;
    /// Opaque identity assigned to a successfully registered resource.
    type ResourceHandle: Copy;
    /// Opaque host-access pin that keeps its owner generation alive.
    type ResourceLease;

    /// Registers a non-overlapping range in [`ResourceState::Provisional`].
    fn register(
        &mut self,
        registration: ResourceRegistration,
    ) -> Result<Self::ResourceHandle, Self::Error>;

    /// Promotes an owner-matched resource from provisional to live.
    fn promote_live(
        &mut self,
        owner: &ResourceOwner,
        handle: Self::ResourceHandle,
    ) -> Result<(), Self::Error>;

    /// Pins a live resource for bounded host-side access.
    ///
    /// `operation` must be permitted by the registered R/W/X mask. Callback
    /// execution additionally requires [`ApiPointerPolicy::Callback`] and an
    /// access range beginning at the registered callback entry; interior
    /// callback pointers must be rejected.
    fn acquire_lease(
        &mut self,
        owner: &ResourceOwner,
        access: GuestResourceRange,
        policy: ApiPointerPolicy,
        operation: ResourceAccess,
    ) -> Result<Self::ResourceLease, Self::Error>;

    /// Releases a host-access pin without exposing its internal identity.
    fn release_lease(&mut self, lease: Self::ResourceLease) -> Result<(), Self::Error>;

    /// Revokes an owner generation before lease draining begins.
    fn revoke_owner(&mut self, owner: &ResourceOwner) -> Result<(), Self::Error>;

    /// Drains every outstanding lease after the owner was revoked.
    fn drain_owner(&mut self, owner: &ResourceOwner) -> Result<(), Self::Error>;

    /// Zeroes and frees an exact-base resource owned by the supplied generation.
    ///
    /// Interior pointers, foreign owners, live resources, and resources with an
    /// outstanding lease must be rejected.
    fn zero_and_free_exact(
        &mut self,
        owner: &ResourceOwner,
        base: GuestAddress,
    ) -> Result<(), Self::Error>;
}

/// Boundary implemented by the future package verifier and module runtime.
pub trait CemodRuntime: Send {
    /// Runtime-specific validation or lifecycle failure.
    type Error;
    /// Runtime-owned, unforgeable result of validating exact package bytes.
    type VerifiedPackage: VerifiedPackage;

    /// Reports package formats and runtime features available in this process.
    fn capabilities(&self) -> CemodCapabilities;

    /// Validates a package without executing its contents.
    fn validate(&self, candidate: &PackageCandidate) -> Result<Self::VerifiedPackage, Self::Error>;

    /// Activates and consumes the exact artifact returned by [`Self::validate`].
    ///
    /// Implementations must use the pinned bytes represented by `package` and
    /// must not re-open [`PackageCandidate::path`].
    fn activate(&mut self, package: Self::VerifiedPackage) -> Result<PackageId, Self::Error>;

    /// Deactivates an active package by identifier.
    fn deactivate(&mut self, package: &PackageId) -> Result<(), Self::Error>;
}

#[cfg(test)]
mod tests {
    use super::{CapabilityState, CemodCapabilities, CemodCapability, PackageCandidate};
    use std::path::PathBuf;

    #[test]
    fn default_report_includes_v4_without_claiming_support() {
        let report = CemodCapabilities::default();

        assert_eq!(
            report.state(CemodCapability::PackageV1),
            CapabilityState::NotImplemented
        );
        assert_eq!(
            report.state(CemodCapability::PackageV4),
            CapabilityState::NotImplemented
        );
        assert_eq!(
            report.state(CemodCapability::WebUiNetworkPolicy),
            CapabilityState::NotImplemented
        );
        assert_eq!(
            report.state(CemodCapability::WupsResourceOwnership),
            CapabilityState::NotImplemented
        );
    }

    #[test]
    fn package_candidate_debug_redacts_path_text() {
        let candidate = PackageCandidate {
            path: PathBuf::from("alpha/sensitive-package.bin"),
        };

        let debug = format!("{candidate:?}");

        assert!(debug.contains("PackageCandidate {"));
        assert!(debug.contains("path_component_count: 2"));
        assert!(debug.contains("path_is_absolute: false"));
        assert!(!debug.contains("sensitive-package.bin"));
        assert!(!debug.contains("alpha"));
    }
}
