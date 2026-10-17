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
    /// Optional stage notification; `None` when the shell passed NULL.
    pub on_stage: Option<extern "C" fn(*mut c_void, u64, i32)>,
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

    fn stage(self, id: u64, stage: i32) {
        if let Some(callback) = self.on_stage {
            callback(self.user, id, stage);
        }
    }
}

struct Job {
    id: u64,
    wav_path: PathBuf,
    /// Provider chosen when the recording started, not when it is recognized:
    /// switching backends mid-recording must not redirect the audio.
    provider: String,
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
    /// Cancel flag of the request the worker is running right now. Clearing it
    /// (or setting it) is how shutdown aborts an in-flight cloud call instead
    /// of waiting out its timeout.
    active_cancel: Arc<Mutex<Option<Arc<AtomicBool>>>>,
    sender: Mutex<Option<Sender<Job>>>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl Engine {
    pub fn new(callbacks: Callbacks) -> Self {
        let (sender, receiver) = mpsc::channel::<Job>();
        let pending = Arc::new(AtomicUsize::new(0));
        let shutting_down = Arc::new(AtomicBool::new(false));
        let active_cancel = Arc::new(Mutex::new(None::<Arc<AtomicBool>>));

        let worker_pending = pending.clone();
        let worker_shutdown = shutting_down.clone();
        let worker_cancel = active_cancel.clone();
        let worker_callbacks = callbacks;

        let worker = std::thread::Builder::new()
            .name("ainput-recognition".to_string())
            .spawn(move || {
                worker_loop(
                    receiver,
                    worker_callbacks,
                    worker_pending,
                    worker_shutdown,
                    worker_cancel,
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
            active_cancel,
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
        let provider = settings.provider.clone();
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
                        provider: provider.clone(),
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
                // The thread never started; release the reserved slot and
                // report. This is the only synchronous failure left now that
                // microphone setup happens inside the thread.
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
            // Keep the handle so shutdown can join the thread. Entries whose
            // thread already finished are reaped here, otherwise every
            // recognition would leak one join handle for the whole session.
            if let Ok(mut list) = self.finishing.lock() {
                list.push(recording);
                list.retain_mut(|finished| {
                    if finished.is_finished() {
                        finished.join();
                        false
                    } else {
                        true
                    }
                });
            }
        }
        // Restore CapsLock as soon as the user lets go, matching the previous
        // behaviour where the visual state is fixed before recognition.
        uinput::restore_capslock();
    }

    pub fn shutdown(&self) {
        self.shutting_down.store(true, Ordering::SeqCst);

        // Abort the request the worker is inside right now. Without this,
        // joining the worker means waiting out a network timeout (60-120 s)
        // while fcitx5 is trying to exit.
        if let Ok(slot) = self.active_cancel.lock() {
            if let Some(cancel) = slot.as_ref() {
                cancel.store(true, Ordering::SeqCst);
            }
        }

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
    active_cancel: Arc<Mutex<Option<Arc<AtomicBool>>>>,
) {
    while let Ok(job) = receiver.recv() {
        let id = job.id;

        if shutting_down.load(Ordering::SeqCst) {
            // Nothing the user said is worth a round trip during shutdown.
            let _ = std::fs::remove_file(&job.wav_path);
            pending.fetch_sub(1, Ordering::SeqCst);
            continue;
        }

        callbacks.stage(id, crate::AINPUT_STAGE_RECOGNIZING);
        let cancel = Arc::new(AtomicBool::new(false));
        if let Ok(mut slot) = active_cancel.lock() {
            *slot = Some(cancel.clone());
        }
        let result = recognize(job, &cancel);
        if let Ok(mut slot) = active_cancel.lock() {
            *slot = None;
        }
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

fn recognize(job: Job, cancel: &Arc<AtomicBool>) -> Result<String, Error> {
    let cleanup = Cleanup(&job.wav_path);

    let provider = asr::create(&job.provider).ok_or_else(|| {
        Error::config_invalid(format!(
            "ASR provider \"{}\" is not available",
            job.provider
        ))
    })?;

    let input = asr::Input {
        wav_path: &job.wav_path,
        cancel,
        credentials: &job.credentials,
    };
    let result = provider.transcribe(&input);
    drop(cleanup);
    result
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
            on_stage: None,
        })
    }

    #[test]
    fn settings_are_clamped() {
        let engine = engine();
        engine.set_settings(Settings {
            activation_msec: 1,
            crest_threshold: -5.0,
            ..Settings::default()
        });
        let snapshot = engine.settings_snapshot();
        assert!(snapshot.activation_msec >= 50);
        assert!(snapshot.crest_threshold >= 0.0);
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
            on_stage: None,
        });

        let mut settings = Settings::default();
        settings.provider = "mock".to_string();
        // Make the gate permissive so a quiet room still reaches the provider.
        settings.crest_threshold = 0.0;
        settings.denoise = crate::settings::Denoise::None;
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

    /// Switching the provider after a recording started must not redirect it.
    ///
    /// The observable difference is the text: `mock` always answers
    /// "hello world", while `qwen` cannot succeed here because qwen.json points
    /// at a closed port. Before the fix, the job read the provider globally at
    /// recognition time and produced a network error instead.
    #[test]
    fn recording_keeps_the_provider_it_started_with() {
        use std::sync::mpsc;
        use std::time::Duration;

        let home = std::env::temp_dir().join("ainput-provider-snapshot");
        let _ = std::fs::remove_dir_all(&home);
        std::fs::create_dir_all(&home).unwrap();
        std::fs::write(
            home.join("qwen.json"),
            r#"{"api_key":"sk-x","endpoint":"http://127.0.0.1:9/x","timeout_sec":2}"#,
        )
        .unwrap();
        let lock = crate::config::test_lock();
        let previous = crate::config::test_config_dir(home);

        static SINK: Mutex<Option<mpsc::Sender<String>>> = Mutex::new(None);
        extern "C" fn on_text(_: *mut c_void, _: u64, text: *const c_char) {
            let text = unsafe { std::ffi::CStr::from_ptr(text) }
                .to_string_lossy()
                .into_owned();
            if let Ok(slot) = SINK.lock() {
                if let Some(tx) = slot.as_ref() {
                    let _ = tx.send(format!("text:{text}"));
                }
            }
        }
        extern "C" fn on_error(_: *mut c_void, _: u64, category: i32, _: *const c_char) {
            if let Ok(slot) = SINK.lock() {
                if let Some(tx) = slot.as_ref() {
                    let _ = tx.send(format!("error:{category}"));
                }
            }
        }

        let (tx, rx) = mpsc::channel();
        *SINK.lock().unwrap() = Some(tx);

        let engine = Engine::new(Callbacks {
            user: std::ptr::null_mut(),
            on_text,
            on_error,
            on_stage: None,
        });
        engine.set_settings(Settings {
            provider: "mock".to_string(),
            crest_threshold: 0.0,
            denoise: crate::settings::Denoise::None,
            ..Settings::default()
        });

        let id = engine.start();
        assert_ne!(id, 0);
        std::thread::sleep(Duration::from_millis(500));
        // The user switches backend before releasing the key.
        engine.stop(id);

        let message = rx.recv_timeout(Duration::from_secs(10)).expect("no callback");
        engine.shutdown();
        *SINK.lock().unwrap() = None;
        crate::config::restore_config_dir(previous);
        crate::settings::set_active_credentials(crate::settings::Credentials::default());
        drop(lock);

        assert_eq!(
            message, "text:hello world",
            "the recording must keep the provider it started with"
        );
    }

    /// Shutdown must abort an in-flight cloud call instead of waiting out its
    /// timeout: fcitx5 -r / logout / package upgrade all go through here.
    #[test]
    fn shutdown_cancels_an_in_flight_request() {
        use std::time::{Duration, Instant};
        let home = std::env::temp_dir().join("ainput-shutdown-cancel");
        let _ = std::fs::remove_dir_all(&home);
        std::fs::create_dir_all(&home).unwrap();

        // A server that accepts the connection and then never answers. A
        // refused or unroutable address would fail instantly and prove
        // nothing; this one can only end through cancellation or its timeout.
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let endpoint = format!("http://{}/v1", listener.local_addr().unwrap());
        let hold = std::thread::spawn(move || {
            if let Ok((stream, _)) = listener.accept() {
                std::thread::sleep(Duration::from_secs(30));
                drop(stream);
            }
        });
        std::fs::write(
            home.join("qwen.json"),
            format!(
                r#"{{"api_key":"sk-x","endpoint":"{endpoint}","timeout_sec":30}}"#
            ),
        )
        .unwrap();
        let lock = crate::config::test_lock();
        let previous = crate::config::test_config_dir(home);

        let engine = engine();
        engine.set_settings(Settings {
            provider: "qwen".to_string(),
            crest_threshold: 0.0,
            denoise: crate::settings::Denoise::None,
            ..Settings::default()
        });

        let id = engine.start();
        assert_ne!(id, 0);
        std::thread::sleep(Duration::from_millis(700));
        engine.stop(id);
        // Give the worker time to enter the request.
        std::thread::sleep(Duration::from_millis(500));

        let began = Instant::now();
        engine.shutdown();
        let took = began.elapsed();

        drop(hold);
        crate::config::restore_config_dir(previous);
        crate::settings::set_active_credentials(crate::settings::Credentials::default());
        drop(lock);
        assert!(
            took < Duration::from_secs(5),
            "shutdown waited {took:?}; the request was not cancelled"
        );
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
