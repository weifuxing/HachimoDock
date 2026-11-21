use serde_json::Value;

pub(super) const CAPABILITY: &str = "widgetWifiFeedMap";
pub(super) const VERSION: &str = "p4-wifi-feed-map-v1";

pub(super) fn valid(value: &Value) -> bool {
    let Some(map) = value.as_object() else { return false };
    ["rows", "id", "label"].iter().all(|key| map.contains_key(*key))
        && map.iter().all(|(key, value)| {
            ["rows", "id", "label", "value", "detail", "meta", "tone"].contains(&key.as_str())
                && value.as_str().is_some_and(|path| {
                    (key == "rows" && path == "$")
                        || (path.len() <= 63 && path.split('.').count() <= 4
                            && path.split('.').all(|segment| {
                                !segment.is_empty()
                                    && segment.bytes().all(|b| b.is_ascii_alphanumeric() || b"_-".contains(&b))
                            }))
                })
        })
}

#[cfg(test)]
mod tests {
    use super::valid;
    use serde_json::json;

    #[test]
    fn bounded_mapping_paths() {
        let mut map = json!({"rows": "$", "id": "record.id", "label": "Name", "tone": "signal"});
        assert!(valid(&map));
        for invalid in [json!(null), json!(""), json!("a..b"), json!("a.b.c.d.e"),
                        json!("a[0]"), json!("a\n"), json!("a".repeat(64)), json!("$")] {
            map["id"] = invalid;
            assert!(!valid(&map));
        }
        map["id"] = json!("a".repeat(63));
        assert!(valid(&map));
        map["ttlMs"] = json!("ttl");
        assert!(!valid(&map));
        assert!(!valid(&json!({"rows": "data.items", "id": "id"})));
    }
}
