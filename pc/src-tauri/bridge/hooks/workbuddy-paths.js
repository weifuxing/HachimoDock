"use strict";
const path = require("node:path");
const os = require("node:os");

function workbuddyPaths({ env = process.env, platform = process.platform,
  // WorkBuddy uses os.homedir(): on Windows this is USERPROFILE, not a
  // Git Bash / shell HOME override. Keep explicit test/embedding homes intact.
  home = (platform === "win32" ? env.USERPROFILE : env.HOME) || os.homedir() } = {}) {
  // WorkBuddy is a separate product: never fall back to CodeBuddy's config.
  const root = env.WORKBUDDY_CONFIG_DIR?.trim() || path.join(home, ".workbuddy");
  const apps = platform === "darwin"
    ? ["/Applications/WorkBuddy.app", path.join(home, "Applications/WorkBuddy.app")]
    : platform === "win32"
      ? [env.LOCALAPPDATA && path.join(env.LOCALAPPDATA, "Programs/WorkBuddy/WorkBuddy.exe"),
         env.ProgramFiles && path.join(env.ProgramFiles, "WorkBuddy/WorkBuddy.exe")].filter(Boolean)
      : [];
  if (env.WORKBUDDY_INSTALL_DIR) apps.unshift(path.join(env.WORKBUDDY_INSTALL_DIR, platform === "win32" ? "WorkBuddy.exe" : "WorkBuddy"));
  return { root, settings: path.join(root, "settings.json"), apps };
}
module.exports = { workbuddyPaths };
