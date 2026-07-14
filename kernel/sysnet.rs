//! Network system-call support -- Rust port of `kernel/sysnet.c` (Phase
//! 2 Wave 28, sub-wave B -- see `docs/rustify/phase2_plan.md`; the final
//! porting wave's network drivers). Socket management and UDP packet
//! handling. Legacy `sockalloc` was already removed in the C original --
//! VFS uses `vfs_sockalloc` in `kernel/vfs/file.rs` (Phase 2 Wave 14)
//! instead, which is this file's one live cross-module dependent: it
//! `extern`s [`sock_lock`]/[`sockets`] directly (as plain C globals) and
//! calls `mbufq_init` (`net.rs`, this wave) to initialise the `rxq` of a
//! `struct sock` it constructs itself via `kalloc()` -- `sock_lock`/
//! `sockets` are preserved below under the exact same symbol names/
//! types `vfs/file.rs` already expects, so that file needs no changes.
//!
//! # `sys_connect` / dead-code note
//!
//! The Wave 28 charter (per the plan) describes this file as owning
//! "`sys_connect` + `sockinit` stub" -- investigated at port time and
//! found stale: `SYS_connect` (`kernel/inc/uabi/syscall.h`) has
//! dispatched to `sys_vfs_connect` (`kernel/vfs/vfs_syscall.rs`, already
//! Rust since Phase 2 Wave 17) since before this wave started, and
//! `sysnet.c` itself never defined a `sys_connect` function -- grepped
//! the whole tree, confirmed. What *is* real and ported here verbatim:
//! [`sockinit`] (an empty stub, matches the C exactly) and
//! [`sockclose`]/[`sockread`]/[`sockwrite`] -- all three declared
//! `extern` in the still-live `kernel/inc/defs.h` (forced per this
//! crate's "every symbol in a still-live header" rule) but, per a
//! call-site grep across the whole tree (both C-era and this session's
//! own audit), **none of the three has ever had a live caller**:
//! `vfs_sockalloc`'s file has no `.ops` (sockets bypass the `vfs_file_ops`
//! read/write dispatch entirely) and `vfs_fput` never branches into the
//! `inode == NULL` (socket) case for cleanup (see `vfs/file.rs`'s own
//! `// Sockets are not opened via inodes, so no cleanup here.` comment)
//! -- so `sockclose` is dead, and with no read/write dispatch reaching a
//! socket file, so are `sockread`/`sockwrite`. This is a pre-existing
//! gap from Wave 14's VFS integration, not introduced by this port --
//! ported faithfully (not fixed, not dropped) per the plan's explicit
//! "port as such" instruction for this file.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_int, c_void};
use core::ptr;

use crate::bindings::{mbuf, spinlock_t, thread, vm, EFAULT, EINTR, ENOMEM};
use crate::net::mbufq;

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
unsafe extern "C" {
    // lock/spinlock.rs -- raw calls (matches this crate's convention for
    // straight-line critical sections with early-return-while-holding
    // exit paths, e.g. `sockread`'s interrupted-sleep path).
    safe fn spin_lock(l: *mut spinlock_t);
    safe fn spin_unlock(l: *mut spinlock_t);

    // string.rs.
    fn kfree(pa: *mut c_void);

    // proc/proc_shims.rs, proc/signal.rs, proc/sched.rs.
    safe fn xv6_current_thread() -> *mut thread;
    safe fn killed(p: *mut thread) -> c_int;
    safe fn sleep_on_chan_interruptible(chan: *mut c_void, lk: *mut spinlock_t) -> c_int;
    safe fn wakeup_on_chan(chan: *mut c_void);

    // mm/vm.rs.
    safe fn vm_copyout(vm_ptr: *mut vm, dstva: u64, src: *const c_void, len: u64) -> c_int;
    safe fn vm_copyin(vm_ptr: *mut vm, dst: *mut c_void, srcva: u64, len: u64) -> c_int;

}
// P3-1D mesh sweep: net.rs is in scope for this wave; signatures are
// identical, so these become plain crate-path imports instead of `extern
// "C"` redeclarations.
use crate::net::{mbufalloc, mbuffree, mbufput, mbufq_empty, mbufq_pophead, mbufq_pushtail, net_tx_udp};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// `kernel/inc/dev/net.h`: `#define MBUF_DEFAULT_HEADROOM 128` -- same
/// local copy as `kernel/net.rs`'s own (private) constant, per this
/// crate's established per-file convention for small integer macros.
const MBUF_DEFAULT_HEADROOM: core::ffi::c_uint = 128;

/// Socket structure. **This is the real, canonical definition** --
/// `kernel/vfs/file.rs` keeps its own separately-maintained local
/// mirror (predates this wave, out of touch scope; see this file's
/// module doc), which is byte-layout-identical by construction (same
/// field order/types) but a distinct Rust type. Only the pointer
/// crosses the `vfs_sockalloc`/`sock_lock`/`sockets` FFI boundary
/// between the two files, so the type identity mismatch is immaterial
/// (matches every other cross-file raw-pointer handoff in this crate).
#[repr(C)]
pub struct sock {
    pub next: *mut sock, // the next socket in the list
    pub raddr: u32,      // the remote IPv4 address
    pub lport: u16,      // the local UDP port number
    pub rport: u16,      // the remote UDP port number
    pub lock: spinlock_t, // protects the rxq
    pub rxq: mbufq,      // a queue of packets waiting to be received
}

/// `spinlock_t sock_lock = SPINLOCK_INITIALIZED("socktbl");` --
/// compile-time initialised (matches `kernel/uart.rs`'s established
/// convention for `SPINLOCK_INITIALIZED` statics). Keeps the exact C
/// symbol name/type.
// P3-1D mesh sweep: caller (`vfs/file.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) static mut sock_lock: spinlock_t =
    spinlock_t { locked: 0, name: c"socktbl".as_ptr() as *mut core::ffi::c_char, cpu: ptr::null_mut() };

/// `struct sock *sockets;` -- head of the (unsorted) list of live
/// sockets, protected by [`sock_lock`]. Keeps the exact C symbol name/
/// type. Kept `#[no_mangle]` -- read via `extern` by 3 out-of-scope
/// files (`vfs/file.rs`, `vfs/vfs_syscall.rs`, `vfs/tmpfs/inode.rs`);
/// same "widely-shared data anchor, no benefit purely for mesh-cosmetic
/// reasons" judgment call `ipi.rs`'s `cpus` documents.
#[no_mangle]
pub static mut sockets: *mut sock = ptr::null_mut();

/// `void sockinit(void) {}` -- empty stub, ported as such (matches the
/// plan's explicit instruction for this file).
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn sockinit() {}

// Legacy `sockalloc` removed -- VFS uses `vfs_sockalloc` in
// `kernel/vfs/file.rs` instead.

/// Closes a socket: unlinks it from the global [`sockets`] list, frees
/// any pending mbufs in its rxq, and frees the socket itself. See
/// module doc: no live caller in the tree (preserved verbatim).
///
/// # Safety
/// `si` must be a live, exclusively-owned `sock` allocated via
/// `kalloc()` (matches `vfs_sockalloc`'s allocation).
// P3-1D mesh sweep: no live caller anywhere in the tree today (see
// module doc) -- demoted; `#[allow(dead_code)]` documents the gap.
#[allow(dead_code)]
pub(crate) unsafe extern "C" fn sockclose(si: *mut sock) {
    // Remove from the list of sockets.
    spin_lock(unsafe { &raw mut sock_lock });
    // SAFETY: `sock_lock` held; `sockets`/`(*node).next` form a plain
    // singly-linked list, walked and unlinked under the lock.
    unsafe {
        let mut pos: *mut *mut sock = &raw mut sockets;
        while !(*pos).is_null() {
            if *pos == si {
                *pos = (*si).next;
                break;
            }
            pos = &raw mut (*(*pos)).next;
        }
    }
    spin_unlock(unsafe { &raw mut sock_lock });

    // Free any pending mbufs.
    // SAFETY: caller contract; no other thread can reach `si` anymore
    // (unlinked above), so its rxq is now exclusively owned.
    unsafe {
        while mbufq_empty(&raw mut (*si).rxq) == 0 {
            let m = mbufq_pophead(&raw mut (*si).rxq);
            mbuffree(m);
        }
        kfree(si as *mut c_void);
    }
}

/// Reads up to `n` bytes from a socket's receive queue into user memory
/// at `addr`. Blocks (interruptibly) until a packet is available.
/// Returns the number of bytes read, or a negative errno. See module
/// doc: no live caller in the tree (preserved verbatim).
///
/// # Safety
/// `si` must be live.
// P3-1D mesh sweep: no live caller anywhere in the tree today (see
// module doc) -- demoted; `#[allow(dead_code)]` documents the gap.
#[allow(dead_code)]
pub(crate) unsafe extern "C" fn sockread(si: *mut sock, addr: u64, n: c_int) -> c_int {
    let pr = xv6_current_thread();

    spin_lock(unsafe { &raw mut (*si).lock });
    // SAFETY: `si` live, lock held.
    while unsafe { mbufq_empty(&raw mut (*si).rxq) } != 0 && killed(pr) == 0 {
        // SAFETY: `si`'s rxq address is a stable, unique wait-channel
        // value (never dereferenced as anything but an address, matches
        // the C `&si->rxq` usage); `(*si).lock` held.
        if sleep_on_chan_interruptible(unsafe { &raw mut (*si).rxq } as *mut c_void, unsafe { &raw mut (*si).lock }) != 0 {
            spin_unlock(unsafe { &raw mut (*si).lock });
            return neg(EINTR);
        }
    }
    if killed(pr) != 0 {
        spin_unlock(unsafe { &raw mut (*si).lock });
        return neg(EINTR);
    }
    // SAFETY: `si` live, lock held, rxq non-empty (loop invariant above).
    let m = unsafe { mbufq_pophead(&raw mut (*si).rxq) };
    spin_unlock(unsafe { &raw mut (*si).lock });

    // SAFETY: `m` live (popped above, exclusively owned by this call
    // now); `pr` is the live current thread.
    let mut len = unsafe { (*m).len } as c_int;
    if len > n {
        len = n;
    }
    // SAFETY: `pr` live; `m` live; `(*pr).vm` is the current thread's
    // live address space.
    if unsafe { vm_copyout((*pr).vm, addr, (*m).head as *const c_void, len as u64) } < 0 {
        unsafe { mbuffree(m) };
        return neg(EFAULT);
    }
    unsafe { mbuffree(m) };
    len
}

/// Writes `n` bytes from user memory at `addr` as a UDP packet to a
/// socket's configured remote endpoint. Returns `n` on success, or a
/// negative errno. See module doc: no live caller in the tree
/// (preserved verbatim).
///
/// # Safety
/// `si` must be live.
// P3-1D mesh sweep: no live caller anywhere in the tree today (see
// module doc) -- demoted; `#[allow(dead_code)]` documents the gap.
#[allow(dead_code)]
pub(crate) unsafe extern "C" fn sockwrite(si: *mut sock, addr: u64, n: c_int) -> c_int {
    let pr = xv6_current_thread();

    let m = mbufalloc(MBUF_DEFAULT_HEADROOM);
    if m.is_null() {
        return neg(ENOMEM);
    }

    // SAFETY: `m` live; `pr` is the live current thread.
    let dst = unsafe { mbufput(m, n as core::ffi::c_uint) };
    if unsafe { vm_copyin((*pr).vm, dst as *mut c_void, addr, n as u64) } < 0 {
        unsafe { mbuffree(m) };
        return neg(EFAULT);
    }
    // SAFETY: `si` live; `m` live, ownership transferred to `net_tx_udp`.
    unsafe { net_tx_udp(m, (*si).raddr, (*si).lport, (*si).rport) };
    n
}

/// Called by the protocol handler layer ([`crate::net::net_rx_udp`... via
/// `net.rs`'s internal call]) to deliver UDP packets: finds the socket
/// matching `(raddr, lport, rport)` and pushes `m` onto its rxq, waking
/// any sleeping reader. Frees `m` if no socket matches.
///
/// # Safety
/// `m` must be live, exclusively owned (ownership transferred to this
/// function either way -- delivered or freed).
// P3-1D mesh sweep: caller (`net.rs`) now imports this via crate-path
// `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn sockrecvudp(m: *mut mbuf, raddr: u32, lport: u16, rport: u16) {
    spin_lock(unsafe { &raw mut sock_lock });
    // SAFETY: `sock_lock` held; `sockets` is a plain singly-linked list.
    let mut si = unsafe { sockets };
    while !si.is_null() {
        // SAFETY: `si` is a live node on the `sockets` list.
        if unsafe { (*si).raddr == raddr && (*si).lport == lport && (*si).rport == rport } {
            // Found -- deliver.
            spin_lock(unsafe { &raw mut (*si).lock });
            // SAFETY: `si` live; `m` caller-owned, transferred to the
            // rxq.
            unsafe {
                mbufq_pushtail(&raw mut (*si).rxq, m);
                wakeup_on_chan(&raw mut (*si).rxq as *mut c_void);
            }
            spin_unlock(unsafe { &raw mut (*si).lock });
            spin_unlock(unsafe { &raw mut sock_lock });
            return;
        }
        si = unsafe { (*si).next };
    }
    spin_unlock(unsafe { &raw mut sock_lock });
    // SAFETY: `m` still caller-owned (no socket matched).
    unsafe { mbuffree(m) };
}
