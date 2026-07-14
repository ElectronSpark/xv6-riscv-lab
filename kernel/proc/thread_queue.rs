//! Pure-Rust port of `kernel/proc/thread_queue.c` (SECTION 16).
//!
//! Provides list-based (`tq_t`) and red-black-tree-based (`ttree_t`)
//! wait-queue primitives used throughout the kernel for sleep/wakeup.
//!
//! Canonical C ABI names (`tq_init`, `tq_wait`, `ttree_wakeup_key`, ...)
//! are exported as `#[no_mangle]`, called directly by sibling Rust
//! modules -- the backwards-compat `xv6_tqport_*` alias layer that used
//! to front these was collapsed in the P3-1B2 sweep.

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

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3D.
//
// `tq_t`/`ttree_t`/`tnode_t` (`kernel/inc/proc/tq_type.h`) are the
// remaining tier-1 aggregates P3-3C's leaf nativization
// (`list_node_t` -> `crate::list::ListNode`, `rb_node`/`rb_root` proven
// layout-identical to their bindgen forms) unblocks
// (`docs/rustify/phase3_plan.md` P3-3D). All three embed only
// already-native leaves plus plain scalars/pointers -- pointer *fields*
// (`name`, `lock`, the `list`/`tree` union arms' `queue` back-pointers)
// are left typed exactly as bindgen has them below: a pointer's own
// layout never depends on its pointee's type, so there is no
// nativization benefit (and no cast burden) in retyping them, unlike
// the by-value `list_node_t`/`rb_node` embeds, which genuinely swap to
// this crate's canonical native mirrors.
//
// All three are naturally aligned (align 8) -- none embeds a lock *by
// value* (only `*mut spinlock_t` pointers), so (unlike `workqueue`,
// which embeds `spinlock_t`/`tq_t` by value and picks up
// `#[repr(align(64))]`) there is no alignment surprise here; confirmed
// directly below, not assumed.
#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct Tq {
    pub(crate) head: ListNode,
    pub(crate) counter: c_int,
    pub(crate) name: *const c_char,
    pub(crate) lock: *mut spinlock_t,
    pub(crate) flags: u64,
}

const _: () = {
    assert!(core::mem::size_of::<Tq>() == core::mem::size_of::<tq_t>(),
        "Tq / tq_t size mismatch");
    assert!(core::mem::align_of::<Tq>() == core::mem::align_of::<tq_t>(),
        "Tq / tq_t alignment mismatch");
    assert!(core::mem::align_of::<tq_t>() == 8,
        "tq_t unexpectedly gained a non-natural alignment -- it only holds a \
         *mut spinlock_t pointer, not an embedded spinlock_t by value");
    assert!(core::mem::offset_of!(Tq, head) == core::mem::offset_of!(tq_t, head),
        "Tq.head / tq_t.head offset mismatch");
    assert!(core::mem::offset_of!(Tq, counter) == core::mem::offset_of!(tq_t, counter),
        "Tq.counter / tq_t.counter offset mismatch");
    assert!(core::mem::offset_of!(Tq, name) == core::mem::offset_of!(tq_t, name),
        "Tq.name / tq_t.name offset mismatch");
    assert!(core::mem::offset_of!(Tq, lock) == core::mem::offset_of!(tq_t, lock),
        "Tq.lock / tq_t.lock offset mismatch");
    assert!(core::mem::offset_of!(Tq, flags) == core::mem::offset_of!(tq_t, flags),
        "Tq.flags / tq_t.flags offset mismatch");
};

/// `root`'s field type is `crate::bindings::rb_root` itself, not a
/// re-wrapped mirror: `bintree.rs`'s own P3-3C convention keeps
/// `RbRoot = rb_root` as the *working* type (its `RawRbRoot` is a
/// separate, deliberately-unwired compile-time layout proof) -- so
/// embedding `rb_root` here already **is** the native form, with zero
/// benefit to introducing a second parallel type.
#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct Ttree {
    pub(crate) root: rb_root,
    pub(crate) counter: c_int,
    pub(crate) name: *const c_char,
    pub(crate) lock: *mut spinlock_t,
    pub(crate) flags: u64,
}

const _: () = {
    assert!(core::mem::size_of::<Ttree>() == core::mem::size_of::<ttree_t>(),
        "Ttree / ttree_t size mismatch");
    assert!(core::mem::align_of::<Ttree>() == core::mem::align_of::<ttree_t>(),
        "Ttree / ttree_t alignment mismatch");
    assert!(core::mem::align_of::<ttree_t>() == 8,
        "ttree_t unexpectedly gained a non-natural alignment");
    assert!(core::mem::offset_of!(Ttree, root) == core::mem::offset_of!(ttree_t, root),
        "Ttree.root / ttree_t.root offset mismatch");
    assert!(core::mem::offset_of!(Ttree, counter) == core::mem::offset_of!(ttree_t, counter),
        "Ttree.counter / ttree_t.counter offset mismatch");
    assert!(core::mem::offset_of!(Ttree, name) == core::mem::offset_of!(ttree_t, name),
        "Ttree.name / ttree_t.name offset mismatch");
    assert!(core::mem::offset_of!(Ttree, lock) == core::mem::offset_of!(ttree_t, lock),
        "Ttree.lock / ttree_t.lock offset mismatch");
    assert!(core::mem::offset_of!(Ttree, flags) == core::mem::offset_of!(ttree_t, flags),
        "Ttree.flags / ttree_t.flags offset mismatch");
};

// `tnode_t`'s union is the one genuinely tricky part of this wave: the
// C `union { struct { list_node_t entry; tq_t *queue; } list; struct {
// rb_node entry; ttree_t *queue; uint64 key; } tree; }` becomes a real
// Rust `union` of two `#[repr(C)]` arm structs below. `entry` in the
// list arm is the native `ListNode`; `entry` in the tree arm stays
// `rb_node` for the same "already native by convention" reason as
// `Ttree::root` above. `offset_of!` cannot chain *through* a union
// field, so the layout gate below checks the union's own size/align
// plus each arm struct independently against its bindgen counterpart,
// rather than chaining `Tnode -> u -> list -> entry` in one expression.
#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct TnodeListArm {
    pub(crate) entry: ListNode,
    pub(crate) queue: *mut tq_t,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct TnodeTreeArm {
    pub(crate) entry: rb_node,
    pub(crate) queue: *mut ttree_t,
    pub(crate) key: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) union TnodeUnion {
    pub(crate) list: TnodeListArm,
    pub(crate) tree: TnodeTreeArm,
}

#[repr(C)]
pub(crate) struct Tnode {
    pub(crate) type_: u32,
    pub(crate) u: TnodeUnion,
    pub(crate) error_no: c_int,
    pub(crate) data: u64,
    pub(crate) thread: *mut thread,
}

const _: () = {
    use crate::bindings::{
        tnode__bindgen_ty_1 as BindgenTnodeUnion,
        tnode__bindgen_ty_1__bindgen_ty_1 as BindgenListArm,
        tnode__bindgen_ty_1__bindgen_ty_2 as BindgenTreeArm,
    };

    assert!(core::mem::size_of::<Tnode>() == core::mem::size_of::<tnode_t>(),
        "Tnode / tnode_t size mismatch");
    assert!(core::mem::align_of::<Tnode>() == core::mem::align_of::<tnode_t>(),
        "Tnode / tnode_t alignment mismatch");
    assert!(core::mem::align_of::<tnode_t>() == 8,
        "tnode_t unexpectedly gained a non-natural alignment");
    assert!(core::mem::offset_of!(Tnode, type_) == core::mem::offset_of!(tnode_t, type_),
        "Tnode.type_ / tnode_t.type_ offset mismatch");
    assert!(core::mem::offset_of!(Tnode, u) == core::mem::offset_of!(tnode_t, __bindgen_anon_1),
        "Tnode.u / tnode_t.__bindgen_anon_1 offset mismatch");
    assert!(core::mem::offset_of!(Tnode, error_no) == core::mem::offset_of!(tnode_t, error_no),
        "Tnode.error_no / tnode_t.error_no offset mismatch");
    assert!(core::mem::offset_of!(Tnode, data) == core::mem::offset_of!(tnode_t, data),
        "Tnode.data / tnode_t.data offset mismatch");
    assert!(core::mem::offset_of!(Tnode, thread) == core::mem::offset_of!(tnode_t, thread),
        "Tnode.thread / tnode_t.thread offset mismatch");

    assert!(core::mem::size_of::<TnodeUnion>() == core::mem::size_of::<BindgenTnodeUnion>(),
        "TnodeUnion / tnode__bindgen_ty_1 size mismatch");
    assert!(core::mem::align_of::<TnodeUnion>() == core::mem::align_of::<BindgenTnodeUnion>(),
        "TnodeUnion / tnode__bindgen_ty_1 alignment mismatch");

    assert!(core::mem::size_of::<TnodeListArm>() == core::mem::size_of::<BindgenListArm>(),
        "TnodeListArm / tnode__bindgen_ty_1__bindgen_ty_1 size mismatch");
    assert!(core::mem::align_of::<TnodeListArm>() == core::mem::align_of::<BindgenListArm>(),
        "TnodeListArm / tnode__bindgen_ty_1__bindgen_ty_1 alignment mismatch");
    assert!(core::mem::offset_of!(TnodeListArm, entry) == core::mem::offset_of!(BindgenListArm, entry),
        "TnodeListArm.entry offset mismatch");
    assert!(core::mem::offset_of!(TnodeListArm, queue) == core::mem::offset_of!(BindgenListArm, queue),
        "TnodeListArm.queue offset mismatch");

    assert!(core::mem::size_of::<TnodeTreeArm>() == core::mem::size_of::<BindgenTreeArm>(),
        "TnodeTreeArm / tnode__bindgen_ty_1__bindgen_ty_2 size mismatch");
    assert!(core::mem::align_of::<TnodeTreeArm>() == core::mem::align_of::<BindgenTreeArm>(),
        "TnodeTreeArm / tnode__bindgen_ty_1__bindgen_ty_2 alignment mismatch");
    assert!(core::mem::offset_of!(TnodeTreeArm, entry) == core::mem::offset_of!(BindgenTreeArm, entry),
        "TnodeTreeArm.entry offset mismatch");
    assert!(core::mem::offset_of!(TnodeTreeArm, queue) == core::mem::offset_of!(BindgenTreeArm, queue),
        "TnodeTreeArm.queue offset mismatch");
    assert!(core::mem::offset_of!(TnodeTreeArm, key) == core::mem::offset_of!(BindgenTreeArm, key),
        "TnodeTreeArm.key offset mismatch");
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
unsafe extern "C" {
    pub safe fn xv6_panic(msg: *const c_char) -> !;
    pub safe fn xv6_current_thread() -> *mut thread;

    // rbtree primitives.
    pub safe fn rb_insert_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node;
    pub safe fn rb_delete_node_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node;
    pub safe fn rb_first_node(root: *mut rb_root) -> *mut rb_node;
    pub safe fn rb_next_node(node: *mut rb_node) -> *mut rb_node;
    pub safe fn rb_find_key_rup(root: *mut rb_root, key: uint64) -> *mut rb_node;
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

#[no_mangle]
pub extern "C" fn tq_init(q: *mut tq_t, name: *const c_char, lock: *mut spinlock_t) {
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

#[no_mangle]
pub extern "C" fn ttree_init(q: *mut ttree_t, name: *const c_char, lock: *mut spinlock_t) {
    ttree_init_impl(q, name, lock);
}

fn tq_set_lock_impl(q: *mut tq_t, lock: *mut spinlock_t) {
    if let Some(qr) = tq_of(q) { qr.set_lock_ptr(lock); }
}

#[no_mangle]
pub extern "C" fn tq_set_lock(q: *mut tq_t, lock: *mut spinlock_t) {
    tq_set_lock_impl(q, lock);
}

fn ttree_set_lock_impl(q: *mut ttree_t, lock: *mut spinlock_t) {
    if let Some(qr) = tt_of(q) { qr.set_lock_ptr(lock); }
}

#[no_mangle]
pub extern "C" fn ttree_set_lock(q: *mut ttree_t, lock: *mut spinlock_t) {
    ttree_set_lock_impl(q, lock);
}

fn tnode_init_impl(node: *mut tnode_t) {
    zero_tnode_ptr(node);
    let Some(nr) = tn_of(node) else { return };
    nr.to_none();
    nr.set_error_no(0);
    nr.set_thread_ptr(xv6_current_thread());
}

#[no_mangle]
pub extern "C" fn tnode_init(node: *mut tnode_t) {
    tnode_init_impl(node);
}

fn tq_size_impl(q: *mut tq_t) -> c_int {
    match tq_of(q) {
        Some(qr) => qr.counter(),
        None => -EINVAL,
    }
}

#[no_mangle]
pub extern "C" fn tq_size(q: *mut tq_t) -> c_int {
    tq_size_impl(q)
}

fn ttree_size_impl(q: *mut ttree_t) -> c_int {
    match tt_of(q) {
        Some(qr) => qr.counter(),
        None => -EINVAL,
    }
}

#[no_mangle]
pub extern "C" fn ttree_size(q: *mut ttree_t) -> c_int {
    ttree_size_impl(q)
}

fn tnode_get_queue_impl(node: *mut tnode_t) -> *mut tq_t {
    let Some(nr) = tn_of(node) else { return null_mut() };
    if nr.type_get() != TYPE_LIST { return null_mut(); }
    nr.list_queue_ptr()
}

#[no_mangle]
pub extern "C" fn tnode_get_queue(node: *mut tnode_t) -> *mut tq_t {
    tnode_get_queue_impl(node)
}

fn tnode_get_tree_impl(node: *mut tnode_t) -> *mut ttree_t {
    let Some(nr) = tn_of(node) else { return null_mut() };
    if nr.type_get() != TYPE_TREE { return null_mut(); }
    nr.tree_queue_ptr()
}

#[no_mangle]
pub extern "C" fn tnode_get_tree(node: *mut tnode_t) -> *mut ttree_t {
    tnode_get_tree_impl(node)
}

fn tnode_get_thread_impl(node: *mut tnode_t) -> *mut thread {
    match tn_of(node) {
        Some(nr) => nr.thread_ptr(),
        None => null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn tnode_get_thread(node: *mut tnode_t) -> *mut thread {
    tnode_get_thread_impl(node)
}

fn tnode_get_errno_impl(node: *mut tnode_t, error_no: *mut c_int) -> c_int {
    let Some(nr) = tn_of(node) else { return -EINVAL };
    if error_no.is_null() { return -EINVAL; }
    write_out(error_no, nr.error_no());
    0
}

#[no_mangle]
pub extern "C" fn tnode_get_errno(node: *mut tnode_t, error_no: *mut c_int) -> c_int {
    tnode_get_errno_impl(node, error_no)
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

#[no_mangle]
pub extern "C" fn tq_push(q: *mut tq_t, node: *mut tnode_t) -> c_int {
    tq_push_impl(q, node)
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

#[no_mangle]
pub extern "C" fn tq_first(q: *mut tq_t) -> *mut tnode_t {
    tq_first_impl(q)
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

#[no_mangle]
pub extern "C" fn tq_remove(q: *mut tq_t, node: *mut tnode_t) -> c_int {
    tq_remove_impl(q, node)
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

#[no_mangle]
pub extern "C" fn tq_pop(q: *mut tq_t) -> *mut tnode_t {
    tq_pop_impl(q)
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

#[no_mangle]
pub extern "C" fn tq_bulk_move(to: *mut tq_t, from: *mut tq_t) -> c_int {
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

#[no_mangle]
pub extern "C" fn tq_wait_cb(
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

#[no_mangle]
pub extern "C" fn tq_wait(q: *mut tq_t, lock: *mut spinlock_t, rdata: *mut u64) -> c_int {
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

#[no_mangle]
pub extern "C" fn tq_wakeup(q: *mut tq_t, error_no: c_int, rdata: u64) -> *mut thread {
    tq_wakeup_one(q, error_no, rdata)
}

#[no_mangle]
pub extern "C" fn tq_wakeup_all(q: *mut tq_t, error_no: c_int, rdata: u64) -> c_int {
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

#[no_mangle]
pub extern "C" fn ttree_add(q: *mut ttree_t, node: *mut tnode_t) -> c_int {
    ttree_add_impl(q, node)
}

fn ttree_first_impl(q: *mut ttree_t) -> *mut tnode_t {
    let Some(qr) = tt_of(q) else { return err_ptr::<tnode_t>(EINVAL) };
    let first = rb_first_node(qr.root_ptr());
    if is_err_or_null(first) { return err_cast::<rb_node, tnode_t>(first); }
    tnode_from_tree_entry(first)
}

#[no_mangle]
pub extern "C" fn ttree_first(q: *mut ttree_t) -> *mut tnode_t {
    ttree_first_impl(q)
}

fn ttree_key_min_impl(q: *mut ttree_t, key: *mut u64) -> c_int {
    let min_node = ttree_first_impl(q);
    if min_node.is_null() { return -ENOENT; }
    if is_err(min_node) { return ptr_err(min_node); }
    let mr = match tn_of(min_node) { Some(r) => r, None => return -EINVAL };
    if key.is_null() { return -EINVAL; }
    write_out(key, mr.tree_key());
    0
}

#[no_mangle]
pub extern "C" fn ttree_key_min(q: *mut ttree_t, key: *mut u64) -> c_int {
    ttree_key_min_impl(q, key)
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

#[no_mangle]
pub extern "C" fn ttree_remove(q: *mut ttree_t, node: *mut tnode_t) -> c_int {
    ttree_remove_impl(q, node)
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

#[no_mangle]
pub extern "C" fn ttree_wait_cb(
    q: *mut ttree_t,
    key: uint64,
    sleep_callback: sleep_callback_t,
    wakeup_callback: wakeup_callback_t,
    callback_data: *mut c_void,
    rdata: *mut u64,
) -> c_int {
    ttree_wait_cb_impl(q, key, sleep_callback, wakeup_callback, callback_data, rdata)
}

fn ttree_wait_impl(
    q: *mut ttree_t,
    key: uint64,
    lock: *mut spinlock_t,
    rdata: *mut u64,
) -> c_int {
    ttree_wait_cb_impl(q, key, Some(spin_sleep_cb), Some(spin_wake_cb), lock as *mut c_void, rdata)
}

#[no_mangle]
pub extern "C" fn ttree_wait(
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

#[no_mangle]
pub extern "C" fn ttree_wakeup_one(
    q: *mut ttree_t,
    key: uint64,
    error_no: c_int,
    rdata: u64,
) -> *mut thread {
    ttree_wakeup_one_impl(q, key, error_no, rdata)
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

#[no_mangle]
pub extern "C" fn ttree_wakeup_key(
    q: *mut ttree_t,
    key: uint64,
    error_no: c_int,
    rdata: u64,
) -> c_int {
    ttree_wakeup_key_impl(q, key, error_no, rdata)
}

fn ttree_wakeup_all_impl(q: *mut ttree_t, error_no: c_int, rdata: u64) -> c_int {
    let Some(qr) = tt_of(q) else { return -EINVAL };
    if qr.counter() <= 0 { return -ENOENT; }
    let mut count = 0;
    let mut pos_node = rb_first_node(qr.root_ptr());
    while !pos_node.is_null() {
        let next_node = rb_next_node(pos_node);
        let pos = tnode_from_tree_entry(pos_node);
        if !tnode_in_tree(q, pos) {
            xv6_panic(c"Thread node is not in the tree".as_ptr());
        }
        if ttree_do_remove(q, pos) != 0 {
            crate::kprintln!("warning: Failed to remove node from tree during wakeup all");
        }
        do_wakeup(pos, error_no, rdata);
        count += 1;
        pos_node = next_node;
    }
    if count == 0 { return -ENOENT; }
    qr.set_root_node(null_mut());
    0
}

#[no_mangle]
pub extern "C" fn ttree_wakeup_all(q: *mut ttree_t, error_no: c_int, rdata: u64) -> c_int {
    ttree_wakeup_all_impl(q, error_no, rdata)
}

// The backwards-compat `xv6_tqport_*` C-ABI alias layer that used to
// front tq_init/ttree_init/tq_set_lock/ttree_set_lock/tnode_init/
// tq_size/ttree_size/tnode_get_queue/tnode_get_tree/tnode_get_thread/
// tnode_get_errno/tq_push/tq_first/tq_pop/tq_remove/tq_bulk_move/tq_wait
// [_cb]/tq_wakeup[_all]/ttree_add/ttree_first/ttree_key_min/ttree_remove/
// ttree_wait[_cb]/ttree_wakeup_one/ttree_wakeup_key/ttree_wakeup_all was
// collapsed in the P3-1B2 sweep: every caller now invokes these
// canonical names directly (crate-path, no FFI hop).
