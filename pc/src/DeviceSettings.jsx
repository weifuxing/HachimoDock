import React, { useMemo, useEffect, useSyncExternalStore } from "react";
import { invoke } from "@tauri-apps/api/core";
import { useExitProtection } from "./lib/exit-protection.js";
import Card from "./shell/Card.jsx";
import PageShell from "./shell/PageShell.jsx";
import DeviceVolumePanel from "./dashboard/DeviceVolumePanel.jsx";
import SharedLocationPanel from "./dashboard/SharedLocationPanel.jsx";
import { createSharedLocationController } from "./dashboard/shared-location-model.js";
import LanAgentPairingPanel from "./dashboard/LanAgentPairingPanel.jsx";

export default function DeviceSettings({ usb, wifi, active, wirelessMaintenanceRequest }) {
  const locationController = useMemo(() => createSharedLocationController({ invoke }), []);
  const locationState = useSyncExternalStore(locationController.subscribe, locationController.getSnapshot, locationController.getSnapshot);
  useExitProtection(locationState.saving ? "位置设置正在保存" : locationState.dirty ? "位置设置有未保存的修改" : "");
  useEffect(() => () => locationController.setActive(false), [locationController]);
  return <section className="api-settings-page device-settings">
    <PageShell title="其它设置" subtitle="管理设备音量、所在地区和 Wi-Fi 配对。">
    <Card title="设备总音量"><DeviceVolumePanel usb={usb} wifi={wifi} active={active} /></Card>
    <Card><SharedLocationPanel active={active} controller={locationController} /></Card>
    <LanAgentPairingPanel usb={usb} wifi={wifi} active={active} maintenanceRequest={wirelessMaintenanceRequest} />
    </PageShell>
  </section>;
}
