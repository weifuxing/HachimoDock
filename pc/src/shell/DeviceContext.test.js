/**
 * [Input] Read DeviceContext.jsx source + runtime-import deriveCurrentDisplay.
 * [Output] Static + runtime Node coverage that the USB-only provider defaults only an unconfigured first run to Codex, exposes the documented context shape, deduplicates Agent rescans and refreshes them when a focused client is stale, force-refreshes appearance records on manual refresh, hydrates the active channel from the bridge profile, polls USB status single-flight without unchanged Context churn or owning serial auto-connect/network availability state, offers a manual USB serial rescan action, and the pure derivation reflects the active desktop assignment.
 * [Pos] test node in pc/src/shell
 * [Sync] If this file changes, update `pc/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import {
  deriveCurrentDisplay,
  deriveDeviceReachability,
  normalizeUsbStatusPayload,
  pairedWifiTarget,
  pairedWifiTargetsEqual,
  resolveUsbRuntime,
  usbStatusSnapshotsEqual,
} from "./DeviceContext.pure.js";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DeviceContext.jsx"), "utf8");

const APPEARANCES = [
  { id: "ap-a", name: "Terrier" },
  { id: "ap-b", name: "Westie" },
];
const AGENTS = [
  { id: "codex", label: "ChatGPT（Codex）", detected: true },
  { id: "claude-code", label: "Claude", detected: true },
];

test("deriveCurrentDisplay returns the active assignment with appearance + label", () => {
  const out = deriveCurrentDisplay(
    { codex: "ap-a" },
    new Set(["codex"]),
    APPEARANCES,
    AGENTS,
  );
  assert.equal(out.agentId, "codex");
  assert.equal(out.appearance?.id, "ap-a");
  assert.equal(out.channelLabel, "ChatGPT（Codex）");
});

test("deriveCurrentDisplay returns null appearance when map is empty", () => {
  const out = deriveCurrentDisplay({}, new Set(), APPEARANCES, AGENTS);
  assert.equal(out.agentId, "");
  assert.equal(out.appearance, null);
});

test("deriveCurrentDisplay falls back to the first mapped agent when no enabled set", () => {
  const out = deriveCurrentDisplay(
    { codex: "ap-b" },
    null,
    APPEARANCES,
    AGENTS,
  );
  assert.equal(out.agentId, "codex");
  assert.equal(out.appearance?.id, "ap-b");
});

test("provider source exposes useDeviceContext and DeviceContextProvider", () => {
  assert.match(source, /export function DeviceContextProvider\s*\(/);
  assert.match(source, /export function useDeviceContext\s*\(/);
});

test("provider selects Codex only when no local or Bridge choice exists", () => {
  assert.match(source, /useState\(\s*\(\)\s*=>\s*new Set\(\[DEFAULT_AGENT_ID\]\)/);
  assert.match(
    source,
    /const enabled = loadEnabledAgents\(\) \|\| new Set\(\[DEFAULT_AGENT_ID\]\)/,
  );
  assert.match(source, /if \(bridgeEnabled\) \{[\s\S]*setEnabledAgents\(bridgeEnabled\)/);
});

test("provider centralizes the documented polling and bridge invocations", () => {
  // No new Tauri commands — strictly re-uses existing ones from the dashboards.
  for (const command of [
    "usb_get_status",
    "usb_scan_devices",
    "usb_connect",
    "load_bridge_profile",
    "load_device_bindings",
    "detect_local_agents",
  ]) {
    assert.match(source, new RegExp(`["']${command}["']`), `expected provider to invoke ${command}`);
  }
});

test("provider hydrates the active channel from bridge profile instead of stale localStorage only", () => {
  assert.match(source, /const\s+loadBridgeSelection\s*=\s*useCallback\(\s*async\s*\(\)\s*=>/);
  assert.match(source, /invoke\(["']load_bridge_profile["']\)/);
  assert.match(source, /profile\?\.selectedAgentId/);
  assert.match(source, /profile\?\.enabledAgents/);
  assert.match(source, /setEnabledAgents\(bridgeEnabled\)/);
  assert.match(source, /saveEnabledAgents\(bridgeEnabled\)/);
});

test("provider polls USB status but does not auto-connect inside the polling effect", () => {
  const pollEffect = source.match(/\/\/ --- USB status poll[\s\S]*?\n  \}, \[[^\]]*\]\);/);
  assert.ok(pollEffect, "expected USB status poll effect");
  assert.match(pollEffect[0], /usb_get_status/);
  assert.doesNotMatch(pollEffect[0], /["']usb_scan_devices["']/);
  assert.doesNotMatch(pollEffect[0], /["']usb_connect["']/);
  assert.doesNotMatch(source, /usb auto-connect failed/);
});

test("provider obtains Wi-Fi reachability from the authenticated paired status", () => {
  assert.match(source, /invoke\("lan_device_status"\)/);
  assert.match(source, /usb\.connected \? null : wifiStatus/);
  assert.match(source, /target = pairedWifiTarget\(await invoke\("lan_device_status"\)\)/);
  assert.match(source, /activeBoardDeviceId = onlineBoardDeviceId \|\| binding\?\.boardDeviceId/);
  assert.match(source, /deriveDeviceReachability\(\{ usb, wifi \}\)/);
  assert.match(source, /if \(usb.connected\) return applyDesktopPet\(agentId, appearance, options\)/);
  assert.match(source, /invoke\("lan_follow_agent", \{ \.\.\.wifi, agentId \}\)/);
});

test("USB status normalization keeps P4 capability metadata", () => {
  const out = normalizeUsbStatusPayload({
    connected: true,
    portName: "COM15",
    baudRate: 4_000_000,
    boardDeviceId: "p4-devkit-001",
    runtime: "esp-p4",
    deviceModel: "ESP32-P4",
    firmware: "0.1.0-p4",
    buildId: "0.1.0-p4+abc123-dirty",
    gitSha: "abc123",
    buildDirty: true,
    protocolSchema: 4,
    wireProtocol: "pet-usb-jsonl-v2",
    capabilities: { usbOnly: true },
  });

  assert.equal(out.connected, true);
  assert.equal(out.runtime, "esp-p4");
  assert.equal(out.baudRate, 4_000_000);
  assert.equal(out.capabilities.usbOnly, true);
  assert.equal(out.buildId, "0.1.0-p4+abc123-dirty");
  assert.equal(out.gitSha, "abc123");
  assert.equal(out.buildDirty, true);
  assert.equal(out.protocolSchema, 4);
});

test("USB status equality suppresses unchanged snapshots but preserves meaningful changes", () => {
  const first = normalizeUsbStatusPayload({
    connected: true,
    portName: "COM15",
    baudRate: 4_000_000,
    boardDeviceId: "p4-devkit-001",
    runtime: "esp-p4",
    firmware: "0.1.0-p4",
    capabilities: { appearance: { formats: ["p4-h264-v1"] } },
  });
  const same = normalizeUsbStatusPayload({
    connected: true,
    portName: "COM15",
    baudRate: 4_000_000,
    boardDeviceId: "p4-devkit-001",
    runtime: "esp-p4",
    firmware: "0.1.0-p4",
    capabilities: { appearance: { formats: ["p4-h264-v1"] } },
  });

  assert.equal(usbStatusSnapshotsEqual(first, same), true);
  assert.equal(usbStatusSnapshotsEqual(first, { ...same, firmware: "0.1.1-p4" }), false);
  assert.equal(usbStatusSnapshotsEqual(first, { ...same, buildId: "0.1.0-p4+other" }), false);
});

test("provider keeps USB status polling single-flight and reuses unchanged state", () => {
  const pollEffect = source.match(/\/\/ --- USB status poll[\s\S]*?\n  \}, \[[^\]]*\]\);/);
  assert.ok(pollEffect, "expected USB status poll effect");
  assert.match(pollEffect[0], /if \(cancelled \|\| inFlight\) return/);
  assert.match(pollEffect[0], /inFlight = true/);
  assert.match(pollEffect[0], /finally\s*\{[\s\S]*inFlight = false/);
  assert.match(pollEffect[0], /usbStatusSnapshotsEqual\(currentUsb, nextUsb\) \? currentUsb : nextUsb/);
});

test("USB runtime comes from handshake metadata and never from a COM number", () => {
  assert.equal(resolveUsbRuntime({ portName: "COM3" }), "");
  assert.equal(resolveUsbRuntime({ portName: "COM5" }), "");
  assert.equal(resolveUsbRuntime({ deviceModel: "ESP32-P4 RISC-V Dual-Core" }), "esp-p4");
  assert.equal(resolveUsbRuntime({
    capabilities: { appearance: { formats: ["p4-mjpeg-v1"] } },
  }), "esp-p4");
  assert.equal(resolveUsbRuntime({ runtime: "radxa" }), "linux");
});

test("P4 reachability prefers USB and uses authenticated Wi-Fi when USB disconnects", () => {
  const usb = normalizeUsbStatusPayload({
    connected: true,
    boardDeviceId: "p4-devkit-001",
    runtime: "esp-p4",
    capabilities: { usbOnly: true },
  });
  const connected = deriveDeviceReachability({
    usb,
  });
  assert.equal(connected.deviceOnline, true);
  assert.equal(connected.onlineBoardDeviceId, "p4-devkit-001");

  const disconnected = deriveDeviceReachability({
    usb: { ...usb, connected: false, boardDeviceId: "" },
  });
  assert.equal(disconnected.deviceOnline, false);
  assert.equal(disconnected.onlineBoardDeviceId, "");
  const status = { boardDeviceId: "p4-123456789abc", wifiConnected: true, bootId: "0123456789abcdef" };
  assert.equal(pairedWifiTarget({ ...status, boardDeviceId: "p4-other" }), null);
  assert.equal(pairedWifiTarget({ ...status, wifiConnected: false }), null);
  assert.equal(pairedWifiTarget({ ...status, bootId: "" }), null);
  const wifi = pairedWifiTarget(status);
  const wifiCapabilities = { trustedVolume: true, controlTrusted: true, wirelessOta: true, componentDelivery: true, appearanceCapacity: 8388608, panelControl: true };
  assert.deepEqual(wifi, { boardDeviceId: status.boardDeviceId, bootId: status.bootId,
    address: "", firmware: "", bundledFirmwareVersion: "", bundledFirmwareRollbackBlocked: false, wifiConnected: true,
    clockSynchronized: false, rebootSupported: false, wirelessRealtime: false, wirelessVoiceInput: false,
    trustedVolume: false, controlTrusted: false, wirelessOta: false, componentDelivery: false,
    appearanceCapacity: 0, inputDoublePress: false, systemSettings: false, panelControl: false });
  assert.deepEqual(pairedWifiTarget({ ...status, ...wifiCapabilities }), { ...wifi, ...wifiCapabilities });
  assert.deepEqual(deriveDeviceReachability({ usb, wifi }), connected);
  assert.deepEqual(deriveDeviceReachability({ usb: { connected: false }, wifi }), {
    deviceOnline: true, onlineBoardDeviceId: status.boardDeviceId,
  });
});

test("Wi-Fi status reconciliation preserves version and recovery information", () => {
  const status = { boardDeviceId: "p4-123456789abc", bootId: "0123456789abcdef", wifiConnected: true,
    address: "192.0.2.12", firmware: "0.7.103-p4-r285-wifi6", bundledFirmwareVersion: "0.7.103-p4-r286-wifi6" };
  const old = pairedWifiTarget(status);
  assert.equal(old.address, status.address);
  assert.equal(old.firmware, status.firmware);
  assert.equal(old.bundledFirmwareVersion, status.bundledFirmwareVersion);
  assert.equal(pairedWifiTargetsEqual(old, pairedWifiTarget({ ...status, bundledFirmwareRollbackBlocked: true })), false);
  assert.equal(pairedWifiTargetsEqual(old, pairedWifiTarget({ ...status })), true);
  assert.equal(pairedWifiTargetsEqual(old, pairedWifiTarget({ ...status, firmware: status.bundledFirmwareVersion })), false);
  assert.equal(pairedWifiTargetsEqual(old, pairedWifiTarget({ ...status, address: "192.0.2.13" })), false);
  assert.equal(pairedWifiTargetsEqual(old, null), false);
});

test("authenticated Wi-Fi keeps double-press capability and recognizes released WiFi6 system settings", () => {
  const status = { boardDeviceId: "p4-123456789abc", bootId: "0123456789abcdef", wifiConnected: true,
    firmware: "0.7.103-p4-r284-wifi6", inputDoublePress: true };
  assert.equal(pairedWifiTarget(status).inputDoublePress, true);
  assert.equal(pairedWifiTarget(status).systemSettings, true);
  assert.equal(pairedWifiTarget({ ...status, systemSettings: false }).systemSettings, false);
  assert.equal(pairedWifiTarget({ ...status, firmware: "0.7.103-p4-r284-pico" }).systemSettings, false);
  assert.equal(pairedWifiTarget({ ...status, firmware: "0.7.103-p4-r284" }).systemSettings, false);
  assert.equal(pairedWifiTarget({ ...status, wifiConnected: false }), null);
});

test("provider hydrates followed channels locally without network availability reconciliation", () => {
  assert.match(source, /const\s+\[bridgeSelectedAgentId,\s*setBridgeSelectedAgentId\]/);
  assert.match(source, /setBridgeSelectedAgentId\(profile\?\.selectedAgentId/);
  assert.doesNotMatch(source, /check_device_availability/);
});

test("provider exposes manual USB serial rescan and connect action", () => {
  assert.match(source, /const\s+rescanUsbDevices\s*=\s*useCallback\(\s*async\s*\(\)\s*=>/);
  assert.match(source, /invoke\(["']usb_scan_devices["']\)/);
  assert.match(source, /invoke\(["']usb_connect["'],\s*\{\s*portName(?:\s*:|\s*\})/);
  assert.match(source, /for\s*\(const device of list\)/);
  assert.match(source, /candidateStatus\.connected/);
  assert.match(source, /rescanUsbDevices/);
});

test("provider exposes a focused force-refresh for immediate picker use", () => {
  assert.match(source, /const\s+loadAppearancesData\s*=\s*useCallback\(\s*async\s*\(\{\s*force\s*=\s*false\s*\}\s*=\s*\{\}\)\s*=>/);
  assert.match(source, /listAppearances\(\{\s*force\s*\}\)/);
  assert.match(source, /const\s+refreshAppearances\s*=\s*useCallback\([\s\S]*loadAppearancesData\(\{\s*force:\s*true\s*\}\)/);
  assert.match(source, /refreshAppearances,\s*\n\s*refresh,/);
});

test("provider exposes a deduplicated Agent refresh with low-frequency focus-based stale scanning", () => {
  assert.match(source, /const\s+AGENT_SCAN_FOCUS_STALE_MS\s*=\s*5\s*\*\s*60_000/);
  assert.match(source, /const\s+\[agentScan,\s*setAgentScan\]\s*=\s*useState/);
  assert.match(source, /agentScanRequestRef/);
  assert.match(source, /const\s+refreshAgents\s*=\s*useCallback\(\s*async\s*\(\)\s*=>/);
  assert.match(source, /if\s*\(agentScanRequestRef\.current\)\s*return\s+agentScanRequestRef\.current/);
  assert.match(source, /window\.addEventListener\("focus",\s*refreshIfStale\)/);
  assert.match(source, /document\.addEventListener\("visibilitychange",\s*refreshIfStale\)/);
  assert.match(source, /elapsed\s*<\s*AGENT_SCAN_FOCUS_STALE_MS/);
  assert.doesNotMatch(source, /setInterval\(refreshAgents/);
});

test("provider exposes the documented context shape fields", () => {
  for (const field of [
    "binding",
    "usb",
    "deviceOnline",
    "onlineBoardDeviceId",
    "deviceConnected",
    "appearances",
    "agentAppearanceMap",
    "enabledAgents",
    "agentOptions",
    "agentScan",
    "currentDisplay",
    "currentComponent",
    "currentComponentTarget",
    "appearanceSync",
    "applyDesktopPet",
    "cancelAppearanceSync",
    "rescanUsbDevices",
    "refreshAgents",
    "refresh",
  ]) {
    assert.match(source, new RegExp(`\\b${field}\\b`), `expected context field ${field}`);
  }
});

test("provider reuses applyDesktopPetAssignment from lib (does not re-implement)", () => {
  assert.match(source, /from\s+["'][^"']*desktop-pet-assignment[^"']*["']/);
  assert.match(source, /applyDesktopPetAssignment\(/);
});

test("provider owns appearance USB sync progress so dashboard tab switches keep the task visible", () => {
  assert.match(source, /const\s+\[appearanceSync,\s*setAppearanceSync\]\s*=\s*useState/);
  assert.match(source, /appearanceSyncTokenRef/);
  assert.match(source, /setAppearanceSync\(\(current\)\s*=>\s*\(\{\s*pending:\s*true/);
  assert.match(
    source,
    /setAppearanceSync\(\{\s*pending:\s*false,\s*cancelling:\s*false,\s*progress:\s*null/,
  );
  assert.match(source, /onProgress\?\.\(progress\)/);
});

test("provider exposes a persistent appearance USB cancellation action", () => {
  assert.match(source, /const\s+cancelAppearanceSync\s*=\s*useCallback/);
  assert.match(source, /invoke\("usb_cancel_appearance_sync"\)/);
  assert.match(source, /cancelling:\s*true/);
  assert.match(source, /正在中断 USB 形象传输/);
});

test("provider resolves currentComponent through the exact USB or SSH target", () => {
  assert.match(source, /readActiveComponentForTarget/);
  assert.match(source, /readConfiguredComponentSshHost/);
  assert.match(source, /const currentComponentTarget = useMemo/);
  assert.match(source, /transport: "usb", boardDeviceId/);
  assert.match(source, /transport: "ssh", sshHost/);
  assert.match(source, /readActiveComponentForTarget\(currentComponentTarget\)/);
});


test("USB identity retains native decimal u64 and verified boot IDs without local substitution", () => {
 const raw={connected:true,runtime:"esp-p4",boardDeviceId:"board",connectionId:"18446744073709551615",bootId:"0011223344556677"};
 const before=normalizeUsbStatusPayload(raw);
 assert.equal(before.connectionId,raw.connectionId);assert.equal(before.bootId,raw.bootId);
 assert.equal(usbStatusSnapshotsEqual(before,normalizeUsbStatusPayload({...raw,connectionId:"18446744073709551614"})),false);
 assert.equal(usbStatusSnapshotsEqual(before,normalizeUsbStatusPayload({...raw,bootId:"8899aabbccddeeff"})),false);
 assert.equal(usbStatusSnapshotsEqual(before,normalizeUsbStatusPayload(raw)),true);
});


test("supported Wi-Fi follows always verify the native pack before following the Agent", () => {
  const flow = source.slice(source.indexOf("const applyWifiDesktopPet"), source.indexOf("const applyDesktopPet"));
  assert.ok(flow.indexOf('invoke("lan_sync_appearance"') < flow.indexOf('invoke("lan_follow_agent"'));
  assert.match(flow, /\["alreadyActive", "activated", "uploaded"\]\.includes\(outcome\)/);
  const follow = source.slice(source.indexOf("const followAgent"), source.indexOf("const cancelAppearanceSync"));
  assert.match(follow, /if \(wifi\.appearanceCapacity\) \{\s*return applyWifiDesktopPet\(agentId, appearance, \{ follow: true \}\)/);
  assert.doesNotMatch(follow, /appearance\?\.id !== currentDisplay\.appearance/);
});
