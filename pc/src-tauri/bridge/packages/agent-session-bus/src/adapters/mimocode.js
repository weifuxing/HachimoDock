"use strict";

const { spawn } = require("child_process");
const { mkdtemp, rm } = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");

const { BaseAdapter } = require("./base");
const { findExecutable } = require("../util/binary-resolver");

const LIST_TIMEOUT_MS = 12000;
const LIST_CACHE_MS = 30000;
const MAX_SESSIONS = 200;
const MAX_OUTPUT_BYTES = 2 * 1024 * 1024;

class MiMoCodeAdapter extends BaseAdapter {
  constructor({
    log,
    env,
    cwd,
    fallbackPaths,
    extraPathDirs,
  } = {}) {
    super({ agentId: "mimocode", log });
    this._env = env || process.env;
    this._cwd = cwd || process.cwd();
    this._fallbackPathsOverride = Array.isArray(fallbackPaths) ? fallbackPaths : null;
    this._extraPathDirsOverride = Array.isArray(extraPathDirs) ? extraPathDirs : null;
    this._sessionList = null;
  }

  _defaultFallbackPaths() {
    const home = this._env.HOME || this._env.USERPROFILE || "~";
    return [
      `${home}/.mimocode/bin/mimo`,
      `${home}/.mimocode/bin/mimo.exe`,
      `${home}/.local/bin/mimo`,
    ];
  }

  _defaultExtraPathDirs() {
    const home = this._env.HOME || this._env.USERPROFILE || "~";
    return [
      `${home}/.mimocode/bin`,
      `${home}/.local/bin`,
    ];
  }

  _resolveBinary() {
    return findExecutable({
      binName: "mimo",
      envVar: "MIMOCODE_CLI_PATH",
      env: this._env,
      fallbackPaths: this._fallbackPathsOverride !== null
        ? this._fallbackPathsOverride
        : this._defaultFallbackPaths(),
      extraPathDirs: this._extraPathDirsOverride !== null
        ? this._extraPathDirsOverride
        : this._defaultExtraPathDirs(),
    });
  }

  async isAvailable() {
    // Even --version loads MiMoCode's native libraries. Status checks only
    // need to know whether the executable is installed.
    return this._resolveBinary()
      ? { ready: true, reason: null }
      : { ready: false, reason: "MiMoCode CLI 未找到（请安装 MiMoCode 或设置 MIMOCODE_CLI_PATH）" };
  }

  async listSessions({ limit = 20 } = {}) {
    const bin = this._resolveBinary();
    if (!bin) {
      this._sessionList = null;
      return [];
    }
    const safeLimit = Math.max(1, Math.min(MAX_SESSIONS, Number.parseInt(limit, 10) || 20));
    let query = this._sessionList;
    if (!query || query.bin !== bin || Date.now() >= query.expiresAt) {
      query = { bin, expiresAt: Infinity };
      this._sessionList = query;
      query.result = this._querySessions(bin).finally(() => {
        query.expiresAt = Date.now() + LIST_CACHE_MS;
      });
    }
    return (await query.result).slice(0, safeLimit);
  }

  async _querySessions(bin) {
    // Bun extracts OpenTUI into the child temp directory and leaves it behind.
    const tempDir = await mkdtemp(path.join(os.tmpdir(), "hachimi-mimocode-"));
    try {
      const stdout = await collectProcessOutput(bin, [
        "session", "list", "--format", "json", "--max-count", String(MAX_SESSIONS),
      ], {
        cwd: this._cwd,
        env: {
          ...this._env,
          TMPDIR: tempDir,
          TMP: tempDir,
          TEMP: tempDir,
        },
      });
      if (!stdout.trim()) return [];
      return parseSessionList(stdout)
        .map(normalizeSession)
        .filter(Boolean)
        .sort((a, b) => b.lastModified - a.lastModified);
    } finally {
      await rm(tempDir, { recursive: true, force: true, maxRetries: 3, retryDelay: 100 });
    }
  }

  async openNew({ cwd } = {}) {
    return {
      id: "",
      lastModified: Date.now(),
      cwd: cwd || this._cwd,
      summary: "new (mimocode)",
    };
  }

  async *inject() {
    yield {
      kind: "error",
      code: "AGENT_INPUT_UNSUPPORTED",
      message: "MiMoCode 不支持后台会话注入；macOS 请使用客户端的当前终端光标语音输入，Windows 暂仅支持状态与会话跟随。",
    };
  }
}

function normalizeSession(row) {
  if (!row || typeof row !== "object") return null;
  const id = typeof row.id === "string" ? row.id.trim() : "";
  if (!id) return null;
  const title = typeof row.title === "string" && row.title.trim()
    ? row.title.trim()
    : "MiMoCode 会话";
  const updated = Number(row.updated);
  const created = Number(row.created);
  return {
    id,
    name: title,
    summary: title,
    cwd: typeof row.directory === "string" ? row.directory : undefined,
    lastModified: Number.isFinite(updated)
      ? updated
      : Number.isFinite(created)
        ? created
        : 0,
  };
}

function parseSessionList(stdout) {
  const text = String(stdout || "").trim();
  try {
    const value = JSON.parse(text);
    return Array.isArray(value) ? value : [];
  } catch {
    const start = text.indexOf("[");
    const end = text.lastIndexOf("]");
    if (start < 0 || end <= start) {
      throw new Error("MiMoCode session list 未返回 JSON 数组");
    }
    const value = JSON.parse(text.slice(start, end + 1));
    return Array.isArray(value) ? value : [];
  }
}

function collectProcessOutput(bin, args, options = {}) {
  return new Promise((resolve, reject) => {
    const useShell = process.platform === "win32" && /\.(cmd|bat)$/i.test(bin);
    const child = spawn(bin, args, {
      cwd: options.cwd,
      env: options.env,
      windowsHide: true,
      shell: useShell,
      stdio: ["ignore", "pipe", "pipe"],
    });
    const stdout = [];
    const stderr = [];
    let outputBytes = 0;
    let failure;
    const stop = (message) => {
      if (failure) return;
      failure = new Error(message);
      child.kill("SIGKILL");
      child.stdout.destroy();
      child.stderr.destroy();
    };
    const append = (target, chunk) => {
      if (failure) return;
      outputBytes += chunk.length;
      if (outputBytes > MAX_OUTPUT_BYTES) {
        stop("MiMoCode session list 输出过大");
        return;
      }
      target.push(chunk);
    };
    child.stdout.on("data", (chunk) => append(stdout, chunk));
    child.stderr.on("data", (chunk) => append(stderr, chunk));
    child.once("error", (error) => { failure ||= error; });
    child.once("close", (code, signal) => {
      clearTimeout(timer);
      if (failure) {
        reject(failure);
      } else if (code === 0) {
        resolve(Buffer.concat(stdout).toString("utf8"));
      } else {
        reject(new Error(
          `mimo session list exited code=${code} signal=${signal || ""}: ${Buffer.concat(stderr).toString("utf8").trim().slice(0, 500)}`,
        ));
      }
    });

    const timer = setTimeout(() => stop("mimo session list timed out"), LIST_TIMEOUT_MS);
    timer.unref?.();
  });
}

module.exports = {
  MiMoCodeAdapter,
  _internal: {
    normalizeSession,
    parseSessionList,
  },
};
