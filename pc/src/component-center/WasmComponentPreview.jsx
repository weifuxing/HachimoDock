import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import "./wasm-preview/screen.css";

import fonts from "virtual:p4-wasm-font";
import { createWasmFrameRenderer } from "./wasm-preview/frame.mjs";
import { AUDIO_PROFILE, MAX_WASM_BYTES } from "./wasm-preview/runtime.mjs";
import { loadAudioBank, SAMPLE_RATE } from "./wasm-preview/audio-bank.mjs";
import { resolveWasmControls } from "./wasm-preview/input.mjs";

const TOUCH_DOWN = 1, TOUCH_MOVE = 2, TOUCH_UP = 3, TOUCH_CANCEL = 6;
const KEY_DOWN = 4;

async function moduleBytes(id, versionHash, preview, signal) {
  const { resolveUrl, sha256 } = preview;
  if (resolveUrl) {
    const response = await fetch(await resolveUrl(), { signal });
    if (!response.ok) throw new Error("内置 WASM 模块无法读取");
    return new Uint8Array(await response.arrayBuffer());
  }
  const bytes = await invoke("read_component_wasm_preview", { id, versionHash, sha256 });
  return Uint8Array.from(bytes);
}

async function audioBytes(id, versionHash, preview, signal) {
  if (preview.resolveAudioUrl) {
    const response = await fetch(await preview.resolveAudioUrl(), { signal });
    if (!response.ok) throw new Error("内置 WASM 语音资源无法读取");
    return response.arrayBuffer();
  }
  if (preview.audioSha256) {
    return invoke("read_component_wasm_audio", { id, versionHash, sha256: preview.audioSha256 });
  }
  return null;
}

const drawFrame = createWasmFrameRenderer(fonts);

function playCue(context, bank, cue, current, loop = false) {
  if (cue === undefined || cue === -2) return;
  if (loop && current.current?.cue === cue) return;
  current.current?.stop();
  current.current = null;
  if (cue === -1 || context?.state !== "running") return;
  const pcm = bank?.(cue);
  if (!pcm) return;
  const voice = context.createBufferSource();
  voice.buffer = context.createBuffer(1, pcm.length, SAMPLE_RATE);
  voice.buffer.copyToChannel(pcm, 0);
  const gain = context.createGain();
  gain.gain.value = loop ? 0.3 : 0.7;
  voice.loop = loop;
  voice.cue = cue;
  voice.connect(gain);
  gain.connect(context.destination);
  current.current = voice;
  voice.onended = () => {
    if (current.current === voice) current.current = null;
    voice.disconnect();
    gain.disconnect();
  };
  voice.start();
}

function playTones(context, tones, voices) {
  if (context?.state !== "running") return;
  const tone = tones.at(-1);
  if (!tone) return;
  for (const voice of voices) voice.stop();
  voices.clear();
  const voice = context.createOscillator();
  const gain = context.createGain();
  const start = context.currentTime;
  const end = start + tone.ms / 1000;
  voice.type = "triangle";
  voice.frequency.value = tone.hz;
  gain.gain.setValueAtTime(0, start);
  gain.gain.linearRampToValueAtTime(tone.volume / 500, Math.min(start + 0.008, end));
  gain.gain.linearRampToValueAtTime(0, end + 0.004);
  voice.connect(gain);
  gain.connect(context.destination);
  voices.add(voice);
  voice.onended = () => {
    voices.delete(voice);
    voice.disconnect();
    gain.disconnect();
  };
  voice.start();
  voice.stop(end + 0.004);
}

export default function WasmComponentPreview({ component, bindings, active, reservedEvents }) {
  const canvas = useRef(null);
  const enqueue = useRef(null);
  const pointer = useRef(null);
  const keys = useRef(new Set());
  const audio = useRef(null);
  const voices = useRef(new Set());
  const speech = useRef(null);
  const music = useRef(null);
  const [visible, setVisible] = useState(() => !document.hidden);
  const [phase, setPhase] = useState("loading");
  const [error, setError] = useState("");
  const [sound, setSound] = useState(false);
  const [clockUnsynced, setClockUnsynced] = useState(false);
  const { id, versionHash, wasmPreview: preview } = component;
  const running = active && visible;
  const controls = resolveWasmControls(bindings, reservedEvents);

  useEffect(() => {
    const changed = () => setVisible(!document.hidden);
    document.addEventListener("visibilitychange", changed);
    return () => document.removeEventListener("visibilitychange", changed);
  }, []);

  useEffect(() => {
    if (!running) return undefined;
    const abort = new AbortController();
    const events = [];
    let session, timeout, animation, bank;
    let stopped = false, busy = false, ready = false;
    let lastFrame = 0;

    const stop = () => {
      stopped = true;
      abort.abort();
      if (session) invoke("close_wasm_preview", { id: session }).catch(() => {});
      clearTimeout(timeout);
      cancelAnimationFrame(animation);
      enqueue.current = null;
      pointer.current = null;
      keys.current.clear();
      for (const voice of voices.current) voice.stop();
      voices.current.clear();
      playCue(audio.current, null, -1, speech);
      playCue(audio.current, null, -1, music);
    };
    const fail = (reason) => {
      stop();
      setError(String(reason?.message || reason));
      setPhase("error");
    };
    const send = async (message) => {
      busy = true;
      try {
        const results = await invoke("tick_wasm_preview", { id: session, dt: message.dt,
          events: message.events, epoch: clockUnsynced ? 0 : Date.now(), offset: -new Date().getTimezoneOffset() });
        if (stopped) return;
        let effect, background;
        for (const result of results) {
          if (result.cue !== undefined && result.cue !== -2) effect = result.cue;
          if (result.music !== undefined && result.music !== -2) background = result.music;
          if (result.frame) drawFrame(canvas.current, result);
          playTones(audio.current, result.tones, voices.current);
          if (result.frame) setPhase("ready");
        }
        playCue(audio.current, bank, background, music, true);
        playCue(audio.current, bank, effect, speech);
      } catch (err) { if (!stopped) fail(err); }
      finally { busy = false; }
    };
    const tick = (now) => {
      if (stopped) return;
      animation = requestAnimationFrame(tick);
      if (!ready || busy) return;
      if (lastFrame && now - lastFrame < 1000 / 30) return;
      const dt = lastFrame ? Math.min(80, Math.round(now - lastFrame)) : 0;
      lastFrame = now;
      send({ type: "tick", events: events.splice(0, 12), dt });
    };
    enqueue.current = (...args) => {
      if (!ready || stopped) return false;
      const type = args[0];
      if (type === TOUCH_MOVE && events.at(-1)?.[0] === TOUCH_MOVE) events.pop();
      const reserve = type === TOUCH_MOVE ? 4 : type === TOUCH_UP || type === TOUCH_CANCEL ? 0 : 1;
      if (events.length >= 24 - reserve) return false;
      events.push(args);
      return true;
    };

    setPhase("loading");
    setError("");
    const start = async () => {
      try {
        const [bytes, bankBytes] = await Promise.all([
          moduleBytes(id, versionHash, preview, abort.signal),
          audioBytes(id, versionHash, preview, abort.signal),
        ]);
        if (bankBytes && crypto.subtle) {
          const hash = await crypto.subtle.digest("SHA-256", bankBytes);
          const hex = Array.from(new Uint8Array(hash), v => v.toString(16).padStart(2, "0")).join("");
          if (hex !== preview.audioSha256) throw new Error("WASM 语音 SHA-256 校验失败");
        }
        bank = bankBytes ? loadAudioBank(bankBytes) : null;
        if (bytes.length > MAX_WASM_BYTES) throw new Error("WASM 模块超过 192 KiB");
        if (crypto.subtle) {
          const hash = await crypto.subtle.digest("SHA-256", bytes);
          const hex = Array.from(new Uint8Array(hash), (v) => v.toString(16).padStart(2, "0")).join("");
          if (hex !== preview.sha256) throw new Error("WASM 模块 SHA-256 校验失败");
        }
        if (stopped) return;
        session = await invoke("open_wasm_preview", { bytes: Array.from(bytes),
          epoch: clockUnsynced ? 0 : Date.now(), offset: -new Date().getTimezoneOffset() });
        if (stopped) { invoke("close_wasm_preview", { id: session }).catch(() => {}); return; }
        ready = true;
        animation = requestAnimationFrame(tick);
      } catch (err) {
        if (!stopped) fail(err);
      }
    };
    start();
    return stop;
  }, [id, versionHash, preview, running, clockUnsynced]);

  useEffect(() => {
    if (!running) {
      audio.current?.suspend().catch(() => {});
      setSound(false);
    }
  }, [running]);
  useEffect(() => () => { audio.current?.close().catch(() => {}); }, []);

  const toggleSound = async () => {
    try {
      const context = audio.current ??= new AudioContext();
      if (sound) {
        playCue(context, null, -1, speech);
        playCue(context, null, -1, music);
        await context.suspend();
      }
      else await context.resume();
      setSound(context.state === "running");
    } catch { setSound(false); }
  };
  const position = (event) => {
    const box = event.currentTarget.getBoundingClientRect();
    const { width, height } = event.currentTarget;
    return [
      Math.max(0, Math.min(width - 1, Math.floor((event.clientX - box.left) * width / box.width))),
      Math.max(0, Math.min(height - 1, Math.floor((event.clientY - box.top) * height / box.height))),
    ];
  };
  const touch = (type, event) => {
    if (!running || phase !== "ready") return;
    if (type === TOUCH_DOWN) {
      if (pointer.current !== null || event.button !== 0) return;
      if (!enqueue.current?.(type, ...position(event))) return;
      event.currentTarget.focus({ preventScroll: true });
      event.currentTarget.setPointerCapture(event.pointerId);
      pointer.current = event.pointerId;
      return;
    }
    if (pointer.current !== event.pointerId) return;
    if (type === TOUCH_UP || type === TOUCH_CANCEL) pointer.current = null;
    enqueue.current?.(type, ...position(event));
  };
  const keyboard = (event) => {
    const key = controls.find(control => control.code === event.code)?.key;
    if (!key || phase !== "ready" || !running || event.altKey || event.ctrlKey || event.metaKey) return;
    event.preventDefault();
    if (!event.repeat && !keys.current.has(event.code) && enqueue.current?.(KEY_DOWN, key, 0)) {
      keys.current.add(event.code);
    }
  };
  const canPlay = phase === "ready" && running;
  const native = preview.canvas === "640x480-v1";

  return (
    <section className="wasm-component-preview" data-wasm-preview={running ? phase : "paused"}>
      <div className="wasm-component-preview__screen">
        <canvas ref={canvas} width={native ? 640 : 320} height={native ? 480 : 240} tabIndex={0}
          aria-label={`${component.name} · WASM 试玩屏幕`}
          onPointerDown={(event) => touch(TOUCH_DOWN, event)}
          onPointerMove={(event) => touch(TOUCH_MOVE, event)}
          onPointerUp={(event) => touch(TOUCH_UP, event)}
          onPointerCancel={(event) => touch(TOUCH_CANCEL, event)}
          onLostPointerCapture={(event) => touch(TOUCH_CANCEL, event)}
          onKeyDown={keyboard}
          onKeyUp={(event) => { if (keys.current.delete(event.code)) event.preventDefault(); }}
          onBlur={() => keys.current.clear()} />
        {phase !== "ready" && component.coverDataUrl && <img src={component.coverDataUrl} alt="" />}
      </div>
      {running && phase === "error" && <p role="alert">试玩失败：{error}</p>}
      {running && phase === "loading" && <p role="status">正在启动组件…</p>}
      <div className="wasm-component-preview__controls" role="group" aria-label="试玩按键">
        {controls.map(({ event, keyLabel, key, label }) =>
          <button className="btn-secondary" key={event} type="button" disabled={!canPlay}
            title={label} aria-label={label}
            onClick={() => enqueue.current?.(KEY_DOWN, key, 0)}>{keyLabel}</button>
        )}
        <button className="btn-secondary" type="button" disabled={!canPlay} aria-pressed={sound}
          onClick={toggleSound}>{sound ? "关闭音效" : "开启音效"}</button>
        {preview.host && <label>
          <input type="checkbox" checked={clockUnsynced}
            onChange={(event) => setClockUnsynced(event.target.checked)} /> 模拟设备未校时
        </label>}
      </div>
      <p className="wasm-component-preview__hint">点击画面可触控，聚焦画面后可用上面标注的对应键。试玩进度独立于设备。</p>
    </section>
  );
}
