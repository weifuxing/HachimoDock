// DeepSeek follow setup only. No transcript, credential or device access.
export function createDshConnectionController(invoke, notify = () => {}) {
  let selected = "";
  let revision = 0;
  let inFlight = null;
  let state = { phase: "idle", message: "" };
  const publish = (next) => { state = next; notify(next); };
  const run = () => {
    if (selected !== "deepseek-harness") return Promise.resolve();
    if (inFlight) return inFlight;
    const owner = revision;
    publish({ phase: "pending", message: "正在配置 DeepSeek 会话跟随…" });
    const request = Promise.resolve().then(() => {
      if (owner !== revision || selected !== "deepseek-harness") return;
      return invoke("connect_dsh_desktop");
    });
    inFlight = request.then((receipt) => {
      if (owner !== revision) return;
      if (receipt?.ok !== true) throw new Error("installation-unconfirmed");
      publish({ phase: "configured", message: "连接组件已配置。若尚无气泡，请重启 DeepSeek 后重试连接。" });
    }).catch(() => {
      if (owner === revision) publish({ phase: "error", message: "DeepSeek 会话跟随未接通。请先打开桌面版，再点“重试连接”。" });
    }).finally(() => {
      inFlight = null;
      // Switching away and back during one install must serialize, not lose
      // the new selection or launch two writers against the same profile.
      if (owner !== revision && selected === "deepseek-harness") void run();
    });
    return inFlight;
  };
  return {
    select(agentId) {
      if (agentId === selected) return inFlight || Promise.resolve();
      selected = agentId; revision++;
      if (agentId !== "deepseek-harness") {
        publish({ phase: "idle", message: "" });
        return Promise.resolve();
      }
      return run();
    },
    retry: run,
    // A submitted native install finishes under the existing serialization;
    // only its retired UI publication and not-yet-submitted work are cancelled.
    retire() {
      selected = "";
      revision++;
      state = { phase: "idle", message: "" };
    },
    getState: () => state,
  };
}
