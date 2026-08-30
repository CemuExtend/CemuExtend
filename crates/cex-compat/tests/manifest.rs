//! Integration coverage for fixture-manifest parsing and privacy invariants.

use cex_compat::{
    CompatError, CompatManifest, DecimalU64, ExpectedCheckpoint, ExpectedOutcome, FixtureStatus,
    MAX_MANIFEST_BYTES, MemoryHashAlgorithm, Sha256Digest, StopReason,
};

const SAMPLE: &str = include_str!("../../../compat/fixtures/synthetic.json");

#[test]
fn checked_in_sample_is_valid_and_stably_serialized() {
    let manifest = CompatManifest::from_json(SAMPLE.as_bytes()).unwrap();
    let serialized = manifest.to_pretty_json().unwrap();
    assert_eq!(serialized, SAMPLE.as_bytes());
    assert!(serialized.len() <= MAX_MANIFEST_BYTES);
}

#[test]
fn typed_manifest_rejects_private_material_fields() {
    let with_key = SAMPLE.replacen(
        "\"artifact\": {",
        "\"private_key\": \"secret bytes\",\n      \"artifact\": {",
        1,
    );
    let error = CompatManifest::from_json(with_key.as_bytes()).unwrap_err();
    assert!(matches!(error, CompatError::Json { .. }));
}

#[test]
fn fixtures_must_be_sorted_and_unique() {
    let mut manifest = CompatManifest::from_json(SAMPLE.as_bytes()).unwrap();
    manifest.fixtures.push(manifest.fixtures[0].clone());
    assert!(matches!(
        manifest.validate(),
        Err(CompatError::InvalidManifest(_))
    ));
}

#[test]
fn pretty_serialization_rejects_a_valid_manifest_over_the_byte_limit() {
    let mut manifest = CompatManifest::from_json(SAMPLE.as_bytes()).unwrap();
    let fixture = &mut manifest.fixtures[0];
    let digest = Sha256Digest::parse("0".repeat(Sha256Digest::HEX_LENGTH)).unwrap();
    fixture.status = FixtureStatus::Validated;
    fixture.expected = Some(ExpectedOutcome {
        terminal_reason: StopReason::TestCompleted,
        final_guest_cycle: DecimalU64(4096),
        trace_sha256: digest.clone(),
        checkpoints: (1..=4096)
            .map(|cycle| ExpectedCheckpoint {
                name: format!("checkpoint_{cycle:04}_{}", "x".repeat(96)),
                guest_cycle: DecimalU64(cycle),
                algorithm: MemoryHashAlgorithm::Sha256V1,
                state_digest: digest.clone(),
            })
            .collect(),
    });

    assert!(manifest.validate().is_ok());
    let first_error = manifest.to_pretty_json().unwrap_err();
    let second_error = manifest.to_pretty_json().unwrap_err();

    assert!(matches!(&first_error, CompatError::InvalidManifest(_)));
    assert_eq!(first_error.to_string(), second_error.to_string());
    assert!(
        first_error
            .to_string()
            .contains(&MAX_MANIFEST_BYTES.to_string())
    );
}
