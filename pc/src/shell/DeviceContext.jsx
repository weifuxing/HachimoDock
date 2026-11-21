/**
 * [Input] children tree consuming useDeviceContext; Tauri invoke/listen for verified P4 USB status, appearance sync progress, manual serial rescan, bindings, and agents; lib helpers for appearance/agent storage.
 * [Output] Shared device state with verified USB priority and authenticated Wi-Fi reachability, current Agent selection and appearance, and explicit Agent follow over the active transport. Appearance transfers use the active supported transport.
 *          Wi-Fi appearance changes use authenticated device-side pack verification.
 *          Saved-persona events refresh the current target immediately; every conversation start reads fresh disk settings.
 *          Current-key usage help is published by the dashboard and consumed across setup surfaces.
 * [Pos] component node in pc/src/shell
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import { invoke } from "@tauri-apps/api/core";
import { LAN_AGENT_PAIRING_UPDATED, readLanAgentPairing, readRealtimeWifiBoard } from "../dashboard/lanAgentPairing.js";
import { buildUsageHelp } from "../lib/usage-help.js";
import { createRealtimePressHandler } from "../lib/realtime-press.js";
import { listen } from "@tauri-apps/api/event";
import {
  DEFAULT_AGENT_ID,
  FIXED_AGENT_OPTIONS,
  assignAppearanceToAgent,
  assignedAgentIds,
  loadAgentAppearanceMap,
  loadEnabledAgents,
  normalizeDetectedAgents,
  saveAgentAppearanceMap,
  saveEnabledAgents,
} from "../lib/agent-appearance-config.js";
import {
  applyDesktopPetAssignment,
  appearanceConfirmationRequired,
  createDesktopPetIntentId,
  APPEARANCE_SYNC_CANCELLED_MESSAGE,
  FOLLOW_ASSIGNMENT_BUSY_MESSAGE,
} from "../lib/desktop-pet-assignment.js";
import { getAppearance, listAppearances } from "../lib/appearance-store.js";
import { buildRealtimeChatStartInput, PERSONA_VOICE_UPDATED_EVENT } from "../lib/persona-voice.js";
import { componentPetVoice, createComponentVoiceSync } from "../lib/component-pet-voice.js";
import {
  readActiveComponentForTarget,
  readConfiguredComponentSshHost,
} from "../lib/active-component-store.js";
import {
  deriveCurrentDisplay,
  deriveDeviceReachability,
  normalizeUsbStatusPayload,
  pairedWifiTarget,
  pairedWifiTargetsEqual,
  usbStatusSnapshotsEqual,
} from "./DeviceContext.pure.js";

export { deriveCurrentDisplay };
import { createStatusReconciler, mergeStatus } from "../lib/status-reconciler.js";

const EMPTY_APPEARANCE_SYNC = {
  pending: false,
  cancelling: false,
  progress: null,
  agentId: "",
  appearanceId: "",
  appearanceName: "",
};

const AGENT_SCAN_FOCUS_STALE_MS = 5 * 60_000;
const USB_STATUS_POLL_MS = 3_000;
const USB_STATUS_SLOW_WARNING_MS = 10_000;

export const DeviceContext = createContext(null);
const EMPTY_USAGE_HELP = buildUsageHelp();

function bridgeEnabledAgents(profile) {
  const selectedAgentId = profile?.selectedAgentId || "";
  if (selectedAgentId) return new Set([selectedAgentId]);
  const enabled = Array.isArray(profile?.enabledAgents)
    ? profile.enabledAgents.filter(Boolean)
    : [];
  return enabled.length > 0 ? new Set([enabled[0]]) : null;
}

export function DeviceContextProvider({ binding: bindingProp, onBindingChange, children }) {
  const [usageHelp, setUsageHelp] = useState(EMPTY_USAGE_HELP);
  const [binding, setBindingState] = useState(bindingProp || null);
  useEffect(() => setBindingState(bindingProp || null), [bindingProp]);
  useEffect(() => { if (!binding) setUsageHelp(EMPTY_USAGE_HELP); }, [binding]);

  const [usb, setUsb] = useState({ connected: false, portName: "", boardDeviceId: "" });
  const [wifiStatus, setWifiStatus] = useState(null);
  const wifi = usb.connected ? null : wifiStatus;
  const { deviceOnline, onlineBoardDeviceId } = useMemo(
    () => deriveDeviceReachability({ usb, wifi }),
    [usb, wifi],
  );
  const activeBoardDeviceId = onlineBoardDeviceId || binding?.boardDeviceId || "";
  const realtimeWifiReady = Boolean(wifi && wifiStatus?.wirelessRealtime && wifiStatus?.controlTrusted);
  const [wifiAppearance, setWifiAppearance] = useState(null);
  const [appearances, setAppearances] = useState([]);
  const [agentAppearanceMap, setAgentAppearanceMap] = useState({});
  const [enabledAgents, setEnabledAgents] = useState(
    () => new Set([DEFAULT_AGENT_ID]),
  );
  const [bridgeSelectedAgentId, setBridgeSelectedAgentId] = useState("");
  const [agentOptions, setAgentOptions] = useState(() =>
    FIXED_AGENT_OPTIONS.map((agent) => ({ ...agent, detected: false })),
  );
  const [agentScan, setAgentScan] = useState({
    pending: false,
    scannedAt: 0,
    detectedCount: 0,
    error: "",
  });
  const agentScanRequestRef = useRef(null);
  const lastAgentScanAttemptAtRef = useRef(0);
  const [activeComponentRevision, setActiveComponentRevision] = useState(0);
  const currentComponentTarget = useMemo(() => {
    const usbBoardDeviceId = String(usb.boardDeviceId || "").trim();
    if (usb.connected && usbBoardDeviceId) {
      return { transport: "usb", boardDeviceId: usbBoardDeviceId };
    }
    if (wifi) return { transport: "usb", boardDeviceId: wifi.boardDeviceId };
    const sshHost = readConfiguredComponentSshHost();
    if (sshHost) return { transport: "ssh", sshHost };
    const rememberedBoardDeviceId = String(binding?.boardDeviceId || "").trim();
    return rememberedBoardDeviceId
      ? { transport: "usb", boardDeviceId: rememberedBoardDeviceId }
      : null;
  }, [
    binding?.boardDeviceId,
    usb.boardDeviceId,
    usb.connected,
    wifi,
  ]);
  const currentComponent = useMemo(
    () => readActiveComponentForTarget(currentComponentTarget),
    [activeComponentRevision, currentComponentTarget],
  );
  const [appearanceSync, setAppearanceSync] = useState(EMPTY_APPEARANCE_SYNC);
  const [appearanceRecovery, setAppearanceRecovery] = useState(null);
  const appearanceSyncTokenRef = useRef(0);
  const appearanceAssignmentOwnerRef = useRef(null);
  const appearanceUsbRef = useRef(usb);
  appearanceUsbRef.current = usb;
  useEffect(() => () => { appearanceAssignmentOwnerRef.current = null; }, []);
  // 实时对话（2026-09-18）：Rust 持有会话，这里只镜像状态并把板端按键请求解析成当前形象。
  const [realtimeChat, setRealtimeChat] = useState({ active: false, state: "idle", error: "", reason: "" });
  const realtimeReconcilerRef = useRef(null);

  // --- USB status poll (3s) + event hints; Rust owns serial auto-connect. ---
  useEffect(() => {
    let cancelled = false;
    let inFlight = false;
    let slowWarningTimer = null;
    let refreshPending = false;
    let revision = 0;
    const unlisteners = [];
    const check = async () => {
      if (cancelled || inFlight) return;
      inFlight = true;
      const startedAt = revision;
      slowWarningTimer = setTimeout(() => {
        console.warn("[DeviceContext] usb_get_status is taking longer than expected");
      }, USB_STATUS_SLOW_WARNING_MS);
      try {
        const status = await invoke("usb_get_status");
        if (cancelled || startedAt !== revision) return;
        const nextUsb = normalizeUsbStatusPayload(status);
        setUsb((currentUsb) => (
          usbStatusSnapshotsEqual(currentUsb, nextUsb) ? currentUsb : nextUsb
        ));
      } catch (err) {
        if (cancelled) return;
        console.warn("[DeviceContext] usb_get_status failed", err);
      } finally {
        if (slowWarningTimer) clearTimeout(slowWarningTimer);
        slowWarningTimer = null;
        inFlight = false;
        if (!cancelled && refreshPending) {
          refreshPending = false;
          void check();
        }
      }
    };
    const requestRefresh = () => {
      if (cancelled) return;
      // A hint invalidates an older read, never authorizes connection/identity.
      revision += 1;
      refreshPending = true;
      if (!inFlight) {
        refreshPending = false;
        void check();
      }
    };
    const retainListener = (stop) => {
      if (cancelled) stop();
      else unlisteners.push(stop);
    };
    listen("usb-message", (event) => {
      const topic = event?.payload?.topic;
      if (topic === "hello" || topic === "availability") requestRefresh();
    }).then((stop) => {
      retainListener(stop);
      // Recover a status change between the initial query and subscription.
      requestRefresh();
    }).catch(() => {});
    listen("usb-disconnected", requestRefresh).then(retainListener).catch(() => {});
    check();
    const id = setInterval(check, USB_STATUS_POLL_MS);
    return () => {
      cancelled = true;
      revision += 1;
      refreshPending = false;
      clearInterval(id);
      if (slowWarningTimer) clearTimeout(slowWarningTimer);
      unlisteners.forEach((stop) => { try { stop(); } catch { /* already closed */ } });
    };
  }, []);

  useEffect(() => {
    if (usb.connected) {
      setWifiStatus(null);
      return;
    }
    let disposed = false;
    let revision = 0;
    let inFlight = false;
    let refreshPending = false;
    const refreshWifi = async () => {
      if (disposed || inFlight) return;
      inFlight = true;
      const startedAt = revision;
      let target = null;
      try {
        if (await readLanAgentPairing()) {
          target = pairedWifiTarget(await invoke("lan_device_status"));
        }
      } catch { /* a saved pairing alone does not prove reachability */ }
      if (!disposed && startedAt === revision) {
        setWifiStatus(previous => pairedWifiTargetsEqual(previous, target) ? previous : target);
      }
      inFlight = false;
      if (refreshPending) {
        refreshPending = false;
        void refreshWifi();
      }
    };
    const onPairingChange = () => {
      revision += 1;
      setWifiStatus(null);
      if (inFlight) refreshPending = true;
      else void refreshWifi();
    };
    void refreshWifi();
    const timer = setInterval(refreshWifi, 5000);
    window.addEventListener(LAN_AGENT_PAIRING_UPDATED, onPairingChange);
    return () => {
      disposed = true;
      clearInterval(timer);
      window.removeEventListener(LAN_AGENT_PAIRING_UPDATED, onPairingChange);
    };
  }, [usb.connected]);

  // --- Initial: load bridge profile, detect agents, list appearances ---
  const loadAppearancesData = useCallback(async ({ force = false } = {}) => {
    try {
      const records = await listAppearances({ force });
      setAppearances(records);
      const map = loadAgentAppearanceMap(records);
      setAgentAppearanceMap(map);
      const enabled = loadEnabledAgents() || new Set([DEFAULT_AGENT_ID]);
      setEnabledAgents(enabled);
      return records;
    } catch (err) {
      console.warn("[DeviceContext] listAppearances failed", err);
      return [];
    }
  }, []);

  const refreshAppearances = useCallback(
    () => loadAppearancesData({ force: true }),
    [loadAppearancesData],
  );

  const loadBridgeSelection = useCallback(async () => {
    try {
      const profile = await invoke("load_bridge_profile");
      setBridgeSelectedAgentId(profile?.selectedAgentId || profile?.enabledAgents?.[0] || "");
      const bridgeEnabled = bridgeEnabledAgents(profile);
      if (bridgeEnabled) {
        setEnabledAgents(bridgeEnabled);
        saveEnabledAgents(bridgeEnabled);
      }
      return bridgeEnabled;
    } catch (err) {
      console.warn("[DeviceContext] load_bridge_profile failed", err);
      return null;
    }
  }, []);

  const refreshAgents = useCallback(async () => {
    if (agentScanRequestRef.current) return agentScanRequestRef.current;

    setAgentScan((current) => ({ ...current, pending: true, error: "" }));
    const request = (async () => {
      try {
        const res = await invoke("detect_local_agents");
        const next = normalizeDetectedAgents(res?.agents || []);
        const scannedAt = Number(res?.scannedAt || res?.scanned_at) || Date.now();
        setAgentOptions(next);
        setAgentScan({
          pending: false,
          scannedAt,
          detectedCount: next.filter((agent) => agent.detected).length,
          error: "",
        });
        return next;
      } catch (err) {
        const message = err?.message || String(err);
        setAgentScan((current) => ({
          ...current,
          pending: false,
          error: message,
        }));
        console.warn("[DeviceContext] detect_local_agents failed", err);
        throw err;
      } finally {
        lastAgentScanAttemptAtRef.current = Date.now();
      }
    })();

    agentScanRequestRef.current = request;
    try {
      return await request;
    } finally {
      if (agentScanRequestRef.current === request) {
        agentScanRequestRef.current = null;
      }
    }
  }, []);

  useEffect(() => {
    invoke("load_device_bindings").catch(() => null);
    loadAppearancesData().then(() => loadBridgeSelection());
    refreshAgents().catch(() => null);
  }, [loadAppearancesData, loadBridgeSelection, refreshAgents]);

  useEffect(() => {
    const refreshIfStale = () => {
      if (document.visibilityState === "hidden") return;
      const elapsed = Date.now() - lastAgentScanAttemptAtRef.current;
      if (elapsed < AGENT_SCAN_FOCUS_STALE_MS) return;
      refreshAgents().catch(() => null);
    };

    window.addEventListener("focus", refreshIfStale);
    document.addEventListener("visibilitychange", refreshIfStale);
    return () => {
      window.removeEventListener("focus", refreshIfStale);
      document.removeEventListener("visibilitychange", refreshIfStale);
    };
  }, [refreshAgents]);

  // --- Active component updates ---
  // `storage` event fires natively for cross-tab writes. Same-tab writers
  // (e.g. ComponentCenter in Plan 4) must `window.dispatchEvent(new Event("storage"))`
  // after `localStorage.setItem("pet-manager:active-component", ...)` to wake this
  // listener — the native event is cross-tab only.
  useEffect(() => {
    const handler = () => setActiveComponentRevision((revision) => revision + 1);
    window.addEventListener("storage", handler);
    return () => window.removeEventListener("storage", handler);
  }, []);

  const refresh = useCallback(async () => {
    await Promise.all([
      refreshAppearances().then(() => loadBridgeSelection()),
      refreshAgents().catch(() => null),
    ]);
  }, [loadBridgeSelection, refreshAgents, refreshAppearances]);

  const getAppearanceSerialResetRequired = useCallback((expectedBoardDeviceId) => (
    invoke("usb_appearance_serial_reset_required", { expectedBoardDeviceId })
  ), []);

  const recoverAppearanceSerial = useCallback(async (expectedBoardDeviceId) => {
    const status = normalizeUsbStatusPayload(await invoke("usb_recover_appearance_serial", { expectedBoardDeviceId }));
    setUsb(status);
    return status;
  }, []);

  const rescanUsbDevices = useCallback(async () => {
    const devices = await invoke("usb_scan_devices");
    const list = Array.isArray(devices) ? devices : [];
    let connectedStatus = null;
    let lastError = null;
    for (const device of list) {
      const portName = device?.portName;
      if (!portName) continue;
      try {
        const candidateStatus = normalizeUsbStatusPayload(
          await invoke("usb_connect", { portName }),
        );
        if (candidateStatus.connected) {
          connectedStatus = candidateStatus;
          break;
        }
      } catch (error) {
        if (!lastError || String(lastError?.message || lastError) !== "appearance_raw_stream_quarantined_no_probe") {
          lastError = error;
        }
      }
    }
    const nextUsb = connectedStatus || normalizeUsbStatusPayload(await invoke("usb_get_status"));
    setUsb(nextUsb);
    if (!nextUsb.connected && lastError) throw lastError;
    return { devices: list, status: nextUsb };
  }, []);

  const currentDisplay = useMemo(() => {
    const display = deriveCurrentDisplay(agentAppearanceMap, enabledAgents, appearances, agentOptions);
    const verified = wifi && wifiAppearance?.boardDeviceId === wifi.boardDeviceId
      && wifiAppearance.bootId === wifi.bootId;
    if (verified) {
      const retained = appearances.find(record => record.id === wifiAppearance.appearanceId);
      if (retained) return { ...display, appearance: retained };
    }
    return wifi?.appearanceCapacity ? { ...display, appearanceUnverified: true } : display;
  }, [agentAppearanceMap, enabledAgents, appearances, agentOptions, wifi, wifiAppearance]);
  const deviceConnected = usb.connected;

  const applyWifiDesktopPet = useCallback(async (agentId, appearance, { follow = false } = {}) => {
    if (!wifi?.appearanceCapacity) throw new Error("请更新设备固件后再通过 Wi-Fi 更换形象。");
    if (!wifi.controlTrusted) throw new Error("请通过 USB 连接同一台设备一次，授权无线控制后即可通过 Wi-Fi 更换形象。");
    if (!appearance?.id) throw new Error("请选择一个形象。");
    if (appearanceAssignmentOwnerRef.current) throw new Error(FOLLOW_ASSIGNMENT_BUSY_MESSAGE);
    const owner = { boardDeviceId: wifi.boardDeviceId, cancelRequested: false };
    appearanceAssignmentOwnerRef.current = owner;
    setAppearanceSync({ ...EMPTY_APPEARANCE_SYNC, pending: true, agentId,
      appearanceId: appearance.id, appearanceName: appearance.name,
      transport: "wifi", progress: { text: "正在确认设备形象…", indeterminate: true } });
    let unlisten;
    let verified = false;
    try {
      unlisten = await listen("wifi-appearance-progress", ({ payload }) => {
        if (appearanceAssignmentOwnerRef.current !== owner || owner.cancelRequested) return;
        const progress = payload.stage === "activating"
          ? { text: "正在切换设备上的形象…", indeterminate: true }
          : payload.stage === "preparing"
            ? { text: "设备缺少目标形象，正在准备素材传输…", indeterminate: true }
            : {
                percent: Math.min(99, Math.floor(payload.bytesSent / payload.bytesTotal * 100)),
                text: payload.bytesSent === payload.bytesTotal ? "正在校验并激活…" : "正在通过 Wi-Fi 传输形象…",
              };
        setAppearanceSync(current => ({ ...current, progress }));
      });
      if (owner.cancelRequested) throw new Error(APPEARANCE_SYNC_CANCELLED_MESSAGE);
      const outcome = await invoke("lan_sync_appearance", { appearanceId: appearance.id,
        boardDeviceId: wifi.boardDeviceId, bootId: wifi.bootId });
      if (!["alreadyActive", "activated", "uploaded"].includes(outcome)) {
        throw new Error("设备形象结果尚未确认，本次未切换 Agent。");
      }
      verified = true;
      const nextMap = assignAppearanceToAgent(agentAppearanceMap, agentId, appearance.id);
      setWifiAppearance({ boardDeviceId: wifi.boardDeviceId, bootId: wifi.bootId, appearanceId: appearance.id });
      if (owner.cancelRequested && follow) throw new Error("设备形象已确认，已取消后续的 Agent 跟随切换。");
      if (follow) {
        try {
          await invoke("lan_follow_agent", { boardDeviceId: wifi.boardDeviceId, bootId: wifi.bootId, agentId });
        } catch {
          throw new Error("形象已更换，Agent 跟随结果尚未确认，请查看设备当前状态。");
        }
        const enabled = new Set([agentId]);
        setEnabledAgents(enabled);
        setBridgeSelectedAgentId(agentId);
        saveEnabledAgents(enabled);
      }
      setAgentAppearanceMap(nextMap);
      saveAgentAppearanceMap(nextMap);
      if (follow) {
        try {
          const runtime = await invoke("ensure_bridge_runtime", { input: { forceRestart: true } });
          if (runtime?.running !== true) throw new Error("本地桥接服务未就绪");
        } catch {
          throw new Error("Wi-Fi 形象和跟随已确认，但本地桥接服务未就绪。");
        }
      }
      const notice = outcome === "alreadyActive" ? `已确认设备正在使用「${appearance.name}」`
        : outcome === "activated" ? `已切换至设备上的「${appearance.name}」`
          : `已通过 Wi-Fi 传输并激活「${appearance.name}」`;
      return { nextMap, notice };
    } catch (error) {
      if (!verified) {
        setWifiAppearance(current => current?.boardDeviceId === owner.boardDeviceId ? null : current);
      }
      throw error;
    } finally {
      try { unlisten?.(); } catch { /* Keep the device result. */ }
      if (appearanceAssignmentOwnerRef.current === owner) {
        appearanceAssignmentOwnerRef.current = null;
        setAppearanceSync(EMPTY_APPEARANCE_SYNC);
      }
    }
  }, [agentAppearanceMap, wifi]);

  const applyDesktopPet = useCallback(
    async (agentId, appearance, options = {}) => {
      if (wifi) return applyWifiDesktopPet(agentId, appearance, { follow: agentId !== currentDisplay.agentId });
      if (appearanceAssignmentOwnerRef.current) throw new Error(FOLLOW_ASSIGNMENT_BUSY_MESSAGE);
      const { initialProgress, onProgress } = options;
      const syncToken = appearanceSyncTokenRef.current + 1;
      appearanceSyncTokenRef.current = syncToken;
      const owner = {
        token: syncToken,
        intentId: createDesktopPetIntentId(),
        boardDeviceId: usb.boardDeviceId || onlineBoardDeviceId || binding?.boardDeviceId || "",
        connectionId: usb.connectionId || "",
        bootId: usb.bootId || "",
        cancelRequested: false,
      };
      appearanceAssignmentOwnerRef.current = owner;
      const isCurrent = (intentId = owner.intentId) => appearanceAssignmentOwnerRef.current === owner
        && intentId === owner.intentId && !owner.cancelRequested
        && appearanceUsbRef.current.connected === true
        && (!owner.boardDeviceId || appearanceUsbRef.current.boardDeviceId === owner.boardDeviceId)
        && (!owner.connectionId || appearanceUsbRef.current.connectionId === owner.connectionId)
        && (!owner.bootId || appearanceUsbRef.current.bootId === owner.bootId);
      const appearanceSyncMeta = {
        agentId,
        appearanceId: appearance?.id || "",
        appearanceName: appearance?.name || "",
      };
      const emitAppearanceSyncProgress = (progress) => {
        if (appearanceAssignmentOwnerRef.current !== owner) return;
        setAppearanceSync((current) => ({
          pending: true,
          cancelling: current.cancelling,
          progress,
          ...appearanceSyncMeta,
        }));
        onProgress?.(progress);
      };
      const currentAppearanceId = currentDisplay.appearance?.id || "";
      try {
        emitAppearanceSyncProgress({
          ...initialProgress,
          text: "正在确认…",
          stage: "confirming",
          followOnly: true,
          indeterminate: true,
          percent: 0,
        });
        // A fresh click after OTA/replug binds native status, not the previous
        // 3-second UI poll. Once pinned, any later boot/generation change still
        // cancels this intent; never retarget an operation already in progress.
        const freshUsb = normalizeUsbStatusPayload(await invoke("usb_get_status"));
        if (appearanceAssignmentOwnerRef.current !== owner || owner.cancelRequested
          || !freshUsb.connected || !owner.boardDeviceId || freshUsb.boardDeviceId !== owner.boardDeviceId) {
          throw appearanceConfirmationRequired();
        }
        owner.connectionId = freshUsb.connectionId;
        owner.bootId = freshUsb.bootId;
        appearanceUsbRef.current = freshUsb;
        setUsb(freshUsb);
        const { nextMap, notice } = await applyDesktopPetAssignment({
          invoke,
          listen,
          agentAppearanceMap,
          agentId,
          appearance,
          agentOptions,
          boardDeviceId: owner.boardDeviceId,
          currentAppearanceId,
          followIntentId: owner.intentId,
          onProgress: emitAppearanceSyncProgress,
          isCurrent,
        });
        setAgentAppearanceMap(nextMap);
        setWifiAppearance(null);
        const enabled = new Set(assignedAgentIds(nextMap, agentId));
        setEnabledAgents(enabled);
        setBridgeSelectedAgentId(agentId);
        saveAgentAppearanceMap(nextMap);
        saveEnabledAgents(enabled);
        setAppearanceRecovery(current => current?.boardDeviceId === owner.boardDeviceId ? null : current);
        return { nextMap, notice };
      } catch (error) {
        if (error?.code === "appearance-confirmation-required" || error?.message === APPEARANCE_SYNC_CANCELLED_MESSAGE) {
          setAppearanceRecovery({
            boardDeviceId: owner.boardDeviceId,
            phase: "needed",
            checking: false,
            message: "暂时无法确认设备状态，本次未切换。请检查连接后再试。",
          });
        }
        // Profile persistence already completed in this explicitly reported
        // partial outcome. Mirror it, while callers still show the bridge error.
        const committed = error?.committedAssignment;
        if (committed && appearanceAssignmentOwnerRef.current === owner) {
          setAgentAppearanceMap(committed.nextMap);
          setWifiAppearance(null);
          setEnabledAgents(new Set(committed.enabledAgents));
          setBridgeSelectedAgentId(committed.agentId);
        }
        throw error;
      } finally {
        if (appearanceAssignmentOwnerRef.current === owner) appearanceAssignmentOwnerRef.current = null;
        if (appearanceSyncTokenRef.current === syncToken) {
          setAppearanceSync({
            pending: false,
            cancelling: false,
            progress: null,
            ...appearanceSyncMeta,
          });
        }
      }
    },
    [agentAppearanceMap, agentOptions, applyWifiDesktopPet, binding, currentDisplay, onlineBoardDeviceId, usb.boardDeviceId, usb.connectionId, usb.bootId, wifi],
  );

  const followAgent = useCallback(async (agentId, appearance, options) => {
    if (usb.connected) return applyDesktopPet(agentId, appearance, options);
    if (!wifi) throw new Error("请连接 USB，或先与当前设备完成 Wi-Fi 配对。");
    if (wifi.appearanceCapacity) {
      return applyWifiDesktopPet(agentId, appearance, { follow: true });
    }
    if (appearanceAssignmentOwnerRef.current) throw new Error(FOLLOW_ASSIGNMENT_BUSY_MESSAGE);
    const owner = { boardDeviceId: wifi.boardDeviceId };
    appearanceAssignmentOwnerRef.current = owner;
    setAppearanceSync({ ...EMPTY_APPEARANCE_SYNC, pending: true, agentId,
      progress: { text: "正在通过 Wi-Fi 提交跟随…", followOnly: true } });
    try {
      await invoke("lan_follow_agent", { ...wifi, agentId });
      if (appearanceAssignmentOwnerRef.current !== owner) return {};
      const enabled = new Set([agentId]);
      setEnabledAgents(enabled);
      setBridgeSelectedAgentId(agentId);
      saveEnabledAgents(enabled);
      if (currentDisplay.appearance?.id) {
        setWifiAppearance({ boardDeviceId: wifi.boardDeviceId, bootId: wifi.bootId,
          appearanceId: currentDisplay.appearance.id });
      }
      try {
        const runtime = await invoke("ensure_bridge_runtime", { input: { forceRestart: true } });
        if (runtime?.running !== true) throw new Error("本地桥接服务未就绪");
      } catch {
        throw new Error("Wi-Fi 跟随已提交，桌面配置已保存，但本地桥接服务未就绪。");
      }
      return { notice: "已通过 Wi-Fi 提交跟随，设备继续使用当前形象。" };
    } finally {
      if (appearanceAssignmentOwnerRef.current === owner) {
        appearanceAssignmentOwnerRef.current = null;
        setAppearanceSync(EMPTY_APPEARANCE_SYNC);
      }
    }
  }, [applyDesktopPet, applyWifiDesktopPet, currentDisplay.appearance?.id, usb.connected, wifi]);

  const cancelAppearanceSync = useCallback(async () => {
    if (!appearanceSync.pending || appearanceSync.cancelling) {
      return { requested: false };
    }
    if (appearanceAssignmentOwnerRef.current) appearanceAssignmentOwnerRef.current.cancelRequested = true;
    const text = appearanceSync.transport === "wifi" ? "正在中断 Wi-Fi 形象传输…" : "正在中断 USB 形象传输…";
    setAppearanceSync(current => ({
      ...current,
      cancelling: true,
      progress: { ...current.progress, text },
    }));
    try {
      const result = await invoke("usb_cancel_appearance_sync");
      if (!result?.requested) {
        setAppearanceSync((current) => ({ ...current, cancelling: false }));
      }
      return result;
    } catch (error) {
      setAppearanceSync((current) => ({ ...current, cancelling: false }));
      throw error;
    }
  }, [appearanceSync.cancelling, appearanceSync.pending, appearanceSync.transport]);

  const saveAgentAppearance = useCallback((agentId, appearanceId) => {
    const nextMap = assignAppearanceToAgent(agentAppearanceMap, agentId, appearanceId);
    setAgentAppearanceMap(nextMap);
    saveAgentAppearanceMap(nextMap);
    return nextMap;
  }, [agentAppearanceMap]);

  const setBinding = useCallback(
    (next) => {
      setBindingState(next);
      onBindingChange?.(next);
    },
    [onBindingChange],
  );

  const realtimeTargetRef = useRef({ appearance: null, boardDeviceId: "" });
  const componentVoiceSync = useRef(null);
  if (!componentVoiceSync.current) componentVoiceSync.current = createComponentVoiceSync(invoke);
  useEffect(() => {
    const input = componentPetVoice(currentDisplay.appearance, usb);
    const sync = () => componentVoiceSync.current(input);
    void sync();
    const timer = setInterval(sync, 5000);
    return () => { clearInterval(timer); void componentVoiceSync.current(null); };
  }, [currentDisplay.appearance, usb.boardDeviceId, usb.connected]);
  useEffect(() => {
    const onPersonaSaved = (event) => {
      const { appearanceId, personaVoice } = event.detail || {};
      if (!appearanceId || !personaVoice) return;
      setAppearances((records) => records.map((record) => record.id === appearanceId ? { ...record, personaVoice } : record));
      const target = realtimeTargetRef.current;
      if (target.appearance?.id === appearanceId) {
        realtimeTargetRef.current = { ...target, appearance: { ...target.appearance, personaVoice } };
      }
    };
    window.addEventListener(PERSONA_VOICE_UPDATED_EVENT, onPersonaSaved);
    return () => window.removeEventListener(PERSONA_VOICE_UPDATED_EVENT, onPersonaSaved);
  }, []);
  useEffect(() => {
    realtimeTargetRef.current = {
      appearance: currentDisplay.appearance || null,
      boardDeviceId: usb.connected ? String(usb.boardDeviceId || "").trim() : "",
    };
  }, [currentDisplay.appearance, usb.boardDeviceId, usb.connected]);

  const startRealtimeChat = useCallback(async (options = {}) => {
    const target = realtimeTargetRef.current;
    const selectedAppearance = options.appearance || target.appearance;
    const appearance = selectedAppearance ? await getAppearance(selectedAppearance.id) : null;
    const boardDeviceId = String(options.boardDeviceId || target.boardDeviceId || await readRealtimeWifiBoard() || "").trim();
    if (!appearance) throw new Error("设备上还没有形象，先在画廊里应用一个形象");
    if (!appearance.personaVoice?.configured) {
      throw new Error(`「${appearance.name}」还没有设置人设与声音，先在画廊卡片右下角设置`);
    }
    if (!boardDeviceId) throw new Error("请通过 USB 连接设备，或在设置中完成 Wi-Fi 配对。");
    if (options.isCurrent && !options.isCurrent()) throw new Error("对话请求已取消");
    realtimeReconcilerRef.current?.invalidate();
    const status = options.lanPress
      ? await invoke("lan_realtime_toggle", { token: options.lanPress, input: buildRealtimeChatStartInput(appearance, boardDeviceId) })
      : await invoke("realtime_chat_start", { input: buildRealtimeChatStartInput(appearance, boardDeviceId) });
    realtimeReconcilerRef.current?.invalidate();
    setRealtimeChat((current) => mergeStatus(current, { ...status, error: "" }));
    return status;
  }, []);

  const stopRealtimeChat = useCallback(async (reason = "ui") => {
    realtimeReconcilerRef.current?.invalidate();
    const status = await invoke("realtime_chat_stop", { reason });
    realtimeReconcilerRef.current?.invalidate();
    setRealtimeChat((current) => mergeStatus(current, status));
    return status;
  }, []);

  useEffect(() => {
    let disposed = false;
    const unlisteners = [];
    const reconciler = createStatusReconciler();
    realtimeReconcilerRef.current = reconciler;
    const pollStatus = () => reconciler.poll(
      () => invoke("realtime_chat_status"),
      status => setRealtimeChat(current => mergeStatus(current, status)),
    ).catch(() => {});
    pollStatus();
    listen("realtime-chat-state", (event) => {
      const status = event?.payload;
      if (!disposed && status && typeof status === "object") {
        reconciler.invalidate();
        setRealtimeChat((current) => mergeStatus(current, status));
      }
    }).then((stop) => { if (disposed) stop(); else unlisteners.push(stop); }).catch(() => {});
    // Recover the final error even if a fast failure raced listener registration.
    const refresh = setInterval(pollStatus, 2000);
    const lanPress = createRealtimePressHandler({
      accept: token => invoke("lan_realtime_toggle", { token, input: null }),
      configure: (token, isCurrent) => startRealtimeChat({ boardDeviceId: token.boardDeviceId, lanPress: token, isCurrent }),
      onError: error => setRealtimeChat(current => ({ ...current, error: error?.message || String(error) })),
    });
    // 板端「实时对话」键：Rust 不知道设备上是哪个形象，由这里用当前形象拉起会话。
    listen("realtime-chat-request", (event) => {
      const boardDeviceId = String(event?.payload?.boardDeviceId || "").trim();
      startRealtimeChat({ boardDeviceId }).catch((error) => {
        reconciler.invalidate();
        if (disposed) return;
        const message = error?.message || String(error);
        setRealtimeChat((current) => ({ ...current, active: false, state: "idle", error: message }));
        invoke("usb_send_speech", { text: message.slice(0, 60) }).catch(() => {});
      });
    }).then(async (stop) => {
      if (disposed) { stop(); return; }
      unlisteners.push(stop);
      // Install key acceptance only after realtime UI handling is registered.
      const stopLan = await listen("lan-realtime-press", lanPress.handle);
      if (disposed) stopLan(); else unlisteners.push(stopLan);
    }).catch(() => {});
    return () => {
      disposed = true;
      lanPress.dispose();
      reconciler.dispose();
      if (realtimeReconcilerRef.current === reconciler) realtimeReconcilerRef.current = null;
      clearInterval(refresh);
      unlisteners.forEach((stop) => { try { stop(); } catch { /* ignore */ } });
    };
  }, [startRealtimeChat]);

  const value = useMemo(
    () => ({
      binding,
      setBinding,
      usb,
      wifi,
      activeBoardDeviceId,
      realtimeWifiReady,
      deviceOnline,
      onlineBoardDeviceId,
      deviceConnected,
      appearances,
      agentAppearanceMap,
      enabledAgents,
      selectedAgentId: bridgeSelectedAgentId,
      agentOptions,
      agentScan,
      currentDisplay,
      currentComponent,
      currentComponentTarget,
      appearanceSync,
      appearanceRecovery,
      realtimeChat,
      usageHelp,
      setUsageHelp,
      startRealtimeChat,
      stopRealtimeChat,
      applyDesktopPet,
      followAgent,
      cancelAppearanceSync,
      saveAgentAppearance,
      rescanUsbDevices,
      recoverAppearanceSerial,
      getAppearanceSerialResetRequired,
      refreshAgents,
      refreshAppearances,
      refresh,
    }),
    [
      binding,
      setBinding,
      usb,
      wifi,
      activeBoardDeviceId,
      realtimeWifiReady,
      deviceOnline,
      onlineBoardDeviceId,
      deviceConnected,
      appearances,
      agentAppearanceMap,
      enabledAgents,
      bridgeSelectedAgentId,
      agentOptions,
      agentScan,
      currentDisplay,
      currentComponent,
      currentComponentTarget,
      appearanceSync,
      appearanceRecovery,
      realtimeChat,
      usageHelp,
      startRealtimeChat,
      stopRealtimeChat,
      applyDesktopPet,
      followAgent,
      cancelAppearanceSync,
      saveAgentAppearance,
      rescanUsbDevices,
      recoverAppearanceSerial,
      getAppearanceSerialResetRequired,
      refreshAgents,
      refreshAppearances,
      refresh,
    ],
  );

  return <DeviceContext.Provider value={value}>{children}</DeviceContext.Provider>;
}

export function useDeviceContext() {
  const ctx = useContext(DeviceContext);
  if (!ctx) throw new Error("useDeviceContext must be used inside <DeviceContextProvider>");
  return ctx;
}

export function useUsageHelp() {
  return useContext(DeviceContext)?.usageHelp || EMPTY_USAGE_HELP;
}
