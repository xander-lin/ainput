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

/// Posts `body` as JSON with the given extra headers.
pub fn post_json(
    url: &str,
    headers: &[(&str, &str)],
    body: &str,
    timeout: Duration,
    cancel: &AtomicBool,
) -> Result<Response, TransportError> {
    let mut easy = Easy::new();
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
        return Err(if error.is_aborted_by_callback() {
            TransportError::Cancelled
        } else if error.is_operation_timedout() {
            TransportError::Timeout(format!("request timed out after {timeout:?}"))
        } else {
            TransportError::Network(error.to_string())
        });
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
