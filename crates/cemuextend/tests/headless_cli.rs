//! Integration tests for the headless CemuExtend command-line interface.

use std::fs::File;
use std::io::BufReader;
use std::process::Command;

use cex_compat::{TraceEvent, TraceReader, TraceValue};

#[test]
fn bundled_fixture_emits_a_complete_canonical_trace() {
    let directory = tempfile::tempdir().expect("temporary directory must be created");
    let trace_path = directory.path().join("trace.jsonl");
    let status = Command::new(env!("CARGO_BIN_EXE_Cemu"))
        .args(["--headless-fixture", "synthetic-boot", "--trace-output"])
        .arg(&trace_path)
        .status()
        .expect("Cemu process must start");

    assert!(status.success());
    let entries = TraceReader::new(BufReader::new(
        File::open(trace_path).expect("trace must be created"),
    ))
    .read_all()
    .expect("trace must be canonical and terminal");
    assert_eq!(entries.len(), 4);
    assert!(matches!(
        entries.last().map(|entry| &entry.event),
        Some(TraceEvent::Terminal(_))
    ));
}

#[test]
fn instruction_budget_terminates_a_loop_without_hanging() {
    let directory = tempfile::tempdir().expect("temporary directory must be created");
    let fixture_path = directory.path().join("loop.cexh");
    let trace_path = directory.path().join("loop.jsonl");
    let mut program = cex_system::builtin_fixture();
    program.code = 0x4800_0000_u32.to_be_bytes().to_vec();
    std::fs::write(
        &fixture_path,
        program.encode().expect("loop fixture must encode"),
    )
    .expect("loop fixture must be written");

    let status = Command::new(env!("CARGO_BIN_EXE_Cemu"))
        .arg("--headless-fixture")
        .arg(fixture_path)
        .args(["--instruction-budget", "3", "--trace-output"])
        .arg(&trace_path)
        .status()
        .expect("Cemu process must start");

    assert_eq!(status.code(), Some(2));
    TraceReader::new(BufReader::new(
        File::open(trace_path).expect("trace must be created"),
    ))
    .read_all()
    .expect("budget trace must be canonical and terminal");
}

#[test]
fn trace_cannot_overwrite_its_input_fixture() {
    let directory = tempfile::tempdir().expect("temporary directory must be created");
    let fixture_path = directory.path().join("fixture.cexh");
    let original = cex_system::builtin_fixture()
        .encode()
        .expect("fixture must encode");
    std::fs::write(&fixture_path, &original).expect("fixture must be written");

    let status = Command::new(env!("CARGO_BIN_EXE_Cemu"))
        .arg("--headless-fixture")
        .arg(&fixture_path)
        .arg("--trace-output")
        .arg(&fixture_path)
        .status()
        .expect("Cemu process must start");

    assert!(!status.success());
    assert_eq!(
        std::fs::read(&fixture_path).expect("fixture must remain readable"),
        original
    );
}

#[test]
fn existing_trace_destination_is_never_replaced() {
    let directory = tempfile::tempdir().expect("temporary directory must be created");
    let trace_path = directory.path().join("existing.jsonl");
    let original = b"user-data-must-survive";
    std::fs::write(&trace_path, original).expect("existing destination must be written");

    let status = Command::new(env!("CARGO_BIN_EXE_Cemu"))
        .args(["--headless-fixture", "synthetic-boot", "--trace-output"])
        .arg(&trace_path)
        .status()
        .expect("Cemu process must start");

    assert!(!status.success());
    assert_eq!(
        std::fs::read(&trace_path).expect("existing destination must remain readable"),
        original
    );
}

#[test]
fn bundled_rpx_emits_a_repeatable_path_free_trace_with_r3_42() {
    let directory = tempfile::tempdir().expect("temporary directory must be created");
    let first_path = directory.path().join("first.jsonl");
    let second_path = directory.path().join("second.jsonl");

    for trace_path in [&first_path, &second_path] {
        let status = Command::new(env!("CARGO_BIN_EXE_Cemu"))
            .args(["--headless-fixture", "synthetic-rpx-boot", "--trace-output"])
            .arg(trace_path)
            .status()
            .expect("Cemu process must start");
        assert!(status.success());
    }

    let first_bytes = std::fs::read(&first_path).expect("first trace must be readable");
    let second_bytes = std::fs::read(&second_path).expect("second trace must be readable");
    assert_eq!(first_bytes, second_bytes);
    let entries = read_trace(&first_path);
    assert_eq!(entries.len(), 4);
    let TraceEvent::Event(loaded) = &entries[0].event else {
        panic!("first RPX trace record must be the load event");
    };
    assert_eq!(loaded.name, "rpx-fixture-loaded");
    assert_eq!(
        loaded.fields.get("fixture"),
        Some(&TraceValue::Text("synthetic-rpx-v1".to_owned()))
    );
    let TraceEvent::CpuState(state) = &entries[1].event else {
        panic!("second RPX trace record must be CPU state");
    };
    assert_eq!(state.gpr[3].0, 42);
}

#[test]
fn external_rpx_trace_uses_a_stable_label_instead_of_its_host_path() {
    let directory = tempfile::tempdir().expect("temporary directory must be created");
    let fixture_path = directory
        .path()
        .join("host-secret-sentinel-external-title.rpx");
    let trace_path = directory.path().join("external.jsonl");
    std::fs::write(&fixture_path, cex_system::builtin_rpx_fixture())
        .expect("RPX fixture must be written");

    let output = Command::new(env!("CARGO_BIN_EXE_Cemu"))
        .arg("--headless-fixture")
        .arg(&fixture_path)
        .arg("--trace-output")
        .arg(&trace_path)
        .output()
        .expect("Cemu process must start");

    assert!(output.status.success());
    let trace_bytes = std::fs::read(&trace_path).expect("trace must be readable");
    assert!(!String::from_utf8_lossy(&trace_bytes).contains("host-secret-sentinel"));
    assert!(!String::from_utf8_lossy(&output.stderr).contains("host-secret-sentinel"));
    let entries = read_trace(&trace_path);
    let TraceEvent::Event(loaded) = &entries[0].event else {
        panic!("first RPX trace record must be the load event");
    };
    assert_eq!(
        loaded.fields.get("fixture"),
        Some(&TraceValue::Text("external-rpx-v1".to_owned()))
    );
}

#[test]
fn malformed_external_rpx_fails_with_a_canonical_path_free_trace() {
    let directory = tempfile::tempdir().expect("temporary directory must be created");
    let fixture_path = directory.path().join("host-secret-sentinel-invalid.rpx");
    let trace_path = directory.path().join("failure.jsonl");
    std::fs::write(&fixture_path, b"\x7fELFtruncated").expect("invalid RPX must be written");

    let output = Command::new(env!("CARGO_BIN_EXE_Cemu"))
        .arg("--headless-fixture")
        .arg(&fixture_path)
        .arg("--trace-output")
        .arg(&trace_path)
        .output()
        .expect("Cemu process must start");

    assert!(!output.status.success());
    assert!(!String::from_utf8_lossy(&output.stderr).contains("host-secret-sentinel"));
    let trace_bytes = std::fs::read(&trace_path).expect("failure trace must be readable");
    assert!(!String::from_utf8_lossy(&trace_bytes).contains("host-secret-sentinel"));
    let entries = read_trace(&trace_path);
    assert!(matches!(
        entries.as_slice(),
        [entry] if matches!(&entry.event, TraceEvent::Terminal(_))
    ));
}

fn read_trace(path: &std::path::Path) -> Vec<cex_compat::TraceEntry> {
    TraceReader::new(BufReader::new(
        File::open(path).expect("trace must be created"),
    ))
    .read_all()
    .expect("trace must be canonical and terminal")
}
