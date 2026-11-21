// Match the firmware's lab action numbers and physical events. The preview
// must exercise the same bindings that will be sent with the component.
const actionCodes = new Map([
  ["lab.left", 1], ["lab.right", 2], ["lab.up", 3],
  ["lab.down", 4], ["lab.a", 5], ["lab.b", 6],
]);

const physicalControls = [
  ["knob.rotate_ccw", "ArrowLeft", "←"],
  ["knob.rotate_cw", "ArrowRight", "→"],
  ["joystick.up", "ArrowUp", "↑"],
  ["joystick.down", "ArrowDown", "↓"],
  ["button.sw1.short_press", "KeyZ", "Z"],
  ["button.sw2.short_press", "KeyX", "X"],
  ["button.sw3.short_press", "KeyC", "C"],
  ["button.sw1.double_press", null, "1 键双击"],
  ["button.sw2.double_press", null, "2 键双击"],
  ["button.sw3.double_press", null, "3 键双击"],
  ["button.encoder.short_press", "Enter", "中按"],
  ["button.encoder.long_press", null, "中按长按"],
  ["button.sw1.long_press", null, "1 键长按"],
];

export function resolveWasmControls(bindings = [], reservedEvents = []) {
  return physicalControls.flatMap(([event, code, keyLabel]) => {
    if (reservedEvents.includes(event)) return [];
    const matches = bindings.filter(binding => binding.event === event ||
      (binding.event === "knob.rotate_cw / knob.rotate_ccw" &&
        (event === "knob.rotate_cw" || event === "knob.rotate_ccw")));
    if (matches.length !== 1) return [];
    const [binding] = matches;
    const key = actionCodes.get(binding.action);
    return key ? [{ event, code, keyLabel, key, label: binding.label || keyLabel }] : [];
  });
}
