//! One thread per dictation, from capture through recognition. The event loop
//! owns delivery order; the engine owns the lifetime and deadline of each job.

use std::collections::HashMap;
use std::ffi::{c_char, c_void, CString};
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::asr;
use crate::capture;
use crate::error::Error;
use crate::http::Transport;
use crate::settings::Settings;
use crate::uinput;

pub const MAX_PENDING: usize = 3;
// Cancelled threads normally exit promptly. Bound the number still unwinding
// without making a newly accepted job wait behind them.
const MAX_THREADS: usize = MAX_PENDING * 2;
static NEXT_ID: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Copy)]
pub struct Callbacks {
    pub user: *mut c_void,
    pub on_text: extern "C" fn(*mut c_void, u64, *const c_char),
    pub on_error: extern "C" fn(*mut c_void, u64, i32, *const c_char),
    pub on_stage: Option<extern "C" fn(*mut c_void, u64, i32)>,
}

// The shell promises these callbacks are thread-safe. Shutdown joins all jobs
// before the C++ receiver can be destroyed.
unsafe impl Send for Callbacks {}
unsafe impl Sync for Callbacks {}

impl Callbacks {
    fn text(self, id: u64, text: &str) {
        let text = CString::new(text.replace('\0', "")).unwrap();
        (self.on_text)(self.user, id, text.as_ptr());
    }

    fn error(self, id: u64, error: &Error) {
        let detail = CString::new(error.detail.replace('\0', "")).unwrap();
        (self.on_error)(self.user, id, error.category.as_i32(), detail.as_ptr());
    }

    fn stage(self, id: u64, stage: i32) {
        if let Some(callback) = self.on_stage {
            callback(self.user, id, stage);
        }
    }
}

struct Task {
    stop: Arc<AtomicBool>,
    cancel: Arc<AtomicBool>,
    released: Option<Instant>,
    budget: Duration,
}

impl Task {
    fn expired(&self, now: Instant) -> bool {
        self.released
            .is_some_and(|release| now - release >= self.budget)
    }
}

#[derive(Default)]
struct Tasks {
    active: HashMap<u64, Task>,
    threads: Vec<JoinHandle<()>>,
    closed: bool,
}

struct Shared {
    tasks: Mutex<Tasks>,
    callbacks: Callbacks,
    transport: Transport,
}

impl Shared {
    fn set_budget(&self, id: u64, budget: Duration) {
        if let Some(task) = self.tasks.lock().unwrap().active.get_mut(&id) {
            task.budget = budget;
        }
    }

    fn begin_recognition(&self, id: u64) -> Result<Option<Instant>, Error> {
        let tasks = self.tasks.lock().unwrap();
        let task = tasks
            .active
            .get(&id)
            .filter(|t| !t.expired(Instant::now()))
            .ok_or_else(|| Error::timeout("cancelled"))?;
        crate::diagnostic::trace(id, "recognition_started");
        self.callbacks.stage(id, crate::AINPUT_STAGE_RECOGNIZING);
        Ok(task
            .released
            .and_then(|released| released.checked_add(task.budget)))
    }

    fn finish(&self, id: u64, result: Result<String, Error>) {
        let mut tasks = self.tasks.lock().unwrap();
        if let Some(task) = tasks.active.remove(&id) {
            if task.expired(Instant::now()) {
                self.timeout(id, &task);
            } else {
                match result {
                    Ok(text) => {
                        crate::diagnostic::trace(id, "recognition_result");
                        self.callbacks.text(id, &text);
                    }
                    Err(error) => {
                        crate::diagnostic::trace_failure(id, &error);
                        self.callbacks.error(id, &error);
                    }
                }
            }
        }
    }

    fn timeout(&self, id: u64, task: &Task) {
        task.cancel.store(true, Ordering::Release);
        crate::diagnostic::trace(id, "recognition_deadline");
        self.callbacks
            .error(id, &Error::timeout("dictation deadline exceeded"));
    }
}

pub struct Engine {
    shared: Arc<Shared>,
    settings: RwLock<Settings>,
}

impl Engine {
    pub fn new(callbacks: Callbacks) -> Self {
        let shared = Arc::new(Shared {
            tasks: Mutex::new(Tasks::default()),
            callbacks,
            transport: Transport::system(),
        });
        Self {
            shared,
            settings: RwLock::new(Settings::default()),
        }
    }

    pub fn set_settings(&self, settings: Settings) {
        let settings = settings.sanitized();
        crate::diagnostic::event(
            "engine",
            "settings_applied",
            &[("provider", settings.provider.clone())],
        );
        crate::settings::set_active_credentials(settings.credentials.clone());
        *self.settings.write().unwrap() = settings;
    }

    fn settings_snapshot(&self) -> Settings {
        self.settings.read().unwrap().clone()
    }

    pub fn start(&self) -> u64 {
        self.start_with_capture(capture::run)
    }

    fn start_with_capture<F>(&self, capture: F) -> u64
    where
        F: FnOnce(u64, Settings, Arc<AtomicBool>, &AtomicBool) -> Result<capture::Captured, Error>
            + Send
            + 'static,
    {
        let settings = self.settings_snapshot();
        let budget = asr::default_timeout(&settings.provider);
        self.start_task(budget, move |shared, id, stop, cancel| {
            // File I/O stays off fcitx5's event loop. Record the actual budget
            // before opening the mic; it is measured from release either way.
            shared.set_budget(id, asr::task_timeout(&settings.provider, budget));
            let provider = settings.provider.clone();
            let credentials = settings.credentials.clone();
            let result = capture(id, settings, stop, &cancel).and_then(|captured| {
                let _cleanup = Cleanup(&captured.wav_path);
                crate::diagnostic::trace(id, "recognition_queued");
                let deadline = shared.begin_recognition(id)?;
                let backend = asr::create(&provider).ok_or_else(|| {
                    Error::config_invalid(format!("ASR provider \"{provider}\" is not available"))
                })?;
                backend.transcribe_with(
                    &asr::Input {
                        wav_path: &captured.wav_path,
                        cancel: &cancel,
                        credentials: &credentials,
                    },
                    asr::Context {
                        id,
                        client: &shared.transport,
                        deadline,
                    },
                )
            });
            shared.finish(id, result);
        })
    }

    fn start_task<F>(&self, budget: Duration, work: F) -> u64
    where
        F: FnOnce(&Shared, u64, Arc<AtomicBool>, Arc<AtomicBool>) + Send + 'static,
    {
        let mut tasks = self.shared.tasks.lock().unwrap();
        if tasks.closed {
            return 0;
        }
        tasks.threads.retain(|worker| !worker.is_finished());
        if tasks.active.len() >= MAX_PENDING || tasks.threads.len() >= MAX_THREADS {
            self.shared
                .callbacks
                .error(0, &Error::runtime("recognition queue full; try again"));
            return 0;
        }
        let id = NEXT_ID.fetch_add(1, Ordering::Relaxed) + 1;
        let stop = Arc::new(AtomicBool::new(false));
        let cancel = Arc::new(AtomicBool::new(false));
        tasks.active.insert(
            id,
            Task {
                stop: stop.clone(),
                cancel: cancel.clone(),
                released: None,
                budget,
            },
        );
        let shared = self.shared.clone();
        match std::thread::Builder::new()
            .name(format!("ainput-job-{id}"))
            .spawn(move || {
                work(&shared, id, stop, cancel);
            }) {
            Ok(worker) => {
                tasks.threads.push(worker);
                id
            }
            Err(error) => {
                tasks.active.remove(&id);
                self.shared.callbacks.error(
                    0,
                    &Error::runtime(format!("cannot start dictation thread: {error}")),
                );
                0
            }
        }
    }

    pub fn stop(&self, id: u64) {
        let mut tasks = self.shared.tasks.lock().unwrap();
        if let Some(task) = tasks.active.get_mut(&id) {
            task.released.get_or_insert_with(Instant::now);
            crate::diagnostic::trace(id, "recording_released");
            task.stop.store(true, Ordering::Release);
        }
    }

    /// Called by the fcitx5 watchdog, independently of all task threads.
    pub fn tick(&self) {
        let now = Instant::now();
        let mut tasks = self.shared.tasks.lock().unwrap();
        tasks.active.retain(|&id, task| {
            if !task.expired(now) {
                return true;
            }
            self.shared.timeout(id, task);
            false
        });
    }

    pub fn cancel_all(&self) {
        Self::discard(&mut self.shared.tasks.lock().unwrap());
    }

    #[cfg(test)]
    fn pending(&self) -> usize {
        self.shared.tasks.lock().unwrap().active.len()
    }

    fn discard(tasks: &mut Tasks) {
        for (id, task) in tasks.active.drain() {
            task.cancel.store(true, Ordering::Release);
            task.stop.store(true, Ordering::Release);
            crate::diagnostic::trace(id, "recording_cancelled");
        }
    }

    pub fn shutdown(&self) {
        let threads = {
            let mut tasks = self.shared.tasks.lock().unwrap();
            if tasks.closed {
                return;
            }
            tasks.closed = true;
            Self::discard(&mut tasks);
            std::mem::take(&mut tasks.threads)
        };
        for thread in threads {
            let _ = thread.join();
        }
        uinput::close();
        crate::diagnostic::event("engine", "shutdown", &[]);
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        self.shutdown();
    }
}

struct Cleanup<'a>(&'a Path);

impl Drop for Cleanup<'_> {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(self.0);
    }
}

#[cfg(test)]
#[path = "engine_tests.rs"]
mod tests;
