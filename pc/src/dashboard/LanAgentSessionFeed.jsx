import { useEffect, useState } from "react";
import { useAgentSessionFeed } from "./useAgentSessionFeed.js";
import { useP4SessionSync } from "./useP4SessionSync.js";
import { LAN_AGENT_PAIRING_UPDATED, readLanAgentPairing } from "./lanAgentPairing.js";

const NO_DISMISSALS = Object.freeze({});

export default function LanAgentSessionFeed({ agentId, usbConnected }) {
  const [paired, setPaired] = useState(false);
  useEffect(() => {
    let current = true;
    const update = (event) => {
      current = false;
      setPaired(Boolean(event.detail));
    };
    window.addEventListener(LAN_AGENT_PAIRING_UPDATED, update);
    readLanAgentPairing().then((address) => {
      if (current) setPaired(Boolean(address));
    }).catch(() => {});
    return () => {
      current = false;
      window.removeEventListener(LAN_AGENT_PAIRING_UPDATED, update);
    };
  }, []);

  const followedAgent = paired && !usbConnected ? agentId || "codex" : "";
  const feed = useAgentSessionFeed({
    agentId: followedAgent,
    displayEnabled: true,
    dismissedSessions: NO_DISMISSALS,
  });
  useP4SessionSync({
    enabled: false,
    agentId: followedAgent,
    usbConnected,
    displayEnabled: true,
    sessions: feed.sessions,
    routingSessions: feed.routingSessions,
    sessionsLoaded: feed.loaded,
    dismissedSessions: NO_DISMISSALS,
  });
  return null;
}
