"use strict";

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const args = process.argv.slice(2);
if (process.env.FAKE_MIMO_INVOCATIONS) {
  const temp = os.tmpdir();
  fs.appendFileSync(process.env.FAKE_MIMO_INVOCATIONS, JSON.stringify({ args, temp }) + "\n");
  fs.writeFileSync(path.join(temp, ".fake-opentui-library"), "temporary native library");
}
if (process.env.FAKE_MIMO_FAIL) process.exit(2);

if (args.includes("--version")) {
  process.stdout.write(`${process.env.FAKE_MIMO_VERSION || "0.1.6"}\n`);
  process.exit(0);
}

if (args[0] === "session" && args[1] === "list") {
  process.stdout.write(process.env.FAKE_MIMO_SESSIONS || "[]");
  process.stdout.write("\n");
  process.exit(0);
}

process.stderr.write(`unsupported fake mimo args: ${args.join(" ")}\n`);
process.exit(2);
