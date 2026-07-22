//! Network system-call support -- Rust port of `kernel/sysnet.c` (Phase
//! 2 Wave 28, sub-wave B -- see `docs/rustify/phase2_plan.md`; the final
//! porting wave's network drivers). Socket management and UDP packet
//! handling. Legacy `sockalloc` was already removed in the C original --
//! VFS uses `vfs_sockalloc` in `kernel/vfs/file.rs` (Phase 2 Wave 14)
//! instead, which is this file's one live cross-module dependent: it
//! locks [`SOCKETS`] directly (a `pub(crate)` crate-path item, not a raw
//! C-ABI extern -- see Wave P3-8d below) and calls `mbufq_init`
//! (`net.rs`, this wave) to initialise the `rxq` of a `struct sock` it
//! constructs itself via `kalloc()`.
//!
//! **Wave P3-8d**: `sock_lock`/`sockets` (originally two independently-
//! paired C globals -- a bare `spinlock_t` plus a separate `*mut sock`)
//! were migrated to [`SOCKETS`], a single `crate::sync::SpinLock<*mut
//! sock>` that owns the list head it protects directly. `vfs/file.rs`'s
//! `vfs_sockalloc` (previously a `KSpinlock::from_bindings` handle onto
//! the raw `spinlock_t`, plus an `extern "C" { static mut sockets }`
//! redeclaration reinterpreting the pointer as its own local `sock`
//! mirror type) now locks this same static and casts the guard's `*mut
//! sock` (this file's type) to/from its own mirror type at the two
//! points it touches the list -- see that file's updated lock-map doc.
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

use crate::bindings::{mbuf, thread, EFAULT, EINTR, ENOMEM};
use crate::proc::proc_shims::xv6_current_thread;
// P3-D2a: proc/sched.rs sleep/wake entry points, reached as plain
// crate-path items instead of `extern "C"` redeclarations. N-R7e: the
// recv wait now goes through the `SpinLockGuard::sleep_on_interruptible`
// method (see [`sockread`]), so the free `sleep_on_chan_interruptible` is
// no longer imported here -- only the wake side (`wakeup_on_chan`) is.
use crate::proc::Scheduler;
// P3-D2b: `killed` (proc/signal.rs) likewise (identical signature).
use crate::net::mbufq;
use crate::sync::SpinLock;

// N-R7e: the per-socket `spin_lock`/`spin_unlock` wrapper pair that used
// to live here is gone -- the per-socket lock now owns its state
// ([`SockInner`] inside [`sock`], via `crate::sync::SpinLock`), so every
// former manual `spin_lock(&(*si).lock) ... spin_unlock` critical section
// is now a `(*si).inner.lock()` RAII guard whose `Deref`/`sleep_on_*`
// handle the acquire/release/wait. Nothing else in this file drove those
// wrappers, so both they and the `spinlock_t` import they needed are
// retired.

// P3-D3a: `vm_copyout`/`vm_copyin` (mm/vm.rs) are ordinary (safe) Rust
// fns now that their `#[no_mangle]` exports are gone; reached as
// crate-path items instead of the `extern "C"` redeclarations that used
// to sit in the block above (identical signatures). `kfree` stays a
// genuinely `unsafe fn` (`crate::mm::kalloc`); its one call site below
// already sits in an `unsafe` block, so the plain `use` keeps it
// unchanged too.
// `kfree` is now `impl Kmem` (kalloc.rs KERNEL-OO wave); called
// fully-qualified at its one call site below instead of `use`-imported.
use crate::mm::Vm;
// P3-1D mesh sweep: net.rs is in scope for this wave; signatures are
// identical, so these become plain crate-path imports instead of `extern
// "C"` redeclarations.
use crate::net::{Mbuf, Net};

/// Zero-sized syscall-support-state type for this file's socket helpers
/// (P3-10c precedent's ZST shape) -- KERNEL-OO home for the free fns
/// below. None of these are `SYSCALLS` dispatch-table entries (per the
/// module doc, `sys_connect` dispatches to `vfs_syscall.rs`'s
/// `sys_vfs_connect`, not anything in this file) and none is
/// address-taken elsewhere -- grep-verified -- so none is FLOOR. Raw
/// params kept, NO `&self` receiver (uniform with this file's sibling
/// drivers).
pub(crate) struct SysNet;

impl SysNet {
#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}
} // impl SysNet (neg)

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
    // N-R7e (lock-owns-data): the per-socket `lock: spinlock_t` +
    // `rxq: mbufq` pair is now a single lock that OWNS the state it
    // protects. `SpinLock<T>` is `#[repr(C)] { UnsafeCell<spinlock_t>,
    // UnsafeCell<T> }` and `UnsafeCell`/[`SockInner`] are `#[repr(C)]`
    // one-field transparent wrappers, so `SpinLock<SockInner>` lays out
    // as `spinlock_t` immediately followed by `mbufq` -- byte-identical
    // to the old `lock, rxq` pair. That layout-identity is load-bearing:
    // `kernel/vfs/file.rs` keeps its own separate `struct sock` mirror
    // (out of this wave's edit scope) that `spin_init`s the lock and
    // `mbufq_init`s the rxq at these exact offsets on a raw `kalloc()`
    // page, then hands the pointer to [`SOCKETS`]; that construction (and
    // the `crate::bindings::sock` = `crate::sysnet::sock` re-export it
    // casts through) stays valid unchanged because the bytes are the same.
    pub inner: SpinLock<SockInner>,
}

/// Per-socket lock-protected state (N-R7e). Only the receive queue is
/// actually guarded by the per-socket lock: `raddr`/`lport`/`rport` above
/// are set once at `vfs_sockalloc` time and read lock-free thereafter
/// (the match in [`sockrecvudp`] runs under [`SOCKETS`], not this lock;
/// [`sockwrite`] reads them with no lock at all), matching the C original.
/// `#[repr(C)]` single-field wrapper so `SpinLock<SockInner>` keeps the
/// exact `spinlock_t`-then-`mbufq` byte layout the `vfs/file.rs` mirror
/// depends on (see [`sock`]).
#[repr(C)]
pub struct SockInner {
    pub rxq: mbufq, // a queue of packets waiting to be received
}

/// Head of the (unsorted) list of live sockets. Wave P3-8d: `sock_lock`
/// (a bare `spinlock_t`, `SPINLOCK_INITIALIZED("socktbl")`) and
/// `sockets` (a separate `#[no_mangle] static mut *mut sock`) used to be
/// two independently-paired globals -- manual `spin_lock`/`spin_unlock`
/// here, and a `KSpinlock::from_bindings` handle onto the same raw
/// `spinlock_t` from `vfs/file.rs::vfs_sockalloc` (the one live
/// cross-file accessor -- see that file's own lock-map doc). Now the
/// lock owns the list head directly (`crate::sync::SpinLock`, same
/// P3-8b/8c/8d precedent as `ramdisk.rs`/`bufcache.rs`/`e1000.rs`);
/// `vfs/file.rs` locks this same `pub(crate)` static instead of a raw
/// `spinlock_t` handle. No longer `#[no_mangle]` -- the only remaining
/// reader (`vfs/file.rs`) is same-crate and reaches it via a plain
/// crate-path `use`, so the C-linkage anchor this file's own P3-1D
/// comment used to document is no longer needed.
pub(crate) static SOCKETS: SpinLock<*mut sock> = SpinLock::new(c"socktbl", ptr::null_mut());

impl SysNet {
/// `void sockinit(void) {}` -- empty stub, ported as such (matches the
/// plan's explicit instruction for this file).
// P3-1D mesh sweep: caller (`start_kernel.rs`) reaches this via a
// crate-path `use` of `SysNet::sockinit` (not an `extern` redeclaration).
pub(crate) extern "C" fn sockinit() {}

// Legacy `sockalloc` removed -- VFS uses `vfs_sockalloc` in
// `kernel/vfs/file.rs` instead.

/// Closes a socket: unlinks it from the global [`SOCKETS`] list, frees
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
    // Remove from the list of sockets. `guard` drops (RAII) at the end
    // of this block, matching the original's `spin_unlock` placement
    // exactly (released before the unlocked mbuf-freeing work below).
    {
        let mut guard = SOCKETS.lock();
        // SAFETY: `guard` proves `SOCKETS`'s lock is held; `*guard`/
        // `(*pos).next` form a plain singly-linked list, walked and
        // unlinked under the lock.
        unsafe {
            let mut pos: *mut *mut sock = &raw mut *guard;
            while !(*pos).is_null() {
                if *pos == si {
                    *pos = (*si).next;
                    break;
                }
                pos = &raw mut (*(*pos)).next;
            }
        }
    }

    // Free any pending mbufs. `si` is unlinked (above), so no other
    // thread can reach it -- its `rxq` is exclusively owned and touched
    // WITHOUT taking the per-socket lock, exactly as the C original did.
    // `inner.data_ptr()` is the documented `UnsafeCell::get`-style escape
    // hatch for precisely this case (state protected by an external
    // protocol -- here exclusive ownership -- rather than the lock);
    // producing the pointer is safe, and the raw mbufq list ops on it stay
    // raw (the intrusive-list class, out of this wave's scope).
    // SAFETY: caller contract (`si` a live, exclusively-owned `kalloc`'d
    // `sock`); `rxq` reachable by no other thread now, so the unlocked
    // drain is race-free.
    unsafe {
        let rxq = &raw mut (*(*si).inner.data_ptr()).rxq;
        while mbufq::empty(rxq) == 0 {
            let m = mbufq::pophead(rxq);
            Mbuf::free(m);
        }
        crate::mm::kalloc::Kmem::kfree(si as *mut c_void);
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

    // N-R7e: acquire the per-socket lock-owns-data guard. `s` `DerefMut`s
    // to the guarded [`SockInner`], and its `sleep_on_interruptible`
    // atomically releases+reacquires THIS lock across the wait (the
    // `sleep(chan,&lk)` protocol -- no lost-wakeup window), returning
    // `-EINTR` if a signal arrives. The guard releases on every exit path
    // below (RAII), replacing the four hand-matched `spin_unlock`s.
    // SAFETY: `si` is a live socket (caller contract); creating the
    // `&(*si).inner` borrow to lock it is the only pointer deref here.
    let m = {
        let mut s = unsafe { (*si).inner.lock() };
        // The rxq address is the stable, unique wait channel (never
        // dereferenced as anything but an address -- matches the C
        // `&si->rxq`, and the deliver side [`sockrecvudp`] wakes on the
        // same address). Computed once; the `&raw mut` borrow of the
        // guard ends immediately, leaving a plain pointer.
        let chan = &raw mut s.rxq as *mut c_void;
        // SAFETY: `s.rxq` is the guarded rxq; the raw mbufq list op stays
        // raw (intrusive-list class, out of scope). Lock held via `s`.
        while unsafe { mbufq::empty(&raw mut s.rxq) } != 0 && crate::proc::access::ThreadAccess::from_ptr(pr).map_or(0, |ta| ta.killed()) == 0 {
            if s.sleep_on_interruptible(chan) != 0 {
                return Self::neg(EINTR); // `s` drops here -> unlock
            }
        }
        if crate::proc::access::ThreadAccess::from_ptr(pr).map_or(0, |ta| ta.killed()) != 0 {
            return Self::neg(EINTR); // `s` drops here -> unlock
        }
        // SAFETY: lock held via `s`, rxq non-empty (loop invariant above).
        unsafe { mbufq::pophead(&raw mut s.rxq) }
    }; // `s` drops here -> unlock

    // SAFETY: `m` live (popped above, exclusively owned by this call
    // now); `pr` is the live current thread.
    let mut len = unsafe { (*m).len } as c_int;
    if len > n {
        len = n;
    }
    // SAFETY: `pr` live; `m` live; `(*pr).vm` is the current thread's
    // live address space.
    if unsafe { Vm::vm_copyout((*pr).vm, addr, (*m).head as *const c_void, len as u64) } < 0 {
        unsafe { Mbuf::free(m) };
        return Self::neg(EFAULT);
    }
    unsafe { Mbuf::free(m) };
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

    let m = Mbuf::alloc(MBUF_DEFAULT_HEADROOM);
    if m.is_null() {
        return Self::neg(ENOMEM);
    }

    // SAFETY: `m` live; `pr` is the live current thread.
    let dst = unsafe { Mbuf::put(m, n as core::ffi::c_uint) };
    if unsafe { Vm::vm_copyin((*pr).vm, dst as *mut c_void, addr, n as u64) } < 0 {
        unsafe { Mbuf::free(m) };
        return Self::neg(EFAULT);
    }
    // SAFETY: `si` live; `m` live, ownership transferred to `net_tx_udp`.
    unsafe { Net::net_tx_udp(m, (*si).raddr, (*si).lport, (*si).rport) };
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
    let guard = SOCKETS.lock();
    // `*guard` is a plain singly-linked list, readable through the held
    // guard with no `unsafe` needed for the field access itself.
    let mut si = *guard;
    while !si.is_null() {
        // SAFETY: `si` is a live node on the `sockets` list.
        if unsafe { (*si).raddr == raddr && (*si).lport == lport && (*si).rport == rport } {
            // Found -- deliver. N-R7e: take the per-socket lock-owns-data
            // guard; enqueue `m` and wake the sleeping reader while it is
            // held (matches the C `acquire(&si->lock); push; wakeup;
            // release` window exactly). `s` releases on scope exit (RAII).
            // Held nested under `SOCKETS` (`guard`), same order as before.
            // SAFETY: `si` is a live node on the sockets list; creating
            // `&(*si).inner` to lock it is the only pointer deref here.
            let mut s = unsafe { (*si).inner.lock() };
            // Same wait channel the reader sleeps on (rxq address).
            let chan = &raw mut s.rxq as *mut c_void;
            // SAFETY: `m` is caller-owned, ownership transferred into the
            // rxq; the raw mbufq list op stays raw (out of scope). Lock
            // held via `s`.
            unsafe { mbufq::pushtail(&raw mut s.rxq, m) };
            Scheduler::wakeup_on_chan(chan);
            return; // `s` then `guard` drop here (RAII).
        }
        si = unsafe { (*si).next };
    }
    // Release the lock before invoking `mbuffree` (matches the
    // original's lock-hold window: `sock_lock`/`SOCKETS` is never held
    // across a call into another function).
    drop(guard);
    // SAFETY: `m` still caller-owned (no socket matched).
    unsafe { Mbuf::free(m) };
}
} // impl SysNet (sockinit/sockclose/sockread/sockwrite/sockrecvudp)
