//! Deterministic headless system services for the Rust rewrite.

mod headless;
mod loader;
mod rpx;
mod rpx_fixture;
mod scheduler;
mod vfs;

pub use headless::{HeadlessError, HeadlessRun, HeadlessSystem};
pub use loader::{
    BUILTIN_FIXTURE_NAME, MAX_SYNTHETIC_CODE_SIZE, MAX_SYNTHETIC_IMAGE_SIZE, ProgramDecodeError,
    SyntheticProgram, builtin_fixture,
};
pub use rpx::{
    MAX_RPX_IMAGE_SIZE, ParsedRpx, RpxError, RpxFileInfo, RpxFileInfoField, RpxHeaderField,
    RpxMappingRegion, RpxSection, RpxUnsupportedFeature, parse_rpx,
};
pub use rpx_fixture::builtin_rpx_fixture;
pub use scheduler::{DeterministicScheduler, ScheduleError, TaskControl, TaskId};
pub use vfs::{
    InMemoryVfs, MAX_GUEST_PATH_BYTES, VfsError, VirtualFileSystem, normalize_guest_path,
};

/// Stable command-line selector for the bundled deterministic RPX fixture.
pub const BUILTIN_RPX_FIXTURE_NAME: &str = "synthetic-rpx-boot";
