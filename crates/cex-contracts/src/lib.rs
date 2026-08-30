//! Stable names and transport limits shared by CemuExtend frontends.
//!
//! The source of truth is `ui/contracts/rpc.json`. The payload expressions in
//! that manifest still describe the existing TypeScript bridge and are not a
//! complete language-neutral schema yet. The generated constants here let the
//! Rust host enforce the current method, event, and role vocabulary without
//! pretending to validate payloads before the JSON Schema migration lands.

use std::{error::Error, fmt};

/// One declared native-to-frontend event.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EventContract {
    /// Stable event name carried by the event envelope.
    pub name: &'static str,
    /// Stable manifest reference for the event payload type.
    pub payload_type_ref: &'static str,
}

include!("generated.rs");

/// A validated, byte-preserving request identifier.
#[derive(Clone, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct RequestId(String);

impl fmt::Debug for RequestId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RequestId")
            .field("byte_len", &self.0.len())
            .finish()
    }
}

impl RequestId {
    /// Validate and retain a request identifier exactly as received.
    pub fn new(value: impl Into<String>) -> Result<Self, ContractValidationError> {
        let value = value.into();
        if value.is_empty() {
            return Err(ContractValidationError::EmptyRequestId);
        }
        if value.len() > MAX_IDENTIFIER_BYTES {
            return Err(ContractValidationError::RequestIdTooLong {
                bytes: value.len(),
                maximum: MAX_IDENTIFIER_BYTES,
            });
        }
        Ok(Self(value))
    }

    /// Borrow the original UTF-8 identifier.
    #[must_use]
    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// Recover the original owned UTF-8 identifier.
    #[must_use]
    pub fn into_inner(self) -> String {
        self.0
    }
}

impl AsRef<str> for RequestId {
    fn as_ref(&self) -> &str {
        self.as_str()
    }
}

impl TryFrom<String> for RequestId {
    type Error = ContractValidationError;

    fn try_from(value: String) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl TryFrom<&str> for RequestId {
    type Error = ContractValidationError;

    fn try_from(value: &str) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

/// A method proven to be in the generated RPC allowlist.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct RpcMethod(&'static str);

impl RpcMethod {
    /// Validate a method name against the generated allowlist.
    pub fn new(value: &str) -> Result<Self, ContractValidationError> {
        if value.len() > MAX_IDENTIFIER_BYTES {
            return Err(ContractValidationError::RpcMethodTooLong {
                bytes: value.len(),
                maximum: MAX_IDENTIFIER_BYTES,
            });
        }
        RPC_METHODS
            .iter()
            .find(|method| **method == value)
            .copied()
            .map(Self)
            .ok_or(ContractValidationError::UnknownRpcMethod)
    }

    /// Borrow the stable method name.
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        self.0
    }
}

impl AsRef<str> for RpcMethod {
    fn as_ref(&self) -> &str {
        self.0
    }
}

impl fmt::Display for RpcMethod {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.0)
    }
}

impl TryFrom<&str> for RpcMethod {
    type Error = ContractValidationError;

    fn try_from(value: &str) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

/// A role proven to be in the generated window-role allowlist.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct WindowRole(&'static str);

impl WindowRole {
    /// Validate a role against all generated host and tool-window roles.
    pub fn new(value: &str) -> Result<Self, ContractValidationError> {
        if value.len() > MAX_IDENTIFIER_BYTES {
            return Err(ContractValidationError::WindowRoleTooLong {
                bytes: value.len(),
                maximum: MAX_IDENTIFIER_BYTES,
            });
        }
        WINDOW_ROLES
            .iter()
            .find(|role| **role == value)
            .copied()
            .map(Self)
            .ok_or(ContractValidationError::UnknownWindowRole)
    }

    /// Borrow the stable role name.
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        self.0
    }

    /// Whether this role can be opened as an implemented detached tool window.
    #[must_use]
    pub fn is_implemented_tool(self) -> bool {
        is_implemented_window_role(self.0)
    }
}

impl AsRef<str> for WindowRole {
    fn as_ref(&self) -> &str {
        self.0
    }
}

impl fmt::Display for WindowRole {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.0)
    }
}

impl TryFrom<&str> for WindowRole {
    type Error = ContractValidationError;

    fn try_from(value: &str) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

/// Failure to validate a transport identifier or generated allowlist name.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ContractValidationError {
    /// Request identifiers must contain at least one byte.
    EmptyRequestId,
    /// A request identifier exceeded the pinned transport limit.
    RequestIdTooLong {
        /// Actual UTF-8 byte length.
        bytes: usize,
        /// Maximum permitted UTF-8 byte length.
        maximum: usize,
    },
    /// A method identifier exceeded the pinned transport limit.
    RpcMethodTooLong {
        /// Actual UTF-8 byte length.
        bytes: usize,
        /// Maximum permitted UTF-8 byte length.
        maximum: usize,
    },
    /// A window-role identifier exceeded the pinned transport limit.
    WindowRoleTooLong {
        /// Actual UTF-8 byte length.
        bytes: usize,
        /// Maximum permitted UTF-8 byte length.
        maximum: usize,
    },
    /// The method is not part of the generated allowlist.
    UnknownRpcMethod,
    /// The role is not part of the generated allowlist.
    UnknownWindowRole,
}

impl fmt::Display for ContractValidationError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::EmptyRequestId => formatter.write_str("request id must not be empty"),
            Self::RequestIdTooLong { bytes, maximum } => write!(
                formatter,
                "request id is {bytes} bytes; the maximum is {maximum}"
            ),
            Self::RpcMethodTooLong { bytes, maximum } => write!(
                formatter,
                "RPC method is {bytes} bytes; the maximum is {maximum}"
            ),
            Self::WindowRoleTooLong { bytes, maximum } => write!(
                formatter,
                "window role is {bytes} bytes; the maximum is {maximum}"
            ),
            Self::UnknownRpcMethod => formatter.write_str("unknown RPC method"),
            Self::UnknownWindowRole => formatter.write_str("unknown window role"),
        }
    }
}

impl Error for ContractValidationError {}

/// Returns whether `method` is part of the public RPC allowlist.
#[must_use]
pub fn is_rpc_method(method: &str) -> bool {
    RPC_METHODS.contains(&method)
}

/// Returns whether `event` is declared by the native event contract.
#[must_use]
pub fn is_rpc_event(event: &str) -> bool {
    RPC_EVENTS.iter().any(|contract| contract.name == event)
}

/// Returns whether `role` is a known window role, including host-only roles.
#[must_use]
pub fn is_window_role(role: &str) -> bool {
    WINDOW_ROLES.contains(&role)
}

/// Returns whether `role` is an implemented detached tool-window role.
#[must_use]
pub fn is_implemented_window_role(role: &str) -> bool {
    IMPLEMENTED_WINDOW_ROLES.contains(&role)
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;

    use serde_json::Value;

    use super::*;

    fn strings(value: &Value) -> Vec<&str> {
        value
            .as_array()
            .expect("contract list must be an array")
            .iter()
            .map(|item| item.as_str().expect("contract name must be a string"))
            .collect()
    }

    fn assert_unique(values: impl IntoIterator<Item = &'static str>) {
        let values = values.into_iter().collect::<Vec<_>>();
        let unique = values.iter().copied().collect::<BTreeSet<_>>();
        assert_eq!(values.len(), unique.len());
    }

    fn assert_dotted_names(values: impl IntoIterator<Item = &'static str>) {
        for value in values {
            let mut segments = value.split('.');
            assert!(segments.clone().count() >= 2, "not dotted: {value}");
            assert!(segments.all(|segment| {
                let mut characters = segment.chars();
                characters.next().is_some_and(char::is_lowercase)
                    && characters.all(char::is_alphanumeric)
            }));
        }
    }

    #[test]
    fn pins_initial_contract_cardinality() {
        assert_eq!(SCHEMA_VERSION, 1);
        assert_eq!(RPC_METHODS.len(), 100);
        assert_eq!(RPC_EVENTS.len(), 22);
        assert_eq!(WINDOW_ROLES.len(), 22);
        assert_eq!(IMPLEMENTED_WINDOW_ROLES.len(), 20);
    }

    #[test]
    fn names_are_unique_and_well_formed() {
        assert_unique(RPC_METHODS);
        assert_unique(RPC_EVENTS.iter().map(|event| event.name));
        assert_unique(WINDOW_ROLES);
        assert_unique(IMPLEMENTED_WINDOW_ROLES);
        assert_dotted_names(RPC_METHODS);
        assert_dotted_names(RPC_EVENTS.iter().map(|event| event.name));
    }

    #[test]
    fn implemented_roles_are_known_tool_roles() {
        assert!(!is_implemented_window_role("main-library"));
        assert!(!is_implemented_window_role("runtime-overlay"));
        assert!(
            IMPLEMENTED_WINDOW_ROLES
                .iter()
                .all(|role| is_window_role(role))
        );
    }

    #[test]
    fn pins_native_transport_limits() {
        assert_eq!(MAX_REQUEST_BYTES, 1024 * 1024);
        assert_eq!(MAX_IDENTIFIER_BYTES, 128);
        assert_eq!(REMEMBERED_REQUEST_IDS, 4096);
    }

    #[test]
    fn all_events_retain_payload_type_references() {
        assert!(
            RPC_EVENTS
                .iter()
                .all(|event| !event.payload_type_ref.is_empty())
        );
    }

    #[test]
    fn request_ids_use_utf8_byte_limits_without_normalization() {
        let identifier = "é".repeat(MAX_IDENTIFIER_BYTES / 2);
        let request_id = RequestId::new(identifier.clone()).expect("identifier fits exactly");
        assert_eq!(request_id.as_str(), identifier);
        assert_eq!(request_id.into_inner(), identifier);
        assert_eq!(
            RequestId::new("é".repeat(MAX_IDENTIFIER_BYTES / 2 + 1)),
            Err(ContractValidationError::RequestIdTooLong {
                bytes: MAX_IDENTIFIER_BYTES + 2,
                maximum: MAX_IDENTIFIER_BYTES,
            })
        );
        assert_eq!(
            RequestId::new(""),
            Err(ContractValidationError::EmptyRequestId)
        );
    }

    #[test]
    fn generated_allowlist_wrappers_reject_unknown_names() {
        let method = RpcMethod::new("system.bootstrap").expect("known method");
        assert_eq!(method.as_str(), "system.bootstrap");
        assert!(matches!(
            RpcMethod::new("native.eval"),
            Err(ContractValidationError::UnknownRpcMethod)
        ));

        let tool = WindowRole::new("general-settings").expect("known role");
        assert!(tool.is_implemented_tool());
        let main = WindowRole::new("main-library").expect("known host role");
        assert!(!main.is_implemented_tool());
        assert!(matches!(
            WindowRole::new("arbitrary-window"),
            Err(ContractValidationError::UnknownWindowRole)
        ));
    }

    #[test]
    fn window_roles_use_utf8_byte_limits_before_retaining_unknown_values() {
        let exact_limit = "é".repeat(MAX_IDENTIFIER_BYTES / 2);
        assert!(matches!(
            WindowRole::new(&exact_limit),
            Err(ContractValidationError::UnknownWindowRole)
        ));

        assert_eq!(
            WindowRole::new(&(exact_limit + "a")),
            Err(ContractValidationError::WindowRoleTooLong {
                bytes: MAX_IDENTIFIER_BYTES + 1,
                maximum: MAX_IDENTIFIER_BYTES,
            })
        );
    }

    #[test]
    fn unknown_name_errors_are_bounded_and_redacted() {
        let method_error = RpcMethod::new("rpc-secret-sentinel").expect_err("unknown method");
        let role_error =
            WindowRole::new("window-secret-sentinel").expect_err("unknown window role");

        assert_eq!(method_error.to_string(), "unknown RPC method");
        assert_eq!(role_error.to_string(), "unknown window role");
        assert_eq!(format!("{method_error:?}"), "UnknownRpcMethod");
        assert_eq!(format!("{role_error:?}"), "UnknownWindowRole");
        assert!(!method_error.to_string().contains("secret-sentinel"));
        assert!(!role_error.to_string().contains("secret-sentinel"));
        assert!(!format!("{method_error:?}").contains("secret-sentinel"));
        assert!(!format!("{role_error:?}").contains("secret-sentinel"));
    }

    #[test]
    fn request_id_debug_reports_only_byte_length() {
        let request_id = RequestId::new("request-secret-sentinel").expect("valid request id");
        let rendered = format!("{request_id:?}");

        assert_eq!(rendered, "RequestId { byte_len: 23 }");
        assert!(!rendered.contains("request-secret-sentinel"));
    }

    #[test]
    fn generated_constants_match_the_manifest() {
        let manifest: Value = serde_json::from_str(include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../ui/contracts/rpc.json"
        )))
        .expect("rpc.json must contain valid JSON");

        assert_eq!(manifest["schemaVersion"], SCHEMA_VERSION);
        assert_eq!(
            manifest["wireProtocol"]["limits"]["maxRequestBytes"],
            MAX_REQUEST_BYTES
        );
        assert_eq!(
            manifest["wireProtocol"]["limits"]["maxIdentifierBytes"],
            MAX_IDENTIFIER_BYTES
        );
        assert_eq!(
            manifest["wireProtocol"]["limits"]["rememberedRequestIds"],
            REMEMBERED_REQUEST_IDS
        );
        assert_eq!(strings(&manifest["windowRoles"]), WINDOW_ROLES);
        assert_eq!(
            strings(&manifest["implementedWindowRoles"]),
            IMPLEMENTED_WINDOW_ROLES
        );
        assert_eq!(
            manifest["methods"]
                .as_array()
                .expect("methods must be an array")
                .iter()
                .map(|method| method["name"]
                    .as_str()
                    .expect("method name must be a string"))
                .collect::<Vec<_>>(),
            RPC_METHODS
        );
        let manifest_events = manifest["events"]
            .as_array()
            .expect("events must be an array")
            .iter()
            .map(|event| {
                (
                    event["name"].as_str().expect("event name must be a string"),
                    event["payloadTypeRef"]
                        .as_str()
                        .expect("event payload type reference must be a string"),
                )
            })
            .collect::<Vec<_>>();
        let generated_events = RPC_EVENTS
            .iter()
            .map(|event| (event.name, event.payload_type_ref))
            .collect::<Vec<_>>();
        assert_eq!(manifest_events, generated_events);
    }
}
