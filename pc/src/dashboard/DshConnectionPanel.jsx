import React, { useEffect, useRef, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { createDshConnectionController } from '../lib/dsh-connection.js';

export function useDshFollowConnection(agentId) {
  const [state, setState] = useState({ phase: 'idle', message: '' });
  const controller = useRef(null);
  if (!controller.current) controller.current = createDshConnectionController(invoke, setState);
  useEffect(() => {
    const owner = controller.current;
    void owner.select(agentId);
    return () => owner.retire();
  }, [agentId]);
  return { ...state, retry: controller.current.retry };
}

// Optional informational view only. Device voice uses the shared desktop
// composer adapter; plugin connectivity is never an input prerequisite.
export default function DshConnectionPanel({ connection }) {
  if (!['pending', 'error'].includes(connection?.phase)) return null;
  return <section className="voice-panel__advanced" aria-label="DeepSeek 会话跟随">
    <strong>DeepSeek 会话跟随</strong>
    <p role={connection.phase === 'error' ? 'alert' : 'status'}>{connection.message}</p>
    {connection.phase === 'error' && <button type="button" onClick={connection.retry}>重试连接</button>}
  </section>;
}
