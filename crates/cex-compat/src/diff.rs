use std::{collections::BTreeSet, fmt};

use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::{RedactionPolicy, TraceEntry, TraceKey};

const VALUE_SUMMARY_LIMIT: usize = 256;
const VALUE_SUMMARY_SUFFIX: &str = "…";

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
/// Classification of the first observable difference between two traces.
///
/// The value is part of the compatibility oracle's diagnostic contract: it
/// tells callers whether ordering, schema selection, or a record payload
/// first prevented an exact canonical comparison.
pub enum TraceMismatchKind {
    /// Corresponding records declare different trace-schema versions.
    SchemaVersion,
    /// The expected producer emitted a record that the actual producer omitted.
    MissingEntry,
    /// The actual producer emitted a record absent from the expected trace.
    UnexpectedEntry,
    /// Matching record keys contain different canonical JSON values.
    DivergentValue,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// The first mismatch found while comparing an expected and actual trace.
///
/// Values are already redacted before they are returned, so this diagnostic
/// can be safely included in compatibility-test output.
pub struct TraceMismatch {
    /// The kind of incompatibility that stopped comparison.
    pub kind: TraceMismatchKind,
    /// Zero-based position of the expected record, or its end-of-trace index.
    pub expected_index: usize,
    /// Zero-based position of the actual record, or its end-of-trace index.
    pub actual_index: usize,
    /// `(guest_cycle, sequence)` of the expected record, when one exists.
    pub expected_key: Option<String>,
    /// `(guest_cycle, sequence)` of the actual record, when one exists.
    pub actual_key: Option<String>,
    /// JSONPath-like location of the first divergence.
    pub path: String,
    /// Redacted expected leaf value at [`Self::path`], when present.
    pub expected: Option<Value>,
    /// Redacted actual leaf value at [`Self::path`], when present.
    pub actual: Option<Value>,
    /// Actionable, category-aware suggestion for investigating the mismatch.
    pub hint: String,
}

impl fmt::Display for TraceMismatch {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(
            formatter,
            "trace mismatch ({:?}) at {}",
            self.kind, self.path
        )?;
        writeln!(
            formatter,
            "  expected entry {} key {}",
            self.expected_index,
            self.expected_key.as_deref().unwrap_or("<end-of-trace>")
        )?;
        writeln!(
            formatter,
            "  actual entry   {} key {}",
            self.actual_index,
            self.actual_key.as_deref().unwrap_or("<end-of-trace>")
        )?;
        if let Some(expected) = &self.expected {
            writeln!(formatter, "  expected: {}", value_summary(expected))?;
        }
        if let Some(actual) = &self.actual {
            writeln!(formatter, "  actual:   {}", value_summary(actual))?;
        }
        write!(formatter, "  hint: {}", self.hint)
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
/// Summary returned by the deterministic first-difference trace comparator.
pub struct TraceDiff {
    /// Number of complete entries proven equal before the mismatch.
    pub compared_entries: usize,
    /// The first mismatch, or `None` when both traces are identical.
    pub mismatch: Option<TraceMismatch>,
}

impl TraceDiff {
    /// Returns whether the expected and actual canonical traces match exactly.
    pub fn is_match(&self) -> bool {
        self.mismatch.is_none()
    }
}

/// Compare two already parsed traces and report only the first divergence.
///
/// Entries are compared by `(guest_cycle, sequence)` and comparison stops at
/// the first missing, unexpected, or divergent record.
pub fn diff_traces(expected: &[TraceEntry], actual: &[TraceEntry]) -> TraceDiff {
    let policy = RedactionPolicy::default();
    let mut expected_index = 0;
    let mut actual_index = 0;
    let mut compared_entries = 0;

    while expected_index < expected.len() && actual_index < actual.len() {
        let expected_entry = &expected[expected_index];
        let actual_entry = &actual[actual_index];
        let expected_key = expected_entry.key();
        let actual_key = actual_entry.key();

        if expected_entry.schema_version != actual_entry.schema_version {
            return TraceDiff {
                compared_entries,
                mismatch: Some(TraceMismatch {
                    kind: TraceMismatchKind::SchemaVersion,
                    expected_index,
                    actual_index,
                    expected_key: Some(expected_key.to_string()),
                    actual_key: Some(actual_key.to_string()),
                    path: "$.schema_version".to_owned(),
                    expected: Some(Value::from(expected_entry.schema_version)),
                    actual: Some(Value::from(actual_entry.schema_version)),
                    hint: "Generate both traces with the same trace schema version.".to_owned(),
                }),
            };
        }

        if expected_key < actual_key {
            return TraceDiff {
                compared_entries,
                mismatch: Some(missing_entry(
                    expected_index,
                    actual_index,
                    expected_key,
                    actual_key,
                    expected_entry.category,
                )),
            };
        }
        if actual_key < expected_key {
            return TraceDiff {
                compared_entries,
                mismatch: Some(unexpected_entry(
                    expected_index,
                    actual_index,
                    expected_key,
                    actual_key,
                    actual_entry.category,
                )),
            };
        }

        let expected_value = redacted_value(expected_entry, &policy);
        let actual_value = redacted_value(actual_entry, &policy);
        if let Some((path, expected_leaf, actual_leaf)) =
            first_json_divergence(&expected_value, &actual_value, "$".to_owned())
        {
            return TraceDiff {
                compared_entries,
                mismatch: Some(TraceMismatch {
                    kind: TraceMismatchKind::DivergentValue,
                    expected_index,
                    actual_index,
                    expected_key: Some(expected_key.to_string()),
                    actual_key: Some(actual_key.to_string()),
                    hint: hint_for_path(&path),
                    path,
                    expected: expected_leaf,
                    actual: actual_leaf,
                }),
            };
        }

        compared_entries += 1;
        expected_index += 1;
        actual_index += 1;
    }

    TraceDiff {
        compared_entries,
        mismatch: trailing_mismatch(expected, actual, expected_index, actual_index, &policy),
    }
}

fn trailing_mismatch(
    expected: &[TraceEntry],
    actual: &[TraceEntry],
    expected_index: usize,
    actual_index: usize,
    policy: &RedactionPolicy,
) -> Option<TraceMismatch> {
    if expected_index < expected.len() {
        let entry = &expected[expected_index];
        Some(TraceMismatch {
            kind: TraceMismatchKind::MissingEntry,
            expected_index,
            actual_index,
            expected_key: Some(entry.key().to_string()),
            actual_key: None,
            path: "$".to_owned(),
            expected: Some(redacted_value(entry, policy)),
            actual: None,
            hint: "The actual trace ended before the expected terminal sequence.".to_owned(),
        })
    } else if actual_index < actual.len() {
        let entry = &actual[actual_index];
        Some(TraceMismatch {
            kind: TraceMismatchKind::UnexpectedEntry,
            expected_index,
            actual_index,
            expected_key: None,
            actual_key: Some(entry.key().to_string()),
            path: "$".to_owned(),
            expected: None,
            actual: Some(redacted_value(entry, policy)),
            hint: "The actual trace emitted records after the expected terminal sequence."
                .to_owned(),
        })
    } else {
        None
    }
}

fn missing_entry(
    expected_index: usize,
    actual_index: usize,
    expected_key: TraceKey,
    actual_key: TraceKey,
    category: crate::TraceCategory,
) -> TraceMismatch {
    TraceMismatch {
        kind: TraceMismatchKind::MissingEntry,
        expected_index,
        actual_index,
        expected_key: Some(expected_key.to_string()),
        actual_key: Some(actual_key.to_string()),
        path: "$.guest_cycle".to_owned(),
        expected: Some(Value::String(expected_key.guest_cycle.to_string())),
        actual: Some(Value::String(actual_key.guest_cycle.to_string())),
        hint: format!(
            "The actual trace skipped the expected {category:?} record; inspect scheduling or event emission immediately before this guest cycle."
        ),
    }
}

fn unexpected_entry(
    expected_index: usize,
    actual_index: usize,
    expected_key: TraceKey,
    actual_key: TraceKey,
    category: crate::TraceCategory,
) -> TraceMismatch {
    TraceMismatch {
        kind: TraceMismatchKind::UnexpectedEntry,
        expected_index,
        actual_index,
        expected_key: Some(expected_key.to_string()),
        actual_key: Some(actual_key.to_string()),
        path: "$.guest_cycle".to_owned(),
        expected: Some(Value::String(expected_key.guest_cycle.to_string())),
        actual: Some(Value::String(actual_key.guest_cycle.to_string())),
        hint: format!(
            "The actual trace emitted an extra {category:?} record; inspect scheduling or duplicate event emission at this guest cycle."
        ),
    }
}

fn redacted_value(entry: &TraceEntry, policy: &RedactionPolicy) -> Value {
    let mut entry = entry.clone();
    policy.redact_entry(&mut entry);
    serde_json::to_value(entry).expect("TraceEntry serialization is infallible")
}

fn first_json_divergence(
    expected: &Value,
    actual: &Value,
    path: String,
) -> Option<(String, Option<Value>, Option<Value>)> {
    match (expected, actual) {
        (Value::Object(expected), Value::Object(actual)) => {
            let keys: BTreeSet<_> = expected.keys().chain(actual.keys()).collect();
            for key in keys {
                let escaped_key = escape_path_key(key);
                let child_path = format!("{path}.{escaped_key}");
                match (expected.get(key), actual.get(key)) {
                    (Some(expected), Some(actual)) => {
                        if let Some(mismatch) = first_json_divergence(expected, actual, child_path)
                        {
                            return Some(mismatch);
                        }
                    }
                    (expected, actual) => {
                        return Some((child_path, expected.cloned(), actual.cloned()));
                    }
                }
            }
            None
        }
        (Value::Array(expected), Value::Array(actual)) => {
            let common_length = expected.len().min(actual.len());
            for index in 0..common_length {
                if let Some(mismatch) = first_json_divergence(
                    &expected[index],
                    &actual[index],
                    format!("{path}[{index}]"),
                ) {
                    return Some(mismatch);
                }
            }
            if expected.len() != actual.len() {
                let index = common_length;
                return Some((
                    format!("{path}[{index}]"),
                    expected.get(index).cloned(),
                    actual.get(index).cloned(),
                ));
            }
            None
        }
        _ if expected == actual => None,
        _ => Some((path, Some(expected.clone()), Some(actual.clone()))),
    }
}

fn escape_path_key(key: &str) -> String {
    if key
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
    {
        key.to_owned()
    } else {
        let escaped_key = key.replace('\'', "\\'");
        format!("['{escaped_key}']")
    }
}

fn hint_for_path(path: &str) -> String {
    if path.contains(".event.gpr[") {
        "A general-purpose register diverged; compare the instruction executed immediately before this checkpoint.".to_owned()
    } else if path.contains(".event.fpr_bits[") || path.ends_with(".event.fpscr") {
        "A floating-point raw bit pattern diverged; check paired-single, rounding, NaN, and denormal handling.".to_owned()
    } else if path.ends_with(".event.digest") || path.ends_with(".event.state_digest") {
        "A deterministic state hash diverged; narrow the named range or insert an earlier checkpoint.".to_owned()
    } else if path.contains(".event.fields.") {
        "An event payload diverged; compare the producer's guest-derived inputs at this cycle."
            .to_owned()
    } else {
        "Compare the two producers at the reported guest cycle and JSON path.".to_owned()
    }
}

fn value_summary(value: &Value) -> String {
    let mut summary = serde_json::to_string(value).unwrap_or_else(|_| "<unprintable>".to_owned());
    if summary.len() > VALUE_SUMMARY_LIMIT {
        summary = truncate_summary(summary, VALUE_SUMMARY_LIMIT, VALUE_SUMMARY_SUFFIX);
    }
    summary
}

fn truncate_summary(mut summary: String, limit: usize, suffix: &str) -> String {
    if summary.len() <= limit {
        return summary;
    }

    let suffix_len = suffix.len();
    if limit <= suffix_len {
        summary.truncate(limit);
        return summary;
    }

    let target_len = limit - suffix_len;
    let truncate_len = summary
        .char_indices()
        .map(|(index, _)| index)
        .chain(std::iter::once(summary.len()))
        .take_while(|&index| index <= target_len)
        .last()
        .unwrap_or(0);

    summary.truncate(truncate_len);
    summary.push_str(suffix);
    summary
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn value_summary_truncates_multibyte_strings_without_breaking_utf8() {
        let long_text = "あ🙂".repeat(80);
        let summary = value_summary(&Value::String(long_text));

        assert!(summary.is_char_boundary(summary.len()));
        assert!(summary.len() <= VALUE_SUMMARY_LIMIT);
        assert!(summary.ends_with(VALUE_SUMMARY_SUFFIX));
    }

    #[test]
    fn value_summary_preserves_short_redacted_values_exactly() {
        let summary = value_summary(&Value::String("<redacted>".to_owned()));

        assert_eq!(summary, "\"<redacted>\"");
    }

    #[test]
    fn trace_mismatch_display_handles_multibyte_summaries() {
        let mismatch = TraceMismatch {
            kind: TraceMismatchKind::DivergentValue,
            expected_index: 0,
            actual_index: 0,
            expected_key: Some("0:0".to_owned()),
            actual_key: Some("0:0".to_owned()),
            path: "$.event.fields.message".to_owned(),
            expected: Some(json!("あ🙂".repeat(80))),
            actual: Some(json!("い🙃".repeat(80))),
            hint: "Compare the two producers at the reported guest cycle and JSON path.".to_owned(),
        };

        let rendered = format!("{mismatch}");

        assert!(rendered.contains(VALUE_SUMMARY_SUFFIX));
        assert!(rendered.contains("trace mismatch (DivergentValue) at $.event.fields.message"));
        assert!(rendered.contains("  expected: "));
        assert!(rendered.contains("  actual:   "));
    }
}
