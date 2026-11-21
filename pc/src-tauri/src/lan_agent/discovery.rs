use std::net::Ipv4Addr;

/// Use each configured subnet's broadcast, including Ethernet and Wi-Fi.
/// The limited broadcast alone can fail with EHOSTUNREACH on macOS.
pub(super) fn broadcast_targets(networks: impl IntoIterator<Item = (Ipv4Addr, u8)>) -> Vec<Ipv4Addr> {
    let mut targets = Vec::new();
    for (ip, prefix) in networks {
        if !(1..=30).contains(&prefix) || ip.is_loopback() || ip.is_unspecified()
            || ip.is_multicast() || ip.is_broadcast() { continue; }
        let host_mask = u32::MAX >> prefix;
        let broadcast = Ipv4Addr::from(u32::from(ip) | host_mask);
        if !targets.contains(&broadcast) { targets.push(broadcast); }
        if targets.len() == 32 { break; }
    }
    if !targets.contains(&Ipv4Addr::BROADCAST) { targets.push(Ipv4Addr::BROADCAST); }
    targets
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn broadcasts_follow_actual_prefix_and_deduplicate_interfaces() {
        assert_eq!(broadcast_targets([
            (Ipv4Addr::new(192,168,31,118),24),
            (Ipv4Addr::new(192,168,31,119),24),
            (Ipv4Addr::new(10,20,4,5),16),
            (Ipv4Addr::new(172,16,5,9),30),
        ]), vec![Ipv4Addr::new(192,168,31,255),Ipv4Addr::new(10,20,255,255),Ipv4Addr::new(172,16,5,11),Ipv4Addr::BROADCAST]);
    }
    #[test]
    fn excludes_loopback_tunnel_hosts_and_invalid_prefixes() {
        assert_eq!(broadcast_targets([
            (Ipv4Addr::LOCALHOST,8), (Ipv4Addr::UNSPECIFIED,24),
            (Ipv4Addr::new(10,1,1,1),32), (Ipv4Addr::new(10,1,1,1),31),
            (Ipv4Addr::new(10,1,1,1),0), (Ipv4Addr::new(10,1,1,1),33),
        ]),vec![Ipv4Addr::BROADCAST]);
    }
}
