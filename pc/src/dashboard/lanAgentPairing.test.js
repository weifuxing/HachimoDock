import assert from "node:assert/strict";
import test from "node:test";
import { readFileSync } from "node:fs";
import { pairLanAgent } from "./lanAgentPairing.js";

test("Wi-Fi discovery retains all results and waits for an explicit device choice", () => {
  const source = readFileSync(new URL("./LanAgentPairingPanel.jsx", import.meta.url), "utf8");
  assert.match(source, /setDevices\(await invoke\("lan_agent_discover"\)\)/);
  assert.match(source, /devices\.map\(device =>/);
  assert.match(source, /onChange=\{\(\) => setSelected\(device\)\}/);
  assert.doesNotMatch(source, /setSelected\([^;]*(?:\.find\(|usb\?)/);
});

test("a saved address does not count as an authenticated Wi-Fi pairing", () => {
  const source = readFileSync(new URL("./LanAgentPairingPanel.jsx", import.meta.url), "utf8");
  assert.match(source, /const isCurrent = Boolean\(address && deviceStatus\?\.boardDeviceId === device\.id\)/);
  assert.match(source, /device\.ip === address && device\.ready && device\.paired === false/);
  assert.match(source, /本机有旧记录，设备报告尚未配对/);
  assert.match(source, /device\.ip === address \? "可重新配对" : "可配对"/);
  assert.match(source, /const checkStatus[\s\S]*?catch \(e\) \{\s*setDeviceStatus\(null\)/);
});

test("pairing clears the input before sending the code to native", async () => {
  const ip = "192.0.2.12";
  const code = "0123 4567 89ab cdef 0123 4567 89ab cdef";
  const input = { value: code };
  globalThis.window = {
    __TAURI_INTERNALS__: {
      invoke(command, args) {
        assert.equal(input.value, "");
        assert.equal(command, "lan_agent_pair");
        assert.deepEqual(args, { ip, code });
        return Promise.reject(new Error("fixture: rejected pairing"));
      },
    },
  };
  try {
    await assert.rejects(pairLanAgent(ip, input), /fixture/);
    assert.equal(input.value, "");
  } finally {
    delete globalThis.window;
  }
});

test("an authenticated status for the same address clears status failures only", () => {
  const source = readFileSync(new URL("./LanAgentPairingPanel.jsx", import.meta.url), "utf8");
  assert.match(source, /address && wifi\?\.wifiConnected && wifi\.address === address/);
  assert.match(source, /if \(verifiedWifi && statusError\) setStatusError\(""\)/);
  assert.match(source, /statusError && !verifiedWifi/);
  assert.match(source, /deviceStatus\?\.wirelessOta && firmwareDisposition !== "latest"/);
});
