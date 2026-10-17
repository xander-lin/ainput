//! Manual smoke test against a real cloud backend.
//!
//! Ignored by default: it needs a configured API key and network access, and
//! it spends a (tiny) amount of the account's quota.
//!
//!   AINPUT_SMOKE_WAV=/path/to/speech.wav \
//!     cargo test --test cloud_smoke -- --ignored --nocapture

#[test]
#[ignore]
fn transcribes_a_real_file() {
    let wav = std::env::var("AINPUT_SMOKE_WAV")
        .expect("set AINPUT_SMOKE_WAV to a 16 kHz mono WAV with speech");
    let provider_id = std::env::var("AINPUT_SMOKE_PROVIDER").unwrap_or_else(|_| "qwen".into());
    let path = std::path::PathBuf::from(wav);
    assert!(path.is_file(), "no such file: {}", path.display());

    let provider = ainput_core::smoke::create(&provider_id).expect("unknown provider");
    let cancel = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    // Optional: test the GUI-provided overrides without touching configtool.
    let credentials = ainput_core::smoke::Credentials {
        qwen_api_key: std::env::var("AINPUT_SMOKE_QWEN_KEY").unwrap_or_default(),
        qwen_model: std::env::var("AINPUT_SMOKE_QWEN_MODEL").unwrap_or_default(),
        doubao_api_key: std::env::var("AINPUT_SMOKE_DOUBAO_KEY").unwrap_or_default(),
        doubao_model: std::env::var("AINPUT_SMOKE_DOUBAO_MODEL").unwrap_or_default(),
    };
    let result = provider.transcribe(&ainput_core::smoke::Input {
        wav_path: &path,
        cancel: &cancel,
        credentials: &credentials,
    });
    match result {
        Ok(text) => println!("OK [{provider_id}] -> {text}"),
        Err(err) => panic!("FAIL [{provider_id}] {} : {}", err.category.as_str(), err.detail),
    }
}
