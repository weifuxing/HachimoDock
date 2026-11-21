"use strict";
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { BaseAdapter } = require("./base");

function devinPaths(env = process.env, home = os.homedir()) {
  const root = env.APPDATA
    ? path.join(env.APPDATA, "devin")
    : path.join(home, ".config", "devin");
  const apps = [];
  if (env.LOCALAPPDATA) {
    apps.push(path.join(env.LOCALAPPDATA, "Programs", "Devin", "devin.exe"));
    apps.push(path.join(env.LOCALAPPDATA, "Devin", "devin.exe"));
  }
  if (env.ProgramFiles) apps.push(path.join(env.ProgramFiles, "Devin", "devin.exe"));
  return { root, apps };
}

class DevinAdapter extends BaseAdapter {
  constructor(options = {}) {
    super({ agentId: "devin", log: options.log });
    this.paths = devinPaths(options.env || process.env, options.home || os.homedir());
    this.cloudEnabled = options.cloudEnabled === true
      || /^(1|true|yes)$/i.test((options.env || process.env).CLAWD_DEVIN_CLOUD_ENABLED || "");
  }
  async isAvailable() {
    const installed = this.paths.apps.some((candidate) => fs.existsSync(candidate))
      || fs.existsSync(this.paths.root);
    const ready = this.cloudEnabled || installed;
    return {
      ready,
      reason: ready
        ? "Devin 云端状态可用；语音输入需在 Devin Desktop 当前对话中使用"
        : "未检测到 Devin 云端配置或 Devin Desktop",
    };
  }
  async listSessions() { return []; }
  async openNew() { throw new Error("请在 Devin 中新建会话"); }
  async *inject() {
    yield {
      kind: "error",
      code: "AGENT_INPUT_UNSUPPORTED",
      message: "Devin 语音输入仅支持向桌面客户端当前可见输入框追加草稿，不支持后台会话注入；不会转发到其他 Agent。",
    };
  }
}

module.exports = { DevinAdapter, devinPaths };
