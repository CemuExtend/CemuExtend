//! Host platform boundaries for windowing, audio, input, USB/HID, and files.
//!
//! The types in this crate do not select a concrete platform library and do
//! not claim that any host service is implemented.

use std::fmt;

/// Maximum UTF-8 byte length of a guest-visible logical host path.
///
/// This intentionally equals `cex-system::MAX_GUEST_PATH_BYTES`: both limits
/// come from the same 640-byte Cafe FSA path buffer (including its terminator).
/// It is repeated here to keep the host boundary independent of system policy.
pub const MAX_LOGICAL_PATH_BYTES: usize = 639;

/// A host integration feature tracked by the rewrite.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(usize)]
pub enum HostCapability {
    /// Native window creation and event delivery.
    Window,
    /// Host audio output.
    Audio,
    /// Controller and keyboard input.
    Input,
    /// USB and HID device access.
    UsbHid,
    /// Sandboxed host filesystem access.
    FileSystem,
}

const CAPABILITY_COUNT: usize = 5;

/// The implementation status of a host capability.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CapabilityState {
    /// The Rust rewrite has not implemented the capability.
    #[default]
    NotImplemented,
    /// An implementation exists, but is unavailable in the current runtime.
    Unavailable,
    /// The current host backend can provide the capability.
    Available,
}

/// An immutable snapshot of host capabilities.
///
/// Constructors and builder methods return a new snapshot; callers should keep
/// the returned value instead of discarding it.
#[must_use = "host capability snapshots must be retained to observe their state"]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HostCapabilities {
    states: [CapabilityState; CAPABILITY_COUNT],
}

impl HostCapabilities {
    /// Returns a report in which every host integration is unimplemented.
    pub const fn none() -> Self {
        Self {
            states: [CapabilityState::NotImplemented; CAPABILITY_COUNT],
        }
    }

    /// Returns a copy of this report with one capability state changed.
    pub const fn with_state(mut self, capability: HostCapability, state: CapabilityState) -> Self {
        self.states[capability as usize] = state;
        self
    }

    /// Returns the state of one capability.
    pub const fn state(self, capability: HostCapability) -> CapabilityState {
        self.states[capability as usize]
    }
}

impl Default for HostCapabilities {
    fn default() -> Self {
        Self::none()
    }
}

/// A host event delivered to the dedicated emulation thread.
#[derive(Clone, Debug, PartialEq)]
pub enum HostEvent {
    /// The user requested application shutdown.
    CloseRequested,
    /// A host window changed its drawable dimensions.
    WindowResized {
        /// New width in physical pixels.
        width: u32,
        /// New height in physical pixels.
        height: u32,
    },
    /// A digital input changed state.
    Button {
        /// Stable backend-defined device identifier.
        device_id: u64,
        /// Stable backend-defined control identifier.
        control: u32,
        /// Whether the control is pressed.
        pressed: bool,
    },
    /// A USB/HID device list changed.
    UsbDevicesChanged,
}

/// Interleaved host audio submitted by the emulation thread.
#[derive(Clone, Copy)]
pub struct AudioBuffer<'a> {
    /// Sample rate in hertz.
    pub sample_rate: u32,
    /// Number of interleaved channels.
    pub channels: u16,
    /// Normalized floating-point samples.
    pub samples: &'a [f32],
}

impl fmt::Debug for AudioBuffer<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("AudioBuffer")
            .field("sample_rate", &self.sample_rate)
            .field("channels", &self.channels)
            .field("sample_count", &self.samples.len())
            .finish()
    }
}

/// A canonical, relative guest-visible path for the host policy layer.
///
/// This type only validates logical path syntax. It neither resolves a host
/// filesystem location nor grants authority to access one. Logical paths use
/// `/` as their separator on every host platform.
#[derive(Clone, Eq, PartialEq)]
pub struct HostPath {
    logical: String,
}

/// Why a logical [`HostPath`] could not be constructed.
///
/// Variants deliberately retain no input path so errors are safe to log.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HostPathError {
    /// The logical path contained no components.
    Empty,
    /// The logical path exceeded the Cafe FSA path-buffer limit.
    PathTooLong {
        /// Supplied UTF-8 byte length.
        bytes: usize,
        /// Maximum accepted UTF-8 byte length.
        maximum: usize,
    },
    /// The path was rooted or used a platform-specific path prefix.
    NotRelative,
    /// The path contained `.` or `..`.
    NonCanonicalComponent,
    /// The path contained a control character.
    ControlCharacter,
    /// The path used an empty component or a non-portable separator.
    AmbiguousSeparator,
}

impl fmt::Display for HostPathError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let description = match self {
            Self::Empty => "logical host path is empty",
            Self::PathTooLong { .. } => "logical host path is too long",
            Self::NotRelative => "logical host path is not relative",
            Self::NonCanonicalComponent => "logical host path contains a non-canonical component",
            Self::ControlCharacter => "logical host path contains a control character",
            Self::AmbiguousSeparator => "logical host path contains an ambiguous separator",
        };
        formatter.write_str(description)
    }
}

impl std::error::Error for HostPathError {}

impl HostPath {
    /// Validates a canonical relative logical path.
    pub fn new(logical: impl AsRef<str>) -> Result<Self, HostPathError> {
        let logical = logical.as_ref();

        if logical.is_empty() {
            return Err(HostPathError::Empty);
        }
        if logical.len() > MAX_LOGICAL_PATH_BYTES {
            return Err(HostPathError::PathTooLong {
                bytes: logical.len(),
                maximum: MAX_LOGICAL_PATH_BYTES,
            });
        }
        if logical.starts_with('/') || has_windows_prefix(logical) {
            return Err(HostPathError::NotRelative);
        }
        if logical.contains('\\') {
            return Err(HostPathError::AmbiguousSeparator);
        }
        if logical.chars().any(char::is_control) {
            return Err(HostPathError::ControlCharacter);
        }

        for component in logical.split('/') {
            if component.is_empty() {
                return Err(HostPathError::AmbiguousSeparator);
            }
            if matches!(component, "." | "..") {
                return Err(HostPathError::NonCanonicalComponent);
            }
        }

        Ok(Self {
            logical: logical.to_owned(),
        })
    }

    /// Returns the validated guest-visible logical path.
    pub fn as_str(&self) -> &str {
        &self.logical
    }
}

fn has_windows_prefix(path: &str) -> bool {
    let bytes = path.as_bytes();
    bytes.len() >= 2 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':'
}

impl fmt::Debug for HostPath {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("HostPath")
            .field("byte_len", &self.logical.len())
            .field("segment_count", &self.logical.split('/').count())
            .finish()
    }
}

/// Interface owned by a concrete desktop or headless host backend.
pub trait HostPlatform: Send {
    /// Host-specific failure returned at the emulation boundary.
    type Error;

    /// Reports host services available in this process.
    fn capabilities(&self) -> HostCapabilities;

    /// Polls one event without blocking the emulation thread indefinitely.
    fn poll_event(&mut self) -> Result<Option<HostEvent>, Self::Error>;

    /// Queues audio for playback without transferring guest-owned memory.
    fn submit_audio(&mut self, buffer: AudioBuffer<'_>) -> Result<(), Self::Error>;
}

#[cfg(test)]
mod tests {
    use super::{
        AudioBuffer, CapabilityState, HostCapabilities, HostCapability, HostPath, HostPathError,
        MAX_LOGICAL_PATH_BYTES,
    };

    #[test]
    fn default_report_does_not_claim_host_support() {
        let report = HostCapabilities::default();

        assert_eq!(
            report.state(HostCapability::Window),
            CapabilityState::NotImplemented
        );
        assert_eq!(
            report.state(HostCapability::FileSystem),
            CapabilityState::NotImplemented
        );
    }

    #[test]
    fn host_path_accepts_safe_canonical_relative_paths() {
        let path = HostPath::new("title/content/001.bin").expect("safe path should be accepted");

        assert_eq!(path.as_str(), "title/content/001.bin");
        assert_eq!(
            HostPath::new("config.json").unwrap().as_str(),
            "config.json"
        );
    }

    #[test]
    fn host_path_enforces_cafe_fsa_byte_limit() {
        let maximum_length = "a".repeat(MAX_LOGICAL_PATH_BYTES);
        assert_eq!(
            HostPath::new(maximum_length).unwrap().as_str().len(),
            MAX_LOGICAL_PATH_BYTES
        );

        let over_limit = "a".repeat(MAX_LOGICAL_PATH_BYTES + 1);
        assert_eq!(
            HostPath::new(over_limit),
            Err(HostPathError::PathTooLong {
                bytes: 640,
                maximum: 639,
            })
        );
    }

    #[test]
    fn host_path_rejects_empty_and_absolute_paths() {
        assert_eq!(HostPath::new(""), Err(HostPathError::Empty));
        assert_eq!(
            HostPath::new("/host/secret"),
            Err(HostPathError::NotRelative)
        );
        assert_eq!(
            HostPath::new("C:/host/secret"),
            Err(HostPathError::NotRelative)
        );
    }

    #[test]
    fn host_path_rejects_traversal_and_dot_components() {
        for unsafe_path in ["..", "../secret", "safe/../secret", ".", "safe/./file"] {
            assert_eq!(
                HostPath::new(unsafe_path),
                Err(HostPathError::NonCanonicalComponent)
            );
        }

        let debug = format!(
            "{:?}",
            HostPath::new("../private-error-sentinel").unwrap_err()
        );
        assert!(!debug.contains("private-error-sentinel"));
    }

    #[test]
    fn host_path_rejects_ambiguous_separators() {
        for unsafe_path in [r"host\secret", "host//secret", "host/secret/"] {
            assert_eq!(
                HostPath::new(unsafe_path),
                Err(HostPathError::AmbiguousSeparator)
            );
        }
    }

    #[test]
    fn host_path_rejects_control_characters() {
        for unsafe_path in ["host/secret\0file", "host/secret\nfile"] {
            assert_eq!(
                HostPath::new(unsafe_path),
                Err(HostPathError::ControlCharacter)
            );
        }
    }

    #[test]
    fn host_path_debug_does_not_expose_logical_path() {
        let path = HostPath::new("private-sentinel/secret-file").unwrap();
        let debug = format!("{path:?}");

        assert!(debug.contains("byte_len"));
        assert!(debug.contains("segment_count"));
        assert!(!debug.contains("private-sentinel"));
        assert!(!debug.contains("secret-file"));
    }

    #[test]
    fn audio_buffer_debug_does_not_expose_samples() {
        let samples = [12_345.625_f32, -98_765.25_f32];
        let buffer = AudioBuffer {
            sample_rate: 48_000,
            channels: 2,
            samples: &samples,
        };
        let debug = format!("{buffer:?}");

        assert!(debug.contains("sample_rate: 48000"));
        assert!(debug.contains("channels: 2"));
        assert!(debug.contains("sample_count: 2"));
        assert!(!debug.contains("samples"));
        assert!(!debug.contains("12345"));
        assert!(!debug.contains("98765"));
    }
}
