//! Golden tests for the stable JSON RPC envelope shapes.

use std::collections::BTreeSet;

use serde_json::{Value, json};

fn fixture(name: &str) -> Value {
    let source = match name {
        "request" => include_str!("fixtures/request.json"),
        "success" => include_str!("fixtures/success.json"),
        "error" => include_str!("fixtures/error.json"),
        "event" => include_str!("fixtures/event.json"),
        _ => panic!("unknown fixture {name}"),
    };
    serde_json::from_str(source).expect("wire fixture must contain valid JSON")
}

fn keys(value: &Value) -> BTreeSet<&str> {
    value
        .as_object()
        .expect("wire envelope must be an object")
        .keys()
        .map(String::as_str)
        .collect()
}

#[test]
fn request_golden_pins_required_object_params() {
    let request = fixture("request");
    assert_eq!(keys(&request), BTreeSet::from(["id", "method", "params"]));
    assert_eq!(request["id"], "request-1");
    assert_eq!(request["method"], "system.bootstrap");
    assert!(request["params"].is_object());
}

#[test]
fn response_goldens_pin_discriminated_shapes() {
    let success = fixture("success");
    assert_eq!(keys(&success), BTreeSet::from(["id", "ok", "result"]));
    assert_eq!(
        success,
        json!({"id": "request-1", "ok": true, "result": {}})
    );

    let error = fixture("error");
    assert_eq!(keys(&error), BTreeSet::from(["error", "id", "ok"]));
    assert_eq!(keys(&error["error"]), BTreeSet::from(["code", "message"]));
    assert_eq!(error["ok"], false);

    let mut with_details = error;
    with_details["error"]
        .as_object_mut()
        .expect("error must be an object")
        .insert("details".to_owned(), Value::Null);
    assert_eq!(
        keys(&with_details["error"]),
        BTreeSet::from(["code", "details", "message"])
    );
    assert!(with_details["error"]["details"].is_null());
}

#[test]
fn event_golden_pins_decimal_sequence_and_payload() {
    let event = fixture("event");
    assert_eq!(
        keys(&event),
        BTreeSet::from(["payload", "sequence", "type"])
    );
    assert_eq!(event["type"], "titles.changed");
    assert_eq!(event["sequence"], "1");
    assert!(event["payload"].is_object());
}
