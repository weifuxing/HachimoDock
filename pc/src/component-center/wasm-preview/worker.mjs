import { createRuntime } from "./runtime.mjs";

let run;
self.onmessage = async ({ data }) => {
  try {
    if (data.type === "load") {
      run = await createRuntime(data.bytes, data.slots, data.clockUnsynced ? { now: () => null } : {});
      const result = run("init", []);
      if ((result.width === 640) !== (data.canvas === "640x480-v1")) {
        throw new Error("WASM 模块与组件包的画布声明不一致");
      }
      if (result.hostProfile !== data.host) {
        throw new Error("WASM 模块与组件包的宿主能力声明不一致");
      }
      if (result.audioProfile !== data.audioProfile) {
        throw new Error("WASM 模块与组件包的离线语音声明不一致");
      }
      self.postMessage({ result });
    } else if (data.type === "tick") {
      const results = data.events.map(args => run("event", args));
      results.push(run("frame", [data.dt]));
      self.postMessage({ results });
    }
  } catch (error) {
    self.postMessage({ error: error.message });
  }
};
