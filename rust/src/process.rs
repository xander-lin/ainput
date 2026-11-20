//! Cancellable execution for per-dictation local helpers.

use std::fs::File;
use std::io;
use std::os::fd::FromRawFd;
use std::os::unix::fs::FileExt;
use std::os::unix::process::CommandExt;
use std::process::{Command, ExitStatus};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use crate::error::{Error, Result};

pub fn run(
    command: &mut Command,
    label: &str,
    deadline: Option<Instant>,
    cancel: &AtomicBool,
) -> Result<ExitStatus> {
    let interrupted = || {
        if cancel.load(Ordering::Acquire) {
            Some(Error::timeout("cancelled"))
        } else if deadline.is_some_and(|end| Instant::now() >= end) {
            Some(Error::timeout(format!("{label} timed out")))
        } else {
            None
        }
    };
    if let Some(error) = interrupted() {
        return Err(error);
    }
    let mut child = command
        .process_group(0)
        .spawn()
        .map_err(|error| Error::local_setup(format!("cannot run {label}: {error}")))?;
    let error = loop {
        if let Some(error) = interrupted() {
            break error;
        }
        match child.try_wait() {
            Ok(Some(status)) => return Ok(status),
            Ok(None) => std::thread::sleep(Duration::from_millis(20)),
            Err(error) => break Error::local_setup(format!("cannot wait for {label}: {error}")),
        }
    };
    // The child has not been reaped. Kill its private group before waiting so
    // shell wrappers cannot leave the actual helper running after Escape.
    unsafe { libc::kill(-(child.id() as libc::pid_t), libc::SIGKILL) };
    let _ = child.kill();
    let _ = child.wait();
    Err(error)
}

/// Regular-file semantics prevent pipe backpressure and inherited writers
/// cannot block result collection. The memfd disappears once closed.
pub fn output_file(command: &mut Command) -> io::Result<File> {
    let fd = unsafe { libc::memfd_create(c"ainput-local-helper".as_ptr(), libc::MFD_CLOEXEC) };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    let file = unsafe { File::from_raw_fd(fd) };
    command.stdout(file.try_clone()?).stderr(file.try_clone()?);
    Ok(file)
}

pub fn read_output(file: File) -> io::Result<String> {
    const MAX_OUTPUT: u64 = 1024 * 1024;
    let len = file.metadata()?.len();
    let mut bytes = vec![0; len.min(MAX_OUTPUT) as usize];
    file.read_exact_at(&mut bytes, len.saturating_sub(MAX_OUTPUT))?;
    Ok(String::from_utf8_lossy(&bytes).into_owned())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::error::Category;

    #[test]
    fn escape_interrupts_helper_after_it_has_started() {
        let cancel = AtomicBool::new(false);
        let mut command = Command::new("sh");
        command.args(["-c", "printf ready; sleep 30 & wait"]);
        let file = output_file(&mut command).unwrap();
        std::thread::scope(|scope| {
            let task = scope.spawn(|| {
                run(
                    &mut command,
                    "helper",
                    Some(Instant::now() + Duration::from_secs(5)),
                    &cancel,
                )
            });
            let start = Instant::now();
            while file.metadata().unwrap().len() == 0 {
                assert!(start.elapsed() < Duration::from_secs(2));
                std::thread::sleep(Duration::from_millis(10));
            }
            cancel.store(true, Ordering::Release);
            let error = task.join().unwrap().unwrap_err();
            assert_eq!(error.category, Category::Timeout);
            assert_eq!(error.detail, "cancelled");
        });
    }

    #[test]
    fn helper_output_larger_than_a_pipe_buffer_is_collected() {
        let mut command = Command::new("sh");
        command.args(["-c", "head -c 262144 /dev/zero; printf 'result\\n' >&2"]);
        let file = output_file(&mut command).unwrap();
        assert!(run(
            &mut command,
            "helper",
            Some(Instant::now() + Duration::from_secs(3)),
            &AtomicBool::new(false)
        )
        .unwrap()
        .success());
        assert!(read_output(file).unwrap().ends_with("result\n"));
    }
}
