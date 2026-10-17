//! HTTP POST helper built on the `curl` crate.
//!
//! Keeps the system libcurl (the crate links it dynamically), so behaviour and
//! cancellation semantics match the previous C++ implementation while the
//! error handling is Rust's.

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use curl::easy::{Easy, List};

#[derive(Debug)]
pub enum TransportError {
    /// The caller asked to stop mid-transfer.
    Cancelled,
    Timeout(String),
    Network(String),
}

impl std::fmt::Display for TransportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TransportError::Cancelled => f.write_str("cancelled"),
            TransportError::Timeout(message) => f.write_str(message),
            TransportError::Network(message) => f.write_str(message),
        }
    }
}

pub struct Response {
    pub status: u64,
    pub body: String,
    pub headers: String,
}

impl Response {
    /// Case-insensitive header lookup, returning the first match.
    pub fn header(&self, name: &str) -> Option<String> {
        let needle = format!("{}:", name.to_ascii_lowercase());
        for line in self.headers.lines() {
            if line.to_ascii_lowercase().starts_with(&needle) {
                return Some(line[needle.len()..].trim().to_string());
            }
        }
        None
    }
}

/// A reusable HTTP client.
///
/// Each request used to build a fresh `Easy`, and libcurl keeps the connection
/// cache on the easy handle. Building a new one therefore reconnects and
/// re-runs the TLS handshake for *every* recognition. Measured against
/// DashScope that is 90-150 ms of pure overhead per request, which is an order
/// of magnitude more than the entire DSP pipeline (see `examples/pipeline.rs`).
///
/// One `Agent` keeps the connection alive across recognitions. The providers
/// share a process-wide instance, and the worker thread serialises requests,
/// so no locking is needed around the handle.
pub struct Agent {
    easy: Easy,
}

impl Agent {
    pub fn new() -> Self {
        let mut easy = Easy::new();
        configure(&mut easy);
        Self { easy }
    }

    /// The local TCP port of the current connection, or 0 before the first
    /// request. An unchanged value across two requests means libcurl reused
    /// the connection instead of opening a new one.
    pub fn local_port(&self) -> u16 {
        self.easy.local_port().unwrap_or(0)
    }

    /// Posts `body` as JSON with the given extra headers.
    ///
    /// `timeout` bounds this one request; the connection itself is kept.
    pub fn post_json(
        &mut self,
        url: &str,
        headers: &[(&str, &str)],
        body: &str,
        timeout: Duration,
        cancel: &AtomicBool,
    ) -> Result<Response, TransportError> {
        let easy = &mut self.easy;
        easy.url(url)
            .map_err(|e| TransportError::Network(e.to_string()))?;
        easy.post(true)
            .map_err(|e| TransportError::Network(e.to_string()))?;
        easy.post_fields_copy(body.as_bytes())
            .map_err(|e| TransportError::Network(e.to_string()))?;
        easy.timeout(timeout)
            .map_err(|e| TransportError::Network(e.to_string()))?;
        easy.connect_timeout(timeout.min(Duration::from_secs(10)))
            .map_err(|e| TransportError::Network(e.to_string()))?;
        easy.signal(false)
            .map_err(|e| TransportError::Network(e.to_string()))?;
        // CURLOPT_PROGRESSFUNCTION is only invoked when progress reporting is
        // on. libcurl enables it for this easy handle by default, but nothing
        // in the types says so, and if it were off the callback below would
        // never run and a stuck request could only end through its timeout.
        easy.progress(true)
            .map_err(|e| TransportError::Network(e.to_string()))?;

        let mut list = List::new();
        list.append("Content-Type: application/json")
            .map_err(|e| TransportError::Network(e.to_string()))?;
        for (name, value) in headers {
            list.append(&format!("{name}: {value}"))
                .map_err(|e| TransportError::Network(e.to_string()))?;
        }
        easy.http_headers(list)
            .map_err(|e| TransportError::Network(e.to_string()))?;

        let mut response_body: Vec<u8> = Vec::new();
        let mut response_headers = String::new();

        let result = {
            let mut transfer = easy.transfer();
            transfer
                .write_function(|data| {
                    response_body.extend_from_slice(data);
                    Ok(data.len())
                })
                .map_err(|e| TransportError::Network(e.to_string()))?;
            transfer
                .header_function(|data| {
                    response_headers.push_str(&String::from_utf8_lossy(data));
                    true
                })
                .map_err(|e| TransportError::Network(e.to_string()))?;
            transfer
                .progress_function(|_, _, _, _| !cancel.load(Ordering::Relaxed))
                .map_err(|e| TransportError::Network(e.to_string()))?;
            transfer.perform()
        };

        if let Err(error) = result {
            let transport = if error.is_aborted_by_callback() {
                TransportError::Cancelled
            } else if error.is_operation_timedout() {
                TransportError::Timeout(format!("request timed out after {timeout:?}"))
            } else {
                TransportError::Network(error.to_string())
            };
            if !matches!(transport, TransportError::Timeout(_)) {
                // A cancelled or failed transfer leaves the connection in an
                // unknown state, and reusing it would poison later requests.
                // `reset()` keeps libcurl's connection cache but drops every
                // option, so the durable settings have to be re-applied.
                easy.reset();
                configure(&mut self.easy);
            }
            return Err(transport);
        }

        if cancel.load(Ordering::Relaxed) {
            return Err(TransportError::Cancelled);
        }

        let status = easy
            .response_code()
            .map_err(|e| TransportError::Network(e.to_string()))?;

        Ok(Response {
            status: status as u64,
            body: String::from_utf8_lossy(&response_body).into_owned(),
            headers: response_headers,
        })
    }
}

impl Default for Agent {
    fn default() -> Self {
        Self::new()
    }
}

/// Options that must survive `Easy::reset()`.
fn configure(easy: &mut Easy) {
    // Do not wedge a connection the peer silently dropped: a keep-alive socket
    // reused after the server's idle timeout otherwise shows up as a stall.
    let _ = easy.tcp_keepalive(true);
    // A dead peer should be noticed by the TCP stack, not by the user.
    let _ = easy.tcp_keepidle(Duration::from_secs(30));
    let _ = easy.connect_timeout(Duration::from_secs(10));
}

/// The process-wide client. The recognition worker is the only caller, so the
/// `Mutex` is uncontended; it exists to satisfy `Sync`, not for contention.
static AGENT: std::sync::Mutex<Option<Agent>> = std::sync::Mutex::new(None);

/// Drops the shared client, forcing the next request to build a new
/// connection. Used by the A/B benchmark to reproduce the old behaviour.
pub fn reset_agent() {
    let mut slot = AGENT.lock().unwrap_or_else(|e| e.into_inner());
    *slot = None;
}

/// Posts `body` as JSON, reusing the keep-alive connection.
pub fn post_json(
    url: &str,
    headers: &[(&str, &str)],
    body: &str,
    timeout: Duration,
    cancel: &AtomicBool,
) -> Result<Response, TransportError> {
    let mut slot = AGENT.lock().unwrap_or_else(|e| e.into_inner());
    let agent = slot.get_or_insert_with(Agent::new);
    agent.post_json(url, headers, body, timeout, cancel)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_lookup_is_case_insensitive() {
        let response = Response {
            status: 200,
            body: String::new(),
            headers: "HTTP/1.1 200 OK\r\nX-Api-Status-Code: 20000000\r\n\r\n".to_string(),
        };
        assert_eq!(
            response.header("x-api-status-code").as_deref(),
            Some("20000000")
        );
        assert!(response.header("missing").is_none());
    }
}
