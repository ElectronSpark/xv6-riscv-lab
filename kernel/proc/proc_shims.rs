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

// See `crate::u`'s doc comment (kernel/lib.rs) for the macro's contract.
use crate::u;

use crate::bindings::{
    ksiginfo, pgroup, session, slab_alloc, slab_cache_init, slab_cache_t, slab_free, thread, thread_group, SLAB_FLAG_EMBEDDED,
};
use crate::proc::access::smp_load_acquire_i32;
use crate::machine::smp_load_acquire_u64;
use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;

// SAFETY: `$ptr` must be a valid, non-null, properly-aligned pointer to a
// live value of the pointee struct type for the duration of the read.
// Every call site in this file passes either (a) a shim-entry-point
// parameter, whose non-nullness/liveness is guaranteed by the kernel
// convention that produced it (e.g. `current`, a proc-table
// lookup result, a list-iteration callback argument), or (b) a pointer
// derived from such a parameter via field projection. Reading a plain
// (non-atomic) field like this additionally requires that no other thread
// is concurrently writing it; callers rely on whatever lock the field's own
// C-side contract names (see the individual `xv6_*`/`t_*`/`pg_*` wrappers
// for which lock, if any, is expected to be held).
macro_rules! field_get {
    ($ptr:expr, $field:ident) => {{ u! { (*($ptr)).$field } }};
    ($ptr:expr, $field:ident.$subfield:ident) => {{ u! { (*($ptr)).$field.$subfield } }};
    ($ptr:expr, $field:ident.$subfield:ident.$leaf:ident) => {{ u! { (*($ptr)).$field.$subfield.$leaf } }};
}

// SAFETY: see `field_get!` for the pointer-validity contract. Writing a
// plain (non-atomic) field additionally requires that the caller has
// exclusive access to it — either genuinely single-owner state (e.g.
// initialisation before the object is published) or a held lock matching
// the field's own C-side contract, same as `field_get!`.
macro_rules! field_set {
    ($ptr:expr, $field:ident, $value:expr) => {{ u! { (*($ptr)).$field = $value } }};
    ($ptr:expr, $field:ident.$subfield:ident, $value:expr) => {{ u! { (*($ptr)).$field.$subfield = $value } }};
    ($ptr:expr, $field:ident.$subfield:ident.$leaf:ident, $value:expr) => {{ u! { (*($ptr)).$field.$subfield.$leaf = $value } }};
}

// SAFETY: see `field_get!`. Only the field projection (`&raw mut ...`)
// happens here, which does not itself read or write through the pointer;
// the returned pointer is not yet dereferenced, so this macro's own
// precondition is just "`$ptr` is valid per `field_get!`". Any subsequent
// deref by the caller is that caller's own obligation.
macro_rules! field_ptr_mut {
    ($ptr:expr, $field:ident) => {{ u! { &raw mut (*($ptr)).$field } }};
    ($ptr:expr, $field:ident.$subfield:ident) => {{ u! { &raw mut (*($ptr)).$field.$subfield } }};
    ($ptr:expr, $field:ident.$subfield:ident.$leaf:ident) => {{ u! { &raw mut (*($ptr)).$field.$subfield.$leaf } }};
}

// SAFETY: see `field_ptr_mut!` (same reasoning, const projection).
macro_rules! field_ptr_const {
    ($ptr:expr, $field:ident) => {{ u! { &raw const (*($ptr)).$field } }};
    ($ptr:expr, $field:ident.$subfield:ident) => {{ u! { &raw const (*($ptr)).$field.$subfield } }};
    ($ptr:expr, $field:ident.$subfield:ident.$leaf:ident) => {{ u! { &raw const (*($ptr)).$field.$subfield.$leaf } }};
}

// SAFETY: see `field_set!`. `+=` is a non-atomic read-modify-write, so this
// additionally requires the caller hold exclusive access (or the field's
// governing lock) across the whole operation, not just around a single
// load or store.
macro_rules! field_add_assign {
    ($ptr:expr, $field:ident, $value:expr) => {{ u! { (*($ptr)).$field += $value } }};
}

// SAFETY: see `field_add_assign!` (same reasoning, `-=`).
macro_rules! field_sub_assign {
    ($ptr:expr, $field:ident, $value:expr) => {{ u! { (*($ptr)).$field -= $value } }};
}

// SAFETY: see `field_get!`. `$method` is a bindgen-generated bitfield
// getter on an anonymous packed union/struct field; it only reads the
// backing storage, so the same non-null/aligned/live + no-concurrent-writer
// contract applies.
macro_rules! bit_get {
    ($ptr:expr, $anon:ident, $method:ident) => {{ u! { (*($ptr)).$anon.$method() } }};
}

// SAFETY: see `field_set!`. `$method` is a bindgen-generated bitfield
// setter; it read-modifies-writes the shared backing storage of the
// anonymous field, so the caller needs exclusive access (or the field's
// governing lock), same as `field_set!`.
macro_rules! bit_set {
    ($ptr:expr, $anon:ident, $method:ident, $value:expr) => {{ u! { (*($ptr)).$anon.$method($value) } }};
}

// SAFETY: `$ptr` must satisfy `field_ptr_mut!`'s contract so the embedded
// `list_node_t` field is a valid projection target. `list_init` only
// stores `self` into `prev`/`next`, so no other invariant is required
// beyond pointer validity — this is typically called before the node is
// linked into any list (or right after unlinking it), so there is no
// concurrent list traversal to race with.
macro_rules! list_init_field {
    ($ptr:expr, $($field:tt)+) => {{ u! { list_init(field_ptr_mut!($ptr, $($field)+)) } }};
}

// SAFETY: see `list_init_field!` for pointer validity. `list_detach`
// additionally mutates the neighbouring `prev`/`next` nodes in the list, so
// the caller must hold whatever lock serialises mutation of that list
// (e.g. `pid_lock`, a pgroup/session lock) to avoid racing a concurrent
// insert/remove on the same list.
macro_rules! list_detach_field {
    ($ptr:expr, $($field:tt)+) => {{ u! { list_detach(field_ptr_mut!($ptr, $($field)+)) } }};
}

// SAFETY: see `list_init_field!` for pointer validity. This only reads
// `next == self`, so (unlike `list_detach_field!`) no list-wide lock is
// strictly required for memory safety, though the caller may still need
// one for a coherent answer (a concurrent mutator could change the result
// out from under an unsynchronised caller).
macro_rules! list_is_detached_field {
    ($ptr:expr, $($field:tt)+) => {{ u! { list_is_detached(field_ptr_mut!($ptr, $($field)+)) as c_int } }};
}

// SAFETY: see `list_detach_field!` — both `$head_ptr` and `$entry_ptr` must
// be valid per `field_ptr_mut!`, and the caller must hold the lock
// protecting the target list, since `list_push_back` mutates the head's
// and the new entry's neighbouring pointers in place.
macro_rules! list_push_back_fields {
    ($head_ptr:expr, $($head_field:ident)+, $entry_ptr:expr, $($entry_field:ident)+) => {{
        u! { list_push_back(field_ptr_mut!($head_ptr, $($head_field)+), field_ptr_mut!($entry_ptr, $($entry_field)+)) }
    }};
}

// SAFETY: relies solely on `field_ptr_const!`'s contract for pointer
// validity; `smp_load_acquire_i32` itself has a safe signature and performs
// an atomic acquire load, so unlike `field_get!` no additional
// "no concurrent writer" precondition is needed — that's the whole point of
// routing counters like `live_threads`/`refcount`/`group_exit` through it.
macro_rules! atomic_load_i32_field {
    ($ptr:expr, $($field:tt)+) => {{ smp_load_acquire_i32(field_ptr_const!($ptr, $($field)+)) }};
}

// SAFETY: see `field_ptr_mut!` for pointer validity. `atomic_fetch_sub_i32`
// performs an atomic RMW, so concurrent atomic accessors of the same field
// are fine; the caller only needs the pointer itself to stay valid for the
// call's duration (i.e. the pointee must not be freed concurrently).
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
// P3-1B2: these three used to be bridged via the `xv6_tgport_*`/
// `xv6_thport_*` C-ABI alias layers (`extern "C"` redeclarations); now
// direct crate-path calls to the real, already-Rust definitions in
// `thread_group.rs`/`thread.rs`.
use crate::proc::{tg_signal_send, tcb_lock, tcb_unlock};

// ===========================================================================
// xv6_panic — PORTED from proc_rust_bridge.c. The C bridge wrapped the
// `panic("%s", msg)` macro (kernel/inc/printf.h); this replicates the same
// halt sequence natively: `__panic_start` (disables interrupts, takes the
// panic message lock, prints a backtrace), the caller's message, then
// `__panic_end` (releases the lock and tail-calls `trigger_panic`, which
// sends a crash IPI to every hart and never returns). P3-D1b/c: every
// consumer now reaches it as a plain crate-path item (directly or via the
// `kstd::xv6_panic` re-export that `kassert!` uses); the former
// `#[no_mangle]` C-ABI export is gone.
// ===========================================================================
unsafe extern "C" {
    fn __panic_start();
    fn __panic_end() -> !;
}

pub(crate) fn xv6_panic(msg: *const c_char) -> ! {
    // SAFETY: `__panic_start`/`__panic_end` are the real kernel panic
    // sequence and take no arguments beyond implicit global state.
    // `kprintln!` with `Cs(msg)` only reads `msg`, which every caller
    // passes as a NUL-terminated string (either a 'static C literal or a
    // caller-owned stack buffer that outlives the call). Nothing in this
    // function can itself panic or otherwise re-enter `xv6_panic`.
    unsafe {
        __panic_start();
    }
    crate::kprintln!("{}", crate::printf::Cs(msg));
    unsafe { __panic_end() }
}

// ===========================================================================
// SECTION 6: pgroup slab cache (was static in pgroup.c, then in
// proc_rust_shims.c). The cache itself moves to Rust file scope; the
// xv6_pgroup_slab_{init,alloc,free} entry points are crate-internal fns
// (P3-D1c) called by pgroup.rs via crate paths.
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

pub(crate) fn xv6_pgroup_slab_init() -> c_int {
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

pub(crate) fn xv6_pgroup_slab_alloc() -> *mut crate::bindings::pgroup {
    // SAFETY: cache was initialised via xv6_pgroup_slab_init at boot.
    u! { slab_alloc(pgroup_slab_ptr()) as *mut crate::bindings::pgroup }
}

pub(crate) fn xv6_pgroup_slab_free(pg: *mut crate::bindings::pgroup) {
    // SAFETY: caller guarantees `pg` came from xv6_pgroup_slab_alloc.
    u! { slab_free(pg as *mut c_void) }
}

// ===========================================================================
// SECTION 7: ksiginfo zero-fill helper. Builds a kernel-origin siginfo whose
// only meaningful field is `signo` and forwards to the thread-group signal
// path.
// ===========================================================================
pub(crate) fn xv6_tg_send_signo(tg: *mut thread_group, signo: c_int) -> c_int {
    // SAFETY: ksiginfo is a plain C struct (no destructor, all-bit-pattern
    // valid fields); zero-init via MaybeUninit::zeroed().assume_init() is
    // layout-equivalent to the C `memset(&info, 0, sizeof(info))`.
    let mut info: ksiginfo = u! { MaybeUninit::<ksiginfo>::zeroed().assume_init() };
    info.signo = signo;
    // SAFETY: tg comes from a Rust caller that already holds an appropriate
    // reference; info points to our own stack frame for the duration of the
    // call (tg_signal_send is synchronous and does not retain it).
    u! { tg_signal_send(tg, &mut info as *mut ksiginfo) }
}

// ===========================================================================
// SECTION 9: RCU read-side and tcb_lock thin wrappers. The xv6_* names
// existed only to give Rust a fixed symbol when the underlying primitives
// were macros / inline functions. With bindgen exposing `rcu_read_lock` and
// `tcb_lock`/`tcb_unlock` (kernel/proc/thread.rs) being plain Rust fns, the
// wrappers are still useful as the documented Rust-facing names.
// ===========================================================================

pub(crate) fn xv6_rcu_read_lock() {
    // SAFETY: rcu_read_lock just bumps a per-thread depth counter and
    // disables preemption; no preconditions beyond "in kernel context".
    u! { rcu_read_lock() }
}

pub(crate) fn xv6_rcu_read_unlock() {
    // SAFETY: must be balanced with a prior xv6_rcu_read_lock; caller's
    // responsibility, same as the C wrapper this replaces.
    u! { rcu_read_unlock() }
}

pub(crate) fn xv6_tcb_lock(p: *mut crate::bindings::thread) {
    tcb_lock(p)
}

pub(crate) fn xv6_tcb_unlock(p: *mut crate::bindings::thread) {
    tcb_unlock(p)
}

// SECTION 1 leftover: xv6_current_thread — replaces the `current` macro,
// which expands to an interrupt-safe load of `mycpu()->proc`. We replicate
// the same sstatus.SIE save/restore dance in Rust inline asm.
pub(crate) fn xv6_current_thread() -> *mut thread {
    crate::machine::current_thread_ptr()
}

// SECTION 1 leftovers: thread_state_short / thread_state_to_str. Pure
// switch tables — direct ports of the static-inline definitions in
// kernel/inc/proc/thread.h.
pub(crate) fn xv6_thread_state_to_str(st: c_int) -> *const c_char {
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

pub(crate) fn xv6_thread_state_short(st: c_int) -> *const c_char {
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

pub(crate) fn t_pid(p: *mut thread) -> c_int { field_get!(p, pid) }
pub(crate) fn t_tgid(p: *mut thread) -> c_int { field_get!(p, tgid) }
pub(crate) fn t_pgid(p: *mut thread) -> c_int { field_get!(p, pgid) }
pub(crate) fn t_sid(p: *mut thread) -> c_int { field_get!(p, sid) }
pub(crate) fn t_set_pid(p: *mut thread, pid: c_int) { field_set!(p, pid, pid) }
pub(crate) fn t_set_pgid(p: *mut thread, v: c_int) { field_set!(p, pgid, v) }

pub(crate) fn t_parent(p: *mut thread) -> *mut thread { field_get!(p, parent) }
pub(crate) fn t_pgroup(p: *mut thread) -> *mut pgroup { field_get!(p, pgroup) }
pub(crate) fn t_session(p: *mut thread) -> *mut session { field_get!(p, session) }
pub(crate) fn t_thread_group(p: *mut thread) -> *mut thread_group {
    field_get!(p, thread_group)
}
pub(crate) fn t_set_pgroup(p: *mut thread, pg: *mut pgroup) {
    field_set!(p, pgroup, pg)
}

pub(crate) fn t_vm(p: *mut thread) -> *mut crate::bindings::vm_t { field_get!(p, vm) }
pub(crate) fn t_fdtable(p: *mut thread) -> *mut crate::bindings::vfs_fdtable { field_get!(p, fdtable) }
pub(crate) fn t_sigacts(p: *mut thread) -> *mut crate::bindings::sigacts_t { field_get!(p, sigacts) }
pub(crate) fn t_trapframe(p: *mut thread) -> *mut crate::bindings::utrapframe { field_get!(p, trapframe) }
pub(crate) fn t_set_vm(p: *mut thread, v: *mut crate::bindings::vm_t) { field_set!(p, vm, v) }
pub(crate) fn t_set_fdtable(p: *mut thread, v: *mut crate::bindings::vfs_fdtable) { field_set!(p, fdtable, v) }
pub(crate) fn t_set_sigacts(p: *mut thread, v: *mut crate::bindings::sigacts_t) { field_set!(p, sigacts, v) }
pub(crate) fn t_set_trapframe(p: *mut thread, v: *mut crate::bindings::utrapframe) { field_set!(p, trapframe, v) }
pub(crate) fn t_set_parent(p: *mut thread, par: *mut thread) { field_set!(p, parent, par) }
pub(crate) fn t_set_thread_group(p: *mut thread, tg: *mut thread_group) { field_set!(p, thread_group, tg) }
pub(crate) fn t_set_session(p: *mut thread, s: *mut session) { field_set!(p, session, s) }
pub(crate) fn t_set_tgid(p: *mut thread, v: c_int) { field_set!(p, tgid, v) }
pub(crate) fn t_set_sid(p: *mut thread, v: c_int) { field_set!(p, sid, v) }

// --- SECTION 3: struct pgroup ---------------------------------------------

pub(crate) fn pg_pgid(pg: *mut pgroup) -> c_int { field_get!(pg, pgid) }
pub(crate) fn pg_t_cnt(pg: *mut pgroup) -> c_int { field_get!(pg, t_cnt) }
pub(crate) fn pg_p_cnt(pg: *mut pgroup) -> c_int { field_get!(pg, p_cnt) }
pub(crate) fn pg_exited(pg: *mut pgroup) -> c_int {
    bit_get!(pg, __bindgen_anon_1, exited) as c_int
}
pub(crate) fn pg_is_kernel(pg: *mut pgroup) -> c_int {
    bit_get!(pg, __bindgen_anon_1, is_kernel) as c_int
}
pub(crate) fn pg_session(pg: *mut pgroup) -> *mut session { field_get!(pg, session) }

pub(crate) fn pg_set_pgid(pg: *mut pgroup, v: c_int) { field_set!(pg, pgid, v) }
pub(crate) fn pg_set_leader(pg: *mut pgroup, tg: *mut thread_group) {
    field_set!(pg, leader, tg)
}
pub(crate) fn pg_set_session(pg: *mut pgroup, s: *mut session) {
    field_set!(pg, session, s)
}
pub(crate) fn pg_set_exited(pg: *mut pgroup, v: c_int) {
    bit_set!(pg, __bindgen_anon_1, set_exited, if v != 0 { 1 } else { 0 })
}
pub(crate) fn pg_set_t_cnt(pg: *mut pgroup, v: c_int) { field_set!(pg, t_cnt, v) }
pub(crate) fn pg_set_p_cnt(pg: *mut pgroup, v: c_int) { field_set!(pg, p_cnt, v) }
pub(crate) fn pg_inc_t_cnt(pg: *mut pgroup) { field_add_assign!(pg, t_cnt, 1) }
pub(crate) fn pg_dec_t_cnt(pg: *mut pgroup) { field_sub_assign!(pg, t_cnt, 1) }
pub(crate) fn pg_inc_p_cnt(pg: *mut pgroup) { field_add_assign!(pg, p_cnt, 1) }
pub(crate) fn pg_dec_p_cnt(pg: *mut pgroup) { field_sub_assign!(pg, p_cnt, 1) }

// --- SECTION 4: struct thread_group ---------------------------------------

pub(crate) fn tg_pgroup(tg: *mut thread_group) -> *mut pgroup { field_get!(tg, pgroup) }
pub(crate) fn tg_tgid(tg: *mut thread_group) -> c_int { field_get!(tg, tgid) }
pub(crate) fn tg_set_pgroup(tg: *mut thread_group, pg: *mut pgroup) {
    field_set!(tg, pgroup, pg)
}
pub(crate) fn tg_group_leader(tg: *mut thread_group) -> *mut thread { field_get!(tg, group_leader) }
pub(crate) fn tg_set_tgid(tg: *mut thread_group, v: c_int) { field_set!(tg, tgid, v) }

// --- SECTION 5: struct session --------------------------------------------

pub(crate) fn session_sid(s: *mut session) -> c_int { field_get!(s, sid) }
pub(crate) fn session_t_cnt(s: *mut session) -> c_int { field_get!(s, t_cnt) }
pub(crate) fn session_pg_cnt(s: *mut session) -> c_int { field_get!(s, pg_cnt) }
pub(crate) fn session_fg_pgrp(s: *mut session) -> *mut pgroup {
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
///
/// SAFETY (caller of this `unsafe fn`): `e` must be a valid, non-null,
/// properly-aligned pointer to a live `list_node_t` (typically obtained via
/// `field_ptr_mut!`/`list_init_field!` from a shim-entry-point parameter).
/// Called either before the node is linked into any list or right after
/// unlinking it, so there is nothing else concurrently traversing it.
#[inline]
unsafe fn list_init(e: *mut list_node_t) {
    // SAFETY: see the fn-level contract above.
    u! {
        (*e).prev = e;
        (*e).next = e;
    }
}

/// SAFETY (caller): see `list_init`. Only reads `e->next`, so no list-wide
/// lock is strictly required for memory safety.
#[inline]
unsafe fn list_is_detached(e: *mut list_node_t) -> bool {
    // SAFETY: see the fn-level contract above.
    u! { (*e).next == e }
}

/// SAFETY (caller): see `list_init` for pointer validity. Additionally
/// mutates the neighbouring `prev`/`next` nodes in place, so the caller
/// must hold whatever lock serialises mutation of the list `e` is
/// currently linked into (matching `list_detach_field!`'s contract).
#[inline]
unsafe fn list_detach(e: *mut list_node_t) {
    // SAFETY: see the fn-level contract above.
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
///
/// SAFETY (caller): `head` and `new` must both be valid per `list_init`'s
/// contract, and the caller must hold the lock protecting the target list
/// (matching `list_push_back_fields!`'s contract), since this mutates
/// `head`'s and `new`'s neighbouring pointers in place.
#[inline]
unsafe fn list_push_back(head: *mut list_node_t, new: *mut list_node_t) {
    // SAFETY: see the fn-level contract above.
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
///
/// SAFETY (caller): `head` must be valid per `list_init`'s contract and
/// `member_offset` must be the true offset of a `list_node_t` field
/// embedded in `T` (every call site below passes a `core::mem::offset_of!`
/// constant, never an arbitrary value), so `entry.sub(member_offset)`
/// recovers a valid `*mut T`. The walk snapshots `next` before invoking
/// `cb`, matching the "safe" (removal-tolerant) C iterator, but `cb` must
/// not free or relink nodes further ahead in the list than the current one.
#[inline]
unsafe fn list_foreach_safe<T>(
    head: *mut list_node_t,
    member_offset: usize,
    mut cb: impl FnMut(*mut T),
) {
    // SAFETY: see the fn-level contract above.
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

/// SAFETY (caller): `p` must be a valid, non-null, 4-byte-aligned pointer to
/// a live `c_int` for the duration of the call (typically a field pointer
/// from `field_ptr_mut!`). The RMW itself is atomic, so no additional
/// exclusivity is required against other atomic accessors of the same word.
#[inline]
unsafe fn atomic_fetch_sub_i32(p: *mut c_int, v: c_int) -> c_int {
    // SAFETY: see the fn-level contract above.
    u! { AtomicI32::from_ptr(p as *mut i32).fetch_sub(v, Ordering::SeqCst) }
}

/// SAFETY (caller): see `atomic_fetch_sub_i32` (same reasoning, 8-byte
/// aligned `u64`).
#[inline]
unsafe fn atomic_or_fetch_u64(p: *mut u64, v: u64) -> u64 {
    // SAFETY: see the fn-level contract above.
    u! { AtomicU64::from_ptr(p).fetch_or(v, Ordering::SeqCst) | v }
}

// ===========================================================================
// THREAD FLAGS — bit positions from kernel/inc/proc/thread_types.h.
// ===========================================================================
const THREAD_FLAG_USER_SPACE: u64 = 5;
const THREAD_FLAG_SELF_REAP: u64 = 6;
const THREAD_FLAG_KILLED: u64 = 2;

const THREAD_STATE_STOPPED: c_int = 9;
const THREAD_STATE_ZOMBIE: c_int = 11;

/// SAFETY (caller): `p`, if non-null, must be a valid, live `*mut thread`
/// (checked below); the null check makes the "if null, treat as not
/// user-space" branch safe without dereferencing.
#[inline]
unsafe fn thread_user_space(p: *mut thread) -> bool {
    if p.is_null() {
        return false;
    }
    // SAFETY: `p` was just checked non-null above; see the fn-level
    // contract for the rest. Plain atomic acquire load — no lock needed.
    let flags = u! { smp_load_acquire_u64(&(*p).flags as *const u64) };
    (flags & (1u64 << THREAD_FLAG_USER_SPACE)) != 0
}

/// SAFETY (caller): see `thread_user_space`.
#[inline]
unsafe fn thread_set_flag(p: *mut thread, bit: u64) {
    if p.is_null() {
        return;
    }
    // SAFETY: `p` was just checked non-null above. `atomic_or_fetch_u64` is
    // an atomic RMW, so concurrent flag-setters on other bits of the same
    // word don't race destructively.
    u! {
        let _ = atomic_or_fetch_u64(&(*p).flags as *const u64 as *mut u64, 1u64 << bit);
    }
}

/// SAFETY (caller): see `thread_user_space`.
#[inline]
unsafe fn thread_state_load(p: *mut thread) -> c_int {
    if p.is_null() {
        return 0;
    }
    // SAFETY: `p` was just checked non-null above. Plain atomic acquire
    // load of `state`, matching `__thread_state_get` — no lock needed.
    u! { smp_load_acquire_i32(&(*p).state as *const _ as *const c_int) }
}

// ===========================================================================
// SECTION 2 — remaining accessors (macros / atomics / lists).
// ===========================================================================

pub(crate) fn t_pg_entry_init(t: *mut thread) {
    list_init_field!(t, pg_entry)
}
pub(crate) fn t_pg_entry_is_detached(t: *mut thread) -> c_int {
    list_is_detached_field!(t, pg_entry)
}

pub(crate) fn t_user_space(p: *mut thread) -> c_int {
    // SAFETY: `thread_user_space` null-checks internally; `p` is a
    // caller-supplied thread pointer per this shim's contract
    // (non-null when meaningful, otherwise treated as "not
    // user-space").
    u! { thread_user_space(p) as c_int }
}

pub(crate) fn t_dmp_list_entry_is_detached(t: *mut thread) -> c_int {
    list_is_detached_field!(t, dmp_list_entry)
}

// ===========================================================================
// SECTION 3 — remaining pgroup accessors.
// ===========================================================================

pub(crate) fn pg_atomic_dec_t_cnt(pg: *mut pgroup) -> c_int {
    // C `atomic_dec` returns the new value.
    atomic_fetch_sub_i32_field!(pg, t_cnt, 1) - 1
}

pub(crate) fn pg_list_entry_init(pg: *mut pgroup) {
    list_init_field!(pg, list_entry)
}
pub(crate) fn pg_list_entry_detach(pg: *mut pgroup) {
    list_detach_field!(pg, list_entry)
}
pub(crate) fn pg_list_entry_is_detached(pg: *mut pgroup) -> c_int {
    list_is_detached_field!(pg, list_entry)
}

pub(crate) fn pg_threads_init(pg: *mut pgroup) {
    list_init_field!(pg, threads)
}
pub(crate) fn pg_tgs_init(pg: *mut pgroup) {
    list_init_field!(pg, thread_groups)
}

pub(crate) fn pg_threads_push_back(pg: *mut pgroup, t: *mut thread) {
    list_push_back_fields!(pg, threads, t, pg_entry)
}
pub(crate) fn pg_threads_detach(t: *mut thread) {
    list_detach_field!(t, pg_entry)
}

pub(crate) fn pg_tgs_push_back(pg: *mut pgroup, tg: *mut thread_group) {
    list_push_back_fields!(pg, thread_groups, tg, list_entry)
}
pub(crate) fn pg_tgs_detach(tg: *mut thread_group) {
    list_detach_field!(tg, list_entry)
}
pub(crate) fn pg_tg_list_entry_is_detached(tg: *mut thread_group) -> c_int {
    list_is_detached_field!(tg, list_entry)
}

pub(crate) fn pg_for_each_tg(
    pg: *mut pgroup,
    fn_cb: Option<unsafe extern "C" fn(*mut thread_group, *mut c_void)>,
    arg: *mut c_void,
) {
    let Some(cb) = fn_cb else { return };
    let off = core::mem::offset_of!(thread_group, list_entry);
    // SAFETY: `pg` is a caller-supplied, live `*mut pgroup`; `off` is a
    // genuine `offset_of!` constant for `thread_group::list_entry`,
    // matching `list_foreach_safe`'s contract. `cb` is an opaque C
    // callback that may detach the *current* node (that's what the
    // "safe"/next-snapshotting walk tolerates) but per that same contract
    // must not relink nodes further ahead in the list.
    u! {
        list_foreach_safe::<thread_group>(&raw mut (*pg).thread_groups, off, |tg| {
            cb(tg, arg);
        })
    }
}

// ===========================================================================
// SECTION 4 — remaining thread_group accessors.
// ===========================================================================

pub(crate) fn tg_list_entry_init(tg: *mut thread_group) {
    list_init_field!(tg, list_entry)
}

pub(crate) fn tg_for_each_thread(
    tg: *mut thread_group,
    fn_cb: Option<unsafe extern "C" fn(*mut thread, *mut c_void)>,
    arg: *mut c_void,
) {
    let Some(cb) = fn_cb else { return };
    let off = core::mem::offset_of!(thread, tg_entry);
    // SAFETY: see `pg_for_each_tg`; `tg` is caller-supplied and live,
    // `off` is a genuine `offset_of!` for `thread::tg_entry`.
    u! {
        list_foreach_safe::<thread>(&raw mut (*tg).thread_list, off, |t| {
            cb(t, arg);
        })
    }
}

// ===========================================================================
// SECTION 11/12 helpers — minimal accessors common to clone/exit support.
// ===========================================================================

pub(crate) fn xv6_thread_is_zombie(t: *mut thread) -> c_int {
    // SAFETY: see `thread_state_load`; `t` is caller-supplied, null-checked
    // internally by `thread_state_load`.
    u! { (thread_state_load(t) == THREAD_STATE_ZOMBIE) as c_int }
}
pub(crate) fn xv6_thread_is_stopped(t: *mut thread) -> c_int {
    // SAFETY: see `xv6_thread_is_zombie`.
    u! { (thread_state_load(t) == THREAD_STATE_STOPPED) as c_int }
}
pub(crate) fn xv6_t_set_user_space(t: *mut thread) {
    // SAFETY: see `thread_set_flag`; `t` is caller-supplied, null-checked
    // internally by `thread_set_flag`.
    u! { thread_set_flag(t, THREAD_FLAG_USER_SPACE) }
}
pub(crate) fn xv6_t_set_self_reap(t: *mut thread) {
    // SAFETY: see `xv6_t_set_user_space`.
    u! { thread_set_flag(t, THREAD_FLAG_SELF_REAP) }
}

// ===========================================================================
// SECTION 11 — clone.c field accessors. The C side uses void* for opaque
// kernel objects (vm_t, fs_struct, vfs_fdtable, sigacts_t, sched_entity);
// we mirror that here. trapframe / utrapframe are exposed by bindgen so we
// can do the full struct copy and sepc/sp/a0 writes directly.
// ===========================================================================

use crate::bindings::{thread_signal_t, utrapframe};

pub(crate) fn xv6_t_kstack_order(t: *mut thread) -> c_int {
    field_get!(t, kstack_order)
}
pub(crate) fn xv6_t_vm(t: *mut thread) -> *mut c_void {
    field_get!(t, vm) as *mut c_void
}
pub(crate) fn xv6_t_set_vm(t: *mut thread, v: *mut c_void) {
    field_set!(t, vm, v as *mut crate::bindings::vm_t)
}
pub(crate) fn xv6_t_fs(t: *mut thread) -> *mut c_void {
    field_get!(t, fs) as *mut c_void
}
pub(crate) fn xv6_t_set_fs(t: *mut thread, f: *mut c_void) {
    field_set!(t, fs, f as *mut crate::bindings::fs_struct)
}
pub(crate) fn xv6_t_fdtable(t: *mut thread) -> *mut c_void {
    field_get!(t, fdtable) as *mut c_void
}
pub(crate) fn xv6_t_set_fdtable(t: *mut thread, f: *mut c_void) {
    field_set!(t, fdtable, f as *mut crate::bindings::vfs_fdtable)
}
pub(crate) fn xv6_t_sigacts(t: *mut thread) -> *mut c_void {
    field_get!(t, sigacts) as *mut c_void
}
pub(crate) fn xv6_t_set_sigacts(t: *mut thread, s: *mut c_void) {
    field_set!(t, sigacts, s as *mut crate::bindings::sigacts_t)
}
pub(crate) fn xv6_t_signal_ptr(t: *mut thread) -> *mut c_void {
    field_ptr_mut!(t, signal) as *mut c_void
}
pub(crate) fn xv6_t_sched_entity(t: *mut thread) -> *mut c_void {
    field_get!(t, sched_entity) as *mut c_void
}
pub(crate) fn xv6_t_set_clone_flags(t: *mut thread, f: u64) {
    field_set!(t, clone_flags, f)
}
pub(crate) fn xv6_t_set_vfork_parent(t: *mut thread, p: *mut thread) {
    field_set!(t, vfork_parent, p)
}
pub(crate) fn xv6_t_vfork_parent(t: *mut thread) -> *mut thread {
    field_get!(t, vfork_parent)
}
pub(crate) fn xv6_t_set_tgid(t: *mut thread, tgid: c_int) {
    field_set!(t, tgid, tgid)
}
pub(crate) fn xv6_t_set_parent(t: *mut thread, p: *mut thread) {
    field_set!(t, parent, p)
}
pub(crate) fn xv6_t_set_thread_group(t: *mut thread, tg: *mut thread_group) {
    field_set!(t, thread_group, tg)
}

pub(crate) fn xv6_t_copy_trapframe(dst: *mut thread, src: *mut thread) {
    // C: *(dst->trapframe) = *(src->trapframe);  // copy utrapframe by value.
    // SAFETY: `dst`/`src` are caller-supplied, live `*mut thread`s (per the
    // clone.c call sites that use this shim); `.trapframe` is a non-null
    // pointer to that thread's dedicated per-thread trapframe page for the
    // lifetime of the thread, so a same-sized non-overlapping copy between
    // two distinct threads' trapframes is valid. The caller (clone path) is
    // setting up `dst` before it is published/scheduled, so nothing else
    // is concurrently touching it.
    u! { ptr::copy_nonoverlapping((*src).trapframe, (*dst).trapframe, 1usize) }
}
pub(crate) fn xv6_t_trapframe_set_sepc(t: *mut thread, v: u64) {
    // SAFETY: see `xv6_t_copy_trapframe`; `t->trapframe` is non-null and
    // live, and only `t` itself (or its clone-time parent, before `t` is
    // published) writes these fields.
    u! { (*(*t).trapframe).trapframe.sepc = v }
}
pub(crate) fn xv6_t_trapframe_set_sp(t: *mut thread, v: u64) {
    // SAFETY: see `xv6_t_trapframe_set_sepc`.
    u! { (*(*t).trapframe).trapframe.sp = v }
}
pub(crate) fn xv6_t_trapframe_set_a0(t: *mut thread, v: u64) {
    // SAFETY: see `xv6_t_trapframe_set_sepc`.
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

pub(crate) fn xv6_t_set_xstate(t: *mut thread, v: c_int) {
    field_set!(t, xstate, v)
}
pub(crate) fn xv6_t_clone_flags(t: *mut thread) -> u64 {
    field_get!(t, clone_flags)
}
pub(crate) fn xv6_t_signal_esignal(t: *mut thread) -> c_int {
    // C field is uint64; cast to int matches the C shim's return type.
    field_get!(t, signal.esignal) as c_int
}
pub(crate) fn xv6_t_signal_stop_signal(t: *mut thread) -> c_int {
    field_get!(t, signal.stop_signal)
}
pub(crate) fn xv6_t_children_count(t: *mut thread) -> c_int {
    field_get!(t, children_count)
}

pub(crate) fn xv6_tg_group_exit_task_is(
    tg: *mut thread_group,
    t: *mut thread,
) -> c_int {
    // SAFETY: see `field_get!` — `tg` is a caller-supplied, live
    // `*mut thread_group`; this only reads `group_exit_task` and compares
    // the pointer value (no deref of `t`).
    u! { ((*tg).group_exit_task == t) as c_int }
}
pub(crate) fn xv6_tg_group_exit_code(tg: *mut thread_group) -> c_int {
    field_get!(tg, group_exit_code)
}
pub(crate) fn xv6_tg_group_leader(tg: *mut thread_group) -> *mut thread {
    field_get!(tg, group_leader)
}

pub(crate) fn xv6_tg_is_exiting(tg: *mut thread_group) -> c_int {
    // C inline: __atomic_load_n(&tg->group_exit, ACQUIRE) != 0.
    if tg.is_null() {
        return 0;
    }
    // SAFETY: see `atomic_load_i32_field!` — `tg` was just checked
    // non-null above; `smp_load_acquire_i32` is a safe atomic acquire load
    // so no additional exclusivity is required.
    u! { (smp_load_acquire_i32(&raw const (*tg).group_exit) != 0) as c_int }
}

// ===========================================================================
// SECTION 1 — misc helpers used by sched_idle.rs / sched_fifo.rs / signal.rs.
// (xv6_current_thread + xv6_thread_state_short/_to_str remain in
// proc_rust_shims.c because they call C-side `current` / `static inline`
// helpers; everything else moves here.)
// ===========================================================================

// P3-1B: only caller is `proc/sched_idle.rs` (typed thin wrapper there,
// not an `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn thread_sched_entity(
    t: *mut thread,
) -> *mut crate::bindings::sched_entity {
    field_get!(t, sched_entity)
}

pub(crate) fn xv6_thread_state_set(p: *mut thread, s: c_int) {
    // C inline: __atomic_store_n(&p->state, state, SEQ_CST); NULL-safe.
    if p.is_null() {
        return;
    }
    // SAFETY: `p` was just checked non-null above; the store is atomic, so
    // it doesn't race destructively with other atomic accessors of `state`.
    u! {
        AtomicI32::from_ptr(&raw mut (*p).state as *mut i32).store(s, Ordering::SeqCst);
    }
}

pub(crate) fn xv6_sizeof_sigaction() -> u64 {
    // _Static_assert in the C side enforced sizeof(struct sigaction) == 24.
    24
}

// IS_ERR / ERR_PTR / PTR_ERR — match the macros in kernel/inc/errno.h.
const MAX_ERRNO: u64 = 4095;

pub(crate) fn xv6_is_err(p: *const c_void) -> c_int {
    // C: (unsigned long)x >= (unsigned long)-MAX_ERRNO.
    // -MAX_ERRNO as u64 is a very large value just below 2^64; the comparison
    // is true exactly when (i64)p ∈ [-MAX_ERRNO, -1].
    let v = p as u64;
    let threshold = (-(MAX_ERRNO as i64)) as u64;
    (v >= threshold) as c_int
}

pub(crate) fn xv6_err_ptr(err: i64) -> *mut c_void {
    err as *mut c_void
}

pub(crate) fn xv6_ptr_err(p: *const c_void) -> i64 {
    p as i64
}

// ===========================================================================
// SECTION 5 leftover: session_for_each_all — walks the global `session_list`
// declared in kernel/inc/tty/session.h.
// ===========================================================================

// P3-1C mesh sweep: tty/session.rs is in scope for this wave, so
// `session_list` becomes a plain crate-path import instead of an
// `extern "C"` redeclaration (identical `list_node_t` type).
use crate::tty::session::session_list;

pub(crate) fn session_for_each_all(
    fn_cb: Option<unsafe extern "C" fn(*mut session, *mut c_void)>,
    arg: *mut c_void,
) {
    let Some(cb) = fn_cb else { return };
    let off = core::mem::offset_of!(session, global_entry);
    // SAFETY: see `pg_for_each_tg`. `session_list` is the kernel-global
    // sentinel `list_node_t` (declared in the `unsafe extern "C"` block
    // above), valid for `'static`; `off` is a genuine `offset_of!` for
    // `session::global_entry`.
    u! {
        list_foreach_safe::<session>(&raw mut session_list, off, |s| {
            cb(s, arg);
        })
    }
}

// ===========================================================================
// SECTION 11/12 trivial wrappers (cpu_relax, intr_on, smp_mb, either_copyout_int).
// ===========================================================================

pub(crate) fn xv6_cpu_relax() {
    crate::machine::cpu_relax();
}

pub(crate) fn xv6_smp_mb() {
    crate::machine::smp_mb();
}

pub(crate) fn xv6_intr_on() {
    crate::machine::intr_on();
}

pub(crate) fn xv6_mycpu_clear_noff() {
    let mut cpu = crate::machine::CpuLocal::current();
    cpu.set_noff(0);
}

// forkret_assert_user — die loudly if a kernel thread is about to return to
// user space. Mirrors the C `assert(THREAD_USER_SPACE(p), "...", p->pid)`.
// P3-1C mesh sweep: printf.rs is in scope for this wave, so `trigger_panic`
// becomes a plain crate-path import instead of an `extern "C"` redeclaration.
use crate::printf::trigger_panic;

pub(crate) fn xv6_forkret_assert_user(p: *mut thread) {
    // SAFETY: `thread_user_space` itself tolerates a null `p` (returns
    // `false`), but the subsequent `(*p).pid` read does not — this
    // function mirrors the C `assert(THREAD_USER_SPACE(p), ...)` call in
    // forkret, which always passes the just-scheduled current thread, never
    // null. `p` must be a live, non-null `*mut thread` for that reason.
    u! {
        if !thread_user_space(p) {
            // Format matches the C assert() output (file/line elided).
            crate::kprintln!(
                "ASSERTION_FAILURE: kernel thread {} tries to return to user space",
                (*p).pid,
            );
            trigger_panic();
        }
    }
}

pub(crate) fn xv6_either_copyout_int(dst: u64, v: c_int) -> c_int {
    // C: either_copyout(1 /*user_dst*/, dst, &v, sizeof(v))
    let local = v;
    // SAFETY: `either_copyout` validates `dst` against the current
    // process's user address space itself (that's its whole job, mirroring
    // the C helper); `&local` is a valid kernel stack pointer, non-null,
    // and exactly `size_of::<c_int>()` bytes, for the duration of this call.
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

pub(crate) fn xv6_thread_from_context(
    ctx: *mut c_void,
) -> *mut thread {
    // C: thread_from_context(ctx) = container_of(ctx, sched_entity, context)->thread
    let off = core::mem::offset_of!(sched_entity, context);
    let se = (ctx as *mut u8).wrapping_sub(off) as *mut sched_entity;
    field_get!(se, thread)
}

pub(crate) fn xv6_exit_find_zombie_child(
    p: *mut thread,
) -> *mut thread {
    let off_siblings = core::mem::offset_of!(thread, siblings);
    let head = field_ptr_mut!(p, children);
    let mut found: *mut thread = core::ptr::null_mut();
    // SAFETY: `head` is valid per `field_ptr_mut!`'s contract (`p` is a
    // caller-supplied live thread); `off_siblings` is a genuine
    // `offset_of!` for `thread::siblings`. `thread_state_load` null-checks
    // defensively, though every child reached here is a live thread.
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

pub(crate) fn xv6_exit_find_stopped_child(
    p: *mut thread,
) -> *mut thread {
    let off_siblings = core::mem::offset_of!(thread, siblings);
    let head = field_ptr_mut!(p, children);
    let mut found: *mut thread = core::ptr::null_mut();
    // SAFETY: see `xv6_exit_find_zombie_child`.
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
//         detach_child(p, child);
//         attach_child(initproc, child);
//     }
//     pid_wunlock(); rcu_read_unlock();
// ---------------------------------------------------------------------------

const SIGCHLD: u64 = 17;

// P3-1B2: these four used to be bridged via the `xv6_thport_*`/
// `xv6_schport_*` C-ABI alias layers (`extern "C"` redeclarations); now
// direct crate-path calls to the real, already-Rust definitions in
// `thread.rs`/`sched.rs`.
use crate::proc::{detach_child, attach_child, thread_destroy, scheduler_yield};

// P3-1B mesh sweep: same-crate `pub(crate)` items as of this wave,
// referenced via a crate path instead of `extern "C"` redeclarations.
// `pid::__free_pid` takes/returns nothing, no cast needed;
// `pid::proctab_proc_remove` takes `pid::Thread`, a distinct (but
// layout-identical) opaque marker from `crate::bindings::thread` --
// reinterpreted via a thin wrapper, same precedent as `sysproc.rs`'s
// `thread_clone`.
use crate::proc::pid::__free_pid;
#[inline(always)]
fn proctab_proc_remove(p: *mut thread) {
    crate::proc::pid::proctab_proc_remove(p as *mut c_void as *mut crate::proc::pid::Thread);
}

pub(crate) fn xv6_exit_reparent_do(
    p: *mut thread,
    initproc: *mut thread,
) -> c_int {
    let mut zombie_found: c_int = 0;
    let off_siblings = core::mem::offset_of!(thread, siblings);
    // SAFETY: `p` and `initproc` are caller-supplied, live `*mut thread`s;
    // `off_siblings` is a genuine `offset_of!` for `thread::siblings`. The
    // walk mutates the child's `signal.esignal` field and detaches/attaches
    // it into a different parent's child list; per the C body this
    // function mirrors (see comment above), the whole operation runs under
    // `_rcu` (an RCU read-side section held for this scope) and
    // `xv6_pid_wlock()` (acquired just below), matching the locking
    // `detach_child`/`attach_child` require.
    u! {
        let _rcu = crate::lock::rcu::KRcuRead::new();
        xv6_pid_wlock();
        let head = &raw mut (*p).children;
        list_foreach_safe::<thread>(head, off_siblings, |child| {
            (*child).signal.esignal = SIGCHLD;
            if thread_state_load(child) == THREAD_STATE_ZOMBIE {
                zombie_found = 1;
            }
            detach_child(p, child);
            attach_child(initproc, child);
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

pub(crate) fn xv6_exit_reap_zombie(
    parent: *mut thread,
    child: *mut thread,
    xstate_out: *mut c_int,
) -> c_int {
    // SAFETY: `parent`/`child` are caller-supplied, live `*mut thread`s
    // (per the fn-level doc comment above: caller holds `pid_rlock` and
    // `parent`'s state is INTERRUPTIBLE); `child->sched_entity` is set at
    // thread-creation time and is non-null for the lifetime of a live
    // thread; `xstate_out` is a caller-supplied valid `*mut c_int`. The
    // lock hand-off sequence (runlock/yield/rlock, then runlock/wlock)
    // mirrors the C body exactly, so the pid-lock protocol invariants the
    // extern helpers (`detach_child`, `proctab_proc_remove`,
    // `__free_pid`, `thread_destroy`) rely on are upheld at each
    // call.
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
                scheduler_yield();
                xv6_pid_rlock();
                xv6_thread_state_set(parent, THREAD_STATE_INTERRUPTIBLE);
                spin_count = 0;
            }
        }
        xv6_thread_state_set(parent, THREAD_STATE_RUNNING);
        // -------------------------------------------------------------------
        // POSIX wait-status encoding (consumed via the WIFEXITED/WEXITSTATUS/
        // WIFSIGNALED/WTERMSIG macros in kernel/inc/uabi/wait.h; WIFSTOPPED's
        // `(stop_signal << 8) | 0x7f` sibling encoding is built directly in
        // `waitpid()`'s WUNTRACED branch, kernel/proc/exit.rs -- a stopped
        // child is never reaped, so it never reaches this function):
        //
        //   * Exited normally: `(*child).xstate` holds the raw status the
        //     process passed to `exit(status)` (kernel/proc/exit.rs sets it
        //     verbatim via `ffi::set_xstate`). Encode as `(status & 0xff)
        //     << 8` -- WIFEXITED checks the low byte is zero, WEXITSTATUS
        //     extracts the high byte (the classic POSIX 8-bit-truncated
        //     exit code).
        //   * Killed by a fatal signal: `THREAD_FLAG_KILLED` is set on the
        //     zombie and `signal.term_signal` holds the actual killing
        //     signal number, recorded at the moment the kill decision was
        //     made (see `set_term_signal_first` call sites in signal.rs,
        //     alongside every `THREAD_SET_KILLED` call). Encode as
        //     `term_signal & 0x7f` -- a nonzero low-7-bits value, matching
        //     WIFSIGNALED's `(w & 0x7f) in (0, 0x7f)` test and WTERMSIG's
        //     `w & 0x7f` extraction. `(*child).xstate` is deliberately NOT
        //     consulted in this branch: it may still hold a stale value
        //     from an internal `exit(-1)` call (sigreturn's corrupt-frame
        //     path, trap.rs's already-killed fast-out) that predates the
        //     kill decision and is no longer meaningful once the flag is
        //     authoritative. `term_signal` is defensively re-validated
        //     (falls back to SIGKILL) in case it is ever 0 despite the flag
        //     being set -- should not happen given the paired call sites,
        //     but a corrupt/garbage status word is worse than a slightly
        //     wrong signal number.
        // -------------------------------------------------------------------
        let killed = (smp_load_acquire_u64(&(*child).flags as *const u64)
            & (1u64 << THREAD_FLAG_KILLED)) != 0;
        *xstate_out = if killed {
            const SIGKILL_FALLBACK: c_int = 9;
            let ts = (*child).signal.term_signal;
            let ts = if ts > 0 && ts < 0x7f { ts } else { SIGKILL_FALLBACK };
            ts & 0x7f
        } else {
            ((*child).xstate & 0xff) << 8
        };
        let pid = (*child).pid;

        if xv6_pid_try_lock_upgrade() == 0 {
            xv6_pid_runlock();
            xv6_pid_wlock();
        }
        detach_child(parent, child);
        proctab_proc_remove(child);
        xv6_pid_wunlock();
        __free_pid();
        thread_destroy(child);
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
///
/// SAFETY (caller): `p` must be a valid, non-null, 8-byte-aligned pointer
/// to a live `i64` for the duration of the call (every call site below
/// passes `&raw mut (*pt()).allocated_cnt`, i.e. a field of the `'static`
/// `PROC_TABLE`). The loop is a lock-free CAS retry, so no additional
/// exclusivity is required against other atomic accessors of the same word.
#[inline]
unsafe fn atomic_inc_unless_i64(p: *mut i64, unless: i64) -> bool {
    // SAFETY: see the fn-level contract above.
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
///
/// SAFETY (caller): see `atomic_inc_unless_i64` (same pointer contract).
#[inline]
unsafe fn atomic_sub_i64(p: *mut i64, n: i64) -> i64 {
    // SAFETY: see the fn-level contract above.
    u! { AtomicI64::from_ptr(p).fetch_sub(n, core::sync::atomic::Ordering::SeqCst) }
}

/// `rcu_assign_pointer(p, v)` — release-store the pointer.
///
/// SAFETY (caller): `p` must be a valid, non-null, properly-aligned pointer
/// to a live `*mut T` for the duration of the call. The store is atomic
/// (release), so it doesn't race destructively with concurrent
/// `rcu_dereference` readers of the same word — that's the point of routing
/// pointer publication through this helper instead of a plain write.
#[inline]
unsafe fn rcu_assign_pointer<T>(p: *mut *mut T, v: *mut T) {
    // SAFETY: see the fn-level contract above.
    u! {
        AtomicPtr::<T>::from_ptr(p).store(v, core::sync::atomic::Ordering::Release);
    }
}

/// `rcu_dereference(p)` — acquire-load the pointer. Caller must be inside a
/// matching rcu_read_lock()/_unlock() region.
///
/// SAFETY (caller): see `rcu_assign_pointer` for the pointer contract; in
/// addition the caller must be inside a matching RCU read-side critical
/// section (or otherwise know the pointee can't be freed concurrently) to
/// safely use the *returned* pointer, though this function's own atomic
/// load only requires `p` itself to be valid.
#[inline]
unsafe fn rcu_dereference<T>(p: *mut *mut T) -> *mut T {
    // SAFETY: see the fn-level contract above.
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

/// Pointer to the file-scope `PROC_TABLE`. Always a valid, non-null,
/// properly-aligned, `'static`-lived pointer — `UnsafeCell::get` never
/// fails — so every `(*pt())` deref below is pointer-valid by construction.
/// What each call site still needs to justify individually is the *access
/// discipline* for the field(s) it touches: plain fields (`nextpid`,
/// `registered_cnt`, `initproc` raw read) require the caller hold
/// `pid_lock` (reader or writer, per field) exactly as the C code did,
/// while `allocated_cnt` and the `initproc`/`procs_list` RCU-published
/// fields go through the atomic/RCU helpers above instead and don't need
/// `pid_lock`.
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
    // SAFETY: see `field_get!` — `a`/`b` are non-null node pointers
    // supplied by the `hlist` implementation, which only ever passes
    // pointers to genuine `thread` objects registered in this proc table
    // (either lookup keys built by `xv6_proctab_get_*`/`_bt_pid`, or live
    // entries already stored in the table).
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

// --- Crate-internal xv6_* entry points (former C-ABI exports, demoted) -----

pub(crate) fn xv6_proctab_init_storage() {
    // SAFETY: see `pt()` for pointer validity. Called exactly once at boot
    // before any other thread can observe `PROC_TABLE`, so no lock is
    // needed for this one-time initialisation (mirrors the C boot sequence).
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

pub(crate) fn xv6_pid_wlock() {
    // SAFETY: see `pt()`. These four are the `pid_lock` primitives
    // themselves; `rwlock_w{,un}lock`/`rwlock_r{,un}lock` handle concurrent
    // access to the lock's own state internally, so no external
    // synchronisation is required to call them beyond `pt()`'s pointer
    // validity and the usual lock/unlock balancing the caller owes.
    u! { rwlock_wlock(&raw mut (*pt()).pid_lock) }
}
pub(crate) fn xv6_pid_wunlock() {
    // SAFETY: see `xv6_pid_wlock`.
    u! { rwlock_wunlock(&raw mut (*pt()).pid_lock) }
}
pub(crate) fn xv6_pid_rlock() {
    // SAFETY: see `xv6_pid_wlock`.
    u! { rwlock_rlock(&raw mut (*pt()).pid_lock) }
}
pub(crate) fn xv6_pid_runlock() {
    // SAFETY: see `xv6_pid_wlock`.
    u! { rwlock_runlock(&raw mut (*pt()).pid_lock) }
}
pub(crate) fn xv6_pid_try_lock_upgrade() -> c_int {
    // SAFETY: see `xv6_pid_wlock`; `__rwl_try_update` is the primitive
    // implementing a read-to-write lock upgrade attempt.
    if u! { __rwl_try_update(&raw mut (*pt()).pid_lock) } {
        1
    } else {
        0
    }
}
pub(crate) fn xv6_pid_wholding() -> c_int {
    // SAFETY: see `xv6_pid_wlock`; `__rwl_w_holding` only inspects the
    // lock's own internally-synchronised state.
    if u! { __rwl_w_holding(&raw mut (*pt()).pid_lock) } {
        1
    } else {
        0
    }
}

pub(crate) fn xv6_proctab_initproc_load() -> *mut thread {
    // SAFETY: see `pt()` and `rcu_dereference`'s fn-level contract; callers
    // of this shim are expected to be inside a matching RCU read-side
    // section before dereferencing the returned pointer, mirroring the C
    // `rcu_dereference(initproc)` usage this replaces.
    u! { rcu_dereference(&raw mut (*pt()).initproc) }
}
pub(crate) fn xv6_proctab_initproc_store(p: *mut thread) {
    // SAFETY: see `pt()` and `rcu_assign_pointer`'s fn-level contract;
    // callers hold `pid_lock` (writer) to serialise against other writers
    // of `initproc`, matching the C convention for this assignment.
    u! { rcu_assign_pointer(&raw mut (*pt()).initproc, p) }
}
pub(crate) fn xv6_proctab_initproc_raw() -> *mut thread {
    // SAFETY: see `pt()`. Plain non-atomic read, used only where the
    // caller has an independent guarantee of no concurrent writer (e.g.
    // single-threaded boot-time access), unlike `xv6_proctab_initproc_load`.
    u! { (*pt()).initproc }
}

pub(crate) fn xv6_proctab_nextpid_get() -> c_int {
    // SAFETY: see `pt()`; `nextpid` is a plain field protected by
    // `pid_lock`, which the caller is expected to hold (matching the C
    // convention for this field).
    u! { (*pt()).nextpid }
}
pub(crate) fn xv6_proctab_nextpid_set(v: c_int) {
    // SAFETY: see `xv6_proctab_nextpid_get`; caller holds `pid_lock` as a
    // writer for this mutation.
    u! {
        (*pt()).nextpid = v;
    }
}
pub(crate) fn xv6_proctab_registered_inc() {
    // SAFETY: see `xv6_proctab_nextpid_get` — `registered_cnt` is likewise
    // a plain field protected by `pid_lock`; caller holds `pid_lock` as
    // a writer for this non-atomic `+= 1`.
    u! {
        (*pt()).registered_cnt += 1;
    }
}
pub(crate) fn xv6_proctab_registered_dec() {
    // SAFETY: see `xv6_proctab_registered_inc`.
    u! {
        (*pt()).registered_cnt -= 1;
    }
}

const EAGAIN: c_int = 11;

pub(crate) fn xv6_proctab_alloc_pid_slot() -> c_int {
    // SAFETY: see `pt()` and `atomic_inc_unless_i64`'s fn-level contract;
    // no `pid_lock` needed since the CAS loop is self-synchronising.
    if u! { atomic_inc_unless_i64(&raw mut (*pt()).allocated_cnt, NR_THREAD) } {
        0
    } else {
        -EAGAIN
    }
}
pub(crate) fn xv6_proctab_free_pid_slot() {
    // SAFETY: see `xv6_proctab_alloc_pid_slot`; `atomic_sub_i64` is
    // likewise self-synchronising.
    u! {
        let _ = atomic_sub_i64(&raw mut (*pt()).allocated_cnt, 1);
    }
}
pub(crate) fn xv6_proctab_allocated_cnt() -> i64 {
    // SAFETY: see `pt()`. This is a plain (non-atomic) read of a counter
    // that `xv6_proctab_alloc_pid_slot`/`_free_pid_slot` mutate atomically;
    // it mirrors the original C code's informational/best-effort read of
    // this counter (e.g. for diagnostics) rather than a correctness-
    // critical decision, so the benign race is intentional, pre-existing
    // behaviour and out of scope for this pass to change.
    u! { (*pt()).allocated_cnt }
}

pub(crate) fn xv6_proctab_get_locked(pid: c_int) -> *mut thread {
    // Pass a `struct thread` lookup key. Bindgen-generated `thread` is `Copy`
    // and large; using `MaybeUninit::zeroed()` keeps the discriminator fields
    // (only `pid`) deterministic and matches the C side's stack-allocated
    // `struct thread dummy = { .pid = pid };`.
    // SAFETY: see `pt()`. `dummy` is a local, fully-owned stack value used
    // only as a lookup key (its address never escapes this call), so
    // zero-initialising the rest of `thread` is fine — `hlist_get` only
    // reads `pid` via the registered `cmp_node`/`hash` callbacks. Caller
    // holds `pid_lock` (reader) around this non-RCU lookup, matching the
    // "_locked" naming and the C convention for `hlist_get`.
    u! {
        let mut dummy: MaybeUninit<thread> = MaybeUninit::zeroed();
        (*dummy.as_mut_ptr()).pid = pid;
        hlist_get(&raw mut (*pt()).procs, dummy.as_mut_ptr() as *mut c_void) as *mut thread
    }
}
pub(crate) fn xv6_proctab_get_rcu(pid: c_int) -> *mut thread {
    // SAFETY: see `xv6_proctab_get_locked`, except this is the RCU-read
    // variant: caller must be inside a matching RCU read-side critical
    // section instead of holding `pid_lock`.
    u! {
        let mut dummy: MaybeUninit<thread> = MaybeUninit::zeroed();
        (*dummy.as_mut_ptr()).pid = pid;
        hlist_get_rcu(&raw mut (*pt()).procs, dummy.as_mut_ptr() as *mut c_void) as *mut thread
    }
}
pub(crate) fn xv6_proctab_put_rcu(p: *mut thread) -> *mut thread {
    // SAFETY: see `pt()`; `p` is a caller-supplied, live `*mut thread`
    // being published into the table. Structural mutation of the hlist
    // requires the caller hold `pid_lock` (writer) even though readers use
    // RCU, matching the C convention for `hlist_put_rcu`.
    u! { hlist_put_rcu(&raw mut (*pt()).procs, p as *mut c_void, false) as *mut thread }
}
pub(crate) fn xv6_proctab_pop_rcu(p: *mut thread) -> *mut thread {
    // SAFETY: see `xv6_proctab_put_rcu`.
    u! { hlist_pop_rcu(&raw mut (*pt()).procs, p as *mut c_void) as *mut thread }
}

// --- procs_list (RCU-safe doubly linked list for procdump iteration) ------

/// `__list_entry_add_rcu(new, prev, next)` — internal helper from list.h.
///
/// SAFETY (caller): `new`, `prev`, and `next` must all be valid, non-null,
/// properly-aligned `*mut list_node_t` (per `list_init`'s pointer
/// contract), with `prev`/`next` genuinely adjacent in the target list, and
/// the caller must hold the lock protecting that list (matching
/// `list_detach_field!`'s contract) — this mutates three nodes' pointers
/// in place, publishing `new` to concurrent RCU readers via
/// `rcu_assign_pointer`.
#[inline]
unsafe fn __list_entry_add_rcu(
    new: *mut list_node_t,
    prev: *mut list_node_t,
    next: *mut list_node_t,
) {
    // SAFETY: see the fn-level contract above.
    u! {
        (*new).next = next;
        (*new).prev = prev;
        rcu_assign_pointer(&raw mut (*prev).next, new);
        (*next).prev = new;
    }
}

/// `list_entry_add_tail_rcu(head, entry)` — insert before `head`.
///
/// SAFETY (caller): see `__list_entry_add_rcu`; `head` and `entry` must be
/// valid, and the caller must hold the list's protecting lock.
#[inline]
unsafe fn list_entry_add_tail_rcu(head: *mut list_node_t, entry: *mut list_node_t) {
    // SAFETY: see the fn-level contract above.
    u! { __list_entry_add_rcu(entry, (*head).prev, head) }
}

/// `list_entry_del_rcu(entry)` — RCU-safe unlink, leaves entry->next
/// untouched so concurrent readers can still traverse it.
///
/// SAFETY (caller): `entry` must be valid per `list_init`'s contract and
/// currently linked into a list; the caller must hold that list's
/// protecting lock against other mutators (concurrent *readers* are fine —
/// that's the point of leaving `entry->next` untouched).
#[inline]
unsafe fn list_entry_del_rcu(entry: *mut list_node_t) {
    // SAFETY: see the fn-level contract above.
    u! {
        let prev = (*entry).prev;
        let next = (*entry).next;
        // WRITE_ONCE(prev->next, next)
        core::ptr::write_volatile(&raw mut (*prev).next, next);
        (*next).prev = prev;
    }
}

/// `list_entry_init_rcu(entry)` — reset to self-pointing with release publish.
///
/// SAFETY (caller): see `list_init`'s pointer contract for `entry`.
#[inline]
unsafe fn list_entry_init_rcu(entry: *mut list_node_t) {
    // SAFETY: see the fn-level contract above.
    u! {
        rcu_assign_pointer(&raw mut (*entry).next, entry);
        (*entry).prev = entry;
    }
}

/// `list_entry_del_init_rcu(entry)` — unlink + reinit.
///
/// SAFETY (caller): see `list_entry_del_rcu` (the stricter of the two
/// component contracts — protecting-lock held, `entry` linked and valid).
#[inline]
unsafe fn list_entry_del_init_rcu(entry: *mut list_node_t) {
    // SAFETY: see the fn-level contract above.
    u! {
        list_entry_del_rcu(entry);
        list_entry_init_rcu(entry);
    }
}

pub(crate) fn xv6_proctab_dmplist_add(p: *mut thread) {
    // SAFETY: see `pt()` and `list_entry_add_tail_rcu`'s fn-level contract;
    // `p` is a caller-supplied, live `*mut thread` not yet in the dump
    // list; caller holds `pid_lock` (writer) around this structural
    // mutation of `procs_list`.
    u! { list_entry_add_tail_rcu(&raw mut (*pt()).procs_list, &raw mut (*p).dmp_list_entry) }
}
pub(crate) fn xv6_proctab_dmplist_del(p: *mut thread) {
    // SAFETY: see `xv6_proctab_dmplist_add`; `p` is currently linked into
    // `procs_list`.
    u! { list_entry_del_init_rcu(&raw mut (*p).dmp_list_entry) }
}

// --- proc_table RCU-guarded iteration --------------------------------------
//
// Native Rust reimplementation of the (deleted) C bridge's
// `xv6_proctab_foreach_rcu`/`xv6_proctab_foreach_inner`, which drove the
// `hlist_foreach_node_rcu(hlist, pos, proctab_entry)` macro from
// kernel/inc/hlist.h. That macro expands to `hlist_first_entry_rcu` +
// `hlist_next_entry_rcu` (both `static inline`, so bindgen cannot see
// them); the two helpers below mirror their exact semantics:
//
//   * each bucket is a sentinel-headed circular doubly-linked list (the
//     bucket itself is a `list_node_t`, never counted as an element);
//   * `next`/`prev` pointer loads go through `rcu_dereference` (an
//     acquire load), matching `list_next_rcu()`, so this is safe to run
//     concurrently with the `hlist_put_rcu`/`hlist_pop_rcu` writers above
//     as long as the caller holds an RCU read-side critical section;
//   * an exhausted bucket falls through to the next non-empty one.
//
// `hlist_entry_t.list_entry` sits at offset 0 (see kernel/inc/hlist_type.h),
// so reinterpreting a `*mut list_node_t` as `*mut hlist_entry_t` is exactly
// what the C `container_of(..., hlist_entry_t, list_entry)` does; recovering
// the owning `*mut thread` reuses `proctab_hash_get_node` (SECTION 8, above).

/// First entry in `bucket` (RCU-safe), or NULL if the bucket is empty.
///
/// SAFETY (caller): `bucket` must be a valid, non-null pointer into
/// `PROC_TABLE.buckets` (every call site passes `&raw mut
/// (*pt()).buckets[i]` for `i < NR_THREAD_HASH_BUCKETS`); the caller must
/// be inside a matching RCU read-side critical section for the returned
/// pointer to be safe to dereference afterwards (see `rcu_dereference`).
/// Reinterpreting the `*mut list_node_t` as `*mut hlist_entry_t` is valid
/// because `hlist_entry_t.list_entry` sits at offset 0 (module doc above).
#[inline]
unsafe fn hlist_bucket_first_rcu(bucket: *mut hlist_bucket_t) -> *mut hlist_entry_t {
    // SAFETY: see the fn-level contract above.
    u! {
        let first = rcu_dereference(&raw mut (*bucket).next);
        if first.is_null() || first == bucket {
            ptr::null_mut()
        } else {
            first as *mut hlist_entry_t
        }
    }
}

/// Next entry within `bucket` after `entry` (RCU-safe), or NULL at the
/// bucket's sentinel head (i.e. `entry` was the last one).
///
/// SAFETY (caller): see `hlist_bucket_first_rcu` for `bucket`; `entry` must
/// additionally be a valid, non-null `*mut hlist_entry_t` currently linked
/// into `*bucket` (i.e. previously returned by `hlist_bucket_first_rcu` or
/// this function for the same bucket).
#[inline]
unsafe fn hlist_bucket_next_rcu(
    bucket: *mut hlist_bucket_t,
    entry: *mut hlist_entry_t,
) -> *mut hlist_entry_t {
    // SAFETY: see the fn-level contract above.
    u! {
        let next = rcu_dereference(&raw mut (*entry).list_entry.next);
        if next.is_null() || next == bucket {
            ptr::null_mut()
        } else {
            next as *mut hlist_entry_t
        }
    }
}

/// RCU-guarded iterator over every thread currently registered in the
/// proc table. Only constructed by [`for_each_proctab_thread`], which
/// pins the iterator's lifetime to a single `rcu_read_lock`/`_unlock`
/// critical section.
struct ProcTableIter<'a> {
    _guard: &'a crate::lock::rcu::KRcuRead,
    bucket_idx: usize,
    cur: *mut hlist_entry_t,
}

impl<'a> ProcTableIter<'a> {
    fn new(guard: &'a crate::lock::rcu::KRcuRead) -> Self {
        let mut bucket_idx = 0usize;
        let mut cur = ptr::null_mut();
        // SAFETY: see `pt()` and `hlist_bucket_first_rcu`'s fn-level
        // contract; `guard` (the `&KRcuRead` borrow taken by this fn)
        // proves the caller is inside the RCU read-side critical section
        // this iteration requires.
        u! {
            while bucket_idx < NR_THREAD_HASH_BUCKETS {
                cur = hlist_bucket_first_rcu(&raw mut (*pt()).buckets[bucket_idx]);
                if !cur.is_null() {
                    break;
                }
                bucket_idx += 1;
            }
        }
        Self { _guard: guard, bucket_idx, cur }
    }
}

impl<'a> Iterator for ProcTableIter<'a> {
    type Item = *mut thread;

    fn next(&mut self) -> Option<*mut thread> {
        if self.cur.is_null() {
            return None;
        }
        let entry = self.cur;
        // SAFETY: see `ProcTableIter::new`; `self._guard` (held for the
        // lifetime of `self`) proves we're still inside the RCU read-side
        // critical section, and `entry`/`self.bucket_idx` were produced by
        // a previous call into this same bucket-walking machinery.
        u! {
            let mut nxt = hlist_bucket_next_rcu(&raw mut (*pt()).buckets[self.bucket_idx], entry);
            while nxt.is_null() {
                self.bucket_idx += 1;
                if self.bucket_idx >= NR_THREAD_HASH_BUCKETS {
                    break;
                }
                nxt = hlist_bucket_first_rcu(&raw mut (*pt()).buckets[self.bucket_idx]);
            }
            self.cur = nxt;
            Some(proctab_hash_get_node(entry) as *mut thread)
        }
    }
}

/// Runs `f` once for every thread currently registered in the proc
/// table, inside a single RCU read-side critical section.
///
/// This is the safe, idiomatic replacement for the deleted C
/// `xv6_proctab_foreach_rcu(void (*cb)(struct thread *, void *), void
/// *arg)`: instead of a raw function-pointer + `void*` pair, callers pass
/// an ordinary closure and get RCU protection for free (acquired here,
/// released when this function returns).
///
/// `f` receives a raw `*mut thread`; dereferencing it is subject to the
/// usual RCU-reader contract — the thread may be concurrently unlinked
/// from the table, but its storage is guaranteed to stay valid at least
/// until the end of this critical section (i.e. for the duration of the
/// call to `f`).
pub fn for_each_proctab_thread(mut f: impl FnMut(*mut thread)) {
    let guard = crate::lock::rcu::KRcuRead::new();
    for t in ProcTableIter::new(&guard) {
        f(t);
    }
}

// ===========================================================================
// SECTION 11 leftover: xv6_t_copy_name (was C — safestrcpy of thread.name)
// ===========================================================================

/// Copy `src->name` into `dst->name` with the same bounds-check semantics
/// as the C `safestrcpy`: copies at most `len-1` non-NUL bytes, then writes
/// a terminating NUL. `len` is the fixed size of `thread.name` (16).
pub(crate) fn xv6_t_copy_name(dst: *mut thread, src: *mut thread) {
    let dn = field_ptr_mut!(dst, name) as *mut u8;
    let sn = field_ptr_const!(src, name) as *const u8;
    let n = 16usize;
    let mut i = 0usize;
    while i + 1 < n {
        // SAFETY: `sn` points into `src`'s fixed `[c_char; 16]` `name`
        // array (valid per `field_ptr_const!`); the loop guard `i + 1 < n`
        // keeps `i <= n - 2`, so `sn.add(i)` stays in bounds.
        let c = u! { *sn.add(i) };
        if c == 0 { break; }
        // SAFETY: see above (same bound on `i`), for `dn` into `dst`'s
        // `name` array (valid per `field_ptr_mut!`).
        u! { *dn.add(i) = c; }
        i += 1;
    }
    // SAFETY: `i <= n - 1` here (either it stopped at `i + 1 == n`, i.e.
    // `i == n - 1`, or broke out earlier with a smaller `i`), so
    // `dn.add(i)` is still in bounds of the 16-byte array.
    u! { *dn.add(i) = 0; }
}

// ===========================================================================
// SECTION 10: procdump print helpers — PORTED from proc_rust_shims.c.
// Uses the kernel `printf` (variadic, bindgen-exposed). Recursion in
// xv6_procdump_tree_recursive is straightforward Rust recursion.
// ===========================================================================

use crate::bindings::{pgroup as Pgroup, session as Session, thread_group as Tgroup};

// `thread_is_group_leader` used to be bridged via the `xv6_tgport_*`
// C-ABI alias layer; now a direct crate-path call to the real (already
// Rust) definition in `thread_group.rs`. Its real signature returns
// `bool`, not `c_int` -- the old `extern "C"` redeclaration here had
// drifted from the truth (same declaration-mismatch class as the
// `thread_group_exit -> !` finding in `sysproc.rs`), harmless in practice
// only because both are 0/1 in the return register; call sites below are
// updated to use the `bool` result directly instead of `!= 0`.
use crate::proc::thread_is_group_leader;

// P3-1D mesh sweep: backtrace.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use crate::backtrace::print_thread_backtrace;

// Variadic printf alias for ergonomics (bindgen already exports it; we
// re-shape the first arg as a *const c_char for use with `c"..."` literals).

/// SAFETY (caller): `p` must be a valid, non-null, live `*mut thread` —
/// every helper below is only ever called (directly or transitively) from
/// the `xv6_procdump_*`/`xv6_dump_session` entry points on a `p` obtained
/// from proc-table iteration or a caller-checked lookup, generally while
/// `tcb_lock(p)` is held for the fields it protects.
#[inline]
unsafe fn t_name_ptr(p: *mut thread) -> *const c_char {
    // SAFETY: see the fn-level contract above.
    u! { &raw const (*p).name as *const c_char }
}
/// SAFETY (caller): see `t_name_ptr`.
#[inline]
unsafe fn s10_t_user_space(p: *mut thread) -> bool {
    // THREAD_FLAG_USER_SPACE = 5
    // SAFETY: see the fn-level contract above. Plain (non-atomic) read of
    // `flags`, matching the call sites' expectation that `tcb_lock` (or
    // equivalent) is held.
    u! { ((*p).flags & (1u64 << 5)) != 0 }
}
/// SAFETY (caller): see `t_name_ptr`; additionally `p->sched_entity` must
/// be non-null, which holds for every live thread (set at creation time).
#[inline]
unsafe fn se_on_cpu(p: *mut thread) -> bool {
    use core::sync::atomic::{AtomicI32, Ordering};
    // SAFETY: see the fn-level contract above.
    u! {
        let pse = (*p).sched_entity;
        AtomicI32::from_ptr(&raw mut (*pse).on_cpu).load(Ordering::Acquire) != 0
    }
}
/// SAFETY (caller): see `se_on_cpu`.
#[inline]
unsafe fn se_cpu_id(p: *mut thread) -> c_int {
    // SAFETY: see the fn-level contract above.
    u! { (*(*p).sched_entity).cpu_id }
}
/// SAFETY (caller): see `t_name_ptr`.
#[inline]
unsafe fn t_state_load(p: *mut thread) -> c_int {
    // Mirror __thread_state_get: smp_load_acquire on thread.state.
    use core::sync::atomic::{AtomicU32, Ordering};
    // SAFETY: see the fn-level contract above. Atomic acquire load, so no
    // additional lock is required for this particular read.
    u! {
        AtomicU32::from_ptr(&raw mut (*p).state as *mut u32).load(Ordering::Acquire) as c_int
    }
}
/// SAFETY (caller): `p` must be a valid, non-null, live `*mut Tgroup`, and
/// `off` must be the byte offset of a 4-byte-aligned `i32`-sized atomic
/// field within `Tgroup` — every call site below passes a genuine
/// `core::mem::offset_of!(Tgroup, ...)` constant for `live_threads`,
/// `refcount`, or `group_exit`, never an arbitrary value.
#[inline]
unsafe fn tg_load_int(p: *mut Tgroup, off: usize) -> c_int {
    use core::sync::atomic::{AtomicI32, Ordering};
    // SAFETY: see the fn-level contract above.
    u! {
        let base = p as *mut u8;
        AtomicI32::from_ptr(base.add(off) as *mut i32).load(Ordering::Acquire)
    }
}

/// safestrcpy reimplementation, returns count written excluding NUL.
///
/// SAFETY (caller): `src` must be a valid pointer to a NUL-terminated
/// C string readable for at least `min(strlen(src), dst.len() - 1) + 1`
/// bytes — every call site passes a `t_name_ptr(p)` result, i.e. a pointer
/// into a live thread's fixed-size, always-NUL-terminated `name` array.
#[inline]
unsafe fn safestr(dst: &mut [u8], src: *const c_char) -> usize {
    let n = dst.len();
    if n == 0 { return 0; }
    let mut i = 0usize;
    while i + 1 < n {
        // SAFETY: see the fn-level contract above; `i` stays `< n - 1 <=
        // src`'s guaranteed-readable prefix length.
        let c = u! { *(src.add(i) as *const u8) };
        if c == 0 { break; }
        dst[i] = c;
        i += 1;
    }
    dst[i] = 0;
    i
}

pub(crate) fn xv6_procdump_header() {
    crate::kprintln!(
        "{:<20} {:<5} {:<2} {:<3} {}",
        "SID:PGID:TGID:TID",
        "CPU",
        "ST",
        "U/K",
        "COMMAND",
    );
}

pub(crate) fn xv6_procdump_bt_header() {
    crate::kprintln!("\n=== Blocked Process Backtraces ===");
}
pub(crate) fn xv6_procdump_bt_footer() {
    crate::kprintln!("\n=== End Backtraces ===");
}

pub(crate) fn xv6_procdump_one(p: *mut thread) -> c_int {
    // SAFETY: `p` is a caller-supplied, live `*mut thread` (from proc-table
    // iteration). `tcb_lock(p)`/`tcb_unlock(p)` bracket every access
    // to `p`'s (and `p->parent`'s) fields below, matching the C locking
    // convention for reading a thread's identity/name fields; `p->parent`
    // is null-checked before deref. Every `k_printf` format string is a
    // fixed 'static literal hand-verified against its argument list (see
    // `xv6_procdump_header`).
    u! {
        let mut name = [0u8; 16];
        let mut pname = [0u8; 16];

        tcb_lock(p);
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
        tcb_unlock(p);

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

        let ustr = if s10_t_user_space(p) { "U" } else { "K" };
        crate::kprintln!(
            "{:<20} {:<5} {:<2} [{}] {}/{}",
            crate::printf::Cs(idbuf.as_ptr() as *const c_char),
            crate::printf::Cs(cpubuf.as_ptr() as *const c_char),
            crate::printf::Cs(xv6_thread_state_short(pstate)),
            ustr,
            crate::printf::Cs(pname.as_ptr() as *const c_char),
            crate::printf::Cs(name.as_ptr() as *const c_char),
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

pub(crate) fn xv6_procdump_bt_one(p: *mut thread) {
    // SAFETY: see `xv6_procdump_one`. `p->sched_entity` is non-null for a
    // live thread (see `se_on_cpu`'s contract); `print_thread_backtrace` is
    // an extern C helper given the thread's own `context`/`kstack`/
    // `kstack_order`, matching the C call site's arguments.
    u! {
        let mut name = [0u8; 16];
        tcb_lock(p);
        let pstate = t_state_load(p);
        let pid = (*p).pid;
        let tgid = (*p).tgid;
        let pgid_v = (*p).pgid;
        let sid_v = (*p).sid;
        safestr(&mut name, t_name_ptr(p));

        // INTERRUPTIBLE=2, UNINTERRUPTIBLE=6
        if pstate == 2 || pstate == 6 {
            let stype = if pstate == 2 { "interruptible" } else { "uninterruptible" };
            if se_on_cpu(p) {
                crate::kprintln!(
                    "\n--- {}:{}:{}:{} [{}] {} --- (on CPU, cannot backtrace)",
                    sid_v, pgid_v, tgid, pid, stype, crate::printf::Cs(name.as_ptr() as *const c_char),
                );
            } else {
                crate::kprintln!(
                    "\n--- {}:{}:{}:{} [{}] {} ---",
                    sid_v, pgid_v, tgid, pid, stype, crate::printf::Cs(name.as_ptr() as *const c_char),
                );
                let pse = (*p).sched_entity;
                print_thread_backtrace(&raw mut (*pse).context, (*p).kstack, (*p).kstack_order);
            }
        }
        tcb_unlock(p);
    }
}

pub(crate) fn xv6_procdump_bt_pid(pid: c_int) {
    use core::mem::MaybeUninit;
    // SAFETY: see `pt()`, `xv6_proctab_get_rcu` (same lookup pattern, under
    // `_rcu`'s RCU read-side section), and `xv6_procdump_one` for the
    // `p`-field/`tcb_lock`/`print_thread_backtrace` reasoning once `p` is
    // found non-null.
    u! {
        let _rcu = crate::lock::rcu::KRcuRead::new();
        let mut dummy: MaybeUninit<thread> = MaybeUninit::zeroed();
        (*dummy.as_mut_ptr()).pid = pid;
        let p = hlist_get_rcu(&raw mut (*pt()).procs, dummy.as_mut_ptr() as *mut c_void) as *mut thread;
        if p.is_null() {
            crate::kprintln!("Process {} not found", pid);
            return;
        }
        tcb_lock(p);
        let pstate = t_state_load(p);
        let mut name = [0u8; 16];
        safestr(&mut name, t_name_ptr(p));
        let tgid = (*p).tgid;
        let pgid_v = (*p).pgid;
        let sid_v = (*p).sid;
        crate::kprintln!(
            "\n--- {}:{}:{}:{} [{}] {} ---",
            sid_v, pgid_v, tgid, pid,
            crate::printf::Cs(xv6_thread_state_short(pstate)),
            crate::printf::Cs(name.as_ptr() as *const c_char),
        );
        if se_on_cpu(p) {
            crate::kprintln!("Process is currently on a CPU, context not saved");
        } else if pstate == 0 {
            // THREAD_UNUSED
            crate::kprintln!("Process is {}, no valid context",
                     crate::printf::Cs(xv6_thread_state_to_str(pstate)));
        } else {
            let pse = (*p).sched_entity;
            print_thread_backtrace(&raw mut (*pse).context, (*p).kstack, (*p).kstack_order);
        }
        tcb_unlock(p);
    }
}

pub(crate) fn xv6_procdump_tree_node(p: *mut thread, depth: c_int) {
    // SAFETY: `p` is a caller-supplied, live `*mut thread` (root of a
    // debug tree dump). Unlike `xv6_procdump_one`/`_bt_one`, this reads
    // `p`'s fields without `tcb_lock`, matching the original C tree-dump
    // helper's best-effort (lock-free) debug-print semantics — the caller
    // is expected to invoke this only where that's an acceptable trade-off
    // (e.g. panic/diagnostic dumps), not a correctness change made here.
    // `k_printf` format strings are fixed 'static literals hand-verified
    // against their argument lists (see `xv6_procdump_header`).
    u! {
        let mut i = 0;
        while i < depth { crate::kprint!("  "); i += 1; }
        if depth > 0 { crate::kprint!("└─ "); }
        let pstate = t_state_load(p);
        let pid = (*p).pid;
        let tgid = (*p).tgid;
        let pgid_v = (*p).pgid;
        let sid_v = (*p).sid;
        let mut name = [0u8; 16];
        safestr(&mut name, t_name_ptr(p));
        let ustr = if s10_t_user_space(p) { "U" } else { "K" };
        crate::kprint!(
            "{}:{}:{}:{} {} [{}] {}",
            sid_v, pgid_v, tgid, pid,
            crate::printf::Cs(xv6_thread_state_short(pstate)),
            ustr,
            crate::printf::Cs(name.as_ptr() as *const c_char),
        );
        if se_on_cpu(p) {
            crate::kprintln!(" (CPU: {})", se_cpu_id(p));
        } else {
            crate::kprintln!();
        }
    }
}

pub(crate) fn xv6_procdump_tree_recursive(p: *mut thread, depth: c_int) {
    // SAFETY: see `xv6_procdump_tree_node`; `sib_off` is a genuine
    // `offset_of!` for `thread::siblings`, matching `list_foreach_safe`'s
    // contract. Recursion depth is bounded by the actual process tree
    // depth, same as the C recursive implementation this replaces.
    u! {
        xv6_procdump_tree_node(p, depth);
        // Walk children list (member: siblings).
        let sib_off = core::mem::offset_of!(thread, siblings);
        list_foreach_safe::<thread>(&raw mut (*p).children, sib_off, |child| {
            xv6_procdump_tree_recursive(child, depth + 1);
        });
    }
}

pub(crate) fn xv6_dump_session(s: *mut Session) {
    // SAFETY: `s` is a caller-supplied, live `*mut Session`. Each nested
    // `list_foreach_safe` walk uses a genuine `offset_of!` for the
    // relevant embedded `list_node_t` (`Pgroup::list_entry`,
    // `Tgroup::list_entry`, `thread::tg_entry`), matching its contract;
    // `pg`/`tg`/`t` are therefore live objects reached only through those
    // walks. `tg_load_int` is given genuine `offset_of!` constants for
    // `Tgroup`'s atomic counter fields (see its own fn-level contract).
    // `k_printf` format strings are fixed 'static literals hand-verified
    // against their argument lists (see `xv6_procdump_header`). This is a
    // best-effort, lock-free debug dump, same trade-off as
    // `xv6_procdump_tree_node`.
    u! {
        let fg_note = if (*s).fg_pgrp.is_null() { ", no fg" } else { "" };
        crate::kprintln!(
            "\nSession {}  (threads={}, pgroups={}{})",
            (*s).sid, (*s).t_cnt, (*s).pg_cnt, fg_note,
        );

        let pg_off = core::mem::offset_of!(Pgroup, list_entry);
        list_foreach_safe::<Pgroup>(&raw mut (*s).pgrps, pg_off, |pg| {
            let fg = if (*s).fg_pgrp == pg { " [fg]" } else { "" };
            let exited = if (*pg).__bindgen_anon_1.exited() != 0 { ", exited" } else { "" };
            crate::kprintln!(
                "  PGroup {}{}  (threads={}, tgroups={}{})",
                (*pg).pgid, fg, (*pg).t_cnt, (*pg).p_cnt, exited,
            );

            let tg_off = core::mem::offset_of!(Tgroup, list_entry);
            list_foreach_safe::<Tgroup>(&raw mut (*pg).thread_groups, tg_off, |tg| {
                let live = tg_load_int(tg, core::mem::offset_of!(Tgroup, live_threads));
                let refc = tg_load_int(tg, core::mem::offset_of!(Tgroup, refcount));
                let gex  = tg_load_int(tg, core::mem::offset_of!(Tgroup, group_exit));
                let gex_note = if gex != 0 { ", exiting" } else { "" };
                crate::kprintln!(
                    "    Process {}  (live={}, refs={}{})",
                    (*tg).tgid, live, refc, gex_note,
                );

                let tge_off = core::mem::offset_of!(thread, tg_entry);
                list_foreach_safe::<thread>(&raw mut (*tg).thread_list, tge_off, |t| {
                    let st = xv6_thread_state_to_str(t_state_load(t));
                    let on_cpu = se_on_cpu(t);
                    let ustr = if s10_t_user_space(t) { "U" } else { "K" };
                    let leader = if thread_is_group_leader(t) { " (leader)" } else { "" };
                    let cpu_n = if on_cpu { " *cpu" } else { "" };
                    crate::kprintln!(
                        "      tid {:<4} [{}] {:<2} {}{}{}",
                        (*t).pid, ustr, crate::printf::Cs(st),
                        crate::printf::Cs(t_name_ptr(t)),
                        leader, cpu_n,
                    );
                });
            });
        });
    }
}

// P3-2 (zero-C wave): the `xv6_print_str`/`xv6_print_d`/`xv6_print_str_d`
// printf trampolines that used to live here were deleted -- their only
// caller (`proc/pid.rs`'s `procdump_*` family) now calls `kprintln!`/
// `kprint!` directly with its own literal format strings. `xv6_print_str_d`
// in particular took its *format string* as a runtime argument (the
// caller's message text embedded a `%d`), which has no equivalent under
// `core::fmt` (format strings must be literals) -- the fix was to give each
// `pid.rs` call site its own literal, not to preserve a dynamic-format
// trampoline.

// ==========================================================================
// SECTION 6: sigacts / ksiginfo / thread_signal scalar accessors.
// Wraps plain scalar/pointer fields of these structs for use by the safe
// access.rs wrappers consumed from signal.rs.
// ==========================================================================
use crate::bindings::sigacts_t as sa_sigacts_t;
use crate::bindings::ksiginfo as sa_ksiginfo;

pub(crate) fn sa_refcount(s: *mut sa_sigacts_t) -> c_int { field_get!(s, refcount) }
pub(crate) fn sa_set_refcount(s: *mut sa_sigacts_t, v: c_int) { field_set!(s, refcount, v) }
pub(crate) fn sa_sigterm(s: *mut sa_sigacts_t) -> u64 { field_get!(s, sa_sigterm) }
pub(crate) fn sa_set_sigterm(s: *mut sa_sigacts_t, v: u64) { field_set!(s, sa_sigterm, v) }
pub(crate) fn sa_sigstop(s: *mut sa_sigacts_t) -> u64 { field_get!(s, sa_sigstop) }
pub(crate) fn sa_set_sigstop(s: *mut sa_sigacts_t, v: u64) { field_set!(s, sa_sigstop, v) }
pub(crate) fn sa_sigcont(s: *mut sa_sigacts_t) -> u64 { field_get!(s, sa_sigcont) }
pub(crate) fn sa_set_sigcont(s: *mut sa_sigacts_t, v: u64) { field_set!(s, sa_sigcont, v) }
pub(crate) fn sa_sigignore(s: *mut sa_sigacts_t) -> u64 { field_get!(s, sa_sigignore) }
pub(crate) fn sa_set_sigignore(s: *mut sa_sigacts_t, v: u64) { field_set!(s, sa_sigignore, v) }

pub(crate) fn ksi_signo(k: *mut sa_ksiginfo) -> c_int { field_get!(k, signo) }
pub(crate) fn ksi_set_signo(k: *mut sa_ksiginfo, v: c_int) { field_set!(k, signo, v) }
pub(crate) fn ksi_receiver(k: *mut sa_ksiginfo) -> *mut thread { field_get!(k, receiver) }
pub(crate) fn ksi_set_receiver(k: *mut sa_ksiginfo, v: *mut thread) { field_set!(k, receiver, v) }
pub(crate) fn ksi_sender(k: *mut sa_ksiginfo) -> *mut thread { field_get!(k, sender) }
pub(crate) fn ksi_set_sender(k: *mut sa_ksiginfo, v: *mut thread) { field_set!(k, sender, v) }

pub(crate) fn ts_sig_mask(p: *mut thread) -> u64 { field_get!(p, signal.sig_mask) }
pub(crate) fn ts_set_sig_mask(p: *mut thread, v: u64) { field_set!(p, signal.sig_mask, v) }
pub(crate) fn ts_sig_saved_mask(p: *mut thread) -> u64 { field_get!(p, signal.sig_saved_mask) }
pub(crate) fn ts_set_sig_saved_mask(p: *mut thread, v: u64) { field_set!(p, signal.sig_saved_mask, v) }
pub(crate) fn ts_sig_pending_mask(p: *mut thread) -> u64 { field_get!(p, signal.sig_pending_mask) }
pub(crate) fn ts_set_sig_pending_mask(p: *mut thread, v: u64) { field_set!(p, signal.sig_pending_mask, v) }
pub(crate) fn ts_sig_ucontext(p: *mut thread) -> u64 { field_get!(p, signal.sig_ucontext) }
pub(crate) fn ts_set_sig_ucontext(p: *mut thread, v: u64) { field_set!(p, signal.sig_ucontext, v) }
pub(crate) fn ts_esignal(p: *mut thread) -> u64 { field_get!(p, signal.esignal) }
pub(crate) fn ts_set_esignal(p: *mut thread, v: u64) { field_set!(p, signal.esignal, v) }
pub(crate) fn ts_stop_signal(p: *mut thread) -> c_int { field_get!(p, signal.stop_signal) }
pub(crate) fn ts_set_stop_signal(p: *mut thread, v: c_int) { field_set!(p, signal.stop_signal, v) }
pub(crate) fn ts_term_signal(p: *mut thread) -> c_int { field_get!(p, signal.term_signal) }
pub(crate) fn ts_set_term_signal(p: *mut thread, v: c_int) { field_set!(p, signal.term_signal, v) }
