//! Active WASM components can read one bounded, city-wide UV snapshot.
use serde_json::{json, Value};
use std::{
    io::Read,
    sync::{Mutex, OnceLock},
    time::{Duration, Instant},
};

const ACTIVE: Duration = Duration::from_secs(8);
const REFRESH: Duration = Duration::from_secs(15 * 60);
const RETRY: Duration = Duration::from_secs(2 * 60);
const MAX_AGE: i64 = 3600;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct City(i32, i32);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Sample {
    timestamp: i64,
    index_centi: u32,
}

enum Provider {
    Public,
    Customer(String),
}

impl Provider {
    fn configured() -> Option<Self> {
        if let Ok(key) = std::env::var("HACHIMO_UV_OPEN_METEO_KEY") {
            let key = key.trim();
            if !key.is_empty() {
                return Some(Self::Customer(key.to_owned()));
            }
        }
        (std::env::var("HACHIMO_UV_OPEN_METEO_PUBLIC").as_deref() == Ok("1"))
            .then_some(Self::Public)
    }
}

#[derive(Default)]
struct Cache {
    request: Option<(u64, Instant)>,
    city: Option<City>,
    sample: Option<Sample>,
    due: Option<Instant>,
    busy: bool,
}

fn cache() -> &'static Mutex<Cache> {
    static CACHE: OnceLock<Mutex<Cache>> = OnceLock::new();
    CACHE.get_or_init(Default::default)
}

pub(super) fn requested(connection: u64) {
    if let Ok(mut cache) = cache().lock() {
        cache.request = Some((connection, Instant::now()));
    }
}

fn parse(value: &Value, now: i64) -> Option<Sample> {
    let timestamp = value["current"]["time"].as_i64()?;
    let uv = value["current"]["uv_index"].as_f64()?;
    if !(0.0..=25.0).contains(&uv)
        || !(now.saturating_sub(MAX_AGE)..=now.saturating_add(60)).contains(&timestamp)
    {
        return None;
    }
    Some(Sample {
        timestamp,
        index_centi: (uv * 100.0).round() as u32,
    })
}

fn fetch(city: City, provider: Provider) -> Option<Sample> {
    let (host, key) = match &provider {
        Provider::Public => ("api.open-meteo.com", None),
        Provider::Customer(key) => ("customer-api.open-meteo.com", Some(key)),
    };
    let addresses = crate::media_dns::resolve(host, 443).ok()?;
    let client = reqwest::blocking::Client::builder()
        .no_proxy()
        .resolve_to_addrs(host, &addresses)
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(Duration::from_secs(5))
        .timeout(Duration::from_secs(10))
        .build()
        .ok()?;
    let latitude = format!("{:.2}", f64::from(city.0) / 100.0);
    let longitude = format!("{:.2}", f64::from(city.1) / 100.0);
    let mut request = client.get(format!("https://{host}/v1/forecast")).query(&[
        ("latitude", latitude.as_str()),
        ("longitude", longitude.as_str()),
        ("current", "uv_index"),
        ("timeformat", "unixtime"),
        ("forecast_days", "1"),
    ]);
    if let Some(key) = key {
        request = request.query(&[("apikey", key)]);
    }
    let response = request.send().ok()?;
    if !response.status().is_success() {
        return None;
    }
    let mut bytes = Vec::new();
    response.take(8193).read_to_end(&mut bytes).ok()?;
    if bytes.len() > 8192 {
        return None;
    }
    parse(
        &serde_json::from_slice(&bytes).ok()?,
        chrono::Utc::now().timestamp(),
    )
}

pub(super) fn heartbeat(connection: u64, epoch: i64) -> Value {
    let now = Instant::now();
    let Ok(mut state) = cache().lock() else {
        return Value::Null;
    };
    if !state
        .request
        .is_some_and(|(owner, at)| owner == connection && now.duration_since(at) <= ACTIVE)
    {
        return Value::Null;
    }
    let Some(provider) = Provider::configured() else {
        return Value::Null;
    };
    let city = crate::shared_location::weather()
        .filter(|(location, _, _)| location.valid())
        .map(|(location, _, _)| {
            City(
                (location.lat * 100.0).round() as i32,
                (location.lon * 100.0).round() as i32,
            )
        });
    if city != state.city {
        state.city = city;
        state.sample = None;
        state.due = None;
    }
    let Some(city) = city else { return Value::Null };
    if !state.busy && state.due.is_none_or(|due| now >= due) {
        state.busy = true;
        let worker = std::thread::Builder::new()
            .name("wasm-uv".into())
            .spawn(move || {
                let sample = fetch(city, provider);
                if let Ok(mut state) = cache().lock() {
                    state.busy = false;
                    if state.city == Some(city) {
                        if sample.is_some() {
                            state.sample = sample;
                        }
                        state.due =
                            Some(Instant::now() + if sample.is_some() { REFRESH } else { RETRY });
                    }
                }
            });
        if worker.is_err() {
            state.busy = false;
            state.due = Some(now + RETRY);
        }
    }
    state
        .sample
        .filter(|sample| {
            (epoch.saturating_sub(MAX_AGE)..=epoch.saturating_add(60)).contains(&sample.timestamp)
        })
        .map_or(
            Value::Null,
            |sample| json!({"sampleSeconds": sample.timestamp, "indexCenti": sample.index_centi}),
        )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_zero_and_rejects_expired_future_and_invalid_samples() {
        let now = 1_800_000_000;
        let payload = |time, uv| json!({"current": {"time": time, "uv_index": uv}});
        assert_eq!(
            parse(&payload(now, 0.0), now),
            Some(Sample {
                timestamp: now,
                index_centi: 0
            })
        );
        assert_eq!(
            parse(&payload(now - 900, 4.567), now).unwrap().index_centi,
            457
        );
        assert!(parse(&payload(now - 3601, 3.0), now).is_none());
        assert!(parse(&payload(now + 61, 3.0), now).is_none());
        assert!(parse(&payload(now, -1.0), now).is_none());
        assert!(parse(&json!({"current": {"time": now}}), now).is_none());
    }
}
