/**
 * [Input] firmware-version.js pure helpers.
 * [Output] Regression coverage for older, equal, newer, suffixed, and invalid versions.
 * [Pos] test node in pc/src/lib.
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import { currentFirmwareCompletion } from "./firmware-version.js";
import test from "node:test";
import assert from "node:assert/strict";
import {
  compareFirmwareVersions,
  firmwareUpdateDisposition,
  parseFirmwareRevision,
  parseFirmwareVersion,
} from "./firmware-version.js";

test("same numeric core is ordered by the -rNNN build revision", () => {
  assert.equal(parseFirmwareRevision("0.7.103-p4-r144"), 144);
  assert.equal(parseFirmwareRevision("0.7.103-p4-r142-framework"), 142);
  assert.equal(parseFirmwareRevision("0.7.34-p4"), 0);
  assert.equal(compareFirmwareVersions("0.7.103-p4-r142-framework", "0.7.103-p4-r144"), -1);
  assert.equal(compareFirmwareVersions("0.7.103-p4-r144", "0.7.103-p4-r144"), 0);
  assert.equal(compareFirmwareVersions("0.7.103-p4-r66", "0.7.103-p4-r144"), -1);
  assert.equal(compareFirmwareVersions("0.7.104-p4-r1", "0.7.103-p4-r144"), 1);
  assert.equal(firmwareUpdateDisposition("0.7.103-p4-r142-framework", "0.7.103-p4-r144"), "update");
  assert.equal(firmwareUpdateDisposition("0.7.103-p4-r144", "0.7.103-p4-r144"), "latest");
  assert.equal(firmwareUpdateDisposition("0.7.103-p4-r66", "0.7.103-p4-r144"), "update");
});

test("firmware versions compare numerically and ignore product suffixes", () => {
  assert.deepEqual(parseFirmwareVersion("v0.7.29-p4"), [0, 7, 29]);
  assert.equal(compareFirmwareVersions("0.7.9-p4", "0.7.29-p4"), -1);
  assert.equal(compareFirmwareVersions("0.7.29", "0.7.29-p4"), 0);
  assert.equal(compareFirmwareVersions("0.8.0-p4", "0.7.29-p4"), 1);
});

test("firmware update disposition exposes update only for older devices", () => {
  assert.equal(firmwareUpdateDisposition("0.7.28-p4", "0.7.29-p4"), "update");
  assert.equal(firmwareUpdateDisposition("0.7.29-p4", "0.7.29-p4"), "latest");
  assert.equal(firmwareUpdateDisposition("0.7.30-p4", "0.7.29-p4"), "latest");
  assert.equal(firmwareUpdateDisposition("unknown", "0.7.29-p4"), "unknown");
});

test("a firmware completion applies only to the connection that passed verification", () => {
  const usb = { connected: true, runtime: "esp-p4", boardDeviceId: "p4-test", connectionId: "2" };
  const completed = { boardDeviceId: "p4-test", connectionId: "2", version: "0.7.103-p4-r195-wifi6" };
  assert.equal(currentFirmwareCompletion(usb, completed), completed);
  assert.equal(currentFirmwareCompletion({ ...usb, connectionId: "3" }, completed), null);
  assert.equal(currentFirmwareCompletion({ ...usb, boardDeviceId: "p4-other" }, completed), null);
  assert.equal(currentFirmwareCompletion({ ...usb, connected: false }, completed), null);
});
