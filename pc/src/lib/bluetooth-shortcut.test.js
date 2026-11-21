import assert from "node:assert/strict";
import test from "node:test";
import { bluetoothShortcutBindings, decodeBluetoothShortcut, encodeBluetoothShortcut } from "./bluetooth-shortcut.js";

test("custom shortcut preserves modifiers and the standard HID key usage", () => {
  assert.equal(encodeBluetoothShortcut(3, 0x1c), "hid-03-1c");
  assert.deepEqual(decodeBluetoothShortcut("hid-0f-52"), { mask: 15, usage: 0x52 });
  for (const invalid of ["hid-00-1c", "hid-10-1c", "hid-01-00", "hid-01-e1", "hid-01-1CX"]) {
    assert.equal(decodeBluetoothShortcut(invalid), null);
  }
});

test("moving or unbinding removes prior shortcuts and restores only their associated default gestures", () => {
  const rows = [
    { event: "button.sw1.long_press", holdEvent: "button.sw1.hold" },
    { event: "button.sw2.short_press" },
    { event: "button.sw3.short_press" },
  ];
  const unrelated = { event: rows[2].event, action: "page_back" };
  const saved = [
    { event: rows[0].event, action: "voice_key_forward", value: "mac-ctrl-alt-d" },
    { event: rows[0].holdEvent, action: "disabled" },
    { event: rows[1].event, action: "component_center" },
    unrelated,
  ];
  const moved = bluetoothShortcutBindings(saved, rows, rows[1], "hid-03-1c");
  assert.deepEqual(moved, [unrelated, { event: rows[1].event, action: "voice_key_forward", value: "hid-03-1c" }]);
  assert.deepEqual(bluetoothShortcutBindings(moved, rows), [unrelated]);
  assert.equal(saved.length, 4);
});


test("HID chord capture distinguishes modifier sides and supports an empty main key", async () => {
  const { keyboardChordKey, isKeyboardChordSupported } = await import("./bluetooth-shortcut.js");
  assert.deepEqual(keyboardChordKey("ControlRight", true), { mask: 0x10, usage: 0 });
  assert.deepEqual(keyboardChordKey("AltRight", true), { mask: 0x40, usage: 0 });
  assert.deepEqual(keyboardChordKey("KeyA", true), { mask: 0, usage: 4 });
  assert.notEqual(keyboardChordKey("Digit1", true).usage, keyboardChordKey("Numpad1", true).usage);
  assert.equal(keyboardChordKey("ControlRight", false), null);
  assert.equal(isKeyboardChordSupported(0x5000, true), true);
  assert.equal(isKeyboardChordSupported(0x5000, false), false);
  assert.equal(isKeyboardChordSupported(0x0106, false), true);
  assert.equal(isKeyboardChordSupported(0x0004, true), true);
  assert.equal(isKeyboardChordSupported(0x10002, true), false);
});
