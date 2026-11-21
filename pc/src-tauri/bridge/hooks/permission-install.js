"use strict";
const path = require("node:path");
function owned(h, legacyMarker) {
  if (!h || typeof h !== "object") return false;
  if (h.type === "command") {
    // Only exact installed script paths belong to us. A filename match could
    // remove another product's permission-hook.js or a user-created wrapper.
    const match = typeof h.command === "string" && h.command.match(/^(?:"[^"\r\n]+"|[^\s"\r\n]+)\s+"([^"\r\n]+)"(?:\s+PermissionRequest)?$/);
    if (!match) return false;
    const normalize = p => p.replace(/\\/g, "/").replace("app.asar/", "app.asar.unpacked/");
    return ["permission-hook.js", legacyMarker].some(name => normalize(match[1]) === normalize(path.join(__dirname, name)));
  }
  if (h.type !== "http") return false;
  try {
    const u = new URL(h.url);
    return u.protocol === "http:" && ["127.0.0.1", "localhost", "[::1]"].includes(u.hostname) &&
      Number(u.port) >= 23333 && Number(u.port) <= 23337 && u.pathname === "/permission" && !u.search && !u.hash && !u.username && !u.password;
  } catch { return false; }
}
function syncPermissionHook(settings, nodeBin, legacyMarker) {
  const before = JSON.stringify(settings.hooks.PermissionRequest);
  const old = settings.hooks.PermissionRequest;
  const entries = Array.isArray(old) ? old : old && typeof old === "object" ? [old] : [];
  const next = [];
  for (const entry of entries) {
    if (Array.isArray(entry?.hooks)) {
      const hooks = entry.hooks.filter(h => !owned(h, legacyMarker));
      if (hooks.length) next.push({ ...entry, hooks });
    } else if (!owned(entry, legacyMarker)) next.push(entry);
  }
  const script = path.join(__dirname, "permission-hook.js").replace(/\\/g, "/").replace("app.asar/", "app.asar.unpacked/");
  next.push({ matcher: "", hooks: [{ type: "command", command: `"${nodeBin}" "${script}"`, timeout: 15 }] });
  settings.hooks.PermissionRequest = next;
  return before !== JSON.stringify(next);
}
module.exports = { syncPermissionHook };
