export function wifiDeviceControlReady(wifi) {
  return wifi?.panelControl === true && wifi?.controlTrusted === true;
}

export function wifiDeviceControl(invoke, wifi, operation, args = {}) {
  if (!wifiDeviceControlReady(wifi)) return Promise.reject(new Error("当前设备未开放这项 Wi-Fi 控制。"));
  return invoke("lan_panel_control", { ...args, boardDeviceId: wifi.boardDeviceId, expectedBootId: wifi.bootId, operation });
}
