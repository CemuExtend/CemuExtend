use std::collections::BTreeSet;

use crate::{EventFields, TraceEntry, TraceEvent, TraceValue};

/// Conservative rules applied to every trace record before serialization.
///
/// The policy preserves the guest-derived structure needed by the oracle while
/// replacing credentials and other sensitive values with a stable sentinel.
#[derive(Clone, Debug)]
pub struct RedactionPolicy {
    sensitive_names: BTreeSet<&'static str>,
}

impl Default for RedactionPolicy {
    fn default() -> Self {
        Self {
            sensitive_names: [
                "account_id",
                "authorization",
                "certificate",
                "client_secret",
                "console_id",
                "console_key",
                "cookie",
                "credential",
                "device_id",
                "email",
                "mac_address",
                "otp",
                "password",
                "pnid",
                "private_key",
                "refresh_token",
                "serial_number",
                "session_id",
                "ticket",
                "token",
                "username",
            ]
            .into_iter()
            .collect(),
        }
    }
}

impl RedactionPolicy {
    /// Canonical text marker used for a value removed from trace output.
    pub const REPLACEMENT: &'static str = "<redacted>";

    /// Returns whether a field name denotes data that must not reach a trace.
    pub fn is_sensitive_field(&self, name: &str) -> bool {
        let normalized = normalize(name);
        self.sensitive_names.contains(normalized.as_str())
            || normalized == "key"
            || normalized.ends_with("_password")
            || normalized.ends_with("_secret")
            || normalized.ends_with("_token")
            || normalized.ends_with("_credential")
            || normalized.ends_with("_private_key")
            || normalized.ends_with("_key")
            || normalized.starts_with("auth_")
    }

    /// Returns whether a field name would encode host-dependent state.
    ///
    /// Such values are rejected rather than redacted because they make an
    /// otherwise identical guest execution non-reproducible across machines.
    pub fn is_forbidden_nondeterministic_field(&self, name: &str) -> bool {
        let normalized = normalize(name);
        [
            "absolute_path",
            "endpoint",
            "file_path",
            "filepath",
            "hostname",
            "host_path",
            "host_time",
            "pid",
            "pointer",
            "ptr",
            "thread_id",
            "timestamp",
            "unix_time",
            "uri",
            "url",
            "wall_clock",
            "wall_time",
        ]
        .contains(&normalized.as_str())
            || normalized.ends_with("_path")
            || normalized.ends_with("_pointer")
            || normalized.ends_with("_thread_id")
            || normalized.ends_with("_timestamp")
            || normalized.ends_with("_endpoint")
            || normalized.ends_with("_hostname")
            || normalized.ends_with("_uri")
            || normalized.ends_with("_url")
    }

    /// Redacts sensitive payload fields in an entry in place.
    pub fn redact_entry(&self, entry: &mut TraceEntry) {
        if let TraceEvent::Event(event) = &mut entry.event {
            self.redact_fields(&mut event.fields);
        }
    }

    /// Redacts sensitive values in a generic event-field map in place.
    pub fn redact_fields(&self, fields: &mut EventFields) {
        for (name, value) in fields {
            if self.requires_redaction(name, value) {
                *value = TraceValue::Redacted;
            }
        }
    }

    pub(crate) fn requires_redaction(&self, name: &str, value: &TraceValue) -> bool {
        self.is_sensitive_field(name) || looks_like_secret(value)
    }

    pub(crate) fn is_forbidden_text(value: &str) -> bool {
        let lowercase = value.to_ascii_lowercase();
        lowercase.contains("://")
            || lowercase.starts_with("urn:")
            || lowercase.contains("?token=")
            || lowercase.contains("&token=")
            || lowercase.contains("?key=")
            || lowercase.contains("&key=")
    }
}

fn normalize(name: &str) -> String {
    let mut normalized = String::with_capacity(name.len());
    let mut previous_was_separator = false;
    let mut previous_was_lowercase_or_digit = false;
    for character in name.chars() {
        if character.is_ascii_alphanumeric() {
            if character.is_ascii_uppercase()
                && previous_was_lowercase_or_digit
                && !previous_was_separator
            {
                normalized.push('_');
            }
            normalized.push(character.to_ascii_lowercase());
            previous_was_separator = false;
            previous_was_lowercase_or_digit =
                character.is_ascii_lowercase() || character.is_ascii_digit();
        } else if !previous_was_separator {
            normalized.push('_');
            previous_was_separator = true;
            previous_was_lowercase_or_digit = false;
        }
    }
    normalized.trim_matches('_').to_owned()
}

fn looks_like_secret(value: &TraceValue) -> bool {
    let TraceValue::Text(value) = value else {
        return false;
    };
    let lowercase = value.to_ascii_lowercase();
    lowercase.starts_with("bearer ")
        || lowercase.contains("-----begin private key-----")
        || lowercase.contains("-----begin certificate-----")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recognizes_normalized_sensitive_names() {
        let policy = RedactionPolicy::default();
        assert!(policy.is_sensitive_field("refresh-token"));
        assert!(policy.is_sensitive_field("PretendoPassword"));
        assert!(policy.is_sensitive_field("auth_header"));
        assert!(policy.is_sensitive_field("key"));
        assert!(policy.is_sensitive_field("apiKey"));
        assert!(policy.is_sensitive_field("clientKey"));
        assert!(policy.is_sensitive_field("signingKey"));
        assert!(policy.is_sensitive_field("consoleKey"));
        assert!(policy.is_sensitive_field("privateKey"));
        assert!(!policy.is_sensitive_field("memory_digest"));
    }

    #[test]
    fn key_matching_requires_an_exact_name_or_normalized_suffix() {
        let policy = RedactionPolicy::default();
        assert!(!policy.is_sensitive_field("keycode"));
        assert!(!policy.is_sensitive_field("keyboard_layout"));
        assert!(!policy.is_sensitive_field("monkey"));
        assert!(!policy.is_sensitive_field("key_id"));
    }

    #[test]
    fn redacts_camel_case_key_fields_without_redacting_key_related_labels() {
        let policy = RedactionPolicy::default();
        let mut fields = EventFields::from([
            ("apiKey".to_owned(), TraceValue::Text("secret".to_owned())),
            (
                "keyboard_layout".to_owned(),
                TraceValue::Text("us".to_owned()),
            ),
        ]);

        policy.redact_fields(&mut fields);

        assert_eq!(fields["apiKey"], TraceValue::Redacted);
        assert_eq!(fields["keyboard_layout"], TraceValue::Text("us".to_owned()));
    }

    #[test]
    fn rejects_nondeterministic_field_names() {
        let policy = RedactionPolicy::default();
        assert!(policy.is_forbidden_nondeterministic_field("host_path"));
        assert!(policy.is_forbidden_nondeterministic_field("workerThreadId"));
        assert!(policy.is_forbidden_nondeterministic_field("serviceUrl"));
        assert!(!policy.is_forbidden_nondeterministic_field("guest_address"));
    }
}
