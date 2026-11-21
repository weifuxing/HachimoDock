/// Match the device's HTTPS GET endpoint grammar. The device also checks the
/// resolved address and validates the certificate against the original host.
pub(super) fn valid(url: &str) -> bool {
    let Some(endpoint) = url.strip_prefix("https://") else {
        return false;
    };
    let host_end = endpoint.find(['/', '?']).unwrap_or(endpoint.len());
    let (host, path) = endpoint.split_at(host_end);
    if host.len() > 127 || !host.contains('.') || !host.rsplit('.').next()
        .is_some_and(|label| label.bytes().any(|byte| byte.is_ascii_alphabetic()))
    {
        return false;
    }
    let valid_host = host.split('.').all(|label| {
        (1..=63).contains(&label.len()) && !label.starts_with('-') && !label.ends_with('-')
            && label.bytes().all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
    });
    valid_host && !path.starts_with("//") && path.len() + usize::from(!path.starts_with('/')) <= 255
        && path.bytes().all(|byte| (b'!'..=b'~').contains(&byte) && !matches!(byte, b'#' | b'\\'))
}
