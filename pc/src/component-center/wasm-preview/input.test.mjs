import assert from "node:assert/strict";
import { test } from "node:test";
import { resolveWasmControls } from "./input.mjs";

test("physical bindings choose guest actions after remapping", () => {
  const controls = resolveWasmControls([
    { event: "knob.rotate_ccw", action: "lab.right" },
    { event: "joystick.up", action: "lab.up" },
    { event: "button.sw1.short_press", action: "lab.b" },
  ]);
  assert.deepEqual(controls.map(({ code, key }) => [code, key]), [
    ["ArrowLeft", 2], ["ArrowUp", 3], ["KeyZ", 6],
  ]);
});

test("unbound, ambiguous and non-WASM actions do not simulate a physical response", () => {
  assert.deepEqual(resolveWasmControls([
    { event: "joystick.up", action: "lab.left" },
    { event: "joystick.up", action: "lab.up" },
    { event: "joystick.down", action: "page_back" },
    { event: "button.sw2.short_press", action: "unknown" },
  ]), []);
});

test("legacy combined rocker binding is previewed in both device directions", () => {
  const controls = resolveWasmControls([{ event: "knob.rotate_cw / knob.rotate_ccw", action: "lab.a" }]);
  assert.deepEqual(controls.map(({ code, key }) => [code, key]), [
    ["ArrowLeft", 5], ["ArrowRight", 5],
  ]);
});

test("global shortcuts take priority over component bindings in the preview", () => {
  const bindings = [
    { event: "button.sw1.short_press", action: "lab.a" },
    { event: "button.sw2.short_press", action: "lab.b" },
  ];
  assert.deepEqual(resolveWasmControls(bindings, ["button.sw1.short_press"]).map(item => item.key), [6]);
});
