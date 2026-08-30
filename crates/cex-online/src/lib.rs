//! Bounded online-service boundary for the CemuExtend Rust rewrite.
//!
//! A concrete worker may use Tokio internally, but host scheduling never
//! determines guest-visible completion order. Requests name a closed logical
//! service rather than an arbitrary URL, have a fixed payload limit, and are
//! correlated with deterministic guest-cycle timestamps.

use cex_types::GuestCycle;

/// Maximum opaque payload accepted by the online boundary.
pub const MAX_ONLINE_PAYLOAD_BYTES: usize = 1024 * 1024;

/// An online feature tracked by the compatibility plan.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(usize)]
pub enum OnlineCapability {
    /// HTTP and TLS transport.
    HttpTls,
    /// Nintendo account/NAS authentication flow.
    NasAuthentication,
    /// NEX protocol services.
    Nex,
    /// PRUDP transport.
    Prudp,
    /// Pretendo endpoint and certificate integration.
    Pretendo,
}

const CAPABILITY_COUNT: usize = 5;

/// The implementation status of an online capability.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CapabilityState {
    /// The Rust rewrite has not implemented the capability.
    #[default]
    NotImplemented,
    /// An implementation exists, but is unavailable in the current runtime.
    Unavailable,
    /// The current online backend can provide the capability.
    Available,
}

/// An immutable snapshot of online capabilities.
///
/// Keep the returned report when calling `none()` or `with_state()` because
/// each builder produces a new capability snapshot.
#[must_use = "OnlineCapabilities is an immutable snapshot; keep the returned report."]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OnlineCapabilities {
    states: [CapabilityState; CAPABILITY_COUNT],
}

impl OnlineCapabilities {
    /// Returns a report in which every online feature is unimplemented.
    pub const fn none() -> Self {
        Self {
            states: [CapabilityState::NotImplemented; CAPABILITY_COUNT],
        }
    }

    /// Returns a copy of this report with one capability state changed.
    pub const fn with_state(
        mut self,
        capability: OnlineCapability,
        state: CapabilityState,
    ) -> Self {
        self.states[capability as usize] = state;
        self
    }

    /// Returns the state of one capability.
    pub const fn state(self, capability: OnlineCapability) -> CapabilityState {
        self.states[capability as usize]
    }
}

impl Default for OnlineCapabilities {
    fn default() -> Self {
        Self::none()
    }
}

/// Logical protocol selected for an online request.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OnlineProtocol {
    /// HTTPS request using the configured certificate policy.
    Https,
    /// NEX request over an established session.
    Nex,
    /// PRUDP datagram exchange.
    Prudp,
}

/// Closed service vocabulary resolved through trusted endpoint configuration.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OnlineService {
    /// Nintendo account/NAS authentication.
    NasAuthentication,
    /// NEX account and session services.
    NexAuthentication,
    /// Friend-list service.
    Friends,
    /// Presence service.
    Presence,
}

/// Correlation identifier allocated by the emulation thread.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct OnlineRequestId(u64);

impl OnlineRequestId {
    /// Constructs an internal request identifier.
    #[must_use]
    pub const fn new(value: u64) -> Self {
        Self(value)
    }

    /// Returns the numeric identifier used for deterministic ordering.
    #[must_use]
    pub const fn get(self) -> u64 {
        self.0
    }
}

/// Error returned before an online request reaches a background worker.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OnlinePayloadError {
    /// The opaque protocol payload exceeded [`MAX_ONLINE_PAYLOAD_BYTES`].
    PayloadTooLarge {
        /// Actual payload size in bytes.
        actual: usize,
    },
}

/// An owned, size-checked request queued from emulation to an online worker.
///
/// Payloads may contain sensitive account material and deliberately do not
/// implement [`Debug`]. Implementations must redact them from logs and traces.
pub struct OnlineRequest {
    id: OnlineRequestId,
    submitted_at: GuestCycle,
    protocol: OnlineProtocol,
    service: OnlineService,
    payload: Vec<u8>,
}

impl OnlineRequest {
    /// Validates and constructs a bounded request to a logical service.
    pub fn new(
        id: OnlineRequestId,
        submitted_at: GuestCycle,
        protocol: OnlineProtocol,
        service: OnlineService,
        payload: Vec<u8>,
    ) -> Result<Self, OnlinePayloadError> {
        if payload.len() > MAX_ONLINE_PAYLOAD_BYTES {
            return Err(OnlinePayloadError::PayloadTooLarge {
                actual: payload.len(),
            });
        }

        Ok(Self {
            id,
            submitted_at,
            protocol,
            service,
            payload,
        })
    }

    /// Returns the correlation identifier.
    #[must_use]
    pub const fn id(&self) -> OnlineRequestId {
        self.id
    }

    /// Returns the deterministic guest submission timestamp.
    #[must_use]
    pub const fn submitted_at(&self) -> GuestCycle {
        self.submitted_at
    }

    /// Returns the selected protocol.
    #[must_use]
    pub const fn protocol(&self) -> OnlineProtocol {
        self.protocol
    }

    /// Returns the policy-resolved logical service.
    #[must_use]
    pub const fn service(&self) -> OnlineService {
        self.service
    }

    /// Returns the sensitive protocol payload.
    #[must_use]
    pub fn payload(&self) -> &[u8] {
        &self.payload
    }
}

/// An owned response returned to emulation.
///
/// Response bytes deliberately do not implement [`Debug`] to reduce accidental
/// credential disclosure.
pub struct OnlineResponse {
    status: u32,
    payload: Vec<u8>,
}

impl OnlineResponse {
    /// Validates and constructs a bounded protocol response.
    pub fn new(status: u32, payload: Vec<u8>) -> Result<Self, OnlinePayloadError> {
        if payload.len() > MAX_ONLINE_PAYLOAD_BYTES {
            return Err(OnlinePayloadError::PayloadTooLarge {
                actual: payload.len(),
            });
        }
        Ok(Self { status, payload })
    }

    /// Returns the protocol-specific status value.
    #[must_use]
    pub const fn status(&self) -> u32 {
        self.status
    }

    /// Returns the sensitive protocol response bytes.
    #[must_use]
    pub fn payload(&self) -> &[u8] {
        &self.payload
    }
}

/// One deterministically ordered completion from the background worker.
pub struct OnlineCompletion {
    /// Correlation identifier of the completed request.
    pub id: OnlineRequestId,
    /// Guest cycle at which emulation may observe the completion.
    pub visible_at: GuestCycle,
    /// Protocol response.
    pub response: OnlineResponse,
}

/// Boundary implemented by a future bounded HTTP/NEX/PRUDP worker.
///
/// Implementations must bound the pending queue, resolve [`OnlineService`]
/// through trusted configuration, and return completions ordered by
/// `(visible_at, id)` regardless of host completion timing.
pub trait OnlineBackend: Send + Sync {
    /// Backend-specific queue, policy, or transport failure.
    type Error;

    /// Reports protocols and services available in this process.
    fn capabilities(&self) -> OnlineCapabilities;

    /// Enqueues a request without blocking the emulation thread.
    fn enqueue(&self, request: OnlineRequest) -> Result<(), Self::Error>;

    /// Polls the next ordered completion visible at or before `guest_cycle`.
    fn poll_completion(
        &self,
        guest_cycle: GuestCycle,
    ) -> Result<Option<OnlineCompletion>, Self::Error>;

    /// Cancels a queued request, or rejects cancellation after visibility.
    fn cancel(&self, id: OnlineRequestId) -> Result<(), Self::Error>;
}

#[cfg(test)]
mod tests {
    use cex_types::GuestCycle;

    use super::{
        CapabilityState, MAX_ONLINE_PAYLOAD_BYTES, OnlineCapabilities, OnlineCapability,
        OnlinePayloadError, OnlineProtocol, OnlineRequest, OnlineRequestId, OnlineService,
    };

    #[test]
    fn default_report_does_not_claim_online_support() {
        let report = OnlineCapabilities::default();

        assert_eq!(
            report.state(OnlineCapability::HttpTls),
            CapabilityState::NotImplemented
        );
        assert_eq!(
            report.state(OnlineCapability::Pretendo),
            CapabilityState::NotImplemented
        );
    }

    #[test]
    fn request_rejects_oversized_payload_before_queueing() {
        let result = OnlineRequest::new(
            OnlineRequestId::new(1),
            GuestCycle::ZERO,
            OnlineProtocol::Https,
            OnlineService::NasAuthentication,
            vec![0; MAX_ONLINE_PAYLOAD_BYTES + 1],
        );

        assert!(matches!(
            result,
            Err(OnlinePayloadError::PayloadTooLarge { .. })
        ));
    }
}
