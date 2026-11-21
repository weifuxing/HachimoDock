//! Physical input mappings and replay for the P4 WASM input ABI.
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

const GAME_ACTIONS: &[&str] = &[
    "lab.left",
    "lab.right",
    "lab.up",
    "lab.down",
    "lab.a",
    "lab.b",
];
const RADIO_ACTIONS: &[&str] = &[
    "media.previous",
    "media.next",
    "media.up",
    "media.down",
    "media.toggle",
    "media.queue",
    "media.select",
];

pub(crate) fn bindings(id: &str, buttons: &Value) -> Result<BTreeMap<&'static str, u8>, String> {
    let actions = if id == "city-radio" {
        RADIO_ACTIONS
    } else {
        GAME_ACTIONS
    };
    let buttons = buttons.as_array().ok_or("WASM buttons.json 必须是数组")?;
    let mut result = BTreeMap::new();
    for (index, button) in buttons.iter().enumerate() {
        let action = button["action"].as_str().unwrap_or_default();
        let event = button["event"].as_str().unwrap_or_default();
        let code = actions.iter().position(|a| *a == action).ok_or_else(|| {
            format!(
                "buttons.json 第 {} 项（{}）的动作 {} 无法进入 WASM，支持 {}",
                index + 1,
                event,
                action,
                actions.join("、")
            )
        })? as u8
            + 1;
        if !crate::clawpkg::COMPONENT_BUTTON_EVENTS.contains(&event) {
            return Err(format!("WASM 动作 {action} 的设备事件 {event} 无效"));
        }
        for part in event.split(" / ") {
            let event = crate::clawpkg::COMPONENT_BUTTON_EVENTS
                .iter()
                .copied()
                .find(|allowed| {
                    *allowed == part
                        && !allowed.contains(" / ")
                        && !allowed.starts_with("screen.region.")
                })
                .ok_or_else(|| format!("WASM 动作 {action} 的设备事件 {part} 无效"))?;
            if result.insert(event, code).is_some() {
                return Err(format!("WASM 设备事件 {event} 被多次绑定"));
            }
        }
    }
    Ok(result)
}

/// Translate the physical gesture through the same action names the board sees.
/// Legacy numeric replays remain available, but only mapped codes count as
/// verified physical input. Touch coordinates remain native canvas pixels.
pub(crate) fn resolve_trace(
    source: &str,
    bindings: &BTreeMap<&str, u8>,
    require_game: bool,
    require_touch: bool,
) -> Result<String, String> {
    let mut output = String::new();
    let mut covered = BTreeSet::new();
    let mut saw_touch = false;
    let mut proved_touch = false;
    let mut observed = false;
    let mut drew_after_input = false;
    // Zero represents touch. Physical WASM key codes start at one.
    let mut pending_input = None;
    for (index, line) in source.lines().enumerate() {
        let line = line.trim();
        let translated;
        let line = if let Some(event) = line.strip_prefix("input ") {
            let code = bindings.get(event).ok_or_else(|| {
                format!("game.trace 第 {} 行的设备事件 {event} 没有组件绑定", index + 1)
            })?;
            translated = format!("event 4 {code} 0");
            &translated
        } else {
            line
        };
        let fields: Vec<_> = line.split_ascii_whitespace().collect();
        match fields.as_slice() {
            ["event", "4", code, _] => {
                let code = code.parse::<u8>()
                    .map_err(|_| format!("game.trace 第 {} 行键码无效", index + 1))?;
                if !bindings.values().any(|value| *value == code) && require_game {
                    return Err(format!("game.trace 第 {} 行的键码 {code} 未绑定到实体输入", index + 1));
                }
                pending_input = observed.then_some(code);
                drew_after_input = false;
            }
            // Real button gestures emit only event 4.
            ["event", "5", _, _] => continue,
            ["observe"] => {
                observed = true;
                pending_input = None;
            }
            ["event", "1", _, _] => {
                saw_touch = true;
                pending_input = observed.then_some(0);
                drew_after_input = false;
            }
            ["event", "6", _, _] if pending_input == Some(0) => pending_input = None,
            ["frame", _] if pending_input.is_some() => drew_after_input = true,
            ["appeared" | "pixel-changed" | "slot-changed", ..] if drew_after_input => {
                match pending_input {
                    Some(0) => proved_touch = true,
                    Some(code) => { covered.insert(code); }
                    None => {}
                }
            }
            _ => {}
        }
        output.push_str(line);
        output.push('\n');
    }
    if require_game && bindings.is_empty() && (!saw_touch || !proved_touch) {
        return Err("game.trace 需要验证至少一次实体输入或触控响应".into());
    }
    if require_game {
        let missing: Vec<_> = bindings
            .iter()
            .filter_map(|(event, code)| (!covered.contains(code)).then_some(*event))
            .collect();
        if !missing.is_empty() {
            return Err(format!(
                "game.trace 未验证这些实体输入的结果：{}。输入前用 observe，输入后渲染一帧，再用 appeared、pixel-changed 或 slot-changed",
                missing.join("、")
            ));
        }
    }
    if require_touch && !proved_touch {
        return Err("组件声明支持触屏，game.trace 需要使用 observe、触摸事件、frame 和 appeared、pixel-changed 或 slot-changed 验证具体结果".into());
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn host_codes_match_firmware_dispatch_order() {
        let firmware = include_str!("../../../firmware/main/pet_p4_wasm_trial.c");
        for (symbol, expected) in [("local[]", GAME_ACTIONS), ("remote[]", RADIO_ACTIONS)] {
            let list = firmware.split_once(symbol).unwrap().1.split_once("};").unwrap().0;
            let actual: Vec<_> = list.split('"').skip(1).step_by(2).collect();
            assert_eq!(actual, expected, "{symbol}");
        }
    }

    #[test]
    fn puzzle_button_aliases_are_rejected_before_they_can_reach_the_board() {
        for (event, action) in [
            ("button.sw1.short_press", "start"),
            ("button.sw2.short_press", "undo"),
            ("knob.rotate_ccw", "move_left"),
            ("joystick.up", "move_up"),
        ] {
            assert!(bindings("pinlegetu", &json!([{"event":event,"action":action}])).is_err());
        }
        let buttons = json!([{"event":"button.sw1.short_press","action":"lab.b"}]);
        let map = bindings("pinlegetu", &buttons).unwrap();
        assert_eq!(
            resolve_trace("frame 0\nobserve\ninput button.sw1.short_press\nframe 0\nappeared Moved\n", &map, true, false).unwrap(),
            "frame 0\nobserve\nevent 4 6 0\nframe 0\nappeared Moved\n"
        );
        assert!(resolve_trace("event 4 5 0\n", &map, true, false).is_err());
        assert_eq!(
            resolve_trace("event 4 6 0\nevent 5 6 0\n", &map, false, false).unwrap(),
            "event 4 6 0\n"
        );
        assert!(resolve_trace("frame 0\nobserve\nevent 1 60 90\nframe 33\nchanged\n", &BTreeMap::new(), true, true).is_err());
        assert!(resolve_trace("input joystick.up\n", &map, true, false).is_err());
        assert!(resolve_trace("event 1 60 90\nframe 33\n", &map, true, true).is_err());
        assert!(resolve_trace(
            "frame 0\nobserve\nevent 1 60 90\nframe 33\nappeared Started\nobserve\ninput button.sw1.short_press\nframe 0\nslot-changed 0 1\n",
            &map,
            true,
            true
        )
        .is_ok());
    }
}
