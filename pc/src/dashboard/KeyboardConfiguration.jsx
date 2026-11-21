import React, { useId, useState } from "react";
import Card from "../shell/Card.jsx";
import PageShell from "../shell/PageShell.jsx";
import BluetoothShortcutPanel from "./BluetoothShortcutPanel.jsx";
import { navigateTabs } from "../lib/tab-navigation.js";

const tabs = [["buttons", "按钮配置"], ["bluetooth", "蓝牙快捷键"]];

export default function KeyboardConfiguration({ usb, wifi, boardDeviceId, children }) {
  const [tab, setTab] = useState("buttons");
  const id = useId();
  const wired = usb?.connected === true;
  const board = wired ? usb.boardDeviceId : wifi?.boardDeviceId || boardDeviceId || usb?.boardDeviceId;
  return <PageShell title="键盘配置">
    <div className="keyboard-configuration-tabs" role="tablist" aria-label="键盘配置类别">
      {tabs.map(([key, label]) => <button key={key} type="button" role="tab"
        id={`${id}-tab-${key}`} aria-controls={`${id}-panel-${key}`}
        aria-selected={tab === key} tabIndex={tab === key ? 0 : -1}
        onClick={() => setTab(key)}
        onKeyDown={event => navigateTabs(event, tabs.map(([key]) => key), key, setTab)}>
        {label}
      </button>)}
    </div>
    <div role="tabpanel" id={`${id}-panel-buttons`} aria-labelledby={`${id}-tab-buttons`}
      hidden={tab !== "buttons"}>
      {children ?? <Card title="按钮配置"><p>连接设备后配置实体按键和摇杆。</p></Card>}
    </div>
    <div role="tabpanel" id={`${id}-panel-bluetooth`} aria-labelledby={`${id}-tab-bluetooth`}
      hidden={tab !== "bluetooth"}>
      <BluetoothShortcutPanel key={JSON.stringify([board, wired, wired ? usb.connectionId : wifi?.bootId])}
        usb={usb} wifi={wifi} board={board} active={tab === "bluetooth"} />
    </div>
  </PageShell>;
}
