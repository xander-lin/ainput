//! CapsLock restore through a virtual keyboard.
//!
//! Holding CapsLock to dictate toggles the physical lock state. After the
//! recording stops, one synthetic CapsLock press+release is injected so the
//! state goes back. This needs write access to `/dev/uinput`, granted by the
//! udev rule the package installs.
//!
//! Failure is reported, not swallowed: without the device the addon still
//! transcribes, but `error()` explains why the lock stays on.

use std::fs::{File, OpenOptions};
use std::io::Write;
use std::os::fd::AsRawFd;
use std::os::unix::fs::OpenOptionsExt;
use std::sync::Mutex;

use crate::diagnostic;

const EV_SYN: u16 = 0x00;
const EV_KEY: u16 = 0x01;
const EV_LED: u16 = 0x11;
const SYN_REPORT: u16 = 0;
const KEY_CAPSLOCK: u16 = 58;
const LED_CAPSL: u16 = 1;

// UINPUT_IOCTL_BASE is 'U' (0x55). The request numbers are generated here
// instead of being copied as magic constants, so a wrong size or number shows
// up as a compile-time expression rather than an EINVAL at runtime.
const IOCTL_BASE: u64 = b'U' as u64;

/// `_IOW(base, nr, type)`
const fn iow(nr: u64, size: usize) -> libc::c_ulong {
    ((1u64 << 30) | ((size as u64) << 16) | (IOCTL_BASE << 8) | nr) as libc::c_ulong
}

/// `_IO(base, nr)`
const fn io(nr: u64) -> libc::c_ulong {
    ((IOCTL_BASE << 8) | nr) as libc::c_ulong
}

/// `struct input_id`
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct InputId {
    bustype: u16,
    vendor: u16,
    product: u16,
    version: u16,
}

const UINPUT_MAX_NAME_SIZE: usize = 80;

/// `struct uinput_setup`; mandatory before `UI_DEV_CREATE`.
#[repr(C)]
#[derive(Clone, Copy)]
struct UinputSetup {
    id: InputId,
    name: [u8; UINPUT_MAX_NAME_SIZE],
    ff_effects_max: u32,
}

const BUS_VIRTUAL: u16 = 0x06;

fn setup_struct() -> UinputSetup {
    let mut name = [0u8; UINPUT_MAX_NAME_SIZE];
    let label = b"Ainput vkbd";
    name[..label.len()].copy_from_slice(label);
    UinputSetup {
        id: InputId {
            bustype: BUS_VIRTUAL,
            ..Default::default()
        },
        name,
        ff_effects_max: 0,
    }
}

const UI_SET_EVBIT: libc::c_ulong = iow(100, 4);
const UI_SET_KEYBIT: libc::c_ulong = iow(101, 4);
const UI_SET_LEDBIT: libc::c_ulong = iow(105, 4);
const UI_DEV_SETUP: libc::c_ulong = iow(3, std::mem::size_of::<UinputSetup>());
const UI_DEV_CREATE: libc::c_ulong = io(1);
const UI_DEV_DESTROY: libc::c_ulong = io(2);

/// `struct input_event` on 64-bit Linux.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct InputEvent {
    sec: i64,
    usec: i64,
    kind: u16,
    code: u16,
    value: i32,
}

static DEVICE: Mutex<Option<File>> = Mutex::new(None);
static LAST_ERROR: Mutex<Option<String>> = Mutex::new(None);

fn set_error(message: impl Into<String>) {
    let message = message.into();
    diagnostic::event("uinput", "error", &[("message", message.clone())]);
    if let Ok(mut slot) = LAST_ERROR.lock() {
        *slot = Some(message);
    }
}

fn ioctl(fd: i32, request: libc::c_ulong, value: libc::c_ulong) -> std::io::Result<()> {
    let result = unsafe { libc::ioctl(fd, request, value) };
    if result < 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}

/// Opens `/dev/uinput` and registers the virtual keyboard. Idempotent.
pub fn open() -> bool {
    let mut slot = match DEVICE.lock() {
        Ok(slot) => slot,
        Err(poisoned) => poisoned.into_inner(),
    };
    if slot.is_some() {
        return true;
    }

    let file = match OpenOptions::new()
        .write(true)
        .custom_flags(libc::O_NONBLOCK)
        .open("/dev/uinput")
    {
        Ok(file) => file,
        Err(err) => {
            drop(slot);
            set_error(format!(
                "cannot open /dev/uinput: {err}. Install/enable \
                 70-ainput-uinput.rules and run 'udevadm control --reload && \
                 udevadm trigger --subsystem-match=misc --sysname-match=uinput'. \
                 Voice input works without it; CapsLock restore does not."
            ));
            return false;
        }
    };

    let fd = file.as_raw_fd();
    let setup = (|| -> std::io::Result<()> {
        ioctl(fd, UI_SET_EVBIT, EV_KEY as libc::c_ulong)?;
        ioctl(fd, UI_SET_KEYBIT, KEY_CAPSLOCK as libc::c_ulong)?;
        ioctl(fd, UI_SET_EVBIT, EV_LED as libc::c_ulong)?;
        ioctl(fd, UI_SET_LEDBIT, LED_CAPSL as libc::c_ulong)?;
        let setup = setup_struct();
        let result = unsafe {
            libc::ioctl(fd, UI_DEV_SETUP, &setup as *const UinputSetup)
        };
        if result < 0 {
            return Err(std::io::Error::last_os_error());
        }
        ioctl(fd, UI_DEV_CREATE, 0)
    })();

    if let Err(err) = setup {
        drop(slot);
        set_error(format!("cannot configure the virtual keyboard: {err}"));
        return false;
    }

    diagnostic::event("uinput", "device_created", &[]);
    *slot = Some(file);
    true
}

/// Injects CapsLock press+release, restoring the lock state.
///
/// The two events go in separate SYN frames: niri accepts a combined frame,
/// some compositors fold it away.
pub fn restore_capslock() {
    let mut slot = match DEVICE.lock() {
        Ok(slot) => slot,
        Err(poisoned) => poisoned.into_inner(),
    };
    let Some(file) = slot.as_mut() else {
        return;
    };

    let events = [
        InputEvent {
            kind: EV_KEY,
            code: KEY_CAPSLOCK,
            value: 1,
            ..Default::default()
        },
        InputEvent {
            kind: EV_SYN,
            code: SYN_REPORT,
            value: 0,
            ..Default::default()
        },
        InputEvent {
            kind: EV_KEY,
            code: KEY_CAPSLOCK,
            value: 0,
            ..Default::default()
        },
        InputEvent {
            kind: EV_SYN,
            code: SYN_REPORT,
            value: 0,
            ..Default::default()
        },
    ];
    for event in events {
        let bytes = unsafe {
            std::slice::from_raw_parts(
                &event as *const InputEvent as *const u8,
                std::mem::size_of::<InputEvent>(),
            )
        };
        if let Err(err) = file.write_all(bytes) {
            drop(slot);
            set_error(format!("cannot write to /dev/uinput: {err}"));
            return;
        }
    }
    diagnostic::event("uinput", "capslock_restored", &[]);
}

/// Destroys the virtual keyboard. Called at shutdown.
pub fn close() {
    let mut slot = match DEVICE.lock() {
        Ok(slot) => slot,
        Err(poisoned) => poisoned.into_inner(),
    };
    if let Some(file) = slot.as_ref() {
        let _ = ioctl(file.as_raw_fd(), UI_DEV_DESTROY, 0);
    }
    *slot = None;
}

/// Last error, or None when the device is usable.
pub fn error() -> Option<String> {
    match LAST_ERROR.lock() {
        Ok(slot) => slot.clone(),
        Err(poisoned) => poisoned.into_inner().clone(),
    }
}
