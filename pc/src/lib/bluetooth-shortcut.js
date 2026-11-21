// Standard keyboard-page HID usages shared with the WiFi6 shortcut firmware.
const keyGroups = [
  [0x04, [..."ABCDEFGHIJKLMNOPQRSTUVWXYZ"]],
  [0x1e, [..."1234567890"]],
  [0x28, ["Enter", "Esc", "Backspace", "Tab", "Space", "-", "=", "[", "]", "\\", "非美式 #", ";", "'", "`", ",", ".", "/"]],
  [0x3a, Array.from({ length: 12 }, (_, index) => `F${index + 1}`)],
  [0x46, ["Print Screen", "Scroll Lock", "Pause", "Insert", "Home", "Page Up", "Delete", "End", "Page Down", "→", "←", "↓", "↑"]],
  [0x65, ["Menu"]],
];
export const BLUETOOTH_SHORTCUT_KEYS = keyGroups.flatMap(([base, labels]) =>
  labels.map((label, index) => ({ usage: base + index, label })));

export const BLUETOOTH_SHORTCUT_MODIFIERS = [
  { mask: 1, label: "Ctrl" },
  { mask: 2, label: "Shift" },
  { mask: 4, label: "Alt / Option" },
  { mask: 8, label: "Win / Command" },
];

// These modifiers are the eight bits defined by HID keyboard reports.
// Existing saved Ctrl/Shift/Alt/Command chords already use the left-hand bits.
export const BLUETOOTH_KEYBOARD_MODIFIERS = [
  { mask: 1, label: "左 Ctrl", code: "ControlLeft" },
  { mask: 16, label: "右 Ctrl", code: "ControlRight" },
  { mask: 2, label: "Shift", code: "ShiftLeft" },
  { mask: 32, label: "右 Shift", code: "ShiftRight" },
  { mask: 4, label: "左 Alt / Option", code: "AltLeft" },
  { mask: 64, label: "右 Alt / Option", code: "AltRight" },
  { mask: 8, label: "Win / Command", code: "MetaLeft" },
  { mask: 128, label: "右 Win / Command", code: "MetaRight" },
];
const punctuationCodes = ["Enter", "Escape", "Backspace", "Tab", "Space", "Minus", "Equal", "BracketLeft",
  "BracketRight", "Backslash", null, "Semicolon", "Quote", "Backquote", "Comma", "Period", "Slash"];
const navigationCodes = ["PrintScreen", "ScrollLock", "Pause", "Insert", "Home", "PageUp", "Delete",
  "End", "PageDown", "ArrowRight", "ArrowLeft", "ArrowDown", "ArrowUp"];
const extraKeys = [
  [0x39, "Caps Lock", "CapsLock"],
  [0x53, "Num Lock", "NumLock"],
  ...["Divide", "Multiply", "Subtract", "Add", "Enter", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Decimal"]
    .map((name, index) => [0x54 + index, `数字键盘 ${name}`, `Numpad${name}`]),
  [0x64, "非美式 \\", "IntlBackslash"],
];
export const BLUETOOTH_KEYBOARD_KEYS = [
  ...BLUETOOTH_SHORTCUT_KEYS,
  ...extraKeys.map(([usage, label]) => ({ usage, label })),
].sort((a, b) => a.usage - b.usage);
const keyByCode = new Map([
  ...[..."ABCDEFGHIJKLMNOPQRSTUVWXYZ"].map((key, i) => [`Key${key}`, 4 + i]),
  ...[..."1234567890"].map((key, i) => [`Digit${key}`, 0x1e + i]),
  ...punctuationCodes.flatMap((code, i) => code ? [[code, 0x28 + i]] : []),
  ...Array.from({length: 12}, (_, i) => [`F${i + 1}`, 0x3a + i]),
  ...navigationCodes.map((code, i) => [code, 0x46 + i]),
  ["ContextMenu", 0x65],
  ...extraKeys.map(([usage, , code]) => [code, usage]),
]);
const modifierByCode = new Map(BLUETOOTH_KEYBOARD_MODIFIERS.map(({ code, mask }) => [code, mask]));

export function keyboardChordKey(code, extended) {
  const mask = modifierByCode.get(code);
  if (mask) return extended || mask <= 8 ? { mask, usage: 0 } : null;
  const usage = keyByCode.get(code);
  const keys = extended ? BLUETOOTH_KEYBOARD_KEYS : BLUETOOTH_SHORTCUT_KEYS;
  return usage && keys.some(key => key.usage === usage) ? { mask: 0, usage } : null;
}

export function isKeyboardChordSupported(chord, extended) {
  if (!Number.isInteger(chord) || chord < 0 || chord > 0xffff) return false;
  if (chord === 0) return true; // Clear the mapping.
  const mask = chord >> 8;
  const usage = chord & 0xff;
  if (!extended) return Boolean(encodeBluetoothShortcut(mask, usage));
  return usage === 0 || BLUETOOTH_KEYBOARD_KEYS.some(key => key.usage === usage);
}

const keyboardSnapshotKey = "hachimidock.bluetooth-keyboard.last-config";

export function readKeyboardSnapshot(board) {
  try {
    const snapshot = JSON.parse(globalThis.localStorage.getItem(keyboardSnapshotKey));
    return snapshot?.board && (!board || snapshot.board === board) && Array.isArray(snapshot.keys)
      ? snapshot
      : null;
  } catch {
    return null;
  }
}

export function storeKeyboardSnapshot(board, keys) {
  if (!board || !Array.isArray(keys)) return;
  try {
    const value = JSON.stringify({ board, keys });
    if (globalThis.localStorage.getItem(keyboardSnapshotKey) !== value) {
      globalThis.localStorage.setItem(keyboardSnapshotKey, value);
    }
  } catch {
    // The live configuration is usable when local storage is unavailable.
  }
}

export function encodeBluetoothShortcut(mask, usage) {
  if (!Number.isInteger(mask) || mask < 1 || mask > 15 ||
      !BLUETOOTH_SHORTCUT_KEYS.some(key => key.usage === usage)) return "";
  return `hid-0${mask.toString(16)}-${usage.toString(16).padStart(2, "0")}`;
}

export function decodeBluetoothShortcut(value) {
  const match = /^hid-0([1-9a-f])-([0-9a-f]{2})$/.exec(value ?? "");
  if (!match) return null;
  const mask = parseInt(match[1], 16);
  const usage = parseInt(match[2], 16);
  return encodeBluetoothShortcut(mask, usage) ? { mask, usage } : null;
}

export function bluetoothShortcutBindings(bindings, rows, row = null, value = "") {
  const oldEvents = new Set(bindings
    .filter(binding => binding.action === "voice_key_forward")
    .map(binding => binding.event));
  // Omitted bindings return to the board's defaults in input/config.
  for (const option of rows) {
    if (oldEvents.has(option.event) && option.holdEvent) oldEvents.add(option.holdEvent);
  }
  const next = bindings.filter(binding => !oldEvents.has(binding.event) &&
    binding.event !== row?.event && binding.event !== row?.holdEvent);
  if (row) {
    next.push({ event: row.event, action: "voice_key_forward", value });
    if (row.holdEvent) next.push({ event: row.holdEvent, action: "disabled" });
  }
  return next;
}
