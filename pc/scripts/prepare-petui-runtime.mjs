// Stage the pinned WASM toolchain. The desktop ships no Python interpreter.
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  copyFileSync, createReadStream, createWriteStream, existsSync, lstatSync, mkdirSync, mkdtempSync,
  readFileSync, readdirSync, renameSync, rmSync, writeFileSync,
} from "node:fs";
import { homedir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";
import { fileURLToPath } from "node:url";

const SDK_BASE = "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/";
const SDK = {
  "macos-arm64": ["wasi-sdk-27.0-arm64-macos.tar.gz", "055c3dc2766772c38e71a05d353e35c322c7b2c6458a36a26a836f9808a550f8"],
  "macos-x64": ["wasi-sdk-27.0-x86_64-macos.tar.gz", "163dfd47f989b1a682744c1ae1f0e09a83ff5c4bbac9dcd8546909ab54cda5a1"],
  "windows-x64": ["wasi-sdk-27.0-x86_64-windows.tar.gz", "4a576c13125c91996d8cc3b70b7ea0612c2044598d2795c9be100d15f874adf6"],
};
const CRT = [
  "https://download.microsoft.com/download/4/7/c/47c6134b-d61f-4024-83bd-b9c9ea951c25/Microsoft.VCLibs.x64.14.00.Desktop.appx",
  "b56a9101f706f9d95f815f5b7fa6efbac972e86573d378b96a07cff5540c5961",
];
const WAMR_REVISION = "47c7c85ac21f240a878026af603e032065d183fb";
const WAMR = [
  `https://codeload.github.com/bytecodealliance/wasm-micro-runtime/tar.gz/${WAMR_REVISION}`,
  "fb5b7bd3f9d09e882463fe126314925af748c1b84caaea83ac15dea830602cef",
];
const firmware = fileURLToPath(new URL("../../firmware/", import.meta.url));
const checkerSource = join(firmware, "tools/wasm-host-check");
const checkerInputs = [
  fileURLToPath(import.meta.url),
  join(checkerSource, "CMakeLists.txt"), join(checkerSource, "main.c"),
  ...["hachi_runtime.c", "hachi_runtime.h", "wamr-options.cmake"].map((name) =>
    join(firmware, "components/hachi_wamr", name)),
];
const checkerDigest = createHash("sha256").update(WAMR_REVISION);
for (const input of checkerInputs) checkerDigest.update(readFileSync(input));
const VERSION = `wasi27-v4-${checkerDigest.digest("hex").slice(0, 12)}`;

async function buildChecker(cache, scratch, stage, target, name) {
  const source = await archive(cache, ...WAMR);
  // The archive is content-pinned above. Extract outside the runtime bundle.
  execFileSync("tar", ["--exclude", "*/language-bindings/python/LICENSE", "-xf", source, "-C", scratch], { stdio: "inherit" });
  const wamr = join(scratch, `wasm-micro-runtime-${WAMR_REVISION}`);
  const build = join(scratch, "wasm-checker");
  const windows = target.startsWith("windows");
  const options = ["-DCMAKE_BUILD_TYPE=Release"];
  if (windows && process.platform === "win32") {
    options.push("-A", "x64", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded");
  } else if (windows) {
    options.push("-DCMAKE_SYSTEM_NAME=Windows", "-DWAMR_BUILD_TARGET=X86_64",
      "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc", "-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++",
      "-DCMAKE_ASM_COMPILER=x86_64-w64-mingw32-gcc", "-DCMAKE_EXE_LINKER_FLAGS=-static");
    // WAMR uses GNU assembly under MinGW and does not need MASM.
    const cmake = join(wamr, "build-scripts/runtime_lib.cmake");
    writeFileSync(cmake, readFileSync(cmake, "utf8").replace(
      'if (WAMR_BUILD_PLATFORM STREQUAL "windows")\n    enable_language (ASM_MASM)',
      'if (WAMR_BUILD_PLATFORM STREQUAL "windows" AND NOT MINGW)\n    enable_language (ASM_MASM)',
    ));
  } else {
    const arm = target.endsWith("arm64");
    options.push(`-DCMAKE_OSX_ARCHITECTURES=${arm ? "arm64" : "x86_64"}`,
      `-DWAMR_BUILD_TARGET=${arm ? "AARCH64" : "X86_64"}`);
  }
  execFileSync("cmake", ["-S", checkerSource, "-B", build, ...options], {
    env: { ...process.env, WAMR_ROOT: wamr }, stdio: "inherit", timeout: 120_000,
  });
  execFileSync("cmake", ["--build", build, "--config", "Release", "--parallel", "4"], {
    stdio: "inherit", timeout: 180_000,
  });
  const candidate = [join(build, name), join(build, "Release", name)].find(existsSync);
  if (!candidate) throw new Error("找不到构建完成的固件同源 WASM 检查器");
  copyFileSync(candidate, join(stage, "bin", name));
  copyFileSync(join(wamr, "LICENSE"), join(stage, "WAMR-LICENSE.txt"));
}

async function sha(path) {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(path)) hash.update(chunk);
  return hash.digest("hex");
}

async function archive(cache, url, expected) {
  const destination = join(cache, url.split("/").at(-1));
  if (existsSync(destination) && await sha(destination) === expected) return destination;
  const scratch = mkdtempSync(join(cache, ".download-"));
  const download = join(scratch, "archive");
  try {
    const response = await fetch(url, { signal: AbortSignal.timeout(300_000) });
    if (!response.ok || !response.body) throw new Error(`无法下载 PetUI 工具归档（HTTP ${response.status}）`);
    await pipeline(Readable.fromWeb(response.body), createWriteStream(download));
    if (await sha(download) !== expected) throw new Error("PetUI 工具归档校验失败");
    renameSync(download, destination);
    return destination;
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

function entries(root, prefix = "") {
  return readdirSync(join(root, prefix), { withFileTypes: true }).flatMap((entry) => {
    const path = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isDirectory()) return entries(root, path);
    if (!entry.isFile()) throw new Error(`PetUI 工具含不支持的文件：${path}`);
    return [path];
  }).sort();
}

async function fingerprints(root) {
  const files = {};
  for (const name of entries(root)) {
    if (name !== "manifest.json") files[name] = await sha(join(root, name));
  }
  return files;
}

function extract(archivePath, members, scratch) {
  const names = join(scratch, "members.txt");
  writeFileSync(names, members.join("\n") + "\n");
  execFileSync("tar", ["-xf", archivePath, "-C", scratch, "-T", names], { stdio: "inherit" });
}

async function prepare(target, output) {
  const [sdkName, sdkHash] = SDK[target];
  const sdkUrl = SDK_BASE + sdkName;
  const windows = target.startsWith("windows");
  const suffix = windows ? ".exe" : "";
  const binaries = [`bin/clang${suffix}`, `bin/wasm-ld${suffix}`];
  const checker = `petui-wasm-check${suffix}`;
  try {
    const manifest = JSON.parse(readFileSync(join(output, "manifest.json"), "utf8"));
    if (manifest.version === VERSION && manifest.target === target
      && !existsSync(join(output, "python")) && [...binaries, `bin/${checker}`].every((name) => manifest.files[name])
      && JSON.stringify(await fingerprints(output)) === JSON.stringify(manifest.files)) return;
  } catch {
    // Missing, old or invalid stages are replaced only after a complete new stage.
  }

  const cache = resolve(process.env.PETUI_AUTHORING_CACHE || join(homedir(), ".cache/petui-build"));
  mkdirSync(cache, { recursive: true });
  const sdk = await archive(cache, sdkUrl, sdkHash);
  const crt = windows ? await archive(cache, ...CRT) : null;
  mkdirSync(dirname(output), { recursive: true });
  const scratch = mkdtempSync(join(dirname(output), ".petui-"));
  const stage = join(scratch, "runtime");
  try {
    const members = execFileSync("tar", ["-tf", sdk], { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 }).trim().split(/\r?\n/);
    const root = members[0].split("/")[0];
    if (!/^[\w.-]+$/.test(root) || root === "." || root === "..") throw new Error("PetUI 工具归档根目录无效");
    const headers = members.filter((name) => name.startsWith(`${root}/lib/clang/`)
      && /^lib\/clang\/[\w.-]+\/include\/[\w./+-]+$/.test(name.slice(root.length + 1))
      && !name.endsWith("/") && !name.split("/").includes(".."));
    if (!headers.some((name) => name.endsWith("/stdint.h"))) throw new Error("PetUI 工具归档缺少 Clang 头文件");
    const libraries = windows ? [] : ["lib/libclang-cpp.dylib", "lib/libLLVM.dylib"];
    const toolMembers = windows ? binaries : ["bin/clang-20", "bin/lld"];
    const selected = [...toolMembers, ...libraries, ...headers.map((name) => name.slice(root.length + 1))];
    extract(sdk, selected.map((name) => `${root}/${name}`), scratch);
    for (const [index, name] of selected.entries()) {
      const path = join(scratch, root, name);
      if (!lstatSync(path).isFile()) throw new Error(`PetUI 工具归档不是普通文件：${name}`);
      const dest = join(stage, index < binaries.length ? binaries[index] : name);
      mkdirSync(dirname(dest), { recursive: true });
      copyFileSync(path, dest);
    }

    if (crt) {
      const dlls = ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"];
      extract(crt, dlls, scratch);
      for (const name of dlls) copyFileSync(join(scratch, name), join(stage, "bin", name));
    }
    await buildChecker(cache, scratch, stage, target, checker);
    writeFileSync(join(stage, "UPSTREAM.txt"), [sdkUrl, WAMR[0], ...(crt ? [CRT[0]] : [])].join("\n") + "\n");
    copyFileSync(fileURLToPath(new URL("./petui-llvm-LICENSE.txt", import.meta.url)), join(stage, "LLVM-LICENSE.txt"));
    writeFileSync(join(stage, "manifest.json"), JSON.stringify({ version: VERSION, target, files: await fingerprints(stage) }));
    if (existsSync(output)) renameSync(output, join(scratch, "old"));
    try {
      renameSync(stage, output);
    } catch (error) {
      if (existsSync(join(scratch, "old"))) renameSync(join(scratch, "old"), output);
      throw error;
    }
  } finally {
    // On rollback failure keep the previous complete directory recoverable.
    if (!existsSync(join(scratch, "old")) || existsSync(output)) rmSync(scratch, { recursive: true, force: true });
  }
}

const [target, output] = process.argv.slice(2);
if (!Object.hasOwn(SDK, target) || !output) throw new Error("Usage: prepare-petui-runtime.mjs <target> <output>");
await prepare(target, resolve(output));
