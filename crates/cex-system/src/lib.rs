//! Deterministic headless system services for the Rust rewrite.

mod headless;
mod loader;
mod scheduler;
mod vfs;

pub use headless::{HeadlessError, HeadlessRun, HeadlessSystem};
pub use loader::{
    BUILTIN_FIXTURE_NAME, MAX_SYNTHETIC_CODE_SIZE, MAX_SYNTHETIC_IMAGE_SIZE, ProgramDecodeError,
    SyntheticProgram, builtin_fixture,
};
pub use scheduler::{DeterministicScheduler, ScheduleError, TaskControl, TaskId};
pub use vfs::{
    InMemoryVfs, MAX_GUEST_PATH_BYTES, VfsError, VirtualFileSystem, normalize_guest_path,
};
