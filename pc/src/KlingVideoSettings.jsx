import React from "react";
import { KLING_MODELS, klingModel, klingVideoParameters } from "./lib/avatar-pipeline/kling-models.js";

export default function KlingVideoSettings({ model, onModel, value, onChange }) {
  const selected = klingModel(model) || KLING_MODELS[0];
  const settings = klingVideoParameters(selected, value);
  const audio26 = selected.id === "kling-2.6" && settings.generateAudio;
  const update = patch => onChange(klingVideoParameters(selected, { ...settings, ...patch }));

  return (
    <>
      <label className="ui-field">
        <span className="ui-field__label">视频生成模型</span>
        <select className="ui-control" value={selected.id} onChange={event => {
          onModel(event.target.value);
          onChange(klingVideoParameters(klingModel(event.target.value), settings));
        }}>
          {KLING_MODELS.map(item => <option key={item.id} value={item.id}>{item.label}</option>)}
        </select>
      </label>
      <label className="ui-field">
        <span className="ui-field__label">视频时长</span>
        <select className="ui-control" value={settings.duration} onChange={event => update({ duration: Number(event.target.value) })}>
          {selected.durations.map(seconds => <option key={seconds} value={seconds}>{seconds} 秒</option>)}
        </select>
      </label>
      <label className="ui-field">
        <span className="ui-field__label">视频分辨率</span>
        <select className="ui-control" value={settings.resolution} disabled={audio26} onChange={event => update({ resolution: event.target.value })}>
          {selected.resolutions.map(resolution => <option key={resolution} value={resolution}>{resolution === "4k" ? "4K" : resolution}</option>)}
        </select>
      </label>
      {selected.audio && <label className="row">
        <input type="checkbox" checked={settings.generateAudio} onChange={event => update({ generateAudio: event.target.checked })} /> 生成原生音频
      </label>}
      {selected.endFrame && <label className="row">
        <input type="checkbox" checked={settings.endFrame} disabled={audio26} onChange={event => update({ endFrame: event.target.checked })} /> 同一张参考图作为首尾帧
      </label>}
      {audio26 && <small className="muted">Kling 2.6 开启原生音频时使用 1080p，尾帧选项不可用。</small>}
      <label className="row">
        <input type="checkbox" checked={settings.watermark} onChange={event => update({ watermark: event.target.checked })} /> 使用带水印的视频
      </label>
    </>
  );
}
