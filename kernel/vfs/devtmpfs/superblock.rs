//! devtmpfs filesystem type and superblock operations — Rust port of
//! `kernel/vfs/devtmpfs/superblock.c` (Phase 2 Wave 20; see
//! `docs/rustify/phase2_plan.md` and the module doc in `super`).
//!
//! devtmpfs reuses the tmpfs inode/dentry infrastructure but registers as
//! a separate filesystem type. On mount it auto-populates `/dev` with the
//! device nodes that have been registered so far via
//! [`devtmpfs_create_node`].
//!
//! Unlike Linux's devtmpfs which uses a kernel thread to serialise node
//! creation, this implementation directly creates/removes nodes under the
//! global devtmpfs spinlock. This is sufficient for xv6 where device
//! registration happens during early boot on a single hart.
//!
//! # tmpfs-contract reuse
//!
//! `kernel/vfs/tmpfs/tmpfs_private.h` was deliberately kept as a real,
//! unmodified C header through Waves 18-19 specifically so this wave could
//! consume it (see `kernel/vfs/tmpfs/mod.rs`'s module doc). Every tmpfs
//! symbol this file needs (`tmpfs_alloc_inode`/`tmpfs_get_inode`/
//! `tmpfs_sync_fs`/`tmpfs_unmount_begin`/`tmpfs_alloc_superblock`/
//! `tmpfs_free`/`tmpfs_make_directory`) is declared in a local `unsafe
//! extern "C"` block below rather than imported by Rust path, even though
//! `tmpfs` and `devtmpfs` are now both submodules of the same Rust crate —
//! matching the established per-file convention (`tmpfs/superblock.rs`'s
//! own doc: "a cross-C-TU call", not a same-logical-unit call, since
//! `devtmpfs/superblock.c` used to be a genuinely separate translation
//! unit that `#include`d `tmpfs_private.h` to reach these symbols). Now
//! that this is the last consumer of that C-ABI contract to move to Rust,
//! `tmpfs_private.h` is *still* kept in `wrapper.h` (unlike
//! `devtmpfs_private.h`, deleted by this wave, whose sole consumer was
//! this file) — not because devtmpfs needs it any more (nothing `#include`s
//! it now), but because `kernel/vfs/tmpfs/*.rs` itself still needs the
//! header for bindgen's `struct tmpfs_inode`/`tmpfs_superblock` layouts.
//! See the Wave 20 report for the full note.
//!
//! # C-ABI surface kept
//!
//! `kernel/inc/devtmpfs.h` (the *public* devtmpfs API, distinct from the
//! now-deleted `kernel/vfs/devtmpfs/devtmpfs_private.h`) is unchanged by
//! this port and still declares `devtmpfs_create_node`/
//! `devtmpfs_remove_node`/`devtmpfs_populate_devices` as plain C externs —
//! `kernel/dev/dev.c` (still C, Wave 21) calls the first two from
//! `device_register()`/`device_unregister()`. All three, plus
//! `devtmpfs_init`/`devtmpfs_post_mount_populate` (called from
//! `kernel/vfs/fs.rs`'s `vfs_init()`), keep their exact C signatures and
//! return-value semantics — in particular
//! [`devtmpfs_create_node`]/[`devtmpfs_remove_node`] still swallow
//! [`__devtmpfs_mknod_relative`]/[`__devtmpfs_unlink_relative`] failures
//! into an unconditional `0` for the registry-buffering half of their
//! contract (`device_register()` discarding *that* return value entirely
//! is `dev.c`'s own pre-existing bug, out of scope here — see the Known
//! issues list in `RUST_REWRITE.md`; fixing it belongs to Wave 21, and
//! this port deliberately keeps the exact return-value shape so that fix
//! lands cleanly).
//!
//! # Locking / concurrency
//!
//! * [`__DEVTMPFS_NODES`] (`__devtmpfs_lock` + the node registry list in
//!   the C original) guards the node registry list and the one-time
//!   registry-list initialisation flag. Wave P3-8d: migrated from a bare
//!   `spinlock_t` paired with a separate `static mut list_node_t` to a
//!   `crate::sync::SpinLock<list_node_t>` that owns the list head it
//!   protects directly (same precedent as `ramdisk.rs`/`bufcache.rs`/
//!   `sysnet.rs`) — see [`__devtmpfs_ensure_init`] for the one place this
//!   changed the *mechanism* (not the observable behaviour, see below)
//!   of the list head's self-reference fixup.
//! * `__DEVTMPFS_SB` (`__devtmpfs_sb` in the C original) is written once
//!   per mount/unmount under the VFS-core mount lock
//!   (`Vfs::vfs_mount_lock()`, held by the generic mount/unmount machinery
//!   that calls [`devtmpfs_mount`]/[`devtmpfs_umount_free`] as its
//!   `vfs_fs_type_ops` callbacks) but *read* without any lock from
//!   [`__devtmpfs_get_root`] — called from [`devtmpfs_create_node`]/
//!   [`devtmpfs_remove_node`], which can run from arbitrary device-driver
//!   contexts at any time. The C original used a plain (non-atomic)
//!   pointer for this exact pattern; ported here as `AtomicPtr` with
//!   `Relaxed` ordering, reproducing the identical observable behaviour
//!   while removing the data race that the C pointer read/write
//!   technically was — same upgrade, same rationale, as `console.rs`'s
//!   `CONSOLE_TTY` (Phase 2 Wave 4).
//! * [`__devtmpfs_ensure_init`]'s one-time flag-set (`__DEVTMPFS_INITIALIZED`)
//!   is still a plain non-atomic `static mut` check-then-set, matching
//!   the C exactly (including its implicit assumption that device
//!   registration is single-hart at the point any of this runs — true
//!   for every registrant in-tree today, see `dev_table_init`'s early-boot
//!   call site). Not upgraded to a CAS-guarded once-init: doing so would
//!   change observable behaviour under a hypothetical concurrent first
//!   call, which the C never defined either, so there is no "identical
//!   behaviour, race removed" upgrade available the way there was for
//!   `__DEVTMPFS_SB` above. Flagged, not fixed, matching this crate's
//!   documented-deviation convention. The list-head self-reference fixup
//!   itself (`next`/`prev` pointing at the head) *does* now briefly take
//!   `__DEVTMPFS_NODES`'s lock (Wave P3-8d) instead of writing the raw
//!   static directly — a mechanical consequence of the lock owning the
//!   data (there is no lock-free raw accessor), and a strict
//!   improvement (was entirely unlocked before) rather than a new
//!   hazard, so it doesn't change the "not upgraded" judgment above.
//! * [`devtmpfs_post_mount_populate`]'s walk of the registry list drops
//!   and reacquires `__DEVTMPFS_NODES`'s lock (`drop(guard)` / re-`lock()`,
//!   Wave P3-8d) around each (potentially sleeping) `__devtmpfs_mknod_relative`
//!   call, exactly like the C's `list_foreach_node_safe` with a
//!   `spin_unlock`/`spin_lock` pair inside the loop body — the "next"
//!   pointer is always captured while the lock is held (either the
//!   initial acquire before the loop, or the re-acquire at the bottom of
//!   the previous iteration), matching the C's safe-iteration discipline
//!   exactly. As in the C, this does not defend against a concurrent
//!   [`devtmpfs_remove_node`] freeing the captured "next" node while the
//!   lock is dropped — a latent race in the original, preserved here
//!   (early-boot single-hart in every in-tree call site, same reasoning
//!   as above).
//!
//! # Deliberate fix
//!
//! [`Devtmpfs::walk_parent`]'s successful `VfsInode::vfs_ilookup()` branches
//! (matching `.`/`..`-less intermediate path components) populate a
//! `vfs_dentry` whose `name` field `vfs_ilookup` heap-allocates via
//! `strndup` — the C original never released it (every other
//! `vfs_ilookup` call site in the crate pairs it with
//! `vfs_release_dentry`; this one didn't). Fixed here as a pure cleanup
//! addition with zero control-flow/return-value change, same category of
//! fix as Wave 18's tmpfs move-UAF fix and Wave-11-era map_pages leak fix.
//!
//! # Wave B free-fn -> associated-fn sweep
//!
//! Every relocatable free fn that used to live in this file is now an
//! associated fn: `impl Devtmpfs` (a ZST private to this file) for the
//! scalar/registry-global helper bodies with no `DevtmpfsNode`-typed
//! subject -- `neg`/`mkdev`/`__devtmpfs_ensure_init` -> `ensure_init`/
//! `__devtmpfs_get_root` -> `get_root`/`__devtmpfs_walk_parent` ->
//! `walk_parent`/`__devtmpfs_mknod_relative` -> `mknod_relative`/
//! `__devtmpfs_unlink_relative` -> `unlink_relative` -- or `impl
//! DevtmpfsNode` for [`DevtmpfsNode::iter`] (was `devtmpfs_node_iter`),
//! whose subject *is* a `*mut list_node_t` == `*mut DevtmpfsNode`
//! pointer. Every relocated body is byte-identical to its old free-fn
//! form; no raw-pointer first param became `&self`/`&T`. The
//! `list_first`/`list_next`/`list_push_front`/`list_detach` intrusive-
//! list primitives stay FLOOR free fns (generic `*mut list_node_t` ops,
//! refify-ceiling floor, same precedent as this crate's other intrusive-
//! list helpers).

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::bindings::{
    device_t, dev_t, list_node_t, mode_t, tmpfs_inode, tmpfs_superblock, vfs_dentry, vfs_fs_type,
    vfs_inode, vfs_superblock, EEXIST, EINVAL, ENODEV, ENOENT,
    ENOMEM, PGSIZE,
};
use crate::kstd::{Errno, KResult};
use crate::vfs::fs::{FsTypeOps, SuperblockOps};
use crate::sync::SpinLock;
// P3-1C mesh sweep: vfs/{fs,inode}.rs and vfs/tmpfs/{superblock,inode}.rs
// are in scope for this wave; converted from `extern "C"` redeclarations
// to plain crate-path items (identical signatures).
use crate::vfs::fs::{Vfs, VfsFsType, VfsSuperblock, vfs_release_dentry};
use crate::vfs::inode::VfsInode;
// Wave A: tmpfs's `tmpfs_make_directory`/`tmpfs_alloc_inode_inner`/
// `tmpfs_alloc_superblock`/`tmpfs_free_impl`/`tmpfs_get_inode_inner`/
// `tmpfs_sync_fs_impl`/`tmpfs_unmount_begin_impl` are now associated fns
// on `TmpfsInode`/`TmpfsSuperblock` (see those modules' own sweep docs);
// call sites below use `TmpfsInode::make_directory`/
// `TmpfsSuperblock::alloc_inode_inner` etc.
use crate::vfs::tmpfs::inode::TmpfsInode;
use crate::vfs::tmpfs::superblock::TmpfsSuperblock;

// ===========================================================================
// Externs — see the module doc above for the convention.
// ===========================================================================

unsafe extern "C" {
    // printf.rs -- C-variadic.

    // string.rs (Wave 2).
    safe fn strlen(s: *const c_char) -> usize;
    safe fn strncmp(s1: *const c_char, s2: *const c_char, n: usize) -> c_int;
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

}
// P3-D3a: `kmm_alloc`/`kmm_free` are genuinely `unsafe fn` in
// `crate::mm::kalloc` now that their `#[no_mangle]` exports are gone;
// `cffi::raw`'s existing thin safe wrappers (identical signatures)
// preserve the `safe fn` facade the old redeclarations asserted.
use crate::mm::cffi::raw::{kmm_alloc, kmm_free};
// P3-1D mesh sweep: dev/dev.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use crate::dev::dev::DevTable;

// `kassert!`/`err_ptr`/`is_err`/`is_err_or_null`/`ptr_err`'s canonical
// homes are `crate::kstd`/crate root (P3-CS1 centralization). Note
// `ptr_err` now returns `c_int` directly (this file's former local copy
// returned `isize`; both of its call sites already cast the result to
// `c_int` immediately, see below).
use crate::kassert;
// P3-10b: the ERR_PTR helper imports are gone — every VFS-core call
// this file makes is `KResult`-native now.

/// ZST marker (Wave B free-fn -> associated-fn sweep, this file only) --
/// see the module doc. Holds the scalar/registry-global helper bodies
/// with no `DevtmpfsNode`-typed subject (`neg`/`mkdev`/`ensure_init`/
/// `get_root`/`walk_parent`/`mknod_relative`/`unlink_relative`).
/// Distinct from [`DevtmpfsNode`] (which owns [`DevtmpfsNode::iter`],
/// whose subject *is* a node-typed pointer) -- same subject-vs-scalar
/// split this crate's other Wave A/B sweeps use.
struct Devtmpfs;

impl Devtmpfs {
    #[inline(always)]
    const fn neg(e: u32) -> c_int {
        -(e as c_int)
    }

    /// `mkdev(major, minor)` (`kernel/inc/defs.h`: `((uint)((m) << 20 | (n)))`).
    #[inline(always)]
    fn mkdev(major: c_int, minor: c_int) -> dev_t {
        ((major << 20) | minor) as dev_t
    }
}

// ===========================================================================
// Global device-node registry
// ===========================================================================

/// Mirrors `struct devtmpfs_node` (`devtmpfs_private.h`, deleted by this
/// port -- see the module doc). Purely internal now: no C translation
/// unit ever touches this layout, so it is a private Rust struct rather
/// than a bindgen type. `list_entry` is kept as the first field (offset
/// 0), exactly like the C, so a `*mut DevtmpfsNode` <-> `*mut
/// list_node_t` round-trip is a plain pointer cast with no `container_of`
/// arithmetic needed -- the same trick `tmpfs_inode`/`tmpfs_superblock`
/// use for their embedded `vfs_inode`/`vfs_sb` (offset 0).
#[repr(C)]
struct DevtmpfsNode {
    list_entry: list_node_t,
    name: *mut c_char,
    name_len: usize,
    mode: mode_t,
    dev: dev_t,
}

/// Wave P3-8d: `__devtmpfs_lock` (a bare `spinlock_t`) + the node
/// registry list head used to be two independently-paired globals
/// (manual `spin_lock`/`spin_unlock` at every site below). Now the lock
/// owns the list head directly (`crate::sync::SpinLock`, same
/// P3-8b/8c/8d precedent as `ramdisk.rs`/`bufcache.rs`/`sysnet.rs`). The
/// list head is still self-referential once initialised (`next == prev
/// == &head`, the C `LIST_ENTRY_INITIALIZED` idiom) -- that
/// self-reference can't be written in a `const` initializer (the
/// static's own address isn't knowable inside its own initializer), so
/// the lock starts life wrapping a not-yet-self-referential placeholder
/// (`next: null, prev: null`) and [`__devtmpfs_ensure_init`] still
/// completes the one-time fixup, now reached through the guard instead
/// of a raw pointer to a separate static.
static __DEVTMPFS_NODES: SpinLock<list_node_t> =
    SpinLock::new(c"devtmpfs", list_node_t { next: ptr::null_mut(), prev: ptr::null_mut() });
static mut __DEVTMPFS_INITIALIZED: bool = false;

/// The mounted devtmpfs superblock. Set by [`devtmpfs_mount`], cleared by
/// [`devtmpfs_umount_free`]. See the module doc's "Locking / concurrency"
/// section for the `AtomicPtr`/`Relaxed` rationale.
static __DEVTMPFS_SB: AtomicPtr<vfs_superblock> = AtomicPtr::new(ptr::null_mut());

impl Devtmpfs {
    fn ensure_init() {
        // SAFETY: `__DEVTMPFS_INITIALIZED` is written here under the same
        // non-atomic check-then-set discipline as the C original (see the
        // module doc's "Locking / concurrency" section for why this is not
        // upgraded to a CAS-guarded once-init). The list-head self-reference
        // fixup itself now goes through `__DEVTMPFS_NODES`'s lock (Wave
        // P3-8d) rather than a direct write to a separate static, since the
        // lock owns the data -- this briefly takes the lock where the C/
        // pre-P3-8d Rust never did, which is a strict improvement (was
        // entirely unlocked before), not a new hazard.
        unsafe {
            if !__DEVTMPFS_INITIALIZED {
                let mut head = __DEVTMPFS_NODES.lock();
                let addr: *mut list_node_t = &raw mut *head;
                (*addr).next = addr;
                (*addr).prev = addr;
                __DEVTMPFS_INITIALIZED = true;
            }
        }
    }
}

// ---------------------------------------------------------------------
// Minimal intrusive doubly-linked list helpers, reimplementing the C
// `static inline` primitives from `list.h` this file needs
// (`list_entry_init`/`list_entry_insert`/`list_entry_detach`/
// `LIST_FIRST_NODE`/`LIST_NEXT_NODE`) -- bindgen cannot call `static
// inline` C functions, so these are hand-ported locally, same precedent
// as `tmpfs/superblock.rs`'s `hlist_first_entry`/`hlist_next_entry`.
// `DevtmpfsNode::list_entry` is this struct's first field (offset 0), so
// every node pointer here doubles as its own `list_node_t*` with a plain
// cast -- no `container_of` needed.
// ---------------------------------------------------------------------

/// Mirrors `LIST_FIRST_NODE(head, ...)`: the first node in the list, or
/// null if empty.
///
/// # Safety
/// `head` must point to a live, initialized `list_node_t` ring head.
unsafe fn list_first(head: *mut list_node_t) -> *mut list_node_t {
    unsafe {
        let n = (*head).next;
        if n == head {
            ptr::null_mut()
        } else {
            n
        }
    }
}

/// Mirrors `LIST_NEXT_NODE(head, node, ...)`: the node after `node`, or
/// null if `node` is the list's last node.
///
/// # Safety
/// `head` must point to a live, initialized `list_node_t` ring head;
/// `node` must be a live node currently linked into that ring.
unsafe fn list_next(head: *mut list_node_t, node: *mut list_node_t) -> *mut list_node_t {
    unsafe {
        let n = (*node).next;
        if n == head {
            ptr::null_mut()
        } else {
            n
        }
    }
}

/// Mirrors `list_entry_push_front()` (== `list_entry_insert(head, entry)`).
///
/// # Safety
/// `head` must point to a live, initialized `list_node_t` ring head;
/// `node` must be a detached, initialized `list_node_t` not currently
/// linked into any list.
unsafe fn list_push_front(head: *mut list_node_t, node: *mut list_node_t) {
    unsafe {
        let next = (*head).next;
        (*node).prev = head;
        (*node).next = next;
        (*head).next = node;
        (*next).prev = node;
    }
}

/// Mirrors `list_entry_detach()` / `list_node_detach()`.
///
/// # Safety
/// `node` must be a live node currently linked into some ring (or a
/// detached, self-referential node, which is a no-op).
unsafe fn list_detach(node: *mut list_node_t) {
    unsafe {
        let p = (*node).prev;
        let n = (*node).next;
        (*p).next = n;
        (*n).prev = p;
        (*node).next = node;
        (*node).prev = node;
    }
}

/// Read-only forward iterator over the registry ring — `successors` over
/// `list_first`/`list_next`, the `mm::slab::cache_registry_iter`/`vm_vma_iter`
/// idiom (a72a0d7). Yields every live `*mut list_node_t` (== `*mut
/// DevtmpfsNode`, offset 0) from first to last.
///
/// For **read-only** walks under a held `__DEVTMPFS_NODES` guard only. The
/// lock-drop mutation walk in [`devtmpfs_post_mount_populate`] deliberately
/// keeps its explicit hand-written loop (it captures `next` before dropping
/// the lock and running a sleeping `mknod`, which this lazy iterator can't
/// express — `successors` would re-read `list_next` on a node that may have
/// been freed while the lock was dropped).
///
/// # Safety
/// `head` must point to a live, initialized `list_node_t` ring head that
/// stays locked (unmutated) for the lifetime of the returned iterator.
impl DevtmpfsNode {
    unsafe fn iter(head: *mut list_node_t) -> impl Iterator<Item = *mut list_node_t> {
        let first = unsafe { list_first(head) };
        core::iter::successors((!first.is_null()).then_some(first), move |&cur| {
            let n = unsafe { list_next(head, cur) };
            (!n.is_null()).then_some(n)
        })
    }
}

// ------------------------------------------------------------------ */
//  Mount-point-agnostic helpers (use root inode, not "/dev/")
// ------------------------------------------------------------------ */

impl Devtmpfs {
    /// `__devtmpfs_get_root` — return the devtmpfs root inode (no extra ref).
    /// Returns null if devtmpfs is not mounted. See the module doc's
    /// "Locking / concurrency" section: reads `__DEVTMPFS_SB` without any
    /// lock, matching the C original's exact (documented) race.
    fn get_root() -> *mut vfs_inode {
        let sb = __DEVTMPFS_SB.load(Ordering::Relaxed);
        if sb.is_null() {
            return ptr::null_mut();
        }
        // SAFETY: a non-null `__DEVTMPFS_SB` is only ever a live
        // `vfs_superblock` written by `devtmpfs_mount` (cleared by
        // `devtmpfs_umount_free` before the superblock is freed); reading
        // `root_inode` is a plain field load.
        unsafe { (*sb).root_inode }
    }

    /// `__devtmpfs_walk_parent` — given a relative path like `"pts/0"`, walk
    /// from `root` through intermediate directories, creating them if needed.
    /// Returns the parent directory inode (with an extra iref) and writes the
    /// leaf name into `*leaf`/`*leaf_len`.
    ///
    /// On failure returns null.
    ///
    /// # Safety
    /// `root` must be a live, referenced `vfs_inode`; `name` must point to at
    /// least `name_len` readable bytes.
    unsafe fn walk_parent(
        root: *mut vfs_inode,
        name: *const c_char,
        name_len: usize,
        leaf: &mut *const c_char,
        leaf_len: &mut usize,
    ) -> *mut vfs_inode {
        // SAFETY: caller's contract.
        let name_bytes = unsafe { core::slice::from_raw_parts(name as *const u8, name_len) };

        // Find the last '/' -- everything before it is a directory path.
        let last_slash: isize = name_bytes
            .iter()
            .rposition(|&b| b == b'/')
            .map(|i| i as isize)
            .unwrap_or(-1);

        let mut dir = root;
        VfsInode::vfs_idup(dir); // take a ref so caller can always iput the result

        if last_slash > 0 {
            let last_slash = last_slash as usize;
            // Path-component tokenizer -> `split('/')` iterator. Empty tokens
            // (leading/consecutive slashes) are skipped exactly like the C's
            // `end == start` continue; each token subslices `name_bytes`, so
            // `token.as_ptr()`/`len()` reproduce the old `name.add(start)`/
            // `end - start` byte-for-byte.
            for token in name_bytes[..last_slash].split(|&b| b == b'/') {
                if token.is_empty() {
                    continue;
                }
                let comp = token.as_ptr() as *const c_char;
                let comp_len = token.len();

                let mut dentry: vfs_dentry = unsafe { core::mem::zeroed() };
                let ret = VfsInode::vfs_ilookup(dir, &mut dentry, comp, comp_len);
                if ret == 0 && dentry.ino != 0 {
                    // Found -- get the inode.
                    // SAFETY: `dir` is non-null (loop invariant); `sb`/`ops`
                    // are set by the generic VFS mount machinery before any
                    // directory op can run. Mirrors the C's direct
                    // `dir->sb->ops->get_inode(dir->sb, dentry.ino)` vtable
                    // dispatch (deliberately not the generic
                    // `vfs_get_inode()` cache-lookup wrapper -- ported
                    // faithfully, see the Wave 20 report).
                    let sb = unsafe { (*dir).sb };
                    // SAFETY: `sb`/`ops` live per the comment above; the
                    // trait method is a driver callback with exactly this
                    // contract (P3-10b: `KResult`-native dyn dispatch, no
                    // ERR_PTR decode).
                    let child = unsafe { crate::vfs::fs::VfsSuperblock::sb_ops(sb).get_inode(sb, dentry.ino) };
                    // Deliberate fix (see module doc): release the dentry's
                    // heap-allocated name now that `.ino` has been consumed.
                    vfs_release_dentry(&mut dentry);
                    VfsInode::vfs_iput(dir);
                    let Ok(child) = child else {
                        return ptr::null_mut();
                    };
                    dir = child;
                } else {
                    // Not found -- create the directory. (P3-10b:
                    // `KResult`-native, no ERR_PTR decode.)
                    let sub = crate::vfs::inode::VfsInode::vfs_mkdir_inner(dir, 0o755, comp, comp_len);
                    VfsInode::vfs_iput(dir);
                    let Ok(sub) = sub else {
                        return ptr::null_mut();
                    };
                    dir = sub;
                }
            }
        }

        // Leaf is the part after the last '/' (or the whole name).
        if last_slash >= 0 {
            let last_slash = last_slash as usize;
            // SAFETY: `last_slash < name_len` (found within `name_bytes`).
            *leaf = unsafe { name.add(last_slash + 1) };
            *leaf_len = name_len - (last_slash + 1);
        } else {
            *leaf = name;
            *leaf_len = name_len;
        }
        dir // caller must VfsInode::vfs_iput()
    }

    /// `__devtmpfs_mknod_relative` — create a device node under the devtmpfs
    /// root inode using the relative name (e.g. `"console"`, `"pts/0"`).
    /// Creates intermediate directories as needed.
    ///
    /// # Safety
    /// `root` must be a live, referenced `vfs_inode`; `name` must point to at
    /// least `name_len` readable bytes.
    unsafe fn mknod_relative(
        root: *mut vfs_inode,
        name: *const c_char,
        name_len: usize,
        mode: mode_t,
        dev: dev_t,
    ) -> c_int {
        let mut leaf: *const c_char = ptr::null();
        let mut leaf_len: usize = 0;
        // SAFETY: caller's contract, forwarded.
        let parent = unsafe { Devtmpfs::walk_parent(root, name, name_len, &mut leaf, &mut leaf_len) };
        if parent.is_null() {
            return Devtmpfs::neg(ENOENT);
        }

        // P3-10b: `KResult`-native, no ERR_PTR decode.
        let mut ret = 0;
        match crate::vfs::inode::VfsInode::vfs_mknod_inner(parent, mode, dev, leaf, leaf_len) {
            Ok(inode) => VfsInode::vfs_iput(inode),
            Err(e) => {
                ret = e.neg();
                if ret == Devtmpfs::neg(EEXIST) {
                    ret = 0; // idempotent
                }
            }
        }
        VfsInode::vfs_iput(parent);
        ret
    }

    /// `__devtmpfs_unlink_relative` — remove a device node from under the
    /// devtmpfs root inode using the relative name (e.g. `"pts/0"`).
    ///
    /// # Safety
    /// `root` must be a live, referenced `vfs_inode`; `name` must point to at
    /// least `name_len` readable bytes.
    unsafe fn unlink_relative(
        root: *mut vfs_inode,
        name: *const c_char,
        name_len: usize,
    ) -> c_int {
        let mut leaf: *const c_char = ptr::null();
        let mut leaf_len: usize = 0;
        // SAFETY: caller's contract, forwarded.
        let parent = unsafe { Devtmpfs::walk_parent(root, name, name_len, &mut leaf, &mut leaf_len) };
        if parent.is_null() {
            return Devtmpfs::neg(ENOENT);
        }

        let ret = VfsInode::vfs_unlink(parent, leaf, leaf_len);
        VfsInode::vfs_iput(parent);
        ret
    }
}

// ------------------------------------------------------------------ */
//  Populate a mounted devtmpfs with registered device nodes
// ------------------------------------------------------------------ */

/// Called AFTER `vfs_mount_path()` succeeds, so the superblock and all
/// inodes are fully VFS-initialised. Uses the stored [`__DEVTMPFS_SB`]
/// root inode so that the code is independent of the mount path.
///
/// Called from `kernel/vfs/fs.rs`'s `vfs_init()` -- kept `#[no_mangle]`
/// per that extern declaration.
pub(crate) extern "C" fn devtmpfs_post_mount_populate() -> c_int {
    let root = Devtmpfs::get_root();
    if root.is_null() {
        crate::kprintln!("devtmpfs: post_mount_populate called but no sb");
        return Devtmpfs::neg(ENODEV);
    }

    // See the module doc's "Locking / concurrency" section for the
    // drop/reacquire discipline this loop follows (matches the C's
    // `list_foreach_node_safe` + mid-body `spin_unlock`/`spin_lock`, now
    // `drop(guard)` / re-`lock()` -- Wave P3-8d). The "next" pointer is
    // always captured while the lock is held, exactly like the C.
    let mut guard = __DEVTMPFS_NODES.lock();
    // SAFETY: `guard` proves the lock is held; `*guard` is a live,
    // initialised list head (`__devtmpfs_ensure_init` always runs before
    // any node can exist to iterate).
    let mut cur = unsafe { list_first(&raw mut *guard) };
    while !cur.is_null() {
        let node = cur as *mut DevtmpfsNode;
        // SAFETY: `node` is a live registry entry; snapshot fields so we
        // can release the lock.
        let (n, nl, m, d) = unsafe { ((*node).name, (*node).name_len, (*node).mode, (*node).dev) };
        // SAFETY: `guard` still held; `cur` still a live linked node.
        let next = unsafe { list_next(&raw mut *guard, cur) };
        drop(guard);

        // SAFETY: `root`/`n`/`nl` satisfy `Devtmpfs::mknod_relative`'s
        // contract.
        let ret = unsafe { Devtmpfs::mknod_relative(root, n, nl, m, d) };
        if ret != 0 && ret != Devtmpfs::neg(EEXIST) {
            crate::kprintln!("devtmpfs: mknod '{}' failed: {}", crate::printf::Cs(n), ret);
        }

        guard = __DEVTMPFS_NODES.lock();
        cur = next;
    }
    drop(guard);
    0
}

// ------------------------------------------------------------------ */
//  Superblock ops — delegate to tmpfs
// ------------------------------------------------------------------ */

/// devtmpfs's [`SuperblockOps`] implementor (P3-10b): delegates the
/// four required methods to tmpfs's `KResult`-native bodies exactly as
/// the old table reused tmpfs's fn pointers. Unlike tmpfs, `statfs` is
/// NOT overridden — the old table's `None` slot becomes the trait's
/// do-nothing `Ok(())` default (generic statfs fields only).
struct DevtmpfsSuperblockOps;

impl SuperblockOps for DevtmpfsSuperblockOps {
    unsafe fn alloc_inode(&self, sb: *mut vfs_superblock) -> KResult<*mut vfs_inode> {
        TmpfsSuperblock::alloc_inode_inner(sb)
    }
    unsafe fn get_inode(&self, sb: *mut vfs_superblock, ino: u64) -> KResult<*mut vfs_inode> {
        TmpfsSuperblock::get_inode_inner(sb, ino)
    }
    unsafe fn sync_fs(&self, sb: *mut vfs_superblock, wait: core::ffi::c_int) -> KResult<()> {
        TmpfsSuperblock::sync_fs_impl(sb, wait)
    }
    unsafe fn unmount_begin(&self, sb: *mut vfs_superblock) {
        TmpfsSuperblock::unmount_begin_impl(sb);
    }
}

static DEVTMPFS_SUPERBLOCK_OPS: DevtmpfsSuperblockOps = DevtmpfsSuperblockOps;

// ------------------------------------------------------------------ */
//  Filesystem type mount/free
// ------------------------------------------------------------------ */

/// devtmpfs's [`FsTypeOps`] implementor (P3-10b). `mount` mirrors
/// `devtmpfs_mount()` (only ever reached through the generic VFS mount
/// machinery in `vfs/fs.rs`, exactly like the old `mount` fn pointer;
/// the `ret_sb` out-param + errno return became
/// `KResult<*mut vfs_superblock>`); `free` mirrors
/// `devtmpfs_umount_free()`.
struct DevtmpfsFsTypeOps;

impl FsTypeOps for DevtmpfsFsTypeOps {
    unsafe fn mount(
        &self,
        mountpoint: *mut vfs_inode,
        device: *mut vfs_inode,
        _flags: c_int,
        _data: *const c_char,
    ) -> KResult<*mut vfs_superblock> {
    if mountpoint.is_null() {
        return Err(Errno::Inval);
    }
    if !device.is_null() {
        return Err(Errno::Inval); // devtmpfs is backendless
    }

    let sb = TmpfsSuperblock::alloc_superblock();
    if sb.is_null() {
        return Err(Errno::NoMem);
    }

    // Initialise next_ino to 1 BEFORE allocating the root inode.
    // tmpfs_alloc_inode calls __tmpfs_ino_alloc which returns next_ino++,
    // so it would return 0 (and fail with ENOENT) if we leave it at the
    // memset-zero default. The root inode gets ino 1, and we bump
    // next_ino to 2 afterwards for subsequent allocations.
    // SAFETY: `sb` is freshly allocated and exclusively owned here.
    unsafe { (*sb).private_data.next_ino = 1 };

    // Use tmpfs's inode allocator to get a proper inode from the shared
    // cache (P3-10b: `KResult`-native, no ERR_PTR decode).
    let vi = match TmpfsSuperblock::alloc_inode_inner(unsafe { ptr::addr_of_mut!((*sb).vfs_sb) }) {
        Ok(vi) => vi,
        Err(e) => {
            TmpfsSuperblock::free_impl(unsafe { ptr::addr_of_mut!((*sb).vfs_sb) });
            return Err(e);
        }
    };
    let root_inode = vi as *mut tmpfs_inode;
    TmpfsInode::make_directory(root_inode);
    // SAFETY: `sb`/`root_inode` are freshly allocated and exclusively
    // owned here.
    unsafe {
        (*root_inode).vfs_inode.n_links = 2;

        (*sb).vfs_sb.block_size = PGSIZE as usize;
        (*sb).vfs_sb.root_inode = ptr::addr_of_mut!((*root_inode).vfs_inode);
        (*sb).vfs_sb.flags.set_backendless(1);
        (*sb).vfs_sb.ops = Some(&DEVTMPFS_SUPERBLOCK_OPS);
    }

    // Save the superblock so create_node/remove_node can find the root
    // inode without relying on the mount path (e.g. "/dev").
    __DEVTMPFS_SB.store(unsafe { ptr::addr_of_mut!((*sb).vfs_sb) }, Ordering::Relaxed);

    // NOTE: Do NOT populate here. The superblock / root inode are not
    // yet initialised by VFS (rwsem, mutex, inode hash, sb->valid are
    // all set up by __vfs_init_superblock_structure / __vfs_init_sb_rooti
    // which run AFTER the mount callback returns). Population is done by
    // devtmpfs_post_mount_populate() called from vfs_init().

    // SAFETY: `sb` is live (allocated above); `vfs_sb` is its first field.
    Ok(unsafe { ptr::addr_of_mut!((*sb).vfs_sb) })
    }

    /// Mirrors `devtmpfs_umount_free()`.
    unsafe fn free(&self, sb: *mut vfs_superblock) {
        if __DEVTMPFS_SB.load(Ordering::Relaxed) == sb {
            __DEVTMPFS_SB.store(ptr::null_mut(), Ordering::Relaxed);
        }
        TmpfsSuperblock::free_impl(sb);
    }
}

static DEVTMPFS_FS_TYPE_OPS: DevtmpfsFsTypeOps = DevtmpfsFsTypeOps;

// ------------------------------------------------------------------ */
//  Public API: create / remove device nodes across all instances
// ------------------------------------------------------------------ */

/// Create a device node in all mounted devtmpfs instances.
///
/// Called by device registration code (`kernel/dev/dev.c`, still C --
/// Wave 21). Kept `#[no_mangle]` per `kernel/inc/devtmpfs.h`'s `extern`
/// declaration -- see the module doc's "C-ABI surface kept" section.
pub(crate) extern "C" fn devtmpfs_create_node(name: *const c_char, mode: mode_t, dev: dev_t) -> c_int {
    Devtmpfs::ensure_init();

    if name.is_null() {
        return Devtmpfs::neg(EINVAL);
    }
    let name_len = strlen(name);
    if name_len == 0 {
        return Devtmpfs::neg(EINVAL);
    }

    // Allocate a registry entry (node struct + name string in one block).
    let alloc_size = core::mem::size_of::<DevtmpfsNode>() + name_len + 1;
    let raw = kmm_alloc(alloc_size);
    if raw.is_null() {
        return Devtmpfs::neg(ENOMEM);
    }
    let node = raw as *mut DevtmpfsNode;

    // SAFETY: `raw`/`node` is a freshly allocated, exclusively-owned
    // block of at least `alloc_size` bytes; `name_copy` is the trailing
    // `name_len + 1` bytes after the `DevtmpfsNode` header, exactly like
    // the C `(char *)(node + 1)` pointer arithmetic.
    unsafe {
        let name_copy = (raw as *mut u8).add(core::mem::size_of::<DevtmpfsNode>()) as *mut c_char;
        memmove(name_copy as *mut c_void, name as *const c_void, name_len);
        *(name_copy.add(name_len)) = 0;
        (*node).name = name_copy;
        (*node).name_len = name_len;
        (*node).mode = mode;
        (*node).dev = dev;
        let entry = &raw mut (*node).list_entry;
        (*entry).next = entry;
        (*entry).prev = entry;

        let mut guard = __DEVTMPFS_NODES.lock();
        list_push_front(&raw mut *guard, entry);
    }

    // If devtmpfs is already mounted, create the node live using the
    // stored superblock root inode (mount-point independent). This is
    // needed for devices registered after boot (e.g. PTYs).
    let root = Devtmpfs::get_root();
    if !root.is_null() {
        // SAFETY: `root`/`name`/`name_len` all satisfy
        // `Devtmpfs::mknod_relative`'s contract.
        unsafe {
            Devtmpfs::mknod_relative(root, name, name_len, mode, dev);
        }
    }

    0
}

/// Remove a device node from all mounted devtmpfs instances.
///
/// Kept `#[no_mangle]` per `kernel/inc/devtmpfs.h`'s `extern` declaration
/// -- see the module doc's "C-ABI surface kept" section.
pub(crate) extern "C" fn devtmpfs_remove_node(name: *const c_char) -> c_int {
    Devtmpfs::ensure_init();

    if name.is_null() {
        return Devtmpfs::neg(EINVAL);
    }
    let name_len = strlen(name);
    if name_len == 0 {
        return Devtmpfs::neg(EINVAL);
    }

    // Remove from the registry list.
    let mut found_node: *mut DevtmpfsNode = ptr::null_mut();
    // SAFETY: the module-private registry, guarded by `__DEVTMPFS_NODES`'s
    // lock throughout.
    {
        let mut guard = __DEVTMPFS_NODES.lock();
        let head = &raw mut *guard;
        // SAFETY: `guard` proves the lock is held; the ring is unmutated
        // during the read-only `.find()`. `find` stops at the match, so the
        // subsequent `list_detach` is NOT a mutation-during-walk.
        if let Some(cur) = unsafe {
            DevtmpfsNode::iter(head).find(|&cur| {
                let node = cur as *mut DevtmpfsNode;
                unsafe { (*node).name_len == name_len && strncmp((*node).name, name, name_len) == 0 }
            })
        } {
            unsafe { list_detach(cur) };
            found_node = cur as *mut DevtmpfsNode;
        }
    }

    if !found_node.is_null() {
        kmm_free(found_node as *mut c_void);
    }

    // Also unlink the live filesystem node if devtmpfs is mounted. Uses
    // the stored root inode so we don't depend on mount path.
    let root = Devtmpfs::get_root();
    if !root.is_null() {
        // SAFETY: `root`/`name`/`name_len` all satisfy
        // `Devtmpfs::unlink_relative`'s contract.
        unsafe {
            Devtmpfs::unlink_relative(root, name, name_len);
        }
    }

    if found_node.is_null() {
        Devtmpfs::neg(ENOENT)
    } else {
        0
    }
}

// ------------------------------------------------------------------ */
//  Retroactive population from device table
// ------------------------------------------------------------------ */

/// Callback for [`DevTable::for_each_device`]: create a devtmpfs registry entry
/// for each device that has a devname set but hasn't been registered yet.
/// Only ever reached from [`devtmpfs_populate_devices`] (as a closure
/// argument, `for_each_device` now takes `impl FnMut` rather than a C
/// fn-pointer + ctx), so this stays private -- same precedent as
/// [`devtmpfs_mount`].
fn __devtmpfs_register_one_device(dev: *mut device_t) -> c_int {
    // SAFETY: `dev` is a live `device_t*` (caller contract:
    // `DevTable::for_each_device`'s callback convention, `kernel/inc/dev/dev.h`).
    let devname = unsafe { (*dev).devname };
    if devname.is_null() {
        return 0; // device has no devtmpfs binding -- skip
    }

    // Check if a node already exists in the registry (e.g. because
    // device_register already called devtmpfs_create_node after the
    // registry list was initialised). Avoid duplicates.
    let name_len = strlen(devname);
    // SAFETY: registry access guarded by `__DEVTMPFS_NODES`'s lock
    // throughout; read-only `.any()` walk.
    let found = {
        let mut guard = __DEVTMPFS_NODES.lock();
        let head = &raw mut *guard;
        unsafe {
            DevtmpfsNode::iter(head).any(|cur| {
                let node = cur as *mut DevtmpfsNode;
                unsafe {
                    (*node).name_len == name_len && strncmp((*node).name, devname, name_len) == 0
                }
            })
        }
    };

    if !found {
        // SAFETY: `dev` live per this function's own contract above.
        let (major, minor, devmode) = unsafe { ((*dev).major, (*dev).minor, (*dev).devmode) };
        devtmpfs_create_node(devname, devmode, Devtmpfs::mkdev(major, minor));
    }
    0
}

/// Retroactively create devtmpfs entries for all devices that were
/// registered before devtmpfs became available. Called once by
/// [`devtmpfs_init`] after the filesystem type is registered. Kept
/// `#[no_mangle]` per `kernel/inc/devtmpfs.h`'s `extern` declaration --
/// see the module doc's "C-ABI surface kept" section.
pub(crate) extern "C" fn devtmpfs_populate_devices() {
    Devtmpfs::ensure_init();

    // Use the DevTable::for_each_device iterator to find all devices that were
    // registered before devtmpfs was initialised.
    DevTable::for_each_device(__devtmpfs_register_one_device);

    // Count how many nodes are now in the registry.
    // SAFETY: registry access guarded by `__DEVTMPFS_NODES`'s lock
    // throughout; read-only `.count()` walk.
    let count = {
        let mut guard = __DEVTMPFS_NODES.lock();
        let head = &raw mut *guard;
        unsafe { DevtmpfsNode::iter(head).count() as c_int }
    };

    crate::kprintln!("devtmpfs: populated {} device nodes from device table", count);
}

// ------------------------------------------------------------------ */
//  Initialisation
// ------------------------------------------------------------------ */

/// Initialise devtmpfs caches and register the filesystem type. Called
/// from `kernel/vfs/fs.rs`'s `vfs_init()` -- kept `#[no_mangle]` per that
/// extern declaration.
pub(crate) extern "C" fn devtmpfs_init() {
    Devtmpfs::ensure_init();

    // Register the devtmpfs filesystem type.
    let fs_type = VfsFsType::vfs_fs_type_allocate();
    kassert!(!fs_type.is_null(), "devtmpfs_init: vfs_fs_type_allocate failed");
    // SAFETY: `fs_type` is freshly allocated and exclusively owned here.
    unsafe {
        (*fs_type).name = c"devtmpfs".as_ptr();
        (*fs_type).ops = Some(&DEVTMPFS_FS_TYPE_OPS);
    }

    Vfs::vfs_mount_lock();
    let ret = VfsFsType::vfs_register_fs_type(fs_type);
    kassert!(ret == 0, "devtmpfs_init: vfs_register_fs_type failed");
    Vfs::vfs_mount_unlock();

    crate::kprintln!("devtmpfs: filesystem type registered");

    // Retroactively create devtmpfs entries for all devices that were
    // registered before devtmpfs became available. device_register()
    // calls devtmpfs_create_node() which buffers entries into the
    // registry list even when devtmpfs is not yet mounted. However, for
    // devices that were registered before this subsystem was initialised
    // (i.e. before __devtmpfs_ensure_init ran), we now iterate them and
    // add any that carry a devname.
    devtmpfs_populate_devices();
}
