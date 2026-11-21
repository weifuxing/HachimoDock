/**
 * [Input] useDeviceContext for binding/usb/rescanUsbDevices; useToast for manual scan result notices.
 * [Output] Dashboard card showing the bound board, its verified desktop connection, and a manual USB serial rescan button.
 * [Pos] component node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import React, { useEffect, useState } from "react";
import { Monitor, RefreshCw, Usb, Wifi, WifiOff } from "lucide-react";
import { useDeviceContext } from "../shell/DeviceContext.jsx";
import { useToast } from "../shell/ToastStack.jsx";

export default function DeviceStatusBar({ onOpenDeviceSettings }) {
  const { binding, usb, wifi, activeBoardDeviceId, rescanUsbDevices, recoverAppearanceSerial, getAppearanceSerialResetRequired } = useDeviceContext();
  const recoveryBoardDeviceId = binding?.boardDeviceId;
  const { push } = useToast();
  const [scanningUsb, setScanningUsb] = useState(false);
  const [needsRecovery, setNeedsRecovery] = useState(false);
  const [recovering, setRecovering] = useState(false);

  useEffect(() => {
    setNeedsRecovery(false);
    if (!recoveryBoardDeviceId || usb.connected || wifi) return;
    let stale = false;
    getAppearanceSerialResetRequired(recoveryBoardDeviceId).then((required) => {
      if (!stale && required) setNeedsRecovery(true);
    }).catch(() => {});
    return () => { stale = true; };
  }, [recoveryBoardDeviceId, usb.connected, wifi, getAppearanceSerialResetRequired]);

  const handleRecover = async () => {
    setRecovering(true);
    try {
      await recoverAppearanceSerial(recoveryBoardDeviceId);
      setNeedsRecovery(false);
      push({ tone: "success", title: "已确认设备重启并恢复 USB 连接", message: "上次形象传输不会自动重发。" });
    } catch (err) {
      const messages = {
        appearance_reset_usb_missing: "未找到原设备的 USB 串口，请接回原设备后重试。",
        appearance_reset_usb_ambiguous: "检测到多个可能的串口，请只连接要恢复的设备后重试。",
        appearance_reset_usb_open_failed: "暂时无法打开原设备的串口。请关闭其他串口工具后重试。",
        appearance_reset_handshake_unconfirmed: "未能确认重新连接。请断开 USB 后重新连接，再点恢复。",
        appearance_reset_metadata_missing: "此旧保护记录缺少设备信息，客户端无法安全确认复位。",
        appearance_reset_no_quarantine: "当前设备没有待恢复的形象传输保护，请重新扫描串口。",
        appearance_recovery_busy: "当前已有设备连接，请断开其他设备后重试。",
        appearance_safety_journal_unavailable: "无法读取或保存本机的传输保护记录，请退出其他客户端实例后重试。",
      };
      const code = String(err?.message || err);
      push({
        tone: "warning",
        title: "USB 连接暂未恢复",
        message: messages[code] || "尚未确认原设备已复位。请断开 USB 后重新连接，再试一次。",
      });
    } finally {
      setRecovering(false);
    }
  };

  const handleRescanUsb = async () => {
    setScanningUsb(true);
    try {
      const result = await rescanUsbDevices();
      if (result?.status?.connected) {
        setNeedsRecovery(false);
        push({
          tone: "success",
          title: `已连接 USB 串口 ${result.status.portName || ""}`.trim(),
        });
      } else if (result?.devices?.length) {
        push({
          tone: "warning",
          title: "已扫描到 USB 串口，但连接未完成",
          message: result.devices.map((device) => device.portName).join(" / "),
        });
      } else {
        push({
          tone: "warning",
          title: "未检测到 USB 串口设备",
          message: "请确认设备端 USB gadget 已启动，或重新插拔线缆后再扫描。",
        });
      }
    } catch (err) {
      const code = String(err?.message || err);
      if (code === "appearance_raw_stream_quarantined_no_probe") {
        setNeedsRecovery(true);
        return;
      }
      push({ tone: "error", title: "USB 串口扫描失败", message: code });
    } finally {
      setScanningUsb(false);
    }
  };

  if (!activeBoardDeviceId) return null;

  const legacyTransportLabel = String(binding?.boardDeviceId === activeBoardDeviceId ? binding.wifiSsid || "" : "").trim();
  const usbOnlyBindingMatch = legacyTransportLabel.match(/^USB\((.+)\)$/);
  const savedUsbPortName = usbOnlyBindingMatch?.[1] || "";
  const usbPortLabel = usb.connected ? usb.portName || savedUsbPortName || "已连接" : "未连接";
  const connectionLabel = usb.connected ? "已USB连接" : wifi ? "已WIFI连接" : "未连接客户端";
  const ConnectionIcon = usb.connected ? Usb : wifi ? Wifi : WifiOff;
  return (
    <>
      <div className="dashboard-status-bar">
        <span className="dashboard-status-bar__icon">
          <Monitor size={18} />
        </span>
        <div className="dashboard-status-bar__copy">
          <strong className="dashboard-status-bar__board-id">{activeBoardDeviceId}</strong>
          <span className="dashboard-status-bar__sub">
            {usb.connected ? `USB: ${usbPortLabel}` : wifi ? "当前 Wi-Fi 配对设备" : "USB: 未连接"}
          </span>
        </div>
        {!usb.connected && onOpenDeviceSettings && (
          <button type="button" className="dashboard-status-bar__scan-btn" onClick={onOpenDeviceSettings}>
            <Wifi size={14} />Wi-Fi 设备配对
          </button>
        )}
        <div className="dashboard-status-bar__chips">
          <span className={`dashboard-status-bar__chip dashboard-status-bar__chip--${usb.connected || wifi ? "ok" : "warn"}`}>
            <ConnectionIcon size={14} />
            {connectionLabel}
          </span>
        </div>
        <button
          type="button"
          className="dashboard-status-bar__scan-btn"
          onClick={handleRescanUsb}
          disabled={scanningUsb || recovering}
          title="重新扫描 USB 串口设备"
        >
          <RefreshCw size={14} className={scanningUsb ? "spin" : undefined} />
          {scanningUsb ? "扫描中..." : "重新扫描串口"}
        </button>
      </div>
      {needsRecovery && !usb.connected && (
        <div className="dashboard-status-bar" role="status">
          <div className="dashboard-status-bar__copy">
            <strong>上次形象同步中断，USB 连接需要恢复</strong>
            <span className="dashboard-status-bar__sub">
              请断开 USB 后重新连接，再点下方按钮。上次传输不会自动重发。
            </span>
          </div>
          <button type="button" className="dashboard-status-bar__scan-btn" onClick={handleRecover} disabled={recovering || scanningUsb}>
            {recovering ? "正在确认设备重启…" : "确认重启并恢复"}
          </button>
        </div>
      )}
    </>
  );
}
