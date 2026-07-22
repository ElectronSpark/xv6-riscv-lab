//! Pure-Rust port of `kernel/proc/thread_queue.c` (SECTION 16).
//!
//! Provides list-based (`tq_t`) and red-black-tree-based (`ttree_t`)
//! wait-queue primitives used throughout the kernel for sleep/wakeup.
//!
//! The canonical operations (`init`, `wait`, `wakeup_key`, ...) are now
//! inherent methods on the `TqRef`/`TtreeRef`/`TnodeRef` handles, invoked
//! directly at each call site (which builds the handle via `from_ptr`).
//! The backwards-compat `xv6_tqport_*` alias layer that used to front the
//! old free-fn entry points was collapsed in the P3-1B2 sweep, the
//! `#[no_mangle] extern "C"` export surface was dismantled in P3-D2a, and
//! the thin free-fn delegators themselves were removed in NO-STANDALONE-FN
//! (call sites construct the handle + method directly).
//!
//! N-METH (RUSTIFY-PROC): the per-object algorithms that used to be
//! free `*_impl(q: *mut …)` fns are now inherent METHODS on the
//! `TqRef`/`TtreeRef`/`TnodeRef` *handles* (defined in
//! [`crate::proc::access`]). The handles read/write the underlying C
//! struct through a `NonNull` raw pointer -- crucially, no `&Tq`/`&Ttree`/
//! `&Tnode` reference is ever formed, so the Freeze-`noalias` read-hoist
//! hazard that would fire on a `&self` method of the (`Copy`, `UnsafeCell`-
//! free => `Freeze`) native structs is structurally avoided: every field
//! touch is a raw load/store the optimizer may not reorder across the
//! `Scheduler::yield_now()` park point. Callers hold raw `*mut tq_t`/`*mut
//! ttree_t` into by-value-embedded queues all over the kernel; each call
//! site null-checks by constructing the handle with `from_ptr` (returning
//! `Option`) before invoking the method.

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
use crate::machine::Riscv;
use crate::proc::access::{
    err_ptr_errno as err_ptr, is_err, is_err_or_null, ptr_err_errno as ptr_err,
    tnode_from_list_entry, tnode_from_tree_entry, write_out, ListNodeRef, TnodeRef, TqRef,
    TtreeRef, zeroed_tnode, zero_tnode_ptr,
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

// NO-STANDALONE-FN: the former `tq_of`/`tn_of`/`tt_of`/`ln_of` null-safe
// ctors were thin forwarders to `TqRef::from_ptr` (etc.), which already
// null-checks; they are deleted and every former call site now names the
// handle constructor directly.

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
// External C dependencies.
// ---------------------------------------------------------------------------
// NO-STANDALONE-FN: the `rb_insert_color`/`rb_delete_node_color`/
// `rb_find_key_rup` safe-facade wrappers (and the never-called
// `rb_first_node`/`rb_next_node` pair) over `crate::{rbtree,bintree}` were
// single-purpose forwarders; each of the three live ones is now inlined as
// a `crate::{rbtree,bintree}::…` call at its sole method call site, with the
// `unsafe` block moved there. See those sites for the SAFETY notes.

// Scheduler hooks. P3-1B2: previously bridged via the `xv6_schport_*`
// C-ABI alias layer (`extern "C"` redeclarations above); now direct
// crate-path calls to the real, already-Rust definitions in `sched.rs`.
use crate::proc::Scheduler;

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
    let r1 = match TnodeRef::from_ptr(n1) { Some(r) => r, None => return 0 };
    let r2 = match TnodeRef::from_ptr(n2) { Some(r) => r, None => return 0 };
    let k1 = r1.tree_key();
    let k2 = r2.tree_key();
    if k1 < k2 { -1 } else if k1 > k2 { 1 }
    else if key1 < key2 { -1 } else if key1 > key2 { 1 } else { 0 }
}

extern "C" fn q_root_keys_cmp_rdown(key1: uint64, key2: uint64) -> c_int {
    let n1 = key1 as *mut tnode_t;
    let n2 = key2 as *mut tnode_t;
    let r1 = match TnodeRef::from_ptr(n1) { Some(r) => r, None => return 0 };
    let r2 = match TnodeRef::from_ptr(n2) { Some(r) => r, None => return 0 };
    let k1 = r1.tree_key();
    let k2 = r2.tree_key();
    if k1 < k2 { -1 } else if k1 > k2 { 1 }
    else if key1 == 0 { 0 } else { 1 }
}

#[repr(transparent)]
struct OptsCell(UnsafeCell<rb_root_opts>);
// SAFETY: `Q_ROOT_OPTS`/`Q_ROOT_RDOWN_OPTS` below are initialised once
// at compile time (`static ... = OptsCell(UnsafeCell::new(rb_root_opts
// { .. }))`) and never mutated afterward — the two `.0.get()` reads in
// `TtreeRef::init`/`find_key_min` only ever read the function pointers
// back via the raw pointer handed to `rb_root_init`. Sharing an immutable
// value across harts is inherently data-race-free (mirrors `vm.rs`'s
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

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------
// NO-STANDALONE-FN: the `name_or_null` helper and the `q_root_opts`/
// `q_root_rdown_opts` static-accessor free fns were single-expression
// forwarders; each is now inlined at its (two) method call sites. Likewise
// the `list_ring_walk` iterator constructor is inlined into
// `TqRef::bulk_move_from` (its sole caller).
const NULL_NAME: &[u8] = b"NULL\0";

// ===========================================================================
// Node (`tnode_t`) operations -- inherent methods on the `TnodeRef` handle.
//
// N-METH: `tnode_init_impl`/`tnode_get_queue_impl`/`tnode_in_tree`/
// `do_wakeup` were free fns taking a raw `*mut tnode_t`; each first did a
// `TnodeRef::from_ptr(node)` null-guard, so the natural receiver is exactly that guarded
// handle. `TnodeRef` wraps a `NonNull<Tnode>` and touches fields through the
// raw pointer, so `&self` here carries no `&Tnode` and no Freeze hazard --
// the waiter's `queue`/`error_no` fields are read fresh after the park even
// though another hart cleared them under the queue lock.
// ===========================================================================
impl<'a> TnodeRef<'a> {
    /// Zero the node, mark it detached, and stamp the current thread as
    /// its owner -- the pre-park setup for a stack-local waiter.
    /// (Former free fn `tnode_init_impl`.)
    #[inline]
    fn init_waiter(&self) {
        zero_tnode_ptr(self.as_ptr());
        self.to_none();
        self.set_error_no(0);
        self.set_thread_ptr(xv6_current_thread());
    }

    /// The list queue this node is enqueued on, or null if it is not a
    /// list node. (Former free fn `tnode_get_queue_impl`.)
    #[inline]
    fn list_queue(&self) -> *mut tq_t {
        if self.type_get() != TYPE_LIST { return null_mut(); }
        self.list_queue_ptr()
    }

    /// Whether this node is currently enqueued on tree `q`.
    /// (Former free fn `tnode_in_tree`.)
    #[inline]
    fn in_tree(&self, q: *mut ttree_t) -> bool {
        if q.is_null() { return false; }
        if self.type_get() != TYPE_TREE { return false; }
        self.tree_queue_ptr() == q
    }

    /// Record the wake result on this (already-dequeued) waiter and hand
    /// its thread to the scheduler. (Former free fn `do_wakeup`.)
    fn do_wakeup(&self, error_no: c_int, rdata: u64) -> *mut thread {
        if self.thread_ptr().is_null() {
            crate::kprintln!("woken thread is NULL");
            return err_ptr::<thread>(EINVAL);
        }
        self.set_error_no(error_no);
        self.set_data(rdata);
        let p = self.thread_ptr();
        Scheduler::wakeup_thread(p);
        p
    }
}

// ===========================================================================
// List queue (`tq_t`) operations -- inherent methods on the `TqRef` handle.
//
// N-METH: the `tq_*_impl` free fns are now `&self` methods on `TqRef`.
// Same freeze-safety argument as `TnodeRef`: `TqRef` is a `NonNull<Tq>`
// handle, every `counter`/`head` touch is a raw load/store, so no `&Tq`
// ever exists and LLVM applies no `noalias`/`readonly` to the queue --
// mandatory here because `counter` is mutated by other harts (under the
// queue's own spinlock, held by the caller) between our reads.
// ===========================================================================
impl<'a> TqRef<'a> {
    /// (Former free fn `tq_init_impl`.) NO-STANDALONE-FN: `pub(crate)` —
    /// invoked directly at cross-kernel call sites via `TqRef::from_ptr(q)`
    /// (former `tq_init` delegator deleted).
    pub(crate) fn init(&self, name: *const c_char, lock: *mut spinlock_t) {
        self.head_ref().init();
        self.set_counter(0);
        self.set_name(if name.is_null() { NULL_NAME.as_ptr() as *const c_char } else { name });
        self.set_lock_ptr(lock);
    }

    /// Number of waiters. (Former free fn `tq_size_impl`, valid-queue path.)
    /// NO-STANDALONE-FN: `pub(crate)` (former `tq_size` delegator deleted).
    #[inline]
    pub(crate) fn size(&self) -> c_int {
        self.counter()
    }

    /// (Former free fn `tq_push_impl`.)
    fn push(&self, node: *mut tnode_t) -> c_int {
        let Some(nr) = TnodeRef::from_ptr(node) else { return -EINVAL };
        if nr.thread_ptr().is_null() { return -EINVAL; }
        if nr.is_enqueued() { return -EINVAL; }
        nr.to_list();
        nr.list_entry_ref().push_back(self.head_ptr());
        nr.set_list_queue(self.as_ptr());
        self.inc_counter();
        fence(Ordering::SeqCst);
        0
    }

    /// (Former free fn `tq_first_impl`.)
    fn first(&self) -> *mut tnode_t {
        let c = self.counter();
        if c == 0 { return null_mut(); }
        if c < 0 { return err_ptr::<tnode_t>(EINVAL); }
        let head = self.head_ptr();
        let first = self.head_ref().next_ptr();
        if first == head {
            xv6_panic(c"tq_first: queue is not empty but failed to get the first node".as_ptr());
        }
        tnode_from_list_entry(first)
    }

    /// (Former free fn `tq_remove_impl`.)
    fn remove(&self, node: *mut tnode_t) -> c_int {
        let Some(nr) = TnodeRef::from_ptr(node) else { return -EINVAL };
        if nr.thread_ptr().is_null() { return -EINVAL; }
        if nr.list_queue() != self.as_ptr() { return -EINVAL; }
        if self.counter() <= 0 {
            xv6_panic(c"tq_remove: queue is empty".as_ptr());
        }
        nr.list_entry_ref().detach();
        nr.to_none();
        self.dec_counter();
        fence(Ordering::SeqCst);
        0
    }

    /// (Former free fn `tq_pop_impl`, valid-queue path.)
    fn pop(&self) -> *mut tnode_t {
        let dequeued = self.first();
        if is_err_or_null(dequeued) { return dequeued; }
        let q_of = TnodeRef::from_ptr(dequeued).map_or(null_mut(), |r| r.list_queue());
        if q_of != self.as_ptr() {
            xv6_panic(c"tq_pop: dequeued node is not in the expected queue".as_ptr());
        }
        let ret = self.remove(dequeued);
        if ret == 0 { dequeued } else { err_ptr::<tnode_t>(-ret) }
    }

    /// Splice every waiter of `from` onto the tail of `self` (which must be
    /// empty) and re-parent each moved node's `queue` back-pointer.
    /// (Former free fn `tq_bulk_move_impl`.) NO-STANDALONE-FN: `pub(crate)`
    /// (former `tq_bulk_move` delegator deleted).
    pub(crate) fn bulk_move_from(&self, from: TqRef<'_>) -> c_int {
        if self.as_ptr() == from.as_ptr() { return -EINVAL; }
        if self.counter() > 0 { return -ENOTEMPTY; }
        let from_count = from.counter();
        if from_count == 0 { return 0; }
        if from_count < 0 { return -EINVAL; }
        self.add_counter(from_count);
        from.set_counter(0);

        let to_head = self.head_ptr();
        let from_head = from.head_ptr();
        let to_tail = self.head_ref().prev_ptr();
        if let Some(tail) = ListNodeRef::from_ptr(to_tail) {
            tail.insert_bulk_from_head(from_head);
        }

        // Re-parent pass: walk the now-spliced ring and point every node's
        // list back-pointer at `self`. Structure is stable across this walk
        // (no detach), so it is a read-only structural traversal expressed
        // as an inline `successors` iterator (former free fn `list_ring_walk`);
        // only each node's `queue` payload is written. Each hop reads `.next`
        // through a `ListNodeRef` handle (a raw pointer load) -- no `&ListNode`
        // is formed, so there is no Freeze-noalias read-hoist. `to_head` is the
        // sentinel: the walk yields `head.next`, `head.next.next`, ... stopping
        // before revisiting the sentinel.
        let ring_first = self.head_ref().next_ptr();
        let ring = core::iter::successors(
            (ring_first != to_head).then_some(ring_first),
            move |&cur| {
                let next = ListNodeRef::from_ptr(cur)?.next_ptr();
                (next != to_head).then_some(next)
            },
        );
        for cur in ring {
            let proc = tnode_from_list_entry(cur);
            if let Some(pr) = TnodeRef::from_ptr(proc) { pr.set_list_queue(self.as_ptr()); }
        }
        0
    }

    /// Park the calling thread on this queue until woken/interrupted.
    /// (Former free fn `tq_wait_cb_impl`, valid-queue path.)
    /// NO-STANDALONE-FN: `pub(crate)` (former `tq_wait_cb` delegator deleted).
    pub(crate) fn wait_cb(
        &self,
        sleep_callback: sleep_callback_t,
        wakeup_callback: wakeup_callback_t,
        callback_data: *mut c_void,
        rdata: *mut u64,
    ) -> c_int {
        let intr = Riscv::intr_off_save();
        let mut waiter: tnode_t = zeroed_tnode();
        let waiter_ptr = &raw mut waiter;
        let wr = TnodeRef::from_ptr(waiter_ptr).unwrap();
        wr.init_waiter();
        wr.set_error_no(-EINTR);
        if self.push(waiter_ptr) != 0 {
            xv6_panic(c"Failed to push thread to sleep queue".as_ptr());
        }

        let mut cb_status: c_int = 0;
        if let Some(cb) = sleep_callback {
            // SAFETY: `cb` is a caller-supplied `sleep_callback_t` fn-ptr
            // invoked with the matching `callback_data` (the spinlock ptr
            // for `wait`); the C-ABI callback contract is the caller's.
            cb_status = unsafe { cb(callback_data) };
        }
        Scheduler::yield_now();
        if let Some(cb) = wakeup_callback {
            // SAFETY: as above, for the paired `wakeup_callback_t`.
            unsafe { cb(callback_data, cb_status); }
        }

        if wr.is_enqueued() {
            if self.remove(waiter_ptr) != 0 {
                xv6_panic(c"Failed to remove interrupted waiter from queue".as_ptr());
            }
        }
        Riscv::intr_restore(intr);

        if !rdata.is_null() {
            write_out(rdata, wr.data());
        }
        wr.error_no()
    }

    /// Spinlock-backed park (releases/reacquires `lock` around the block).
    /// (Former free fn `tq_wait_impl`, valid-queue path.)
    /// NO-STANDALONE-FN: `pub(crate)` (former `tq_wait` delegator deleted).
    #[inline]
    pub(crate) fn wait(&self, lock: *mut spinlock_t, rdata: *mut u64) -> c_int {
        self.wait_cb(Some(spin_sleep_cb), Some(spin_wake_cb), lock as *mut c_void, rdata)
    }

    /// Pop and wake a single waiter. (Former free fn `tq_wakeup_one`,
    /// valid-queue path.) NO-STANDALONE-FN: `pub(crate)` (former `tq_wakeup`
    /// delegator deleted).
    pub(crate) fn wakeup_one(&self, error_no: c_int, rdata: u64) -> *mut thread {
        let woken = self.pop();
        // `woken` is a `*mut tnode_t`; on the error/null path we hand the same
        // bit-pattern back as a `*mut thread` (former free fn `err_cast`).
        if is_err_or_null(woken) { return woken as *mut thread; }
        // `woken` is non-null and valid here (the `is_err_or_null` guard).
        TnodeRef::from_ptr(woken).map_or(err_ptr::<thread>(EINVAL), |r| r.do_wakeup(error_no, rdata))
    }

    /// Drain the queue, waking every waiter. (Former free fn body of
    /// `tq_wakeup_all`.) This is a wake+dequeue drain -- each `wakeup_one`
    /// structurally pops a node -- so it stays a raw `loop`, not an iterator.
    /// NO-STANDALONE-FN: `pub(crate)` (former `tq_wakeup_all` delegator deleted).
    pub(crate) fn wakeup_all(&self, error_no: c_int, rdata: u64) -> c_int {
        let mut counter: c_int = 0;
        loop {
            let p = self.wakeup_one(error_no, rdata);
            if p.is_null() {
                if self.counter() != 0 {
                    xv6_panic(c"Queue counter is not zero when queue is empty".as_ptr());
                }
                break;
            }
            if is_err(p) { return ptr_err(p); }
            counter += 1;
        }
        counter
    }
}

// ===========================================================================
// Tree queue (`ttree_t`) operations -- inherent methods on `TtreeRef`.
//
// N-METH: same handle-receiver / freeze-safety story as `TqRef`.
// ===========================================================================
impl<'a> TtreeRef<'a> {
    /// (Former free fn `ttree_init_impl`.) NO-STANDALONE-FN: `pub(super)` —
    /// sole caller is `crate::proc::sched` via `TtreeRef::from_ptr(q)`
    /// (former `ttree_init` delegator deleted).
    pub(super) fn init(&self, name: *const c_char, lock: *mut spinlock_t) {
        self.set_root_node(null_mut());
        self.set_root_opts(Q_ROOT_OPTS.0.get());
        self.set_counter(0);
        self.set_name(if name.is_null() { NULL_NAME.as_ptr() as *const c_char } else { name });
        self.set_lock_ptr(lock);
    }

    /// The smallest-key node with `key`, or null. (Former free fn
    /// `ttree_find_key_min`.)
    fn find_key_min(&self, key: uint64) -> *mut tnode_t {
        let mut dummy_root: rb_root = self.root_copy();
        dummy_root.opts = Q_ROOT_RDOWN_OPTS.0.get();

        let mut dummy: tnode_t = zeroed_tnode();
        TnodeRef::from_ptr(&raw mut dummy).unwrap().set_tree_key(key);

        // Former safe-facade `rb_find_key_rup`; SAFETY per
        // [`crate::bintree::rb_find_key_rup`]'s contract (valid `dummy_root`
        // with `rdown` opts, `dummy` keyed above).
        let node = unsafe { crate::bintree::rb_find_key_rup(&raw mut dummy_root, &raw const dummy as uint64) };
        if node.is_null() { return null_mut(); }
        let target = tnode_from_tree_entry(node);
        match TnodeRef::from_ptr(target) {
            Some(r) if r.tree_key() == key => target,
            _ => null_mut(),
        }
    }

    /// (Former free fn `ttree_add_impl`.)
    fn add(&self, node: *mut tnode_t) -> c_int {
        let Some(nr) = TnodeRef::from_ptr(node) else { return -EINVAL };
        if nr.thread_ptr().is_null() { return -EINVAL; }
        if nr.is_enqueued() { return -EINVAL; }
        nr.to_tree();
        nr.set_tree_queue(self.as_ptr());
        let entry = nr.tree_entry_ptr();
        // Former safe-facade `rb_insert_color`; SAFETY per
        // [`crate::rbtree::rb_insert_color`]'s contract.
        let inserted = unsafe { crate::rbtree::rb_insert_color(self.root_ptr(), entry) };
        if inserted != entry {
            xv6_panic(c"Failed to insert node into tree".as_ptr());
        }
        self.inc_counter();
        fence(Ordering::SeqCst);
        0
    }

    /// Unchecked removal (caller has already confirmed membership).
    /// (Former free fn `ttree_do_remove`.)
    fn do_remove(&self, node: *mut tnode_t) -> c_int {
        let Some(nr) = TnodeRef::from_ptr(node) else { return -EINVAL };
        // Former safe-facade `rb_delete_node_color`; SAFETY per
        // [`crate::rbtree::rb_delete_node_color`]'s contract.
        let removed = unsafe { crate::rbtree::rb_delete_node_color(self.root_ptr(), nr.tree_entry_ptr()) };
        if removed.is_null() { return -ENOENT; }
        nr.to_none();
        self.dec_counter();
        fence(Ordering::SeqCst);
        0
    }

    /// Membership-checked removal. (Former free fn `ttree_remove_impl`,
    /// valid-queue path.)
    fn remove(&self, node: *mut tnode_t) -> c_int {
        if node.is_null() { return -EINVAL; }
        if !TnodeRef::from_ptr(node).map_or(false, |r| r.in_tree(self.as_ptr())) { return -EINVAL; }
        self.do_remove(node)
    }

    /// Park the calling thread on this tree keyed by `key`.
    /// (Former free fn `ttree_wait_cb_impl`, valid-queue path.)
    fn wait_cb(
        &self,
        key: uint64,
        sleep_callback: sleep_callback_t,
        wakeup_callback: wakeup_callback_t,
        callback_data: *mut c_void,
        rdata: *mut u64,
    ) -> c_int {
        let intr = Riscv::intr_off_save();
        let mut waiter: tnode_t = zeroed_tnode();
        let waiter_ptr = &raw mut waiter;
        let wr = TnodeRef::from_ptr(waiter_ptr).unwrap();
        wr.init_waiter();
        wr.set_error_no(-EINTR);
        wr.set_tree_key(key);

        if self.add(waiter_ptr) != 0 {
            xv6_panic(c"Failed to push thread to sleep tree".as_ptr());
        }

        let mut cb_status: c_int = 0;
        if let Some(cb) = sleep_callback {
            // SAFETY: `cb` is a caller-supplied `sleep_callback_t` fn-ptr
            // invoked with the matching `callback_data` (the spinlock ptr
            // for `wait`); the C-ABI callback contract is the caller's.
            cb_status = unsafe { cb(callback_data) };
        }
        Scheduler::yield_now();
        if let Some(cb) = wakeup_callback {
            // SAFETY: as above, for the paired `wakeup_callback_t`.
            unsafe { cb(callback_data, cb_status); }
        }

        if wr.is_enqueued() {
            if self.remove(waiter_ptr) != 0 {
                xv6_panic(c"Failed to remove interrupted waiter from tree".as_ptr());
            }
        }
        Riscv::intr_restore(intr);

        if !rdata.is_null() {
            write_out(rdata, wr.data());
        }
        wr.error_no()
    }

    /// Spinlock-backed keyed park. (Former free fn `ttree_wait_impl`,
    /// valid-queue path.) NO-STANDALONE-FN: `pub(super)` — sole caller is
    /// `crate::proc::sched` (former `ttree_wait` delegator deleted).
    #[inline]
    pub(super) fn wait(&self, key: uint64, lock: *mut spinlock_t, rdata: *mut u64) -> c_int {
        self.wait_cb(key, Some(spin_sleep_cb), Some(spin_wake_cb), lock as *mut c_void, rdata)
    }

    /// Wake the smallest-key waiter matching `key`. (Former free fn
    /// `ttree_wakeup_one_impl`, valid-queue path.)
    fn wakeup_one(&self, key: uint64, error_no: c_int, rdata: u64) -> *mut thread {
        let target = self.find_key_min(key);
        if target.is_null() { return err_ptr::<thread>(ENOENT); }
        let ret = self.do_remove(target);
        if ret != 0 { return err_ptr::<thread>(-ret); }
        TnodeRef::from_ptr(target).map_or(err_ptr::<thread>(EINVAL), |r| r.do_wakeup(error_no, rdata))
    }

    /// Wake every waiter matching `key`. (Former free fn
    /// `ttree_wakeup_key_impl`, valid-queue path.) Wake+dequeue drain ->
    /// stays a raw `loop`. NO-STANDALONE-FN: `pub(super)` — sole caller is
    /// `crate::proc::sched` (former `ttree_wakeup_key` delegator deleted).
    pub(super) fn wakeup_key(&self, key: uint64, error_no: c_int, rdata: u64) -> c_int {
        let mut count = 0;
        loop {
            let p = self.wakeup_one(key, error_no, rdata);
            if is_err_or_null(p) { break; }
            count += 1;
        }
        if count == 0 { -ENOENT } else { 0 }
    }
}

// ---------------------------------------------------------------------------
// NO-STANDALONE-FN: crate-facing entry points eliminated.
//
// The eleven thin free-fn delegators that used to live here --
//   tq_init, tq_size, tq_bulk_move, tq_wait_cb, tq_wait, tq_wakeup,
//   tq_wakeup_all  (were `pub(crate)`, called from lock/, vfs/, mm/,
//                    virtio, tty, and -- for `tq_wait` -- `crate::sync`'s
//                    `SpinLockGuard::wait_on`),
//   ttree_init, ttree_wait, ttree_wakeup_key, tnode_get_thread
//                   (were `pub(super)`, called only from `crate::proc::sched`)
// -- have been DELETED. Each was a null-check-then-forward wrapper; the
// null check is now performed at the call site, which constructs the
// `TqRef`/`TtreeRef`/`TnodeRef` handle via `from_ptr(...)` (returning
// `Option`) and invokes the corresponding inherent method above with the
// same null->error mapping the delegator encoded. See the per-method
// `NO-STANDALONE-FN` doc notes for the visibility each now carries.
//
// The former `xv6_tqport_*` C-ABI alias layer was collapsed in P3-1B2 and
// the `#[no_mangle] extern "C"` export surface dismantled in P3-D2a; with
// the delegators gone, the only remaining free fns are the `extern "C"`
// comparator/get-key trampolines whose ADDRESS is stored in the static
// `rb_root_opts` tables (`q_root_get_key`/`q_root_keys_cmp`/
// `q_root_keys_cmp_rdown`) -- genuine floor.
// ---------------------------------------------------------------------------
