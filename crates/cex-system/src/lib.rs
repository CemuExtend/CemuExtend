//! Deterministic headless system services for the Rust rewrite.

mod headless;
mod loader;
mod rpl_call_fixture;
mod rpl_link;
mod rpl_link_fixture;
mod rpx;
mod rpx_fixture;
mod scheduler;
mod vfs;

pub use headless::{HeadlessError, HeadlessRun, HeadlessSystem};
pub use loader::{
    BUILTIN_FIXTURE_NAME, MAX_SYNTHETIC_CODE_SIZE, MAX_SYNTHETIC_IMAGE_SIZE, ProgramDecodeError,
    SyntheticProgram, builtin_fixture,
};
pub use rpl_link::{
    RplModuleName, RplModuleNameError, RpxRplCommitError, RpxRplLinkModule, RpxRplLinkPhase,
    RpxRplLinkPlan, RpxRplLinkProof, RpxRplLinkRecord, RpxRplLinkRegion, RpxRplPlanError,
    RpxRplRelocationProof, commit_rpx_rpl_link, plan_rpx_rpl_link,
};
pub use rpx::{
    CafeExport, CafeImport, CafeModuleKind, CafeRelocation, CafeRelocationKind, CafeSymbol,
    CafeSymbolKind, MAX_RPX_IMAGE_SIZE, ParsedRpl, ParsedRpx, RplRecordError, RpxError,
    RpxFileInfo, RpxFileInfoField, RpxHeaderField, RpxMappingRegion, RpxSection,
    RpxUnsupportedFeature, parse_rpl, parse_rpx,
};
pub use rpx_fixture::builtin_rpx_fixture;
pub use scheduler::{DeterministicScheduler, ScheduleError, TaskControl, TaskId};
pub use vfs::{
    InMemoryVfs, MAX_GUEST_PATH_BYTES, VfsError, VirtualFileSystem, normalize_guest_path,
};

/// Stable command-line selector for the bundled deterministic RPX fixture.
pub const BUILTIN_RPX_FIXTURE_NAME: &str = "synthetic-rpx-boot";

/// Return a source-generated, deterministic RPX fixture for RPL-link API tests.
///
/// This synthetic input is solely a bounded interoperability fixture; it is not
/// a retail title or a general-purpose RPX builder.  Every call returns fresh
/// owned bytes and consults no mutable global state.
#[doc(hidden)]
pub fn synthetic_rpx_rpl_link_main_fixture() -> Result<Vec<u8>, RpxError> {
    rpl_link_fixture::main_rpx_link_fixture().map_err(|error| match error {
        rpl_link_fixture::RplLinkFixtureError::AllocationFailed { requested } => {
            RpxError::AllocationFailed { requested }
        }
    })
}

/// Return a source-generated, deterministic RPL provider fixture for link API tests.
///
/// This synthetic input contains the one supported import/export/`ADDR32`
/// shape.  Every call returns fresh owned bytes and consults no mutable global
/// state.
#[doc(hidden)]
pub fn synthetic_rpx_rpl_link_provider_fixture() -> Result<Vec<u8>, RpxError> {
    rpl_link_fixture::provider_rpl_link_fixture().map_err(|error| match error {
        rpl_link_fixture::RplLinkFixtureError::AllocationFailed { requested } => {
            RpxError::AllocationFailed { requested }
        }
    })
}

/// Return a source-generated main RPX whose entry performs one imported call.
///
/// The call-site fixture is kept separate from the original `ADDR32` link
/// fixture so both compatibility contracts remain independently reproducible.
#[doc(hidden)]
pub fn synthetic_rpx_rpl_call_main_fixture() -> Result<Vec<u8>, RpxError> {
    rpl_call_fixture::main_rpx_call_fixture().map_err(|error| match error {
        rpl_call_fixture::RplCallFixtureError::AllocationFailed { requested } => {
            RpxError::AllocationFailed { requested }
        }
    })
}

/// Return the deterministic provider RPL for the imported-call fixture.
#[doc(hidden)]
pub fn synthetic_rpx_rpl_call_provider_fixture() -> Result<Vec<u8>, RpxError> {
    rpl_call_fixture::provider_rpl_call_fixture().map_err(|error| match error {
        rpl_call_fixture::RplCallFixtureError::AllocationFailed { requested } => {
            RpxError::AllocationFailed { requested }
        }
    })
}
