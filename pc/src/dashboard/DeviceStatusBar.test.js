/**
 * [Input] Read DeviceStatusBar.jsx source.
 * [Output] Static Node coverage for the P4 status bar's USB and Wi-Fi states and manual serial rescan.
 * [Pos] test node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DeviceStatusBar.jsx"), "utf8");

test("DeviceStatusBar exports a default React component", () => {
  assert.match(source, /export default function DeviceStatusBar\s*\(/);
});

test("DeviceStatusBar consumes useDeviceContext (no local polling)", () => {
  assert.match(source, /useDeviceContext\(/);
  assert.doesNotMatch(source, /invoke\(/);
  assert.doesNotMatch(source, /usb_get_status/);
  assert.doesNotMatch(source, /check_device_availability/);
});

test("DeviceStatusBar exposes a manual USB serial rescan button", () => {
  assert.match(source, /rescanUsbDevices/);
  assert.match(source, /重新扫描串口/);
  assert.match(source, /handleRescanUsb/);
});

test("DeviceStatusBar renders USB, paired Wi-Fi, and offline states in priority order", () => {
  assert.match(source, /usb\.connected \? "已USB连接" : wifi \? "已WIFI连接" : "未连接客户端"/);
  assert.match(source, /当前 Wi-Fi 配对设备/);
  assert.match(source, /onClick=\{onOpenDeviceSettings\}/);
  assert.match(source, /dashboard-status-bar__chips/);
});

test("DeviceStatusBar reads the board id and tolerates the legacy USB port field", () => {
  assert.match(source, /\{activeBoardDeviceId\}/);
  assert.match(source, /binding\.wifiSsid/);
  assert.match(source, /legacyTransportLabel/);
});

test("DeviceStatusBar uses the documented class names", () => {
  assert.match(source, /className="dashboard-status-bar/);
  assert.match(source, /dashboard-status-bar__chip/);
  assert.match(source, /dashboard-status-bar__scan-btn/);
});
