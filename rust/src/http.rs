//! HTTP POST helper built on the `curl` crate.
//!
//! Keeps the system libcurl (the crate links it dynamically), so behaviour and
//! cancellation semantics match the previous C++ implementation while the
//! error handling is Rust's.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use crate::network::{Network, SystemNetwork};
use curl::easy::{Easy, List};

#[derive(Debug)]
pub enum TransportError {
    /// The caller asked to stop mid-transfer.
    Cancelled,
    NetworkChanged,
    Timeout(String),
    Network(String),
}

impl std::fmt::Display for TransportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TransportError::Cancelled => f.write_str("cancelled"),
            TransportError::NetworkChanged => f.write_str("network changed during the request"),
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
/// An agent is checked out exclusively for one request and can be reused on
/// the same network after a successful transfer.
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
        self.post_json_while(url, headers, body, timeout, || {
            !cancel.load(Ordering::Acquire)
        })
    }

    fn post_json_while(
        &mut self,
        url: &str,
        headers: &[(&str, &str)],
        body: &str,
        timeout: Duration,
        keep_running: impl Fn() -> bool,
    ) -> Result<Response, TransportError> {
        if !keep_running() {
            return Err(TransportError::Cancelled);
        }
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
                .progress_function(|_, _, _, _| keep_running())
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
            return Err(transport);
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

fn configure(easy: &mut Easy) {
    // Do not wedge a connection the peer silently dropped: a keep-alive socket
    // reused after the server's idle timeout otherwise shows up as a stall.
    let _ = easy.tcp_keepalive(true);
    // A dead peer should be noticed by the TCP stack, not by the user.
    let _ = easy.tcp_keepidle(Duration::from_secs(30));
    let _ = easy.connect_timeout(Duration::from_secs(10));
}

pub struct Transport {
    network: Arc<dyn Network>,
    idle: Mutex<Vec<(u64, Agent)>>,
}

impl Transport {
    pub(crate) fn system() -> Self {
        Self::new(Arc::new(SystemNetwork::new()))
    }

    pub(crate) fn new(network: Arc<dyn Network>) -> Self {
        Self {
            network,
            idle: Mutex::new(Vec::new()),
        }
    }

    pub(crate) fn post_json(
        &self,
        id: u64,
        stage: &str,
        url: &str,
        headers: &[(&str, &str)],
        body: &str,
        timeout: Duration,
        cancel: &AtomicBool,
    ) -> Result<Response, TransportError> {
        if cancel.load(Ordering::Acquire) {
            return Err(TransportError::Cancelled);
        }
        let (generation, agent) = {
            let mut idle = self.idle.lock().unwrap();
            let generation = self.network.generation();
            idle.retain(|(epoch, _)| *epoch == generation);
            (generation, idle.pop().map(|(_, agent)| agent))
        };
        let mut agent = agent.unwrap_or_default();
        if id != 0 {
            crate::diagnostic::trace(id, stage);
        }
        // Route messages also describe unrelated interface changes and route
        // renewals. Keep a working transfer alive; only its reuse is unsafe.
        let result = agent.post_json(
            url,
            headers,
            body,
            timeout.max(Duration::from_millis(1)),
            cancel,
        );
        let same_network = self.network.generation() == generation;
        if cancel.load(Ordering::Acquire) {
            return Err(TransportError::Cancelled);
        }
        if result.is_err() && !same_network {
            return Err(TransportError::NetworkChanged);
        }
        if result.is_ok() && same_network {
            let mut idle = self.idle.lock().unwrap();
            if idle.len() < 3 {
                idle.push((generation, agent));
            }
        }
        // On any transport error drop the actual handle (and its caches).
        result
    }
}

pub(crate) fn default_transport() -> &'static Transport {
    static TRANSPORT: OnceLock<Transport> = OnceLock::new();
    TRANSPORT.get_or_init(Transport::system)
}

/// Drops the shared client, forcing the next request to build a new
/// connection. Used by the A/B benchmark to reproduce the old behaviour.
pub fn reset_agent() {
    default_transport().idle.lock().unwrap().clear();
}

/// Posts `body` as JSON, reusing the keep-alive connection.
pub fn post_json(
    url: &str,
    headers: &[(&str, &str)],
    body: &str,
    timeout: Duration,
    cancel: &AtomicBool,
) -> Result<Response, TransportError> {
    default_transport().post_json(0, "http", url, headers, body, timeout, cancel)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    pub fn read_request(stream: &mut std::net::TcpStream) -> String {
        use std::io::Read;
        stream
            .set_read_timeout(Some(Duration::from_secs(4)))
            .unwrap();
        let mut bytes = Vec::new();
        let mut byte = [0];
        while !bytes.ends_with(b"\r\n\r\n") {
            stream.read_exact(&mut byte).unwrap();
            bytes.push(byte[0]);
        }
        let headers = String::from_utf8(bytes).unwrap();
        let length = headers
            .lines()
            .find_map(|line| {
                let (name, value) = line.split_once(':')?;
                name.eq_ignore_ascii_case("Content-Length")
                    .then(|| value.trim().parse().unwrap())
            })
            .unwrap_or(0);
        stream.read_exact(&mut vec![0; length]).unwrap();
        headers
    }

    #[test]
    fn network_change_discards_reused_connection_and_unblocks_new_request() {
        use std::io::Write;
        use std::net::TcpListener;
        use std::sync::mpsc;
        use std::thread;
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let url = format!("http://{}/recognize", listener.local_addr().unwrap());
        let network = Arc::new(crate::network::MockNetwork::default());
        let transport = Arc::new(Transport::new(network.clone()));
        let (stalling, stalled) = mpsc::channel();
        let (release, hold) = mpsc::channel();
        let server = thread::spawn(move || {
            let (mut old, _) = listener.accept().unwrap();
            read_request(&mut old);
            old.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nold")
                .unwrap();
            read_request(&mut old); // Must reuse the warmed connection.
            stalling.send(()).unwrap();
            let (mut fresh, _) = listener.accept().unwrap();
            read_request(&mut fresh);
            fresh
                .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nnew")
                .unwrap();
            hold.recv_timeout(Duration::from_secs(4)).unwrap();
            old.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nkept")
                .unwrap();
            read_request(&mut fresh); // The obsolete connection must stay out of the pool.
            fresh
                .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nstill")
                .unwrap();
        });
        let post = |transport: &Transport, url: &str| {
            transport.post_json(
                0,
                "http",
                url,
                &[],
                "{}",
                Duration::from_secs(3),
                &AtomicBool::new(false),
            )
        };
        assert_eq!(post(&transport, &url).unwrap().body, "old");
        let old = {
            let transport = transport.clone();
            let url = url.clone();
            thread::spawn(move || post(&transport, &url))
        };
        stalled.recv_timeout(Duration::from_secs(3)).unwrap();
        network.0.fetch_add(1, Ordering::SeqCst);
        assert_eq!(post(&transport, &url).unwrap().body, "new");
        release.send(()).unwrap();
        assert_eq!(old.join().unwrap().unwrap().body, "kept");
        assert_eq!(post(&transport, &url).unwrap().body, "still");
        server.join().unwrap();
    }

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
