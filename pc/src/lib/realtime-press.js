// Frontend acceptance is explicit. Repeated offers never repeat one key action;
// errors remain retryable until native's original boot-bound TTL expires.
export function createRealtimePressHandler({ accept, configure, onError = () => {} }) {
  let disposed = false, busy = false;
  const completed = new Set();
  const current = () => !disposed;
  const handle = async ({ payload: token } = {}) => {
    if (disposed || busy || !token || !/^p4-[0-9a-f]{12}$/.test(token.boardDeviceId || "")
      || !/^[0-9a-f]{16}$/.test(token.bootId || "") || !Number.isInteger(token.seq)
      || token.seq <= 0 || token.seq > 0xffffffff) return;
    const key = `${token.boardDeviceId}:${token.bootId}:${token.seq}`;
    if (completed.has(key)) return;
    busy = true;
    try {
      const result = await accept(token);
      if (disposed) return;
      if (result === null) await configure(token, current);
      if (disposed) return;
      completed.add(key);
      if (completed.size > 16) completed.delete(completed.values().next().value);
    } catch (error) {
      if (!disposed) onError(error);
    } finally { busy = false; }
  };
  return { handle, dispose() { disposed = true; completed.clear(); } };
}
