//! Engine orchestration: captures recordings, serialises recognition, and
//! calls back into the C++ shell.
//!
//! Threading model:
//!   - one capture thread per recording (it preprocesses and writes the WAV)
//!   - one recognition worker for the whole process (strict FIFO)
//!
//! At most `MAX_PENDING` recordings are accepted, counting the one currently
//! being recognized. This replaces the old per-provider worker threads: one
//! queue means one place where "is something in flight?" is answered.

use std::collections::HashMap;
use std::ffi::{c_char, c_void, CString};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::mpsc::{self, Sender};
use std::sync::{Arc, Mutex, RwLock};
use std::thread::JoinHandle;

use crate::asr;
use crate::capture::{self, Recording};
use crate::diagnostic;
use crate::error::{Category, Error};
use crate::settings::Settings;
use crate::uinput;

pub const MAX_PENDING: usize = 3;

#[derive(Clone, Copy)]
pub struct Callbacks {
    pub user: *mut c_void,
    pub on_text: extern "C" fn(*mut c_void, u64, *const c_char),
    pub on_error: extern "C" fn(*mut c_void, u64, i32, *const c_char),
}

// The shell promises the callbacks are thread-safe; the pointer is only ever
// handed back to the same shell object.
unsafe impl Send for Callbacks {}
unsafe impl Sync for Callbacks {}

impl Callbacks {
    fn text(self, id: u64, text: &str) {
        if let Ok(cstring) = CString::new(text.replace('\0', "")) {
            (self.on_text)(self.user, id, cstring.as_ptr());
        }
    }

    fn error(self, id: u64, error: &Error) {
        if let Ok(cstring) = CString::new(error.detail.replace('\0', "")) {
            (self.on_error)(self.user, id, error.category.as_i32(), cstring.as_ptr());
        }
    }
}

struct Job {
    id: u64,
    wav_path: PathBuf,
    credentials: crate::settings::Credentials,
}

pub struct Engine {
    callbacks: Callbacks,
    settings: RwLock<Settings>,
    next_id: AtomicU64,
    pending: Arc<AtomicUsize>,
    shutting_down: Arc<AtomicBool>,
    recordings: Mutex<HashMap<u64, Recording>>,
    finishing: Mutex<Vec<Recording>>,
    sender: Mutex<Option<Sender<Job>>>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl Engine {
    pub fn new(callbacks: Callbacks) -> Self {
        let (sender, receiver) = mpsc::channel::<Job>();
        let pending = Arc::new(AtomicUsize::new(0));
        let shutting_down = Arc::new(AtomicBool::new(false));

        let worker_pending = pending.clone();
        let worker_shutdown = shutting_down.clone();
        let worker_callbacks = callbacks;

        let worker = std::thread::Builder::new()
            .name("ainput-recognition".to_string())
            .spawn(move || {
                worker_loop(
                    receiver,
                    worker_callbacks,
                    worker_pending,
                    worker_shutdown,
                );
            })
            .ok();

        Self {
            callbacks,
            settings: RwLock::new(Settings::default()),
            next_id: AtomicU64::new(0),
            pending,
            shutting_down,
            recordings: Mutex::new(HashMap::new()),
            finishing: Mutex::new(Vec::new()),
            sender: Mutex::new(Some(sender)),
            worker: Mutex::new(worker),
        }
    }

    pub fn set_settings(&self, settings: Settings) {
        let settings = settings.sanitized();
        diagnostic::event(
            "engine",
            "settings_applied",
            &[
                ("provider", settings.provider.clone()),
                ("denoise", settings.denoise.as_str().to_string()),
                ("crest_threshold", format!("{:.2}", settings.crest_threshold)),
                ("buffer_ms", settings.buffer_ms.to_string()),
            ],
        );
        crate::settings::set_active_credentials(settings.credentials.clone());
        if let Ok(mut slot) = self.settings.write() {
            *slot = settings;
        }
    }

    fn settings_snapshot(&self) -> Settings {
        self.settings
            .read()
            .map(|guard| guard.clone())
            .unwrap_or_default()
    }

    /// Starts a recording. Returns 0 when the queue is full or capture could
    /// not start (the error is already reported through `on_error`).
    pub fn start(&self) -> u64 {
        if self.shutting_down.load(Ordering::Relaxed) {
            return 0;
        }
        let settings = self.settings_snapshot();

        // Reserve a slot before creating anything, so concurrent starts cannot
        // overshoot the limit.
        loop {
            let current = self.pending.load(Ordering::SeqCst);
            if current >= MAX_PENDING {
                let error = Error::runtime("recognition queue full; try again");
                self.callbacks.error(0, &error);
                return 0;
            }
            if self
                .pending
                .compare_exchange(current, current + 1, Ordering::SeqCst, Ordering::SeqCst)
                .is_ok()
            {
                break;
            }
        }

        let id = self.next_id.fetch_add(1, Ordering::SeqCst) + 1;
        let credentials = settings.credentials.clone();
        let callbacks = self.callbacks;
        let pending = self.pending.clone();
        let sender = self
            .sender
            .lock()
            .ok()
            .and_then(|guard| guard.clone());

        let result = capture::spawn(id, settings, move |id, outcome| match outcome {
            Ok(captured) => match sender {
                Some(sender) => {
                    let job = Job {
                        id,
                        wav_path: captured.wav_path.clone(),
                        credentials: credentials.clone(),
                    };
                    if sender.send(job).is_err() {
                        pending.fetch_sub(1, Ordering::SeqCst);
                        let _ = std::fs::remove_file(&captured.wav_path);
                        callbacks.error(id, &Error::runtime("recognition worker is gone"));
                    } else {
                        diagnostic::event(
                            "engine",
                            "recognition_queued",
                            &[
                                ("recognition_id", id.to_string()),
                                (
                                    "samples",
                                    captured.samples.len().to_string(),
                                ),
                                ("loudness", format!("{:.1}", captured.loudness)),
                                (
                                    "capture_ms",
                                    captured.elapsed.as_millis().to_string(),
                                ),
                            ],
                        );
                    }
                }
                None => {
                    pending.fetch_sub(1, Ordering::SeqCst);
                    let _ = std::fs::remove_file(&captured.wav_path);
                }
            },
            Err(error) => {
                pending.fetch_sub(1, Ordering::SeqCst);
                diagnostic::event(
                    "engine",
                    "recognition_aborted",
                    &[
                        ("recognition_id", id.to_string()),
                        ("category", error.category.as_str().to_string()),
                        ("detail", error.detail.clone()),
                    ],
                );
                callbacks.error(id, &error);
            }
        });

        match result {
            Ok(recording) => {
                if let Ok(mut map) = self.recordings.lock() {
                    map.insert(id, recording);
                }
                id
            }
            Err(error) => {
                self.pending.fetch_sub(1, Ordering::SeqCst);
                self.callbacks.error(id, &error);
                0
            }
        }
    }

    /// Stops the recording with `id`. Non-blocking.
    pub fn stop(&self, id: u64) {
        let recording = self
            .recordings
            .lock()
            .ok()
            .and_then(|mut map| map.remove(&id));
        if let Some(mut recording) = recording {
            recording.stop();
            // Keep the handle so shutdown can join the thread; the thread
            // itself is already finishing preprocessing.
            if let Ok(mut list) = self.finishing.lock() {
                list.push(recording);
            }
        }
        // Restore CapsLock as soon as the user lets go, matching the previous
        // behaviour where the visual state is fixed before recognition.
        uinput::restore_capslock();
    }

    pub fn shutdown(&self) {
        self.shutting_down.store(true, Ordering::SeqCst);

        if let Ok(mut map) = self.recordings.lock() {
            for (_, mut recording) in map.drain() {
                recording.stop();
                recording.join();
            }
        }
        if let Ok(mut list) = self.finishing.lock() {
            for mut recording in list.drain(..) {
                recording.join();
            }
        }

        // Closing the sender lets the worker drain and exit.
        if let Ok(mut slot) = self.sender.lock() {
            *slot = None;
        }
        if let Ok(mut worker) = self.worker.lock() {
            if let Some(handle) = worker.take() {
                let _ = handle.join();
            }
        }
        uinput::close();
        diagnostic::event("engine", "shutdown", &[]);
    }
}

fn worker_loop(
    receiver: mpsc::Receiver<Job>,
    callbacks: Callbacks,
    pending: Arc<AtomicUsize>,
    shutting_down: Arc<AtomicBool>,
) {
    while let Ok(job) = receiver.recv() {
        let id = job.id;
        let result = recognize(job);
        match result {
            Ok(text) => {
                diagnostic::event(
                    "engine",
                    "recognition_result",
                    &[
                        ("recognition_id", id.to_string()),
                        ("text_length", text.len().to_string()),
                    ],
                );
                callbacks.text(id, &text);
            }
            Err(error) => {
                diagnostic::event(
                    "engine",
                    "recognition_error",
                    &[
                        ("recognition_id", id.to_string()),
                        ("category", error.category.as_str().to_string()),
                        ("detail", error.detail.clone()),
                    ],
                );
                if shutting_down.load(Ordering::Relaxed)
                    && error.category == Category::Timeout
                    && error.detail == "cancelled"
                {
                    // Expected during shutdown: do not bother the user.
                } else {
                    callbacks.error(id, &error);
                }
            }
        }
        pending.fetch_sub(1, Ordering::SeqCst);
    }
}

fn recognize(job: Job) -> Result<String, Error> {
    let cleanup = Cleanup(&job.wav_path);

    let provider_id = current_provider();
    let provider = asr::create(&provider_id).ok_or_else(|| {
        Error::config_invalid(format!("ASR provider \"{provider_id}\" is not available"))
    })?;

    let cancel = Arc::new(AtomicBool::new(false));
    let input = asr::Input {
        wav_path: &job.wav_path,
        cancel: &cancel,
        credentials: &job.credentials,
    };
    let result = provider.transcribe(&input);
    drop(cleanup);
    result
}

/// Provider selection is part of the shell's configuration; it is mirrored
/// into a process-global so the worker can read it without a handle.
static CURRENT_PROVIDER: Mutex<String> = Mutex::new(String::new());

pub fn set_current_provider(provider: &str) {
    if let Ok(mut slot) = CURRENT_PROVIDER.lock() {
        *slot = provider.to_string();
    }
}

fn current_provider() -> String {
    CURRENT_PROVIDER
        .lock()
        .map(|guard| {
            if guard.is_empty() {
                "qwen".to_string()
            } else {
                guard.clone()
            }
        })
        .unwrap_or_else(|_| "qwen".to_string())
}

struct Cleanup<'a>(&'a PathBuf);

impl Drop for Cleanup<'_> {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(self.0);
    }
}

/// Test hook: run one recognition synchronously (used by provider tests).
#[cfg(test)]
mod tests {
    use super::*;

    extern "C" fn noop_text(_: *mut c_void, _: u64, _: *const c_char) {}
    extern "C" fn noop_error(_: *mut c_void, _: u64, _: i32, _: *const c_char) {}

    fn engine() -> Engine {
        Engine::new(Callbacks {
            user: std::ptr::null_mut(),
            on_text: noop_text,
            on_error: noop_error,
        })
    }

    #[test]
    fn provider_selection_defaults_to_qwen() {
        assert_eq!(current_provider(), "qwen");
        set_current_provider("mock");
        assert_eq!(current_provider(), "mock");
        set_current_provider("");
        assert_eq!(current_provider(), "qwen");
    }

    #[test]
    fn settings_are_clamped() {
        let engine = engine();
        let mut settings = Settings::default();
        settings.activation_msec = 1;
        settings.crest_threshold = -5.0;
        settings.buffer_ms = 5;
        engine.set_settings(settings);
        let snapshot = engine.settings_snapshot();
        assert!(snapshot.activation_msec >= 50);
        assert!(snapshot.crest_threshold >= 0.0);
        assert!(snapshot.buffer_ms >= 20);
        engine.shutdown();
    }

    /// End-to-end check against the real microphone and PulseAudio.
    ///
    /// Ignored by default because it needs audio hardware. Run with:
    ///   cargo test --lib -- --ignored --nocapture
    #[test]
    #[ignore]
    fn records_and_reports_through_callbacks() {
        use std::sync::mpsc;
        use std::time::Duration;

        static SINK: Mutex<Option<mpsc::Sender<String>>> = Mutex::new(None);

        extern "C" fn on_text(_: *mut c_void, id: u64, text: *const c_char) {
            let text = if text.is_null() {
                String::new()
            } else {
                unsafe { std::ffi::CStr::from_ptr(text) }
                    .to_string_lossy()
                    .into_owned()
            };
            if let Ok(slot) = SINK.lock() {
                if let Some(tx) = slot.as_ref() {
                    let _ = tx.send(format!("text:{id}:{text}"));
                }
            }
        }
        extern "C" fn on_error(
            _: *mut c_void,
            id: u64,
            category: i32,
            detail: *const c_char,
        ) {
            let detail = if detail.is_null() {
                String::new()
            } else {
                unsafe { std::ffi::CStr::from_ptr(detail) }
                    .to_string_lossy()
                    .into_owned()
            };
            if let Ok(slot) = SINK.lock() {
                if let Some(tx) = slot.as_ref() {
                    let _ = tx.send(format!("error:{id}:{category}:{detail}"));
                }
            }
        }

        let (tx, rx) = mpsc::channel();
        *SINK.lock().unwrap() = Some(tx);

        let engine = Engine::new(Callbacks {
            user: std::ptr::null_mut(),
            on_text,
            on_error,
        });

        let mut settings = Settings::default();
        settings.provider = "mock".to_string();
        // Make the gate permissive so a quiet room still reaches the provider.
        settings.crest_threshold = 0.0;
        // Lets a manual run exercise a non-default fragment size:
        //   AINPUT_BUFFER_MS=250 cargo test --features diagnostics -- --ignored
        if let Ok(value) = std::env::var("AINPUT_BUFFER_MS") {
            if let Ok(ms) = value.parse::<i32>() {
                settings.buffer_ms = ms;
            }
        }
        settings.denoise = crate::settings::Denoise::None;
        set_current_provider("mock");
        engine.set_settings(settings);

        let id = engine.start();
        assert_ne!(id, 0, "recording did not start");

        std::thread::sleep(Duration::from_millis(1500));
        engine.stop(id);

        let message = rx
            .recv_timeout(Duration::from_secs(15))
            .expect("no callback within 15 s");
        println!("callback: {message}");
        assert!(
            message.starts_with("text:") || message.starts_with("error:"),
            "unexpected callback: {message}"
        );

        // The queue must drain even when the recording produced no speech.
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while engine.pending.load(Ordering::SeqCst) != 0
            && std::time::Instant::now() < deadline
        {
            std::thread::sleep(Duration::from_millis(20));
        }
        assert_eq!(engine.pending.load(Ordering::SeqCst), 0);
        engine.shutdown();
    }

    #[test]
    fn queue_limit_is_never_exceeded() {
        let engine = engine();
        // PulseAudio may or may not be reachable here; either way the number
        // of accepted recordings must stay within the limit.
        for _ in 0..6 {
            let _ = engine.start();
            assert!(engine.pending.load(Ordering::SeqCst) <= MAX_PENDING);
        }
        engine.shutdown();
    }
}
