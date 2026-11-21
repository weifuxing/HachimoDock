import React from "react";

export const P4_MAP_CONTROLS = [
  { id: "p4_sw1", label: "SW1", detail: "左侧按键", x: 84 },
  { id: "p4_sw2", label: "SW2", detail: "中间按键", x: 150 },
  { id: "p4_sw3", label: "SW3", detail: "右侧按键", x: 216 },
  { id: "p4_joystick", label: "摇杆", detail: "四向与中按" },
];

export default function P4BoardInputMap({ gestureCount, highlightedControl, activeControl, onHover, onSelect }) {
  function controlProps({ id, label }) {
    return {
      "data-button-id": id,
      onMouseEnter: () => onHover?.(id),
      onMouseLeave: () => onHover?.(""),
      ...(onSelect && {
        role: "button",
        tabIndex: 0,
        "aria-label": `配置${label}`,
        "aria-pressed": activeControl === id,
        onClick: () => onSelect(id),
        onKeyDown: event => {
          if (event.key !== "Enter" && event.key !== " ") return;
          event.preventDefault();
          onSelect(id);
        },
      }),
    };
  }

  return (
    <svg className="board-button-map__device board-button-panel__svg" viewBox="0 0 456 320"
      role={onSelect ? "group" : "img"} aria-label="板端外观和按钮位置示意">
      <rect className="board-button-map__body board-button-map__body--p4" x="48" y="20" width="360" height="278" rx="32" />
      <rect className="board-button-map__screen-bezel board-button-map__screen-bezel--p4" x="70" y="40" width="316" height="154" rx="20" />
      <rect className="board-button-map__screen board-button-map__screen--p4" x="84" y="54" width="288" height="126" rx="10" />
      <circle className="board-button-map__screen-status" cx="108" cy="82" r="4" />
      <text className="board-button-map__screen-kicker" x="120" y="86">HARDWARE INPUT</text>
      <text className="board-button-map__screen-title" x="108" y="128">{gestureCount} GESTURES</text>
      <text className="board-button-map__screen-copy" x="108" y="153">SW1 · SW2 · SW3 · JOYSTICK</text>
      <rect className="board-button-map__control-deck" x="66" y="206" width="324" height="76" rx="22" />
      {P4_MAP_CONTROLS.map(control => {
        const { id, label, x } = control;
        const state = [
          activeControl === id && "is-active",
          highlightedControl === id && "is-hovered",
        ].filter(Boolean).join(" ");
        return <g key={id} {...controlProps(control)}>
          {id === "p4_joystick" ? <>
            <circle className={`board-button-map__encoder board-button-map__encoder--p4 ${state}`} cx="328" cy="242" r="32" />
            <circle className="board-button-map__encoder-cap" cx="328" cy="242" r="24" />
            <text className="board-button-map__encoder-label" x="328" y="246" textAnchor="middle">{label}</text>
          </> : <>
            <rect className={`board-button-map__hardware-key ${state}`} x={x} y="218" width="48" height="48" rx="12" />
            <text className="board-button-map__hardware-key-label" x={x + 24} y="247" textAnchor="middle">{label}</text>
          </>}
        </g>;
      })}
    </svg>
  );
}

export function ControlGlyph({ controlId, label, active = false }) {
  const isJoystick = controlId === "p4_joystick";
  const isEncoder = controlId === "legacy_encoder";
  return (
    <svg
      className={`board-button-control-glyph${isEncoder || isJoystick ? " is-encoder" : " is-key"}${isJoystick ? " is-joystick" : ""}${active ? " is-active" : ""}`}
      viewBox="0 0 120 92"
      role="img"
      aria-label={`${label} 实体控件示意`}
    >
      <rect className="board-button-control-glyph__deck" x="5" y="5" width="110" height="82" rx="22" />
      {isJoystick ? (
        <>
          <circle className="board-button-control-glyph__encoder-shadow" cx="60" cy="49" r="27" />
          <circle className="board-button-control-glyph__encoder" cx="60" cy="45" r="25" />
          <circle className="board-button-control-glyph__encoder-cap" cx="60" cy="45" r="15" />
          <path className="board-button-control-glyph__arrow" d="M60 12 V25 M55 18 L60 12 L65 18" />
          <path className="board-button-control-glyph__arrow" d="M60 78 V65 M55 72 L60 78 L65 72" />
          <path className="board-button-control-glyph__arrow" d="M22 45 H36 M28 40 L22 45 L28 50" />
          <path className="board-button-control-glyph__arrow" d="M98 45 H84 M92 40 L98 45 L92 50" />
        </>
      ) : isEncoder ? (
        <>
          <path className="board-button-control-glyph__rotation" d="M28 53 A34 34 0 0 1 92 53" />
          <path className="board-button-control-glyph__arrow" d="M28 53 L25 43 M28 53 L38 50" />
          <path className="board-button-control-glyph__arrow" d="M92 53 L95 43 M92 53 L82 50" />
          <circle className="board-button-control-glyph__encoder-shadow" cx="60" cy="47" r="29" />
          <circle className="board-button-control-glyph__encoder" cx="60" cy="43" r="27" />
          <circle className="board-button-control-glyph__encoder-cap" cx="60" cy="43" r="18" />
          <path className="board-button-control-glyph__encoder-mark" d="M60 19 V27" />
        </>
      ) : (
        <>
          <rect className="board-button-control-glyph__key-shadow" x="32" y="29" width="56" height="46" rx="15" />
          <rect className="board-button-control-glyph__key" x="32" y="23" width="56" height="46" rx="15" />
          <path className="board-button-control-glyph__press-mark" d="M49 14 H71 M54 9 L49 14 L54 19 M66 9 L71 14 L66 19" />
        </>
      )}
      <text className="board-button-control-glyph__label" x="60" y={isEncoder || isJoystick ? "49" : "52"} textAnchor="middle">
        {label}
      </text>
    </svg>
  );
}
