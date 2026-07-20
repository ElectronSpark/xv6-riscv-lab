//! VFS inode operations — Rust port of `kernel/vfs/inode.c` (Phase 2
//! Wave 13, see `docs/rustify/phase2_plan.md`).
//!
//! This is the inode cache/refcount/locking core of the VFS: the
//! `vfs_inode` lifecycle (`vfs_idup`/`vfs_iput`/`vfs_ilock`/`vfs_iunlock`),
//! the `vfs_inode_ops` vtable dispatch to filesystem drivers (tmpfs/
//! xv6fs/devtmpfs — all still C), directory lookup/iteration, and
//! `vfs_namei` path resolution. Every other file under `kernel/vfs/`
//! (`fs.c`, `file.c`, `fdtable.c`, `vfs_syscall.c`, `pipe.c`, and the
//! `tmpfs/`/`xv6fs/`/`devtmpfs/` filesystem drivers) remains C and is
//! called through the `unsafe extern "C"` block below, exactly as any
//! other C translation unit would call these functions.
//!
//! # Lock map
//!
//! 1. mount lock (`vfs_mount_lock`, fs.c) — never touched by this file.
//! 2. `vfs_superblock.lock` (rwsem) — via the C `vfs_superblock_{r,w}lock`/
//!    `vfs_superblock_unlock` entry points (fs.c owns the `rwsem_t`
//!    storage; this file never reaches into `sb->lock` directly).
//! 3. `vfs_inode.mutex` — via [`vfs_ilock`]/[`vfs_ilock_trylock`]/
//!    [`vfs_iunlock`] (this file owns and ports these).
//! 4. buffer mutex / 5. filesystem log spinlock — internal to the
//!    filesystem driver callbacks invoked through `vfs_inode_ops`/
//!    `vfs_superblock_ops`; opaque to this file.
//!
//! `vfs_ilock`/`vfs_ilock_trylock`/`vfs_iunlock` are a paired C-ABI
//! lock/unlock triple called from a dozen other C translation units
//! (tmpfs, xv6fs, `file.c`, `fs.c`, `vfs_syscall.c`, ...) exactly like
//! `vm_rlock`/`vm_runlock` from `kernel/mm/vm.rs` (mm WP1 precedent) —
//! they stay thin forwards to `mutex_lock`/`mutex_trylock`/`mutex_unlock`
//! rather than `crate::sync::KMutex` RAII, because the matching unlock
//! call happens in a *different* function invocation (often a different
//! C translation unit), which no RAII guard can span. Internal critical
//! sections in this file follow the exact lock/unlock placement of the
//! C original (including its retry loops and asymmetric unlock
//! ordering — e.g. `vfs_iput`'s `inode´ then `sb` order, `vfs_unlink`'s
//! `target` then `dir` then `sb` order) rather than being restructured
//! into RAII scopes, because several of them (`vfs_iput` above all) drop
//! and reacquire different lock *combinations* across a `retry:` loop in
//! a way that does not nest as stack-scoped guards. `fs_struct.lock` (a
//! plain spinlock with no other C caller of its own address) is the one
//! internal, cleanly-paired site in this file and does use
//! [`crate::sync::KSpinlock`] RAII (see [`fs_lock`]), matching the
//! `crate::sync` precedent already used by `tty/ptmx.rs`/`tty/tty.rs`/
//! `irq/irq_core.rs`.
//!
//! # Refcount map
//!
//! `vfs_inode.ref_count` (a plain C `int`) is manipulated both by atomic
//! RMW (`vfs_idup`/`vfs_idup_not_zero`/`vfs_iput`/mount-descend in
//! `vfs_namei`, all lock-free) and, in exactly the two spots the C
//! original does, by a non-atomic read protected by the inode+superblock
//! locks held at that point (`vfs_iput`'s post-decrement underflow
//! assert, folded here into the `fetch_sub` return value instead of a
//! second racy read; `vfs_unlink`'s orphan-marking `n_links==0 &&
//! ref_count>1` check, kept as a genuine plain read to match the C
//! exactly — see the comment at that call site). All atomic ops use
//! `Ordering::SeqCst` (CAS) / `Ordering::Acquire` (initial load),
//! mirroring `smp/atomic.h`'s `atomic_oper_cond` macro
//! (`__ATOMIC_SEQ_CST` compare-exchange over an `__ATOMIC_ACQUIRE`
//! initial load) verbatim — this is a line-faithful port, not an
//! independent re-derivation of weaker orderings, because refcounting
//! correctness here depends on matching the original exactly.
//!
//! # Style notes (rust-skills)
//!
//! Every `unsafe` block is scoped to the smallest expression that needs
//! it (`unsafe-minimize-scope`) with a `SAFETY:` comment at each
//! non-obvious site; the C `static inline` helpers this file depends on
//! (`list_entry_init`/`list_entry_detach`/`hlist_entry_init` from
//! `list.h`/`hlist.h`, `__vfs_inode_valid`/`vfs_inode_is_local_root`
//! from `vfs_private.h`/`fs.h`) have no external linkage and are
//! reimplemented natively here, same precedent as `kernel/kobject.rs`
//! (Wave 1). Bitfield flags (`valid`/`dirty`/`mount`/`orphan`/
//! `destroying`/`delay_put`) are accessed through the native
//! [`VfsInodeFlagBits`] holder's `.flags.field()`/`.set_field()`
//! accessors (P3-N5; bit-identical to the bindgen `__bindgen_anon_1`
//! methods they replaced) on a temporary place expression
//! (`(*ptr).flags...`), the same pattern `kernel/mm/pcache.rs` uses —
//! never through the P3-7c boundary reference (bitfield/union access
//! stays on the raw place expression; see the Reference-ification note
//! below). Deliberate deviations from a byte-for-byte C
//! transliteration are called out inline (see `vfs_chroot`'s discarded
//! `vfs_chdir` return value, and the `panic!`-free tail-call use of
//! `xv6_panic`'s `-> !` return type replacing the C's dead
//! `return -EINVAL;`).
//!
//! # Reference-ification (P3-7c)
//!
//! Extending the [`fdtable.rs`](crate::vfs::fdtable) P3-7a and
//! [`file.rs`](crate::vfs::file) P3-7b hoist pattern to this file's
//! `*_inner` helpers: the null-check plus a single audited
//! `unsafe { &mut *inode }` conversion happen once, right after the entry
//! null-check (the outermost frame that genuinely receives a raw
//! pointer), so the interior *plain* field reads (`sb`, `mode`) become
//! ordinary safe access (`dir.sb` vs `unsafe { (*dir).sb }`). Signatures
//! keep their `*mut vfs_inode` (the extern C-ABI wrappers and the
//! cross-module `pub(crate)` callers pass raw pointers), so the hoist is
//! internal — matching the fdtable P3-7a slice.
//!
//! Converted: `vfs_create`/`vfs_mknod`/`vfs_mkdir`/`vfs_symlink`/
//! `vfs_link`/`vfs_unlink`/`vfs_readlink`/`vfs_itruncate`/
//! `vfs_dirty_inode`/`vfs_sync_inode`/`vfs_ilookup`/`vfs_dir_iter`/
//! `vfs_chdir`/`vfs_move`'s `_inner`s (the primary `dir`/`inode` param).
//! What deliberately STAYS raw (lifetime/union/traversal honesty):
//! * the `dev_mnt` union arm reads (`dev_mnt.mnt.mnt_rooti`) — union
//!   access is `unsafe` regardless of how the container is borrowed;
//! * the bitfield-flag accessors (`flags.mount()`/`.orphan()`/...), which
//!   operate on the raw place expression `(*ptr).flags`, unchanged;
//! * the [`InodeOps`] trait dispatch and the `vfs_ilock`/`vfs_iunlock`/
//!   `vfs_inode_valid_locked` C-ABI helpers — reached by reborrowing the
//!   reference as `&raw mut *inode`, a synchronous call (cross-module raw
//!   boundary, per P3-7b's trait-boundary finding);
//! * the refcount-drop / free paths — [`vfs_iput`]/[`vfs_iput_finalize`],
//!   the [`IRef`] smart pointer, `vfs_unlink`'s `target` (it is
//!   `vfs_iput`-ed) — a `&mut` there would assert validity past the point
//!   the object may be freed;
//! * the path-traversal walkers ([`vfs_namei_once`], `get_mnt_recursive`,
//!   `mountpoint_go_up`, `vfs_dotdot_target`, `vfs_ilock_two_directories`)
//!   — they hop `parent`/`sb`/mount pointers across *many* inodes and
//!   free them mid-walk, so no single `&mut` describes the working set;
//! * buffer / user-memory / `path` pointers.
//! Aliasing caveat, documented approximation (same class as file.rs's
//! P3-7b note and the crate's `__ATOMIC_CONSUME` -> `Acquire` mapping): a
//! held reference operated on under `vfs_ilock` may be observed by the
//! driver callback through the reborrowed raw pointer while the `&mut`
//! is nominally live; those accesses are serialized by the inode mutex
//! and invisible to each frame's codegen, but are a foreign `&mut`
//! overlap under a strict Stacked/Tree-Borrows reading — the same
//! layout-frozen approximation the crate accepts crate-wide.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ops::Deref;
use core::ptr;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicI32, AtomicU64, Ordering};

use crate::bindings::{
    dev_t, fs_struct, hlist_entry_t, list_node_t, loff_t, mode_t, thread, vfs_dentry,
    vfs_dir_iter, vfs_inode, vfs_inode_ref, vfs_superblock, EAGAIN, EBUSY, EINVAL, ENAMETOOLONG,
    ENOENT, ENOMEM, ENOSYS, ENOTDIR, ENOTEMPTY, EPERM, EXDEV,
};
use crate::proc::proc_shims::{xv6_current_thread, xv6_panic};

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N5 (VFS type family, dentry/dir-iter slice).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/vfs/
// vfs_types.h`'s `struct vfs_dentry`/`struct vfs_dir_iter` now:
// `build.rs` blocklists the bindgen-generated forms and injects `pub use
// crate::vfs::inode::... as ...;` facade re-exports (no `_t` typedefs
// exist for either). Field names/types reproduce bindgen's exactly; both
// derived Copy/Clone in the pre-nativization bindgen output (and
// `vfs_dir_iter` is still embedded by value by `vfs_file`'s position
// union, so an accurate Copy=Yes in build.rs's NativeTypeCallbacks is
// what keeps *that* derive line unchanged until `vfs_file` itself goes
// native).
// ---------------------------------------------------------------------------

/// `struct vfs_dentry` (`kernel/inc/vfs/vfs_types.h`). No dentry cache
/// right now; `name` is managed by the slab allocator.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VfsDentry {
    pub sb: *mut vfs_superblock,
    pub parent: *mut vfs_inode,
    pub ino: crate::bindings::uint64,
    pub name: *mut c_char,
    pub name_len: crate::bindings::uint16,
    pub cookies: crate::bindings::int64,
}

/// `struct vfs_dir_iter` (`kernel/inc/vfs/vfs_types.h`): directory
/// iteration state (`cookies` is filesystem-private and opaque).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VfsDirIter {
    pub cookies: crate::bindings::int64,
    pub index: crate::bindings::int64,
}

// P3-N5 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (verified in-tree by a temporary
// `offset_of!` gate on `crate::bindings::{vfs_dentry,vfs_dir_iter}`
// before the switch) and independently confirmed by a cross-compiler
// `_Static_assert` probe (toolchain riscv64-unknown-elf-gcc,
// rv64gc/lp64d, gcc & clang-18 agree) against `kernel/inc/vfs/
// vfs_types.h` — see the P3-N5 wave record
// (scratchpad p3n5_static_assert_probe.c): vfs_dentry 48/8 offsets
// 0/8/16/24/32/40, vfs_dir_iter 16/8 offsets 0/8.
const _: () = {
    assert!(core::mem::size_of::<VfsDentry>() == 48, "vfs_dentry size");
    assert!(core::mem::align_of::<VfsDentry>() == 8, "vfs_dentry alignment");
    assert!(core::mem::offset_of!(VfsDentry, sb) == 0, "vfs_dentry.sb offset");
    assert!(core::mem::offset_of!(VfsDentry, parent) == 8, "vfs_dentry.parent offset");
    assert!(core::mem::offset_of!(VfsDentry, ino) == 16, "vfs_dentry.ino offset");
    assert!(core::mem::offset_of!(VfsDentry, name) == 24, "vfs_dentry.name offset");
    assert!(core::mem::offset_of!(VfsDentry, name_len) == 32, "vfs_dentry.name_len offset");
    assert!(core::mem::offset_of!(VfsDentry, cookies) == 40, "vfs_dentry.cookies offset");
    assert!(core::mem::size_of::<VfsDirIter>() == 16, "vfs_dir_iter size");
    assert!(core::mem::align_of::<VfsDirIter>() == 8, "vfs_dir_iter alignment");
    assert!(core::mem::offset_of!(VfsDirIter, cookies) == 0, "vfs_dir_iter.cookies offset");
    assert!(core::mem::offset_of!(VfsDirIter, index) == 8, "vfs_dir_iter.index offset");
};

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N5 (VFS type family, inode hub slice).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/vfs/
// vfs_types.h`'s `struct vfs_inode`/`struct vfs_inode_ops` now:
// `build.rs` blocklists the bindgen-generated forms and injects `pub use
// crate::vfs::inode::... as ...;` facade re-exports (no `_t` typedefs
// exist). The ops-table fn-pointer fields reproduce bindgen's
// `Option<unsafe extern "C" fn>` forms exactly, including the `move` ->
// `move_` keyword rename (trait-ification is P3-10's job). The anonymous
// C bitfield struct became the named 8/8 `{bits,_pad}` holder
// [`VfsInodeFlagBits`] (field `flags`) with safe masking accessors
// bit-identical to bindgen's little-endian unit; the anonymous
// device/mount union became the named real Rust union
// [`VfsInodeDevMnt`] (field `dev_mnt`, inner named struct
// [`VfsInodeMnt`] for bindgen's nested `__bindgen_anon_1`); consumers
// re-pointed from `__bindgen_anon_1`/`__bindgen_anon_2`. Copy fidelity:
// `vfs_inode_ops` derived Copy/Clone in the pre-nativization bindgen
// output; `vfs_inode` derived NEITHER, so the native deliberately has
// no derives — the still-bindgen `tmpfs_inode`/`xv6fs_inode` embed
// `vfs_inode` BY VALUE (as their first field) and had no derives
// either; the accurate NONCOPY answer in build.rs keeps their derive
// lines unchanged. `pcache` (mm/pcache_types.h), `completion_t`-carried
// `mutex_t` lock types (already native), `stat` (uabi/stat.h — P3-4
// scrutiny class), `thread` and `vfs_inode_ref` stay referenced by
// their `crate::bindings` paths — the sanctioned mixed-tier pattern.
// `_pad0`/`_pad1`/`_pad2` reproduce bindgen's
// `__bindgen_padding_0/1/2` verbatim (the C `__ALIGNED_CACHELINE`
// rides the `mutex_t`/`completion_t` typedefs and the `struct pcache`
// record; the natives are align 8 in Rust, so the explicit pads carry
// the 64-byte placements, exactly as bindgen emitted them —
// `i_data`'s 64-alignment comes from the still-bindgen `pcache`'s own
// `repr(align(64))`).
// ---------------------------------------------------------------------------

/// Native replacement for the anonymous C bitfield struct inside
/// `struct vfs_inode` (bindgen's `vfs_inode__bindgen_ty_1`, 8/8): the
/// inode state flags. Bit order (LE unit byte 0): valid=0, dirty=1,
/// mount=2, orphan=3, destroying=4, delay_put=5 — identical to
/// bindgen's `get(N,1)`/`set(N,1)` accessors. See the C header's long
/// comment on the valid/dirty lifecycle (`kernel/inc/vfs/vfs_types.h`).
///
/// # Interior mutability (N-R5a)
///
/// The single bit unit is now an [`AtomicU64`] instead of a plain
/// `u8 + [u8; 7]` pad. These flags (`valid`/`dirty`/`mount`/`orphan`/
/// `destroying`) are read LOCK-FREE in the inode reclaim/evict pre-check
/// loops (both here and in `fs.rs`). A plain read of a Freeze field
/// through a `&VfsInode` inside such a loop is hoisted out by LLVM's
/// noalias analysis — the freeze-noalias HANG (see MEMORY
/// `freeze-noalias-hazard`) that gates the P3-7 `*mut vfs_inode ->
/// &VfsInode` conversion (N-R5b). An atomic is not Freeze, so the reads
/// stay in the loop. This is layout-identical to the former unit: the
/// `AtomicU64` is size 8 / align 8 and, on little-endian, keeps every
/// flag bit N in byte 0 exactly where the old `u8 bits` held it (the
/// upper seven bytes are the former `_pad`, never observed by any
/// accessor). The accessor method *signatures* are unchanged, so every
/// caller (this file, `fs.rs`, `vfs_syscall.rs`) is untouched — their
/// flag reads simply become non-hoistable relaxed atomics.
///
/// Ordering: [`Ordering::Relaxed`]. The real synchronization of these
/// flags is the inode mutex (every valid/dirty state transition and its
/// authoritative re-check happen under it); the lock-free pre-check
/// reads were plain, unordered bitfield accesses in the C original, so
/// `Relaxed` — which adds atomicity (no torn/lost bits) and defeats the
/// noalias hoist, but no cross-variable ordering — is both the weakest
/// correct choice (`conc-atomic-ordering`) and behaviour-identical to
/// the C's unordered bitfield semantics. No `Copy`/`Clone` (an
/// `AtomicU64` interior forbids them): nothing copies this holder by
/// value — every use is an accessor method call on the `(*ptr).flags`
/// place, and the embedding `VfsInode` was already non-`Copy`/`Clone`.
#[repr(C, align(8))]
pub struct VfsInodeFlagBits {
    bits: AtomicU64,
}

macro_rules! inode_flag_bit {
    ($(#[$attr:meta])* $get:ident, $set:ident, $bit:expr) => {
        $(#[$attr])*
        #[inline]
        pub(crate) fn $get(&self) -> u64 {
            (self.bits.load(Ordering::Relaxed) >> $bit) & 0b1
        }
        $(#[$attr])*
        #[inline]
        pub(crate) fn $set(&mut self, val: u64) {
            // Atomic RMW (`fetch_or`/`fetch_and`), never load-modify-store,
            // so concurrent single-bit updates to *other* flags cannot be
            // lost. Signature keeps `&mut self` (identical to the pre-N-R5a
            // form); the atomic methods take `&self`, reached by reborrow.
            if val & 0b1 != 0 {
                self.bits.fetch_or(1u64 << $bit, Ordering::Relaxed);
            } else {
                self.bits.fetch_and(!(1u64 << $bit), Ordering::Relaxed);
            }
        }
    };
}

impl VfsInodeFlagBits {
    inode_flag_bit!(valid, set_valid, 0);
    inode_flag_bit!(dirty, set_dirty, 1);
    inode_flag_bit!(mount, set_mount, 2);
    inode_flag_bit!(orphan, set_orphan, 3);
    inode_flag_bit!(destroying, set_destroying, 4);
    inode_flag_bit!(
        /// No Rust consumer today (the C original's tmpfs delay-put
        /// path never made it into a Rust call site) — kept for header
        /// fidelity, same `#[allow(dead_code)]`-documents-the-gap
        /// precedent as `dev/cdev.rs`'s `cdev_dup`.
        #[allow(dead_code)]
        delay_put, set_delay_put, 5
    );
}

/// Native replacement for the nested anonymous struct inside `struct
/// vfs_inode`'s device/mount union (bindgen's
/// `vfs_inode__bindgen_ty_2__bindgen_ty_1`, 16/8): the mounted
/// superblock + its root inode when this inode is a mountpoint.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VfsInodeMnt {
    pub mnt_sb: *mut vfs_superblock,
    pub mnt_rooti: *mut VfsInode,
}

/// Native replacement for the anonymous C union inside `struct
/// vfs_inode` (bindgen's `vfs_inode__bindgen_ty_2`, 16/8): the
/// character/block device number for device inodes, or the mount
/// linkage ([`VfsInodeMnt`]) for mountpoint inodes.
#[repr(C)]
#[derive(Copy, Clone)]
pub union VfsInodeDevMnt {
    pub cdev: crate::bindings::uint32,
    pub bdev: crate::bindings::uint32,
    pub mnt: VfsInodeMnt,
}

/// `struct vfs_inode` (`kernel/inc/vfs/vfs_types.h`) — the VFS hub
/// type. The anonymous bitfield struct became the named `flags` field;
/// the anonymous device/mount union became the named `dev_mnt` field
/// (consumers re-pointed from bindgen's
/// `__bindgen_anon_1`/`__bindgen_anon_2`).
#[repr(C, align(64))]
pub struct VfsInode {
    pub hash_entry: hlist_entry_t,
    pub ino: crate::bindings::uint64,
    pub n_links: crate::bindings::uint32,
    pub n_blocks: crate::bindings::uint64,
    pub size: loff_t,
    pub mode: crate::bindings::uint32,
    pub uid: crate::bindings::uint32,
    pub gid: crate::bindings::uint32,
    pub atime: crate::bindings::uint64,
    pub mtime: crate::bindings::uint64,
    pub ctime: crate::bindings::uint64,
    pub(crate) _pad0: [u64; 4],
    pub mutex: crate::bindings::mutex_t,
    pub flags: VfsInodeFlagBits,
    pub orphan_entry: list_node_t,
    pub owner: *mut thread,
    pub sb: *mut vfs_superblock,
    pub i_mapping: *mut crate::bindings::pcache,
    pub(crate) _pad1: [u64; 2],
    pub i_data: crate::bindings::pcache,
    /// The driver's inode-operations vtable — a real Rust trait object
    /// as of P3-10b (16-byte fat pointer; `None` only for the zeroed
    /// dummy `vfs_root_inode`, which never reaches a dispatch site —
    /// exactly the role of the old null table pointer). The fat pointer
    /// absorbed the old `_pad2` word, keeping `completion` on its
    /// 64-byte boundary.
    pub ops: Option<&'static dyn InodeOps>,
    pub ref_count: c_int,
    pub fs_data: *mut c_void,
    pub parent: *mut VfsInode,
    pub name: *mut c_char,
    pub dev_mnt: VfsInodeDevMnt,
    pub completion: crate::bindings::completion_t,
}

impl VfsInode {
    /// Borrow this inode's superblock, if attached (N-R5c).
    ///
    /// `sb` is a **set-once** pointer: once an inode is added to a
    /// superblock's inode hash it points to that live `vfs_superblock`
    /// for the rest of the inode's life, and the superblock outlives
    /// every inode still referencing it (`vfs_iput` frees the sb only
    /// after removing its last inode). The returned shared borrow is
    /// bounded by `&self`, so it can never outlive a live borrow of the
    /// inode, and callers read only the now-atomic `flags` (N-R5c) and
    /// other set-once fields (`root_inode`) through it — free of the
    /// freeze-noalias hazard even inside a loop.
    ///
    /// This encapsulates the single `*mut vfs_superblock -> &` deref that
    /// the inode-validity predicates each used to spell inline as an
    /// `unsafe`-wrapped `(*sb).flags.valid()` block — the superblock
    /// raw-ptr floor N-R5b left standing (now collapsed: the callers
    /// become unsafe-free).
    #[inline]
    pub(crate) fn superblock(&self) -> Option<&vfs_superblock> {
        // SAFETY: `self.sb`, when non-null, is the set-once pointer to
        // this inode's live superblock (see the method doc); the returned
        // borrow is tied to `&self`, so the superblock cannot be freed
        // under it. `as_ref` yields `None` for a null pointer.
        unsafe { self.sb.as_ref() }
    }

    /// Whether this inode is the local root of its own filesystem
    /// (mirrors `fs.h`'s `static inline vfs_inode_is_local_root()`).
    ///
    /// N-METH: the shared `&self` method that unifies the two former
    /// free-fn twins — `inode.rs`'s raw `vfs_inode_is_local_root(*mut
    /// vfs_inode)` (now a thin null-tolerant delegate) and `fs.rs`'s
    /// borrowed `inode_is_local_root(&vfs_inode)` (deleted, callers
    /// repointed here). Only the **set-once** `sb`/`root_inode`/`parent`
    /// fields are touched — no Freeze field, no loop — so the shared
    /// borrow carries no freeze-noalias hazard, and routing the
    /// superblock deref through [`VfsInode::superblock`] leaves the
    /// method unsafe-free (the raw `(*sb).root_inode` block the `fs.rs`
    /// free fn still carried collapses).
    pub(crate) fn is_local_root(&self) -> bool {
        let Some(sb) = self.superblock() else {
            return false;
        };
        if ptr::eq(self, sb.root_inode) {
            kassert!(
                ptr::eq(self.parent, self),
                "vfs_inode_is_local_root: root inode's parent is not itself"
            );
            return true;
        }
        false
    }
}

/// The per-filesystem inode-operations vtable — wave P3-10b's
/// replacement for the C-style `struct vfs_inode_ops` fn-pointer table
/// (the 19-slot flagship of the ops-table redesign; P3-10a `FileOps`
/// precedent: full Rust style, C-compatible interface removed).
/// Implementors are zero-sized unit structs with a `static` instance
/// (`XV6FS_INODE_OPS`, `TMPFS_INODE_OPS` — devtmpfs inodes come from
/// tmpfs's allocator and share `TMPFS_INODE_OPS`) installed into
/// [`VfsInode::ops`] by the driver's inode allocator as
/// `Some(&STATIC)`. The VFS core acquires the inode mutex before
/// invoking any callback (see each method's `# Safety`).
///
/// Slot-nullability mapping (each old `Option<fn>` slot's `None`
/// dispatch behavior is preserved exactly):
///
/// * `lookup`/`dir_iter`/`readlink`/`create`/`getattr`/`setattr`/
///   `link`/`unlink`/`mkdir`/`rmdir`/`mknod`/`symlink`/`truncate`/
///   `destroy_inode`/`free_inode`/`open` — REQUIRED methods: every
///   historical table (xv6fs, tmpfs) filled all sixteen, so "ops
///   present but slot `None`" never existed for them; the old
///   dispatches' missing-slot branches (`-ENOSYS` returns, `vfs_iput`'s
///   skip-the-destroy-dance, `vfs_dir_isempty`'s assume-not-empty,
///   `vfs_iput_finalize`'s `expect`) were all dead code and collapse
///   with the trait guarantee. (`getattr` keeps a genuine fallback, but
///   it keys on the whole table being absent — the zeroed
///   `vfs_root_inode` — which is now `VfsInode::ops == None`, not on a
///   `None` slot.)
/// * `move_` — default `Err(Errno::NoSys)`: exactly what `vfs_move`
///   returned for xv6fs's `None` slot. (Keeps bindgen's `move` ->
///   `move_` keyword rename for traceability.) The old fn-pointer
///   equality gate (`old_move != new_move` -> `-ENOSYS`) collapsed:
///   `vfs_move` proves both directories same-superblock first
///   (`-EXDEV`), and every inode of a superblock shares its driver's
///   single ops instance, so the tables can no longer differ.
/// * `dirty_inode`/`sync_inode` — default `Ok(())`: a `None` slot
///   (tmpfs) was silently skipped / treated as success by
///   `vfs_dirty_inode`/`vfs_sync_inode`/`__vfs_evict_unused_inodes`,
///   and the no-op `Ok(())` takes the identical path.
///
/// The four `*mut vfs_inode`-returning slots (`create`/`mkdir`/
/// `mknod`/`symlink`) drop their `ERR_PTR` encoding for
/// `KResult<*mut VfsInode>` (never `Ok(null)`); `Errno::Again` (or a
/// raw `-EAGAIN` passthrough) drives the callers' documented
/// inode-allocation retry loops exactly as the old `EAGAIN` pointer
/// did.
///
/// `Sync` supertrait: instances are shared crate-wide as `&'static`
/// references reachable from any CPU.
pub trait InodeOps: Sync {
    /// Look `name` up in directory `dir`, filling `dentry` on success.
    ///
    /// # Safety
    /// `dir` must be a live, valid directory inode of this driver with
    /// its mutex held (and its superblock read-locked); `dentry` must
    /// be a live out-param; `name` must have `name_len` readable bytes.
    unsafe fn lookup(
        &self,
        dir: *mut VfsInode,
        dentry: *mut VfsDentry,
        name: *const c_char,
        name_len: usize,
    ) -> KResult<()>;

    /// Advance one directory entry, filling `ret_dentry` (an empty
    /// name signals end-of-directory).
    ///
    /// # Safety
    /// Same locking contract as [`InodeOps::lookup`]; `iter`/
    /// `ret_dentry` must be live and VFS-prepared.
    unsafe fn dir_iter(
        &self,
        dir: *mut VfsInode,
        iter: *mut VfsDirIter,
        ret_dentry: *mut VfsDentry,
    ) -> KResult<()>;

    /// Read a symlink's target into `buf`, returning the byte count.
    ///
    /// # Safety
    /// `inode` must be a live, valid symlink inode of this driver with
    /// its mutex held; `buf` must be writable for `buflen` bytes.
    unsafe fn readlink(&self, inode: *mut VfsInode, buf: *mut c_char, buflen: usize)
        -> KResult<isize>;

    /// Create a regular file `name` in `dir`, returning the new inode
    /// (unlocked, one reference).
    ///
    /// # Safety
    /// `dir` must be a live, valid directory inode of this driver with
    /// its mutex held, its superblock write-locked, and (for
    /// transactional drivers) a transaction open; `name` must have
    /// `name_len` readable bytes.
    unsafe fn create(
        &self,
        dir: *mut VfsInode,
        mode: mode_t,
        name: *const c_char,
        name_len: usize,
    ) -> KResult<*mut VfsInode>;

    /// Fill `stat` from `inode` (which is NOT yet locked — the driver
    /// takes the inode mutex itself, matching the C convention).
    ///
    /// # Safety
    /// `inode` must be a live inode of this driver, NOT locked by the
    /// caller; `stat` must be a live out-param.
    unsafe fn getattr(&self, inode: *mut VfsInode, stat: *mut crate::bindings::stat)
        -> KResult<()>;

    /// Apply `stat` to `inode`. (No VFS dispatch site exists yet —
    /// slot preserved from the C table; both drivers reject with
    /// `-EOPNOTSUPP`.)
    ///
    /// # Safety
    /// `inode` must be a live inode of this driver; `stat` must be a
    /// live kernel `stat`.
    unsafe fn setattr(&self, inode: *mut VfsInode, stat: *const crate::bindings::stat)
        -> KResult<()>;

    /// Hard-link existing inode `old` into `dir` under `name`.
    ///
    /// # Safety
    /// `old`/`dir` must be live, valid inodes of this driver, both
    /// mutexes held, the superblock write-locked, and (for
    /// transactional drivers) a transaction open.
    unsafe fn link(
        &self,
        old: *mut VfsInode,
        dir: *mut VfsInode,
        name: *const c_char,
        name_len: usize,
    ) -> KResult<()>;

    /// Remove the directory entry `dentry` referring to non-directory
    /// `target`.
    ///
    /// # Safety
    /// `dentry` must be a live dentry naming `target`; `target` and its
    /// parent directory must be locked, the superblock write-locked,
    /// and (for transactional drivers) a transaction open.
    unsafe fn unlink(&self, dentry: *mut VfsDentry, target: *mut VfsInode) -> KResult<()>;

    /// Create directory `name` in `dir` (same return/locking contract
    /// as [`InodeOps::create`]).
    ///
    /// # Safety
    /// Same contract as [`InodeOps::create`].
    unsafe fn mkdir(
        &self,
        dir: *mut VfsInode,
        mode: mode_t,
        name: *const c_char,
        name_len: usize,
    ) -> KResult<*mut VfsInode>;

    /// Remove the directory entry `dentry` referring to the empty
    /// directory `target`.
    ///
    /// # Safety
    /// Same contract as [`InodeOps::unlink`] (plus: the VFS core has
    /// already proven `target` empty and not busy).
    unsafe fn rmdir(&self, dentry: *mut VfsDentry, target: *mut VfsInode) -> KResult<()>;

    /// Create a device/special file `name` in `dir` (same return and
    /// locking contract as [`InodeOps::create`]).
    ///
    /// # Safety
    /// Same contract as [`InodeOps::create`].
    unsafe fn mknod(
        &self,
        dir: *mut VfsInode,
        mode: mode_t,
        dev: dev_t,
        name: *const c_char,
        name_len: usize,
    ) -> KResult<*mut VfsInode>;

    /// Rename `old_dentry` (in `old_dir`) to `name` in `new_dir`.
    ///
    /// # Safety
    /// `old_dir`/`new_dir` must be live, valid, same-superblock
    /// directory inodes of this driver, both mutexes held and the
    /// superblock write-locked; `old_dentry` must be live; `name` must
    /// have `name_len` readable bytes.
    unsafe fn move_(
        &self,
        _old_dir: *mut VfsInode,
        _old_dentry: *mut VfsDentry,
        _new_dir: *mut VfsInode,
        _name: *const c_char,
        _name_len: usize,
    ) -> KResult<()> {
        Err(Errno::NoSys) // Rename not implemented (old `None`-slot behavior).
    }

    /// Create symlink `name` -> `target` in `dir` (same return and
    /// locking contract as [`InodeOps::create`]).
    ///
    /// # Safety
    /// Same contract as [`InodeOps::create`]; `target` must have
    /// `target_len` readable bytes.
    unsafe fn symlink(
        &self,
        dir: *mut VfsInode,
        mode: mode_t,
        name: *const c_char,
        name_len: usize,
        target: *const c_char,
        target_len: usize,
    ) -> KResult<*mut VfsInode>;

    /// Truncate the regular file `inode` to `new_size` bytes.
    ///
    /// # Safety
    /// `inode` must be a live, valid regular-file inode of this driver
    /// with its mutex held.
    unsafe fn truncate(&self, inode: *mut VfsInode, new_size: loff_t) -> KResult<()>;

    /// Destroy `inode`'s backing data (last reference gone and
    /// `n_links == 0`, or filesystem detached). Called with the inode
    /// LOCKED for cache-eviction/unmount paths but UNLOCKED (and
    /// transaction open) from `vfs_iput`'s destroy dance — exactly the
    /// C callback's two calling conventions.
    ///
    /// # Safety
    /// `inode` must be a live inode of this driver per the convention
    /// above; no other thread may be using its data.
    unsafe fn destroy_inode(&self, inode: *mut VfsInode);

    /// Free the in-memory inode structure itself (after removal from
    /// the superblock's inode cache).
    ///
    /// # Safety
    /// `inode` must be a dead, unhashed, unlocked inode of this driver
    /// with no remaining references.
    unsafe fn free_inode(&self, inode: *mut VfsInode);

    /// Mark `inode` dirty in the driver's own bookkeeping.
    ///
    /// # Safety
    /// `inode` must be a live, valid inode of this driver with its
    /// mutex held.
    unsafe fn dirty_inode(&self, _inode: *mut VfsInode) -> KResult<()> {
        Ok(()) // VFS-level dirty flag only (old `None`-slot behavior).
    }

    /// Write `inode`'s metadata back to the backend.
    ///
    /// # Safety
    /// `inode` must be a live, valid inode of this driver with its
    /// mutex held and (for transactional drivers) a transaction open.
    unsafe fn sync_inode(&self, _inode: *mut VfsInode) -> KResult<()> {
        Ok(()) // Backendless filesystem (old `None`-slot skip).
    }

    /// Per-open hook: install the driver's `FileOps` into `file` (and
    /// do any lazy per-inode setup). Called with the inode mutex held.
    ///
    /// # Safety
    /// `inode` must be a live, valid inode of this driver with its
    /// mutex held; `file` must be a live, freshly allocated `vfs_file`.
    unsafe fn open(
        &self,
        inode: *mut VfsInode,
        file: *mut crate::bindings::vfs_file,
        f_flags: c_int,
    ) -> KResult<()>;
}

/// The driver ops of a driver-allocated inode. Every inode reaching a
/// dispatch site was allocated by a filesystem driver that installed
/// its ops (the sole `None`-ops inode, the dummy `vfs_root_inode`,
/// never reaches one), so a `None` here is a bug — the old code would
/// have dereferenced a null table pointer (UB); this panics instead.
#[inline]
pub(crate) fn inode_ops(inode: *mut VfsInode) -> &'static dyn InodeOps {
    // SAFETY: every caller passes a non-null, live inode (their own
    // documented precondition).
    unsafe { (*inode).ops.expect("vfs: inode ops missing") }
}

// P3-10b layout facts — NATIVE-OWNED, no C mirror exists (bindgen,
// wrapper.h, and every C consumer are gone; these asserts document the
// new truth rather than pinning to a header). `vfs_inode_ops` no
// longer exists as a record type (it is the [`InodeOps`] trait now),
// so its 152/8 table asserts are deleted. `vfs_inode` keeps its
// 1088/64 size/align and every offset up to and including `i_data`:
// the fat `ops` pointer (8 -> 16 bytes) absorbed the old trailing
// `_pad2` word, so `completion` stays on its 64-byte boundary at 960
// (the alignment invariant the record leans on, together with
// `mutex`@128, `flags`@256 and the pcache-aligned `i_data`@320); the
// small fields between `ops` and `completion` each shifted +8 (no
// consumer depends on their absolute offsets — asserted below as the
// new truth). The niche-optimized `Option<&'static dyn InodeOps>`
// stays a plain fat pointer (16/8), and the all-zero bit pattern of
// the zeroed dummy `vfs_root_inode` reads as `None` (the data-pointer
// null niche), preserving `fs.rs`'s `mem::zeroed()` initialization.
const _: () = {
    assert!(core::mem::size_of::<VfsInodeFlagBits>() == 8, "vfs_inode anon bitfield size");
    assert!(core::mem::align_of::<VfsInodeFlagBits>() == 8, "vfs_inode anon bitfield align");
    assert!(core::mem::size_of::<VfsInodeMnt>() == 16, "vfs_inode mnt struct size");
    assert!(core::mem::offset_of!(VfsInodeMnt, mnt_sb) == 0, "vfs_inode.mnt_sb offset in union");
    assert!(core::mem::offset_of!(VfsInodeMnt, mnt_rooti) == 8, "vfs_inode.mnt_rooti offset in union");
    assert!(core::mem::size_of::<VfsInodeDevMnt>() == 16, "vfs_inode dev/mnt union size");
    assert!(core::mem::align_of::<VfsInodeDevMnt>() == 8, "vfs_inode dev/mnt union alignment");
    assert!(
        core::mem::size_of::<Option<&'static dyn InodeOps>>() == 16,
        "inode ops fat pointer size"
    );
    assert!(
        core::mem::align_of::<Option<&'static dyn InodeOps>>() == 8,
        "inode ops fat pointer alignment"
    );
    assert!(core::mem::size_of::<VfsInode>() == 1088, "vfs_inode size");
    assert!(core::mem::align_of::<VfsInode>() == 64, "vfs_inode alignment");
    assert!(core::mem::offset_of!(VfsInode, hash_entry) == 0, "vfs_inode.hash_entry offset");
    assert!(core::mem::offset_of!(VfsInode, ino) == 24, "vfs_inode.ino offset");
    assert!(core::mem::offset_of!(VfsInode, n_links) == 32, "vfs_inode.n_links offset");
    assert!(core::mem::offset_of!(VfsInode, n_blocks) == 40, "vfs_inode.n_blocks offset");
    assert!(core::mem::offset_of!(VfsInode, size) == 48, "vfs_inode.size offset");
    assert!(core::mem::offset_of!(VfsInode, mode) == 56, "vfs_inode.mode offset");
    assert!(core::mem::offset_of!(VfsInode, uid) == 60, "vfs_inode.uid offset");
    assert!(core::mem::offset_of!(VfsInode, gid) == 64, "vfs_inode.gid offset");
    assert!(core::mem::offset_of!(VfsInode, atime) == 72, "vfs_inode.atime offset");
    assert!(core::mem::offset_of!(VfsInode, mtime) == 80, "vfs_inode.mtime offset");
    assert!(core::mem::offset_of!(VfsInode, ctime) == 88, "vfs_inode.ctime offset");
    assert!(core::mem::offset_of!(VfsInode, mutex) == 128, "vfs_inode.mutex offset");
    assert!(core::mem::offset_of!(VfsInode, flags) == 256, "vfs_inode anon bitfield offset");
    assert!(core::mem::offset_of!(VfsInode, orphan_entry) == 264, "vfs_inode.orphan_entry offset");
    assert!(core::mem::offset_of!(VfsInode, owner) == 280, "vfs_inode.owner offset");
    assert!(core::mem::offset_of!(VfsInode, sb) == 288, "vfs_inode.sb offset");
    assert!(core::mem::offset_of!(VfsInode, i_mapping) == 296, "vfs_inode.i_mapping offset");
    assert!(core::mem::offset_of!(VfsInode, i_data) == 320, "vfs_inode.i_data offset");
    assert!(core::mem::offset_of!(VfsInode, ops) == 896, "vfs_inode.ops offset");
    assert!(core::mem::offset_of!(VfsInode, ref_count) == 912, "vfs_inode.ref_count offset");
    assert!(core::mem::offset_of!(VfsInode, fs_data) == 920, "vfs_inode.fs_data offset");
    assert!(core::mem::offset_of!(VfsInode, parent) == 928, "vfs_inode.parent offset");
    assert!(core::mem::offset_of!(VfsInode, name) == 936, "vfs_inode.name offset");
    assert!(core::mem::offset_of!(VfsInode, dev_mnt) == 944, "vfs_inode anon union offset");
    assert!(
        core::mem::offset_of!(VfsInode, completion) == 960,
        "vfs_inode.completion offset (cache-line boundary)"
    );
};
/// Native `struct vfs_inode_ref` (`kernel/inc/types.h`) — a paired
/// (superblock, inode) reference, embedded by value by the (native)
/// `vfs_file` (its `inode` field) and `fs_struct` (`rooti`/`cwd`).
///
/// P3-N8 nativization (user directive: remove the C-compatible
/// interfaces; N5 skipped this one). `build.rs` blocklists the bindgen
/// emission and re-exports this type as `crate::bindings::vfs_inode_ref`
/// (facade `pub use`, N2 pattern). Derives Copy/Clone exactly as the
/// pre-nativization bindgen emission did (two raw pointers — the
/// by-value embedders' derive lines depend on it).
///
/// Layout evidence: temporary in-tree `offset_of!` gate on the live
/// bindgen form + cross-compiler `_Static_assert`/value probe
/// (toolchain gcc, rv64gc/lp64d — scratchpad p3n8_probe_values.c);
/// both agree on every value asserted below (no pipe-style divergence).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VfsInodeRef {
    pub sb: *mut vfs_superblock,
    pub inode: *mut vfs_inode,
}

// P3-N8 hardcoded layout proof — values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the gcc probe.
const _: () = {
    assert!(core::mem::size_of::<VfsInodeRef>() == 16, "vfs_inode_ref size");
    assert!(core::mem::align_of::<VfsInodeRef>() == 8, "vfs_inode_ref alignment");
    assert!(core::mem::offset_of!(VfsInodeRef, sb) == 0, "vfs_inode_ref.sb offset");
    assert!(core::mem::offset_of!(VfsInodeRef, inode) == 8, "vfs_inode_ref.inode offset");
};

// ===========================================================================
// Externs — every cross-module C-ABI symbol this file calls, declared
// locally rather than imported by Rust path (matches the established
// convention across the crate: `irq/trap.rs`, `irq/syscall.rs`,
// `tty/session.rs`, `mm/vm.rs`'s own externs).
//
// Nearly everything is marked `safe`: passing a raw pointer is never
// itself unsafe in Rust, and every one of these C entry points either
// null-checks internally (the `vfs_superblock_*` lock wrappers) or is
// called here only with pointers this file already proved non-null —
// the same `safe fn` convention `kernel/mm/vm.rs`/`kernel/sync/sync.rs`
// use for `rwsem_acquire_read` and friends. `printf` (C-variadic) is the
// one exception: every call site wraps it in `unsafe`, matching every
// other printf-calling module in the crate.
// ===========================================================================

// P3-D2a: `scheduler_yield` (proc/sched.rs) is a plain crate-path item
// now that its `extern "C"` redeclaration is gone.
use crate::proc::Scheduler;

// P3-D3b: lock/mutex.rs's entry points (for `inode->mutex`) are plain
// safe Rust fns now that their `#[no_mangle]` exports are gone; reached
// by crate path.
use crate::lock::mutex::{holding_mutex, mutex_init, mutex_lock, mutex_trylock, mutex_unlock};

unsafe extern "C" {
    // printf.rs — C-variadic.

    // string.rs
    safe fn strndup(s: *const c_char, n: usize) -> *mut c_char;
    safe fn strtok_r(s: *mut c_char, delim: *const c_char, saveptr: *mut *mut c_char) -> *mut c_char;
    safe fn strlen(s: *const c_char) -> usize;
}

// P3-D3a: `kmm_alloc`/`kmm_free` are genuinely `unsafe fn` in
// `crate::mm::kalloc` now that their `#[no_mangle]` exports are gone;
// `cffi::raw`'s existing thin safe wrappers (identical signatures)
// preserve the `safe fn` facade the old redeclarations asserted.
use crate::mm::cffi::raw::{kmm_alloc, kmm_free};

// P3-1C mesh sweep: vfs/fs.rs is in scope for this wave; these were its
// superblock lock/refcount + inode-cache/dentry/orphan helpers,
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures, same `crate::bindings::*` types this file
// already imports). `vfs_root_inode` is fs.rs's single dummy VFS-root
// `vfs_inode` instance (no superblock; see `vfs_private.h`) -- only its
// address is ever used here (pointer identity checks), never its
// contents.
use crate::vfs::fs::{
    __vfs_final_unmount_cleanup, sb_ops, vfs_get_dentry_inode_inner, vfs_inode_deref,
    vfs_inode_get_ref,
    vfs_inode_put_ref, vfs_make_orphan, vfs_release_dentry, vfs_remove_inode, vfs_root_inode,
    vfs_superblock_rlock, vfs_superblock_unlock, vfs_superblock_wholding, vfs_superblock_wlock,
};

// `kassert!`'s canonical home is crate root / `crate::kstd` (P3-CS2
// centralization).
use crate::kassert;

// ===========================================================================
// Small helpers: negative-errno constants, ERR_PTR family, mode bits,
// list/hlist "static inline" reimplementations, refcount atomics.
// ===========================================================================

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

// `err_ptr`/`is_err`/`is_err_or_null`/`ptr_err`'s canonical home is now
// `crate::kstd` (P3-CS2 centralization; this file's old doc comment here
// used to explain why it kept its own copy "rather than reusing
// `kernel/proc/access.rs`'s equivalents" — see `kstd.rs`'s module doc for
// the full history). Note `kstd::ptr_err` returns `c_int`, not `isize` —
// every call site below already casts its result to `c_int` or compares
// it against another `c_int` (`neg(...)`), so the narrower return type is
// a no-op change (see `is_eagain_ptr`, whose `as isize` comparison cast
// is dropped accordingly).
use crate::kstd::{
    errptr_to_result, is_err, is_err_or_null, ptr_err, result_to_errptr,
    result_to_neg_errno, Errno, KResult,
};

/// P3-10b: the `KResult` spelling of the old `is_eagain_ptr` retry
/// check — matches both `Errno::Again` and a raw `-EAGAIN` passthrough,
/// exactly as the encoded-pointer form did.
#[inline(always)]
fn is_again<T>(r: &KResult<T>) -> bool {
    matches!(r, Err(e) if e.neg() == neg(EAGAIN))
}

// `uabi/stat.h`'s `S_IF*`/`S_IS*` macros.
const S_IFMT: u32 = 0o170000;
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const S_IFLNK: u32 = 0o120000;
#[inline(always)]
fn is_dir(mode: u32) -> bool {
    mode & S_IFMT == S_IFDIR
}
#[inline(always)]
fn is_reg(mode: u32) -> bool {
    mode & S_IFMT == S_IFREG
}
#[inline(always)]
fn is_lnk(mode: u32) -> bool {
    mode & S_IFMT == S_IFLNK
}

const VFS_DITER_INDEX_END: i64 = -1;
const VFS_DITER_INDEX_START: i64 = 0;
const VFS_DITER_INDEX_CURRENT: i64 = 1;
const VFS_DITER_INDEX_PARENT: i64 = 2;
const VFS_PATH_MAX: usize = 65535;
const VFS_INODE_MAX_REFCOUNT: i32 = 0x7FFF0000;
const VFS_NAMEI_MAX_RETRIES: i32 = 10;

#[inline(always)]
fn root_inode_ptr() -> *mut vfs_inode {
    // SAFETY: taking the address of an extern static is always sound;
    // this never reads through the pointer.
    unsafe { ptr::addr_of_mut!(vfs_root_inode) }
}

// ---------------------------------------------------------------------------
// `list.h`/`hlist.h` `static inline` primitives (no external linkage —
// reimplemented natively here, same precedent as `kernel/kobject.rs`).
// ---------------------------------------------------------------------------

/// Mirrors `list_entry_init()`.
///
/// # Safety
/// `e` must point to a live `list_node_t`.
#[inline(always)]
unsafe fn ln_init(e: *mut list_node_t) {
    unsafe {
        (*e).next = e;
        (*e).prev = e;
    }
}

/// Mirrors `list_entry_detach()`.
///
/// # Safety
/// `e` must point to a live, linked `list_node_t`.
#[inline(always)]
unsafe fn ln_detach(e: *mut list_node_t) {
    unsafe {
        let prev = (*e).prev;
        let next = (*e).next;
        (*prev).next = next;
        (*next).prev = prev;
        ln_init(e);
    }
}

/// Mirrors `hlist_entry_init()`.
///
/// # Safety
/// `e` must point to a live `hlist_entry_t`.
#[inline(always)]
unsafe fn hli_init(e: *mut hlist_entry_t) {
    unsafe {
        (*e).bucket = ptr::null_mut();
        ln_init(ptr::addr_of_mut!((*e).list_entry));
    }
}

// ---------------------------------------------------------------------------
// `ref_count` atomics — mirrors `smp/atomic.h`'s `atomic_dec_unless`/
// `atomic_inc_unless`/`atomic_inc_in_range`/`atomic_dec` macros exactly
// (see the module doc's "Refcount map" section for the ordering
// rationale).
// ---------------------------------------------------------------------------

/// # Safety
/// `inode` must point to a live, aligned `vfs_inode`.
#[inline(always)]
fn refcount_atomic<'a>(inode: *mut vfs_inode) -> &'a AtomicI32 {
    // SAFETY: `ref_count` is a plain C `int` field, same size/align as
    // `AtomicI32`; caller ensures `inode` is a live, aligned `vfs_inode`.
    unsafe { &*(ptr::addr_of_mut!((*inode).ref_count) as *const AtomicI32) }
}

fn atomic_oper_cond(
    a: &AtomicI32,
    mut op: impl FnMut(i32) -> i32,
    mut cond: impl FnMut(i32) -> bool,
) -> bool {
    let mut val = a.load(Ordering::Acquire);
    while cond(val) {
        let new_val = op(val);
        match a.compare_exchange(val, new_val, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(_) => return true,
            Err(cur) => val = cur,
        }
    }
    false
}
#[inline(always)]
fn atomic_dec_unless(a: &AtomicI32, unless: i32) -> bool {
    atomic_oper_cond(a, |v| v - 1, move |v| v != unless)
}
#[inline(always)]
fn atomic_inc_unless(a: &AtomicI32, unless: i32) -> bool {
    atomic_oper_cond(a, |v| v + 1, move |v| v != unless)
}
#[inline(always)]
fn atomic_inc_in_range(a: &AtomicI32, min: i32, max: i32) -> bool {
    atomic_oper_cond(a, |v| v + 1, move |v| v > min && v < max)
}

/// Mirrors `fs.h`'s `static inline vfs_inode_refcount()` (`__ATOMIC_SEQ_CST`
/// load). Used only for the `vfs_unlink` `EBUSY` check, matching the one
/// call site in the C original that goes through the atomic helper rather
/// than a plain field read (see `vfs_unlink`'s orphan-marking check for
/// the contrasting plain read, preserved 1:1 from the C).
fn vfs_inode_refcount_locked(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() {
        return -1;
    }
    refcount_atomic(inode).load(Ordering::SeqCst)
}

fn fs_lock(fs: *mut fs_struct) -> crate::sync::KSpinGuard {
    // SAFETY: `fs` is a live `fs_struct` (caller-supplied, matching every
    // call site's precondition below); `lock` is its first field.
    crate::sync::KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*fs).lock) }).lock()
}

#[inline(always)]
fn current_fs() -> *mut fs_struct {
    // SAFETY: `xv6_current_thread()` returns the live current thread in
    // every context this file is called from (matches the C `current`
    // macro's own assumption).
    unsafe { (*xv6_current_thread()).fs }
}

#[inline(always)]
fn current_proc_rooti() -> *mut vfs_inode {
    let fs = current_fs();
    // SAFETY: `fs` is live (see `current_fs`); `rooti` is a plain
    // embedded `vfs_inode_ref` field.
    unsafe { vfs_inode_deref(ptr::addr_of_mut!((*fs).rooti)) }
}

/// Mirrors `fs.h`'s `static inline vfs_inode_is_local_root()`. N-METH:
/// a thin null-tolerant delegate to the shared [`VfsInode::is_local_root`]
/// method (single source of truth for the set-once `sb`/`root_inode`/
/// `parent` logic); the two raw callers here keep passing `*mut`.
fn vfs_inode_is_local_root(inode: *mut vfs_inode) -> bool {
    // SAFETY: null-tolerant — `as_ref` yields `None` for a null pointer;
    // the method reads only set-once fields through the shared borrow.
    match unsafe { inode.as_ref() } {
        Some(i) => i.is_local_root(),
        None => false,
    }
}

/// Mirrors `vfs_private.h`'s `static inline __vfs_inode_valid()`.
fn vfs_inode_valid_locked(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: non-null `inode`; only reads the mutex-held flag and the
    // valid/superblock-valid bitfields.
    unsafe {
        if holding_mutex(ptr::addr_of_mut!((*inode).mutex)) == 0 {
            return neg(EPERM);
        }
        if (*inode).flags.valid() == 0 {
            return neg(EINVAL);
        }
        if !core::ptr::eq(inode, root_inode_ptr()) {
            let sb = (*inode).sb;
            if sb.is_null() || (*sb).flags.valid() == 0 {
                crate::kprintln!("__vfs_inode_valid: inode's superblock is not valid");
                return neg(EINVAL);
            }
        }
    }
    0
}

// ===========================================================================
// Inode Private APIs
// ===========================================================================

/// Initialize VFS-managed inode fields. Called by `fs.c` (root inode init,
/// `vfs_alloc_inode`/`vfs_get_inode`) before adding a freshly allocated
/// inode to the superblock's inode hash list.
pub(crate) extern "C" fn __vfs_inode_init(inode: *mut vfs_inode) {
    // SAFETY: caller supplies a freshly allocated, otherwise-untouched
    // `vfs_inode` (matches the C original's documented precondition).
    unsafe {
        mutex_init(
            ptr::addr_of_mut!((*inode).mutex),
            c"vfs_inode_mutex".as_ptr() as *mut c_char,
        );
        hli_init(ptr::addr_of_mut!((*inode).hash_entry));
        ln_init(ptr::addr_of_mut!((*inode).orphan_entry));
        (*inode).flags.set_orphan(0);
        (*inode).ref_count = 1;
    }
}

// ===========================================================================
// Inode Public APIs
// ===========================================================================

pub(crate) extern "C" fn vfs_ilock(inode: *mut vfs_inode) {
    kassert!(!inode.is_null(), "vfs_ilock: inode is NULL");
    // SAFETY: non-null `inode`.
    mutex_lock(unsafe { ptr::addr_of_mut!((*inode).mutex) });
}

pub(crate) extern "C" fn vfs_ilock_trylock(inode: *mut vfs_inode) -> c_int {
    kassert!(!inode.is_null(), "vfs_ilock_trylock: inode is NULL");
    // SAFETY: non-null `inode`.
    mutex_trylock(unsafe { ptr::addr_of_mut!((*inode).mutex) })
}

pub(crate) extern "C" fn vfs_iunlock(inode: *mut vfs_inode) {
    kassert!(!inode.is_null(), "vfs_iunlock: inode is NULL");
    // SAFETY: non-null `inode`.
    mutex_unlock(unsafe { ptr::addr_of_mut!((*inode).mutex) });
}

/// Check if an inode is valid for use. Must be called while holding the
/// inode lock.
pub(crate) extern "C" fn vfs_inode_check_valid(inode: *mut vfs_inode) -> c_int {
    vfs_inode_valid_locked(inode)
}

/// Increment inode reference count. Atomic-only: no locks, no sleeping,
/// no allocation (may be called while holding the inode lock).
pub(crate) extern "C" fn vfs_idup(inode: *mut vfs_inode) {
    kassert!(!inode.is_null(), "vfs_idup: inode is NULL");
    // SAFETY: non-null `inode`; only reads `sb` for the assert.
    let sb_null = unsafe { (*inode).sb.is_null() };
    kassert!(!sb_null, "vfs_idup: inode's superblock is NULL");
    let success = atomic_inc_unless(refcount_atomic(inode), VFS_INODE_MAX_REFCOUNT);
    kassert!(success, "vfs_idup: inode refcount overflow");
}

/// Try to increment inode reference count if not zero (and not at max).
/// Use when obtaining a reference from a cache/lookup where the inode
/// might be dying.
pub(crate) extern "C" fn vfs_idup_not_zero(inode: *mut vfs_inode) -> bool {
    kassert!(!inode.is_null(), "vfs_idup_not_zero: inode is NULL");
    atomic_inc_in_range(refcount_atomic(inode), 0, VFS_INODE_MAX_REFCOUNT)
}

/// Decrease inode ref count; free the inode when the last reference is
/// dropped. Caller must not hold the inode lock or the superblock write
/// lock when calling.
pub(crate) extern "C" fn vfs_iput(inode_in: *mut vfs_inode) {
    kassert!(!inode_in.is_null(), "vfs_iput: inode is NULL");
    // SAFETY: non-null `inode_in`; preconditions mirror the C asserts.
    unsafe {
        let sb0 = (*inode_in).sb;
        let holds_wlock = !sb0.is_null() && vfs_superblock_wholding(sb0);
        kassert!(
            !holds_wlock,
            "vfs_iput: cannot hold superblock write lock when calling"
        );
        let holds_ilock = holding_mutex(ptr::addr_of_mut!((*inode_in).mutex)) != 0;
        kassert!(!holds_ilock, "vfs_iput: cannot hold inode lock when calling");
    }

    let mut inode = inode_in;
    // SAFETY: non-null `inode`.
    let mut sb = unsafe { (*inode).sb };
    let mut failed_clean = false;

    loop {
        // retry:
        if atomic_dec_unless(refcount_atomic(inode), 1) {
            return;
        }

        if sb.is_null() {
            vfs_iput_finalize(inode, None);
            return;
        }

        vfs_superblock_wlock(sb);
        // SAFETY: non-null `inode`.
        if mutex_trylock(unsafe { ptr::addr_of_mut!((*inode).mutex) }) == 0 {
            vfs_superblock_unlock(sb);
            Scheduler::yield_now();
            continue;
        }

        if atomic_dec_unless(refcount_atomic(inode), 1) {
            // SAFETY: non-null `inode`.
            mutex_unlock(unsafe { ptr::addr_of_mut!((*inode).mutex) });
            vfs_superblock_unlock(sb);
            return;
        }

        // N-R5c: one ephemeral `&vfs_superblock` borrow covers all three
        // superblock reads (the atomic `flags` bitset + the set-once
        // `root_inode`), collapsing what were two separate `unsafe`-
        // wrapped `(*sb)...` blocks into one.
        //
        // SAFETY: `sb` is this inode's live set-once superblock (checked
        // non-null above and held via the write lock); the borrow is
        // ephemeral — it is dropped at the end of this block, well before
        // the later `(*sb).orphan_count -= 1` raw mutation — so it never
        // aliases a mutation, and it reads only the relaxed-atomic
        // `flags` (N-R5c Step A) and the set-once `root_inode`, so it is
        // free of the freeze-noalias hazard even inside this loop.
        let (attached, backendless, is_root_inode) = {
            let sbref = unsafe { &*sb };
            (
                sbref.flags.attached() != 0,
                sbref.flags.backendless() != 0,
                core::ptr::eq(inode, sbref.root_inode),
            )
        };
        let n_links = unsafe { (*inode).n_links };
        let is_mount = unsafe { (*inode).flags.mount() != 0 };

        if attached && (n_links > 0 || is_mount) && (backendless || is_root_inode || is_mount) {
            let old = refcount_atomic(inode).fetch_sub(1, Ordering::SeqCst);
            kassert!(old - 1 >= 0, "vfs_iput: inode refcount underflow");
            // SAFETY: non-null `inode`.
            mutex_unlock(unsafe { ptr::addr_of_mut!((*inode).mutex) });
            vfs_superblock_unlock(sb);
            return;
        }

        kassert!(!is_mount, "vfs_iput: refcount of mountpoint inode reached zero");

        // Orphan cleanup: remove from the in-memory orphan list and, for
        // backend filesystems, the on-disk orphan journal.
        let is_orphan = unsafe { (*inode).flags.orphan() != 0 };
        if is_orphan {
            // SAFETY: `inode` is on `sb->orphan_list` (orphan flag set);
            // `orphan_entry` is a live linked `list_node_t`.
            unsafe {
                ln_detach(ptr::addr_of_mut!((*inode).orphan_entry));
                (*sb).orphan_count -= 1;
                (*inode).flags.set_orphan(0);
                if sb_ops(sb).remove_orphan(sb, inode).is_err() {
                    crate::kprintln!(
                        "vfs_iput: warning: failed to remove orphan inode {} from journal",
                        (*inode).ino as u64,
                    );
                }
            }
        }

        // If dirty and no cleanup has failed yet, try to sync before
        // freeing; retry from the top afterwards (someone else may have
        // grabbed a reference while locks were dropped for the sync).
        let dirty = unsafe { (*inode).flags.dirty() != 0 };
        let valid = unsafe { (*inode).flags.valid() != 0 };
        if dirty && valid && !failed_clean && attached {
            // SAFETY: non-null `inode`.
            mutex_unlock(unsafe { ptr::addr_of_mut!((*inode).mutex) });
            vfs_superblock_unlock(sb);
            failed_clean = vfs_sync_inode(inode) != 0;
            continue;
        }

        let mode = unsafe { (*inode).mode };
        let parent_field = unsafe { (*inode).parent };
        let mut parent: *mut vfs_inode = ptr::null_mut();
        if is_dir(mode) && !parent_field.is_null() && !core::ptr::eq(parent_field, inode) && attached
        {
            parent = parent_field;
        }

        // If no links remain (or the fs is detached), destroy the
        // on-disk data before freeing the in-memory inode.
        let n_links2 = unsafe { (*inode).n_links };
        if n_links2 == 0 || !attached {
            // P3-10b: `destroy_inode` is a required trait method (every
            // historical table filled the slot), so the old `None`-slot
            // "skip the whole destroy dance" branch was dead code and
            // the presence gate collapses.
            {
                // Mark the inode as being destroyed so lookups don't try
                // to use it while destroy_inode is in progress; the
                // inode stays in the cache meanwhile.
                unsafe { (*inode).flags.set_destroying(1) };

                // Release locks before acquiring the transaction:
                // destroy_inode may do begin/end_op cycles internally
                // for batching and cannot run with these locks held.
                unsafe { mutex_unlock(ptr::addr_of_mut!((*inode).mutex)) };
                vfs_superblock_unlock(sb);

                let mut skip_destroy = false;
                if unsafe { sb_ops(sb).begin_transaction(sb) }.is_err() {
                    // Transaction failed — on-disk data remains,
                    // will be cleaned on next mount via orphan
                    // recovery. Still need to remove from cache.
                    unsafe { (*inode).flags.set_destroying(0) };
                    vfs_superblock_wlock(sb);
                    unsafe { mutex_lock(ptr::addr_of_mut!((*inode).mutex)) };
                    skip_destroy = true;
                }

                if !skip_destroy {
                    // SAFETY: `inode` is live, unlocked, with a
                    // transaction open — `vfs_iput`'s documented
                    // `destroy_inode` calling convention.
                    unsafe { inode_ops(inode).destroy_inode(inode) };

                    if let Err(e) = unsafe { sb_ops(sb).end_transaction(sb) } {
                        crate::kprintln!("vfs_iput: warning: end_transaction failed with error {}", e.neg());
                    }

                    vfs_superblock_wlock(sb);
                    unsafe {
                        mutex_lock(ptr::addr_of_mut!((*inode).mutex));
                        // After destroy, on-disk data is freed: mark
                        // invalid/not-dirty so we never try to sync it.
                        (*inode).flags.set_valid(0);
                        (*inode).flags.set_dirty(0);
                        (*inode).flags.set_destroying(0);
                    }
                }
            }
        }

        // skip_destroy:
        // SAFETY: non-null `inode`/`sb`, both locked here.
        let ret = unsafe { vfs_remove_inode((*inode).sb, inode) };
        kassert!(
            ret == 0,
            "vfs_iput: failed to remove inode from superblock inode cache"
        );

        let should_free_sb = !attached && unsafe { (*sb).orphan_count } == 0;

        unsafe { mutex_unlock(ptr::addr_of_mut!((*inode).mutex)) };
        vfs_superblock_unlock(sb);

        // out:
        vfs_iput_finalize(inode, if should_free_sb { Some(sb) } else { None });

        // If this was a directory inode, decrease the parent's refcount
        // too. Avoid recursion (limited kernel stack): loop back to
        // `retry:` operating on the parent instead.
        if !parent.is_null() {
            inode = parent;
            sb = unsafe { (*inode).sb };
            failed_clean = false;
            continue;
        }
        return;
    }
}

/// Shared tail of every `vfs_iput` exit path: free the directory name (if
/// any) and hand the inode to its driver's `free_inode`, then run the
/// final detached-superblock cleanup if this was the last orphan.
fn vfs_iput_finalize(inode: *mut vfs_inode, sb_to_free: Option<*mut vfs_superblock>) {
    // SAFETY: non-null `inode`, no longer locked (matches every call
    // site, which unlocks before reaching here).
    unsafe {
        if !(*inode).name.is_null() {
            kmm_free((*inode).name as *mut c_void);
            (*inode).name = ptr::null_mut();
        }
        inode_ops(inode).free_inode(inode);
    }
    if let Some(sb) = sb_to_free {
        __vfs_final_unmount_cleanup(sb);
    }
}

// ---------------------------------------------------------------------------
// `IRef` -- an RAII smart pointer over `vfs_inode.ref_count` (Phase 3
// P3-9d). Inode's refcount is a bespoke `AtomicI32` field manipulated by
// this file's own `vfs_idup`/`vfs_idup_not_zero`/`vfs_iput` (see the
// module doc's "Refcount map"), not a `struct kobject` -- it cannot use
// `KArc<T>` (`kernel/kobject.rs`). `IRef` deliberately mirrors `KArc<T>`'s
// shape (`Clone`/`Drop`/`Deref`/`from_raw`/`into_raw`/`as_ptr`) but is
// built directly on top of the get/put pair above rather than
// re-deriving the atomics -- it inherits their exact, already-proven
// ordering (`Ordering::SeqCst` CAS / `Ordering::Acquire` initial load,
// matching `smp/atomic.h`'s `atomic_oper_cond` verbatim) instead of a
// second, independently-reasoned-about copy. `vfs_iput`'s retry/destroy
// internals are untouched -- `IRef` only wraps the get/put pair at call
// sites, exactly as the KArc precedent wraps `kobject_get`/`kobject_put`
// rather than reimplementing them.
//
// # Invariant
// A live `IRef` owns exactly one held reference on the pointee
// `vfs_inode` (one increment of `ref_count` not yet matched by a
// `vfs_iput`). `Clone` (-> `vfs_idup`) and `Drop` (-> `vfs_iput`) are the
// only ways that invariant is created/destroyed internally;
// [`IRef::from_raw`]/[`IRef::try_from_raw`] accept one from the caller
// (the success postcondition of `vfs_namei`/`vfs_ilookup` +
// `vfs_get_dentry_inode`/`vfs_idup`-family constructors) and
// [`IRef::into_raw`] hands one back out -- the same `from_raw`/`into_raw`
// contract `Box`/`Arc`/`KArc` use, for the C-ABI boundary where a caller
// still juggles a raw `*mut vfs_inode` and manual `vfs_iput`.
// ---------------------------------------------------------------------------

/// See the module section doc above.
pub(crate) struct IRef {
    ptr: NonNull<vfs_inode>,
}

impl IRef {
    /// Take ownership of an existing, already-held inode reference.
    ///
    /// # Safety
    /// `ptr` must be non-null and point to a live `vfs_inode` whose
    /// `ref_count` currently carries a reference the caller is
    /// transferring to the returned `IRef` -- the caller must not also
    /// call `vfs_iput` (or drop another `IRef` over the same reference)
    /// on that same held count. This is exactly the success
    /// postcondition of `vfs_namei`/`vfs_nameiparent`/`vfs_ilookup` +
    /// `vfs_get_dentry_inode`/`vfs_idup`.
    pub(crate) unsafe fn from_raw(ptr: *mut vfs_inode) -> Self {
        debug_assert!(!ptr.is_null(), "IRef::from_raw: ptr is NULL");
        // SAFETY: caller guarantees `ptr` is non-null (also debug-checked
        // above) and owns an already-held reference (function contract).
        IRef { ptr: unsafe { NonNull::new_unchecked(ptr) } }
    }

    /// `vfs_idup_not_zero` analog: attempt to upgrade a bare,
    /// liveness-unproven `*mut vfs_inode` (e.g. one just read out of a
    /// cache/hash bucket under a lock, with no existing reference of its
    /// own) into an owned `IRef`. Returns `None` if the refcount had
    /// already reached zero (a concurrent final `Drop`/`vfs_iput` raced
    /// ahead of this call) or is already at `VFS_INODE_MAX_REFCOUNT`.
    ///
    /// # Safety
    /// `ptr` must be non-null and point to a `vfs_inode` that is still
    /// valid to dereference for the duration of this call -- the same
    /// precondition a raw call to `vfs_idup_not_zero` has.
    pub(crate) unsafe fn try_from_raw(ptr: *mut vfs_inode) -> Option<Self> {
        debug_assert!(!ptr.is_null(), "IRef::try_from_raw: ptr is NULL");
        if vfs_idup_not_zero(ptr) {
            // SAFETY: `ptr` was just proven non-null-derived and live by
            // the successful `vfs_idup_not_zero` above, which also
            // established the held reference this `IRef` now owns.
            Some(IRef { ptr: unsafe { NonNull::new_unchecked(ptr) } })
        } else {
            None
        }
    }

    /// Give up the owned reference without decrementing the refcount,
    /// returning the raw pointer -- the flip side of [`IRef::from_raw`],
    /// for a C-ABI boundary that expects to receive one held reference as
    /// a bare pointer (e.g. storing into `fs_struct.cwd`/`.rooti` via
    /// `vfs_inode_get_ref`, or a still-C-shaped vtable slot/out-param).
    pub(crate) fn into_raw(this: Self) -> *mut vfs_inode {
        let ptr = this.ptr.as_ptr();
        core::mem::forget(this);
        ptr
    }

    /// The raw pointer this `IRef` owns a reference on, without giving up
    /// ownership (contrast [`IRef::into_raw`]) -- for passing to the many
    /// still-C-ABI-shaped helpers in this module that borrow a
    /// `*mut vfs_inode` without taking or dropping a reference of their
    /// own (`vfs_ilock`, vtable dispatch functions, `vfs_readlink`, ...).
    pub(crate) fn as_ptr(this: &Self) -> *mut vfs_inode {
        this.ptr.as_ptr()
    }
}

impl Clone for IRef {
    fn clone(&self) -> Self {
        // Struct invariant: `self` owns a currently-held reference, so
        // `vfs_idup`'s precondition (an already-held reference to
        // increment from) holds; matches `KArc::clone`'s use of
        // `kobject_get` under the same reasoning.
        vfs_idup(self.ptr.as_ptr());
        IRef { ptr: self.ptr }
    }
}

impl Drop for IRef {
    fn drop(&mut self) {
        // Struct invariant: `self` owns exactly one held reference, which
        // this call gives up; `vfs_iput` runs its own retry/destroy path
        // if this was the last one (untouched by this wrapper -- see the
        // module section doc above). Every current `IRef` call site
        // upholds `vfs_iput`'s precondition (neither the inode lock nor
        // the superblock write lock held here): none call `into_raw`
        // while holding either, and none hold an `IRef` across a
        // `vfs_ilock`/`vfs_superblock_wlock` acquisition.
        vfs_iput(self.ptr.as_ptr());
    }
}

impl Deref for IRef {
    type Target = vfs_inode;
    fn deref(&self) -> &vfs_inode {
        // SAFETY: struct invariant -- a live `IRef` holds a reference,
        // which keeps the pointee allocated for as long as any `IRef`
        // over it exists (mirrors `KArc<T>::deref`'s reasoning). Shared
        // access only: mutation of individual `vfs_inode` fields
        // elsewhere in this crate is synchronized per-field by
        // `vfs_ilock`/atomics, not by unique ownership of the whole
        // struct -- the same discipline every existing `*mut vfs_inode`
        // call site already relies on.
        unsafe { self.ptr.as_ref() }
    }
}

/// Mark inode as dirty.
fn vfs_dirty_inode_inner(inode: *mut vfs_inode) -> KResult<()> {
    if inode.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `inode` is non-null (just checked); a live, referenced inode
    // the caller holds locked. P3-7c boundary hoist — the `sb` read goes
    // safe; validate/dispatch reborrow `&raw mut *inode`.
    let inode = unsafe { &mut *inode };
    if inode.sb.is_null() {
        return Err(Errno::Inval);
    }
    let ret = vfs_inode_valid_locked(&raw mut *inode);
    if ret != 0 {
        return Err(Errno::Raw(ret));
    }
    // SAFETY: non-null, valid, locked `inode` (checked above) — the
    // trait method's contract; a driver without its own dirty tracking
    // inherits the `Ok(())` default (old `None`-slot behavior).
    unsafe { inode_ops(&raw mut *inode).dirty_inode(&raw mut *inode) }
}

pub(crate) extern "C" fn vfs_dirty_inode(inode: *mut vfs_inode) -> c_int {
    result_to_neg_errno(vfs_dirty_inode_inner(inode))
}

/// Sync inode to disk.
fn vfs_sync_inode_inner(inode: *mut vfs_inode) -> KResult<()> {
    if inode.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `inode` is non-null (just checked); a live, referenced inode
    // the caller holds open. P3-7c boundary hoist — the `sb` read goes
    // safe; lock/validate/dispatch reborrow `&raw mut *inode`.
    let inode = unsafe { &mut *inode };
    if inode.sb.is_null() {
        return Err(Errno::Inval);
    }
    let sb = inode.sb;

    // Begin transaction BEFORE acquiring the inode lock, to avoid
    // sleeping with locks held.
    unsafe { sb_ops(sb).begin_transaction(sb) }?;

    vfs_ilock(&raw mut *inode);
    let v = vfs_inode_valid_locked(&raw mut *inode);
    if v != 0 {
        vfs_iunlock(&raw mut *inode);
        let _ = unsafe { sb_ops(sb).end_transaction(sb) };
        return Err(Errno::Raw(v));
    }

    // SAFETY: non-null, valid, locked `inode` with a transaction open —
    // the trait method's contract; a backendless driver inherits the
    // `Ok(())` default (old `None`-slot skip).
    let ret = unsafe { inode_ops(&raw mut *inode).sync_inode(&raw mut *inode) };
    vfs_iunlock(&raw mut *inode);

    if let Err(e) = unsafe { sb_ops(sb).end_transaction(sb) } {
        crate::kprintln!("vfs_sync_inode: warning: end_transaction failed with error {}", e.neg());
    }
    ret
}

pub(crate) extern "C" fn vfs_sync_inode(inode: *mut vfs_inode) -> c_int {
    result_to_neg_errno(vfs_sync_inode_inner(inode))
}

/// Get the outermost mount layer. Caller must hold a reference to
/// `rooti` or one of its descendants.
fn get_mnt_recursive(rooti: *mut vfs_inode) -> *mut vfs_inode {
    let mut inode = rooti;
    // SAFETY: `rooti` is a live, referenced inode (caller precondition).
    let mut sb = unsafe { (*rooti).sb };
    let proc_rooti = current_proc_rooti();
    loop {
        if core::ptr::eq(inode, proc_rooti) {
            return inode;
        }
        if core::ptr::eq(inode, root_inode_ptr()) {
            return inode;
        }
        kassert!(!sb.is_null(), "__get_mnt_recursive: inode's superblock mismatch");
        // SAFETY: non-null `sb`.
        let sb_root = unsafe { (*sb).root_inode };
        if !core::ptr::eq(inode, sb_root) {
            kassert!(!sb_root.is_null(), "__get_mnt_recursive: superblock root inode is NULL");
            return inode;
        }
        // SAFETY: non-null `sb`.
        inode = unsafe { (*sb).mountpoint };
        kassert!(!inode.is_null(), "__get_mnt_recursive: mountpoint inode is NULL");
        // SAFETY: non-null `inode`.
        sb = unsafe { (*inode).sb };
    }
}

/// Get the parent inode of the mountpoint recursively. Caller must hold
/// a reference to `dir` or one of its descendants.
fn mountpoint_go_up(dir: *mut vfs_inode) -> *mut vfs_inode {
    let mut inode = dir;
    let proc_rooti = current_proc_rooti();
    loop {
        if core::ptr::eq(inode, proc_rooti) {
            return inode;
        }
        if core::ptr::eq(inode, root_inode_ptr()) {
            return inode;
        }
        // SAFETY: non-null `inode`.
        let parent = unsafe { (*inode).parent };
        if !core::ptr::eq(parent, inode) {
            kassert!(!parent.is_null(), "__mountpoint_go_up: inode's parent is NULL");
            return parent;
        }
        inode = get_mnt_recursive(inode);
    }
}

/// Resolve ".." for a directory inode. Returns `Some(target)` for the
/// synthesized-".." fast paths (process root, local fs root); `None`
/// means "fall through to driver lookup for a normal ..".
fn vfs_dotdot_target(dir: *mut vfs_inode) -> *mut vfs_inode {
    let proc_rooti = current_proc_rooti();
    if core::ptr::eq(dir, proc_rooti) {
        return dir;
    }
    if vfs_inode_is_local_root(dir) {
        let parent = mountpoint_go_up(dir);
        if core::ptr::eq(parent, root_inode_ptr()) {
            return dir;
        }
        return parent;
    }
    ptr::null_mut()
}

fn make_iter_present(iter: *mut vfs_dir_iter, ret_dentry: *mut vfs_dentry) -> c_int {
    let n = strndup(c".".as_ptr(), 1);
    if n.is_null() {
        return neg(ENOMEM);
    }
    // SAFETY: non-null `iter`/`ret_dentry` (caller precondition).
    unsafe {
        (*ret_dentry).name = n;
        (*ret_dentry).name_len = 1;
        (*ret_dentry).cookies = 0;
        (*iter).cookies = 0;
        (*iter).index = VFS_DITER_INDEX_CURRENT;
    }
    0
}

fn make_iter_parent(iter: *mut vfs_dir_iter, ret_dentry: *mut vfs_dentry) -> c_int {
    vfs_release_dentry(ret_dentry); // release "."
    let n = strndup(c"..".as_ptr(), 2);
    if n.is_null() {
        return neg(ENOMEM);
    }
    // SAFETY: non-null `iter`/`ret_dentry` (caller precondition).
    unsafe {
        (*ret_dentry).name = n;
        (*ret_dentry).name_len = 2;
        (*ret_dentry).cookies = 0;
        (*iter).cookies = 0;
        (*iter).index = VFS_DITER_INDEX_PARENT;
    }
    0
}

/// Lookup a dentry in a directory inode. Assumes the VFS core already
/// handled ".".
fn vfs_ilookup_inner(
    dir: *mut vfs_inode,
    dentry: *mut vfs_dentry,
    name: *const c_char,
    name_len: usize,
) -> KResult<()> {
    if dir.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `dir` is non-null (just checked); a live, referenced
    // directory inode the caller holds open. P3-7c boundary hoist — plain
    // reads (`sb`, `ino`, `mode`) go safe; `dentry` writes, the `..`
    // target, and lock/validate/dispatch keep raw (reborrow `&raw mut
    // *dir` / `&raw const *dir`).
    let dir = unsafe { &mut *dir };
    if dir.sb.is_null() {
        return Err(Errno::Inval);
    }
    if dentry.is_null() || name.is_null() || name_len == 0 {
        return Err(Errno::Inval);
    }
    // SAFETY: `name`/`name_len` describe a valid byte range (caller
    // precondition — the VFS convention for all name/name_len pairs).
    let name_bytes = unsafe { core::slice::from_raw_parts(name as *const u8, name_len) };

    if name_len == 1 && name_bytes[0] == b'.' {
        // SAFETY: non-null `dentry`; `dir` is the boundary reference.
        unsafe {
            (*dentry).sb = dir.sb;
            (*dentry).ino = dir.ino;
            (*dentry).parent = &raw mut *dir;
        }
        let n = strndup(c".".as_ptr(), 1);
        if n.is_null() {
            return Err(Errno::NoMem);
        }
        unsafe {
            (*dentry).name = n;
            (*dentry).name_len = 1;
            (*dentry).cookies = 0;
        }
        return Ok(());
    }

    if name_len == 2 && name_bytes[0] == b'.' && name_bytes[1] == b'.' {
        let target = vfs_dotdot_target(&raw mut *dir);
        if !target.is_null() {
            let n = strndup(c"..".as_ptr(), 2);
            if n.is_null() {
                return Err(Errno::NoMem);
            }
            // SAFETY: non-null `target`/`dentry`; `target` is a distinct
            // inode (still raw); `dir` is the boundary reference.
            unsafe {
                (*dentry).sb = (*target).sb;
                (*dentry).ino = (*target).ino;
                (*dentry).parent = if core::ptr::eq(target, &raw const *dir) { ptr::null_mut() } else { target };
                (*dentry).name = n;
                (*dentry).name_len = 2;
                (*dentry).cookies = 0;
            }
            return Ok(());
        }
        // fall through to driver lookup for normal ".."
    }

    let sb = dir.sb;
    vfs_superblock_rlock(sb);
    vfs_ilock(&raw mut *dir);

    let ret: KResult<()> = 'out: {
        let v = vfs_inode_valid_locked(&raw mut *dir);
        if v != 0 {
            break 'out Err(Errno::Raw(v));
        }
        if !is_dir(dir.mode) {
            break 'out Err(Errno::NotDir);
        }
        // SAFETY: `dir` is live, valid, locked, superblock read-locked
        // — the trait method's contract (reborrowed as the raw pointer).
        unsafe { inode_ops(&raw mut *dir).lookup(&raw mut *dir, dentry, name, name_len) }
    };

    vfs_iunlock(&raw mut *dir);
    vfs_superblock_unlock(sb);
    ret
}

/// Lookup a dentry in a directory inode. Assumes the VFS core already
/// handled ".".
pub(crate) extern "C" fn vfs_ilookup(
    dir: *mut vfs_inode,
    dentry: *mut vfs_dentry,
    name: *const c_char,
    name_len: usize,
) -> c_int {
    result_to_neg_errno(vfs_ilookup_inner(dir, dentry, name, name_len))
}

/// Iterate over directory entries in a directory inode.
pub(crate) extern "C" fn vfs_dir_iter(
    dir: *mut vfs_inode,
    iter: *mut vfs_dir_iter,
    ret_dentry: *mut vfs_dentry,
) -> c_int {
    if dir.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: `dir` is non-null (just checked); a live, referenced
    // directory inode the caller holds open. P3-7c boundary hoist — plain
    // reads (`sb`, `ino`, `mode`) go safe; `iter`/`ret_dentry` writes, the
    // `..` target, and lock/validate/dispatch keep raw (reborrow).
    let dir = unsafe { &mut *dir };
    if dir.sb.is_null() {
        return neg(EINVAL);
    }
    if iter.is_null() || ret_dentry.is_null() {
        return neg(EINVAL);
    }

    let sb = dir.sb;
    vfs_superblock_rlock(sb);
    vfs_ilock(&raw mut *dir);

    let mut need_lookup = false;
    let ret: c_int = 'body: {
        let v = vfs_inode_valid_locked(&raw mut *dir);
        if v != 0 {
            break 'body v;
        }
        if !is_dir(dir.mode) {
            break 'body neg(ENOTDIR);
        }
        // P3-10b: `dir_iter` is a required trait method (every
        // historical table filled the slot), so the old pre-flight
        // `None` -> `-ENOSYS` check was dead code and collapses.
        // SAFETY: non-null `iter`.
        let index = unsafe { (*iter).index };
        if index == VFS_DITER_INDEX_END {
            unsafe {
                (*ret_dentry).name = ptr::null_mut();
                (*ret_dentry).name_len = 0;
            }
            break 'body 0;
        }

        if index == VFS_DITER_INDEX_START {
            let r = make_iter_present(iter, ret_dentry);
            if r != 0 {
                break 'body r;
            }
            unsafe {
                (*ret_dentry).ino = dir.ino;
                (*ret_dentry).sb = dir.sb;
                (*ret_dentry).parent = ptr::null_mut();
            }
            break 'body 0;
        }

        if index == 1 {
            let proc_rooti = current_proc_rooti();
            if core::ptr::eq(&raw const *dir, proc_rooti) {
                let r = make_iter_parent(iter, ret_dentry);
                if r != 0 {
                    break 'body r;
                }
                unsafe {
                    (*ret_dentry).ino = dir.ino;
                    (*ret_dentry).sb = dir.sb;
                    (*ret_dentry).parent = ptr::null_mut();
                }
                break 'body 0;
            } else if vfs_inode_is_local_root(&raw mut *dir) {
                let r = make_iter_parent(iter, ret_dentry);
                if r != 0 {
                    break 'body r;
                }
                unsafe { (*ret_dentry).parent = ptr::null_mut() };
                need_lookup = true;
                break 'body 0;
            }
            // Ordinary directory: fall through to the driver call
            // without modifying iter->index.
        }

        if unsafe { (*iter).index } >= VFS_DITER_INDEX_PARENT {
            unsafe {
                (*iter).index += 1;
                (*ret_dentry).sb = dir.sb;
                (*ret_dentry).parent = &raw mut *dir;
                (*ret_dentry).cookies = (*iter).cookies;
            }
        }
        // SAFETY: `dir` is live, valid, locked, superblock read-locked
        // — the trait method's contract (reborrowed as the raw pointer).
        let r = match unsafe { inode_ops(&raw mut *dir).dir_iter(&raw mut *dir, iter, ret_dentry) } {
            Ok(()) => 0,
            Err(e) => e.neg(),
        };
        if r == 0 && unsafe { (*iter).index } == VFS_DITER_INDEX_CURRENT {
            unsafe { (*iter).index = VFS_DITER_INDEX_PARENT };
        }
        r
    };

    vfs_iunlock(&raw mut *dir);
    vfs_superblock_unlock(sb);

    if ret == 0 {
        if unsafe { (*iter).index } == VFS_DITER_INDEX_PARENT && need_lookup {
            // Synthesizing ".." for a mounted root: fill in the correct
            // parent inode now that locks are released.
            let target = vfs_dotdot_target(&raw mut *dir);
            unsafe {
                (*ret_dentry).ino = (*target).ino;
                (*ret_dentry).sb = (*target).sb;
                (*ret_dentry).parent = target;
            }
        }
        if unsafe { (*iter).index } > VFS_DITER_INDEX_PARENT {
            let name_present = unsafe { !(*ret_dentry).name.is_null() && (*ret_dentry).name_len > 0 };
            if name_present {
                if unsafe { (*ret_dentry).ino } == 0 {
                    return neg(EINVAL); // Driver did not fill ino
                }
                unsafe { (*iter).cookies = (*ret_dentry).cookies };
                return 0;
            }
            // Reached end of directory; reset the iterator.
            unsafe {
                (*iter).index = VFS_DITER_INDEX_END;
                (*iter).cookies = 0;
                (*ret_dentry).parent = ptr::null_mut();
                (*ret_dentry).cookies = 0;
                (*ret_dentry).ino = 0;
                (*ret_dentry).sb = ptr::null_mut();
            }
            return 0;
        }
    }

    ret
}

/// Check if a directory is empty (contains only "." and ".."). Caller
/// must hold the inode lock.
pub(crate) extern "C" fn vfs_dir_isempty(dir: *mut vfs_inode) -> c_int {
    // SAFETY: `dir` is live and locked (caller's contract). P3-7c boundary
    // hoist — the `mode` read goes safe; the trait dispatch reborrows
    // `&raw mut *dir`.
    let dir = unsafe { &mut *dir };
    if !is_dir(dir.mode) {
        return 0;
    }
    let mut iter: vfs_dir_iter = unsafe { core::mem::zeroed() };
    let mut dentry: vfs_dentry = unsafe { core::mem::zeroed() };
    iter.index = VFS_DITER_INDEX_PARENT; // Skip "." and ".."
    iter.cookies = 0;

    // P3-10b: required trait method — the old `None`-slot "can't check,
    // assume not empty" branch was dead code and collapses.
    let ret = unsafe { inode_ops(&raw mut *dir).dir_iter(&raw mut *dir, &mut iter, &mut dentry) };
    if ret.is_err() {
        vfs_release_dentry(&mut dentry);
        return 0; // Error, assume not empty
    }
    let is_empty = dentry.name.is_null();
    vfs_release_dentry(&mut dentry);
    is_empty as c_int
}

fn vfs_readlink_inner(inode: *mut vfs_inode, buf: *mut c_char, buflen: usize) -> KResult<isize> {
    if inode.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `inode` is non-null (just checked); a live, referenced inode
    // the caller holds open. P3-7c boundary hoist — plain reads (`sb`,
    // `mode`) go safe; lock/validate/dispatch reborrow `&raw mut *inode`.
    let inode = unsafe { &mut *inode };
    if inode.sb.is_null() {
        return Err(Errno::Inval);
    }
    if buf.is_null() || buflen == 0 {
        return Err(Errno::Inval);
    }
    vfs_ilock(&raw mut *inode);
    let ret: KResult<isize> = 'out: {
        let v = vfs_inode_valid_locked(&raw mut *inode);
        if v != 0 {
            break 'out Err(Errno::Raw(v));
        }
        if !is_lnk(inode.mode) {
            break 'out Err(Errno::Inval);
        }
        // SAFETY: `inode` is live, valid, locked — the trait method's
        // contract (reborrowed as the raw pointer it still takes).
        let r = match unsafe { inode_ops(&raw mut *inode).readlink(&raw mut *inode, buf, buflen) } {
            Ok(r) => r,
            Err(e) => break 'out Err(e),
        };
        if r >= 0 && (r as usize) >= buflen {
            break 'out Err(Errno::NameTooLong);
        }
        Ok(r)
    };
    vfs_iunlock(&raw mut *inode);
    ret
}

pub(crate) extern "C" fn vfs_readlink(inode: *mut vfs_inode, buf: *mut c_char, buflen: usize) -> isize {
    match vfs_readlink_inner(inode, buf, buflen) {
        Ok(r) => r,
        Err(e) => e.neg() as isize,
    }
}

fn vfs_create_inner(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
) -> KResult<*mut vfs_inode> {
    if dir.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `dir` is non-null (just checked); the caller passes a live,
    // referenced directory inode it holds open, valid for this call.
    // P3-7c boundary hoist — plain reads (`sb`, `mode`) go safe; the
    // lock/validate/dispatch helpers reborrow `&raw mut *dir`.
    let dir = unsafe { &mut *dir };
    if dir.sb.is_null() {
        return Err(Errno::Inval);
    }
    if name.is_null() || name_len == 0 {
        return Err(Errno::Inval);
    }

    loop {
        let sb = dir.sb;
        unsafe { sb_ops(sb).begin_transaction(sb) }?;

        vfs_superblock_wlock(sb);
        vfs_ilock(&raw mut *dir);

        // P3-10b: `KResult` end to end — the transitional internal
        // `ERR_PTR` domain (and its `is_eagain_ptr` retry check) is
        // gone; `is_again(&ret)` drives the identical retry.
        let ret: KResult<*mut vfs_inode> = {
            let v = vfs_inode_valid_locked(&raw mut *dir);
            if v != 0 {
                Err(Errno::Raw(v))
            } else if !is_dir(dir.mode) {
                Err(Errno::NotDir)
            } else {
                // SAFETY: `dir` is live, valid, locked, superblock
                // write-locked, transaction open — the trait method's
                // contract (reborrowed as the raw pointer it still takes).
                unsafe { inode_ops(&raw mut *dir).create(&raw mut *dir, mode, name, name_len) }
            }
        };

        // Handle EAGAIN: inode allocation collided with a destroying
        // inode. Release all locks and the transaction, yield, retry.
        if is_again(&ret) {
            vfs_iunlock(&raw mut *dir);
            vfs_superblock_unlock(sb);
            let _ = unsafe { sb_ops(sb).end_transaction(sb) };
            Scheduler::yield_now();
            continue;
        }

        vfs_iunlock(&raw mut *dir);
        vfs_superblock_unlock(sb);

        if let Err(e) = unsafe { sb_ops(sb).end_transaction(sb) } {
            crate::kprintln!("vfs_create: warning: end_transaction failed with error {}", e.neg());
        }
        return ret;
    }
}

pub(crate) extern "C" fn vfs_create(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
) -> *mut vfs_inode {
    result_to_errptr(vfs_create_inner(dir, mode, name, name_len))
}

pub(crate) fn vfs_mknod_inner(
    dir: *mut vfs_inode,
    mode: mode_t,
    dev: dev_t,
    name: *const c_char,
    name_len: usize,
) -> KResult<*mut vfs_inode> {
    if dir.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `dir` is non-null (just checked); a live, referenced
    // directory inode the caller holds open. P3-7c boundary hoist — see
    // `vfs_create_inner`.
    let dir = unsafe { &mut *dir };
    if dir.sb.is_null() {
        return Err(Errno::Inval);
    }
    if name.is_null() || name_len == 0 {
        return Err(Errno::Inval);
    }

    loop {
        let sb = dir.sb;
        unsafe { sb_ops(sb).begin_transaction(sb) }?;

        vfs_superblock_wlock(sb);
        vfs_ilock(&raw mut *dir);

        let ret: KResult<*mut vfs_inode> = {
            let v = vfs_inode_valid_locked(&raw mut *dir);
            if v != 0 {
                Err(Errno::Raw(v))
            } else if !is_dir(dir.mode) {
                Err(Errno::NotDir)
            } else {
                // SAFETY: as in `vfs_create_inner`.
                unsafe { inode_ops(&raw mut *dir).mknod(&raw mut *dir, mode, dev, name, name_len) }
            }
        };

        if is_again(&ret) {
            vfs_iunlock(&raw mut *dir);
            vfs_superblock_unlock(sb);
            let _ = unsafe { sb_ops(sb).end_transaction(sb) };
            Scheduler::yield_now();
            continue;
        }

        vfs_iunlock(&raw mut *dir);
        vfs_superblock_unlock(sb);

        if let Err(e) = unsafe { sb_ops(sb).end_transaction(sb) } {
            crate::kprintln!("vfs_mknod: warning: end_transaction failed with error {}", e.neg());
        }
        return ret;
    }
}

pub(crate) extern "C" fn vfs_mknod(
    dir: *mut vfs_inode,
    mode: mode_t,
    dev: dev_t,
    name: *const c_char,
    name_len: usize,
) -> *mut vfs_inode {
    result_to_errptr(vfs_mknod_inner(dir, mode, dev, name, name_len))
}

fn vfs_link_inner(
    old: *mut vfs_dentry,
    dir: *mut vfs_inode,
    name: *const c_char,
    name_len: usize,
) -> KResult<()> {
    if dir.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `dir` is non-null (just checked); a live, referenced
    // directory inode the caller holds open. P3-7c boundary hoist — plain
    // reads (`sb`, `mode`) go safe; `target` stays an `IRef` (free path),
    // and lock/validate/dispatch reborrow `&raw mut *dir`.
    let dir = unsafe { &mut *dir };
    if dir.sb.is_null() {
        return Err(Errno::Inval);
    }
    if name.is_null() || name_len == 0 || old.is_null() {
        return Err(Errno::Inval);
    }

    // P3-10b: `KResult`-native lookup (no ERR_PTR decode).
    let target = vfs_get_dentry_inode_inner(old)?;
    kassert!(!target.is_null(), "vfs_link: old dentry inode is NULL");
    // `IRef::from_raw`: `vfs_get_dentry_inode` succeeded (non-null,
    // non-error), whose postcondition is an owned, already-held
    // reference -- `target`'s `Drop` now replaces every manual
    // `vfs_iput(target)` below, including the two error-path calls this
    // wave deletes (P3-9d).
    // SAFETY: see the comment above.
    let target = unsafe { IRef::from_raw(target) };
    if target.sb != dir.sb {
        return Err(Errno::XDev); // Cross-device hard link not supported
    }

    let sb = dir.sb;
    unsafe { sb_ops(sb).begin_transaction(sb) }?;

    vfs_superblock_wlock(sb);
    let ret: c_int = 'out_unlock_sb: {
        if is_dir(target.mode) {
            break 'out_unlock_sb neg(EPERM); // Cannot create hard link to a directory
        }
        if !is_dir(dir.mode) {
            break 'out_unlock_sb neg(ENOTDIR);
        }
        vfs_ilock_two_nondirectories(&raw mut *dir, IRef::as_ptr(&target));
        let r = 'out: {
            let v = vfs_inode_valid_locked(&raw mut *dir);
            if v != 0 {
                break 'out v;
            }
            let v = vfs_inode_valid_locked(IRef::as_ptr(&target));
            if v != 0 {
                break 'out v;
            }
            // SAFETY: `target`/`dir` are live, valid, locked, the
            // superblock write-locked, transaction open — the trait
            // method's contract.
            match unsafe { inode_ops(&raw mut *dir).link(IRef::as_ptr(&target), &raw mut *dir, name, name_len) } {
                Ok(()) => 0,
                Err(e) => e.neg(),
            }
        };
        vfs_iunlock_two(IRef::as_ptr(&target), &raw mut *dir);
        r
    };
    vfs_superblock_unlock(sb);

    if let Err(e) = unsafe { sb_ops(sb).end_transaction(sb) } {
        crate::kprintln!("vfs_link: warning: end_transaction failed with error {}", e.neg());
    }

    // `target` (an `IRef`) drops here, calling `vfs_iput` exactly once --
    // matches the deleted final `vfs_iput(target)`; unlike the original,
    // every early `return` above also runs this same drop (no separate
    // manual call to remember).
    if ret == 0 { Ok(()) } else { Err(Errno::Raw(ret)) }
}

pub(crate) extern "C" fn vfs_link(
    old: *mut vfs_dentry,
    dir: *mut vfs_inode,
    name: *const c_char,
    name_len: usize,
) -> c_int {
    result_to_neg_errno(vfs_link_inner(old, dir, name, name_len))
}

fn vfs_unlink_inner(dir: *mut vfs_inode, name: *const c_char, name_len: usize) -> KResult<()> {
    if dir.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `dir` is non-null (just checked); a live, referenced
    // directory inode the caller holds open. P3-7c boundary hoist — `dir`
    // plain reads (`sb`) go safe; `target` (from lookup) stays raw — it is
    // `vfs_iput`-ed here (free path), which a `&mut` could not express.
    let dir = unsafe { &mut *dir };
    if dir.sb.is_null() {
        return Err(Errno::Inval);
    }
    if name.is_null() || name_len == 0 {
        return Err(Errno::Inval);
    }

    // SAFETY: `name`/`name_len` describe a valid byte range.
    let name_bytes = unsafe { core::slice::from_raw_parts(name as *const u8, name_len) };
    if (name_len == 1 && name_bytes == b".") || (name_len == 2 && name_bytes == b"..") {
        return Err(Errno::Inval); // Cannot unlink "." or ".."
    }

    let mut dentry: vfs_dentry = unsafe { core::mem::zeroed() };
    let lret = vfs_ilookup(&raw mut *dir, &mut dentry, name, name_len);
    if lret != 0 {
        return Err(Errno::Raw(lret));
    }

    // P3-10b: `KResult`-native lookup (no ERR_PTR decode).
    let target = match vfs_get_dentry_inode_inner(&mut dentry) {
        Ok(t) => t,
        Err(e) => {
            vfs_release_dentry(&mut dentry);
            return Err(e);
        }
    };

    let sb = dir.sb;
    if let Err(e) = unsafe { sb_ops(sb).begin_transaction(sb) } {
        vfs_iput(target); // Drop our reference from lookup
        vfs_release_dentry(&mut dentry);
        return Err(e);
    }

    vfs_superblock_wlock(sb);
    vfs_ilock(&raw mut *dir);
    vfs_ilock(target);

    let ret: c_int = 'out: {
        let v = vfs_inode_valid_locked(&raw mut *dir);
        if v != 0 {
            break 'out v;
        }
        let v = vfs_inode_valid_locked(target);
        if v != 0 {
            break 'out v;
        }
        if is_dir(unsafe { (*target).mode }) {
            // P3-10b: `rmdir` is a required trait method — the old
            // pre-flight `None` -> `-ENOSYS` check was dead code.
            if vfs_dir_isempty(target) == 0 {
                break 'out neg(ENOTEMPTY);
            }
            // Directory in use (refcount > 1 means someone else has it
            // open) — uses the atomic refcount helper, matching the C
            // original's one call site that does so (see the plain read
            // just below for the contrasting orphan-marking check).
            if vfs_inode_refcount_locked(target) > 1 {
                break 'out neg(EBUSY);
            }
            // SAFETY: `dentry` names `target`; `dir`/`target` locked,
            // superblock write-locked, transaction open — the trait
            // method's contract.
            match unsafe { inode_ops(&raw mut *dir).rmdir(&mut dentry, target) } {
                Ok(()) => 0,
                Err(e) => e.neg(),
            }
        } else {
            // SAFETY: same contract as the `rmdir` arm above.
            match unsafe { inode_ops(&raw mut *dir).unlink(&mut dentry, target) } {
                Ok(()) => 0,
                Err(e) => e.neg(),
            }
        }
    };

    // If unlink succeeded and the inode still has references beyond
    // ours, mark it as orphan (checked while still holding the locks).
    // Preserved 1:1 from the C original: a genuine *plain* (non-atomic)
    // `ref_count` read here, unlike the atomic helper used for the
    // `EBUSY` check above — both locks are held at this point so no
    // concurrent atomic writer can race this specific read/decision.
    let n_links = unsafe { (*target).n_links };
    let refcount = unsafe { (*target).ref_count };
    let is_orphan = unsafe { (*target).flags.orphan() != 0 };
    if n_links == 0 && refcount > 1 && !is_orphan {
        vfs_make_orphan(target);
    }

    vfs_iunlock(target);
    vfs_iunlock(&raw mut *dir);
    vfs_superblock_unlock(sb);

    if let Err(e) = unsafe { sb_ops(sb).end_transaction(sb) } {
        crate::kprintln!("vfs_unlink: warning: end_transaction failed with error {}", e.neg());
    }
    vfs_iput(target); // Drop our reference from lookup
    vfs_release_dentry(&mut dentry);
    if ret == 0 { Ok(()) } else { Err(Errno::Raw(ret)) }
}

pub(crate) extern "C" fn vfs_unlink(dir: *mut vfs_inode, name: *const c_char, name_len: usize) -> c_int {
    result_to_neg_errno(vfs_unlink_inner(dir, name, name_len))
}

pub(crate) fn vfs_mkdir_inner(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
) -> KResult<*mut vfs_inode> {
    if dir.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `dir` is non-null (just checked); a live, referenced
    // directory inode the caller holds open. P3-7c boundary hoist — see
    // `vfs_create_inner`.
    let dir = unsafe { &mut *dir };
    if dir.sb.is_null() {
        return Err(Errno::Inval);
    }
    if name.is_null() || name_len == 0 {
        return Err(Errno::Inval);
    }

    loop {
        let sb = dir.sb;
        unsafe { sb_ops(sb).begin_transaction(sb) }?;

        vfs_superblock_wlock(sb);
        vfs_ilock(&raw mut *dir);

        let ret: KResult<*mut vfs_inode> = {
            let v = vfs_inode_valid_locked(&raw mut *dir);
            if v != 0 {
                Err(Errno::Raw(v))
            } else if !is_dir(dir.mode) {
                Err(Errno::NotDir)
            } else {
                // SAFETY: as in `vfs_create_inner`.
                unsafe { inode_ops(&raw mut *dir).mkdir(&raw mut *dir, mode, name, name_len) }
            }
        };

        if is_again(&ret) {
            vfs_iunlock(&raw mut *dir);
            vfs_superblock_unlock(sb);
            let _ = unsafe { sb_ops(sb).end_transaction(sb) };
            Scheduler::yield_now();
            continue;
        }

        if let Ok(new_inode) = ret {
            vfs_ilock(new_inode);
            // SAFETY: `new_inode` is the freshly created child (non-null
            // on `Ok`), locked here; `&raw mut *dir` reborrows the parent
            // reference as the raw pointer `parent` stores.
            unsafe { (*new_inode).parent = &raw mut *dir };
            vfs_idup(&raw mut *dir); // increase parent dir refcount
            vfs_iunlock(new_inode);
        }

        vfs_iunlock(&raw mut *dir);
        vfs_superblock_unlock(sb);

        if let Err(e) = unsafe { sb_ops(sb).end_transaction(sb) } {
            crate::kprintln!("vfs_mkdir: warning: end_transaction failed with error {}", e.neg());
        }
        return ret;
    }
}

pub(crate) extern "C" fn vfs_mkdir(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
) -> *mut vfs_inode {
    result_to_errptr(vfs_mkdir_inner(dir, mode, name, name_len))
}

fn vfs_move_inner(
    old_dir: *mut vfs_inode,
    old_dentry: *mut vfs_dentry,
    new_dir: *mut vfs_inode,
    name: *const c_char,
    name_len: usize,
) -> KResult<()> {
    if old_dir.is_null() || new_dir.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `old_dir`/`new_dir` are non-null (just checked); live,
    // referenced directory inodes the caller holds open. P3-7c boundary
    // hoist — plain reads (`sb`, `mode`) go safe; validate/lock/dispatch
    // reborrow `&raw mut *old_dir`/`&raw mut *new_dir`.
    let old_dir = unsafe { &mut *old_dir };
    let new_dir = unsafe { &mut *new_dir };
    if old_dir.sb.is_null() || new_dir.sb.is_null() {
        return Err(Errno::Inval);
    }
    if old_dentry.is_null() || name.is_null() || name_len == 0 {
        return Err(Errno::Inval);
    }
    let mut ret = vfs_inode_valid_locked(&raw mut *old_dir);
    if ret != 0 && ret != neg(EPERM) {
        return Err(Errno::Raw(ret));
    }
    ret = vfs_inode_valid_locked(&raw mut *new_dir);
    if ret != 0 && ret != neg(EPERM) {
        return Err(Errno::Raw(ret));
    }
    let sb = old_dir.sb;
    if sb != new_dir.sb {
        return Err(Errno::XDev); // Cross-device move not supported
    }

    vfs_superblock_wlock(sb);
    let result: c_int = 'out: {
        if !is_dir(old_dir.mode) {
            break 'out neg(ENOTDIR);
        }
        if !is_dir(new_dir.mode) {
            break 'out neg(ENOTDIR);
        }
        // Return value intentionally discarded, matching the C original:
        // `old_dir`/`new_dir` are already proven same-filesystem above,
        // so `vfs_ilock_two_directories` cannot fail (`-EXDEV`) here.
        let _ = vfs_ilock_two_directories(&raw mut *old_dir, &raw mut *new_dir);

        // P3-10b: the old fn-pointer equality gate
        // (`old_move != new_move` -> `-ENOSYS`) collapsed — both
        // directories were just proven same-superblock (`-EXDEV`
        // above), and every inode of a superblock shares its driver's
        // single ops instance, so the tables can no longer differ. A
        // driver without rename (xv6fs) inherits the trait's
        // `Err(NoSys)` default — the same errno the old `None`-slot
        // check produced.
        // SAFETY: `old_dir`/`new_dir` are live, valid, same-superblock
        // directories, both locked, superblock write-locked — the
        // trait method's contract.
        let r = match unsafe {
            inode_ops(&raw mut *old_dir).move_(&raw mut *old_dir, old_dentry, &raw mut *new_dir, name, name_len)
        } {
            Ok(()) => 0,
            Err(e) => e.neg(),
        };
        vfs_iunlock_two(&raw mut *old_dir, &raw mut *new_dir);
        r
    };
    vfs_superblock_unlock(sb);
    if result == 0 { Ok(()) } else { Err(Errno::Raw(result)) }
}

pub(crate) extern "C" fn vfs_move(
    old_dir: *mut vfs_inode,
    old_dentry: *mut vfs_dentry,
    new_dir: *mut vfs_inode,
    name: *const c_char,
    name_len: usize,
) -> c_int {
    result_to_neg_errno(vfs_move_inner(old_dir, old_dentry, new_dir, name, name_len))
}

fn vfs_symlink_inner(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
    target: *const c_char,
    target_len: usize,
) -> KResult<*mut vfs_inode> {
    if dir.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `dir` is non-null (just checked); a live, referenced
    // directory inode the caller holds open. P3-7c boundary hoist — see
    // `vfs_create_inner`.
    let dir = unsafe { &mut *dir };
    if dir.sb.is_null() {
        return Err(Errno::Inval);
    }
    if target.is_null() || target_len == 0 || target_len > VFS_PATH_MAX {
        return Err(Errno::Inval);
    }
    if name.is_null() || name_len == 0 {
        return Err(Errno::Inval);
    }

    loop {
        let sb = dir.sb;
        unsafe { sb_ops(sb).begin_transaction(sb) }?;

        vfs_superblock_wlock(sb);
        vfs_ilock(&raw mut *dir);

        let ret: KResult<*mut vfs_inode> = {
            let v = vfs_inode_valid_locked(&raw mut *dir);
            if v != 0 {
                Err(Errno::Raw(v))
            } else if !is_dir(dir.mode) {
                Err(Errno::NotDir)
            } else {
                // SAFETY: as in `vfs_create_inner`.
                unsafe { inode_ops(&raw mut *dir).symlink(&raw mut *dir, mode, name, name_len, target, target_len) }
            }
        };

        if is_again(&ret) {
            vfs_iunlock(&raw mut *dir);
            vfs_superblock_unlock(sb);
            let _ = unsafe { sb_ops(sb).end_transaction(sb) };
            Scheduler::yield_now();
            continue;
        }

        // Note: symlinks don't need a parent reference (no ".."
        // traversal needed).
        vfs_iunlock(&raw mut *dir);
        vfs_superblock_unlock(sb);

        if let Err(e) = unsafe { sb_ops(sb).end_transaction(sb) } {
            crate::kprintln!("vfs_symlink: warning: end_transaction failed with error {}", e.neg());
        }
        return ret;
    }
}

pub(crate) extern "C" fn vfs_symlink(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
    target: *const c_char,
    target_len: usize,
) -> *mut vfs_inode {
    result_to_errptr(vfs_symlink_inner(dir, mode, name, name_len, target, target_len))
}

fn vfs_itruncate_inner(inode: *mut vfs_inode, new_size: loff_t) -> KResult<()> {
    if inode.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `inode` is non-null (just checked); a live, referenced inode
    // the caller holds open. P3-7c boundary hoist — see
    // `vfs_readlink_inner`.
    let inode = unsafe { &mut *inode };
    if inode.sb.is_null() {
        return Err(Errno::Inval);
    }
    vfs_ilock(&raw mut *inode);
    let ret: c_int = 'out: {
        let v = vfs_inode_valid_locked(&raw mut *inode);
        if v != 0 {
            break 'out v;
        }
        if !is_reg(inode.mode) {
            break 'out neg(EINVAL);
        }
        // SAFETY: `inode` is live, valid, locked, a regular file — the
        // trait method's contract (reborrowed as the raw pointer it takes).
        match unsafe { inode_ops(&raw mut *inode).truncate(&raw mut *inode, new_size) } {
            Ok(()) => 0,
            Err(e) => e.neg(),
        }
    };
    vfs_iunlock(&raw mut *inode);
    if ret == 0 { Ok(()) } else { Err(Errno::Raw(ret)) }
}

pub(crate) extern "C" fn vfs_itruncate(inode: *mut vfs_inode, new_size: loff_t) -> c_int {
    result_to_neg_errno(vfs_itruncate_inner(inode, new_size))
}

/// Lock two non-directory inodes to prevent deadlock (lower address
/// first).
pub(crate) extern "C" fn vfs_ilock_two_nondirectories(inode1: *mut vfs_inode, inode2: *mut vfs_inode) {
    kassert!(
        !inode1.is_null() && !inode2.is_null(),
        "vfs_ilock_two_nondirectories: inode is NULL"
    );
    if (inode1 as usize) < (inode2 as usize) {
        vfs_ilock(inode1);
        vfs_ilock(inode2);
    } else if (inode1 as usize) > (inode2 as usize) {
        vfs_ilock(inode2);
        vfs_ilock(inode1);
    } else {
        vfs_ilock(inode1);
    }
}

/// Lock two directory inodes to prevent deadlock. Caller must hold the
/// superblock read lock and ensure both inodes are directories.
fn vfs_ilock_two_directories_inner(inode1: *mut vfs_inode, inode2: *mut vfs_inode) -> KResult<()> {
    if core::ptr::eq(inode1, inode2) {
        vfs_ilock(inode1);
        return Ok(());
    }
    if unsafe { (*inode1).sb } != unsafe { (*inode2).sb } {
        return Err(Errno::XDev); // Cross-filesystem locking not supported
    }

    // Borrowed from Linux kernel's lockdep strategy.
    let mut p = inode1;
    let mut q;
    let mut r: *mut vfs_inode;
    loop {
        r = unsafe { (*p).parent };
        if core::ptr::eq(r, inode2) || core::ptr::eq(r, p) {
            break;
        }
        p = r;
    }
    if core::ptr::eq(r, inode2) {
        // inode2 is the ancestor of inode1
        vfs_ilock(inode2);
        vfs_ilock(inode1);
        return Ok(());
    }
    q = inode2;
    loop {
        r = unsafe { (*q).parent };
        if core::ptr::eq(r, inode1) || core::ptr::eq(r, q) || core::ptr::eq(r, p) {
            break;
        }
        q = r;
    }
    if core::ptr::eq(r, inode1) {
        // inode1 is the ancestor of inode2
        vfs_ilock(inode1);
        vfs_ilock(inode2);
        return Ok(());
    } else if core::ptr::eq(r, p) {
        // inode1 and inode2 are in different branches
        if (inode1 as usize) < (inode2 as usize) {
            vfs_ilock(inode1);
            vfs_ilock(inode2);
        } else {
            vfs_ilock(inode2);
            vfs_ilock(inode1);
        }
        return Ok(());
    }

    // Since both inodes are on the same filesystem, they must share a
    // common ancestor (the fs root).
    xv6_panic(c"vfs_ilock_two_directories: unexpected condition".as_ptr());
}

pub(crate) extern "C" fn vfs_ilock_two_directories(inode1: *mut vfs_inode, inode2: *mut vfs_inode) -> c_int {
    result_to_neg_errno(vfs_ilock_two_directories_inner(inode1, inode2))
}

pub(crate) extern "C" fn vfs_iunlock_two(inode1: *mut vfs_inode, inode2: *mut vfs_inode) {
    if !inode1.is_null() {
        vfs_iunlock(inode1);
    }
    if !inode2.is_null() && !core::ptr::eq(inode2, inode1) {
        vfs_iunlock(inode2);
    }
}

fn vfs_chdir_inner(new_cwd: *mut vfs_inode) -> KResult<()> {
    if new_cwd.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `new_cwd` is non-null (just checked); a live, referenced
    // inode the caller holds open. P3-7c boundary hoist — plain reads
    // (`sb`, `mode`) go safe; the identity checks and lock/validate/
    // get_ref helpers reborrow `&raw mut *new_cwd`.
    let new_cwd = unsafe { &mut *new_cwd };
    if new_cwd.sb.is_null() {
        return Err(Errno::Inval);
    }
    if core::ptr::eq(&raw const *new_cwd, root_inode_ptr()) {
        return Err(Errno::Inval); // not allowed to change to the dummy root
    }
    let fs = current_fs();
    if core::ptr::eq(&raw const *new_cwd, unsafe { vfs_inode_deref(ptr::addr_of_mut!((*fs).cwd)) }) {
        return Ok(()); // No change
    }

    let sb = new_cwd.sb;
    vfs_superblock_rlock(sb);
    vfs_ilock(&raw mut *new_cwd);

    let mut old: vfs_inode_ref = unsafe { core::mem::zeroed() };

    let v = vfs_inode_valid_locked(&raw mut *new_cwd);
    if v != 0 {
        vfs_iunlock(&raw mut *new_cwd);
        vfs_superblock_unlock(sb);
        vfs_inode_put_ref(&mut old);
        return Err(Errno::Raw(v));
    }
    if !is_dir(new_cwd.mode) {
        vfs_iunlock(&raw mut *new_cwd);
        vfs_superblock_unlock(sb);
        vfs_inode_put_ref(&mut old);
        return Err(Errno::NotDir);
    }

    let mut new_ref: vfs_inode_ref = unsafe { core::mem::zeroed() };
    let r = vfs_inode_get_ref(&raw mut *new_cwd, &mut new_ref);
    if r != 0 {
        vfs_iunlock(&raw mut *new_cwd);
        vfs_superblock_unlock(sb);
        vfs_inode_put_ref(&mut old);
        return Err(Errno::Raw(r));
    }
    vfs_iunlock(&raw mut *new_cwd);
    vfs_superblock_unlock(sb);

    // To keep it simple, fs_struct is only locked around the swap itself
    // (matches the C original's own comment).
    {
        let _g = fs_lock(fs);
        // SAFETY: `fs` is live; `cwd` is a plain embedded field.
        old = unsafe { (*fs).cwd };
        unsafe { (*fs).cwd = new_ref };
    }
    vfs_inode_put_ref(&mut old);
    Ok(())
}

pub(crate) extern "C" fn vfs_chdir(new_cwd: *mut vfs_inode) -> c_int {
    result_to_neg_errno(vfs_chdir_inner(new_cwd))
}

fn vfs_chroot_inner(new_root: *mut vfs_inode) -> KResult<()> {
    // NOTE (preserved 1:1 from the C original): `vfs_chdir`'s return
    // value is discarded here — if it fails (e.g. `new_root` is not a
    // directory or is invalid), `vfs_chroot` still proceeds to change
    // the root. Flagged, not fixed, per port fidelity.
    let _ = vfs_chdir(new_root);

    if core::ptr::eq(new_root, root_inode_ptr()) {
        return Err(Errno::Inval); // not allowed to change to the dummy root
    }
    let fs = current_fs();
    if core::ptr::eq(new_root, unsafe { vfs_inode_deref(ptr::addr_of_mut!((*fs).rooti)) }) {
        return Ok(()); // No change
    }

    let mut new_ref: vfs_inode_ref = unsafe { core::mem::zeroed() };
    let r = vfs_inode_get_ref(new_root, &mut new_ref);
    if r != 0 {
        return Err(Errno::Raw(r));
    }
    vfs_idup(new_root);

    let mut old: vfs_inode_ref;
    {
        let _g = fs_lock(fs);
        old = unsafe { (*fs).rooti };
        unsafe { (*fs).rooti = new_ref };
    }
    vfs_inode_put_ref(&mut old);
    Ok(())
}

pub(crate) extern "C" fn vfs_chroot(new_root: *mut vfs_inode) -> c_int {
    result_to_neg_errno(vfs_chroot_inner(new_root))
}

/// Get current working directory inode of the current process. Caller
/// must call `vfs_iput` on the returned inode when done.
pub(crate) extern "C" fn vfs_curdir() -> *mut vfs_inode {
    let fs = current_fs();
    // SAFETY: `fs` is live; `cwd` is a plain embedded field. Only the
    // current process can change its own cwd, so no lock is needed here
    // (matches the C original).
    let cwd = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*fs).cwd)) };
    kassert!(!cwd.is_null(), "vfs_curdir: current working directory inode is NULL");
    vfs_idup(cwd);
    cwd
}

/// Get current root directory inode of the current process. Caller must
/// call `vfs_iput` on the returned inode when done.
pub(crate) extern "C" fn vfs_curroot() -> *mut vfs_inode {
    let fs = current_fs();
    // SAFETY: see `vfs_curdir`.
    let rooti = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*fs).rooti)) };
    kassert!(!rooti.is_null(), "vfs_curroot: current root directory inode is NULL");
    vfs_idup(rooti);
    rooti
}

/// Internal path lookup implementation. Returns `Err(Errno::Again)` on a
/// transient race (e.g. inode freed during mount traversal); the caller
/// ([`vfs_namei_inner`]) retries.
fn vfs_namei_once(path: *const c_char, path_len: usize) -> KResult<*mut vfs_inode> {
    if path.is_null() || path_len == 0 {
        return Err(Errno::Inval);
    }
    if path_len > VFS_PATH_MAX {
        return Err(Errno::NameTooLong);
    }

    let pathbuf = kmm_alloc(path_len + 1) as *mut c_char;
    if pathbuf.is_null() {
        return Err(Errno::NoMem);
    }

    // Get current root for ".." at root handling.
    let mut rooti = vfs_curroot();
    if is_err_or_null(rooti) {
        kmm_free(pathbuf as *mut c_void);
        if rooti.is_null() {
            return Err(Errno::Inval);
        }
        // `vfs_curroot` is a cross-module-callable C-ABI fn (not owned by
        // this cluster); its `ERR_PTR` encoding is preserved verbatim via
        // `Errno::Raw`.
        return Err(Errno::Raw(ptr_err(rooti)));
    }

    if unsafe { (*rooti).flags.mount() != 0 } {
        // SAFETY: `rooti` is a mountpoint inode; the mount union arm is
        // the live one.
        let mnt_rooti = unsafe { (*rooti).dev_mnt.mnt.mnt_rooti };
        if mnt_rooti.is_null() {
            vfs_iput(rooti);
            kmm_free(pathbuf as *mut c_void);
            return Err(Errno::Inval); // Mounted root inode has no mounted root
        }
        if !atomic_inc_in_range(refcount_atomic(mnt_rooti), 0, VFS_INODE_MAX_REFCOUNT) {
            vfs_iput(rooti);
            kmm_free(pathbuf as *mut c_void);
            return Err(Errno::Again); // Mounted root is dying, retry
        }
        vfs_iput(rooti);
        rooti = mnt_rooti;
    }

    let mut path = path;
    let mut path_len = path_len;
    let mut pos: *mut vfs_inode;
    if unsafe { *path } == b'/' as c_char {
        // Absolute path, start from root.
        pos = rooti;
        vfs_idup(pos);
        path = unsafe { path.add(1) }; // skip leading '/'
        path_len -= 1;
    } else {
        // Relative path, start from cwd.
        pos = vfs_curdir();
        if is_err(pos) {
            let e = Errno::Raw(ptr_err(pos));
            vfs_iput(rooti);
            kmm_free(pathbuf as *mut c_void);
            return Err(e);
        }
    }

    // Copy the path since strtok_r modifies the string.
    if path_len > 0 {
        // SAFETY: `pathbuf` was allocated with `path_len + 1` bytes;
        // `path`/`path_len` describe a valid, disjoint byte range.
        unsafe { core::ptr::copy(path as *const u8, pathbuf as *mut u8, path_len) };
    }
    unsafe { *pathbuf.add(path_len) = 0 };

    let mut saveptr: *mut c_char = ptr::null_mut();
    let mut token = strtok_r(pathbuf, c"/".as_ptr(), &mut saveptr);
    // Mirrors the C original's `ret_inode`/`errored` pair: `result` tracks
    // both the in-flight success pointer and any error, set immediately
    // before each `break` exactly where the original set `ret_inode` +
    // `errored = true`.
    let mut result: KResult<*mut vfs_inode> = Ok(pos);

    while !token.is_null() {
        let token_len = strlen(token);

        let mut dentry: vfs_dentry = unsafe { core::mem::zeroed() };
        let lret = vfs_ilookup(pos, &mut dentry, token, token_len);
        if lret != 0 {
            vfs_iput(pos);
            pos = ptr::null_mut();
            result = Err(Errno::Raw(lret));
            break;
        }

        // P3-10b: `KResult`-native lookup (no ERR_PTR decode).
        let next = vfs_get_dentry_inode_inner(&mut dentry);
        vfs_release_dentry(&mut dentry);
        let next = match next {
            Ok(n) => n,
            Err(e) => {
                vfs_iput(pos);
                pos = ptr::null_mut();
                result = Err(e);
                break;
            }
        };

        vfs_iput(pos);
        pos = next;

        let mut mount_race = false;
        loop {
            let is_mount = unsafe { (*pos).flags.mount() != 0 };
            let mnt_rooti = unsafe { (*pos).dev_mnt.mnt.mnt_rooti };
            if !(is_mount && !mnt_rooti.is_null()) {
                break;
            }
            if !atomic_inc_in_range(refcount_atomic(mnt_rooti), 0, VFS_INODE_MAX_REFCOUNT) {
                // Mount root is dying, need to retry the entire lookup.
                vfs_iput(pos);
                pos = ptr::null_mut();
                result = Err(Errno::Again);
                mount_race = true;
                break;
            }
            vfs_iput(pos);
            pos = mnt_rooti;
        }
        if mount_race {
            break;
        }

        result = Ok(pos);
        token = strtok_r(ptr::null_mut(), c"/".as_ptr(), &mut saveptr);
    }

    vfs_iput(rooti);
    kmm_free(pathbuf as *mut c_void);
    match result {
        Ok(p) if p.is_null() => Err(Errno::NoEnt),
        other => other,
    }
}

/// Resolve a path to an inode, handling retry logic for transient race
/// conditions (e.g. inode freed during mount traversal).
fn vfs_namei_inner(path: *const c_char, path_len: usize) -> KResult<*mut vfs_inode> {
    let mut retries = 0;
    loop {
        match vfs_namei_once(path, path_len) {
            Err(Errno::Again) => {
                // Transient race condition, yield and retry.
                Scheduler::yield_now();
                retries += 1;
                if retries >= VFS_NAMEI_MAX_RETRIES {
                    // Too many retries, return the error.
                    return Err(Errno::Again);
                }
            }
            other => return other,
        }
    }
}

/// Resolve a path to an inode. Public wrapper handling retry logic for
/// transient race conditions (e.g. inode freed during mount traversal).
///
/// Returns an inode pointer with its refcount incremented on success, or
/// `ERR_PTR(errno)` on failure.
pub(crate) extern "C" fn vfs_namei(path: *const c_char, path_len: usize) -> *mut vfs_inode {
    result_to_errptr(vfs_namei_inner(path, path_len))
}

/// Resolve the parent directory of a path and copy the final name
/// component into `name`. Returns the parent directory inode with a
/// reference held on success, or `Err` on failure.
fn vfs_nameiparent_inner(
    path: *const c_char,
    path_len: usize,
    name: *mut c_char,
    name_size: usize,
) -> KResult<*mut vfs_inode> {
    if path.is_null() || path_len == 0 || name.is_null() || name_size == 0 {
        return Err(Errno::Inval);
    }
    if path_len > VFS_PATH_MAX {
        return Err(Errno::NameTooLong);
    }

    // Find the last path component.
    let mut end = path_len;
    // SAFETY: `path`/`path_len` describe a valid byte range.
    while end > 0 && unsafe { *path.add(end - 1) } == b'/' as c_char {
        end -= 1;
    }
    if end == 0 {
        // Path is just "/" or empty after trimming.
        return Err(Errno::Inval);
    }

    // Find the start of the last component.
    let mut name_start = end;
    while name_start > 0 && unsafe { *path.add(name_start - 1) } != b'/' as c_char {
        name_start -= 1;
    }

    // Extract the name component, truncating to fit the buffer (xv6
    // compatibility).
    let mut final_name_len = end - name_start;
    if final_name_len >= name_size {
        final_name_len = name_size - 1;
    }
    // SAFETY: `name` has room for `name_size` bytes (caller contract);
    // `path[name_start..name_start+final_name_len]` is in-bounds.
    unsafe {
        core::ptr::copy(
            path.add(name_start) as *const u8,
            name as *mut u8,
            final_name_len,
        );
        *name.add(final_name_len) = 0;
    }

    // Now get the parent path.
    let mut parent_len = name_start;
    while parent_len > 0 && unsafe { *path.add(parent_len - 1) } == b'/' as c_char {
        parent_len -= 1;
    }

    if parent_len == 0 {
        // Parent is root.
        return Ok(if unsafe { *path } == b'/' as c_char {
            vfs_curroot()
        } else {
            // Relative path with just one component, parent is cwd.
            vfs_curdir()
        });
    }

    // Resolve the parent path. `vfs_namei` is this same file's public
    // C-ABI wrapper (still `ERR_PTR`-encoded, unchanged); bridge back to
    // `KResult` at this internal call site via `errptr_to_result`.
    errptr_to_result(vfs_namei(path, parent_len)).map_err(Errno::Raw)
}

/// Resolve the parent directory of a path and copy the final name
/// component into `name`. Returns the parent directory inode with a
/// reference held on success, or `ERR_PTR` on failure.
pub(crate) extern "C" fn vfs_nameiparent(
    path: *const c_char,
    path_len: usize,
    name: *mut c_char,
    name_size: usize,
) -> *mut vfs_inode {
    result_to_errptr(vfs_nameiparent_inner(path, path_len, name, name_size))
}
