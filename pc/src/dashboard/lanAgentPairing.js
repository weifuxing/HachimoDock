import { invoke } from "@tauri-apps/api/core";

export const LAN_AGENT_PAIRING_UPDATED = "lan-agent-pairing-updated";

export function readLanAgentPairing() {
  return invoke("lan_agent_pair_status");
}

export function publishLanAgentPairing(address) {
  window.dispatchEvent(new CustomEvent(LAN_AGENT_PAIRING_UPDATED, { detail: address }));
}

export function pairLanAgent(ip, codeField) {
  const code = codeField.value;
  codeField.value = "";
  return invoke("lan_agent_pair", { ip, code });
}


export async function readRealtimeWifiBoard() {
  if (!await readLanAgentPairing()) return "";
  const status = await invoke("lan_device_status");
  return status?.wirelessRealtime && status?.controlTrusted && status?.wifiConnected
    ? status.boardDeviceId || ""
    : "";
}
