//! Pure-Rust port of `kernel/proc/thread_queue.c` (SECTION 16).
//!
//! Provides list-based (`tq_t`) and red-black-tree-based (`ttree_t`)
//! wait-queue primitives used throughout the kernel for sleep/wakeup.
//!
//! The canonical entry points (`tq_init`, `tq_wait`, `ttree_wakeup_key`,
//! ...) are crate-internal Rust fns called directly by sibling Rust
//! modules via `crate::proc::*` paths -- the backwards-compat
//! `xv6_tqport_*` alias layer that used to front these was collapsed in
//! the P3-1B2 sweep, and the `#[no_mangle] extern "C"` export surface
//! itself was dismantled in P3-D2a (dead exports deleted).

#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::ptr::null_mut;
use core::sync::atomic::{fence, Ordering};

use crate::bindings::{
    rb_node, rb_root, rb_root_opts, sleep_callback_t, spinlock as spinlock_t,
    thread, tnode as tnode_t, tq as tq_t, ttree as ttree_t, uint64, wakeup_callback_t,
};
use crate::lock::spinlock::spin_sleep_cb;
use crate::lock::spinlock::spin_wake_cb;
use crate::machine::{intr_off_save, intr_restore};
use crate::proc::access::{
    err_ptr_errno as err_ptr, is_err, is_err_or_null, ptr_err_errno as ptr_err,
    tnode_from_list_entry, tnode_from_tree_entry, write_out, ListNodeRef, TnodeRef, TqRef,
    TtreeRef, zero_tnode_ptr, zeroed_tnode,
};
use crate::list::ListNode;
use crate::proc::proc_shims::{xv6_current_thread, xv6_panic};

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3D, nativized in Wave P3-N2.
//
// `Tq`/`Ttree`/`Tnode` ARE the kernel-wide Rust definitions of
// `kernel/inc/proc/tq_type.h`'s `tq_t`/`ttree_t`/`tnode_t` now:
// `build.rs` blocklists the bindgen-generated forms and injects
// `pub use crate::proc::thread_queue::Tq as tq_t;` (etc.) raw-line
// re-exports, so every remaining bindgen struct that embeds a thread
// queue by value (`workqueue.idle_queue`, `rwsem.read_queue`, ...) and
// every `crate::bindings::{tq_t,ttree_t,tnode_t}` path across the
// crate resolves here. `tq_type_t` (a plain C enum typedef nothing on
// the Rust side reads) is redirected to a bare `c_uint` alias, exactly
// what bindgen's constified-enum lowering produced.
//
// All three are naturally aligned (align 8) -- none embeds a lock *by
// value* (only `*mut spinlock_t` pointers), so (unlike `workqueue`,
// which embeds `spinlock_t`/`tq_t` by value and picks up
// `#[repr(align(64))]`) there is no alignment surprise here; proven
// directly below, not assumed.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Tq {
    pub(crate) head: ListNode,
    pub(crate) counter: c_int,
    pub(crate) name: *const c_char,
    pub(crate) lock: *mut spinlock_t,
    pub(crate) flags: u64,
}

// P3-N2 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct tq
// { head: list_node_t, counter: c_int, name: *const c_char, lock:
// *mut spinlock_t, flags: uint64 }`) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe against
// `kernel/inc/proc/tq_type.h` (size 48, align 8, offsets 0/16/24/32/40).
const _: () = {
    assert!(core::mem::size_of::<Tq>() == 48, "tq_t size");
    assert!(core::mem::align_of::<Tq>() == 8, "tq_t natural alignment");
    assert!(core::mem::offset_of!(Tq, head) == 0, "tq.head offset");
    assert!(core::mem::offset_of!(Tq, counter) == 16, "tq.counter offset");
    assert!(core::mem::offset_of!(Tq, name) == 24, "tq.name offset");
    assert!(core::mem::offset_of!(Tq, lock) == 32, "tq.lock offset");
    assert!(core::mem::offset_of!(Tq, flags) == 40, "tq.flags offset");
};

/// `root`'s field type is `crate::bindings::rb_root` itself, not a
/// re-wrapped mirror: `bintree.rs`'s own P3-3C convention keeps
/// `RbRoot = rb_root` as the *working* type (its `RawRbRoot` is a
/// separate, deliberately-unwired compile-time layout proof) -- so
/// embedding `rb_root` here already **is** the native form, with zero
/// benefit to introducing a second parallel type.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Ttree {
    pub(crate) root: rb_root,
    pub(crate) counter: c_int,
    pub(crate) name: *const c_char,
    pub(crate) lock: *mut spinlock_t,
    pub(crate) flags: u64,
}

// P3-N2 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (`pub struct ttree { root: rb_root,
// counter: c_int, name: *const c_char, lock: *mut spinlock_t, flags:
// uint64 }`; `rb_root` is 16/8) and independently confirmed by the
// cross-compiler `_Static_assert` probe against
// `kernel/inc/proc/tq_type.h` (size 48, align 8, offsets 0/16/24/32/40).
// NOTE: the bindgen `ttree` derived no Copy/Clone (a bindgen
// derive-analysis quirk around the blocklisted `rb_root` member); this
// native has always derived both. Nothing in the remaining bindgen
// output embeds `ttree_t` by value (verified), so the difference is
// unobservable there, and `build.rs`'s `NativeTypeCallbacks` now
// answers Copy=Yes for it, matching this real definition.
const _: () = {
    assert!(core::mem::size_of::<Ttree>() == 48, "ttree_t size");
    assert!(core::mem::align_of::<Ttree>() == 8, "ttree_t natural alignment");
    assert!(core::mem::offset_of!(Ttree, root) == 0, "ttree.root offset");
    assert!(core::mem::offset_of!(Ttree, counter) == 16, "ttree.counter offset");
    assert!(core::mem::offset_of!(Ttree, name) == 24, "ttree.name offset");
    assert!(core::mem::offset_of!(Ttree, lock) == 32, "ttree.lock offset");
    assert!(core::mem::offset_of!(Ttree, flags) == 40, "ttree.flags offset");
};

// `tnode_t`'s union is the one genuinely tricky part of this family: the
// C `union { struct { list_node_t entry; tq_t *queue; } list; struct {
// rb_node entry; ttree_t *queue; uint64 key; } tree; }` is a real
// Rust `union` of two `#[repr(C)]` arm structs below.
//
// P3-N2 representation note: the pre-nativization bindgen output did
// NOT emit a native Rust union here — because the arms directly embed
// the P3-N1-blocklisted `list_node_t`/`rb_node`, bindgen degraded the
// anonymous union to its `__BindgenUnionField` blob form
// (`tnode__bindgen_ty_1 { list: __BindgenUnionField<...>, tree:
// __BindgenUnionField<...>, bindgen_union_field: [u64; 5] }`) — the
// exact "known side effect" recorded in P3-N1's commit message. That
// blob is layout-identical to this real union (both are 40 bytes,
// align 8, every member at offset 0): the C ground truth was
// re-proven for this wave with a riscv64-unknown-elf-gcc
// `_Static_assert` probe against `kernel/inc/proc/tq_type.h` —
// offsetof(tnode_t, list.entry)==8, list.queue==24, tree.entry==8,
// tree.queue==32, tree.key==40, sizeof(list arm)==24, sizeof(tree
// arm)==40 — matching the hardcoded interior offsets asserted below.
// No Rust code ever touched the `__BindgenUnionField` accessors (this
// module and `access.rs` have always gone through this native union),
// so retiring the blob changes zero access code.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct TnodeListArm {
    pub(crate) entry: ListNode,
    pub(crate) queue: *mut tq_t,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct TnodeTreeArm {
    pub(crate) entry: rb_node,
    pub(crate) queue: *mut ttree_t,
    pub(crate) key: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union TnodeUnion {
    pub(crate) list: TnodeListArm,
    pub(crate) tree: TnodeTreeArm,
}

// Like the bindgen `tnode` (which derived neither Copy nor Clone),
// this stays underivable-by-policy: `build.rs`'s `NativeTypeCallbacks`
// answers Copy=No for `tnode`/`tnode_t`, and nothing in the remaining
// bindgen output embeds it by value (verified).
#[repr(C)]
pub struct Tnode {
    pub(crate) type_: u32,
    pub(crate) u: TnodeUnion,
    pub(crate) error_no: c_int,
    pub(crate) data: u64,
    pub(crate) thread: *mut thread,
}

// P3-N2 hardcoded layout proof — struct-level values captured from the
// pre-nativization bindgen output (`pub struct tnode { type_:
// tq_type_t, __bindgen_anon_1: tnode__bindgen_ty_1 /* [u64; 5] blob */,
// error_no: c_int, data: uint64, thread: *mut thread }`), union-arm
// interior offsets from the cross-compiler `_Static_assert` probe (see
// the representation note above). `offset_of!` cannot chain *through*
// a union field, so the arms are checked independently; the arm
// structs sit at offset 0 of the union by `#[repr(C)]` union rules,
// so `Tnode.u == 8` + arm-interior offsets fully pin every C offset.
const _: () = {
    assert!(core::mem::size_of::<Tnode>() == 72, "tnode_t size");
    assert!(core::mem::align_of::<Tnode>() == 8, "tnode_t natural alignment");
    assert!(core::mem::offset_of!(Tnode, type_) == 0, "tnode.type offset");
    assert!(core::mem::offset_of!(Tnode, u) == 8, "tnode anonymous-union offset");
    assert!(core::mem::offset_of!(Tnode, error_no) == 48, "tnode.error_no offset");
    assert!(core::mem::offset_of!(Tnode, data) == 56, "tnode.data offset");
    assert!(core::mem::offset_of!(Tnode, thread) == 64, "tnode.thread offset");

    assert!(core::mem::size_of::<TnodeUnion>() == 40, "tnode union size (tree arm dominates)");
    assert!(core::mem::align_of::<TnodeUnion>() == 8, "tnode union alignment");

    assert!(core::mem::size_of::<TnodeListArm>() == 24, "tnode.list arm size");
    assert!(core::mem::align_of::<TnodeListArm>() == 8, "tnode.list arm alignment");
    assert!(core::mem::offset_of!(TnodeListArm, entry) == 0, "tnode.list.entry offset (8 in tnode)");
    assert!(core::mem::offset_of!(TnodeListArm, queue) == 16, "tnode.list.queue offset (24 in tnode)");

    assert!(core::mem::size_of::<TnodeTreeArm>() == 40, "tnode.tree arm size");
    assert!(core::mem::align_of::<TnodeTreeArm>() == 8, "tnode.tree arm alignment");
    assert!(core::mem::offset_of!(TnodeTreeArm, entry) == 0, "tnode.tree.entry offset (8 in tnode)");
    assert!(core::mem::offset_of!(TnodeTreeArm, queue) == 24, "tnode.tree.queue offset (32 in tnode)");
    assert!(core::mem::offset_of!(TnodeTreeArm, key) == 32, "tnode.tree.key offset (40 in tnode)");
};

// Safe wrappers around null-safe pointer constructors.
#[inline] fn tq_of<'a>(p: *mut tq_t) -> Option<TqRef<'a>> { TqRef::from_ptr(p) }
#[inline] fn tn_of<'a>(p: *mut tnode_t) -> Option<TnodeRef<'a>> { TnodeRef::from_ptr(p) }
#[inline] fn tt_of<'a>(p: *mut ttree_t) -> Option<TtreeRef<'a>> { TtreeRef::from_ptr(p) }
#[allow(dead_code)]
#[inline] fn ln_of<'a>(p: *mut crate::bindings::list_node_t) -> Option<ListNodeRef<'a>> {
    ListNodeRef::from_ptr(p)
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
const EINVAL: c_int = 22;
const ENOENT: c_int = 2;
const ENOTEMPTY: c_int = 39;
const EINTR: c_int = 4;

const TYPE_LIST: u32 = 1;
const TYPE_TREE: u32 = 2;

// ---------------------------------------------------------------------------
// External C dependencies. `memset` remains unsafe because it writes raw memory.
// ---------------------------------------------------------------------------
// P3-D3c: the rbtree/bintree primitives are genuinely `unsafe fn`s in
// `crate::{rbtree,bintree}` now that their `#[no_mangle]` exports are
// gone; this file's original extern declarations asserted `pub safe fn`
// (usual FFI-facade convention). Thin wrappers preserve that safe facade
// for the unchanged call sites.
/// SAFETY: see [`crate::rbtree::rb_insert_color`]'s contract.
fn rb_insert_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::rbtree::rb_insert_color(root, node) }
}
/// SAFETY: see [`crate::rbtree::rb_delete_node_color`]'s contract.
fn rb_delete_node_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::rbtree::rb_delete_node_color(root, node) }
}
/// SAFETY: see [`crate::bintree::rb_first_node`]'s contract.
fn rb_first_node(root: *mut rb_root) -> *mut rb_node {
    unsafe { crate::bintree::rb_first_node(root) }
}
/// SAFETY: see [`crate::bintree::rb_next_node`]'s contract.
fn rb_next_node(node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::bintree::rb_next_node(node) }
}
/// SAFETY: see [`crate::bintree::rb_find_key_rup`]'s contract.
fn rb_find_key_rup(root: *mut rb_root, key: uint64) -> *mut rb_node {
    unsafe { crate::bintree::rb_find_key_rup(root, key) }
}

// Scheduler hooks. P3-1B2: previously bridged via the `xv6_schport_*`
// C-ABI alias layer (`extern "C"` redeclarations above); now direct
// crate-path calls to the real, already-Rust definitions in `sched.rs`.
use crate::proc::{scheduler_yield, scheduler_wakeup};

#[inline]
fn invoke_sleep_cb(cb: unsafe extern "C" fn(*mut c_void) -> c_int, data: *mut c_void) -> c_int {
    unsafe { cb(data) }
}

#[inline]
fn invoke_wakeup_cb(cb: unsafe extern "C" fn(*mut c_void, c_int), data: *mut c_void, status: c_int) {
    unsafe { cb(data, status); }
}

#[inline]
fn err_cast<T, U>(p: *mut T) -> *mut U {
    p as *mut U
}

// ---------------------------------------------------------------------------
// rb_root_opts: static comparator tables
// ---------------------------------------------------------------------------
extern "C" fn q_root_get_key(node: *mut rb_node) -> uint64 {
    if node.is_null() {
        return 0;
    }
    tnode_from_tree_entry(node) as uint64
}

extern "C" fn q_root_keys_cmp(key1: uint64, key2: uint64) -> c_int {
    let n1 = key1 as *mut tnode_t;
    let n2 = key2 as *mut tnode_t;
    let r1 = match tn_of(n1) { Some(r) => r, None => return 0 };
    let r2 = match tn_of(n2) { Some(r) => r, None => return 0 };
    let k1 = r1.tree_key();
    let k2 = r2.tree_key();
    if k1 < k2 { -1 } else if k1 > k2 { 1 }
    else if key1 < key2 { -1 } else if key1 > key2 { 1 } else { 0 }
}

extern "C" fn q_root_keys_cmp_rdown(key1: uint64, key2: uint64) -> c_int {
    let n1 = key1 as *mut tnode_t;
    let n2 = key2 as *mut tnode_t;
    let r1 = match tn_of(n1) { Some(r) => r, None => return 0 };
    let r2 = match tn_of(n2) { Some(r) => r, None => return 0 };
    let k1 = r1.tree_key();
    let k2 = r2.tree_key();
    if k1 < k2 { -1 } else if k1 > k2 { 1 }
    else if key1 == 0 { 0 } else { 1 }
}

#[repr(transparent)]
struct OptsCell(UnsafeCell<rb_root_opts>);
// SAFETY: `Q_ROOT_OPTS`/`Q_ROOT_RDOWN_OPTS` below are initialised once
// at compile time (`static ... = OptsCell(UnsafeCell::new(rb_root_opts
// { .. }))`) and never mutated afterward — `q_root_opts()`/
// `q_root_rdown_opts()` only ever read the function pointers back via
// the raw pointer handed to `rb_root_init`. Sharing an immutable value
// across harts is inherently data-race-free (mirrors `vm.rs`'s
// `VmTreeOptsWrap`).
unsafe impl Sync for OptsCell {}

static Q_ROOT_OPTS: OptsCell = OptsCell(UnsafeCell::new(rb_root_opts {
    keys_cmp_fun: Some(q_root_keys_cmp),
    get_key_fun: Some(q_root_get_key),
}));

static Q_ROOT_RDOWN_OPTS: OptsCell = OptsCell(UnsafeCell::new(rb_root_opts {
    keys_cmp_fun: Some(q_root_keys_cmp_rdown),
    get_key_fun: Some(q_root_get_key),
}));

#[inline] fn q_root_opts() -> *mut rb_root_opts { Q_ROOT_OPTS.0.get() }
#[inline] fn q_root_rdown_opts() -> *mut rb_root_opts { Q_ROOT_RDOWN_OPTS.0.get() }

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------
const NULL_NAME: &[u8] = b"NULL\0";

#[inline]
fn name_or_null(name: *const c_char) -> *const c_char {
    if name.is_null() { NULL_NAME.as_ptr() as *const c_char } else { name }
}

fn tq_init_impl(q: *mut tq_t, name: *const c_char, lock: *mut spinlock_t) {
    let Some(qr) = tq_of(q) else { return };
    qr.head_ref().init();
    qr.set_counter(0);
    qr.set_name(name_or_null(name));
    qr.set_lock_ptr(lock);
}

pub(crate) fn tq_init(q: *mut tq_t, name: *const c_char, lock: *mut spinlock_t) {
    tq_init_impl(q, name, lock);
}

fn ttree_init_impl(q: *mut ttree_t, name: *const c_char, lock: *mut spinlock_t) {
    let Some(qr) = tt_of(q) else { return };
    qr.set_root_node(null_mut());
    qr.set_root_opts(q_root_opts());
    qr.set_counter(0);
    qr.set_name(name_or_null(name));
    qr.set_lock_ptr(lock);
}

pub(crate) fn ttree_init(q: *mut ttree_t, name: *const c_char, lock: *mut spinlock_t) {
    ttree_init_impl(q, name, lock);
}

fn tnode_init_impl(node: *mut tnode_t) {
    zero_tnode_ptr(node);
    let Some(nr) = tn_of(node) else { return };
    nr.to_none();
    nr.set_error_no(0);
    nr.set_thread_ptr(xv6_current_thread());
}

fn tq_size_impl(q: *mut tq_t) -> c_int {
    match tq_of(q) {
        Some(qr) => qr.counter(),
        None => -EINVAL,
    }
}

pub(crate) fn tq_size(q: *mut tq_t) -> c_int {
    tq_size_impl(q)
}

fn tnode_get_queue_impl(node: *mut tnode_t) -> *mut tq_t {
    let Some(nr) = tn_of(node) else { return null_mut() };
    if nr.type_get() != TYPE_LIST { return null_mut(); }
    nr.list_queue_ptr()
}

fn tnode_get_thread_impl(node: *mut tnode_t) -> *mut thread {
    match tn_of(node) {
        Some(nr) => nr.thread_ptr(),
        None => null_mut(),
    }
}

pub(crate) fn tnode_get_thread(node: *mut tnode_t) -> *mut thread {
    tnode_get_thread_impl(node)
}

// ---------------------------------------------------------------------------
// List queue operations
// ---------------------------------------------------------------------------
fn tq_push_impl(q: *mut tq_t, node: *mut tnode_t) -> c_int {
    let Some(qr) = tq_of(q) else { return -EINVAL };
    let Some(nr) = tn_of(node) else { return -EINVAL };
    if nr.thread_ptr().is_null() { return -EINVAL; }
    if nr.is_enqueued() { return -EINVAL; }
    nr.to_list();
    nr.list_entry_ref().push_back(qr.head_ptr());
    nr.set_list_queue(q);
    qr.inc_counter();
    fence(Ordering::SeqCst);
    0
}

fn tq_first_impl(q: *mut tq_t) -> *mut tnode_t {
    let Some(qr) = tq_of(q) else { return err_ptr::<tnode_t>(EINVAL) };
    let c = qr.counter();
    if c == 0 { return null_mut(); }
    if c < 0 { return err_ptr::<tnode_t>(EINVAL); }
    let head = qr.head_ptr();
    let first = qr.head_ref().next_ptr();
    if first == head {
        xv6_panic(c"tq_first: queue is not empty but failed to get the first node".as_ptr());
    }
    tnode_from_list_entry(first)
}

fn tq_remove_impl(q: *mut tq_t, node: *mut tnode_t) -> c_int {
    let Some(qr) = tq_of(q) else { return -EINVAL };
    let Some(nr) = tn_of(node) else { return -EINVAL };
    if nr.thread_ptr().is_null() { return -EINVAL; }
    if tnode_get_queue_impl(node) != q { return -EINVAL; }
    if qr.counter() <= 0 {
        xv6_panic(c"tq_remove: queue is empty".as_ptr());
    }
    nr.list_entry_ref().detach();
    nr.to_none();
    qr.dec_counter();
    fence(Ordering::SeqCst);
    0
}

fn tq_pop_impl(q: *mut tq_t) -> *mut tnode_t {
    if q.is_null() { return err_ptr::<tnode_t>(EINVAL); }
    let dequeued = tq_first_impl(q);
    if is_err_or_null(dequeued) { return dequeued; }
    if tnode_get_queue_impl(dequeued) != q {
        xv6_panic(c"tq_pop: dequeued node is not in the expected queue".as_ptr());
    }
    let ret = tq_remove_impl(q, dequeued);
    if ret == 0 { dequeued } else { err_ptr::<tnode_t>(-ret) }
}

fn tq_bulk_move_impl(to: *mut tq_t, from: *mut tq_t) -> c_int {
    let Some(to_r) = tq_of(to) else { return -EINVAL };
    let Some(from_r) = tq_of(from) else { return -EINVAL };
    if to == from { return -EINVAL; }
    if to_r.counter() > 0 { return -ENOTEMPTY; }
    let from_count = from_r.counter();
    if from_count == 0 { return 0; }
    if from_count < 0 { return -EINVAL; }
    to_r.add_counter(from_count);
    from_r.set_counter(0);

    let to_head = to_r.head_ptr();
    let from_head = from_r.head_ptr();
    let to_tail = to_r.head_ref().prev_ptr();
    if let Some(tail) = ln_of(to_tail) {
        tail.insert_bulk_from_head(from_head);
    }

    let mut cur = to_r.head_ref().next_ptr();
    while cur != to_head {
        let lr = match ln_of(cur) { Some(r) => r, None => break };
        let next = lr.next_ptr();
        let proc = tnode_from_list_entry(cur);
        if let Some(pr) = tn_of(proc) { pr.set_list_queue(to); }
        cur = next;
    }
    0
}

pub(crate) fn tq_bulk_move(to: *mut tq_t, from: *mut tq_t) -> c_int {
    tq_bulk_move_impl(to, from)
}

// ---------------------------------------------------------------------------
// Wait primitives
// ---------------------------------------------------------------------------
fn tq_wait_cb_impl(
    q: *mut tq_t,
    sleep_callback: sleep_callback_t,
    wakeup_callback: wakeup_callback_t,
    callback_data: *mut c_void,
    rdata: *mut u64,
) -> c_int {
    if q.is_null() { return -EINVAL; }
    let intr = intr_off_save();
    let mut waiter: tnode_t = zeroed_tnode();
    let waiter_ptr = &raw mut waiter;
    tnode_init_impl(waiter_ptr);
    let wr = tn_of(waiter_ptr).unwrap();
    wr.set_error_no(-EINTR);
    if tq_push_impl(q, waiter_ptr) != 0 {
        xv6_panic(c"Failed to push thread to sleep queue".as_ptr());
    }

    let mut cb_status: c_int = 0;
    if let Some(cb) = sleep_callback {
        cb_status = invoke_sleep_cb(cb, callback_data);
    }
    scheduler_yield();
    if let Some(cb) = wakeup_callback {
        invoke_wakeup_cb(cb, callback_data, cb_status);
    }

    if wr.is_enqueued() {
        if tq_remove_impl(q, waiter_ptr) != 0 {
            xv6_panic(c"Failed to remove interrupted waiter from queue".as_ptr());
        }
    }
    intr_restore(intr);

    if !rdata.is_null() {
        write_out(rdata, wr.data());
    }
    wr.error_no()
}

pub(crate) fn tq_wait_cb(
    q: *mut tq_t,
    sleep_callback: sleep_callback_t,
    wakeup_callback: wakeup_callback_t,
    callback_data: *mut c_void,
    rdata: *mut u64,
) -> c_int {
    tq_wait_cb_impl(q, sleep_callback, wakeup_callback, callback_data, rdata)
}

fn tq_wait_impl(q: *mut tq_t, lock: *mut spinlock_t, rdata: *mut u64) -> c_int {
    tq_wait_cb_impl(q, Some(spin_sleep_cb), Some(spin_wake_cb), lock as *mut c_void, rdata)
}

pub(crate) fn tq_wait(q: *mut tq_t, lock: *mut spinlock_t, rdata: *mut u64) -> c_int {
    tq_wait_impl(q, lock, rdata)
}

// ---------------------------------------------------------------------------
// Wakeup helpers
// ---------------------------------------------------------------------------
fn do_wakeup(woken: *mut tnode_t, error_no: c_int, rdata: u64) -> *mut thread {
    let Some(wr) = tn_of(woken) else { return err_ptr::<thread>(EINVAL) };
    if wr.thread_ptr().is_null() {
        crate::kprintln!("woken thread is NULL");
        return err_ptr::<thread>(EINVAL);
    }
    wr.set_error_no(error_no);
    wr.set_data(rdata);
    let p = wr.thread_ptr();
    scheduler_wakeup(p);
    p
}

fn tq_wakeup_one(q: *mut tq_t, error_no: c_int, rdata: u64) -> *mut thread {
    if q.is_null() { return err_ptr::<thread>(EINVAL); }
    let woken = tq_pop_impl(q);
    if is_err_or_null(woken) { return err_cast::<tnode_t, thread>(woken); }
    do_wakeup(woken, error_no, rdata)
}

pub(crate) fn tq_wakeup(q: *mut tq_t, error_no: c_int, rdata: u64) -> *mut thread {
    tq_wakeup_one(q, error_no, rdata)
}

pub(crate) fn tq_wakeup_all(q: *mut tq_t, error_no: c_int, rdata: u64) -> c_int {
    let Some(qr) = tq_of(q) else { return -EINVAL };
    let mut counter: c_int = 0;
    loop {
        let p = tq_wakeup_one(q, error_no, rdata);
        if p.is_null() {
            if qr.counter() != 0 {
                xv6_panic(c"Queue counter is not zero when queue is empty".as_ptr());
            }
            break;
        }
        if is_err(p) { return ptr_err(p); }
        counter += 1;
    }
    counter
}

// ---------------------------------------------------------------------------
// Tree queue operations
// ---------------------------------------------------------------------------
fn tnode_in_tree(q: *mut ttree_t, node: *mut tnode_t) -> bool {
    let Some(nr) = tn_of(node) else { return false };
    if q.is_null() { return false; }
    if nr.type_get() != TYPE_TREE { return false; }
    nr.tree_queue_ptr() == q
}

fn ttree_find_key_min(q: *mut ttree_t, key: uint64) -> *mut tnode_t {
    let Some(qr) = tt_of(q) else { return null_mut() };
    let mut dummy_root: rb_root = qr.root_copy();
    dummy_root.opts = q_root_rdown_opts();

    let mut dummy: tnode_t = zeroed_tnode();
    let dr = tn_of(&raw mut dummy).unwrap();
    dr.set_tree_key(key);

    let node = rb_find_key_rup(&raw mut dummy_root, &raw const dummy as uint64);
    if node.is_null() { return null_mut(); }
    let target = tnode_from_tree_entry(node);
    let tr = match tn_of(target) { Some(r) => r, None => return null_mut() };
    if tr.tree_key() != key { return null_mut(); }
    target
}

fn ttree_add_impl(q: *mut ttree_t, node: *mut tnode_t) -> c_int {
    let Some(qr) = tt_of(q) else { return -EINVAL };
    let Some(nr) = tn_of(node) else { return -EINVAL };
    if nr.thread_ptr().is_null() { return -EINVAL; }
    if nr.is_enqueued() { return -EINVAL; }
    nr.to_tree();
    nr.set_tree_queue(q);
    let entry = nr.tree_entry_ptr();
    let inserted = rb_insert_color(qr.root_ptr(), entry);
    if inserted != entry {
        xv6_panic(c"Failed to insert node into tree".as_ptr());
    }
    qr.inc_counter();
    fence(Ordering::SeqCst);
    0
}

fn ttree_do_remove(q: *mut ttree_t, node: *mut tnode_t) -> c_int {
    let Some(qr) = tt_of(q) else { return -EINVAL };
    let Some(nr) = tn_of(node) else { return -EINVAL };
    let removed = rb_delete_node_color(qr.root_ptr(), nr.tree_entry_ptr());
    if removed.is_null() { return -ENOENT; }
    nr.to_none();
    qr.dec_counter();
    fence(Ordering::SeqCst);
    0
}

fn ttree_remove_impl(q: *mut ttree_t, node: *mut tnode_t) -> c_int {
    if q.is_null() || node.is_null() { return -EINVAL; }
    if !tnode_in_tree(q, node) { return -EINVAL; }
    ttree_do_remove(q, node)
}

fn ttree_wait_cb_impl(
    q: *mut ttree_t,
    key: uint64,
    sleep_callback: sleep_callback_t,
    wakeup_callback: wakeup_callback_t,
    callback_data: *mut c_void,
    rdata: *mut u64,
) -> c_int {
    if q.is_null() { return -EINVAL; }
    let intr = intr_off_save();
    let mut waiter: tnode_t = zeroed_tnode();
    let waiter_ptr = &raw mut waiter;
    tnode_init_impl(waiter_ptr);
    let wr = tn_of(waiter_ptr).unwrap();
    wr.set_error_no(-EINTR);
    wr.set_tree_key(key);

    if ttree_add_impl(q, waiter_ptr) != 0 {
        xv6_panic(c"Failed to push thread to sleep tree".as_ptr());
    }

    let mut cb_status: c_int = 0;
    if let Some(cb) = sleep_callback {
        cb_status = invoke_sleep_cb(cb, callback_data);
    }
    scheduler_yield();
    if let Some(cb) = wakeup_callback {
        invoke_wakeup_cb(cb, callback_data, cb_status);
    }

    if wr.is_enqueued() {
        if ttree_remove_impl(q, waiter_ptr) != 0 {
            xv6_panic(c"Failed to remove interrupted waiter from tree".as_ptr());
        }
    }
    intr_restore(intr);

    if !rdata.is_null() {
        write_out(rdata, wr.data());
    }
    wr.error_no()
}

fn ttree_wait_impl(
    q: *mut ttree_t,
    key: uint64,
    lock: *mut spinlock_t,
    rdata: *mut u64,
) -> c_int {
    ttree_wait_cb_impl(q, key, Some(spin_sleep_cb), Some(spin_wake_cb), lock as *mut c_void, rdata)
}

pub(crate) fn ttree_wait(
    q: *mut ttree_t,
    key: uint64,
    lock: *mut spinlock_t,
    rdata: *mut u64,
) -> c_int {
    ttree_wait_impl(q, key, lock, rdata)
}

fn ttree_wakeup_one_impl(
    q: *mut ttree_t,
    key: uint64,
    error_no: c_int,
    rdata: u64,
) -> *mut thread {
    if q.is_null() { return err_ptr::<thread>(EINVAL); }
    let target = ttree_find_key_min(q, key);
    if target.is_null() { return err_ptr::<thread>(ENOENT); }
    let ret = ttree_do_remove(q, target);
    if ret != 0 { return err_ptr::<thread>(-ret); }
    do_wakeup(target, error_no, rdata)
}

fn ttree_wakeup_key_impl(
    q: *mut ttree_t,
    key: uint64,
    error_no: c_int,
    rdata: u64,
) -> c_int {
    if q.is_null() { return -EINVAL; }
    let mut count = 0;
    loop {
        let p = ttree_wakeup_one_impl(q, key, error_no, rdata);
        if is_err_or_null(p) { break; }
        count += 1;
    }
    if count == 0 { return -ENOENT; }
    0
}

pub(crate) fn ttree_wakeup_key(
    q: *mut ttree_t,
    key: uint64,
    error_no: c_int,
    rdata: u64,
) -> c_int {
    ttree_wakeup_key_impl(q, key, error_no, rdata)
}

// The backwards-compat `xv6_tqport_*` C-ABI alias layer that used to
// front these entry points was collapsed in the P3-1B2 sweep; P3-D2a
// then dismantled the `#[no_mangle] extern "C"` export surface itself:
// the live entry points above are plain `pub(crate)` fns, and the dead
// exports (tq_set_lock/ttree_set_lock/tnode_init/ttree_size/
// tnode_get_queue/tnode_get_tree/tnode_get_errno/tq_push/tq_first/
// tq_remove/tq_pop/ttree_add/ttree_first/ttree_key_min/ttree_remove/
// ttree_wait_cb/ttree_wakeup_one/ttree_wakeup_all -- 0-ref verified
// crate-wide) were deleted outright, along with the `_impl` bodies only
// they referenced. The internal `_impl` helpers that live callers still
// share (tq_push_impl, tq_remove_impl, ttree_add_impl, ttree_remove_impl,
// ttree_wait_cb_impl, ttree_wakeup_one_impl, ...) are unchanged.
