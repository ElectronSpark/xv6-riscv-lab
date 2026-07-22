//! Intrusive hash list — Rust port of `kernel/hlist.c`.
//!
//! `struct hlist_struct` / `hlist_entry` are the real bindgen-generated
//! `crate::bindings::{hlist_struct, hlist_entry}` types (already
//! allowlisted in `build.rs`) — no opaque stand-ins. (The historical
//! `hlist_func_struct` fn-pointer table is gone — TRAIT-OPS replaced it
//! with the [`HlistOps`] trait below.) Each bucket (`hlist_bucket_t`) is
//! itself a
//! `list_node_t` sentinel head; `hlist_entry_t::list_entry` is the
//! *first* field of the struct, so every `*mut list_node_t` <->
//! `*mut hlist_entry_t` conversion below is an offset-0 reinterpretation,
//! not `container_of` arithmetic.
//!
//! `kernel/inc/list.h` and `kernel/inc/hlist.h` implement the underlying
//! doubly-linked-list primitives (`list_entry_init`, `list_node_push_front`,
//! `list_node_detach`, the RCU `list_entry_{add,del,replace}_rcu` family,
//! ...) as `static inline` C functions with no external linkage, so they
//! cannot be called from Rust. The `llist` submodule below reimplements
//! the exact subset this file needs, matching the C memory-ordering
//! choices one-for-one (`WRITE_ONCE` -> volatile store, `rcu_dereference`
//! -> `Acquire` load, `rcu_assign_pointer` -> `Release` store). Both
//! headers stay unchanged for the remaining C consumers that `#include`
//! them directly (`kernel/bio.c`, `kernel/vfs/fs.c`,
//! `kernel/vfs/tmpfs/inode.c`, `kernel/dev/fdt.c`, ...).
//!
//! Internal helpers with zero callers outside this file
//! (`__hlist_hash_bucket_init`, `__hlist_replace_node_entry`,
//! `__hlist_insert_node_entry`, `__hlist_remove_node_entry`, `__hlist_get`,
//! `__hlist_get_rcu`, and the header's private `__hlist_*` static-inlines
//! — verified by grepping every `.c`/`.h`/`.rs` file in the tree) are
//! ported as private, non-`#[no_mangle]` functions rather than kept as a
//! C-ABI surface nobody uses. Two provably-dead helpers
//! (`__hlist_calc_node_bucket`, defined but never called even within
//! `hlist.c` itself) are dropped outright.
//!
//! `__hlist_is_bucket_of`'s original C used `offset >= 0 || offset <
//! bucket_cnt` (an `||` that makes the upper-bound check a no-op — always
//! true whenever `offset >= 0`). This function has zero callers anywhere
//! in the tree, so the bug is inert; [`is_bucket_of`] below fixes it to
//! the evidently-intended `&&`.

#![allow(non_camel_case_types)]

use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::bindings::{hlist_entry, hlist_struct, list_node_t};

pub type Hlist = hlist_struct;
pub type HlistEntry = hlist_entry;
pub type HlistBucket = list_node_t;
pub type HtHash = u64;

const HLIST_BUCKET_CNT_MAX: u64 = 0xffff;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3C, nativized in Wave P3-N1.
//
// `RawHlistBucket`/`RawHlistEntry`/`RawHlist` ARE the kernel-wide Rust
// definitions of `kernel/inc/hlist_type.h`'s
// `hlist_bucket_t`/`hlist_entry_t`/`hlist_t` now: `build.rs` blocklists the
// bindgen-generated versions and injects `pub type hlist_* =
// crate::hlist::RawHlist*;` raw-line aliases, so every remaining bindgen
// struct that embeds one (`thread.proctab_entry`, `tmpfs_inode`/
// `tmpfs_dentry` hash entries, ...) and every `crate::bindings::hlist_*`
// path across the crate resolves here. The `Hlist`/`HlistEntry`/
// `HlistBucket` aliases above keep resolving through `crate::bindings`, so
// the ~10 cross-module call sites (`vfs/fs.rs`, `vfs/tmpfs/{superblock,
// inode}.rs`, `proc/proc_shims.rs`, `proc/access.rs`, `bufcache.rs`, ...)
// compile unchanged. `hlist_func_struct`/`hlist_func_t` (the old
// `RawHlistFunc` fn-pointer table) are GONE as of the TRAIT-OPS conversion
// -- replaced by the [`HlistOps`] trait, with `Option<&'static dyn
// HlistOps>` as the new `RawHlist::ops` storage. Layout is proven by the
// hardcoded `const _` asserts below.
//
// `hlist_bucket_t` is *literally* `list_node_t` (`kernel/inc/
// hlist_type.h`: `typedef struct list_node hlist_bucket_t;` -- confirmed
// against the real bindgen output: `pub type hlist_bucket_t =
// list_node;`), so its native mirror is exactly `crate::list::ListNode`
// (Wave P3-3C's other leaf type) -- no second struct definition needed.
//
// Deliberately NOT wired into this file's own implementation for the
// same reason as `bintree.rs`'s `RawRbNode`: every function below
// already does plain, correctly-typed field access on the bindgen alias
// (no reinterpret-cast chain to simplify), so rewiring would only
// relocate the casting work without an unsafe-reduction payoff.
pub type RawHlistBucket = crate::list::ListNode;

// ---------------------------------------------------------------------------
// `HlistOps` — TRAIT-OPS conversion of the old `hlist_func_struct`
// fn-pointer table (4 raw `Option<unsafe extern "C" fn(...)>` slots) into a
// single trait, `Cdev`/`Pcache`/`Netdev`/`IrqHandler` precedent.
//
// None-semantics: ALL FOUR methods are REQUIRED (no defaulted body), unlike
// e.g. `PcacheOps`'s optional slots. Evidence from every call site below:
// `Hlist::validate()` (the sole gate every dispatching entry point
// -- `get`/`put`/`pop`/`len`/`*_rcu` -- runs before touching `func`) rejected
// a hlist unless `cmp_node`/`get_node`/`hash`/`get_entry` were ALL `Some`,
// and `Hlist::init()` refused construction unless all four were supplied
// together. No table anywhere in the tree (grep-confirmed: bufcache, fdt
// compat/phandle, proctab, tmpfs dir, vfs sb-inode) ever populated a subset
// -- it was always "all four or none". So the *whole table* collapses to a
// single `Option<&'static dyn HlistOps>` on [`RawHlist`]: `None` reproduces
// the old all-`None` (freshly zeroed, pre-`init`) state exactly, and `Some`
// reproduces the old all-`Some` (post-`init`) state exactly; the individual
// `call_*` dispatchers below still each document their own (per-method)
// contract, matching the original per-slot doc comments.
pub trait HlistOps: Sync {
    /// Compute this node's hash. Mirrors the old `func.hash` slot.
    ///
    /// # Safety
    /// `node` must be a live, caller-owned node this hlist knows how to
    /// hash (a lookup key or a registered node, per the caller's contract).
    unsafe fn hash(&self, node: *mut c_void) -> HtHash;

    /// Recover the owning node from one of its linked hlist entries.
    /// Mirrors the old `func.get_node` slot.
    ///
    /// # Safety
    /// `entry` must be a live [`HlistEntry`] currently linked into one of
    /// this hlist's buckets.
    unsafe fn get_node(&self, entry: *mut HlistEntry) -> *mut c_void;

    /// Recover a node's embedded hlist entry. Mirrors the old
    /// `func.get_entry` slot.
    ///
    /// # Safety
    /// `node` must be a live, caller-owned node.
    unsafe fn get_entry(&self, node: *mut c_void) -> *mut HlistEntry;

    /// Compare two nodes for hash-bucket equality (`0` iff equal). Mirrors
    /// the old `func.cmp_node` slot.
    ///
    /// # Safety
    /// `hlist` must be the live hlist this ops table is installed on;
    /// `n1`/`n2` must be live nodes as passed by the caller.
    unsafe fn cmp_node(&self, hlist: *mut Hlist, n1: *mut c_void, n2: *mut c_void) -> i32;
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RawHlistEntry {
    pub list_entry: RawHlistBucket,
    pub bucket: *mut RawHlistBucket,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RawHlist {
    pub bucket_cnt: u64,
    pub elem_cnt: i64,
    /// The hlist's dispatch table -- a real Rust trait object,
    /// `Option<&'static dyn HlistOps>` (a 16-byte fat pointer; `None` is
    /// the pre-`Hlist::init` / zeroed placeholder state, matching the old
    /// all-`None` `func` value bit-for-bit: `Option<&dyn Trait>` niches on
    /// the data pointer being null, so an all-zero `RawHlist` still reads
    /// back `ops == None`, exactly like the old all-`None` `RawHlistFunc`
    /// did under `MaybeUninit::zeroed()` in `bufcache.rs`/`proc_shims.rs`).
    pub ops: Option<&'static dyn HlistOps>,
    pub buckets: [RawHlistBucket; 0],
}

// P3-N1 hardcoded layout proof — values captured from the pre-nativization
// bindgen output (kernel_bindings.rs) for `kernel/inc/hlist_type.h`'s
// `hlist_bucket_t` (== `struct list_node`) and `struct hlist_entry`
// (list_node_t + pointer). All 8-byte members, no padding, on
// riscv64/lp64d.
//
// TRAIT-OPS: `struct hlist_func_struct` (4 function pointers, 32 bytes) is
// GONE -- replaced by the `HlistOps` trait above -- so `hlist_struct`
// shrinks: `u64 + i64 + 16-byte `Option<&dyn HlistOps>` fat pointer +
// [bucket; 0]` = 32 bytes (was 48). Every embedder (`BCacheHash.cached`,
// `ProcTable.procs`, `VfsSuperblock.inodes`, `TmpfsDirData.children`) feels
// this shrink in its OWN following-field offsets -- each file's own
// layout-assert block documents its honest new numbers (see
// `bufcache.rs`/`proc/proc_shims.rs`/`vfs/fs.rs`/`vfs/tmpfs/inode.rs`).
const _: () = {
    assert!(core::mem::size_of::<RawHlistBucket>() == 16, "hlist_bucket_t == list_node: 2 x 8-byte pointer");
    assert!(core::mem::align_of::<RawHlistBucket>() == 8, "hlist_bucket_t: natural pointer alignment");

    assert!(core::mem::size_of::<RawHlistEntry>() == 24, "hlist_entry: list_node (16) + bucket pointer (8)");
    assert!(core::mem::align_of::<RawHlistEntry>() == 8, "hlist_entry: natural pointer alignment");
    assert!(core::mem::offset_of!(RawHlistEntry, list_entry) == 0, "hlist_entry.list_entry offset");
    assert!(core::mem::offset_of!(RawHlistEntry, bucket) == 16, "hlist_entry.bucket offset");

    assert!(
        core::mem::size_of::<Option<&'static dyn HlistOps>>() == 16,
        "hlist ops fat pointer size"
    );
    assert!(
        core::mem::align_of::<Option<&'static dyn HlistOps>>() == 8,
        "hlist ops fat pointer alignment"
    );

    assert!(core::mem::size_of::<RawHlist>() == 32, "hlist_struct: u64 + i64 + 16-byte ops fat pointer + [bucket; 0]");
    assert!(core::mem::align_of::<RawHlist>() == 8, "hlist_struct: natural 8-byte alignment");
    assert!(core::mem::offset_of!(RawHlist, bucket_cnt) == 0, "hlist_struct.bucket_cnt offset");
    assert!(core::mem::offset_of!(RawHlist, elem_cnt) == 8, "hlist_struct.elem_cnt offset");
    assert!(core::mem::offset_of!(RawHlist, ops) == 16, "hlist_struct.ops offset");
    assert!(core::mem::offset_of!(RawHlist, buckets) == 32, "hlist_struct.buckets (flexible array) offset");
};

// ---------------------------------------------------------------------------
// Minimal intrusive doubly-linked-list primitives (private re-implementation
// of the relevant subset of `kernel/inc/list.h`).
// ---------------------------------------------------------------------------
mod llist {
    use super::*;

    /// Mirrors `list_entry_init()`.
    ///
    /// # Safety
    /// `e` must point to a live `list_node_t`.
    #[inline(always)]
    pub(super) unsafe fn init(e: *mut list_node_t) {
        unsafe {
            (*e).next = e;
            (*e).prev = e;
        }
    }

    /// Mirrors `LIST_ENTRY_IS_DETACHED(e)` / `LIST_IS_EMPTY(e)` (same
    /// formula: a self-linked node is either a detached entry or an empty
    /// list head).
    ///
    /// # Safety
    /// `e` must point to a live `list_node_t`.
    #[inline(always)]
    pub(super) unsafe fn is_self_linked(e: *mut list_node_t) -> bool {
        unsafe { (*e).next == e }
    }

    /// Mirrors `list_entry_detach()`.
    ///
    /// # Safety
    /// `e` must point to a live, linked `list_node_t`.
    #[inline(always)]
    pub(super) unsafe fn detach(e: *mut list_node_t) {
        unsafe {
            let prev = (*e).prev;
            let next = (*e).next;
            (*prev).next = next;
            (*next).prev = prev;
            init(e);
        }
    }

    /// Mirrors `list_entry_insert(prev, e)`.
    ///
    /// # Safety
    /// `prev` and `e` must point to live `list_node_t`s; `prev` must
    /// already be linked into a list (or be a list head).
    #[inline(always)]
    unsafe fn insert_after(prev: *mut list_node_t, e: *mut list_node_t) {
        unsafe {
            let next = (*prev).next;
            (*e).prev = prev;
            (*e).next = next;
            (*prev).next = e;
            (*next).prev = e;
        }
    }

    /// Mirrors `list_entry_push_front(head, e)` /
    /// `list_node_push_front(head, node, member)`.
    ///
    /// # Safety
    /// Same as [`insert_after`].
    #[inline(always)]
    pub(super) unsafe fn push_front(head: *mut list_node_t, e: *mut list_node_t) {
        unsafe { insert_after(head, e) };
    }

    /// Mirrors `list_entry_replace(old, new)`.
    ///
    /// # Safety
    /// `old` and `new` must point to live `list_node_t`s.
    #[inline(always)]
    pub(super) unsafe fn replace(old: *mut list_node_t, new: *mut list_node_t) {
        unsafe {
            if old.is_null() || new.is_null() {
                return;
            }
            init(new);
            if !is_self_linked(old) {
                let prev = (*old).prev;
                detach(old);
                insert_after(prev, new);
            }
        }
    }

    // --- RCU primitives -----------------------------------------------
    //
    // `WRITE_ONCE`/`READ_ONCE` (`kernel/inc/smp/atomic.h`) are plain
    // volatile accesses (a compiler barrier, no cross-hart ordering).
    // `rcu_assign_pointer`/`rcu_dereference` (`kernel/inc/lock/rcu.h`) are
    // a genuine release-store / (consume, approximated here by acquire-)
    // load pair. The C original uses each independently per call site
    // (e.g. `list_entry_del_rcu` unlinks via `WRITE_ONCE`, not
    // `rcu_assign_pointer`) — mirrored exactly below.

    #[inline(always)]
    unsafe fn write_once(dst: *mut *mut list_node_t, val: *mut list_node_t) {
        unsafe { ptr::write_volatile(dst, val) };
    }

    #[inline(always)]
    unsafe fn rcu_assign(dst: *mut *mut list_node_t, val: *mut list_node_t) {
        // SAFETY: `dst` is a valid, aligned `*mut list_node_t` field slot;
        // `AtomicPtr<list_node_t>` has the same layout as `*mut list_node_t`.
        unsafe { (*(dst as *const AtomicPtr<list_node_t>)).store(val, Ordering::Release) };
    }

    /// Mirrors `list_next_rcu(e)` = `rcu_dereference(e->next)`.
    ///
    /// # Safety
    /// `e` must point to a live `list_node_t`.
    #[inline(always)]
    pub(super) unsafe fn next_rcu(e: *mut list_node_t) -> *mut list_node_t {
        // SAFETY: see `rcu_assign`. Rust has no `memory_order_consume`;
        // `Acquire` is the closest available ordering and matches every
        // other RCU-consume site already ported in this crate
        // (`kernel/lock/rcu.rs`).
        unsafe { (*(ptr::addr_of!((*e).next) as *const AtomicPtr<list_node_t>)).load(Ordering::Acquire) }
    }

    /// Mirrors `list_entry_add_rcu(head, e)`.
    ///
    /// # Safety
    /// `head` and `e` must point to live `list_node_t`s; the caller holds
    /// whatever lock serializes writers (readers may run concurrently).
    pub(super) unsafe fn add_rcu(head: *mut list_node_t, e: *mut list_node_t) {
        unsafe {
            let next = (*head).next;
            (*e).next = next;
            (*e).prev = head;
            rcu_assign(ptr::addr_of_mut!((*head).next), e);
            (*next).prev = e;
        }
    }

    /// Mirrors `list_entry_del_rcu(e)`. Does NOT reinitialize `e` —
    /// concurrent readers may still be traversing it.
    ///
    /// # Safety
    /// `e` must point to a live, linked `list_node_t`; the caller holds
    /// whatever lock serializes writers and defers freeing `e` past the
    /// next grace period.
    pub(super) unsafe fn del_rcu(e: *mut list_node_t) {
        unsafe {
            let prev = (*e).prev;
            let next = (*e).next;
            write_once(ptr::addr_of_mut!((*prev).next), next);
            (*next).prev = prev;
        }
    }

    /// Mirrors `list_entry_replace_rcu(old, new)`.
    ///
    /// # Safety
    /// `old` and `new` must point to live `list_node_t`s; same writer/RCU
    /// contract as [`del_rcu`].
    pub(super) unsafe fn replace_rcu(old: *mut list_node_t, new: *mut list_node_t) {
        unsafe {
            (*new).next = (*old).next;
            (*new).prev = (*old).prev;
            rcu_assign(ptr::addr_of_mut!((*(*new).prev).next), new);
            (*(*new).next).prev = new;
        }
    }
}

// ---------------------------------------------------------------------------
// `hlist_entry_t::list_entry` is at offset 0 -> trivial reinterpret casts.
// ---------------------------------------------------------------------------

impl HlistEntry {
    #[inline(always)]
    fn from_node(n: *mut HlistBucket) -> *mut HlistEntry {
        n as *mut HlistEntry
    }
}

impl HlistBucket {
    #[inline(always)]
    fn from_entry(e: *mut HlistEntry) -> *mut HlistBucket {
        e as *mut HlistBucket
    }
}

// ---------------------------------------------------------------------------
// `BucketIter` — additive lazy iterator over the entries chained in a single
// hlist bucket (goal #2). Same anti-churn, raw-yield contract as
// `bintree.rs`'s `RbTreeIter` (git 856d205): a bucket is a `list_node_t`
// sentinel head, and this walks `head.next` around the ring until it returns
// to the head, yielding each linked node reinterpreted as a `*mut HlistEntry`
// (offset-0 cast). It yields **raw pointers, never `&HlistEntry`**, so it
// creates no `&Freeze`-into-a-loop borrow of the intrusive nodes — the exact
// hazard the memory `freeze-noalias-hazard` note warns against.
//
// NON-RCU ONLY: `next()` reads `(*cur).next` with a plain field load, so it
// is valid only on the writer-serialized (externally locked) lookup path.
// The RCU read-side walk (`find_entry_in_bucket_rcu`) keeps its hand-rolled
// `llist::next_rcu` acquire-load form — an RCU-consume boundary, not a plain
// chain, left raw per the crate's "RCU edges stay raw" convention.
struct BucketIter {
    head: *mut HlistBucket,
    cur: *mut HlistBucket,
}

impl BucketIter {
    /// Seed a walk at `bucket`'s first linked node.
    ///
    /// # Safety
    /// `bucket` must be a live bucket sentinel head; the caller serializes
    /// against writers for the whole walk (the non-RCU contract).
    #[inline(always)]
    unsafe fn new(bucket: *mut HlistBucket) -> Self {
        // SAFETY: caller contract — `bucket` is a live sentinel head.
        BucketIter { head: bucket, cur: unsafe { (*bucket).next } }
    }
}

impl Iterator for BucketIter {
    type Item = *mut HlistEntry;

    #[inline]
    fn next(&mut self) -> Option<*mut HlistEntry> {
        if self.cur == self.head {
            return None;
        }
        let entry = HlistEntry::from_node(self.cur);
        // SAFETY: `cur != head`, so it is a live entry node currently linked
        // into the bucket ring; advancing to its `.next` under the caller's
        // writer-serialization contract (see `BucketIter::new`).
        self.cur = unsafe { (*self.cur).next };
        Some(entry)
    }
}

// ---------------------------------------------------------------------------
// hlist_entry_{add,del,replace}_rcu — `static inline` in `kernel/inc/hlist.h`
// (no external linkage); reimplemented on top of `llist` for `hlist_c`'s
// own RCU write paths (`hlist_put_rcu`/`hlist_pop_rcu`).
//
// KERNEL-OO: relocated onto `impl Hlist`/`impl HlistEntry` (raw params, no
// `&self` — `Hlist`/`HlistEntry` are `Freeze`, intrusively linked, and
// mutated cross-hart under external locking, same rule as every other
// method below).
// ---------------------------------------------------------------------------

impl Hlist {
    /// # Safety
    /// `hlist`, `bucket`, `entry` must all point to live values; caller holds
    /// the writer-side lock.
    unsafe fn entry_add_rcu(hlist: *mut Hlist, bucket: *mut HlistBucket, entry: *mut HlistEntry) {
        unsafe {
            llist::add_rcu(bucket, HlistBucket::from_entry(entry));
            ptr::write_volatile(ptr::addr_of_mut!((*entry).bucket), bucket);
            (*hlist).elem_cnt += 1;
        }
    }

    /// # Safety
    /// `hlist` and `entry` must point to live values; caller holds the
    /// writer-side lock. Does not clear `entry->bucket` (matches C: readers
    /// may still be checking it).
    unsafe fn entry_del_rcu(hlist: *mut Hlist, entry: *mut HlistEntry) {
        unsafe {
            llist::del_rcu(HlistBucket::from_entry(entry));
            (*hlist).elem_cnt -= 1;
        }
    }
}

impl HlistEntry {
    /// # Safety
    /// `old` and `new` must point to live entries; caller holds the
    /// writer-side lock.
    unsafe fn replace_rcu(old: *mut HlistEntry, new: *mut HlistEntry) {
        unsafe {
            llist::replace_rcu(HlistBucket::from_entry(old), HlistBucket::from_entry(new));
            ptr::write_volatile(ptr::addr_of_mut!((*new).bucket), (*old).bucket);
        }
    }
}

// ---------------------------------------------------------------------------
// Callback dispatch through `hlist->func`, and the bucket-array/lookup
// helpers below it — all relocated onto `impl Hlist` (raw params, no
// `&self`; see the doc comment on the RCU-entry impl blocks above).
// ---------------------------------------------------------------------------

impl Hlist {
    /// # Safety
    /// `hlist` must point to a live `Hlist` with `ops` installed; `node`
    /// per the trait method's own contract (non-null, caller-supplied).
    #[inline(always)]
    unsafe fn call_hash(hlist: *mut Hlist, node: *mut c_void) -> HtHash {
        unsafe { (*hlist).ops.expect("BUG: hlist: ops is None").hash(node) }
    }

    /// # Safety
    /// Same contract as [`Hlist::call_hash`], for `HlistOps::get_node`.
    #[inline(always)]
    unsafe fn call_get_node(hlist: *mut Hlist, entry: *mut HlistEntry) -> *mut c_void {
        unsafe { (*hlist).ops.expect("BUG: hlist: ops is None").get_node(entry) }
    }

    /// # Safety
    /// Same contract as [`Hlist::call_hash`], for `HlistOps::get_entry`.
    #[inline(always)]
    unsafe fn call_get_entry(hlist: *mut Hlist, node: *mut c_void) -> *mut HlistEntry {
        unsafe { (*hlist).ops.expect("BUG: hlist: ops is None").get_entry(node) }
    }

    /// # Safety
    /// Same contract as [`Hlist::call_hash`], for `HlistOps::cmp_node`.
    #[inline(always)]
    unsafe fn call_cmp_node(hlist: *mut Hlist, n1: *mut c_void, n2: *mut c_void) -> i32 {
        unsafe { (*hlist).ops.expect("BUG: hlist: ops is None").cmp_node(hlist, n1, n2) }
    }

    /// # Safety
    /// `hlist` must point to a live `Hlist` whose `buckets` flexible-array
    /// member has been sized to at least `idx + 1` entries.
    #[inline(always)]
    unsafe fn bucket_at(hlist: *mut Hlist, idx: u64) -> *mut HlistBucket {
        unsafe { (*hlist).buckets.as_mut_ptr().add(idx as usize) }
    }

    /// Mirrors `__hlist_is_bucket_of` with the `||` -> `&&` fix (see module
    /// doc comment): `Some(index)` iff `bucket` is one of `hlist`'s own
    /// buckets.
    ///
    /// # Safety
    /// `hlist` must point to a live `Hlist`.
    unsafe fn bucket_index(hlist: *mut Hlist, bucket: *mut HlistBucket) -> Option<u64> {
        unsafe {
            let base = (*hlist).buckets.as_mut_ptr() as usize;
            let b = bucket as usize;
            let elem_size = core::mem::size_of::<HlistBucket>();
            if b < base {
                return None;
            }
            let byte_diff = b - base;
            if byte_diff % elem_size != 0 {
                return None;
            }
            let idx = (byte_diff / elem_size) as u64;
            if idx < (*hlist).bucket_cnt {
                Some(idx)
            } else {
                None
            }
        }
    }

    /// # Safety
    /// `hlist` must point to a live `Hlist`; `bucket` must be null or point
    /// to a live `HlistBucket` (not necessarily one of `hlist`'s own).
    #[inline(always)]
    unsafe fn is_bucket_of(hlist: *mut Hlist, bucket: *mut HlistBucket) -> bool {
        unsafe { Self::bucket_index(hlist, bucket).is_some() }
    }

    /// # Safety
    /// `hlist` must be null or point to a live `Hlist`.
    unsafe fn validate(hlist: *mut Hlist) -> bool {
        if hlist.is_null() {
            return false;
        }
        unsafe {
            if (*hlist).bucket_cnt == 0 {
                return false;
            }
            // TRAIT-OPS: the four old per-slot `is_some()` checks collapse
            // into one -- every table in the tree was always "all four
            // slots or none" (see `HlistOps`'s doc comment), so a single
            // `ops.is_some()` is behaviorally identical.
            (*hlist).ops.is_some()
        }
    }

    /// # Safety
    /// `hlist` must be null or point to a live `Hlist`; `node` must be null
    /// or a live caller-owned node.
    unsafe fn node_bucket(hlist: *mut Hlist, node: *mut c_void) -> *mut HlistBucket {
        unsafe {
            if hlist.is_null() || node.is_null() {
                return ptr::null_mut();
            }
            let entry = Self::call_get_entry(hlist, node);
            if entry.is_null() {
                return ptr::null_mut();
            }
            (*entry).bucket
        }
    }

    /// # Safety
    /// `hlist` must point to a valid, non-zero-bucket-count `Hlist`; `hash`
    /// need not be validated beyond that (`hash % bucket_cnt` is always
    /// in-range).
    #[inline(always)]
    unsafe fn hash_bucket(hlist: *mut Hlist, hash: HtHash) -> *mut HlistBucket {
        unsafe { Self::bucket_at(hlist, hash % (*hlist).bucket_cnt) }
    }

    /// Mirrors `__hlist_find_entry_in_bucket` (non-RCU `list_foreach_node_safe`
    /// walk).
    ///
    /// # Safety
    /// `hlist` must be validated; `bucket` must be a live bucket head of
    /// `*hlist`; `node` is passed through to `func.cmp_node`/`func.get_node`.
    unsafe fn find_entry_in_bucket(hlist: *mut Hlist, bucket: *mut HlistBucket, node: *mut c_void) -> *mut HlistEntry {
        // SAFETY: caller contract — `bucket` is a live bucket head of `*hlist`,
        // walked under the writer-serialization the non-RCU path assumes. The
        // find is read-only (no node is unlinked mid-walk), so the C original's
        // "_safe" capture-next is behaviourally redundant here and the plain
        // forward `BucketIter` reproduces it exactly.
        unsafe { BucketIter::new(bucket) }
            .find(|&entry| {
                // SAFETY: `entry` is a live linked entry from `bucket`'s chain;
                // `func.get_node`/`func.cmp_node` are the hlist's validated
                // callbacks (checked by `validate` before this is reached).
                let node1 = unsafe { Self::call_get_node(hlist, entry) };
                unsafe { Self::call_cmp_node(hlist, node1, node) == 0 }
            })
            .unwrap_or(ptr::null_mut())
    }

    /// Mirrors `__hlist_find_entry_in_bucket_rcu`.
    ///
    /// # Safety
    /// Same contract as [`Hlist::find_entry_in_bucket`], but must be called
    /// from within an RCU read-side critical section.
    unsafe fn find_entry_in_bucket_rcu(hlist: *mut Hlist, bucket: *mut HlistBucket, node: *mut c_void) -> *mut HlistEntry {
        unsafe {
            let mut cur = llist::next_rcu(bucket);
            while !cur.is_null() && cur != bucket {
                let entry = HlistEntry::from_node(cur);
                let node1 = Self::call_get_node(hlist, entry);
                if Self::call_cmp_node(hlist, node1, node) == 0 {
                    return entry;
                }
                cur = llist::next_rcu(cur);
            }
            ptr::null_mut()
        }
    }

    /// Mirrors `__hlist_get` (returns `(bucket, entry)` instead of writing
    /// through two out-params — internal-only, so no ABI to preserve here).
    ///
    /// # Safety
    /// `hlist` must be validated; `node` is passed through to `func.hash`.
    unsafe fn bucket_entry(hlist: *mut Hlist, node: *mut c_void) -> (*mut HlistBucket, *mut HlistEntry) {
        unsafe {
            let hash_val = Self::call_hash(hlist, node);
            if hash_val == 0 {
                return (ptr::null_mut(), ptr::null_mut());
            }
            let bucket = Self::hash_bucket(hlist, hash_val);
            let entry = Self::find_entry_in_bucket(hlist, bucket, node);
            (bucket, entry)
        }
    }

    /// Mirrors `__hlist_get_rcu`.
    ///
    /// # Safety
    /// Same contract as [`Hlist::bucket_entry`], RCU read-side.
    unsafe fn bucket_entry_rcu(hlist: *mut Hlist, node: *mut c_void) -> (*mut HlistBucket, *mut HlistEntry) {
        unsafe {
            let hash_val = Self::call_hash(hlist, node);
            if hash_val == 0 {
                return (ptr::null_mut(), ptr::null_mut());
            }
            let bucket = Self::hash_bucket(hlist, hash_val);
            let entry = Self::find_entry_in_bucket_rcu(hlist, bucket, node);
            (bucket, entry)
        }
    }

    /// # Safety
    /// `hlist` must point to a live `Hlist`; `bucket` must be a live bucket
    /// head of `*hlist`; `entry` must point to a live, detached `HlistEntry`.
    unsafe fn insert_node_entry(hlist: *mut Hlist, bucket: *mut HlistBucket, entry: *mut HlistEntry) {
        unsafe {
            llist::push_front(bucket, HlistBucket::from_entry(entry));
            (*entry).bucket = bucket;
            (*hlist).elem_cnt += 1;
        }
    }

    /// # Safety
    /// `hlist` must point to a live `Hlist`; `entry` must point to a live,
    /// linked `HlistEntry`.
    unsafe fn remove_node_entry(hlist: *mut Hlist, entry: *mut HlistEntry) {
        unsafe {
            llist::detach(HlistBucket::from_entry(entry));
            (*entry).bucket = ptr::null_mut();
            (*hlist).elem_cnt -= 1;
        }
    }
}

impl HlistEntry {
    /// Mirrors `__hlist_replace_node_entry`: replace `old` in its bucket with
    /// `new` (non-RCU).
    ///
    /// # Safety
    /// `old` must point to a live, linked `HlistEntry`; `new` must point to a
    /// live, detached `HlistEntry`.
    unsafe fn replace_node(old: *mut HlistEntry, new: *mut HlistEntry) {
        unsafe {
            llist::replace(HlistBucket::from_entry(old), HlistBucket::from_entry(new));
            (*new).bucket = (*old).bucket;
            (*old).bucket = ptr::null_mut();
        }
    }
}

// ---------------------------------------------------------------------------
// Public C ABI — exact symbol/signature parity with `kernel/inc/hlist.h`.
// Every `hlist_*` free fn here relocates onto `impl Hlist` with its
// redundant `hlist_` prefix dropped (`hlist_init` -> `Hlist::init`, ...),
// matching the crate-wide convention (see e.g. `SessionTable`/`Session` in
// `tty/session.rs`). No `#[no_mangle]`/`extern "C"` remains on any of
// these (already dropped in wave P3-D3c, verified by grep — see report).
// ---------------------------------------------------------------------------

impl Hlist {
    /// # Safety
    /// `hlist` must be null or point to a live `Hlist`; `node` must be null
    /// or a live caller-owned node whose `func.get_entry` callback is safe to
    /// invoke.
    pub(crate) unsafe fn node_in_list(hlist: *mut Hlist, node: *mut c_void) -> bool {
        unsafe {
            let bucket = Self::node_bucket(hlist, node);
            if bucket.is_null() || hlist.is_null() {
                return false;
            }
            Self::is_bucket_of(hlist, bucket)
        }
    }

    /// # Safety
    /// `hlist` must be null or point to writable, zero-or-more-buckets-sized
    /// storage.
    ///
    /// TRAIT-OPS: `func: *mut HlistFunc` (a pointer to a 4-slot fn-pointer
    /// struct, copied wholesale into `(*hlist).func`) became `ops:
    /// Option<&'static dyn HlistOps>` (the value itself -- no indirection
    /// needed, a trait object reference is already a plain value to copy).
    /// `func.is_null() || (*func).slot.is_none()` (any of 4) collapses to
    /// `ops.is_none()` -- see [`HlistOps`]'s doc comment for why that's
    /// behaviorally identical.
    pub(crate) unsafe fn init(hlist: *mut Hlist, bucket_cnt: u64, ops: Option<&'static dyn HlistOps>) -> i32 {
        unsafe {
            if hlist.is_null() || ops.is_none() {
                return -1;
            }
            if bucket_cnt == 0 || bucket_cnt > HLIST_BUCKET_CNT_MAX {
                return -1;
            }

            for i in 0..bucket_cnt {
                llist::init(Self::bucket_at(hlist, i));
            }

            (*hlist).bucket_cnt = bucket_cnt;
            (*hlist).ops = ops;
            (*hlist).elem_cnt = 0;

            0
        }
    }

    /// # Safety
    /// `hlist` must be null or point to a live `Hlist`; `node` must be null
    /// or a live caller-owned node.
    pub(crate) unsafe fn get_node_hash(hlist: *mut Hlist, node: *mut c_void) -> HtHash {
        unsafe {
            if hlist.is_null() || node.is_null() {
                return 0;
            }
            if (*hlist).ops.is_none() {
                return 0;
            }
            Self::call_hash(hlist, node)
        }
    }

    /// # Safety
    /// `hlist` must be null or point to a live `Hlist`; `node` must be null
    /// or a live caller-owned node.
    pub(crate) unsafe fn get(hlist: *mut Hlist, node: *mut c_void) -> *mut c_void {
        unsafe {
            if node.is_null() {
                return ptr::null_mut();
            }
            if !Self::validate(hlist) {
                return ptr::null_mut();
            }
            let (_, entry) = Self::bucket_entry(hlist, node);
            if entry.is_null() {
                return ptr::null_mut();
            }
            Self::call_get_node(hlist, entry)
        }
    }

    /// # Safety
    /// `hlist` must be null or point to a live `Hlist`; `node` must be a live
    /// caller-owned node.
    pub(crate) unsafe fn put(hlist: *mut Hlist, node: *mut c_void, replace: bool) -> *mut c_void {
        unsafe {
            if !Self::validate(hlist) {
                return node;
            }
            let new_entry = Self::call_get_entry(hlist, node);
            if new_entry.is_null() {
                return node;
            }
            if !(*new_entry).bucket.is_null() {
                // HLIST_ENTRY_ATTACHED: cannot insert an already-attached node.
                return node;
            }

            let (bucket, entry) = Self::bucket_entry(hlist, node);
            if bucket.is_null() {
                return node;
            }

            if entry.is_null() {
                Self::insert_node_entry(hlist, bucket, new_entry);
                return ptr::null_mut();
            }

            let old_node = Self::call_get_node(hlist, entry);
            if old_node.is_null() {
                return node;
            } else if node == old_node {
                return node;
            }
            if replace {
                HlistEntry::replace_node(entry, new_entry);
            }
            old_node
        }
    }

    /// # Safety
    /// `hlist` must be null or point to a live `Hlist`; `node` must be null
    /// or a live caller-owned node.
    pub(crate) unsafe fn pop(hlist: *mut Hlist, node: *mut c_void) -> *mut c_void {
        unsafe {
            if !Self::validate(hlist) {
                return ptr::null_mut();
            }
            if (*hlist).elem_cnt == 0 {
                return ptr::null_mut();
            }

            if node.is_null() {
                for i in 0..(*hlist).bucket_cnt {
                    let bucket = Self::bucket_at(hlist, i);
                    if !llist::is_self_linked(bucket) {
                        let entry = HlistEntry::from_node((*bucket).next);
                        let ret_node = Self::call_get_node(hlist, entry);
                        Self::remove_node_entry(hlist, entry);
                        return ret_node;
                    }
                }
                return ptr::null_mut();
            }

            let (_, entry) = Self::bucket_entry(hlist, node);
            if !entry.is_null() {
                let ret_node = Self::call_get_node(hlist, entry);
                if !ret_node.is_null() {
                    Self::remove_node_entry(hlist, entry);
                }
                ret_node
            } else {
                ptr::null_mut()
            }
        }
    }

    /// # Safety
    /// `hlist` must be null or point to a live `Hlist`.
    pub(crate) unsafe fn len(hlist: *mut Hlist) -> usize {
        unsafe {
            if !Self::validate(hlist) {
                return 0;
            }
            (*hlist).elem_cnt as usize
        }
    }

    /// # Safety
    /// Same contract as [`Hlist::get`]; caller holds `rcu_read_lock()`.
    pub(crate) unsafe fn get_rcu(hlist: *mut Hlist, node: *mut c_void) -> *mut c_void {
        unsafe {
            if node.is_null() {
                return ptr::null_mut();
            }
            if !Self::validate(hlist) {
                return ptr::null_mut();
            }
            let (_, entry) = Self::bucket_entry_rcu(hlist, node);
            if entry.is_null() {
                return ptr::null_mut();
            }
            Self::call_get_node(hlist, entry)
        }
    }

    /// # Safety
    /// Same contract as [`Hlist::put`]; caller serializes writers externally
    /// (e.g. holds a lock) and defers freeing any replaced node past the next
    /// RCU grace period.
    pub(crate) unsafe fn put_rcu(hlist: *mut Hlist, node: *mut c_void, replace: bool) -> *mut c_void {
        unsafe {
            if !Self::validate(hlist) {
                return node;
            }
            let new_entry = Self::call_get_entry(hlist, node);
            if new_entry.is_null() {
                return node;
            }
            if !(*new_entry).bucket.is_null() {
                return node;
            }

            // Non-RCU lookup: the writer already holds whatever lock
            // serializes writers (matches the C comment on `hlist_put_rcu`).
            let (bucket, entry) = Self::bucket_entry(hlist, node);
            if bucket.is_null() {
                return node;
            }

            if entry.is_null() {
                Self::entry_add_rcu(hlist, bucket, new_entry);
                return ptr::null_mut();
            }

            let old_node = Self::call_get_node(hlist, entry);
            if old_node.is_null() {
                return node;
            } else if node == old_node {
                return node;
            }
            if replace {
                HlistEntry::replace_rcu(entry, new_entry);
            }
            old_node
        }
    }

    /// # Safety
    /// Same contract as [`Hlist::pop`]; caller serializes writers externally
    /// and defers freeing the returned node past the next RCU grace period.
    pub(crate) unsafe fn pop_rcu(hlist: *mut Hlist, node: *mut c_void) -> *mut c_void {
        unsafe {
            if !Self::validate(hlist) {
                return ptr::null_mut();
            }
            if (*hlist).elem_cnt == 0 {
                return ptr::null_mut();
            }

            if node.is_null() {
                for i in 0..(*hlist).bucket_cnt {
                    let bucket = Self::bucket_at(hlist, i);
                    if !llist::is_self_linked(bucket) {
                        let entry = HlistEntry::from_node((*bucket).next);
                        let ret_node = Self::call_get_node(hlist, entry);
                        Self::entry_del_rcu(hlist, entry);
                        return ret_node;
                    }
                }
                return ptr::null_mut();
            }

            let (_, entry) = Self::bucket_entry(hlist, node);
            if !entry.is_null() {
                let ret_node = Self::call_get_node(hlist, entry);
                if !ret_node.is_null() {
                    Self::entry_del_rcu(hlist, entry);
                }
                ret_node
            } else {
                ptr::null_mut()
            }
        }
    }
}
