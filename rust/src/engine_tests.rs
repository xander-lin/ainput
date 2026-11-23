use super::*;
use std::sync::mpsc;

fn mock_capture(
    id: u64,
    _: Settings,
    stop: Arc<AtomicBool>,
    cancel: &AtomicBool,
) -> Result<capture::Captured, Error> {
    while !stop.load(Ordering::Acquire) && !cancel.load(Ordering::Acquire) {
        std::thread::sleep(Duration::from_millis(1));
    }
    let wav_path =
        std::env::temp_dir().join(format!("ainput-test-{}-{id}.wav", std::process::id()));
    crate::wav::write(&wav_path, &[1; 1600])?;
    Ok(capture::Captured { wav_path })
}

#[derive(Debug, PartialEq)]
enum Event {
    Text(u64),
    Error(u64, i32),
}

fn observed_engine(tx: &mpsc::Sender<Event>) -> Engine {
    extern "C" fn text(user: *mut c_void, id: u64, _: *const c_char) {
        let _ = unsafe { &*user.cast::<mpsc::Sender<Event>>() }.send(Event::Text(id));
    }
    extern "C" fn error(user: *mut c_void, id: u64, category: i32, _: *const c_char) {
        let _ = unsafe { &*user.cast::<mpsc::Sender<Event>>() }.send(Event::Error(id, category));
    }
    Engine::new(Callbacks {
        user: std::ptr::from_ref(tx).cast_mut().cast(),
        on_text: text,
        on_error: error,
        on_stage: None,
    })
}

#[test]
fn slow_task_does_not_block_another_and_late_success_is_ignored() {
    let (events, rx) = mpsc::channel();
    let engine = observed_engine(&events);
    let (release, blocked) = mpsc::channel();
    let (returned, done) = mpsc::channel();
    let first = engine.start_task(Duration::from_millis(30), move |shared, id, _, cancel| {
        let _ = blocked.recv_timeout(Duration::from_secs(5));
        shared.finish(id, Ok("late".into()));
        let _ = returned.send(cancel.load(Ordering::Acquire));
    });
    // Time before release does not spend the recognition budget.
    std::thread::sleep(Duration::from_millis(40));
    engine.tick();
    assert_eq!(engine.pending(), 1);
    engine.stop(first);
    let second = engine.start_task(Duration::from_secs(5), |shared, id, _, _| {
        shared.finish(id, Ok("ready".into()));
    });
    assert_eq!(
        rx.recv_timeout(Duration::from_secs(2)).unwrap(),
        Event::Text(second)
    );
    std::thread::sleep(Duration::from_millis(40));
    engine.tick();
    assert_eq!(
        rx.recv_timeout(Duration::from_secs(2)).unwrap(),
        Event::Error(first, crate::error::Category::Timeout.as_i32())
    );
    assert_eq!(engine.pending(), 0);
    release.send(()).unwrap();
    assert!(done.recv_timeout(Duration::from_secs(2)).unwrap());
    engine.shutdown();
    assert!(rx.try_recv().is_err());
}

#[test]
fn cancel_all_signals_recording_and_recognition_and_admits_new_work() {
    let (events, rx) = mpsc::channel();
    let engine = observed_engine(&events);
    let mut gates = Vec::new();
    let (report, flags) = mpsc::channel();
    for index in 0..MAX_PENDING {
        let (gate, waiting) = mpsc::channel();
        gates.push(gate);
        let report = report.clone();
        let id = engine.start_task(Duration::from_secs(10), move |shared, id, stop, cancel| {
            let _ = waiting.recv_timeout(Duration::from_secs(5));
            shared.finish(id, Ok("discard".into()));
            let _ = report.send((stop.load(Ordering::Acquire), cancel.load(Ordering::Acquire)));
        });
        assert_ne!(id, 0);
        if index > 0 {
            engine.stop(id);
        }
    }
    engine.cancel_all();
    assert_eq!(engine.pending(), 0);
    let fresh = engine.start_task(Duration::from_secs(10), |shared, id, _, _| {
        shared.finish(id, Ok("fresh".into()));
    });
    assert_ne!(fresh, 0);
    assert_eq!(
        rx.recv_timeout(Duration::from_secs(2)).unwrap(),
        Event::Text(fresh)
    );
    for gate in gates {
        gate.send(()).unwrap();
    }
    for _ in 0..MAX_PENDING {
        assert_eq!(
            flags.recv_timeout(Duration::from_secs(2)).unwrap(),
            (true, true)
        );
    }
    engine.shutdown();
    assert!(rx.try_recv().is_err());
}

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

fn validate_captured_wav(captured: capture::Captured) -> Result<capture::Captured, Error> {
    let result = crate::wav::read(&captured.wav_path).and_then(|samples| {
        if crate::dsp::has_voice(&samples, 0.0)? {
            Ok(())
        } else {
            Err(Error::runtime("WAV has insufficient signal"))
        }
    });
    match result {
        Ok(()) => Ok(captured),
        Err(error) => {
            drop(Cleanup(&captured.wav_path));
            Err(Error::runtime(format!("invalid captured WAV: {error}")))
        }
    }
}

#[test]
fn only_recordings_with_a_remaining_signal_reach_recognition() {
    use crate::error::Category;
    use crate::settings::Denoise;

    let (events, rx) = mpsc::channel();
    let engine = observed_engine(&events);
    // Use the per-engine snapshot without writing the global display credentials.
    *engine.settings.write().unwrap() = Settings {
        provider: "mock".into(),
        crest_threshold: 0.0,
        denoise: Denoise::Speexdsp,
        ..Settings::default()
    };

    let quiet: Vec<i16> = (0..1600)
        .map(|i| {
            let phase = std::f64::consts::TAU * 440.0 * f64::from(i) / 16_000.0;
            (3.0 * phase.sin()) as i16
        })
        .collect();
    for (samples, expected_error) in [
        (vec![0; 1600], Some(Category::AudioData)),
        (vec![1; 1600], Some(Category::NoSpeech)),
        (quiet, None),
    ] {
        let id = engine.start_with_capture(move |id, settings, _, cancel| {
            validate_captured_wav(capture::finish_recording(id, settings, samples, cancel)?)
        });
        assert_ne!(id, 0);
        engine.stop(id);
        let expected = match expected_error {
            Some(category) => Event::Error(id, category.as_i32()),
            None => Event::Text(id),
        };
        assert_eq!(rx.recv_timeout(Duration::from_secs(2)).unwrap(), expected);
    }
}

/// End-to-end check against the real microphone and PulseAudio. Provide sound
/// at the selected capture source during the 1.5-second recording.
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
    extern "C" fn on_error(_: *mut c_void, id: u64, category: i32, detail: *const c_char) {
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
    // Exercise the activity check without the optional crest and VAD gate.
    settings.crest_threshold = 0.0;
    settings.denoise = crate::settings::Denoise::Speexdsp;
    engine.set_settings(settings);

    let id = engine.start_with_capture(|id, settings, stop, cancel| {
        validate_captured_wav(capture::run(id, settings, stop, cancel)?)
    });
    assert_ne!(id, 0, "recording did not start");

    std::thread::sleep(Duration::from_millis(1500));
    engine.stop(id);

    let message = rx
        .recv_timeout(Duration::from_secs(15))
        .expect("no callback within 15 s");
    println!("callback: {message}");
    assert_eq!(message, format!("text:{id}:hello world"));

    // The queue must drain after the recording is delivered.
    let deadline = std::time::Instant::now() + Duration::from_secs(5);
    while engine.pending() != 0 && std::time::Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(20));
    }
    assert_eq!(engine.pending(), 0);
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

    let id = engine.start_with_capture(mock_capture);
    assert_ne!(id, 0);
    // The user switches backend before releasing the key.
    engine.set_settings(Settings {
        provider: "qwen".to_string(),
        ..Settings::default()
    });
    engine.stop(id);

    let message = rx
        .recv_timeout(Duration::from_secs(10))
        .expect("no callback");
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
    let (accepted, connected) = mpsc::channel();
    let (release, wait) = mpsc::channel();
    let hold = std::thread::spawn(move || {
        if let Ok((stream, _)) = listener.accept() {
            let _ = accepted.send(());
            let _ = wait.recv_timeout(Duration::from_secs(30));
            drop(stream);
        }
    });
    std::fs::write(
        home.join("qwen.json"),
        format!(r#"{{"api_key":"sk-x","endpoint":"{endpoint}","timeout_sec":30}}"#),
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

    let id = engine.start_with_capture(mock_capture);
    assert_ne!(id, 0);
    engine.stop(id);
    connected
        .recv_timeout(Duration::from_secs(5))
        .expect("request never connected");

    let began = Instant::now();
    engine.shutdown();
    let took = began.elapsed();

    release.send(()).unwrap();
    hold.join().unwrap();
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
    // Leave the mock recordings active while attempting more starts.
    for _ in 0..6 {
        let _ = engine.start_with_capture(mock_capture);
        assert!(engine.pending() <= MAX_PENDING);
    }
    assert_eq!(engine.pending(), MAX_PENDING);
    engine.shutdown();
}
