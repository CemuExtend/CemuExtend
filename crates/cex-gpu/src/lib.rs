//! GPU-facing architecture for the CemuExtend Rust rewrite.
//!
//! This crate defines the boundary between emulation and a future `wgpu`
//! renderer. It intentionally contains no renderer or shader translator yet.

use core::fmt;

use cex_types::GuestCycle;

/// A renderer feature tracked by the compatibility plan.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(usize)]
pub enum GpuCapability {
    /// Latte command processor execution.
    CommandProcessor,
    /// Guest shader translation through the backend-independent shader IR.
    ShaderTranslation,
    /// Guest texture decoding, aliasing, and caching.
    TextureCache,
    /// Render-target tracking and render-to-texture behavior.
    RenderTargets,
    /// Guest GPU query support.
    Queries,
    /// Guest stream-out support.
    StreamOut,
    /// TV display output.
    TvOutput,
    /// GamePad display output.
    GamePadOutput,
    /// Composition of the CEF off-screen overlay.
    CefOverlay,
}

const CAPABILITY_COUNT: usize = 9;

/// The implementation status of a GPU capability.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CapabilityState {
    /// The Rust rewrite has not implemented the capability.
    #[default]
    NotImplemented,
    /// An implementation exists, but is unavailable in the current runtime.
    Unavailable,
    /// The current renderer can provide the capability.
    Available,
}

/// An immutable snapshot of renderer capabilities.
///
/// Constructors and builder methods return a new snapshot; callers should keep
/// the returned value instead of discarding it.
#[must_use = "renderer capability snapshots must be retained to observe their state"]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct GpuCapabilities {
    states: [CapabilityState; CAPABILITY_COUNT],
}

impl GpuCapabilities {
    /// Returns a report in which every GPU capability is unimplemented.
    pub const fn none() -> Self {
        Self {
            states: [CapabilityState::NotImplemented; CAPABILITY_COUNT],
        }
    }

    /// Returns a copy of this report with one capability state changed.
    pub const fn with_state(mut self, capability: GpuCapability, state: CapabilityState) -> Self {
        self.states[capability as usize] = state;
        self
    }

    /// Returns the state of one capability.
    pub const fn state(self, capability: GpuCapability) -> CapabilityState {
        self.states[capability as usize]
    }
}

impl Default for GpuCapabilities {
    fn default() -> Self {
        Self::none()
    }
}

/// The display surface targeted by a guest presentation command.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DisplayTarget {
    /// The primary television output.
    Tv,
    /// The Wii U GamePad output.
    GamePad,
}

/// A batch of raw Latte command words with its deterministic guest timestamp.
#[derive(Clone, Copy)]
pub struct GpuSubmission<'a> {
    /// Guest cycle at which the batch was submitted.
    pub guest_cycle: GuestCycle,
    /// Decoded guest command words, before backend-specific translation.
    pub command_words: &'a [u32],
}

impl fmt::Debug for GpuSubmission<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("GpuSubmission")
            .field("guest_cycle", &self.guest_cycle)
            .field("command_word_count", &self.command_words.len())
            .finish()
    }
}

/// Boundary implemented by a future `wgpu`-based Latte renderer.
pub trait Renderer: Send {
    /// Renderer-specific failure returned at the emulation boundary.
    type Error;

    /// Reports features available from this renderer and host backend.
    fn capabilities(&self) -> GpuCapabilities;

    /// Queues one ordered guest command batch.
    fn submit(&mut self, submission: GpuSubmission<'_>) -> Result<(), Self::Error>;

    /// Presents the most recently completed image for a guest display.
    fn present(&mut self, target: DisplayTarget) -> Result<(), Self::Error>;
}

#[cfg(test)]
mod tests {
    use super::{CapabilityState, GpuCapabilities, GpuCapability, GpuSubmission};
    use cex_types::GuestCycle;

    #[test]
    fn default_report_does_not_claim_gpu_support() {
        let report = GpuCapabilities::default();

        assert_eq!(
            report.state(GpuCapability::CommandProcessor),
            CapabilityState::NotImplemented
        );
        assert_eq!(
            report.state(GpuCapability::CefOverlay),
            CapabilityState::NotImplemented
        );
    }

    #[test]
    fn gpu_submission_debug_redacts_raw_command_words() {
        let submission = GpuSubmission {
            guest_cycle: GuestCycle::new(42),
            command_words: &[0xDEAD_BEEF, 0xCAFE_BABE, 0x0BAD_F00D],
        };

        let debug = format!("{submission:?}");

        assert!(debug.contains("GpuSubmission {"));
        assert!(debug.contains("guest_cycle: GuestCycle(42)"));
        assert!(debug.contains("command_word_count: 3"));
        assert!(!debug.contains("3735928559"));
        assert!(!debug.contains("3405691582"));
        assert!(!debug.contains("195948557"));
    }
}
