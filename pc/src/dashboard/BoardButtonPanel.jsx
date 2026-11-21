/**
 * [Input] Connected runtime plus persisted button actions/values, OTA state, and update/apply callbacks.
 * [Output] Runtime-aware hardware-control workspace with the device map above
 *          physical-order SVG control cards, inline button/joystick gesture editors,
 *          repeatable actions across gestures, per-gesture Code Agent prompts,
 *          multi-trigger voice-input row hints, current-key usage instructions, and USB sync feedback.
 * [Pos] component node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import React, { useState } from "react";
import { ChevronDown, UploadCloud, Mic, Usb } from "lucide-react";
import {
  BUTTON_FUNCTION_OPTIONS,
  VOICE_KEY_PRESETS,
  DEFAULT_BUTTON_ACTIONS,
  DEFAULT_VOICE_CONFIG,
  actionOptionById,
  buttonControlRowsForRuntime,
} from "../DeviceDashboard.jsx";
import Button from "../shell/Button";
import P4BoardInputMap, { ControlGlyph } from "./P4BoardInputMap.jsx";
import { decodeBluetoothShortcut } from "../lib/bluetooth-shortcut.js";
import { buildUsageHelp, friendlyControlLabel } from "../lib/usage-help.js";

const CONTROL_GROUP_META = {
  p4_sw1: { label: "SW1", detail: "左侧按键" },
  p4_sw2: { label: "SW2", detail: "中间按键" },
  p4_sw3: { label: "SW3", detail: "右侧按键" },
  p4_joystick: { label: "摇杆", detail: "四向与中按" },
  legacy_encoder: { label: "前方旋钮", detail: "旋转与按压" },
};

function defaultVoiceKeyPreset() {
  return globalThis.navigator?.platform?.startsWith("Mac") ? "mac-ctrl-alt-d" : "win-ctrl";
}

// Each callout has an (x,y) anchor in the enlarged SVG viewport where the label is drawn.
const CALLOUT_ANCHORS = {
  encoder_button_short: { x: 476, y: 58,  align: "start" },
  encoder_button:       { x: 476, y: 110, align: "start" },
  encoder_rotate:       { x: 476, y: 162, align: "start" },
};

// Callout path d-attributes — endpoint coordinates match the visual button center.
const CALLOUT_PATHS = {
  encoder_button_short: "M370 117 L468 62",
  encoder_button:       "M370 117 L468 114",
  encoder_rotate:       "M370 117 L468 166",
};

function renderCalloutLabel(row, action, anchor, isHovered) {
  return (
    <text
      key={row.id}
      className={`board-button-panel__callout-label${isHovered ? " is-hovered" : ""}`}
      x={anchor.x}
      y={anchor.y}
      textAnchor={anchor.align}
      data-button-id={row.id}
      aria-label={`${row.label}: ${action.label}`}
    >
      <tspan className="board-button-panel__callout-label-name" x={anchor.x}>
        {row.label}
      </tspan>
      <tspan className="board-button-panel__callout-label-action" x={anchor.x} dy="14">
        {action.label}
      </tspan>
    </text>
  );
}

function groupControlRows(controlRows, isP4Runtime) {
  const groups = [];
  const byId = new Map();
  controlRows.forEach((row) => {
    const groupId = isP4Runtime ? row.controlId || row.id : "legacy_encoder";
    if (!byId.has(groupId)) {
      const meta = CONTROL_GROUP_META[groupId] || {
        label: row.label,
        detail: "设备按键",
      };
      const group = { id: groupId, ...meta, rows: [] };
      groups.push(group);
      byId.set(groupId, group);
    }
    byId.get(groupId).rows.push(row);
  });
  return groups;
}

function gestureLabel(row, group) {
  return row.label
    .replace(new RegExp(`^${group.label}\\s*`), "")
    .replace(/^前方旋钮\s*/, "")
    .replace(/^摇杆\s*/, "")
    || row.label;
}

export default function BoardButtonPanel({
  selectedAgentId,
  voiceConfig,
  buttonActions = DEFAULT_BUTTON_ACTIONS,
  buttonValues = {},
  buttonLabels = {},
  runtime = "",
  voiceConfigDirty,
  voiceConfigOtaState,
  usbConnected,
  wifiConnected = false,
  wifiSyncAvailable = false,
  systemSettingsSupported = false,
  doublePressSupported = false,
  voiceKeySupported = false,
  selectedTrigger,
  onVoiceConfigChange,
  onApplyVoiceConfig,
}) {
  const [hoveredButtonId, setHoveredButton] = useState("");
  const triggerId = selectedTrigger?.id || DEFAULT_VOICE_CONFIG.trigger;
  const controlRows = buttonControlRowsForRuntime(runtime)
    .filter(row => doublePressSupported || !row.event.endsWith(".double_press"));
  const isP4Runtime = String(runtime || "").toLowerCase() === "esp-p4";
  const controlGroups = groupControlRows(controlRows, isP4Runtime);
  const help = buildUsageHelp(controlRows, buttonActions, voiceConfig?.enabled, voiceConfigDirty);
  const syncAvailable = usbConnected || wifiSyncAvailable;
  const syncPending = syncAvailable && voiceConfigOtaState?.pending;
  const confirmed = syncAvailable && !syncPending && voiceConfigOtaState?.tone === "success";
  const syncHint = wifiConnected && !wifiSyncAvailable
    ? (voiceConfigDirty ? "修改已保存在电脑上。" : "当前显示电脑保存的按键配置。") + "普通按键配置需要通过 USB 同步。"
    : voiceConfigOtaState?.message;
  const syncTone = wifiConnected && !wifiSyncAvailable ? "warning" : voiceConfigOtaState?.tone || "info";

  if (controlRows.length === 0) {
    return (
      <div className="board-button-panel__unverified" role="status">
        <Usb size={28} aria-hidden="true" />
        <strong>等待设备协议握手</strong>
        <span>{usbConnected ? "正在确认设备型号" : "尚未连接受支持的设备"}</span>
      </div>
    );
  }

  const onButtonActionChange = (row, actionId) => {
    const nextActions = { ...(buttonActions || DEFAULT_BUTTON_ACTIONS), [row.id]: actionId };
    const nextValues = { ...buttonValues };
    const nextLabels = { ...buttonLabels };
    const patch = { buttonActions: nextActions, buttonValues: nextValues, buttonLabels: nextLabels };
    if (actionId !== "miniapp_action") {
      delete nextLabels[row.id];
    }
    if (actionId === "agent_prompt" && !nextValues[row.id]) {
      nextValues[row.id] = row.defaultValue || "继续当前任务。";
    }
    if (actionId === "voice_key_forward" && !VOICE_KEY_PRESETS.some(preset => preset.id === nextValues[row.id]) && !decodeBluetoothShortcut(nextValues[row.id])) {
      nextValues[row.id] = defaultVoiceKeyPreset();
    }
    if (actionId === "voice_ptt" && row.voiceTriggerId) {
      patch.trigger = row.voiceTriggerId;
    } else if (row.voiceTriggerId && triggerId === row.voiceTriggerId) {
      const fallbackVoiceRow = controlRows.find(
        (item) => item.voiceTriggerId && nextActions[item.id] === "voice_ptt",
      );
      patch.trigger = fallbackVoiceRow?.voiceTriggerId || DEFAULT_VOICE_CONFIG.trigger;
    }
    onVoiceConfigChange(patch);
  };

  const onButtonValueChange = (row, value) => {
    onVoiceConfigChange({ buttonValues: { ...buttonValues, [row.id]: value } });
  };

  const isControlHovered = (controlId) => hoveredButtonId === controlId
    || controlRows.some((row) => row.controlId === controlId && row.id === hoveredButtonId);

  return (
    <div className="board-button-panel" data-testid="board-button-config-card">
      <div className="board-button-panel__toolbar">
        <div className="board-button-panel__sync-state">
          <h2>按钮配置</h2>
          <strong>{controlGroups.length} 个控件 <i /> {controlRows.length} 个手势</strong>
          <span className={`board-button-panel__sync-label${voiceConfigDirty ? " is-dirty" : ""}`}>
            {voiceConfigDirty ? "待同步" : confirmed ? "已同步" : "待核对"}
          </span>
        </div>
        <div className="board-button-panel__toolbar-actions">
          <Button
            variant="primary"
            size="medium"
            className={`voice-config-apply-btn${voiceConfigDirty ? " is-dirty" : ""}`}
            onClick={onApplyVoiceConfig}
            loading={syncPending}
            loadingLabel="正在同步…"
            disabled={syncPending || !syncAvailable}
            title={syncAvailable ? "将当前映射同步到设备" : "当前连接无法同步普通按键"}
          >
            <UploadCloud size={14} />
            同步到设备
          </Button>
        </div>
      </div>

      {syncHint && (
        <div
          className={`board-button-panel__hint board-button-panel__hint--${syncTone}`}
          role="status"
          aria-live="polite"
        >
          {syncHint}
        </div>
      )}

      <div className="usage-help">
        <p>为按键和摇杆选择功能，修改后点击「同步到设备」生效。同一个按键可以分别设置短按和长按。{doublePressSupported && "设置双击后，该键的单击会等待约 280 毫秒。语音长按和组件退出优先。"}</p>
        <details open>
          <summary>常用操作 · 按当前配置显示{voiceConfigDirty ? "（待同步）" : ""}</summary>
          <dl className="usage-help__operations">
            <dt>语音输入</dt><dd>{help.voice} {help.confirm}</dd>
            <dt>宠物／组件切换</dt><dd>{help.component}</dd>
            <dt>选择并打开组件</dt><dd>{help.select}；{help.enter}</dd>
            <dt>退出组件</dt><dd>{help.back} 组件内的其他操作以「怎么玩」为准。</dd>
            <dt>实时对话</dt><dd>{help.chat} 等它说完再说。</dd>
            <dt>查看会话</dt><dd>{help.sessions}</dd>
          </dl>
          <p>使用语音功能时，请保持设备连接电脑，并让 Pet Manager 持续运行。</p>
        </details>
      </div>

      <div className={`board-button-panel__workspace${isP4Runtime ? " board-button-panel__workspace--p4" : ""}`}>
        <section className="board-button-panel__left" aria-label="设备控件导航">
          <div className="board-button-panel__device-stage">
            {isP4Runtime ? (
              <P4BoardInputMap
                gestureCount={controlRows.length}
                highlightedControl={controlGroups.find(group => isControlHovered(group.id))?.id}
                activeControl={buttonActions.p4_sw1_long === "voice_ptt" && voiceConfig.enabled ? "p4_sw1" : ""}
                onHover={setHoveredButton}
              />
            ) : (
              <svg
                className="board-button-map__device board-button-panel__svg"
                viewBox="0 0 560 320"
                role="img"
                aria-label="板端外观和按钮位置示意"
              >
                  <rect className="board-button-map__body" x="72" y="58" width="374" height="200" rx="32" />
                  <rect className="board-button-map__screen-bezel" x="100" y="80" width="198" height="130" rx="18" />
                  <rect className="board-button-map__screen" x="119" y="95" width="160" height="100" rx="8" />
                  <circle
                    className={`board-button-map__encoder${buttonActions.encoder_button === "voice_ptt" && voiceConfig.enabled ? " is-active" : ""}`}
                    cx="370" cy="117" r="48"
                    onMouseEnter={() => setHoveredButton("encoder_button")}
                    onMouseLeave={() => setHoveredButton("")}
                    data-button-id="encoder_button"
                  />
                {controlRows.map((row) => [row.id, CALLOUT_PATHS[row.id]]).filter(([, d]) => d).map(([id, d]) => (
                  <path key={id} className={`board-button-map__callout${hoveredButtonId === id ? " is-hovered" : ""}`} d={d} />
                ))}
                {controlRows.map((row) => {
                  const anchor = CALLOUT_ANCHORS[row.id];
                  if (!anchor) return null;
                  const action = actionOptionById(buttonActions[row.id] || row.defaultAction);
                  return renderCalloutLabel(row, action, anchor, hoveredButtonId === row.id);
                })}
              </svg>
            )}
          </div>
        </section>

        <section className="board-button-panel__right" aria-label="实体控件与手势配置">
          <div className="board-button-control-groups">
            {controlGroups.map((group, groupIndex) => (
              <fieldset
                className={`board-button-control-group board-button-control-group--${group.rows.length}`}
                key={group.id}
                onMouseEnter={() => setHoveredButton(group.id)}
                onMouseLeave={() => setHoveredButton("")}
              >
                <legend>
                  <span>{String(groupIndex + 1).padStart(2, "0")}</span>
                  <small>{group.label} · {group.detail}</small>
                </legend>
                <div className="board-button-control-group__visual">
                  <ControlGlyph
                    controlId={group.id}
                    label={group.label}
                    active={group.rows.some(
                      (row) => buttonActions?.[row.id] === "voice_ptt" && voiceConfig.enabled,
                    )}
                  />
                  <span>
                    <strong>{friendlyControlLabel(group.label)}</strong>
                    <small>
                      {group.id === "p4_joystick"
                        ? "上、下、左、右与中按分别配置"
                        : group.rows.some(row => row.event.endsWith(".double_press"))
                          ? "短按、双击、长按分别配置" : "短按、长按分别配置"}
                    </small>
                  </span>
                </div>
                <div className={`board-button-control-group__rows board-button-control-group__rows--${group.rows.length}`}>
                  {group.rows.map((row) => {
                    const currentActionId = buttonActions?.[row.id] || row.defaultAction;
                    const isShortPress = isP4Runtime && row.event.endsWith(".short_press");
                    const allowedOptions = BUTTON_FUNCTION_OPTIONS.filter((option) => {
                      if (option.id === "system_settings" && !systemSettingsSupported
                          && currentActionId !== option.id) return false;
                      if (option.id === "voice_key_forward" && !voiceKeySupported && currentActionId !== option.id) return false;
                      if (option.id === "dsh_interrupt" && selectedAgentId !== "deepseek-harness") return false;
                      return row.actionOptions.includes(option.id)
                        || (isShortPress && option.id === "voice_ptt");
                    });
                    const isVoicePttRow = currentActionId === "voice_ptt";
                    const rowHovered = hoveredButtonId === row.id || (row.controlId && hoveredButtonId === row.controlId);
                    const fieldId = `button-action-${row.id}`;
                    const promptId = `button-prompt-${row.id}`;
                    return (
                      <div
                        className={`board-button-control-field${rowHovered ? " is-hovered" : ""}${currentActionId === "agent_prompt" ? " has-prompt" : ""}`}
                        key={row.id}
                        data-event={row.event}
                        onMouseEnter={() => setHoveredButton(row.id)}
                        onMouseLeave={() => setHoveredButton(group.id)}
                      >
                        <span className="board-button-control-field__head">
                          <label htmlFor={fieldId}>{gestureLabel(row, group)}</label>
                          {isVoicePttRow && (
                            <span className={`board-button-panel__voice-chip${voiceConfig.enabled ? " is-on" : ""}`}>
                              <Mic size={11} />
                              {voiceConfig.enabled ? "语音输入已开启" : "语音输入未开启"}
                            </span>
                          )}
                        </span>
                        {allowedOptions.length <= 1 ? (
                          <span className="voice-button-action-fixed" id={fieldId}>
                            {allowedOptions[0]?.label || actionOptionById(row.defaultAction).label}
                          </span>
                        ) : (
                          <span className="voice-button-action-select-shell">
                            <select
                              id={fieldId}
                              className="voice-button-action-select"
                              value={currentActionId}
                              aria-label={`${friendlyControlLabel(group.label)}${gestureLabel(row, group)}功能`}
                              onChange={(event) => onButtonActionChange(row, event.target.value)}
                            >
                              {allowedOptions.map((option) => {
                                const requiresLongPress = option.id === "voice_ptt" && isShortPress;
                                const disabledReason = requiresLongPress
                                  ? "需要长按"
                                  : "";
                                return (
                                  <option
                                    key={option.id}
                                    value={option.id}
                                    disabled={Boolean(disabledReason)}
                                    title={disabledReason || undefined}
                                  >
                                    {option.label}{disabledReason ? `（${disabledReason}）` : ""}
                                  </option>
                                );
                              })}
                            </select>
                            <ChevronDown
                              className="voice-button-action-select-shell__chevron"
                              size={14}
                              aria-hidden="true"
                            />
                          </span>
                        )}
                        {row.id === "p4_sw3_long" && currentActionId !== row.defaultAction && (
                          <div className="board-button-control-field__prompt">
                            <small>默认动作：打开系统设置</small>
                            <Button variant="ghost" size="small"
                              disabled={!systemSettingsSupported || syncPending}
                              title={systemSettingsSupported
                                ? "只修改 SW3 长按，同步到设备后生效"
                                : "需要设备固件支持系统设置"}
                              onClick={() => onButtonActionChange(row, row.defaultAction)}>
                              恢复默认：打开系统设置
                            </Button>
                          </div>
                        )}
                        {currentActionId === "voice_key_forward" && (
                          <label className="board-button-control-field__prompt">
                            <span>电脑语音快捷键</span>
                            <select value={(VOICE_KEY_PRESETS.some(preset => preset.id === buttonValues[row.id]) || decodeBluetoothShortcut(buttonValues[row.id])) ? buttonValues[row.id] : defaultVoiceKeyPreset()}
                              onChange={event => onButtonValueChange(row, event.target.value)}>
                              {decodeBluetoothShortcut(buttonValues[row.id]) && <option value={buttonValues[row.id]}>旧版自定义组合键</option>}
                              {VOICE_KEY_PRESETS.map(preset => <option key={preset.id} value={preset.id}>{preset.label}</option>)}
                            </select>
                          </label>
                        )}
                        {currentActionId === "agent_prompt" && (
                          <label className="board-button-control-field__prompt" htmlFor={promptId}>
                            <span>
                              <strong>自定义指令</strong>
                              <small>按键后直接发送给 Code Agent</small>
                            </span>
                            <textarea
                              id={promptId}
                              className="voice-button-action-value"
                              value={buttonValues[row.id] || row.defaultValue || ""}
                              onChange={(event) => onButtonValueChange(row, event.target.value)}
                              maxLength={120}
                              rows={3}
                              placeholder="例如：总结当前进度，检查报错并继续实现。"
                              aria-label={`${row.label} 发送给 Code Agent 的自定义指令`}
                            />
                            <small className="board-button-control-field__prompt-count">
                              {String(buttonValues[row.id] || row.defaultValue || "").length}/120
                            </small>
                          </label>
                        )}
                      </div>
                    );
                  })}
                </div>
              </fieldset>
            ))}
          </div>
        </section>
      </div>
    </div>
  );
}
