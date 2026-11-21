/**
 * [Input] open/onClose callbacks, expected board identity, and ACK-gated P4
 *         diagnostic Tauri commands.
 * [Output] Live reset/crash, memory, storage, runtime and USB WASM capabilities with safe reboot
 *          and input-binding reset actions that preserve appearance assets and
 *          await client cache rehydration from the board.
 * [Pos] component node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import React, { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { AlertTriangle, LoaderCircle, Power, RefreshCw, RotateCcw, X } from "lucide-react";
import { wifiDeviceControl } from "../lib/wifi-device-control.js";

function formatBytes(value) {
  if (!Number.isFinite(value)) return "—";
  const bytes = value;
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function formatDuration(value) {
  const seconds = Math.max(0, Math.floor((Number(value) || 0) / 1000));
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const rest = seconds % 60;
  if (hours) return `${hours} 小时 ${minutes} 分`;
  if (minutes) return `${minutes} 分 ${rest} 秒`;
  return `${rest} 秒`;
}

const LCD_PHASE_LABELS = ["帧间", "画面合成", "旋转", "帧比较", "提交到 LCD"];

const RESET_LABELS = {
  power_on: "上电启动",
  external_pin: "外部复位",
  software: "软件重启",
  panic: "程序异常",
  interrupt_watchdog: "中断看门狗",
  task_watchdog: "任务看门狗",
  watchdog: "看门狗",
  brownout: "供电欠压",
  power_glitch: "电源抖动",
  cpu_lockup: "CPU 锁死",
  usb: "USB 复位",
  jtag: "JTAG 复位",
};

function widgetDataEvidence(streams) {
  if (!streams || typeof streams !== "object" || Array.isArray(streams)) return null;
  const counters = Object.entries(streams).filter(([source, stream]) =>
    /^[a-z0-9._-]{1,47}$/.test(source) && stream?.received === true
      && typeof stream.fresh === "boolean"
      && [stream.usbCount, stream.wifiCount].every(count => Number.isSafeInteger(count) && count >= 0),
  );
  return {
    widgetData: Object.fromEntries(counters.map(([source, { received, fresh, usbCount, wifiCount }]) =>
      [source, { received, fresh, usbCount, wifiCount }],
    )),
  };
}

function Metric({ label, value, tone = "" }) {
  return (
    <div className="device-diagnostics-modal__metric" data-tone={tone || undefined}>
      <dt>{label}</dt>
      <dd>{value}</dd>
    </div>
  );
}

export default function DeviceDiagnosticsModal({
  open,
  onClose,
  onInputConfigReset,
  expectedBoardDeviceId,
  wifiTarget,
  wasmCapabilities,
}) {
  const [report, setReport] = useState(null);
  const [pending, setPending] = useState(false);
  const [action, setAction] = useState("");
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [confirmAction, setConfirmAction] = useState("");
  const readSequence = useRef(0);

  const loadDiagnostics = useCallback(async () => {
    const sequence = ++readSequence.current;
    setPending(true);
    setError("");
    try {
      const response = wifiTarget ? await wifiDeviceControl(invoke, wifiTarget, "readDiagnostics")
        : await invoke("usb_get_diagnostics", { expectedBoardDeviceId });
      if (sequence === readSequence.current) setReport(response);
    } catch (loadError) {
      if (sequence === readSequence.current) setError(typeof loadError === "string" ? loadError : String(loadError));
    } finally {
      if (sequence === readSequence.current) setPending(false);
    }
  }, [expectedBoardDeviceId, wifiTarget]);

  useEffect(() => {
    if (!open) return;
    setReport(null);
    setAction("");
    setError("");
    setNotice("");
    setConfirmAction("");
    loadDiagnostics();
    return () => { ++readSequence.current; };
  }, [loadDiagnostics, open]);

  const resetInputs = async () => {
    if (confirmAction !== "reset-inputs") {
      setConfirmAction("reset-inputs");
      return;
    }
    setAction("reset-inputs");
    setError("");
    setNotice("");
    try {
      const result = wifiTarget ? await wifiDeviceControl(invoke, wifiTarget, "resetInput")
        : await invoke("usb_reset_input_config", { expectedBoardDeviceId });
      setNotice(result?.message || "按键和摇杆已恢复默认映射，形象素材未改动。");
      setConfirmAction("");
      await onInputConfigReset?.(wifiTarget ? result : undefined);
      await loadDiagnostics();
    } catch (resetError) {
      setError(typeof resetError === "string" ? resetError : String(resetError));
    } finally {
      setAction("");
    }
  };

  const reboot = async () => {
    if (confirmAction !== "reboot") {
      setConfirmAction("reboot");
      return;
    }
    setAction("reboot");
    setError("");
    try {
      if (wifiTarget) {
        const result = await invoke("lan_reboot", { expectedBootId: wifiTarget.bootId });
        setNotice(result.observed ? "已确认设备重新上线。请重新打开诊断读取新状态。" : "重启状态尚未确认。");
      } else {
        const result = await invoke("usb_reboot_device", { expectedBoardDeviceId });
        setNotice(result?.message || "设备正在重启，USB 会短暂断开。");
      }
      setConfirmAction("");
    } catch (rebootError) {
      setError(typeof rebootError === "string" ? rebootError : String(rebootError));
    } finally {
      setAction("");
    }
  };

  if (!open) return null;
  const evidence = widgetDataEvidence(report?.widgetData);
  const copyWidgetDataEvidence = async () => {
    if (!evidence) return;
    setError("");
    setNotice("");
    try {
      await navigator.clipboard.writeText(JSON.stringify(evidence, null, 2));
      setNotice("组件数据链路诊断已复制。");
    } catch {
      setError("无法复制组件数据链路诊断。");
    }
  };
  const runtime = report?.runtime || {};
  const touchHealth = runtime.touchHealth || {};
  const touchReady = runtime.touchReady === true ? "已就绪" : runtime.touchReady === false ? "未就绪" : "未提供状态";
  const wasmCapability = (name) => {
    if (!wasmCapabilities) return "未读取";
    const value = wasmCapabilities[name];
    return typeof value === "string" && value ? value : "设备未声明";
  };
  const memory = report?.memory || {};
  const storage = report?.storage || {};
  const audio = report?.audio;
  const scanout = runtime.lcdScanout;
  const desktopBuild = report?.desktopBuild || {};
  const panicSites = report?.previousNativePanic?.available === true
    ? (report.previousNativePanic.sites || []).filter(site => Number.isInteger(site.pc))
    : [];
  const resetReason = RESET_LABELS[report?.lastResetReason] || report?.lastResetReason || "未知";
  const storageUsage = storage.totalBytes
    ? `${formatBytes(storage.usedBytes)} / ${formatBytes(storage.totalBytes)}`
    : "未知";

  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={() => onClose?.()}>
      <div
        className="modal-card device-diagnostics-modal"
        role="dialog"
        aria-modal="true"
        aria-label="ESP32-P4 设备诊断"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <div>
            <h3 className="modal-title">设备诊断</h3>
            <p className="device-diagnostics-modal__subtitle">ESP32-P4 实时运行状态与恢复工具</p>
          </div>
          <button type="button" className="icon-btn" onClick={() => onClose?.()} aria-label="关闭">
            <X size={16} />
          </button>
        </div>

        <div className="modal-body device-diagnostics-modal__body">
          <div className="device-diagnostics-modal__toolbar">
            <span className={report?.lastResetWasFault ? "is-fault" : ""}>
              {report?.lastResetWasFault && <AlertTriangle size={14} />}
              最近复位：{resetReason}
            </span>
            {evidence && (
              <button type="button" className="btn-secondary btn-sm" onClick={copyWidgetDataEvidence} disabled={pending}>
                复制组件数据链路
              </button>
            )}
            <button type="button" className="icon-btn" onClick={loadDiagnostics} disabled={pending} title="刷新诊断">
              {pending ? <LoaderCircle className="spin" size={15} /> : <RefreshCw size={15} />}
            </button>
          </div>

          {report && (
            <dl className="device-diagnostics-modal__metrics">
              <Metric label="运行时间" value={formatDuration(runtime.uptimeMs)} />
              <Metric
                label="固件 / 分区 / 镜像状态"
                value={`${runtime.firmware || "未知"} / ${runtime.runningPartition || "未知"} / ${runtime.imageState || "未知"}`}
              />
              <Metric label="设备构建" value={runtime.buildId || "旧固件未提供"} />
              <Metric label="PC 构建" value={desktopBuild.buildId || "未知"} />
              <Metric label="协议 Schema（设备 / PC）" value={`${runtime.protocolSchema || "?"} / ${desktopBuild.protocolSchema || "?"}`} />
              <Metric label="启动 / 故障复位" value={`${report.bootCount || 0} / ${report.faultResetCount || 0}`} tone={report.lastResetWasFault ? "warning" : ""} />
              {Number.isInteger(scanout?.underruns) && <Metric label="LCD 供数不足（本次开机）"
                value={scanout.underruns === 0 ? "0 次" : `${scanout.underruns} 次 · ${LCD_PHASE_LABELS[scanout.lastPhase] || "未知阶段"}${scanout.videoWorkerActive ? " · 视频解码或打包进行中" : ""}`}
                tone={scanout.underruns > 0 ? "warning" : ""} />}
              {panicSites.length > 0 && <Metric label="上次异常位置"
                value={panicSites.map(site => `CPU ${site.core} · 0x${site.pc.toString(16)}`).join(" / ")} tone="warning" />}
              {audio && <Metric label="完成提示音（入队 / 获准播放 / 抑制）"
                value={`${audio.completionCueQueued ?? "?"} / ${audio.completionCueAdmitted ?? "?"} / ${audio.completionCueSuppressed ?? "?"}`} />}
              {audio && <Metric label="扬声器状态" value={audio.musicActive ? "媒体播放" : audio.conversationActive ? "实时对话" : audio.playbackActive ? "播放中" : audio.playbackReady ? "空闲" : "未就绪"} />}
              <Metric label="页面 / Agent" value={`${runtime.screenPage || "未知"} / ${runtime.agentState || "idle"}`} />
              <Metric label="WASM 运行时 · widgetWasm" value={wasmCapability("widgetWasm")} />
              <Metric label="WASM 原生画布 · widgetWasmCanvas" value={wasmCapability("widgetWasmCanvas")} />
              <Metric label="WASM 宿主能力 · widgetWasmHost" value={wasmCapability("widgetWasmHost")} />
              {Number.isFinite(report.wasmDroppedInputs) && (
                <Metric label="WASM 输入丢包（本次开机）" value={report.wasmDroppedInputs}
                  tone={report.wasmDroppedInputs ? "warning" : ""} />
              )}
              {report.wasmLastFailure?.componentId && (
                <Metric label="最近组件运行错误" tone="warning"
                  value={`${report.wasmLastFailure.componentId} · ${report.wasmLastFailure.wasmSha256?.slice(0, 12) || "未知版本"} · ${report.wasmLastFailure.error}`} />
              )}
              <Metric label="触屏状态" value={touchReady} tone={runtime.touchReady === false ? "warning" : ""} />
              <Metric label="触控控制器 / 初始化" value={`${touchHealth.controller || "未识别"} / ${touchHealth.stage || "未知"}`} />
              <Metric label="触屏读取错误（连续 / 累计）" value={`${touchHealth.consecutiveErrors ?? "?"} / ${touchHealth.pollErrors ?? "?"}`} />
              <Metric label="堆内存（当前 / 最低）" value={`${formatBytes(memory.freeHeapBytes)} / ${formatBytes(memory.minimumFreeHeapBytes)}`} />
              <Metric label="PSRAM（当前 / 最低）" value={`${formatBytes(memory.freePsramBytes)} / ${formatBytes(memory.minimumFreePsramBytes)}`} />
              <Metric label="形象存储 / 槽位" value={`${storageUsage} / ${storage.activeAppearanceSlot ?? "?"}`} />
              <Metric label="任务 / 输入丢包" value={`${runtime.taskCount || 0} / ${runtime.inputDroppedEvents || 0}`} tone={runtime.inputDroppedEvents ? "warning" : ""} />
            </dl>
          )}
          {pending && !report && <div className="device-diagnostics-modal__loading"><LoaderCircle className="spin" size={18} />正在读取设备...</div>}
          {error && <div className="device-diagnostics-modal__message" data-tone="error">{error}</div>}
          {notice && <div className="device-diagnostics-modal__message" data-tone="success">{notice}</div>}

          <div className="device-diagnostics-modal__actions">
            <button type="button" className="btn-secondary" onClick={resetInputs} disabled={Boolean(action)}>
              {action === "reset-inputs" ? <LoaderCircle className="spin" size={14} /> : <RotateCcw size={14} />}
              {confirmAction === "reset-inputs" ? "再次点击确认" : "恢复默认按键"}
            </button>
            <button type="button" className="btn-secondary" onClick={reboot} disabled={Boolean(action)}>
              {action === "reboot" ? <LoaderCircle className="spin" size={14} /> : <Power size={14} />}
              {confirmAction === "reboot" ? "再次点击确认" : "重启设备"}
            </button>
          </div>
          <p className="device-diagnostics-modal__preserve">以上操作均保留设备中的形象素材与当前可回滚固件。</p>
        </div>
      </div>
    </div>
  );
}
