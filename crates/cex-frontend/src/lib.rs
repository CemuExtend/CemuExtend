//! Frontend boundary for the existing React UI and third-party CEF runtime.
//!
//! RPC payloads remain opaque JSON at this layer so the generated contract
//! crate can preserve the existing wire shape. No CEF host or RPC bridge is
//! implemented here.

use std::{error::Error, fmt};

use cex_contracts::{MAX_REQUEST_BYTES, RequestId, RpcMethod, WindowRole};
use serde_json::{Value, json};

/// Maximum response JSON size accepted by the frontend adapter.
///
/// This is an adapter-local resource guard for bounding serde allocations; it
/// is intentionally separate from the shared request wire limit.
pub const MAX_RESPONSE_BYTES: usize = 64 * 1024 * 1024;

/// A frontend feature tracked by the compatibility plan.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(usize)]
pub enum FrontendCapability {
    /// React launcher hosted in CEF.
    CefLauncher,
    /// CEF off-screen rendering for guest overlays.
    CefOsrOverlay,
    /// Application-owned URL scheme handling.
    CustomScheme,
    /// Bidirectional typed RPC bridge.
    RpcBridge,
    /// Frontend navigation and network policy enforcement.
    NetworkPolicy,
}

const CAPABILITY_COUNT: usize = 5;

/// The implementation status of a frontend capability.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CapabilityState {
    /// The Rust rewrite has not implemented the capability.
    #[default]
    NotImplemented,
    /// An implementation exists, but is unavailable in the current runtime.
    Unavailable,
    /// The current frontend backend can provide the capability.
    Available,
}

/// An immutable snapshot of frontend capabilities.
///
/// Constructors and builder methods return a new snapshot; callers should keep
/// the returned value instead of discarding it.
#[must_use = "frontend capability snapshots must be retained to observe their state"]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FrontendCapabilities {
    states: [CapabilityState; CAPABILITY_COUNT],
}

impl FrontendCapabilities {
    /// Returns a report in which every frontend feature is unimplemented.
    pub const fn none() -> Self {
        Self {
            states: [CapabilityState::NotImplemented; CAPABILITY_COUNT],
        }
    }

    /// Returns a copy of this report with one capability state changed.
    pub const fn with_state(
        mut self,
        capability: FrontendCapability,
        state: CapabilityState,
    ) -> Self {
        self.states[capability as usize] = state;
        self
    }

    /// Returns the state of one capability.
    pub const fn state(self, capability: FrontendCapability) -> CapabilityState {
        self.states[capability as usize]
    }
}

impl Default for FrontendCapabilities {
    fn default() -> Self {
        Self::none()
    }
}

/// A validated RPC call whose JSON payload preserves the existing wire shape.
///
/// Its [`fmt::Debug`] implementation intentionally reports only identifier and
/// payload sizes (plus the allowlisted method), never the raw values.
#[derive(Clone, Eq, PartialEq)]
pub struct RpcCall {
    /// Correlation identifier supplied by the caller.
    id: RequestId,
    /// Method name from the RPC manifest.
    method: RpcMethod,
    /// JSON-encoded parameters, without frontend-specific transformation.
    params_json: String,
}

impl fmt::Debug for RpcCall {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcCall")
            .field("id_bytes", &self.id.as_str().len())
            .field("method", &self.method.as_str())
            .field("params_bytes", &self.params_json.len())
            .finish()
    }
}

impl RpcCall {
    /// Validates object parameters and the pinned maximum request size.
    pub fn new(
        id: RequestId,
        method: RpcMethod,
        params_json: String,
    ) -> Result<Self, RpcEnvelopeError> {
        if params_json.len() > MAX_REQUEST_BYTES {
            return Err(RpcEnvelopeError::RequestTooLarge {
                bytes: params_json.len(),
                maximum: MAX_REQUEST_BYTES,
            });
        }
        let params: Value =
            serde_json::from_str(&params_json).map_err(|_| RpcEnvelopeError::InvalidJson)?;
        if !params.is_object() {
            return Err(RpcEnvelopeError::ParamsMustBeObject);
        }
        let encoded = serde_json::to_vec(&json!({
            "id": id.as_str(),
            "method": method.as_str(),
            "params": params,
        }))
        .map_err(|_| RpcEnvelopeError::InvalidJson)?;
        if encoded.len() > MAX_REQUEST_BYTES {
            return Err(RpcEnvelopeError::RequestTooLarge {
                bytes: encoded.len(),
                maximum: MAX_REQUEST_BYTES,
            });
        }

        Ok(Self {
            id,
            method,
            params_json,
        })
    }

    /// Returns the byte-preserving request identifier.
    #[must_use]
    pub fn id(&self) -> &RequestId {
        &self.id
    }

    /// Returns the allowlisted RPC method.
    #[must_use]
    pub const fn method(&self) -> RpcMethod {
        self.method
    }

    /// Returns the validated JSON object parameters.
    #[must_use]
    pub fn params_json(&self) -> &str {
        &self.params_json
    }
}

/// A complete validated `{id,ok,result|error}` response envelope.
///
/// The full envelope, rather than a bare result payload, crosses the CEF
/// adapter boundary so the request identifier is echoed without conversion.
/// Its [`fmt::Debug`] implementation reports only the byte length and
/// validated `ok` status, never the raw response JSON.
#[derive(Clone, Eq, PartialEq)]
pub struct RpcResponseEnvelope {
    json: String,
    ok: bool,
}

impl fmt::Debug for RpcResponseEnvelope {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcResponseEnvelope")
            .field("response_bytes", &self.json.len())
            .field("ok", &self.ok)
            .finish()
    }
}

impl RpcResponseEnvelope {
    /// Validates a complete response envelope against its expected request ID.
    pub fn new(id: &RequestId, json: String) -> Result<Self, RpcEnvelopeError> {
        if json.len() > MAX_RESPONSE_BYTES {
            return Err(RpcEnvelopeError::ResponseTooLarge {
                bytes: json.len(),
                maximum: MAX_RESPONSE_BYTES,
            });
        }
        let value: Value =
            serde_json::from_str(&json).map_err(|_| RpcEnvelopeError::InvalidJson)?;
        let object = value
            .as_object()
            .ok_or(RpcEnvelopeError::InvalidResponseShape)?;
        if object.get("id").and_then(Value::as_str) != Some(id.as_str()) {
            return Err(RpcEnvelopeError::ResponseIdMismatch);
        }
        let ok = object
            .get("ok")
            .and_then(Value::as_bool)
            .ok_or(RpcEnvelopeError::InvalidResponseShape)?;
        let valid = if ok {
            object.contains_key("result")
        } else {
            let error = object.get("error").and_then(Value::as_object);
            error.is_some_and(|error| {
                error.get("code").is_some_and(Value::is_string)
                    && error.get("message").is_some_and(Value::is_string)
            })
        };
        if !valid {
            return Err(RpcEnvelopeError::InvalidResponseShape);
        }
        Ok(Self { json, ok })
    }

    /// Returns the complete response JSON passed to CEF unchanged.
    #[must_use]
    pub fn as_json(&self) -> &str {
        &self.json
    }
}

/// Failure to validate an RPC payload at the frontend boundary.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpcEnvelopeError {
    /// Payload text was not valid JSON.
    InvalidJson,
    /// Request parameters must be a JSON object.
    ParamsMustBeObject,
    /// A serialized request exceeded the pinned transport limit.
    RequestTooLarge {
        /// Actual serialized byte length.
        bytes: usize,
        /// Maximum permitted byte length.
        maximum: usize,
    },
    /// A response exceeded the frontend adapter's resource guard.
    ResponseTooLarge {
        /// Actual response byte length.
        bytes: usize,
        /// Maximum permitted response byte length.
        maximum: usize,
    },
    /// A response did not use the exact success or error wire shape.
    InvalidResponseShape,
    /// A response did not echo the expected request identifier.
    ResponseIdMismatch,
}

impl fmt::Display for RpcEnvelopeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidJson => formatter.write_str("RPC payload is not valid JSON"),
            Self::ParamsMustBeObject => formatter.write_str("RPC params must be an object"),
            Self::RequestTooLarge { bytes, maximum } => {
                write!(
                    formatter,
                    "RPC request is {bytes} bytes; maximum is {maximum}"
                )
            }
            Self::ResponseTooLarge { bytes, maximum } => {
                write!(
                    formatter,
                    "RPC response is {bytes} bytes; maximum is {maximum}"
                )
            }
            Self::InvalidResponseShape => formatter.write_str("invalid RPC response shape"),
            Self::ResponseIdMismatch => formatter.write_str("RPC response id does not match"),
        }
    }
}

impl Error for RpcEnvelopeError {}

/// An event crossing from the frontend process into the Rust host.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum FrontendEvent {
    /// A frontend window has finished loading.
    Ready(WindowRole),
    /// A manifest-defined RPC call arrived.
    Rpc(RpcCall),
    /// A frontend window was closed.
    Closed(WindowRole),
}

/// Boundary implemented by a future CEF adapter.
pub trait FrontendBridge: Send {
    /// Frontend-specific startup or transport failure.
    type Error;

    /// Reports frontend features available in this process.
    fn capabilities(&self) -> FrontendCapabilities;

    /// Opens one manifest-defined window role.
    fn open_window(&mut self, role: &WindowRole) -> Result<(), Self::Error>;

    /// Polls one event without indefinitely blocking the emulation thread.
    fn poll_event(&mut self) -> Result<Option<FrontendEvent>, Self::Error>;

    /// Delivers an RPC result using an unchanged JSON wire payload.
    fn complete_rpc(&mut self, response: &RpcResponseEnvelope) -> Result<(), Self::Error>;
}

#[cfg(test)]
mod tests {
    use cex_contracts::{MAX_REQUEST_BYTES, RequestId, RpcMethod};

    use super::{
        CapabilityState, FrontendCapabilities, FrontendCapability, FrontendEvent,
        MAX_RESPONSE_BYTES, RpcCall, RpcEnvelopeError, RpcResponseEnvelope,
    };

    #[test]
    fn default_report_does_not_claim_frontend_support() {
        let report = FrontendCapabilities::default();

        assert_eq!(
            report.state(FrontendCapability::CefLauncher),
            CapabilityState::NotImplemented
        );
        assert_eq!(
            report.state(FrontendCapability::RpcBridge),
            CapabilityState::NotImplemented
        );
    }

    #[test]
    fn oversized_malformed_params_are_rejected_before_json_parsing() {
        let result = RpcCall::new(
            RequestId::new("request-1").expect("valid fixture id"),
            RpcMethod::new("system.bootstrap").expect("allowlisted fixture method"),
            "[".repeat(MAX_REQUEST_BYTES + 1),
        );

        assert!(matches!(
            result,
            Err(RpcEnvelopeError::RequestTooLarge { .. })
        ));
    }

    #[test]
    fn response_ignores_unknown_outer_and_error_fields() {
        let id = RequestId::new("request-1").expect("valid fixture id");
        let result = RpcResponseEnvelope::new(
            &id,
            r#"{"id":"request-1","ok":false,"futureOuter":true,"error":{"code":"failed","message":"failed","futureNested":true}}"#.to_owned(),
        );

        assert!(result.is_ok());
    }

    #[test]
    fn oversized_malformed_response_is_rejected_before_json_parsing() {
        let id = RequestId::new("request-1").expect("valid fixture id");
        let result = RpcResponseEnvelope::new(&id, "[".repeat(MAX_RESPONSE_BYTES + 1));

        assert!(matches!(
            result,
            Err(RpcEnvelopeError::ResponseTooLarge { bytes, maximum })
                if bytes == MAX_RESPONSE_BYTES + 1 && maximum == MAX_RESPONSE_BYTES
        ));
    }

    #[test]
    fn rpc_debug_redacts_identifier_and_params_through_frontend_event() {
        let id = RequestId::new("request-secret-sentinel").expect("valid fixture id");
        let call = RpcCall::new(
            id,
            RpcMethod::new("system.bootstrap").expect("allowlisted fixture method"),
            r#"{"token":"params-secret-sentinel"}"#.to_owned(),
        )
        .expect("valid fixture call");

        let rendered = format!("{call:?}");
        assert!(!rendered.contains("request-secret-sentinel"));
        assert!(!rendered.contains("params-secret-sentinel"));
        assert!(rendered.contains("id_bytes"));
        assert!(rendered.contains("system.bootstrap"));
        assert!(rendered.contains("params_bytes"));

        let event = FrontendEvent::Rpc(call);
        let event_rendered = format!("{event:?}");
        assert!(!event_rendered.contains("request-secret-sentinel"));
        assert!(!event_rendered.contains("params-secret-sentinel"));
    }

    #[test]
    fn response_debug_redacts_json_and_reports_validated_status() {
        let id = RequestId::new("request-1").expect("valid fixture id");
        let response = RpcResponseEnvelope::new(
            &id,
            r#"{"id":"request-1","ok":false,"error":{"code":"failed","message":"response-secret-sentinel"}}"#.to_owned(),
        )
        .expect("valid fixture response");

        let rendered = format!("{response:?}");
        assert!(!rendered.contains("response-secret-sentinel"));
        assert!(!rendered.contains("request-1"));
        assert!(rendered.contains("response_bytes"));
        assert!(rendered.contains("ok: false"));
    }
}
