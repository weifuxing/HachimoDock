//! Inspect captured widget receipt metadata without opening a device connection.
use crate::clawpkg;
use serde::Deserialize;
use serde_json::{json, Value};
use std::{ffi::OsString, path::Path};

pub(super) const USAGE: &str =
    "petui data-link <runtime/widget.json> <diagnostics.json> [--board <boardDeviceId>] [--boot <bootId>]";

#[derive(Default)]
struct Scope<'a> {
    board: Option<&'a str>,
    boot: Option<&'a str>,
}

impl<'a> Scope<'a> {
    fn parse(mut args: &'a [OsString]) -> Result<Self, &'static str> {
        let mut scope = Self::default();
        while let [flag, value, rest @ ..] = args {
            let value = value
                .to_str()
                .filter(|s| !s.is_empty() && !s.starts_with("--"));
            match (flag.to_str(), value) {
                (Some("--board"), Some(board)) if scope.board.is_none() => {
                    scope.board = Some(board)
                }
                (Some("--boot"), Some(boot)) if scope.boot.is_none() => scope.boot = Some(boot),
                _ => return Err(USAGE),
            }
            args = rest;
        }
        if !args.is_empty() {
            return Err(USAGE);
        }
        Ok(scope)
    }

    fn matches(&self, report: &Value) -> bool {
        self.board
            .is_none_or(|id| report["boardDeviceId"].as_str() == Some(id))
            && self
                .boot
                .is_none_or(|id| report["bootId"].as_str() == Some(id))
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Counters {
    received: bool,
    fresh: bool,
    usb_count: u32,
    wifi_count: u32,
}

fn declared_source(runtime: &Value) -> Result<&str, &'static str> {
    if runtime["engine"] != "p4-bounded-runtime-v4" {
        return Err("data-link 需要 p4-bounded-runtime-v4 声明");
    }
    let source = match runtime.get("data") {
        Some(data) => data["source"].as_str(),
        None if runtime["host"] == true
            && runtime["vars"]["petui.wifi_data"]["type"] == "string"
            && runtime["vars"]["petui.wifi_data"]["init"] == "news.hn" =>
        {
            Some("news.hn")
        }
        None => None,
    };
    source
        .filter(|source| {
            (1..=47).contains(&source.len())
                && !source.starts_with("wifi.feed.")
                && source
                    .bytes()
                    .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"._-".contains(&b))
        })
        .ok_or("data-link 需要有效的数据源声明")
}

fn inspect(runtime: &Value, report: &Value, scope: Scope<'_>) -> Result<Value, &'static str> {
    let source = declared_source(runtime)?;
    let declared_route = match source {
        "wifi.feed" | "news.hn" => "device-https",
        "stocks.watchlist" => "pc-snapshot-or-device-https",
        _ => "pc-snapshot",
    };
    let mut answer = json!({
        "source": source, "declaredRoute": declared_route, "status": "unknown",
        "received": null, "fresh": null, "usbCount": null, "wifiCount": null,
    });
    if declared_route != "pc-snapshot" {
        answer["deviceHttps"] = "unknown".into();
    }
    let unknown = if !scope.matches(report) {
        "scope_mismatch"
    } else if !report.is_object() || report.get("ok").is_some_and(|ok| ok != true) {
        "invalid_report"
    } else if declared_route == "device-https" {
        "device_https_not_counted"
    } else if let Some(entry) = report["widgetData"].get(source) {
        if let Ok(counts) = Counters::deserialize(entry) {
            if counts.received && (counts.usb_count > 0 || counts.wifi_count > 0) {
                answer["status"] = "observed".into();
                answer["received"] = counts.received.into();
                answer["fresh"] = counts.fresh.into();
                answer["usbCount"] = counts.usb_count.into();
                answer["wifiCount"] = counts.wifi_count.into();
                return Ok(answer);
            }
        }
        "invalid_counters"
    } else {
        "source_unobserved"
    };
    answer["reason"] = unknown.into();
    Ok(answer)
}

pub(super) fn run(runtime: &Path, report: &Path, args: &[OsString]) -> Result<String, String> {
    let scope = Scope::parse(args)?;
    let runtime = clawpkg::read_clawpkg_source_file(runtime, clawpkg::CLAWPKG_MAX_ENTRY_BYTES)
        .map_err(|_| "无法读取受大小限制的 runtime JSON 文件")?;
    let runtime = serde_json::from_slice(&runtime).map_err(|_| "runtime JSON 格式无效")?;
    let report = clawpkg::read_clawpkg_source_file(report, 64 * 1024)
        .map_err(|_| "无法读取诊断 JSON 文件，文件须不超过 64 KiB")?;
    let bytes = report.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&report);
    let report = serde_json::from_slice(bytes).map_err(|_| "诊断 JSON 格式无效")?;
    Ok(inspect(&runtime, &report, scope)?.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn runtime(source: &str) -> Value {
        json!({"engine":"p4-bounded-runtime-v4", "data":{"source":source,"page_var":"list_page"}})
    }

    fn report() -> Value {
        json!({
            "ok": true, "boardDeviceId": "p4-001122334455", "bootId": "0123456789abcdef",
            "runtime": {"firmware": "version-marker"},
            "widgetData": {
                "computer.status": {"received":true,"fresh":true,"usbCount":0,"wifiCount":4,
                    "rows": [{"value": "private-marker"}]},
                "stocks.watchlist": {"received":true,"fresh":false,"usbCount":7,"wifiCount":1},
                "fetch.authorized": {"received":true,"fresh":true,"usbCount":1,"wifiCount":0}
            }
        })
    }

    #[test]
    fn data_link_uses_exact_source_and_outputs_only_receipt_metadata() {
        let dir = tempfile::tempdir().unwrap();
        let runtime_path = dir.path().join("widget.json");
        let report_path = dir.path().join("diagnostics.json");
        std::fs::write(&runtime_path, runtime("computer.status").to_string()).unwrap();
        std::fs::write(&report_path, format!("\u{feff}{}", report())).unwrap();
        let args = ["--boot", "0123456789abcdef", "--board", "p4-001122334455"].map(OsString::from);
        let output = run(&runtime_path, &report_path, &args).unwrap();
        assert_eq!(
            serde_json::from_str::<Value>(&output).unwrap(),
            json!({
                "source": "computer.status", "declaredRoute": "pc-snapshot", "status": "observed",
                "received": true, "fresh": true, "usbCount": 0, "wifiCount": 4,
            })
        );
        assert!(Scope::parse(&["--boot", "x", "--boot", "y"].map(OsString::from)).is_err());
        assert!(Scope::parse(&[OsString::from("--board")]).is_err());
        assert!(declared_source(&runtime("invalid source")).is_err());
    }

    #[test]
    fn data_link_missing_invalid_and_wrong_boot_are_unknown() {
        let source = runtime("computer.status");
        let mut captured = report();
        let wrong = inspect(
            &source,
            &captured,
            Scope {
                boot: Some("different"),
                ..Scope::default()
            },
        )
        .unwrap();
        assert_eq!(wrong["reason"], "scope_mismatch");
        assert!(wrong["wifiCount"].is_null());
        assert_eq!(
            inspect(
                &source,
                &json!({"widgetDataDelivery":true}),
                Scope::default()
            )
            .unwrap()["reason"],
            "source_unobserved"
        );
        captured["widgetData"]["computer.status"]["wifiCount"] = (-1).into();
        assert_eq!(
            inspect(&source, &captured, Scope::default()).unwrap()["reason"],
            "invalid_counters"
        );
        assert_eq!(
            inspect(&runtime("fetch.authorized"), &captured, Scope::default()).unwrap()["usbCount"],
            1
        );
    }

    #[test]
    fn data_link_accepts_the_copy_button_shape_without_scope() {
        let copy = json!({"widgetData": report()["widgetData"]});
        let source = runtime("stocks.watchlist");
        let observed = inspect(&source, &copy, Scope::default()).unwrap();
        assert_eq!(observed["usbCount"], 7);
        assert_eq!(observed["wifiCount"], 1);
        let scoped = inspect(
            &source,
            &copy,
            Scope {
                board: Some("p4-001122334455"),
                ..Scope::default()
            },
        )
        .unwrap();
        assert_eq!(scoped["reason"], "scope_mismatch");
    }

    #[test]
    fn data_link_keeps_device_https_independent_of_pc_receipts() {
        let mut captured = report();
        let stocks = inspect(&runtime("stocks.watchlist"), &captured, Scope::default()).unwrap();
        assert_eq!(stocks["declaredRoute"], "pc-snapshot-or-device-https");
        assert_eq!(stocks["status"], "observed");
        assert_eq!(stocks["deviceHttps"], "unknown");
        assert_eq!(stocks["fresh"], false);
        captured["widgetData"]["wifi.feed"] = captured["widgetData"]["computer.status"].clone();
        let feed = inspect(&runtime("wifi.feed"), &captured, Scope::default()).unwrap();
        assert_eq!(feed["declaredRoute"], "device-https");
        assert_eq!(feed["reason"], "device_https_not_counted");
        assert!(feed["wifiCount"].is_null());
        let news = json!({"engine":"p4-bounded-runtime-v4","host":true,
            "vars":{"petui.wifi_data":{"type":"string","init":"news.hn"}}});
        assert_eq!(
            inspect(&news, &captured, Scope::default()).unwrap()["source"],
            "news.hn"
        );
    }
}
