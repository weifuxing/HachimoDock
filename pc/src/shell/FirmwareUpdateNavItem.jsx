/**
 * [Input] Verified USB and Wi-Fi device snapshots, bundled-firmware metadata/update Tauri
 *         commands, firmware progress events, and toast feedback.
 * [Output] Sidebar-only firmware module: older devices show an inline Update
 *          button, equal/newer devices show non-interactive Up to date copy,
 *          both transports upgrade directly from the sidebar.
 * [Pos] app-shell sidebar node in pc/src/shell.
 * [Sync] If this file changes, update `pc/src/shell/.folder.md` and `pc/src/.folder.md`.
 */

import React, { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { Download, LoaderCircle } from "lucide-react";
import { currentFirmwareCompletion, firmwareUpdateDisposition } from "../lib/firmware-version.js";
import { useToast } from "./ToastStack.jsx";
import { useExitProtection } from "../lib/exit-protection.js";
import { firmwareAlreadyLatest, firmwareRollback, firmwareUpdateFailureMessage } from "../lib/firmware-update-error.js";

import { useDialogFocus } from "../component-center/use-dialog-focus.js";

function FirmwareBoardDialog({ onCancel, onConfirm }) {
  const [board, setBoard] = useState("");
  const dialogRef = useDialogFocus(onCancel);
  return createPortal(
    <div className="modal-backdrop" onClick={onCancel}>
      <form
        ref={dialogRef}
        className="modal-card firmware-board-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="firmware-board-title"
        onClick={(event) => event.stopPropagation()}
        onSubmit={(event) => {
          event.preventDefault();
          if (board) onConfirm(board);
        }}
      >
        <div className="modal-header">
          <h3 id="firmware-board-title" className="modal-title">选择设备板型</h3>
        </div>
        <div className="modal-body">
          <p>暂时无法自动确认板型。请核对开发板上的型号标识后再选择。</p>
          <div className="firmware-board-dialog__options">
            {[["wifi6", "WiFi6"], ["pico", "Pico"]].map(([value, label]) => (
              <label key={value}>
                <input
                  type="radio"
                  name="firmware-board-model"
                  value={value}
                  checked={board === value}
                  onChange={() => setBoard(value)}
                />
                {label}
              </label>
            ))}
          </div>
        </div>
        <div className="channel-switch-confirm-modal__actions">
          <button type="button" className="btn-secondary" onClick={onCancel}>取消</button>
          <button type="submit" className="btn-primary" disabled={!board}>开始更新</button>
        </div>
      </form>
    </div>,
    document.body,
  );
}

export default function FirmwareUpdateNavItem({ usb = {}, wifi }) {
  const { push } = useToast();
  const [probe, setProbe] = useState(null);
  const [probeRefresh, setProbeRefresh] = useState(0);
  const [expiredProbe, setExpiredProbe] = useState(null);
  const [choosingBoard, setChoosingBoard] = useState(null);
  const [completed, setCompleted] = useState(null);
  const [rollbackBoardId, setRollbackBoardId] = useState(null);
  const [wifiCompleted, setWifiCompleted] = useState(null);
  const [pending, setPending] = useState(false);
  const updateInFlight = useRef(false);
  const [percent, setPercent] = useState(0);
  const [stage, setStage] = useState("");
  const { boardDeviceId, connectionId, firmware } = usb;
  const reportedBoard = usb.capabilities?.firmwareUpdate?.hardwareBoardModel;
  const chipRevision = usb.capabilities?.firmwareUpdate?.chipRevision;
  const connectedP4 = usb.connected === true && usb.runtime === "esp-p4" && Boolean(boardDeviceId && connectionId);
  const hardwareProbePending = usb.capabilities?.firmwareUpdate?.hardwareBoardProbe === "pending";
  const probeKey = JSON.stringify([boardDeviceId, connectionId, firmware, reportedBoard, chipRevision, hardwareProbePending]);
  const awaitingBoardProbe = hardwareProbePending && expiredProbe !== probeKey;
  const currentProbe = connectedP4 && probe?.key === probeKey ? probe : null;
  const bundledFirmware = currentProbe?.info;
  const currentCompletion = currentFirmwareCompletion(usb, completed);
  const usbRollback = connectedP4 && !currentCompletion &&
    (bundledFirmware?.rollbackBlocked || rollbackBoardId === boardDeviceId);
  const requiresBoardChoice = currentProbe?.info === null && !awaitingBoardProbe && !currentCompletion && !usbRollback;
  const boardDialogOpen = requiresBoardChoice && choosingBoard === probeKey && !pending;
  const boardLabel = { wifi6: "WiFi6", pico: "Pico" }[bundledFirmware?.boardModel || currentCompletion?.boardModel];
  useExitProtection(pending ? "固件升级中，请等待设备重新连接并校验完成" : "");

  useEffect(() => {
    setChoosingBoard(null);
    if (!connectedP4) return;
    if (awaitingBoardProbe) {
      const timer = setTimeout(() => setExpiredProbe(probeKey), 2500);
      return () => clearTimeout(timer);
    }
    let cancelled = false;
    invoke("usb_get_bundled_firmware_info", {
      expectedBoardDeviceId: boardDeviceId,
      expectedConnectionId: connectionId,
    }).then((info) => {
      if (!cancelled) setProbe({ key: probeKey, info });
    }).catch(() => {
      if (!cancelled) setProbe({ key: probeKey, label: "暂不可更新" });
    });
    return () => {
      cancelled = true;
    };
  }, [connectedP4, probeKey, boardDeviceId, connectionId, awaitingBoardProbe, probeRefresh]);

  const currentVersion = currentCompletion?.version || String(bundledFirmware?.currentVersion || firmware || "");
  const stageLabel = ({begin:"准备升级",upload:"写入中",recover:"恢复连接",verify:"校验中",reboot:"等待重启",validate:"确认版本"})[stage] || "准备升级";
  const disposition = connectedP4
    ? firmwareUpdateDisposition(currentVersion, bundledFirmware?.version || currentCompletion?.version)
    : "unknown";
  const currentWifiCompletion = wifi && wifiCompleted?.boardDeviceId === wifi.boardDeviceId
    && (wifiCompleted.previousBootId === wifi.bootId || wifiCompleted.bootId === wifi.bootId)
    ? wifiCompleted : null;
  const wifiVersion = currentWifiCompletion?.version || wifi?.firmware;
  const wifiDisposition = !usb.connected && wifi
    ? firmwareUpdateDisposition(wifiVersion, wifi.bundledFirmwareVersion)
    : "unknown";
  const wifiRollback = !usb.connected && wifiDisposition === "update" &&
    (wifi?.bundledFirmwareRollbackBlocked || rollbackBoardId === wifi?.boardDeviceId);
  const wifiUpdate = !usb.connected && wifi?.wirelessOta && wifiDisposition === "update" && !wifiRollback;
  const showRollback = (usbRollback && disposition !== "latest") || wifiRollback;

  const startUpdate = async (confirmedBoardModel) => {
    if (updateInFlight.current || showRollback) return;
    if (wifiUpdate) {
      if (!wifi.controlTrusted) {
        push({
          tone: "warning",
          title: "尚未授权无线更新",
          message: "请将同一台设备通过 USB 连接一次，授权无线控制后可直接更新。",
        });
        return;
      }
      if (!wifi.boardDeviceId || !wifi.bootId) {
        push({ tone: "warning", title: "暂未确认设备状态", message: "请等待设备重新上线后再更新。" });
        return;
      }
    } else if (requiresBoardChoice ? !boardDialogOpen || !confirmedBoardModel : disposition !== "update") {
      return;
    }

    updateInFlight.current = true;
    setChoosingBoard(null);
    setPending(true);
    setPercent(0);
    setStage("begin");
    let stop;
    const wifiScope = wifiUpdate ? { boardDeviceId: wifi.boardDeviceId, previousBootId: wifi.bootId } : null;
    try {
      const progressEvent = wifiUpdate ? "lan-firmware-update-progress" : "usb-firmware-update-progress";
      stop = await listen(progressEvent, ({ payload }) => {
        setPercent(Math.max(0, Math.min(100, Number(payload?.percent) || 0)));
        setStage(String(payload?.stage || ""));
      }).catch((error) => console.warn("[firmware-nav] progress unavailable", error));
      const result = wifiUpdate
        ? await invoke("lan_update_bundled_firmware", {
            expectedBoardDeviceId: wifiScope.boardDeviceId,
            expectedBootId: wifiScope.previousBootId,
          })
        : await invoke("usb_update_bundled_firmware", {
            expectedBoardDeviceId: boardDeviceId,
            expectedConnectionId: connectionId,
            ...(confirmedBoardModel ? { confirmedBoardModel } : {}),
          });
      const version = String(result?.version || bundledFirmware?.version || "");
      if (wifiUpdate) {
        setWifiCompleted({ ...wifiScope, bootId: result.bootId, version });
      } else {
        setCompleted({
          boardDeviceId,
          connectionId: result?.connectionId,
          version,
          boardModel: confirmedBoardModel || bundledFirmware?.boardModel,
        });
      }
      push({ tone: "success", title: "设备固件已更新", message: `版本 ${version || "未知"} 已确认有效。` });
    } catch (error) {
      if (firmwareRollback(error)) {
        setRollbackBoardId(wifiScope?.boardDeviceId || boardDeviceId);
      }
      if (firmwareAlreadyLatest(error)) {
        if (wifiUpdate) {
          setWifiCompleted({ ...wifiScope, version: wifi.bundledFirmwareVersion });
        } else {
          setProbe(null);
          setProbeRefresh((value) => value + 1);
        }
        push({ tone: "info", title: "固件已是最新", message: "当前设备无需升级。" });
        return;
      }
      push({
        tone: "error",
        title: "固件升级未完成",
        message: firmwareUpdateFailureMessage(error, { wireless: wifiUpdate }),
      });
    } finally {
      try { stop?.(); } catch { /* Keep the update result. */ }
      updateInFlight.current = false;
      setPending(false);
    }
  };

  return (
    <div
      className="sidebar-nav__item sidebar-firmware-update"
      aria-label="固件升级"
      title={bundledFirmware?.version
        ? `${boardLabel} · 设备 ${currentVersion || "未知"} · 内置 ${bundledFirmware.version}`
        : !usb.connected && wifi?.bundledFirmwareVersion
          ? `WiFi6 · 设备 ${wifiVersion || "未知"} · 内置 ${wifi.bundledFirmwareVersion}`
          : "固件升级"}
    >
      {pending ? <LoaderCircle size={16} className="spin" /> : <Download size={16} />}
      <span className="sidebar-nav-label">固件升级</span>
      {boardLabel && <span className="sidebar-firmware-update__board">{boardLabel}</span>}
      {connectedP4 && !pending && !showRollback && !boardLabel && !requiresBoardChoice && disposition !== "latest" && (
        <span className="sidebar-firmware-update__latest" role="status">
          {currentProbe?.label || "正在识别"}
        </span>
      )}
      {(pending || (!showRollback && (disposition === "update" || wifiUpdate || requiresBoardChoice))) && (
        <button
          type="button"
          className="sidebar-firmware-update__action"
          onClick={() => {
            if (requiresBoardChoice) setChoosingBoard(probeKey);
            else startUpdate();
          }}
          disabled={pending}
          aria-live="polite"
        >
          {pending ? `${stageLabel}${stage === "upload" ? ` ${percent}%` : "…"}` : "更新"}
        </button>
      )}
      {boardDialogOpen && (
        <FirmwareBoardDialog onCancel={() => setChoosingBoard(null)} onConfirm={startUpdate} />
      )}
      {!pending && showRollback && (
        <span className="sidebar-firmware-update__latest" role="status">已回退</span>
      )}
      {!pending && (disposition === "latest" || wifiDisposition === "latest") && (
        <span className="sidebar-firmware-update__latest">已最新</span>
      )}
    </div>
  );
}
