#!/usr/bin/env node
"use strict";
// Command hooks load the current epoch for each request; settings never contain a token.
const http = require("node:http");
const { readDescriptor, descriptorPath, headersFor } = require("../packages/agent-session-bus/src/local-capability");
let input = "", finished = false;
function finish(value = {}) { if (finished) return; finished = true; process.stdout.write(JSON.stringify(value)); }
const deadline = setTimeout(() => { finish(); process.exit(0); }, 10000);
process.stdin.setEncoding("utf8");
process.stdin.on("data", chunk => {
  input += chunk;
  if (Buffer.byteLength(input) > 512 * 1024) { finish(); process.exit(0); }
});
process.stdin.on("end", () => {
  try {
    JSON.parse(input);
    const { hookPort } = readDescriptor(descriptorPath("hook"));
    const req = http.request({ hostname: "127.0.0.1", port: hookPort, path: "/permission", method: "POST",
      headers: { "Content-Type": "application/json", ...headersFor("hook", hookPort) }, timeout: 8000 }, res => {
      let raw = "";
      res.setEncoding("utf8");
      res.on("data", chunk => { raw += chunk; if (raw.length > 65536) { res.destroy(); finish(); } });
      res.on("end", () => {
        try { finish(res.statusCode === 200 ? JSON.parse(raw) : {}); } catch { finish(); }
        clearTimeout(deadline);
      });
      res.on("error", () => { finish(); clearTimeout(deadline); });
    });
    req.on("timeout", () => req.destroy());
    req.on("error", () => { finish(); clearTimeout(deadline); });
    req.end(input);
  } catch { finish(); clearTimeout(deadline); }
});
