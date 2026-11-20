//! Each request snapshots a network generation. Route notifications advance
//! it without taking any lock held by an HTTP transfer.

use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::sync::Mutex;

pub(crate) trait Network: Send + Sync {
    fn generation(&self) -> u64;
}

pub(crate) struct SystemNetwork(Mutex<State>);

struct State {
    routes: Option<OwnedFd>,
    generation: u64,
}

impl SystemNetwork {
    pub fn new() -> Self {
        let routes = match route_socket() {
            Ok(fd) => Some(fd),
            Err(error) => {
                eprintln!("Ainput: cannot watch network routes: {error}");
                None
            }
        };
        Self(Mutex::new(State {
            routes,
            generation: 0,
        }))
    }
}

impl Network for SystemNetwork {
    fn generation(&self) -> u64 {
        let mut state = self.0.lock().unwrap();
        if let Some(fd) = &state.routes {
            let mut changed = false;
            // Datagrams need not be parsed: every subscribed group concerns
            // links, interface addresses, or IPv4/IPv6 routes.
            for _ in 0..64 {
                let mut byte = 0u8;
                let rc = unsafe {
                    libc::recv(
                        fd.as_raw_fd(),
                        (&mut byte as *mut u8).cast(),
                        1,
                        libc::MSG_DONTWAIT,
                    )
                };
                if rc < 0 {
                    match io::Error::last_os_error().raw_os_error() {
                        Some(libc::EINTR) => continue,
                        Some(libc::ENOBUFS) => {
                            changed = true;
                            continue;
                        }
                        _ => break,
                    }
                }
                changed = true;
            }
            if changed {
                state.generation += 1;
            }
        }
        state.generation
    }
}

fn route_socket() -> io::Result<OwnedFd> {
    let fd = unsafe {
        libc::socket(
            libc::AF_NETLINK,
            libc::SOCK_RAW | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
            libc::NETLINK_ROUTE,
        )
    };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    let fd = unsafe { OwnedFd::from_raw_fd(fd) };
    let mut address: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
    address.nl_family = libc::AF_NETLINK as libc::sa_family_t;
    address.nl_groups = (libc::RTMGRP_LINK
        | libc::RTMGRP_IPV4_IFADDR
        | libc::RTMGRP_IPV6_IFADDR
        | libc::RTMGRP_IPV4_ROUTE
        | libc::RTMGRP_IPV6_ROUTE
        | libc::RTMGRP_IPV4_RULE) as u32;
    let rc = unsafe {
        libc::bind(
            fd.as_raw_fd(),
            (&address as *const libc::sockaddr_nl).cast(),
            std::mem::size_of_val(&address) as libc::socklen_t,
        )
    };
    if rc < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(fd)
    }
}

#[cfg(test)]
#[derive(Default)]
pub(crate) struct MockNetwork(pub std::sync::atomic::AtomicU64);

#[cfg(test)]
impl Network for MockNetwork {
    fn generation(&self) -> u64 {
        self.0.load(std::sync::atomic::Ordering::SeqCst)
    }
}
