//! Intrusive hash list — Rust port of `kernel/hlist.c`.
//!
//! `struct hlist_struct` / `hlist_entry` / `hlist_func_struct` are the
//! real bindgen-generated `crate::bindings::{hlist_struct, hlist_entry,
//! hlist_func_struct}` types (already allowlisted in `build.rs`) — no
//! opaque stand-ins. Each bucket (`hlist_bucket_t`) is itself a
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

use crate::bindings::{hlist_entry, hlist_func_struct, hlist_struct, list_node_t};

pub type Hlist = hlist_struct;
pub type HlistEntry = hlist_entry;
pub type HlistBucket = list_node_t;
pub type HlistFunc = hlist_func_struct;
pub type HtHash = u64;

const HLIST_BUCKET_CNT_MAX: u64 = 0xffff;

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

#[inline(always)]
fn entry_from_node(n: *mut HlistBucket) -> *mut HlistEntry {
    n as *mut HlistEntry
}

#[inline(always)]
fn node_from_entry(e: *mut HlistEntry) -> *mut HlistBucket {
    e as *mut HlistBucket
}

// ---------------------------------------------------------------------------
// hlist_entry_{add,del,replace}_rcu — `static inline` in `kernel/inc/hlist.h`
// (no external linkage); reimplemented on top of `llist` for `hlist_c`'s
// own RCU write paths (`hlist_put_rcu`/`hlist_pop_rcu`).
// ---------------------------------------------------------------------------

/// # Safety
/// `hlist`, `bucket`, `entry` must all point to live values; caller holds
/// the writer-side lock.
unsafe fn hlist_entry_add_rcu(hlist: *mut Hlist, bucket: *mut HlistBucket, entry: *mut HlistEntry) {
    unsafe {
        llist::add_rcu(bucket, node_from_entry(entry));
        ptr::write_volatile(ptr::addr_of_mut!((*entry).bucket), bucket);
        (*hlist).elem_cnt += 1;
    }
}

/// # Safety
/// `hlist` and `entry` must point to live values; caller holds the
/// writer-side lock. Does not clear `entry->bucket` (matches C: readers
/// may still be checking it).
unsafe fn hlist_entry_del_rcu(hlist: *mut Hlist, entry: *mut HlistEntry) {
    unsafe {
        llist::del_rcu(node_from_entry(entry));
        (*hlist).elem_cnt -= 1;
    }
}

/// # Safety
/// `old` and `new` must point to live entries; caller holds the
/// writer-side lock.
unsafe fn hlist_entry_replace_rcu(old: *mut HlistEntry, new: *mut HlistEntry) {
    unsafe {
        llist::replace_rcu(node_from_entry(old), node_from_entry(new));
        ptr::write_volatile(ptr::addr_of_mut!((*new).bucket), (*old).bucket);
    }
}

// ---------------------------------------------------------------------------
// Callback dispatch through `hlist->func`.
// ---------------------------------------------------------------------------

/// # Safety
/// `hlist` must point to a live, `hash`-having `Hlist`; `node` per the
/// callback's own contract (non-null, caller-supplied).
#[inline(always)]
unsafe fn call_hash(hlist: *mut Hlist, node: *mut c_void) -> HtHash {
    unsafe { ((*hlist).func.hash.expect("BUG: hlist: func.hash is None"))(node) }
}

/// # Safety
/// Same contract as [`call_hash`], for `func.get_node`.
#[inline(always)]
unsafe fn call_get_node(hlist: *mut Hlist, entry: *mut HlistEntry) -> *mut c_void {
    unsafe { ((*hlist).func.get_node.expect("BUG: hlist: func.get_node is None"))(entry) }
}

/// # Safety
/// Same contract as [`call_hash`], for `func.get_entry`.
#[inline(always)]
unsafe fn call_get_entry(hlist: *mut Hlist, node: *mut c_void) -> *mut HlistEntry {
    unsafe { ((*hlist).func.get_entry.expect("BUG: hlist: func.get_entry is None"))(node) }
}

/// # Safety
/// Same contract as [`call_hash`], for `func.cmp_node`.
#[inline(always)]
unsafe fn call_cmp_node(hlist: *mut Hlist, n1: *mut c_void, n2: *mut c_void) -> i32 {
    unsafe { ((*hlist).func.cmp_node.expect("BUG: hlist: func.cmp_node is None"))(hlist, n1, n2) }
}

// ---------------------------------------------------------------------------
// Bucket-array helpers.
// ---------------------------------------------------------------------------

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
    unsafe { bucket_index(hlist, bucket).is_some() }
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
        let f = &(*hlist).func;
        f.cmp_node.is_some() && f.get_node.is_some() && f.hash.is_some() && f.get_entry.is_some()
    }
}

/// # Safety
/// `hlist` must be null or point to a live `Hlist`; `node` must be null
/// or a live caller-owned node.
unsafe fn get_node_bucket(hlist: *mut Hlist, node: *mut c_void) -> *mut HlistBucket {
    unsafe {
        if hlist.is_null() || node.is_null() {
            return ptr::null_mut();
        }
        let entry = call_get_entry(hlist, node);
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
unsafe fn calc_hash_bucket(hlist: *mut Hlist, hash: HtHash) -> *mut HlistBucket {
    unsafe { bucket_at(hlist, hash % (*hlist).bucket_cnt) }
}

/// Mirrors `__hlist_find_entry_in_bucket` (non-RCU `list_foreach_node_safe`
/// walk).
///
/// # Safety
/// `hlist` must be validated; `bucket` must be a live bucket head of
/// `*hlist`; `node` is passed through to `func.cmp_node`/`func.get_node`.
unsafe fn find_entry_in_bucket(hlist: *mut Hlist, bucket: *mut HlistBucket, node: *mut c_void) -> *mut HlistEntry {
    unsafe {
        let mut cur = (*bucket).next;
        while cur != bucket {
            let next = (*cur).next; // captured up front: "_safe" iteration
            let entry = entry_from_node(cur);
            let node1 = call_get_node(hlist, entry);
            if call_cmp_node(hlist, node1, node) == 0 {
                return entry;
            }
            cur = next;
        }
        ptr::null_mut()
    }
}

/// Mirrors `__hlist_find_entry_in_bucket_rcu`.
///
/// # Safety
/// Same contract as [`find_entry_in_bucket`], but must be called from
/// within an RCU read-side critical section.
unsafe fn find_entry_in_bucket_rcu(hlist: *mut Hlist, bucket: *mut HlistBucket, node: *mut c_void) -> *mut HlistEntry {
    unsafe {
        let mut cur = llist::next_rcu(bucket);
        while !cur.is_null() && cur != bucket {
            let entry = entry_from_node(cur);
            let node1 = call_get_node(hlist, entry);
            if call_cmp_node(hlist, node1, node) == 0 {
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
unsafe fn get_bucket_entry(hlist: *mut Hlist, node: *mut c_void) -> (*mut HlistBucket, *mut HlistEntry) {
    unsafe {
        let hash_val = call_hash(hlist, node);
        if hash_val == 0 {
            return (ptr::null_mut(), ptr::null_mut());
        }
        let bucket = calc_hash_bucket(hlist, hash_val);
        let entry = find_entry_in_bucket(hlist, bucket, node);
        (bucket, entry)
    }
}

/// Mirrors `__hlist_get_rcu`.
///
/// # Safety
/// Same contract as [`get_bucket_entry`], RCU read-side.
unsafe fn get_bucket_entry_rcu(hlist: *mut Hlist, node: *mut c_void) -> (*mut HlistBucket, *mut HlistEntry) {
    unsafe {
        let hash_val = call_hash(hlist, node);
        if hash_val == 0 {
            return (ptr::null_mut(), ptr::null_mut());
        }
        let bucket = calc_hash_bucket(hlist, hash_val);
        let entry = find_entry_in_bucket_rcu(hlist, bucket, node);
        (bucket, entry)
    }
}

/// # Safety
/// `hlist` must point to a live `Hlist`; `bucket` must be a live bucket
/// head of `*hlist`; `entry` must point to a live, detached `HlistEntry`.
unsafe fn insert_node_entry(hlist: *mut Hlist, bucket: *mut HlistBucket, entry: *mut HlistEntry) {
    unsafe {
        llist::push_front(bucket, node_from_entry(entry));
        (*entry).bucket = bucket;
        (*hlist).elem_cnt += 1;
    }
}

/// # Safety
/// `hlist` must point to a live `Hlist`; `entry` must point to a live,
/// linked `HlistEntry`.
unsafe fn remove_node_entry(hlist: *mut Hlist, entry: *mut HlistEntry) {
    unsafe {
        llist::detach(node_from_entry(entry));
        (*entry).bucket = ptr::null_mut();
        (*hlist).elem_cnt -= 1;
    }
}

/// Mirrors `__hlist_replace_node_entry`: replace `old` in its bucket with
/// `new` (non-RCU).
///
/// # Safety
/// `old` must point to a live, linked `HlistEntry`; `new` must point to a
/// live, detached `HlistEntry`.
unsafe fn replace_node_entry(old: *mut HlistEntry, new: *mut HlistEntry) {
    unsafe {
        llist::replace(node_from_entry(old), node_from_entry(new));
        (*new).bucket = (*old).bucket;
        (*old).bucket = ptr::null_mut();
    }
}

// ---------------------------------------------------------------------------
// Public C ABI — exact symbol/signature parity with `kernel/inc/hlist.h`.
// ---------------------------------------------------------------------------

/// # Safety
/// `hlist` must be null or point to a live `Hlist`; `node` must be null
/// or a live caller-owned node whose `func.get_entry` callback is safe to
/// invoke.
#[no_mangle]
pub unsafe extern "C" fn hlist_node_in_list(hlist: *mut Hlist, node: *mut c_void) -> bool {
    unsafe {
        let bucket = get_node_bucket(hlist, node);
        if bucket.is_null() || hlist.is_null() {
            return false;
        }
        is_bucket_of(hlist, bucket)
    }
}

/// # Safety
/// `hlist` must be null or point to writable, zero-or-more-buckets-sized
/// storage; `func` must be null or point to a live `HlistFunc`.
#[no_mangle]
pub unsafe extern "C" fn hlist_init(hlist: *mut Hlist, bucket_cnt: u64, func: *mut HlistFunc) -> i32 {
    unsafe {
        if hlist.is_null() || func.is_null() {
            return -1;
        }
        if (*func).get_entry.is_none()
            || (*func).get_node.is_none()
            || (*func).hash.is_none()
            || (*func).cmp_node.is_none()
        {
            return -1;
        }
        if bucket_cnt == 0 || bucket_cnt > HLIST_BUCKET_CNT_MAX {
            return -1;
        }

        for i in 0..bucket_cnt {
            llist::init(bucket_at(hlist, i));
        }

        (*hlist).bucket_cnt = bucket_cnt;
        (*hlist).func = *func;
        (*hlist).elem_cnt = 0;

        0
    }
}

/// # Safety
/// `hlist` must be null or point to a live `Hlist`; `node` must be null
/// or a live caller-owned node.
#[no_mangle]
pub unsafe extern "C" fn hlist_get_node_hash(hlist: *mut Hlist, node: *mut c_void) -> HtHash {
    unsafe {
        if hlist.is_null() || node.is_null() {
            return 0;
        }
        if (*hlist).func.hash.is_none() {
            return 0;
        }
        call_hash(hlist, node)
    }
}

/// # Safety
/// `hlist` must be null or point to a live `Hlist`; `node` must be null
/// or a live caller-owned node.
#[no_mangle]
pub unsafe extern "C" fn hlist_get(hlist: *mut Hlist, node: *mut c_void) -> *mut c_void {
    unsafe {
        if node.is_null() {
            return ptr::null_mut();
        }
        if !validate(hlist) {
            return ptr::null_mut();
        }
        let (_, entry) = get_bucket_entry(hlist, node);
        if entry.is_null() {
            return ptr::null_mut();
        }
        call_get_node(hlist, entry)
    }
}

/// # Safety
/// `hlist` must be null or point to a live `Hlist`; `node` must be a live
/// caller-owned node.
#[no_mangle]
pub unsafe extern "C" fn hlist_put(hlist: *mut Hlist, node: *mut c_void, replace: bool) -> *mut c_void {
    unsafe {
        if !validate(hlist) {
            return node;
        }
        let new_entry = call_get_entry(hlist, node);
        if new_entry.is_null() {
            return node;
        }
        if !(*new_entry).bucket.is_null() {
            // HLIST_ENTRY_ATTACHED: cannot insert an already-attached node.
            return node;
        }

        let (bucket, entry) = get_bucket_entry(hlist, node);
        if bucket.is_null() {
            return node;
        }

        if entry.is_null() {
            insert_node_entry(hlist, bucket, new_entry);
            return ptr::null_mut();
        }

        let old_node = call_get_node(hlist, entry);
        if old_node.is_null() {
            return node;
        } else if node == old_node {
            return node;
        }
        if replace {
            replace_node_entry(entry, new_entry);
        }
        old_node
    }
}

/// # Safety
/// `hlist` must be null or point to a live `Hlist`; `node` must be null
/// or a live caller-owned node.
#[no_mangle]
pub unsafe extern "C" fn hlist_pop(hlist: *mut Hlist, node: *mut c_void) -> *mut c_void {
    unsafe {
        if !validate(hlist) {
            return ptr::null_mut();
        }
        if (*hlist).elem_cnt == 0 {
            return ptr::null_mut();
        }

        if node.is_null() {
            for i in 0..(*hlist).bucket_cnt {
                let bucket = bucket_at(hlist, i);
                if !llist::is_self_linked(bucket) {
                    let entry = entry_from_node((*bucket).next);
                    let ret_node = call_get_node(hlist, entry);
                    remove_node_entry(hlist, entry);
                    return ret_node;
                }
            }
            return ptr::null_mut();
        }

        let (_, entry) = get_bucket_entry(hlist, node);
        if !entry.is_null() {
            let ret_node = call_get_node(hlist, entry);
            if !ret_node.is_null() {
                remove_node_entry(hlist, entry);
            }
            ret_node
        } else {
            ptr::null_mut()
        }
    }
}

/// # Safety
/// `hlist` must be null or point to a live `Hlist`.
#[no_mangle]
pub unsafe extern "C" fn hlist_len(hlist: *mut Hlist) -> usize {
    unsafe {
        if !validate(hlist) {
            return 0;
        }
        (*hlist).elem_cnt as usize
    }
}

/// # Safety
/// Same contract as [`hlist_get`]; caller holds `rcu_read_lock()`.
#[no_mangle]
pub unsafe extern "C" fn hlist_get_rcu(hlist: *mut Hlist, node: *mut c_void) -> *mut c_void {
    unsafe {
        if node.is_null() {
            return ptr::null_mut();
        }
        if !validate(hlist) {
            return ptr::null_mut();
        }
        let (_, entry) = get_bucket_entry_rcu(hlist, node);
        if entry.is_null() {
            return ptr::null_mut();
        }
        call_get_node(hlist, entry)
    }
}

/// # Safety
/// Same contract as [`hlist_put`]; caller serializes writers externally
/// (e.g. holds a lock) and defers freeing any replaced node past the next
/// RCU grace period.
#[no_mangle]
pub unsafe extern "C" fn hlist_put_rcu(hlist: *mut Hlist, node: *mut c_void, replace: bool) -> *mut c_void {
    unsafe {
        if !validate(hlist) {
            return node;
        }
        let new_entry = call_get_entry(hlist, node);
        if new_entry.is_null() {
            return node;
        }
        if !(*new_entry).bucket.is_null() {
            return node;
        }

        // Non-RCU lookup: the writer already holds whatever lock
        // serializes writers (matches the C comment on `hlist_put_rcu`).
        let (bucket, entry) = get_bucket_entry(hlist, node);
        if bucket.is_null() {
            return node;
        }

        if entry.is_null() {
            hlist_entry_add_rcu(hlist, bucket, new_entry);
            return ptr::null_mut();
        }

        let old_node = call_get_node(hlist, entry);
        if old_node.is_null() {
            return node;
        } else if node == old_node {
            return node;
        }
        if replace {
            hlist_entry_replace_rcu(entry, new_entry);
        }
        old_node
    }
}

/// # Safety
/// Same contract as [`hlist_pop`]; caller serializes writers externally
/// and defers freeing the returned node past the next RCU grace period.
#[no_mangle]
pub unsafe extern "C" fn hlist_pop_rcu(hlist: *mut Hlist, node: *mut c_void) -> *mut c_void {
    unsafe {
        if !validate(hlist) {
            return ptr::null_mut();
        }
        if (*hlist).elem_cnt == 0 {
            return ptr::null_mut();
        }

        if node.is_null() {
            for i in 0..(*hlist).bucket_cnt {
                let bucket = bucket_at(hlist, i);
                if !llist::is_self_linked(bucket) {
                    let entry = entry_from_node((*bucket).next);
                    let ret_node = call_get_node(hlist, entry);
                    hlist_entry_del_rcu(hlist, entry);
                    return ret_node;
                }
            }
            return ptr::null_mut();
        }

        let (_, entry) = get_bucket_entry(hlist, node);
        if !entry.is_null() {
            let ret_node = call_get_node(hlist, entry);
            if !ret_node.is_null() {
                hlist_entry_del_rcu(hlist, entry);
            }
            ret_node
        } else {
            ptr::null_mut()
        }
    }
}
