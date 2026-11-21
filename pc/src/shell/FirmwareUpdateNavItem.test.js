/**
 * [Input] FirmwareUpdateNavItem.jsx and App.jsx source.
 * [Output] Static coverage for sidebar placement, version-gated Update/Up-to-date
 *          states, bundled OTA command, progress, and no detail-page routing.
 * [Pos] test node in pc/src/shell.
 * [Sync] If this file changes, update `pc/src/shell/.folder.md` and `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "FirmwareUpdateNavItem.jsx"), "utf8");
const app = readFileSync(join(here, "..", "App.jsx"), "utf8");

test("firmware module sits directly below API configuration without a page route", () => {
  assert.match(app, /title="其它设置"[\s\S]*?<FirmwareUpdateNavItem usb=\{usb\} wifi=\{wifi\} \/>/);
  assert.doesNotMatch(source, /onOpenWirelessMaintenance|setView\(/);
  assert.doesNotMatch(app, /view === "firmware"|setView\("firmware"\)/);
});

test("firmware module exposes Update only for older devices and Up to date otherwise", () => {
  assert.match(source, /firmwareUpdateDisposition\(currentVersion, bundledFirmware\?\.version \|\| currentCompletion\?\.version\)/);
  assert.match(source, /disposition === "update"[\s\S]*"更新"/);
  assert.match(source, /disposition === "latest"[\s\S]*已最新/);
  assert.match(source, /usb_get_bundled_firmware_info/);
  assert.match(source, /usb_update_bundled_firmware/);
  assert.match(source, /usb-firmware-update-progress/);
});


test("a validated OTA closes a stale board-choice prompt", () => {
  assert.match(source, /requiresBoardChoice = currentProbe\?\.info === null && !awaitingBoardProbe && !currentCompletion/);
  assert.match(source, /connectionId: result\?\.connectionId/);
});

test("Wi-Fi shares the firmware comparison and suppresses the action when already current", () => {
  assert.match(source, /firmwareUpdateDisposition\(wifiVersion, wifi\.bundledFirmwareVersion\)/);
  assert.match(source, /wifiUpdate = !usb\.connected && wifi\?\.wirelessOta && wifiDisposition === "update"/);
  assert.match(source, /wifiDisposition === "latest"[\s\S]*?已最新/);
  assert.match(source, /invoke\("lan_update_bundled_firmware", \{\s*expectedBoardDeviceId: wifiScope.boardDeviceId,\s*expectedBootId: wifiScope.previousBootId/);
  assert.match(source, /lan-firmware-update-progress/);
  assert.doesNotMatch(source, /Wi-Fi 升级/);
});

test("sidebar uses the current native version and handles already-latest as a refresh", () => {
  assert.match(source, /bundledFirmware\?\.currentVersion \|\| firmware/);
  assert.match(source, /if \(firmwareAlreadyLatest\(error\)\) \{[\s\S]*?setProbeRefresh[\s\S]*?tone: "info"[\s\S]*?return;/);
});

test("confirmed rollback hides the OTA action and remains visible after reloading native status", () => {
  assert.match(source, /bundledFirmware\?\.rollbackBlocked/);
  assert.match(source, /!showRollback && \(disposition === "update"/);
  assert.match(source, /!pending && showRollback[\s\S]*?已回退/);
});
