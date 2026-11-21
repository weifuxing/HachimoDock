"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");
const { DevinAdapter, devinPaths } = require("../src/adapters/devin");

test("Devin paths use the global config directory and standard Windows install locations", () => {
  const paths = devinPaths({
    APPDATA: "/fixture/roaming",
    LOCALAPPDATA: "/fixture/local",
    ProgramFiles: "/fixture/programs",
  }, "/fixture/home");
  assert.equal(paths.root, path.join("/fixture/roaming", "devin"));
  assert.ok(paths.apps.includes(path.join("/fixture/local", "Programs", "Devin", "devin.exe")));
  assert.ok(paths.apps.includes(path.join("/fixture/programs", "Devin", "devin.exe")));
});

test("Devin adapter availability recognizes cloud config or an installed desktop app", async () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "devin-adapter-"));
  try {
    const cloud = new DevinAdapter({ env: {}, home, cloudEnabled: true });
    assert.equal((await cloud.isAvailable()).ready, true);

    const local = path.join(home, "LocalAppData");
    const executable = path.join(local, "Programs", "Devin", "devin.exe");
    fs.mkdirSync(path.dirname(executable), { recursive: true });
    fs.writeFileSync(executable, "");
    const desktop = new DevinAdapter({
      env: { LOCALAPPDATA: local },
      home: path.join(home, "no-config"),
    });
    assert.equal((await desktop.isAvailable()).ready, true);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});

test("Devin adapter refuses background injection with an agent-specific message", async () => {
  const adapter = new DevinAdapter({ env: {}, home: "/fixture/no-devin" });
  const events = [];
  for await (const event of adapter.inject({ text: "draft" })) events.push(event);
  assert.equal(events[0].code, "AGENT_INPUT_UNSUPPORTED");
  assert.match(events[0].message, /Devin/);
});
