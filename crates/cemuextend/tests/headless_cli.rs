//! Integration tests for the headless CemuExtend command-line interface.

use std::fs::File;
use std::io::BufReader;
use std::process::Command;

use cex_compat::{TraceEvent, TraceReader};

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
