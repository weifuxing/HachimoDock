/**
 * [Input] Component-library records with `createdAtMs` or ISO `createdAt`.
 * [Output] Recently installed user ordering, followed by the shared builtin sequence.
 * [Pos] component-center library node in pc/src/component-center
 * [Sync] If this file changes, update `pc/src/component-center/.folder.md`.
 */

import {compareVersions} from './catalog-state.js';
import factoryConfig from '../../../firmware/factory-config.json' with {type: 'json'};

// Shared with device bootstrap, factory resources and launcher artwork generation.
export const DEFAULT_COMPONENT_ORDER = Object.freeze(factoryConfig.components.ids.filter(id => !factoryConfig.components.firmwareOnlyIds?.includes(id)));
const defaultRanks = new Map(DEFAULT_COMPONENT_ORDER.map((id, index) => [id, index]));

export function sortComponentsByDefaultOrder(components) {
  return [...(components || [])]
    .map((component, index) => ({component, index}))
    .sort((a, b) => (defaultRanks.get(a.component.id) ?? defaultRanks.size)
      - (defaultRanks.get(b.component.id) ?? defaultRanks.size) || a.index - b.index)
    .map(({component}) => component);
}

export function componentCreatedAtMs(component) {
  const numeric = Number(component?.createdAtMs);
  if (Number.isFinite(numeric) && numeric > 0) return numeric;

  const parsed = Date.parse(String(component?.createdAt || ""));
  return Number.isFinite(parsed) ? parsed : 0;
}

export function sortComponentsByCreatedAt(components) {
  return [...(components || [])]
    .map((component, index) => ({ component, index }))
    .sort((left, right) => (
      componentCreatedAtMs(right.component) - componentCreatedAtMs(left.component)
      || left.index - right.index
    ))
    .map(({ component }) => component);
}

export function mergeComponentCatalog(publishedItems, builtins) {
  const publishedIds = new Set(publishedItems.map((item) => item.id));
  const merged = publishedItems.map(local => {
      const builtin=builtins.find(item=>item.id===local.id);
      if(!builtin)return local;
      // Keep both sources available: choosing the newer default never deletes a custom revision.
      const selected=builtin.valid!==false&&(local.valid===false||compareVersions(builtin.version,local.version)===1)?builtin:local;
      return {...selected, isBundledCopy:local.isBundledCopy===true, sourceAlternatives:[local,builtin]};
    });
  return [
    ...merged.filter(item=>!item.isBundledCopy),
    ...sortComponentsByDefaultOrder([...merged.filter(item=>item.isBundledCopy),...builtins.filter((item) => !publishedIds.has(item.id))]),
  ].filter(isComponentVisible);
}

// A withdrawal also applies to packages cached by earlier releases. Match the
// exact package identity, not shared media capabilities or user-authored names.
// Coordinate explicit withdrawals with the packaged factory catalog.
const withdrawnComponentIds = new Set(["upcoming-todos"]); // Merged into hachimi-notes; unified todo storage is retained.
export function isWithdrawnComponent(component) {
  return withdrawnComponentIds.has(component?.id ?? component?.componentId);
}

// Release availability, not deletion: preserve local packages and user data.
export function isComponentVisible(component) {
  return !isWithdrawnComponent(component)
    && component?.id !== "music-player"
    && (component?.mediaSource !== "audio.player" || component?.isLocal === true);
}

export function legacyAudioInstallBlockedReason(component) {
  if (component?.syncRequirements?.blockedReason) return component.syncRequirements.blockedReason;
  if (component?.mediaSource !== "audio.player") return "";
  return "此包使用已停用的旧播放器接口 audio.player。请用 PetUI 的本地媒体服务桥重新打包，再导入和同步。";
}

export function componentUsbInstallBlockedReason(component, usb) {
  const requirements = Object.entries(component?.syncRequirements?.usbCapabilities || {});
  if (!requirements.length) return "";
  if (!usb?.connected || !usb.boardDeviceId) {
    return "此组件需要通过 USB 握手确认服务能力。请连接 USB 设备后同步。";
  }
  const missing = requirements.filter(([key, version]) => usb.capabilities?.[key] !== version);
  return missing.length
    ? `当前 USB 设备未声明组件所需能力：${missing.map(([key, version]) => `${key}=${version}`).join("、")}。请检查实际固件版本和设备握手。`
    : "";
}
