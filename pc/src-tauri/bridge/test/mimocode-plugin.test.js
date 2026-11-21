"use strict";

/*
 * [Input] Temporary MiMoCode config roots and synthetic official plugin events.
 * [Output] Regression coverage for managed plugin installation and Pet Manager state payloads.
 * [Pos] Node tests for the MiMoCode status integration.
 */

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const http = require("node:http");
const os = require("node:os");
const path = require("node:path");

const {
  MANAGED_PLUGIN_FILE_NAME,
  resolveMiMoCodeConfigDir,
  syncMiMoCodePlugin,
} = require("../hooks/mimocode-install");

const capabilityApi = require("../packages/agent-session-bus/src/local-capability");
const PLUGIN_SOURCE = path.join(__dirname, "..", "hooks", "mimocode-pet-manager-plugin.js");

test("MiMoCode plugin sync is idempotent and preserves user plugins", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "mimocode-plugin-sync-"));
  try {
    const configDir = resolveMiMoCodeConfigDir({ home, env: {} });
    const pluginDir = path.join(configDir, "plugin");
    fs.mkdirSync(pluginDir, { recursive: true });
    const userPlugin = path.join(pluginDir, "user-plugin.js");
    fs.writeFileSync(userPlugin, "export const UserPlugin = async () => ({});\n", "utf8");

    const first = syncMiMoCodePlugin({ configDir, sourcePath: PLUGIN_SOURCE });
    assert.equal(first.added, 1);
    assert.equal(first.updated, 0);
    assert.equal(fs.existsSync(path.join(pluginDir, MANAGED_PLUGIN_FILE_NAME)), true);
    assert.equal(fs.readFileSync(userPlugin, "utf8"), "export const UserPlugin = async () => ({});\n");

    const second = syncMiMoCodePlugin({ configDir, sourcePath: PLUGIN_SOURCE });
    assert.equal(second.added, 0);
    assert.equal(second.updated, 0);
    assert.equal(second.skipped, 1);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});

test("MiMoCode official lifecycle events publish working, waiting, and done states", async (t) => {
  const received = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => {
      assert.equal(capability.authorize(req, ["hook"]), true);
      received.push(JSON.parse(Buffer.concat(chunks).toString("utf8")));
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end('{"ok":true}');
    });
  });
  t.after(async () => {
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  const capRoot = fs.mkdtempSync(path.join(os.tmpdir(), "mimo-capability-"));
  const capDir = path.join(capRoot, "capability");
  const oldCapDir = process.env.HACHIMODOCK_BRIDGE_CAPABILITY_DIR;
  process.env.HACHIMODOCK_BRIDGE_CAPABILITY_DIR = capDir;
  const capability = capabilityApi.createCapability();
  const previousPort = process.env.PET_MANAGER_BRIDGE_PORT;
  process.env.PET_MANAGER_BRIDGE_PORT = String(address.port);

  try {
    capability.publish({ hookPort: address.port });
    const source = fs.readFileSync(PLUGIN_SOURCE, "utf8").replace(JSON.stringify("__HACHIMO_CAPABILITY_MODULE__"), JSON.stringify(require.resolve("../packages/agent-session-bus/src/local-capability")));
    const dataUrl = `data:text/javascript;base64,${Buffer.from(
      `${source}\n// test-${Date.now()}`,
      "utf8",
    ).toString("base64")}`;
    const module = await import(dataUrl);
    const hooks = await module.PetManagerMiMoCodePlugin({ directory: "D:/repo" });
    const sessionID = "mimo-session-1";

    await hooks.event({
      event: {
        type: "session.updated",
        properties: {
          sessionID,
          info: {
            id: sessionID,
            title: "接入 MiMoCode 状态",
            directory: "D:/repo",
          },
        },
      },
    });
    await hooks["session.pre"]({ sessionID, agentID: "build" }, {});
    await hooks.event({
      event: {
        type: "permission.asked",
        properties: {
          id: "permission-1",
          sessionID,
          permission: "bash",
          patterns: [],
          metadata: {},
          always: [],
        },
      },
    });
    await hooks.event({
      event: {
        type: "message.part.updated",
        properties: {
          sessionID,
          part: {
            type: "text",
            text: "MiMoCode 状态桥接完成。",
          },
        },
      },
    });
    await hooks.event({
      event: {
        type: "session.idle",
        properties: { sessionID },
      },
    });

    await waitFor(() => received.length >= 3);
    assert.deepEqual(received.map((item) => item.state), ["working", "waiting_user", "done"]);
    assert.ok(received.every((item) => item.agent_id === "mimocode"));
    assert.ok(received.every((item) => item.session_id === sessionID));
    assert.equal(received[2].display_content, "MiMoCode 状态桥接完成。");
  } finally {
    capability.clear();
    if (oldCapDir === undefined) delete process.env.HACHIMODOCK_BRIDGE_CAPABILITY_DIR; else process.env.HACHIMODOCK_BRIDGE_CAPABILITY_DIR = oldCapDir;
    fs.rmSync(capRoot, { recursive: true, force: true });
    if (previousPort === undefined) delete process.env.PET_MANAGER_BRIDGE_PORT;
    else process.env.PET_MANAGER_BRIDGE_PORT = previousPort;
  }
});

async function waitFor(predicate, timeoutMs = 3000) {
  const started = Date.now();
  while (!predicate()) {
    if (Date.now() - started > timeoutMs) throw new Error("timed out waiting for plugin state");
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
}
