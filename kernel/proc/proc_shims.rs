//! Rust ports of small accessor / wrapper functions formerly hosted in
//! `kernel/proc/proc_rust_shims.c`. The C accumulator is being demolished
//! one section at a time; each batch lives here until it can be folded into
//! the relevant `*.rs` module.
//!
//! ## Centralized-unsafe convention
//!
//! All FFI declarations live in the single `unsafe extern "C" { ... }` block
//! below. Pure-Rust statics that *replace* C globals (e.g. the pgroup slab
//! cache) are wrapped in tiny `Sync` newtypes so they can live at file scope
//! without per-call `unsafe`.

#![allow(non_camel_case_types, non_snake_case)]

macro_rules! u {
    ($($tokens:tt)*) => {
        unsafe { $($tokens)* }
    };
}

use crate::bindings::{
    ksiginfo, pgroup, session, slab_alloc, slab_cache_init, slab_cache_t, slab_free, thread, thread_group, SLAB_FLAG_EMBEDDED,
};
use crate::proc::access::smp_load_acquire_i32;
use crate::machine::smp_load_acquire_u64;
use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;

macro_rules! field_get {
    ($ptr:expr, $field:ident) => {{ u! { (*($ptr)).$field } }};
    ($ptr:expr, $field:ident.$subfield:ident) => {{ u! { (*($ptr)).$field.$subfield } }};
    ($ptr:expr, $field:ident.$subfield:ident.$leaf:ident) => {{ u! { (*($ptr)).$field.$subfield.$leaf } }};
}

macro_rules! field_set {
    ($ptr:expr, $field:ident, $value:expr) => {{ u! { (*($ptr)).$field = $value } }};
    ($ptr:expr, $field:ident.$subfield:ident, $value:expr) => {{ u! { (*($ptr)).$field.$subfield = $value } }};
    ($ptr:expr, $field:ident.$subfield:ident.$leaf:ident, $value:expr) => {{ u! { (*($ptr)).$field.$subfield.$leaf = $value } }};
}

macro_rules! field_ptr_mut {
    ($ptr:expr, $field:ident) => {{ u! { &raw mut (*($ptr)).$field } }};
    ($ptr:expr, $field:ident.$subfield:ident) => {{ u! { &raw mut (*($ptr)).$field.$subfield } }};
    ($ptr:expr, $field:ident.$subfield:ident.$leaf:ident) => {{ u! { &raw mut (*($ptr)).$field.$subfield.$leaf } }};
}

macro_rules! field_ptr_const {
    ($ptr:expr, $field:ident) => {{ u! { &raw const (*($ptr)).$field } }};
    ($ptr:expr, $field:ident.$subfield:ident) => {{ u! { &raw const (*($ptr)).$field.$subfield } }};
    ($ptr:expr, $field:ident.$subfield:ident.$leaf:ident) => {{ u! { &raw const (*($ptr)).$field.$subfield.$leaf } }};
}

macro_rules! field_add_assign {
    ($ptr:expr, $field:ident, $value:expr) => {{ u! { (*($ptr)).$field += $value } }};
}

macro_rules! field_sub_assign {
    ($ptr:expr, $field:ident, $value:expr) => {{ u! { (*($ptr)).$field -= $value } }};
}

macro_rules! bit_get {
    ($ptr:expr, $anon:ident, $method:ident) => {{ u! { (*($ptr)).$anon.$method() } }};
}

macro_rules! bit_set {
    ($ptr:expr, $anon:ident, $method:ident, $value:expr) => {{ u! { (*($ptr)).$anon.$method($value) } }};
}

macro_rules! list_init_field {
    ($ptr:expr, $($field:tt)+) => {{ u! { list_init(field_ptr_mut!($ptr, $($field)+)) } }};
}

macro_rules! list_detach_field {
    ($ptr:expr, $($field:tt)+) => {{ u! { list_detach(field_ptr_mut!($ptr, $($field)+)) } }};
}

macro_rules! list_is_detached_field {
    ($ptr:expr, $($field:tt)+) => {{ u! { list_is_detached(field_ptr_mut!($ptr, $($field)+)) as c_int } }};
}

macro_rules! list_push_back_fields {
    ($head_ptr:expr, $($head_field:ident)+, $entry_ptr:expr, $($entry_field:ident)+) => {{
        u! { list_push_back(field_ptr_mut!($head_ptr, $($head_field)+), field_ptr_mut!($entry_ptr, $($entry_field)+)) }
    }};
}

macro_rules! atomic_load_i32_field {
    ($ptr:expr, $($field:tt)+) => {{ smp_load_acquire_i32(field_ptr_const!($ptr, $($field)+)) }};
}

macro_rules! atomic_fetch_sub_i32_field {
    ($ptr:expr, $field:ident, $value:expr) => {{ u! { atomic_fetch_sub_i32(field_ptr_mut!($ptr, $field), $value) } }};
    ($ptr:expr, $field:ident.$subfield:ident, $value:expr) => {{ u! { atomic_fetch_sub_i32(field_ptr_mut!($ptr, $field.$subfield), $value) } }};
}

// ---------------------------------------------------------------------------
// FFI — kernel symbols not (yet) covered by the bindgen allowlist.
// SAFETY: each is a real kernel linker symbol with the documented C ABI.
// Passing them raw pointers preserves the same precondition contract as the
// underlying C call site.
// ---------------------------------------------------------------------------
unsafe extern "C" {

    // Defined in proc_rust_shims.c SECTION 15 (thread_group body) — will
    // become a direct Rust call once that section is ported.
    fn xv6_tgport_tg_signal_send(tg: *mut thread_group, info: *mut ksiginfo) -> c_int;

    // Defined in proc_rust_shims.c SECTION 17 (thread body) — likewise.
    fn xv6_thport_tcb_lock(t: *mut crate::bindings::thread);
    fn xv6_thport_tcb_unlock(t: *mut crate::bindings::thread);
}

// ===========================================================================
// SECTION 6: pgroup slab cache (was static in pgroup.c, then in
// proc_rust_shims.c). The cache itself moves to Rust file scope; the public
// xv6_pgroup_slab_{init,alloc,free} entry points keep their C ABI names so
// that pgroup.rs (and any future C caller) link unchanged.
// ===========================================================================

/// Thin `Sync` wrapper for the file-scope pgroup slab cache. The C kernel
/// originally placed the cache in `.bss` (`= {0}`) and protected it via the
/// slab allocator's own internal locks; nothing else accesses the storage.
#[repr(transparent)]
struct SlabCacheCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: slab_cache_t's internal locking serialises all field mutation;
// see kernel/inc/mm/slab.h. Sharing the storage between CPUs is what the
// allocator was designed for.
unsafe impl Sync for SlabCacheCell {}

static PGROUP_SLAB: SlabCacheCell = SlabCacheCell(UnsafeCell::new(MaybeUninit::zeroed()));

fn pgroup_slab_ptr() -> *mut slab_cache_t {
    PGROUP_SLAB.0.get() as *mut slab_cache_t
}

#[no_mangle]
pub extern "C" fn xv6_pgroup_slab_init() -> c_int {
    const NAME: &[u8] = b"pgroup_cache\0";
    // SAFETY: NAME is a valid 'static NUL-terminated string; the cache
    // pointer is a unique, properly-aligned static.
    u! {
        slab_cache_init(
            pgroup_slab_ptr(),
            NAME.as_ptr() as *mut c_char,
            core::mem::size_of::<crate::bindings::pgroup>(),
            SLAB_FLAG_EMBEDDED as u64,
        )
    }
}

#[no_mangle]
pub extern "C" fn xv6_pgroup_slab_alloc() -> *mut crate::bindings::pgroup {
    // SAFETY: cache was initialised via xv6_pgroup_slab_init at boot.
    u! { slab_alloc(pgroup_slab_ptr()) as *mut crate::bindings::pgroup }
}

#[no_mangle]
pub extern "C" fn xv6_pgroup_slab_free(pg: *mut crate::bindings::pgroup) {
    // SAFETY: caller guarantees `pg` came from xv6_pgroup_slab_alloc.
    u! { slab_free(pg as *mut c_void) }
}

// ===========================================================================
// SECTION 7: ksiginfo zero-fill helper. Builds a kernel-origin siginfo whose
// only meaningful field is `signo` and forwards to the thread-group signal
// path.
// ===========================================================================
#[no_mangle]
pub extern "C" fn xv6_tg_send_signo(tg: *mut thread_group, signo: c_int) -> c_int {
    // ksiginfo is a plain C struct (no destructor); zero-init via
    // MaybeUninit::zeroed() is layout-equivalent to the C `memset(&info, 0,
    // sizeof(info))`.
    let mut info: ksiginfo = u! { MaybeUninit::<ksiginfo>::zeroed().assume_init() };
    info.signo = signo;
    // SAFETY: tg comes from a Rust caller that already holds an appropriate
    // reference; info points to our own stack frame for the duration of the
    // call (xv6_tgport_tg_signal_send is synchronous and does not retain it).
    u! { xv6_tgport_tg_signal_send(tg, &mut info as *mut ksiginfo) }
}

// ===========================================================================
// SECTION 9: RCU read-side and tcb_lock thin wrappers. The xv6_* names
// existed only to give Rust a fixed symbol when the underlying primitives
// were macros / inline functions. With bindgen exposing `rcu_read_lock` and
// the soon-to-be-ported `xv6_thport_tcb_lock`, the wrappers are still
// useful as the documented Rust-facing names.
// ===========================================================================

#[no_mangle]
pub extern "C" fn xv6_rcu_read_lock() {
    // SAFETY: rcu_read_lock just bumps a per-thread depth counter and
    // disables preemption; no preconditions beyond "in kernel context".
    u! { rcu_read_lock() }
}

#[no_mangle]
pub extern "C" fn xv6_rcu_read_unlock() {
    // SAFETY: must be balanced with a prior xv6_rcu_read_lock; caller's
    // responsibility, same as the C wrapper this replaces.
    u! { rcu_read_unlock() }
}

#[no_mangle]
pub extern "C" fn xv6_tcb_lock(p: *mut crate::bindings::thread) {
    // SAFETY: p must be a live thread the caller has a reference to.
    u! { xv6_thport_tcb_lock(p) }
}

#[no_mangle]
pub extern "C" fn xv6_tcb_unlock(p: *mut crate::bindings::thread) {
    // SAFETY: must be balanced with a prior xv6_tcb_lock(p).
    u! { xv6_thport_tcb_unlock(p) }
}

// SECTION 1 leftover: xv6_current_thread — replaces the `current` macro,
// which expands to an interrupt-safe load of `mycpu()->proc`. We replicate
// the same sstatus.SIE save/restore dance in Rust inline asm.
#[no_mangle]
pub extern "C" fn xv6_current_thread() -> *mut thread {
    crate::machine::current_thread_ptr()
}

// SECTION 1 leftovers: thread_state_short / thread_state_to_str. Pure
// switch tables — direct ports of the static-inline definitions in
// kernel/inc/proc/thread.h.
#[no_mangle]
pub extern "C" fn xv6_thread_state_to_str(st: c_int) -> *const c_char {
    let s: &core::ffi::CStr = match st {
        0  => c"unused",
        1  => c"used",
        2  => c"interruptible",
        3  => c"killable",
        4  => c"timer",
        5  => c"killable_timer",
        6  => c"uninterruptible",
        7  => c"wakening",
        8  => c"running",
        9  => c"stopped",
        10 => c"exiting",
        11 => c"zombie",
        _  => c"*unknown",
    };
    s.as_ptr()
}

#[no_mangle]
pub extern "C" fn xv6_thread_state_short(st: c_int) -> *const c_char {
    let s: &core::ffi::CStr = match st {
        8  => c"R",   // RUNNING
        2  => c"S",   // INTERRUPTIBLE
        6  => c"D",   // UNINTERRUPTIBLE
        9  => c"T",   // STOPPED
        11 => c"Z",   // ZOMBIE
        10 => c"X",   // EXITING
        3  => c"K",   // KIILABLE
        7  => c"W",   // WAKENING
        4  => c"Tm",  // TIMER
        5  => c"Kt",  // KIILABLE_TIMER
        _  => c"?",
    };
    s.as_ptr()
}



// --- SECTION 2: struct thread ---------------------------------------------

#[no_mangle]
pub extern "C" fn t_pid(p: *mut thread) -> c_int { field_get!(p, pid) }
#[no_mangle]
pub extern "C" fn t_tgid(p: *mut thread) -> c_int { field_get!(p, tgid) }
#[no_mangle]
pub extern "C" fn t_pgid(p: *mut thread) -> c_int { field_get!(p, pgid) }
#[no_mangle]
pub extern "C" fn t_sid(p: *mut thread) -> c_int { field_get!(p, sid) }
#[no_mangle]
pub extern "C" fn t_set_pid(p: *mut thread, pid: c_int) { field_set!(p, pid, pid) }
#[no_mangle]
pub extern "C" fn t_set_pgid(p: *mut thread, v: c_int) { field_set!(p, pgid, v) }

#[no_mangle]
pub extern "C" fn t_parent(p: *mut thread) -> *mut thread { field_get!(p, parent) }
#[no_mangle]
pub extern "C" fn t_pgroup(p: *mut thread) -> *mut pgroup { field_get!(p, pgroup) }
#[no_mangle]
pub extern "C" fn t_session(p: *mut thread) -> *mut session { field_get!(p, session) }
#[no_mangle]
pub extern "C" fn t_thread_group(p: *mut thread) -> *mut thread_group {
    field_get!(p, thread_group)
}
#[no_mangle]
pub extern "C" fn t_set_pgroup(p: *mut thread, pg: *mut pgroup) {
    field_set!(p, pgroup, pg)
}

#[no_mangle]
pub extern "C" fn t_vm(p: *mut thread) -> *mut crate::bindings::vm_t { field_get!(p, vm) }
#[no_mangle]
pub extern "C" fn t_fdtable(p: *mut thread) -> *mut crate::bindings::vfs_fdtable { field_get!(p, fdtable) }
#[no_mangle]
pub extern "C" fn t_sigacts(p: *mut thread) -> *mut crate::bindings::sigacts_t { field_get!(p, sigacts) }
#[no_mangle]
pub extern "C" fn t_trapframe(p: *mut thread) -> *mut crate::bindings::utrapframe { field_get!(p, trapframe) }
#[no_mangle]
pub extern "C" fn t_set_vm(p: *mut thread, v: *mut crate::bindings::vm_t) { field_set!(p, vm, v) }
#[no_mangle]
pub extern "C" fn t_set_fdtable(p: *mut thread, v: *mut crate::bindings::vfs_fdtable) { field_set!(p, fdtable, v) }
#[no_mangle]
pub extern "C" fn t_set_sigacts(p: *mut thread, v: *mut crate::bindings::sigacts_t) { field_set!(p, sigacts, v) }
#[no_mangle]
pub extern "C" fn t_set_trapframe(p: *mut thread, v: *mut crate::bindings::utrapframe) { field_set!(p, trapframe, v) }
#[no_mangle]
pub extern "C" fn t_set_parent(p: *mut thread, par: *mut thread) { field_set!(p, parent, par) }
#[no_mangle]
pub extern "C" fn t_set_thread_group(p: *mut thread, tg: *mut thread_group) { field_set!(p, thread_group, tg) }
#[no_mangle]
pub extern "C" fn t_set_session(p: *mut thread, s: *mut session) { field_set!(p, session, s) }
#[no_mangle]
pub extern "C" fn t_set_tgid(p: *mut thread, v: c_int) { field_set!(p, tgid, v) }
#[no_mangle]
pub extern "C" fn t_set_sid(p: *mut thread, v: c_int) { field_set!(p, sid, v) }

// --- SECTION 3: struct pgroup ---------------------------------------------

#[no_mangle]
pub extern "C" fn pg_pgid(pg: *mut pgroup) -> c_int { field_get!(pg, pgid) }
#[no_mangle]
pub extern "C" fn pg_t_cnt(pg: *mut pgroup) -> c_int { field_get!(pg, t_cnt) }
#[no_mangle]
pub extern "C" fn pg_p_cnt(pg: *mut pgroup) -> c_int { field_get!(pg, p_cnt) }
#[no_mangle]
pub extern "C" fn pg_exited(pg: *mut pgroup) -> c_int {
    bit_get!(pg, __bindgen_anon_1, exited) as c_int
}
#[no_mangle]
pub extern "C" fn pg_is_kernel(pg: *mut pgroup) -> c_int {
    bit_get!(pg, __bindgen_anon_1, is_kernel) as c_int
}
#[no_mangle]
pub extern "C" fn pg_session(pg: *mut pgroup) -> *mut session { field_get!(pg, session) }

#[no_mangle]
pub extern "C" fn pg_set_pgid(pg: *mut pgroup, v: c_int) { field_set!(pg, pgid, v) }
#[no_mangle]
pub extern "C" fn pg_set_leader(pg: *mut pgroup, tg: *mut thread_group) {
    field_set!(pg, leader, tg)
}
#[no_mangle]
pub extern "C" fn pg_set_session(pg: *mut pgroup, s: *mut session) {
    field_set!(pg, session, s)
}
#[no_mangle]
pub extern "C" fn pg_set_exited(pg: *mut pgroup, v: c_int) {
    bit_set!(pg, __bindgen_anon_1, set_exited, if v != 0 { 1 } else { 0 })
}
#[no_mangle]
pub extern "C" fn pg_set_t_cnt(pg: *mut pgroup, v: c_int) { field_set!(pg, t_cnt, v) }
#[no_mangle]
pub extern "C" fn pg_set_p_cnt(pg: *mut pgroup, v: c_int) { field_set!(pg, p_cnt, v) }
#[no_mangle]
pub extern "C" fn pg_inc_t_cnt(pg: *mut pgroup) { field_add_assign!(pg, t_cnt, 1) }
#[no_mangle]
pub extern "C" fn pg_dec_t_cnt(pg: *mut pgroup) { field_sub_assign!(pg, t_cnt, 1) }
#[no_mangle]
pub extern "C" fn pg_inc_p_cnt(pg: *mut pgroup) { field_add_assign!(pg, p_cnt, 1) }
#[no_mangle]
pub extern "C" fn pg_dec_p_cnt(pg: *mut pgroup) { field_sub_assign!(pg, p_cnt, 1) }

// --- SECTION 4: struct thread_group ---------------------------------------

#[no_mangle]
pub extern "C" fn tg_pgroup(tg: *mut thread_group) -> *mut pgroup { field_get!(tg, pgroup) }
#[no_mangle]
pub extern "C" fn tg_tgid(tg: *mut thread_group) -> c_int { field_get!(tg, tgid) }
#[no_mangle]
pub extern "C" fn tg_set_pgroup(tg: *mut thread_group, pg: *mut pgroup) {
    field_set!(tg, pgroup, pg)
}
#[no_mangle]
pub extern "C" fn tg_group_leader(tg: *mut thread_group) -> *mut thread { field_get!(tg, group_leader) }
#[no_mangle]
pub extern "C" fn tg_set_tgid(tg: *mut thread_group, v: c_int) { field_set!(tg, tgid, v) }

// --- SECTION 5: struct session --------------------------------------------

#[no_mangle]
pub extern "C" fn session_sid(s: *mut session) -> c_int { field_get!(s, sid) }
#[no_mangle]
pub extern "C" fn session_t_cnt(s: *mut session) -> c_int { field_get!(s, t_cnt) }
#[no_mangle]
pub extern "C" fn session_pg_cnt(s: *mut session) -> c_int { field_get!(s, pg_cnt) }
#[no_mangle]
pub extern "C" fn session_fg_pgrp(s: *mut session) -> *mut pgroup {
    field_get!(s, fg_pgrp)
}

// silence unused warnings during incremental porting
#[allow(dead_code)]
fn _ptr_unused() -> *mut () {
    ptr::null_mut()
}

// ===========================================================================
// LIST PRIMITIVES — Rust reimplementations of the kernel's `static inline`
// `list_entry_*` helpers and `list_node_*` macros from kernel/inc/list.h.
//
// The kernel uses a circular doubly-linked list where every embedded
// `list_node_t` is "detached" (prev == next == self) when not in any list.
// The C side relies on a sentinel head (`list_entry_init(&head)`) and
// inserts ordinary entries with prev/next chained to neighbours.
// ===========================================================================

use crate::bindings::list_node_t;

/// Initialise an embedded list entry to the detached state (prev = next = self).
#[inline]
unsafe fn list_init(e: *mut list_node_t) {
    u! {
        (*e).prev = e;
        (*e).next = e;
    }
}

#[inline]
unsafe fn list_is_detached(e: *mut list_node_t) -> bool {
    u! { (*e).next == e }
}

#[inline]
unsafe fn list_detach(e: *mut list_node_t) {
    u! {
        let prev = (*e).prev;
        let next = (*e).next;
        (*prev).next = next;
        (*next).prev = prev;
        list_init(e);
    }
}

/// `list_entry_push_back(head, new)` — insert `new` immediately before `head`,
/// matching the kernel's static inline.
#[inline]
unsafe fn list_push_back(head: *mut list_node_t, new: *mut list_node_t) {
    u! {
        let prev = (*head).prev;
        (*new).prev = prev;
        (*new).next = head;
        (*prev).next = new;
        (*head).prev = new;
    }
}

/// Iterate `list_foreach_node_safe(head, pos, tmp, member)` invoking `cb` on
/// each containing struct. `member_offset` is the byte offset of the embedded
/// `list_node_t` within the container struct (`core::mem::offset_of!`).
#[inline]
unsafe fn list_foreach_safe<T>(
    head: *mut list_node_t,
    member_offset: usize,
    mut cb: impl FnMut(*mut T),
) {
    u! {
        let mut entry = (*head).next;
        while entry != head && !entry.is_null() {
            let next = (*entry).next;
            let node = (entry as *mut u8).sub(member_offset) as *mut T;
            cb(node);
            entry = next;
        }
    }
}

// ===========================================================================
// ATOMIC PRIMITIVES — Rust replacements for `smp_load_acquire`, `atomic_dec`
// etc. Backed by `core::sync::atomic` over the underlying field pointers.
// `_Atomic` qualifiers on the C side are erased by build.rs macro stubs;
// the underlying storage is still a plain integer, so the atomic ops are
// equivalent.
// ===========================================================================
use core::sync::atomic::{AtomicI32, AtomicU64, Ordering};
use crate::lock::rcu::rcu_read_lock;
use crate::lock::rcu::rcu_read_unlock;
use crate::mm::either_copyout;

#[inline]
unsafe fn atomic_fetch_sub_i32(p: *mut c_int, v: c_int) -> c_int {
    u! { AtomicI32::from_ptr(p as *mut i32).fetch_sub(v, Ordering::SeqCst) }
}

#[inline]
unsafe fn atomic_or_fetch_u64(p: *mut u64, v: u64) -> u64 {
    u! { AtomicU64::from_ptr(p).fetch_or(v, Ordering::SeqCst) | v }
}

// ===========================================================================
// THREAD FLAGS — bit positions from kernel/inc/proc/thread_types.h.
// ===========================================================================
const THREAD_FLAG_USER_SPACE: u64 = 5;
const THREAD_FLAG_SELF_REAP: u64 = 6;

const THREAD_STATE_STOPPED: c_int = 9;
const THREAD_STATE_ZOMBIE: c_int = 11;

#[inline]
unsafe fn thread_user_space(p: *mut thread) -> bool {
    if p.is_null() {
        return false;
    }
    let flags = u! { smp_load_acquire_u64(&(*p).flags as *const u64) };
    (flags & (1u64 << THREAD_FLAG_USER_SPACE)) != 0
}

#[inline]
unsafe fn thread_set_flag(p: *mut thread, bit: u64) {
    if p.is_null() {
        return;
    }
    u! {
        let _ = atomic_or_fetch_u64(&(*p).flags as *const u64 as *mut u64, 1u64 << bit);
    }
}

#[inline]
unsafe fn thread_state_load(p: *mut thread) -> c_int {
    if p.is_null() {
        return 0;
    }
    u! { smp_load_acquire_i32(&(*p).state as *const _ as *const c_int) }
}

// ===========================================================================
// SECTION 2 — remaining accessors (macros / atomics / lists).
// ===========================================================================

#[no_mangle]
pub extern "C" fn t_pg_entry_init(t: *mut thread) {
    list_init_field!(t, pg_entry)
}
#[no_mangle]
pub extern "C" fn t_pg_entry_detach(t: *mut thread) {
    list_detach_field!(t, pg_entry)
}
#[no_mangle]
pub extern "C" fn t_pg_entry_is_detached(t: *mut thread) -> c_int {
    list_is_detached_field!(t, pg_entry)
}

#[no_mangle]
pub extern "C" fn t_user_space(p: *mut thread) -> c_int {
    u! { thread_user_space(p) as c_int }
}

#[no_mangle]
pub extern "C" fn t_dmp_list_entry_is_detached(t: *mut thread) -> c_int {
    list_is_detached_field!(t, dmp_list_entry)
}

// Iterate `p->children` (safe traversal — caller-provided callback may delete
// the current child, so we snapshot `next` before invoking).
#[no_mangle]
pub extern "C" fn t_for_each_child(
    p: *mut thread,
    fn_cb: Option<unsafe extern "C" fn(*mut thread, *mut c_void)>,
    arg: *mut c_void,
) {
    let Some(cb) = fn_cb else { return };
    let off = core::mem::offset_of!(thread, siblings);
    u! {
        list_foreach_safe::<thread>(&raw mut (*p).children, off, |child| {
            cb(child, arg);
        })
    }
}

// ===========================================================================
// SECTION 3 — remaining pgroup accessors.
// ===========================================================================

#[no_mangle]
pub extern "C" fn pg_atomic_dec_t_cnt(pg: *mut pgroup) -> c_int {
    // C `atomic_dec` returns the new value.
    atomic_fetch_sub_i32_field!(pg, t_cnt, 1) - 1
}

#[no_mangle]
pub extern "C" fn pg_list_entry_init(pg: *mut pgroup) {
    list_init_field!(pg, list_entry)
}
#[no_mangle]
pub extern "C" fn pg_list_entry_detach(pg: *mut pgroup) {
    list_detach_field!(pg, list_entry)
}
#[no_mangle]
pub extern "C" fn pg_list_entry_is_detached(pg: *mut pgroup) -> c_int {
    list_is_detached_field!(pg, list_entry)
}

#[no_mangle]
pub extern "C" fn pg_threads_init(pg: *mut pgroup) {
    list_init_field!(pg, threads)
}
#[no_mangle]
pub extern "C" fn pg_tgs_init(pg: *mut pgroup) {
    list_init_field!(pg, thread_groups)
}

#[no_mangle]
pub extern "C" fn pg_threads_push_back(pg: *mut pgroup, t: *mut thread) {
    list_push_back_fields!(pg, threads, t, pg_entry)
}
#[no_mangle]
pub extern "C" fn pg_threads_detach(t: *mut thread) {
    list_detach_field!(t, pg_entry)
}

#[no_mangle]
pub extern "C" fn pg_tgs_push_back(pg: *mut pgroup, tg: *mut thread_group) {
    list_push_back_fields!(pg, thread_groups, tg, list_entry)
}
#[no_mangle]
pub extern "C" fn pg_tgs_detach(tg: *mut thread_group) {
    list_detach_field!(tg, list_entry)
}
#[no_mangle]
pub extern "C" fn pg_tg_list_entry_is_detached(tg: *mut thread_group) -> c_int {
    list_is_detached_field!(tg, list_entry)
}

#[no_mangle]
pub extern "C" fn pg_for_each_tg(
    pg: *mut pgroup,
    fn_cb: Option<unsafe extern "C" fn(*mut thread_group, *mut c_void)>,
    arg: *mut c_void,
) {
    let Some(cb) = fn_cb else { return };
    let off = core::mem::offset_of!(thread_group, list_entry);
    u! {
        list_foreach_safe::<thread_group>(&raw mut (*pg).thread_groups, off, |tg| {
            cb(tg, arg);
        })
    }
}

// ===========================================================================
// SECTION 4 — remaining thread_group accessors.
// ===========================================================================

#[no_mangle]
pub extern "C" fn tg_live_threads(tg: *mut thread_group) -> c_int {
    atomic_load_i32_field!(tg, live_threads)
}
#[no_mangle]
pub extern "C" fn tg_refcount(tg: *mut thread_group) -> c_int {
    atomic_load_i32_field!(tg, refcount)
}
#[no_mangle]
pub extern "C" fn tg_group_exit(tg: *mut thread_group) -> c_int {
    atomic_load_i32_field!(tg, group_exit)
}
#[no_mangle]
pub extern "C" fn tg_list_entry_init(tg: *mut thread_group) {
    list_init_field!(tg, list_entry)
}

#[no_mangle]
pub extern "C" fn tg_for_each_thread(
    tg: *mut thread_group,
    fn_cb: Option<unsafe extern "C" fn(*mut thread, *mut c_void)>,
    arg: *mut c_void,
) {
    let Some(cb) = fn_cb else { return };
    let off = core::mem::offset_of!(thread, tg_entry);
    u! {
        list_foreach_safe::<thread>(&raw mut (*tg).thread_list, off, |t| {
            cb(t, arg);
        })
    }
}

// ===========================================================================
// SECTION 5 — remaining session accessors.
// ===========================================================================

#[no_mangle]
pub extern "C" fn session_for_each_pg(
    s: *mut session,
    fn_cb: Option<unsafe extern "C" fn(*mut pgroup, *mut c_void)>,
    arg: *mut c_void,
) {
    let Some(cb) = fn_cb else { return };
    let off = core::mem::offset_of!(pgroup, list_entry);
    u! {
        list_foreach_safe::<pgroup>(&raw mut (*s).pgrps, off, |pg| {
            cb(pg, arg);
        })
    }
}

// ===========================================================================
// SECTION 11/12 helpers — minimal accessors common to clone/exit support.
// ===========================================================================

#[no_mangle]
pub extern "C" fn xv6_thread_is_zombie(t: *mut thread) -> c_int {
    u! { (thread_state_load(t) == THREAD_STATE_ZOMBIE) as c_int }
}
#[no_mangle]
pub extern "C" fn xv6_thread_is_stopped(t: *mut thread) -> c_int {
    u! { (thread_state_load(t) == THREAD_STATE_STOPPED) as c_int }
}
#[no_mangle]
pub extern "C" fn xv6_t_set_user_space(t: *mut thread) {
    u! { thread_set_flag(t, THREAD_FLAG_USER_SPACE) }
}
#[no_mangle]
pub extern "C" fn xv6_t_set_self_reap(t: *mut thread) {
    u! { thread_set_flag(t, THREAD_FLAG_SELF_REAP) }
}

// ===========================================================================
// SECTION 11 — clone.c field accessors. The C side uses void* for opaque
// kernel objects (vm_t, fs_struct, vfs_fdtable, sigacts_t, sched_entity);
// we mirror that here. trapframe / utrapframe are exposed by bindgen so we
// can do the full struct copy and sepc/sp/a0 writes directly.
// ===========================================================================

use crate::bindings::{thread_signal_t, utrapframe};

#[no_mangle]
pub extern "C" fn xv6_t_kstack_order(t: *mut thread) -> c_int {
    field_get!(t, kstack_order)
}
#[no_mangle]
pub extern "C" fn xv6_t_vm(t: *mut thread) -> *mut c_void {
    field_get!(t, vm) as *mut c_void
}
#[no_mangle]
pub extern "C" fn xv6_t_set_vm(t: *mut thread, v: *mut c_void) {
    field_set!(t, vm, v as *mut crate::bindings::vm_t)
}
#[no_mangle]
pub extern "C" fn xv6_t_fs(t: *mut thread) -> *mut c_void {
    field_get!(t, fs) as *mut c_void
}
#[no_mangle]
pub extern "C" fn xv6_t_set_fs(t: *mut thread, f: *mut c_void) {
    field_set!(t, fs, f as *mut crate::bindings::fs_struct)
}
#[no_mangle]
pub extern "C" fn xv6_t_fdtable(t: *mut thread) -> *mut c_void {
    field_get!(t, fdtable) as *mut c_void
}
#[no_mangle]
pub extern "C" fn xv6_t_set_fdtable(t: *mut thread, f: *mut c_void) {
    field_set!(t, fdtable, f as *mut crate::bindings::vfs_fdtable)
}
#[no_mangle]
pub extern "C" fn xv6_t_sigacts(t: *mut thread) -> *mut c_void {
    field_get!(t, sigacts) as *mut c_void
}
#[no_mangle]
pub extern "C" fn xv6_t_set_sigacts(t: *mut thread, s: *mut c_void) {
    field_set!(t, sigacts, s as *mut crate::bindings::sigacts_t)
}
#[no_mangle]
pub extern "C" fn xv6_t_signal_ptr(t: *mut thread) -> *mut c_void {
    field_ptr_mut!(t, signal) as *mut c_void
}
#[no_mangle]
pub extern "C" fn xv6_t_sched_entity(t: *mut thread) -> *mut c_void {
    field_get!(t, sched_entity) as *mut c_void
}
#[no_mangle]
pub extern "C" fn xv6_t_set_clone_flags(t: *mut thread, f: u64) {
    field_set!(t, clone_flags, f)
}
#[no_mangle]
pub extern "C" fn xv6_t_set_vfork_parent(t: *mut thread, p: *mut thread) {
    field_set!(t, vfork_parent, p)
}
#[no_mangle]
pub extern "C" fn xv6_t_vfork_parent(t: *mut thread) -> *mut thread {
    field_get!(t, vfork_parent)
}
#[no_mangle]
pub extern "C" fn xv6_t_set_tgid(t: *mut thread, tgid: c_int) {
    field_set!(t, tgid, tgid)
}
#[no_mangle]
pub extern "C" fn xv6_t_set_parent(t: *mut thread, p: *mut thread) {
    field_set!(t, parent, p)
}
#[no_mangle]
pub extern "C" fn xv6_t_set_thread_group(t: *mut thread, tg: *mut thread_group) {
    field_set!(t, thread_group, tg)
}

#[no_mangle]
pub extern "C" fn xv6_t_copy_trapframe(dst: *mut thread, src: *mut thread) {
    // C: *(dst->trapframe) = *(src->trapframe);  // copy utrapframe by value.
    u! { ptr::copy_nonoverlapping((*src).trapframe, (*dst).trapframe, 1usize) }
}
#[no_mangle]
pub extern "C" fn xv6_t_trapframe_set_sepc(t: *mut thread, v: u64) {
    u! { (*(*t).trapframe).trapframe.sepc = v }
}
#[no_mangle]
pub extern "C" fn xv6_t_trapframe_set_sp(t: *mut thread, v: u64) {
    u! { (*(*t).trapframe).trapframe.sp = v }
}
#[no_mangle]
pub extern "C" fn xv6_t_trapframe_set_a0(t: *mut thread, v: u64) {
    u! { (*(*t).trapframe).trapframe.a0 = v }
}

// Suppress unused warning for re-exported utrapframe type when nothing else
// in this module mentions it directly.
#[allow(dead_code)]
const _UTRAPFRAME_SIZE_PROBE: usize = core::mem::size_of::<utrapframe>();
#[allow(dead_code)]
const _SIGNAL_SIZE_PROBE: usize = core::mem::size_of::<thread_signal_t>();

// ===========================================================================
// SECTION 12 — exit.c field accessors.
// ===========================================================================

#[no_mangle]
pub extern "C" fn xv6_t_xstate(t: *mut thread) -> c_int {
    field_get!(t, xstate)
}
#[no_mangle]
pub extern "C" fn xv6_t_set_xstate(t: *mut thread, v: c_int) {
    field_set!(t, xstate, v)
}
#[no_mangle]
pub extern "C" fn xv6_t_clone_flags(t: *mut thread) -> u64 {
    field_get!(t, clone_flags)
}
#[no_mangle]
pub extern "C" fn xv6_t_signal_esignal(t: *mut thread) -> c_int {
    // C field is uint64; cast to int matches the C shim's return type.
    field_get!(t, signal.esignal) as c_int
}
#[no_mangle]
pub extern "C" fn xv6_t_set_signal_esignal(t: *mut thread, v: c_int) {
    field_set!(t, signal.esignal, v as u64)
}
#[no_mangle]
pub extern "C" fn xv6_t_signal_stop_signal(t: *mut thread) -> c_int {
    field_get!(t, signal.stop_signal)
}
#[no_mangle]
pub extern "C" fn xv6_t_children_count(t: *mut thread) -> c_int {
    field_get!(t, children_count)
}

#[no_mangle]
pub extern "C" fn xv6_tg_group_exit_task_is(
    tg: *mut thread_group,
    t: *mut thread,
) -> c_int {
    u! { ((*tg).group_exit_task == t) as c_int }
}
#[no_mangle]
pub extern "C" fn xv6_tg_group_exit_code(tg: *mut thread_group) -> c_int {
    field_get!(tg, group_exit_code)
}
#[no_mangle]
pub extern "C" fn xv6_tg_group_leader(tg: *mut thread_group) -> *mut thread {
    field_get!(tg, group_leader)
}

#[no_mangle]
pub extern "C" fn xv6_tg_is_exiting(tg: *mut thread_group) -> c_int {
    // C inline: __atomic_load_n(&tg->group_exit, ACQUIRE) != 0.
    if tg.is_null() {
        return 0;
    }
    u! { (smp_load_acquire_i32(&raw const (*tg).group_exit) != 0) as c_int }
}

// ===========================================================================
// SECTION 1 — misc helpers used by sched_idle.rs / sched_fifo.rs / signal.rs.
// (xv6_current_thread + xv6_thread_state_short/_to_str remain in
// proc_rust_shims.c because they call C-side `current` / `static inline`
// helpers; everything else moves here.)
// ===========================================================================

#[no_mangle]
pub extern "C" fn thread_sched_entity(
    t: *mut thread,
) -> *mut crate::bindings::sched_entity {
    field_get!(t, sched_entity)
}

#[no_mangle]
pub extern "C" fn xv6_thread_state_set(p: *mut thread, s: c_int) {
    // C inline: __atomic_store_n(&p->state, state, SEQ_CST); NULL-safe.
    if p.is_null() {
        return;
    }
    u! {
        AtomicI32::from_ptr(&raw mut (*p).state as *mut i32).store(s, Ordering::SeqCst);
    }
}

#[no_mangle]
pub extern "C" fn xv6_thread_state_get(p: *mut thread) -> c_int {
    // C inline: __atomic_load_n(&p->state, SEQ_CST); NULL → THREAD_UNUSED (0).
    if p.is_null() {
        return 0;
    }
    u! {
        AtomicI32::from_ptr(&raw mut (*p).state as *mut i32).load(Ordering::SeqCst)
    }
}

#[no_mangle]
pub extern "C" fn xv6_sizeof_sigaction() -> u64 {
    // _Static_assert in the C side enforced sizeof(struct sigaction) == 24.
    24
}

// IS_ERR / ERR_PTR / PTR_ERR — match the macros in kernel/inc/errno.h.
const MAX_ERRNO: u64 = 4095;

#[no_mangle]
pub extern "C" fn xv6_is_err(p: *const c_void) -> c_int {
    // C: (unsigned long)x >= (unsigned long)-MAX_ERRNO.
    // -MAX_ERRNO as u64 is a very large value just below 2^64; the comparison
    // is true exactly when (i64)p ∈ [-MAX_ERRNO, -1].
    let v = p as u64;
    let threshold = (-(MAX_ERRNO as i64)) as u64;
    (v >= threshold) as c_int
}

#[no_mangle]
pub extern "C" fn xv6_err_ptr(err: i64) -> *mut c_void {
    err as *mut c_void
}

#[no_mangle]
pub extern "C" fn xv6_ptr_err(p: *const c_void) -> i64 {
    p as i64
}

// ===========================================================================
// SECTION 4 leftover: tg_is_group_leader — inlined here so the
// xv6_tgport_thread_is_group_leader FFI hop disappears.
// ===========================================================================

#[no_mangle]
pub extern "C" fn tg_is_group_leader(p: *mut thread) -> c_int {
    u! {
        if p.is_null() {
            return 1;
        }
        let tg = (*p).thread_group;
        if tg.is_null() {
            return 1;
        }
        ((*tg).group_leader == p) as c_int
    }
}

// ===========================================================================
// SECTION 5 leftover: session_for_each_all — walks the global `session_list`
// declared in kernel/inc/tty/session.h.
// ===========================================================================

unsafe extern "C" {
    static mut session_list: list_node_t;
}

#[no_mangle]
pub extern "C" fn session_for_each_all(
    fn_cb: Option<unsafe extern "C" fn(*mut session, *mut c_void)>,
    arg: *mut c_void,
) {
    let Some(cb) = fn_cb else { return };
    let off = core::mem::offset_of!(session, global_entry);
    u! {
        list_foreach_safe::<session>(&raw mut session_list, off, |s| {
            cb(s, arg);
        })
    }
}

// ===========================================================================
// SECTION 11/12 trivial wrappers (cpu_relax, intr_on, smp_mb, either_copyout_int).
// ===========================================================================

#[no_mangle]
pub extern "C" fn xv6_cpu_relax() {
    crate::machine::cpu_relax();
}

#[no_mangle]
pub extern "C" fn xv6_smp_mb() {
    crate::machine::smp_mb();
}

#[no_mangle]
pub extern "C" fn xv6_intr_on() {
    crate::machine::intr_on();
}

#[no_mangle]
pub extern "C" fn xv6_mycpu_clear_noff() {
    let mut cpu = crate::machine::CpuLocal::current();
    cpu.set_noff(0);
}

// forkret_assert_user — die loudly if a kernel thread is about to return to
// user space. Mirrors the C `assert(THREAD_USER_SPACE(p), "...", p->pid)`.
unsafe extern "C" {
    fn printf(fmt: *const c_char, ...) -> c_int;
    fn trigger_panic() -> !;
}

#[no_mangle]
pub extern "C" fn xv6_forkret_assert_user(p: *mut thread) {
    u! {
        if !thread_user_space(p) {
            // Format matches the C assert() output (file/line elided).
            printf(
                c"ASSERTION_FAILURE: kernel thread %d tries to return to user space\n".as_ptr(),
                (*p).pid,
            );
            trigger_panic();
        }
    }
}

#[no_mangle]
pub extern "C" fn xv6_either_copyout_int(dst: u64, v: c_int) -> c_int {
    // C: either_copyout(1 /*user_dst*/, dst, &v, sizeof(v))
    let local = v;
    u! {
        either_copyout(
            1,
            dst,
            &local as *const c_int as *mut c_void,
            core::mem::size_of::<c_int>() as u64,
        )
    }
}

// ===========================================================================
// SECTION 11/12 logic helpers: thread_from_context, exit_find_zombie_child,
// exit_find_stopped_child.
// ===========================================================================

use crate::bindings::sched_entity;

#[no_mangle]
pub extern "C" fn xv6_thread_from_context(
    ctx: *mut c_void,
) -> *mut thread {
    // C: thread_from_context(ctx) = container_of(ctx, sched_entity, context)->thread
    let off = core::mem::offset_of!(sched_entity, context);
    let se = (ctx as *mut u8).wrapping_sub(off) as *mut sched_entity;
    field_get!(se, thread)
}

#[no_mangle]
pub extern "C" fn xv6_exit_find_zombie_child(
    p: *mut thread,
) -> *mut thread {
    let off_siblings = core::mem::offset_of!(thread, siblings);
    let head = field_ptr_mut!(p, children);
    let mut found: *mut thread = core::ptr::null_mut();
    u! {
        list_foreach_safe::<thread>(head, off_siblings, |child| {
            if !found.is_null() {
                return;
            }
            if thread_state_load(child) == THREAD_STATE_ZOMBIE {
                found = child;
            }
        });
    }
    found
}

#[no_mangle]
pub extern "C" fn xv6_exit_find_stopped_child(
    p: *mut thread,
) -> *mut thread {
    let off_siblings = core::mem::offset_of!(thread, siblings);
    let head = field_ptr_mut!(p, children);
    let mut found: *mut thread = core::ptr::null_mut();
    u! {
        list_foreach_safe::<thread>(head, off_siblings, |child| {
            if !found.is_null() {
                return;
            }
            if thread_state_load(child) == THREAD_STATE_STOPPED {
                found = child;
            }
        });
    }
    found
}

// ---------------------------------------------------------------------------
// xv6_exit_reparent_do — detach every child from `p` and re-attach to
// `initproc`. Returns 1 if any zombie was encountered, else 0.
// C body:
//     rcu_read_lock(); pid_wlock();
//     list_foreach_node_safe(&p->children, child, tmp, siblings) {
//         child->signal.esignal = SIGCHLD;
//         if (THREAD_ZOMBIE(child)) zombie_found = 1;
//         xv6_thport_detach_child(p, child);
//         xv6_thport_attach_child(initproc, child);
//     }
//     pid_wunlock(); rcu_read_unlock();
// ---------------------------------------------------------------------------

const SIGCHLD: u64 = 17;

unsafe extern "C" {
    fn xv6_thport_detach_child(parent: *mut thread, child: *mut thread);
    fn xv6_thport_attach_child(parent: *mut thread, child: *mut thread);
    fn xv6_schport_scheduler_yield();
    fn xv6_thport_thread_destroy(p: *mut thread);
    fn proctab_proc_remove(p: *mut thread);
    fn __free_pid();
}

#[no_mangle]
pub extern "C" fn xv6_exit_reparent_do(
    p: *mut thread,
    initproc: *mut thread,
) -> c_int {
    let mut zombie_found: c_int = 0;
    let off_siblings = core::mem::offset_of!(thread, siblings);
    u! {
        let _rcu = crate::lock::rcu::KRcuRead::new();
        xv6_pid_wlock();
        let head = &raw mut (*p).children;
        list_foreach_safe::<thread>(head, off_siblings, |child| {
            (*child).signal.esignal = SIGCHLD;
            if thread_state_load(child) == THREAD_STATE_ZOMBIE {
                zombie_found = 1;
            }
            xv6_thport_detach_child(p, child);
            xv6_thport_attach_child(initproc, child);
        });
        xv6_pid_wunlock();
    }
    zombie_found
}

// ---------------------------------------------------------------------------
// xv6_exit_reap_zombie — caller holds pid_rlock and parent state is
// INTERRUPTIBLE. Spin-waits for child to leave CPU, demotes parent state,
// drops/re-acquires the pid lock as a writer, removes child from siblings,
// frees the pid slot, and destroys the thread. Returns child's pid; writes
// encoded status to *xstate_out.
// ---------------------------------------------------------------------------

const THREAD_STATE_RUNNING: c_int = 8;
const THREAD_STATE_INTERRUPTIBLE: c_int = 2;

#[no_mangle]
pub extern "C" fn xv6_exit_reap_zombie(
    parent: *mut thread,
    child: *mut thread,
    xstate_out: *mut c_int,
) -> c_int {
    u! {
        let mut spin_count: i32 = 0;
        loop {
            let se = (*child).sched_entity;
            let on_cpu = smp_load_acquire_i32(&raw const (*se).on_cpu);
            if on_cpu == 0 {
                break;
            }
            xv6_cpu_relax();
            spin_count += 1;
            if spin_count > 1000 {
                xv6_thread_state_set(parent, THREAD_STATE_RUNNING);
                xv6_pid_runlock();
                xv6_schport_scheduler_yield();
                xv6_pid_rlock();
                xv6_thread_state_set(parent, THREAD_STATE_INTERRUPTIBLE);
                spin_count = 0;
            }
        }
        xv6_thread_state_set(parent, THREAD_STATE_RUNNING);
        *xstate_out = ((*child).xstate & 0xff) << 8;
        let pid = (*child).pid;

        if xv6_pid_try_lock_upgrade() == 0 {
            xv6_pid_runlock();
            xv6_pid_wlock();
        }
        xv6_thport_detach_child(parent, child);
        proctab_proc_remove(child);
        xv6_pid_wunlock();
        __free_pid();
        xv6_thport_thread_destroy(child);
        pid
    }
}

// ===========================================================================
// SECTION 8 — proc_table storage + accessors (PORTED from proc_rust_shims.c).
//
// Encapsulates the kernel's per-PID hash table, the protective rwlock, and
// trivial counters. All callers reach this state through the public
// `xv6_*` entry points; no other code touches PROC_TABLE.
//
// Kernel primitives consumed:
//   * `hlist_init/get/get_rcu/put_rcu/pop_rcu`     — extern C (kernel/hlist.c)
//   * `rwlock_init/wlock/wunlock/rlock/runlock`    — extern Rust (kernel/lock/rwlock.rs)
//   * `rwlock_try_update`                          — extern Rust
//   * `__rwl_w_holding`                            — extern C (rwlock_shim.c)
//   * `hlist_hash_int`, `rcu_assign_pointer`,
//     `rcu_dereference`, `list_entry_add_tail_rcu`,
//     `list_entry_del_init_rcu`, `atomic_inc_unless`,
//     `atomic_sub` — were C macros / static inlines, now reimplemented in
//     Rust below as `priv` helpers.
// ===========================================================================

use crate::bindings::{
    hlist_bucket_t, hlist_entry_t, hlist_func_struct, hlist_func_t, hlist_t, ht_hash_t, rwlock,
    __IncompleteArrayField,
};
use core::sync::atomic::{AtomicI64, AtomicPtr};

// Mirror of `#define NR_THREAD_HASH_BUCKETS 31` in proc/proc_private.h.
// Keep in sync if the C header changes.
const NR_THREAD_HASH_BUCKETS: usize = 31;
// Mirror of `#define NR_THREAD 10000` in param.h. Used as the cap for
// atomic_inc_unless on allocated_cnt (`atomic_inc_unless(...,NR_THREAD)`).
const NR_THREAD: i64 = 10000;

unsafe extern "C" {
    fn hlist_init(h: *mut hlist_t, bucket_cnt: u64, func: *mut hlist_func_t) -> c_int;
    fn hlist_get(h: *mut hlist_t, node: *mut c_void) -> *mut c_void;
    fn hlist_get_rcu(h: *mut hlist_t, node: *mut c_void) -> *mut c_void;
    fn hlist_put_rcu(h: *mut hlist_t, node: *mut c_void, replace: bool) -> *mut c_void;
    fn hlist_pop_rcu(h: *mut hlist_t, node: *mut c_void) -> *mut c_void;

    fn rwlock_init(rw: *mut rwlock, name: *const c_char);
    fn rwlock_wlock(rw: *mut rwlock);
    fn rwlock_wunlock(rw: *mut rwlock);
    fn rwlock_rlock(rw: *mut rwlock);
    fn rwlock_runlock(rw: *mut rwlock);
    fn __rwl_try_update(rw: *mut rwlock) -> bool;
    fn __rwl_w_holding(rw: *mut rwlock) -> bool;
}

// --- Rust replacements for the C macros / static inlines -------------------

/// `hlist_hash_int(key)` from kernel/inc/hlist.h.
#[inline]
fn hlist_hash_int(key: c_int) -> ht_hash_t {
    const GOLDEN_RATIO_PRIME: u64 = 0x9e37fffffffc0001u64;
    let mult: u64 = 0x100000001u64.wrapping_sub(GOLDEN_RATIO_PRIME);
    let mut ret = (key as i64 as u64).wrapping_mul(mult);
    if ret == 0 {
        ret = GOLDEN_RATIO_PRIME;
    }
    ret
}

/// `atomic_inc_unless(value, unless)` from kernel/inc/smp/atomic.h.
/// Returns `true` if the increment succeeded, `false` if `*value == unless`.
#[inline]
unsafe fn atomic_inc_unless_i64(p: *mut i64, unless: i64) -> bool {
    let a = u! { AtomicI64::from_ptr(p) };
    let mut cur = a.load(core::sync::atomic::Ordering::SeqCst);
    loop {
        if cur == unless {
            return false;
        }
        match a.compare_exchange_weak(
            cur,
            cur + 1,
            core::sync::atomic::Ordering::SeqCst,
            core::sync::atomic::Ordering::SeqCst,
        ) {
            Ok(_) => return true,
            Err(v) => cur = v,
        }
    }
}

/// `atomic_sub(value, amount)` — `__atomic_fetch_sub(SEQ_CST)`.
#[inline]
unsafe fn atomic_sub_i64(p: *mut i64, n: i64) -> i64 {
    u! { AtomicI64::from_ptr(p).fetch_sub(n, core::sync::atomic::Ordering::SeqCst) }
}

/// `rcu_assign_pointer(p, v)` — release-store the pointer.
#[inline]
unsafe fn rcu_assign_pointer<T>(p: *mut *mut T, v: *mut T) {
    u! {
        AtomicPtr::<T>::from_ptr(p).store(v, core::sync::atomic::Ordering::Release);
    }
}

/// `rcu_dereference(p)` — acquire-load the pointer. Caller must be inside a
/// matching rcu_read_lock()/_unlock() region.
#[inline]
unsafe fn rcu_dereference<T>(p: *mut *mut T) -> *mut T {
    u! { AtomicPtr::<T>::from_ptr(p).load(core::sync::atomic::Ordering::Acquire) }
}

// --- proc_table storage ----------------------------------------------------

#[repr(C)]
struct ProcTable {
    // hlist + flexible-array buckets (mirrors the C-side `struct { hlist_t
    // procs; hlist_bucket_t buckets[NR_THREAD_HASH_BUCKETS]; }`). The
    // `__IncompleteArrayField` is zero-sized, so the layout is the same.
    procs: hlist_t,
    buckets: [hlist_bucket_t; NR_THREAD_HASH_BUCKETS],
    registered_cnt: i64,
    allocated_cnt: i64,
    procs_list: list_node_t,
    initproc: *mut thread,
    nextpid: c_int,
    pid_lock: rwlock,
}

/// Thin `Sync` newtype so we can keep `PROC_TABLE` at file scope. All
/// access is serialised by the inner `pid_lock` (writes) or RCU (reads).
struct ProcTableCell(UnsafeCell<ProcTable>);
unsafe impl Sync for ProcTableCell {}

const NULL_LIST_NODE: list_node_t = list_node_t {
    prev: ptr::null_mut(),
    next: ptr::null_mut(),
};

static PROC_TABLE: ProcTableCell = ProcTableCell(UnsafeCell::new(ProcTable {
    procs: hlist_t {
        bucket_cnt: 0,
        elem_cnt: 0,
        func: hlist_func_struct {
            hash: None,
            get_node: None,
            get_entry: None,
            cmp_node: None,
        },
        buckets: __IncompleteArrayField::new(),
    },
    buckets: [NULL_LIST_NODE; NR_THREAD_HASH_BUCKETS],
    registered_cnt: 0,
    allocated_cnt: 0,
    procs_list: NULL_LIST_NODE,
    initproc: ptr::null_mut(),
    nextpid: 0,
    pid_lock: rwlock {
        state: 0,
        w_holder: 0,
        name: ptr::null(),
    },
}));

#[inline]
fn pt() -> *mut ProcTable {
    PROC_TABLE.0.get()
}

// --- Hash function callbacks (extern C, registered with hlist_init) --------

unsafe extern "C" fn proctab_hash(node: *mut c_void) -> ht_hash_t {
    let p = node as *mut thread;
    hlist_hash_int(field_get!(p, pid))
}

unsafe extern "C" fn proctab_hash_cmp(
    _h: *mut hlist_t,
    a: *mut c_void,
    b: *mut c_void,
) -> c_int {
    let pa = a as *mut thread;
    let pb = b as *mut thread;
    u! { (*pa).pid - (*pb).pid }
}

unsafe extern "C" fn proctab_hash_get_entry(node: *mut c_void) -> *mut hlist_entry_t {
    let p = node as *mut thread;
    field_ptr_mut!(p, proctab_entry)
}

unsafe extern "C" fn proctab_hash_get_node(entry: *mut hlist_entry_t) -> *mut c_void {
    let off = core::mem::offset_of!(thread, proctab_entry);
    (entry as *mut u8).wrapping_sub(off) as *mut c_void
}

// --- Public xv6_* entry points (preserve original C ABI symbol names) ------

#[no_mangle]
pub extern "C" fn xv6_proctab_init_storage() {
    u! {
        let t = &mut *pt();
        let mut funcs = hlist_func_struct {
            hash: Some(proctab_hash),
            get_node: Some(proctab_hash_get_node),
            get_entry: Some(proctab_hash_get_entry),
            cmp_node: Some(proctab_hash_cmp),
        };
        hlist_init(
            &raw mut t.procs,
            NR_THREAD_HASH_BUCKETS as u64,
            &raw mut funcs,
        );
        rwlock_init(&raw mut t.pid_lock, c"pid_lock".as_ptr());
        list_init(&raw mut t.procs_list);
        t.initproc = ptr::null_mut();
        t.nextpid = 1;
    }
}

#[no_mangle]
pub extern "C" fn xv6_pid_wlock() {
    u! { rwlock_wlock(&raw mut (*pt()).pid_lock) }
}
#[no_mangle]
pub extern "C" fn xv6_pid_wunlock() {
    u! { rwlock_wunlock(&raw mut (*pt()).pid_lock) }
}
#[no_mangle]
pub extern "C" fn xv6_pid_rlock() {
    u! { rwlock_rlock(&raw mut (*pt()).pid_lock) }
}
#[no_mangle]
pub extern "C" fn xv6_pid_runlock() {
    u! { rwlock_runlock(&raw mut (*pt()).pid_lock) }
}
#[no_mangle]
pub extern "C" fn xv6_pid_try_lock_upgrade() -> c_int {
    if u! { __rwl_try_update(&raw mut (*pt()).pid_lock) } {
        1
    } else {
        0
    }
}
#[no_mangle]
pub extern "C" fn xv6_pid_wholding() -> c_int {
    if u! { __rwl_w_holding(&raw mut (*pt()).pid_lock) } {
        1
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn xv6_proctab_initproc_load() -> *mut thread {
    u! { rcu_dereference(&raw mut (*pt()).initproc) }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_initproc_store(p: *mut thread) {
    u! { rcu_assign_pointer(&raw mut (*pt()).initproc, p) }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_initproc_raw() -> *mut thread {
    u! { (*pt()).initproc }
}

#[no_mangle]
pub extern "C" fn xv6_proctab_nextpid_get() -> c_int {
    u! { (*pt()).nextpid }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_nextpid_set(v: c_int) {
    u! {
        (*pt()).nextpid = v;
    }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_registered_cnt() -> i64 {
    u! { (*pt()).registered_cnt }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_registered_inc() {
    u! {
        (*pt()).registered_cnt += 1;
    }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_registered_dec() {
    u! {
        (*pt()).registered_cnt -= 1;
    }
}

const EAGAIN: c_int = 11;

#[no_mangle]
pub extern "C" fn xv6_proctab_alloc_pid_slot() -> c_int {
    if u! { atomic_inc_unless_i64(&raw mut (*pt()).allocated_cnt, NR_THREAD) } {
        0
    } else {
        -EAGAIN
    }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_free_pid_slot() {
    u! {
        let _ = atomic_sub_i64(&raw mut (*pt()).allocated_cnt, 1);
    }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_allocated_cnt() -> i64 {
    u! { (*pt()).allocated_cnt }
}

#[no_mangle]
pub extern "C" fn xv6_proctab_get_locked(pid: c_int) -> *mut thread {
    // Pass a `struct thread` lookup key. Bindgen-generated `thread` is `Copy`
    // and large; using `MaybeUninit::zeroed()` keeps the discriminator fields
    // (only `pid`) deterministic and matches the C side's stack-allocated
    // `struct thread dummy = { .pid = pid };`.
    u! {
        let mut dummy: MaybeUninit<thread> = MaybeUninit::zeroed();
        (*dummy.as_mut_ptr()).pid = pid;
        hlist_get(&raw mut (*pt()).procs, dummy.as_mut_ptr() as *mut c_void) as *mut thread
    }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_get_rcu(pid: c_int) -> *mut thread {
    u! {
        let mut dummy: MaybeUninit<thread> = MaybeUninit::zeroed();
        (*dummy.as_mut_ptr()).pid = pid;
        hlist_get_rcu(&raw mut (*pt()).procs, dummy.as_mut_ptr() as *mut c_void) as *mut thread
    }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_put_rcu(p: *mut thread) -> *mut thread {
    u! { hlist_put_rcu(&raw mut (*pt()).procs, p as *mut c_void, false) as *mut thread }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_pop_rcu(p: *mut thread) -> *mut thread {
    u! { hlist_pop_rcu(&raw mut (*pt()).procs, p as *mut c_void) as *mut thread }
}

// --- procs_list (RCU-safe doubly linked list for procdump iteration) ------

/// `__list_entry_add_rcu(new, prev, next)` — internal helper from list.h.
#[inline]
unsafe fn __list_entry_add_rcu(
    new: *mut list_node_t,
    prev: *mut list_node_t,
    next: *mut list_node_t,
) {
    u! {
        (*new).next = next;
        (*new).prev = prev;
        rcu_assign_pointer(&raw mut (*prev).next, new);
        (*next).prev = new;
    }
}

/// `list_entry_add_tail_rcu(head, entry)` — insert before `head`.
#[inline]
unsafe fn list_entry_add_tail_rcu(head: *mut list_node_t, entry: *mut list_node_t) {
    u! { __list_entry_add_rcu(entry, (*head).prev, head) }
}

/// `list_entry_del_rcu(entry)` — RCU-safe unlink, leaves entry->next
/// untouched so concurrent readers can still traverse it.
#[inline]
unsafe fn list_entry_del_rcu(entry: *mut list_node_t) {
    u! {
        let prev = (*entry).prev;
        let next = (*entry).next;
        // WRITE_ONCE(prev->next, next)
        core::ptr::write_volatile(&raw mut (*prev).next, next);
        (*next).prev = prev;
    }
}

/// `list_entry_init_rcu(entry)` — reset to self-pointing with release publish.
#[inline]
unsafe fn list_entry_init_rcu(entry: *mut list_node_t) {
    u! {
        rcu_assign_pointer(&raw mut (*entry).next, entry);
        (*entry).prev = entry;
    }
}

/// `list_entry_del_init_rcu(entry)` — unlink + reinit.
#[inline]
unsafe fn list_entry_del_init_rcu(entry: *mut list_node_t) {
    u! {
        list_entry_del_rcu(entry);
        list_entry_init_rcu(entry);
    }
}

#[no_mangle]
pub extern "C" fn xv6_proctab_dmplist_add(p: *mut thread) {
    u! { list_entry_add_tail_rcu(&raw mut (*pt()).procs_list, &raw mut (*p).dmp_list_entry) }
}
#[no_mangle]
pub extern "C" fn xv6_proctab_dmplist_del(p: *mut thread) {
    u! { list_entry_del_init_rcu(&raw mut (*p).dmp_list_entry) }
}

// --- proc_table foreach helpers -------------------------------------------
//
// `hlist_foreach_node_rcu(hlist, pos, member)` (a macro that internally
// calls `hlist_first_entry_rcu` + `hlist_next_entry_rcu`, both static
// inline) is not bindgen-visible. The C accumulator's foreach helpers are
// only used by procdump (called from human-driven console diagnostics), so
// for now they are forwarded to the C-side trampolines that still live in
// the original file. Once SECTION 10 (procdump) is folded into Rust we'll
// either reimplement the iteration in pure Rust by walking the hlist
// buckets directly, or expose `hlist_first_entry_rcu` via a tiny shim.
//
// NOTE: The two functions below — `xv6_proctab_foreach_rcu` and
// `xv6_proctab_foreach_inner` — remain in proc_rust_shims.c and are
// declared `extern` here for SECTION 10 to use. Do not delete them from C
// until the static-inline iteration helpers are also ported.
unsafe extern "C" {
    pub fn xv6_proctab_foreach_rcu(
        cb: Option<unsafe extern "C" fn(*mut thread, *mut c_void)>,
        arg: *mut c_void,
    );
    pub fn xv6_proctab_foreach_inner(
        cb: Option<unsafe extern "C" fn(*mut thread, *mut c_void)>,
        arg: *mut c_void,
    );
}

/// Accessor exposed to the two C-side foreach helpers (which still rely on
/// the `hlist_foreach_node_rcu` static-inline macro). Returns a raw pointer
/// to `PROC_TABLE.procs`. The Rust-side `ProcTable` deliberately lays out
/// `procs: hlist_t` followed immediately by `buckets: [hlist_bucket_t; 31]`
/// so that the C macro's flexible-array bucket access lands on the correct
/// memory.
#[no_mangle]
pub extern "C" fn xv6_proctab_hlist_ptr() -> *mut hlist_t {
    u! { &raw mut (*pt()).procs }
}

// ===========================================================================
// SECTION 11 leftover: xv6_t_copy_name (was C — safestrcpy of thread.name)
// ===========================================================================

/// Copy `src->name` into `dst->name` with the same bounds-check semantics
/// as the C `safestrcpy`: copies at most `len-1` non-NUL bytes, then writes
/// a terminating NUL. `len` is the fixed size of `thread.name` (16).
#[no_mangle]
pub extern "C" fn xv6_t_copy_name(dst: *mut thread, src: *mut thread) {
    let dn = field_ptr_mut!(dst, name) as *mut u8;
    let sn = field_ptr_const!(src, name) as *const u8;
    let n = 16usize;
    let mut i = 0usize;
    while i + 1 < n {
        let c = u! { *sn.add(i) };
        if c == 0 { break; }
        u! { *dn.add(i) = c; }
        i += 1;
    }
    u! { *dn.add(i) = 0; }
}

// ===========================================================================
// SECTION 10: procdump print helpers — PORTED from proc_rust_shims.c.
// Uses the kernel `printf` (variadic, bindgen-exposed). Recursion in
// xv6_procdump_tree_recursive is straightforward Rust recursion.
// ===========================================================================

use crate::bindings::{pgroup as Pgroup, session as Session, thread_group as Tgroup};

unsafe extern "C" {
    // C-side helpers we reuse here. (xv6_thport_tcb_lock/unlock are
    // already declared earlier in this file; do not redeclare.)
    pub fn xv6_tgport_thread_is_group_leader(p: *mut thread) -> c_int;
    pub fn print_thread_backtrace(ctx: *mut crate::bindings::context, kstack: u64, kstack_order: c_int);
}

// Variadic printf alias for ergonomics (bindgen already exports it; we
// re-shape the first arg as a *const c_char for use with `c"..."` literals).
unsafe extern "C" {
    #[link_name = "printf"]
    fn k_printf(fmt: *const c_char, ...) -> c_int;
}

#[inline]
unsafe fn t_name_ptr(p: *mut thread) -> *const c_char {
    u! { &raw const (*p).name as *const c_char }
}
#[inline]
unsafe fn s10_t_user_space(p: *mut thread) -> bool {
    // THREAD_FLAG_USER_SPACE = 5
    u! { ((*p).flags & (1u64 << 5)) != 0 }
}
#[inline]
unsafe fn se_on_cpu(p: *mut thread) -> bool {
    use core::sync::atomic::{AtomicI32, Ordering};
    u! {
        let pse = (*p).sched_entity;
        AtomicI32::from_ptr(&raw mut (*pse).on_cpu).load(Ordering::Acquire) != 0
    }
}
#[inline]
unsafe fn se_cpu_id(p: *mut thread) -> c_int {
    u! { (*(*p).sched_entity).cpu_id }
}
#[inline]
unsafe fn t_state_load(p: *mut thread) -> c_int {
    // Mirror __thread_state_get: smp_load_acquire on thread.state.
    use core::sync::atomic::{AtomicU32, Ordering};
    u! {
        AtomicU32::from_ptr(&raw mut (*p).state as *mut u32).load(Ordering::Acquire) as c_int
    }
}
#[inline]
unsafe fn tg_load_int(p: *mut Tgroup, off: usize) -> c_int {
    use core::sync::atomic::{AtomicI32, Ordering};
    u! {
        let base = p as *mut u8;
        AtomicI32::from_ptr(base.add(off) as *mut i32).load(Ordering::Acquire)
    }
}

// safestrcpy reimplementation, returns count written excluding NUL.
#[inline]
unsafe fn safestr(dst: &mut [u8], src: *const c_char) -> usize {
    let n = dst.len();
    if n == 0 { return 0; }
    let mut i = 0usize;
    while i + 1 < n {
        let c = u! { *(src.add(i) as *const u8) };
        if c == 0 { break; }
        dst[i] = c;
        i += 1;
    }
    dst[i] = 0;
    i
}

#[no_mangle]
pub extern "C" fn xv6_procdump_header() {
    u! {
        k_printf(
            c"%-20s %-5s %-2s %-3s %s\n".as_ptr(),
            c"SID:PGID:TGID:TID".as_ptr(),
            c"CPU".as_ptr(),
            c"ST".as_ptr(),
            c"U/K".as_ptr(),
            c"COMMAND".as_ptr(),
        );
    }
}

#[no_mangle]
pub extern "C" fn xv6_procdump_bt_header() {
    u! { k_printf(c"\n=== Blocked Process Backtraces ===\n".as_ptr()); }
}
#[no_mangle]
pub extern "C" fn xv6_procdump_bt_footer() {
    u! { k_printf(c"\n=== End Backtraces ===\n".as_ptr()); }
}

#[no_mangle]
pub extern "C" fn xv6_procdump_one(p: *mut thread) -> c_int {
    u! {
        let mut name = [0u8; 16];
        let mut pname = [0u8; 16];

        xv6_thport_tcb_lock(p);
        let pstate = t_state_load(p);
        let tid = (*p).pid;
        let tgid = (*p).tgid;
        let pgid_v = (*p).pgid;
        let sid_v = (*p).sid;
        safestr(&mut name, t_name_ptr(p));
        if !(*p).parent.is_null() {
            safestr(&mut pname, t_name_ptr((*p).parent));
        } else {
            pname[..4].copy_from_slice(b"N/A\0");
        }
        xv6_thport_tcb_unlock(p);

        // THREAD_UNUSED = 0
        if pstate == 0 { return 0; }

        // Print "sid:pgid:tgid:tid" directly using printf.
        // Use a fixed-width composite format.
        let cpu_on = se_on_cpu(p);
        let cpu = se_cpu_id(p);
        // Build cpu prefix string into stack buffer.
        let mut cpubuf = [0u8; 8];
        let mut ci = 0usize;
        if cpu_on { cpubuf[ci] = b'*'; ci += 1; }
        if cpu >= 10 { cpubuf[ci] = b'0' + (cpu as u8 / 10); ci += 1; }
        cpubuf[ci] = b'0' + (cpu as u8 % 10);
        cpubuf[ci + 1] = 0;

        // Build id string "sid:pgid:tgid:tid" with a helper.
        // Easiest: use printf with %d:%d:%d:%d into a temp via two-pass —
        // but kernel printf has no sprintf. Print id columns directly.
        // We mirror the C version's "%-20s" by left-padding ourselves.
        let mut idbuf = [0u8; 40];
        let pos = fmt_id(&mut idbuf, sid_v, pgid_v, tgid, tid);
        idbuf[pos] = 0;

        let ustr = if s10_t_user_space(p) { c"U".as_ptr() } else { c"K".as_ptr() };
        k_printf(
            c"%-20s %-5s %-2s [%s] %s/%s\n".as_ptr(),
            idbuf.as_ptr() as *const c_char,
            cpubuf.as_ptr() as *const c_char,
            xv6_thread_state_short(pstate),
            ustr,
            pname.as_ptr() as *const c_char,
            name.as_ptr() as *const c_char,
        );
        1
    }
}

/// Write "a:b:c:d" into buf and return length written (no trailing NUL).
fn fmt_id(buf: &mut [u8], a: c_int, b: c_int, c: c_int, d: c_int) -> usize {
    let mut pos = 0usize;
    let vals = [a, b, c, d];
    for (k, &v) in vals.iter().enumerate() {
        if k > 0 && pos < buf.len() - 1 { buf[pos] = b':'; pos += 1; }
        let mut uv: u32;
        if v < 0 {
            if pos < buf.len() - 1 { buf[pos] = b'-'; pos += 1; }
            uv = (-(v as i64)) as u32;
        } else {
            uv = v as u32;
        }
        let mut tmp = [0u8; 12];
        let mut ti = 0usize;
        loop {
            tmp[ti] = b'0' + (uv % 10) as u8;
            ti += 1;
            uv /= 10;
            if uv == 0 { break; }
        }
        while ti > 0 && pos < buf.len() - 1 {
            ti -= 1;
            buf[pos] = tmp[ti];
            pos += 1;
        }
    }
    pos
}

#[no_mangle]
pub extern "C" fn xv6_procdump_bt_one(p: *mut thread) {
    u! {
        let mut name = [0u8; 16];
        xv6_thport_tcb_lock(p);
        let pstate = t_state_load(p);
        let pid = (*p).pid;
        let tgid = (*p).tgid;
        let pgid_v = (*p).pgid;
        let sid_v = (*p).sid;
        safestr(&mut name, t_name_ptr(p));

        // INTERRUPTIBLE=2, UNINTERRUPTIBLE=6
        if pstate == 2 || pstate == 6 {
            let stype = if pstate == 2 { c"interruptible".as_ptr() } else { c"uninterruptible".as_ptr() };
            if se_on_cpu(p) {
                k_printf(
                    c"\n--- %d:%d:%d:%d [%s] %s --- (on CPU, cannot backtrace)\n".as_ptr(),
                    sid_v, pgid_v, tgid, pid, stype, name.as_ptr() as *const c_char,
                );
            } else {
                k_printf(
                    c"\n--- %d:%d:%d:%d [%s] %s ---\n".as_ptr(),
                    sid_v, pgid_v, tgid, pid, stype, name.as_ptr() as *const c_char,
                );
                let pse = (*p).sched_entity;
                print_thread_backtrace(&raw mut (*pse).context, (*p).kstack, (*p).kstack_order);
            }
        }
        xv6_thport_tcb_unlock(p);
    }
}

#[no_mangle]
pub extern "C" fn xv6_procdump_bt_pid(pid: c_int) {
    use core::mem::MaybeUninit;
    u! {
        let _rcu = crate::lock::rcu::KRcuRead::new();
        let mut dummy: MaybeUninit<thread> = MaybeUninit::zeroed();
        (*dummy.as_mut_ptr()).pid = pid;
        let p = hlist_get_rcu(&raw mut (*pt()).procs, dummy.as_mut_ptr() as *mut c_void) as *mut thread;
        if p.is_null() {
            k_printf(c"Process %d not found\n".as_ptr(), pid);
            return;
        }
        xv6_thport_tcb_lock(p);
        let pstate = t_state_load(p);
        let mut name = [0u8; 16];
        safestr(&mut name, t_name_ptr(p));
        let tgid = (*p).tgid;
        let pgid_v = (*p).pgid;
        let sid_v = (*p).sid;
        k_printf(
            c"\n--- %d:%d:%d:%d [%s] %s ---\n".as_ptr(),
            sid_v, pgid_v, tgid, pid,
            xv6_thread_state_short(pstate),
            name.as_ptr() as *const c_char,
        );
        if se_on_cpu(p) {
            k_printf(c"Process is currently on a CPU, context not saved\n".as_ptr());
        } else if pstate == 0 {
            // THREAD_UNUSED
            k_printf(c"Process is %s, no valid context\n".as_ptr(),
                     xv6_thread_state_to_str(pstate));
        } else {
            let pse = (*p).sched_entity;
            print_thread_backtrace(&raw mut (*pse).context, (*p).kstack, (*p).kstack_order);
        }
        xv6_thport_tcb_unlock(p);
    }
}

#[no_mangle]
pub extern "C" fn xv6_procdump_tree_node(p: *mut thread, depth: c_int) {
    u! {
        let mut i = 0;
        while i < depth { k_printf(c"  ".as_ptr()); i += 1; }
        if depth > 0 { k_printf(c"\xe2\x94\x94\xe2\x94\x80 ".as_ptr()); /* └─ */ }
        let pstate = t_state_load(p);
        let pid = (*p).pid;
        let tgid = (*p).tgid;
        let pgid_v = (*p).pgid;
        let sid_v = (*p).sid;
        let mut name = [0u8; 16];
        safestr(&mut name, t_name_ptr(p));
        let ustr = if s10_t_user_space(p) { c"U".as_ptr() } else { c"K".as_ptr() };
        k_printf(
            c"%d:%d:%d:%d %s [%s] %s".as_ptr(),
            sid_v, pgid_v, tgid, pid,
            xv6_thread_state_short(pstate),
            ustr,
            name.as_ptr() as *const c_char,
        );
        if se_on_cpu(p) {
            k_printf(c" (CPU: %d)\n".as_ptr(), se_cpu_id(p));
        } else {
            k_printf(c"\n".as_ptr());
        }
    }
}

#[no_mangle]
pub extern "C" fn xv6_procdump_tree_recursive(p: *mut thread, depth: c_int) {
    u! {
        xv6_procdump_tree_node(p, depth);
        // Walk children list (member: siblings).
        let sib_off = core::mem::offset_of!(thread, siblings);
        list_foreach_safe::<thread>(&raw mut (*p).children, sib_off, |child| {
            xv6_procdump_tree_recursive(child, depth + 1);
        });
    }
}

#[no_mangle]
pub extern "C" fn xv6_dump_session(s: *mut Session) {
    u! {
        let fg_note = if (*s).fg_pgrp.is_null() { c", no fg".as_ptr() } else { c"".as_ptr() };
        k_printf(
            c"\nSession %d  (threads=%d, pgroups=%d%s)\n".as_ptr(),
            (*s).sid, (*s).t_cnt, (*s).pg_cnt, fg_note,
        );

        let pg_off = core::mem::offset_of!(Pgroup, list_entry);
        list_foreach_safe::<Pgroup>(&raw mut (*s).pgrps, pg_off, |pg| {
            let fg = if (*s).fg_pgrp == pg { c" [fg]".as_ptr() } else { c"".as_ptr() };
            let exited = if (*pg).__bindgen_anon_1.exited() != 0 { c", exited".as_ptr() } else { c"".as_ptr() };
            k_printf(
                c"  PGroup %d%s  (threads=%d, tgroups=%d%s)\n".as_ptr(),
                (*pg).pgid, fg, (*pg).t_cnt, (*pg).p_cnt, exited,
            );

            let tg_off = core::mem::offset_of!(Tgroup, list_entry);
            list_foreach_safe::<Tgroup>(&raw mut (*pg).thread_groups, tg_off, |tg| {
                let live = tg_load_int(tg, core::mem::offset_of!(Tgroup, live_threads));
                let refc = tg_load_int(tg, core::mem::offset_of!(Tgroup, refcount));
                let gex  = tg_load_int(tg, core::mem::offset_of!(Tgroup, group_exit));
                let gex_note = if gex != 0 { c", exiting".as_ptr() } else { c"".as_ptr() };
                k_printf(
                    c"    Process %d  (live=%d, refs=%d%s)\n".as_ptr(),
                    (*tg).tgid, live, refc, gex_note,
                );

                let tge_off = core::mem::offset_of!(thread, tg_entry);
                list_foreach_safe::<thread>(&raw mut (*tg).thread_list, tge_off, |t| {
                    let st = xv6_thread_state_to_str(t_state_load(t));
                    let on_cpu = se_on_cpu(t);
                    let ustr = if s10_t_user_space(t) { c"U".as_ptr() } else { c"K".as_ptr() };
                    let leader = if xv6_tgport_thread_is_group_leader(t) != 0 { c" (leader)".as_ptr() } else { c"".as_ptr() };
                    let cpu_n = if on_cpu { c" *cpu".as_ptr() } else { c"".as_ptr() };
                    k_printf(
                        c"      tid %-4d [%s] %-2s %s%s%s\n".as_ptr(),
                        (*t).pid, ustr, st,
                        t_name_ptr(t),
                        leader, cpu_n,
                    );
                });
            });
        });
    }
}

// Simple printf trampolines (used by Rust callers elsewhere).
#[no_mangle]
pub extern "C" fn xv6_print_str(s: *const c_char) {
    u! { k_printf(c"%s".as_ptr(), s); }
}
#[no_mangle]
pub extern "C" fn xv6_print_d(v: c_int) {
    u! { k_printf(c"%d".as_ptr(), v); }
}
#[no_mangle]
pub extern "C" fn xv6_print_str_d(s: *const c_char, v: c_int) {
    u! { k_printf(s, v); }
}

// ==========================================================================
// SECTION 6: sigacts / ksiginfo / thread_signal scalar accessors.
// Wraps plain scalar/pointer fields of these structs for use by the safe
// access.rs wrappers consumed from signal.rs.
// ==========================================================================
use crate::bindings::sigacts_t as sa_sigacts_t;
use crate::bindings::ksiginfo as sa_ksiginfo;

#[no_mangle]
pub extern "C" fn sa_refcount(s: *mut sa_sigacts_t) -> c_int { field_get!(s, refcount) }
#[no_mangle]
pub extern "C" fn sa_set_refcount(s: *mut sa_sigacts_t, v: c_int) { field_set!(s, refcount, v) }
#[no_mangle]
pub extern "C" fn sa_sigterm(s: *mut sa_sigacts_t) -> u64 { field_get!(s, sa_sigterm) }
#[no_mangle]
pub extern "C" fn sa_set_sigterm(s: *mut sa_sigacts_t, v: u64) { field_set!(s, sa_sigterm, v) }
#[no_mangle]
pub extern "C" fn sa_sigstop(s: *mut sa_sigacts_t) -> u64 { field_get!(s, sa_sigstop) }
#[no_mangle]
pub extern "C" fn sa_set_sigstop(s: *mut sa_sigacts_t, v: u64) { field_set!(s, sa_sigstop, v) }
#[no_mangle]
pub extern "C" fn sa_sigcont(s: *mut sa_sigacts_t) -> u64 { field_get!(s, sa_sigcont) }
#[no_mangle]
pub extern "C" fn sa_set_sigcont(s: *mut sa_sigacts_t, v: u64) { field_set!(s, sa_sigcont, v) }
#[no_mangle]
pub extern "C" fn sa_sigignore(s: *mut sa_sigacts_t) -> u64 { field_get!(s, sa_sigignore) }
#[no_mangle]
pub extern "C" fn sa_set_sigignore(s: *mut sa_sigacts_t, v: u64) { field_set!(s, sa_sigignore, v) }

#[no_mangle]
pub extern "C" fn ksi_signo(k: *mut sa_ksiginfo) -> c_int { field_get!(k, signo) }
#[no_mangle]
pub extern "C" fn ksi_set_signo(k: *mut sa_ksiginfo, v: c_int) { field_set!(k, signo, v) }
#[no_mangle]
pub extern "C" fn ksi_receiver(k: *mut sa_ksiginfo) -> *mut thread { field_get!(k, receiver) }
#[no_mangle]
pub extern "C" fn ksi_set_receiver(k: *mut sa_ksiginfo, v: *mut thread) { field_set!(k, receiver, v) }
#[no_mangle]
pub extern "C" fn ksi_sender(k: *mut sa_ksiginfo) -> *mut thread { field_get!(k, sender) }
#[no_mangle]
pub extern "C" fn ksi_set_sender(k: *mut sa_ksiginfo, v: *mut thread) { field_set!(k, sender, v) }

#[no_mangle]
pub extern "C" fn ts_sig_mask(p: *mut thread) -> u64 { field_get!(p, signal.sig_mask) }
#[no_mangle]
pub extern "C" fn ts_set_sig_mask(p: *mut thread, v: u64) { field_set!(p, signal.sig_mask, v) }
#[no_mangle]
pub extern "C" fn ts_sig_saved_mask(p: *mut thread) -> u64 { field_get!(p, signal.sig_saved_mask) }
#[no_mangle]
pub extern "C" fn ts_set_sig_saved_mask(p: *mut thread, v: u64) { field_set!(p, signal.sig_saved_mask, v) }
#[no_mangle]
pub extern "C" fn ts_sig_pending_mask(p: *mut thread) -> u64 { field_get!(p, signal.sig_pending_mask) }
#[no_mangle]
pub extern "C" fn ts_set_sig_pending_mask(p: *mut thread, v: u64) { field_set!(p, signal.sig_pending_mask, v) }
#[no_mangle]
pub extern "C" fn ts_sig_ucontext(p: *mut thread) -> u64 { field_get!(p, signal.sig_ucontext) }
#[no_mangle]
pub extern "C" fn ts_set_sig_ucontext(p: *mut thread, v: u64) { field_set!(p, signal.sig_ucontext, v) }
#[no_mangle]
pub extern "C" fn ts_esignal(p: *mut thread) -> u64 { field_get!(p, signal.esignal) }
#[no_mangle]
pub extern "C" fn ts_set_esignal(p: *mut thread, v: u64) { field_set!(p, signal.esignal, v) }
#[no_mangle]
pub extern "C" fn ts_stop_signal(p: *mut thread) -> c_int { field_get!(p, signal.stop_signal) }
#[no_mangle]
pub extern "C" fn ts_set_stop_signal(p: *mut thread, v: c_int) { field_set!(p, signal.stop_signal, v) }
