//! Proves the shared client keeps one TCP connection open across requests.
//! Prints the local port libcurl is using for each request; an unchanged port
//! across requests means the connection was reused rather than re-established.
use std::time::Duration;

fn main() {
    let cancel = std::sync::atomic::AtomicBool::new(false);
    let mut agent = ainput_core::http::Agent::new();
    for index in 1..=3 {
        agent
            .post_json(
                "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation",
                &[("Content-Type", "application/json")],
                "{}",
                Duration::from_secs(20),
                &cancel,
            )
            .ok();
        println!("request {index}: local_port={:?}", agent.local_port());
    }
}
