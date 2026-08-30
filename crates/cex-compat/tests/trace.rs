//! Integration coverage for canonical traces, redaction, ordering, and limits.

use std::{collections::BTreeMap, io::Cursor};

use cex_compat::{
    CompatError, CpuStateEvent, GenericEvent, HexU32, HexU64, MAX_CANONICAL_LINE_BYTES, StopReason,
    TerminalEvent, TraceCategory, TraceEntry, TraceEvent, TraceMismatchKind, TraceReader,
    TraceSource, TraceValue, TraceWriter, diff_traces,
};

fn terminal(cycle: u64) -> TraceEntry {
    TraceEntry::new(
        cycle,
        0,
        TraceSource::TestHarness,
        None,
        TraceCategory::Terminal,
        TraceEvent::Terminal(TerminalEvent {
            reason: StopReason::TestCompleted,
            guest_exit_code: Some(HexU32(0)),
            detail_code: Some("synthetic_complete".to_owned()),
        }),
    )
}

fn cpu(cycle: u64) -> TraceEntry {
    TraceEntry::new(
        cycle,
        0,
        TraceSource::Cpu,
        Some(0),
        TraceCategory::Cpu,
        TraceEvent::CpuState(Box::new(CpuStateEvent {
            pc: HexU32(0x1000_0100),
            gpr: [HexU32(0); 32],
            cr: HexU32(0),
            lr: HexU32(0x1000_0200),
            ctr: HexU32(0),
            xer: HexU32(0),
            fpr_bits: [[HexU64(0); 2]; 32],
            fpscr: HexU32(0),
            ugqr: [HexU32(0); 8],
            reservation: None,
            pending_fault: None,
            instructions_retired: 0_u64.into(),
        })),
    )
}

#[test]
fn writer_is_canonical_redacted_and_round_trips() {
    let mut fields = BTreeMap::new();
    fields.insert("z_count".to_owned(), TraceValue::Unsigned(7_u64.into()));
    fields.insert(
        "PretendoPassword".to_owned(),
        TraceValue::Text("do-not-write-this".to_owned()),
    );
    fields.insert("a_ready".to_owned(), TraceValue::Boolean(true));
    let event = TraceEntry::new(
        12,
        0,
        TraceSource::Online,
        Some(1),
        TraceCategory::Online,
        TraceEvent::Event(GenericEvent {
            name: "nex_login_complete".to_owned(),
            fields,
        }),
    );

    let mut writer = TraceWriter::new(Vec::new());
    writer.write_entry(&event).unwrap();
    writer.write_entry(&terminal(13)).unwrap();
    let bytes = writer.finish().unwrap();
    let text = std::str::from_utf8(&bytes).unwrap();

    assert!(!text.contains("do-not-write-this"));
    assert!(text.contains("\"PretendoPassword\":{\"type\":\"redacted\"}"));
    assert!(text.find("a_ready").unwrap() < text.find("z_count").unwrap());
    assert!(text.lines().all(|line| !line.contains(' ')));

    let entries = TraceReader::new(Cursor::new(bytes)).read_all().unwrap();
    assert_eq!(entries.len(), 2);
    let TraceEvent::Event(event) = &entries[0].event else {
        panic!("expected event")
    };
    assert_eq!(event.fields["PretendoPassword"], TraceValue::Redacted);
}

#[test]
fn raw_bits_and_large_cycles_use_fixed_portable_strings() {
    let mut entry = cpu(u64::MAX);
    let TraceEvent::CpuState(state) = &mut entry.event else {
        unreachable!()
    };
    state.gpr[0] = HexU32(u32::MAX);
    state.fpr_bits[0][1] = HexU64(0x7ff8_0000_0000_0001);

    let line = cex_compat::canonical_json_line(&entry).unwrap();
    let text = std::str::from_utf8(&line).unwrap();
    assert!(text.contains("\"guest_cycle\":\"18446744073709551615\""));
    assert!(text.contains("\"0xffffffff\""));
    assert!(text.contains("\"0x7ff8000000000001\""));
}

#[test]
fn reader_rejects_noncanonical_and_unredacted_secret_values() {
    let mut fields = BTreeMap::new();
    fields.insert(
        "message".to_owned(),
        TraceValue::Text("safe-value".to_owned()),
    );
    let entry = TraceEntry::new(
        1,
        0,
        TraceSource::Online,
        None,
        TraceCategory::Online,
        TraceEvent::Event(GenericEvent {
            name: "request".to_owned(),
            fields,
        }),
    );
    let mut bytes = cex_compat::canonical_json_line(&entry).unwrap();
    let text = String::from_utf8(bytes).unwrap();
    bytes = text.replace("safe-value", "Bearer credential").into_bytes();
    bytes.extend_from_slice(&cex_compat::canonical_json_line(&terminal(2)).unwrap());

    let error = TraceReader::new(Cursor::new(bytes)).read_all().unwrap_err();
    assert!(matches!(error, CompatError::InvalidTrace(_)));
}

#[test]
fn writer_rejects_url_fields_and_host_paths() {
    for (field, value) in [
        (
            "serviceUrl",
            TraceValue::Text("https://example.test".to_owned()),
        ),
        ("message", TraceValue::Text("/home/user/private".to_owned())),
    ] {
        let entry = TraceEntry::new(
            1,
            0,
            TraceSource::Online,
            None,
            TraceCategory::Online,
            TraceEvent::Event(GenericEvent {
                name: "request".to_owned(),
                fields: BTreeMap::from([(field.to_owned(), value)]),
            }),
        );
        let error = TraceWriter::new(Vec::new())
            .write_entry(&entry)
            .unwrap_err();
        assert!(matches!(error, CompatError::InvalidTrace(_)));
    }
}

#[test]
fn reader_rejects_an_overlong_line_before_parsing_it() {
    let bytes = vec![b'a'; MAX_CANONICAL_LINE_BYTES + 1];
    let error = TraceReader::new(Cursor::new(bytes)).read_all().unwrap_err();
    assert!(matches!(error, CompatError::TraceLineTooLong { .. }));
}

#[test]
fn ordering_and_terminal_are_enforced() {
    let mut writer = TraceWriter::new(Vec::new());
    writer.write_entry(&cpu(10)).unwrap();
    let error = writer.write_entry(&cpu(9)).unwrap_err();
    assert!(matches!(error, CompatError::OutOfOrder { .. }));

    let mut writer = TraceWriter::new(Vec::new());
    writer.write_entry(&cpu(10)).unwrap();
    assert!(matches!(writer.finish(), Err(CompatError::MissingTerminal)));
}

#[test]
fn diff_reports_first_register_path_and_redacts_values() {
    let expected = vec![cpu(10), terminal(11)];
    let mut actual = expected.clone();
    let TraceEvent::CpuState(state) = &mut actual[0].event else {
        unreachable!()
    };
    state.gpr[4] = HexU32(0xfeed_face);

    let diff = diff_traces(&expected, &actual);
    let mismatch = diff.mismatch.expect("trace should differ");
    assert_eq!(mismatch.kind, TraceMismatchKind::DivergentValue);
    assert_eq!(mismatch.path, "$.event.gpr[4]");
    assert!(mismatch.to_string().contains("general-purpose register"));
}

#[test]
fn reader_rejects_schema_mismatch() {
    let mut entry = terminal(1);
    entry.schema_version += 1;
    let mut bytes = serde_json::to_vec(&entry).unwrap();
    bytes.push(b'\n');
    let error = TraceReader::new(Cursor::new(bytes)).read_all().unwrap_err();
    assert!(matches!(error, CompatError::UnsupportedTraceSchema { .. }));
}
