import test from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { runInNewContext } from "node:vm";

const require = createRequire(import.meta.url);
const React = require("react");
const { renderToStaticMarkup } = require("react-dom/server");
const { buildSync } = require("esbuild");
const entry = fileURLToPath(new URL("./KeyboardConfiguration.jsx", import.meta.url));
const bundle = buildSync({ entryPoints: [entry], bundle: true, write: false,
  platform: "node", format: "cjs", external: ["react"], logLevel: "silent" });

function renderKeyboard(props, snapshot) {
  const module = { exports: {} };
  const localStorage = { getItem: () => JSON.stringify(snapshot) };
  runInNewContext(bundle.outputFiles[0].text,
    { module, exports: module.exports, require, localStorage, URL, TextEncoder });
  return renderToStaticMarkup(React.createElement(module.exports.default, props));
}

test("offline Bluetooth configuration still renders the device and all gesture mappings", () => {
  const html = renderKeyboard({ usb: { connected: false } }, null);
  assert.match(html, /role="tablist" aria-label="键盘配置类别"/);
  assert.match(html, /aria-selected="true"[^>]*>按钮配置</);
  assert.match(html, /aria-selected="false"[^>]*>蓝牙快捷键</);
  assert.match(html, /role="tabpanel"[^>]*hidden=""/);
  assert.match(html, /蓝牙控件导航/);
  assert.match(html, /蓝牙实体控件与手势配置/);
  assert.match(html, /蓝牙模式状态未确认/);
  assert.equal((html.match(/>未读取</g) || []).length, 15);
  assert.doesNotMatch(html, /role="switch"|蓝牙模式已关闭|保存快捷键|配对码/);
});

test("offline mappings use the last reading only when it matches the selected board", () => {
  const snapshot = { board: "p4-123", keys: [0x0106, 0], enabled: true, passkey: 654321 };
  const props = { usb: { connected: false, boardDeviceId: "p4-123" } };
  const html = renderKeyboard(props, snapshot);
  assert.match(html, /上次读取的设备配置（p4-123）/);
  assert.match(html, />左 Ctrl \+ C</);
  assert.match(html, />未设置</);
  assert.doesNotMatch(html, /role="switch"|蓝牙模式已关闭|654321|保存快捷键/);
  assert.doesNotMatch(renderKeyboard({ ...props, boardDeviceId: "p4-456" }, snapshot),
    /上次读取的设备配置|>左 Ctrl \+ C</);
});

test("a Wi-Fi peer exposes configuration loading, but an active USB connection takes priority", () => {
  const wifi = { boardDeviceId: "p4-123", bootId: "0123456789abcdef" };
  const wireless = renderKeyboard({ usb: { connected: false }, wifi }, null);
  assert.match(wireless, /正在读取配置…/);
  assert.match(wireless, /蓝牙模式状态未确认/);
  assert.doesNotMatch(wireless, /role="switch"|可离线查看快捷键/);
  const wired = renderKeyboard({ usb: { connected: true, boardDeviceId: "p4-456", connectionId: "9" },
    wifi }, null);
  assert.match(wired, /当前设备固件尚未开放独立的蓝牙键盘配置/);
  assert.doesNotMatch(wired, /正在读取配置…/);
});
