//! VFS mount/superblock registry — Rust port of `kernel/vfs/fs.c` (Phase 2
//! Wave 16, see `docs/rustify/phase2_plan.md`).
//!
//! # Scope note (deviates from the plan's guess)
//!
//! The wave plan describes fs.c as "mount/superblock registry vs path
//! lookup". In the actual source tree `vfs_namei`/`vfs_nameiparent`/
//! `vfs_chdir`/`vfs_chroot`/`vfs_curdir`/`vfs_curroot` (the real
//! path-lookup machinery) already live in `vfs/inode.c` and were ported
//! to [`super::inode`] in Wave 13 — confirmed against the pre-Wave-13 C
//! (`git show HEAD:kernel/vfs/inode.c`). `fs.c` itself never defined
//! them. So this file has no separate "path lookup" half; it is the
//! filesystem-type registry, the mount/unmount state machine, the
//! superblock lock/refcount/mountcount API (the `vfs_superblock_{r,w}lock`
//! pair `inode.rs` already calls as an extern), the per-superblock inode
//! hash cache (`vfs_get_inode_cached`/`vfs_add_inode`/`vfs_remove_inode`),
//! dentry-to-inode resolution, `fs_struct` lifecycle, and the debug-dump
//! functions. The two build+boot checkpoints called for by the plan were
//! done as: (a) this file compiling and linking cleanly against every
//! existing extern declaration in `inode.rs`/`file.rs`/`pipe.rs`/
//! `fdtable.rs`/`thread.rs`/`clone.rs`/`exit.rs` (a pure symbol-contract
//! check — nothing else in the crate changed), then (b) full boot +
//! mount + path-battery verification (see the wave's final report).
//!
//! # Lock order (unchanged from the C original's own comment)
//!
//! 1. mount mutex, [`vfs_mount_lock`]/[`vfs_mount_unlock`] (this file).
//! 2. `vfs_superblock.lock` (rwsem), [`vfs_superblock_rlock`]/
//!    [`vfs_superblock_wlock`]/[`vfs_superblock_unlock`] (this file —
//!    the same paired-C-ABI-lock/unlock convention as `inode.rs`'s
//!    `vfs_ilock`/`vfs_iunlock`, since the matching unlock call often
//!    happens in a different function/translation unit than the lock
//!    call, which no RAII guard could span).
//! 3. `vfs_inode.mutex`, via `inode.rs`'s `vfs_ilock`/`vfs_iunlock`.
//!
//! When multiple locks of the same type are needed: parent superblock
//! before child superblock; directory inode before child inode. Crossing
//! filesystems requires releasing an inode lock before acquiring another
//! filesystem's inode lock; inode locks are always acquired after any
//! superblock lock (including a mounted superblock's).
//!
//! # Style notes (rust-skills)
//!
//! Self-contained per this crate's established per-file convention (see
//! `inode.rs`'s externs-block doc): every cross-module C-ABI symbol is
//! declared locally rather than imported by Rust path, and small
//! `list.h`/`hlist.h` `static inline` helpers with no external linkage in
//! the C original (`list_entry_init`, `list_node_push_back/front`,
//! `list_node_detach`, `hlist_first_entry`/`hlist_next_entry` and
//! friends) are reimplemented natively here rather than borrowed from
//! `kernel/list.rs`/`kernel/hlist.rs`, matching `inode.rs`/`file.rs`/
//! `pipe.rs`/`fdtable.rs`. Bitfield flags go through the native flag
//! holders' `.flags.field()`/`.set_field()` accessors (P3-N5; bindgen's
//! `__bindgen_anon_N` forms for the still-bindgen `vfs_inode`) on a
//! temporary place expression, never via a materialized `&mut` of the
//! whole struct. `unsafe` is scope-minimized with `SAFETY:` comments at each
//! non-obvious site. One **deliberate fidelity deviation**, already
//! established by `inode.rs`/`file.rs`/`pipe.rs`/`fdtable.rs`: the C
//! `assert(expr, fmt, ...)` macro's `printf`-style format arguments
//! (e.g. `"errno=%d"`) are dropped in favor of this crate's fixed-message
//! `xv6_panic(msg) -> !` (`__panic_start(); printf("%s\n", msg);
//! __panic_end();`, no variadic formatting) — these are unrecoverable
//! invariant violations either way, so the exact panic text doesn't
//! affect behavior. Genuine informational `printf(...)` calls (mount
//! status lines, the debug-dump functions) keep their real format
//! strings and arguments.
//!
//! `vfs_root_inode` is deliberately **not** run through `mutex_init`
//! anywhere (C original: `struct vfs_inode vfs_root_inode = {0};` +
//! `__vfs_rooti_init()`'s `memset(..., 0, ...)`, never `mutex_init`) —
//! its embedded `mutex_t`/`completion_t` stay all-zero-bytes for the
//! process lifetime. This is a pre-existing quirk of the root inode
//! being a VFS-synthesized sentinel with no backing filesystem, carried
//! over unchanged; not something this port introduces or fixes.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::mem::offset_of;
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{
    fs_struct, hlist_bucket_t, hlist_entry_t, hlist_func_t, hlist_t, kobject, list_node_t,
    mutex_t, slab_cache_t, spinlock_t, thread, vfs_dentry, vfs_fs_type,
    vfs_inode, vfs_inode_ref, vfs_superblock, work_struct,
    workqueue, EAGAIN, EALREADY, EBUSY, EEXIST, EINVAL, ENODEV, ENOENT, ENOSPC, ENOTDIR, EPERM,
    RWLOCK_PRIO_READ,
};

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N5 (VFS type family, fs_struct slice).
//
// This IS the kernel-wide Rust definition of `kernel/inc/vfs/
// vfs_types.h`'s `struct fs_struct` now: `build.rs` blocklists the
// bindgen-generated form and injects a `pub use crate::vfs::fs::FsStruct
// as fs_struct;` facade re-export (no `_t` typedef exists). This file
// owns the concept (`VFS_STRUCT_CACHE`, `vfs_struct_init`/`_clone`/
// `_dup`/`_put` lifecycle). Field names/types reproduce bindgen's
// exactly; derived Copy/Clone exactly as the pre-nativization bindgen
// output did. `vfs_inode_ref` (kernel/inc/types.h) deliberately stays
// bindgen-emitted — out of this wave's scope — and is embedded by value
// via its `crate::bindings` path, the sanctioned mixed-tier pattern.
// The C `__ALIGNED_CACHELINE` on the embedded `spinlock_t` typedef
// gives the record align 64, carried here as an explicit
// `repr(align(64))` exactly as bindgen emitted it.
// ---------------------------------------------------------------------------

/// `struct fs_struct` (`kernel/inc/vfs/vfs_types.h`): per-process
/// filesystem state (root inode + cwd), allocated on the kernel stack
/// below `utrapframe` or from [`VFS_STRUCT_CACHE`].
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct FsStruct {
    pub lock: spinlock_t,
    pub rooti: vfs_inode_ref,
    pub cwd: vfs_inode_ref,
    pub ref_count: c_int,
}

// P3-N5 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (verified in-tree by a temporary
// `offset_of!` gate on `crate::bindings::fs_struct` before the switch)
// and independently confirmed by the cross-compiler `_Static_assert`
// probe (toolchain riscv64-unknown-elf-gcc, rv64gc/lp64d, gcc &
// clang-18 agree; scratchpad p3n5_static_assert_probe.c): 64/64,
// offsets 0/24/40/56.
const _: () = {
    assert!(core::mem::size_of::<FsStruct>() == 64, "fs_struct size");
    assert!(core::mem::align_of::<FsStruct>() == 64, "fs_struct alignment");
    assert!(core::mem::offset_of!(FsStruct, lock) == 0, "fs_struct.lock offset");
    assert!(core::mem::offset_of!(FsStruct, rooti) == 24, "fs_struct.rooti offset");
    assert!(core::mem::offset_of!(FsStruct, cwd) == 40, "fs_struct.cwd offset");
    assert!(core::mem::offset_of!(FsStruct, ref_count) == 56, "fs_struct.ref_count offset");
};

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N5 (VFS type family, fs_type + superblock
// slice).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/vfs/
// vfs_types.h`'s `struct vfs_fs_type`/`struct vfs_fs_type_ops`/`struct
// vfs_superblock`/`struct vfs_superblock_ops` now: `build.rs` blocklists
// the bindgen-generated forms and injects `pub use crate::vfs::fs::...
// as ...;` facade re-exports (no `_t` typedefs exist). The ops-table
// fn-pointer fields reproduce bindgen's `Option<unsafe extern "C" fn>`
// forms exactly (trait-ification is P3-10's job). The two anonymous C
// bitfield structs became named 8/8 `{bits,_pad}` holders
// ([`VfsFsTypeFlagBits`]/[`VfsSuperblockFlagBits`], field `flags`) with
// safe masking accessors bit-identical to bindgen's little-endian units
// (N3/N4 precedent); `vfs_superblock`'s anonymous inode-hash struct
// (bindgen's `vfs_superblock__bindgen_ty_1`) is flattened into direct
// `inodes`/`inodes_buckets` fields at identical offsets — C anonymous
// struct members are accessed directly in C anyway, so the flattened
// form is the faithful one. Copy fidelity: the ops tables derived
// Copy/Clone in the pre-nativization bindgen output; `vfs_fs_type`
// (kobject-embedder class, N4 precedent) and `vfs_superblock` derived
// NEITHER, so the natives deliberately have no derives — the
// still-bindgen `tmpfs_superblock`/`xv6fs_superblock` embed
// `vfs_superblock` by value and had no derives either, and the accurate
// NONCOPY answer in build.rs keeps their derive lines unchanged.
// `statfs` (kernel/inc/uabi/statfs.h — uabi, P3-4's scrutiny class) and
// `pcache` stay bindgen-emitted, referenced by `crate::bindings` path.
// ---------------------------------------------------------------------------

/// The per-filesystem type-operations vtable — wave P3-10b's
/// replacement for the C-style `struct vfs_fs_type_ops` fn-pointer
/// table (ops-table redesign, P3-10a `FileOps` precedent: full Rust
/// style, C-compatible interface removed). Implementors are zero-sized
/// unit structs with a `static` instance (`XV6FS_FS_TYPE_OPS`,
/// `TMPFS_FS_TYPE_OPS`, `DEVTMPFS_FS_TYPE_OPS`) installed into
/// [`VfsFsType::ops`] as `Some(&STATIC)`.
///
/// Slot-nullability mapping: `mount` and `free` are both REQUIRED
/// methods — `vfs_register_fs_type` used to reject a table with either
/// slot `None` (`-EINVAL`), so no registered type ever dispatched a
/// missing slot; the trait now guarantees both at compile time and the
/// registration-time slot checks collapse into the single
/// [`VfsFsType::ops`] `is_some` check.
///
/// `mount`'s C shape (`ret_sb` out-param + `int` errno return) became a
/// `KResult<*mut VfsSuperblock>`: `Ok(sb)` is exactly the old
/// "`return 0` with `*ret_sb` written", `Err(e)` the old non-zero errno
/// return (every implementor wrote `*ret_sb` only on success, so no
/// information is lost).
///
/// `Sync` supertrait: instances are shared crate-wide as `&'static`
/// references reachable from any CPU.
pub trait FsTypeOps: Sync {
    /// Allocate and initialize a new (still-private) superblock for a
    /// mount of this filesystem type at `mountpoint` backed by `device`
    /// (null for backendless filesystems).
    ///
    /// # Safety
    /// `mountpoint` must be a live, locked directory inode;
    /// `device` must be null or a live device inode; `data` must be
    /// null or a NUL-terminated option string. Called with the mount
    /// mutex held.
    unsafe fn mount(
        &self,
        mountpoint: *mut vfs_inode,
        device: *mut vfs_inode,
        flags: c_int,
        data: *const c_char,
    ) -> KResult<*mut VfsSuperblock>;

    /// Free a superblock previously returned by [`FsTypeOps::mount`]
    /// (after the VFS core has fully detached it).
    ///
    /// # Safety
    /// `sb` must be a superblock allocated by this instance's `mount`,
    /// already detached from the mount tree with no live inodes.
    unsafe fn free(&self, sb: *mut VfsSuperblock);
}

/// Native replacement for the anonymous C bitfield struct
/// `struct { uint64 registered : 1; }` inside `struct vfs_fs_type`
/// (bindgen's `vfs_fs_type__bindgen_ty_1`: a 1-byte
/// `__BindgenBitfieldUnit` + 7 pad bytes, `repr(C, align(8))`, 8/8).
/// riscv64 is little-endian, so C allocates `registered` at bit 0 of
/// the unit's byte 0 — identical to bindgen's `get(0,1)` accessor.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct VfsFsTypeFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

impl VfsFsTypeFlagBits {
    #[inline]
    pub(crate) fn registered(&self) -> u64 {
        (self.bits & 0b1) as u64
    }
    #[inline]
    pub(crate) fn set_registered(&mut self, val: u64) {
        self.bits = (self.bits & !0b1) | ((val as u8) & 0b1);
    }
}

/// `struct vfs_fs_type` (`kernel/inc/vfs/vfs_types.h`): a registered
/// filesystem type, protected by the global `vfs_fs_types_lock`. The
/// anonymous bitfield struct became the named `flags` field (consumers
/// re-pointed from bindgen's `__bindgen_anon_1`).
#[repr(C)]
pub struct VfsFsType {
    pub list_entry: list_node_t,
    pub superblocks: list_node_t,
    pub kobj: kobject,
    pub flags: VfsFsTypeFlagBits,
    pub sb_count: c_int,
    pub name: *const c_char,
    /// The driver's type-operations vtable — a real Rust trait object
    /// as of P3-10b (16-byte fat pointer; `None` only for a
    /// freshly-allocated, not-yet-registered `vfs_fs_type`, which
    /// `vfs_register_fs_type` rejects exactly as it rejected the old
    /// null table pointer).
    pub ops: Option<&'static dyn FsTypeOps>,
}

/// Native replacement for the anonymous C bitfield struct inside
/// `struct vfs_superblock` (bindgen's `vfs_superblock__bindgen_ty_2`,
/// 8/8): the superblock state flags. Bit order (LE unit byte 0):
/// valid=0, dirty=1, backendless=2, initialized=3, registered=4,
/// syncing=5, unmounting=6, attached=7 — identical to bindgen's
/// `get(N,1)`/`set(N,1)` accessors.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct VfsSuperblockFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

macro_rules! sb_flag_bit {
    ($get:ident, $set:ident, $bit:expr) => {
        #[inline]
        pub(crate) fn $get(&self) -> u64 {
            ((self.bits >> $bit) & 0b1) as u64
        }
        #[inline]
        pub(crate) fn $set(&mut self, val: u64) {
            self.bits = (self.bits & !(1u8 << $bit)) | (((val as u8) & 0b1) << $bit);
        }
    };
}

impl VfsSuperblockFlagBits {
    sb_flag_bit!(valid, set_valid, 0);
    sb_flag_bit!(dirty, set_dirty, 1);
    sb_flag_bit!(backendless, set_backendless, 2);
    sb_flag_bit!(initialized, set_initialized, 3);
    sb_flag_bit!(registered, set_registered, 4);
    sb_flag_bit!(syncing, set_syncing, 5);
    sb_flag_bit!(unmounting, set_unmounting, 6);
    sb_flag_bit!(attached, set_attached, 7);
}

// ===========================================================================
// Native uabi `struct statfs` — P3-4b nativization (user directive:
// remove the C-compatible interfaces; userspace-ABI scrutiny class).
// `Statfs` is the canonical KERNEL-SIDE definition of
// `kernel/inc/uabi/statfs.h`'s `struct statfs`: `build.rs` blocklists
// the bindgen emission and re-exports this type as
// `crate::bindings::statfs` (facade `pub use`, N2 pattern).
//
// *** USERSPACE ABI — HANDLE WITH P3-4 SCRUTINY *** The C header STAYS:
// user/ programs compile against uabi/statfs.h, and the kernel
// `either_copyout`s this record BY VALUE into user buffers
// (sys_statfs path in vfs_syscall.rs; filled by the per-fs
// `VfsSuperblockOps::statfs` ops). A layout slip here is SILENT
// userspace breakage, so the byte-exact asserts below pin the native to
// the header. HOST determination: no host-side tool consumes
// uabi/statfs.h (grep-verified), so the gcc probe is target-only.
//
// DERIVE DECISION (P3-4b): Copy + Clone, exactly as the
// pre-nativization bindgen output derived (nine plain `uint64`s).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen form + toolchain-gcc `_Static_assert` probe (rv64gc/lp64d —
// scratchpad p3_4b_uabi_probe.c); both agree on every value asserted
// below.
// ===========================================================================

/// Native uabi `struct statfs` (`kernel/inc/uabi/statfs.h`) — the
/// record `statfs(2)` copies out to userspace by value.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Statfs {
    /// Type of filesystem.
    pub f_type: crate::bindings::uint64,
    /// Optimal transfer block size.
    pub f_bsize: crate::bindings::uint64,
    /// Total data blocks in filesystem.
    pub f_blocks: crate::bindings::uint64,
    /// Free blocks in filesystem.
    pub f_bfree: crate::bindings::uint64,
    /// Free blocks available to unprivileged user.
    pub f_bavail: crate::bindings::uint64,
    /// Total inodes in filesystem.
    pub f_files: crate::bindings::uint64,
    /// Free inodes in filesystem.
    pub f_ffree: crate::bindings::uint64,
    /// Maximum length of filenames.
    pub f_namelen: crate::bindings::uint64,
    /// Fragment size.
    pub f_frsize: crate::bindings::uint64,
}

// P3-4b hardcoded layout proof — the USERSPACE byte contract
// (`uabi/statfs.h` `struct statfs`), every field. Values captured from
// the pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the target gcc probe.
const _: () = {
    assert!(core::mem::size_of::<Statfs>() == 72, "statfs size (USERSPACE ABI)");
    assert!(core::mem::align_of::<Statfs>() == 8, "statfs alignment");
    assert!(core::mem::offset_of!(Statfs, f_type) == 0, "statfs.f_type offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Statfs, f_bsize) == 8, "statfs.f_bsize offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Statfs, f_blocks) == 16, "statfs.f_blocks offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Statfs, f_bfree) == 24, "statfs.f_bfree offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Statfs, f_bavail) == 32, "statfs.f_bavail offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Statfs, f_files) == 40, "statfs.f_files offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Statfs, f_ffree) == 48, "statfs.f_ffree offset (USERSPACE ABI)");
    assert!(
        core::mem::offset_of!(Statfs, f_namelen) == 56,
        "statfs.f_namelen offset (USERSPACE ABI)"
    );
    assert!(
        core::mem::offset_of!(Statfs, f_frsize) == 64,
        "statfs.f_frsize offset (USERSPACE ABI)"
    );
};

/// `struct vfs_superblock` (`kernel/inc/vfs/vfs_types.h`). The
/// anonymous inode-hash struct is flattened into direct
/// `inodes`/`inodes_buckets` fields (bindgen's `__bindgen_anon_1`, same
/// offsets); the anonymous bitfield struct became the named `flags`
/// field (bindgen's `__bindgen_anon_2`). `_pad0`/`_pad1` reproduce
/// bindgen's `__bindgen_padding_0`/`_1` verbatim (the C
/// `__ALIGNED_CACHELINE` rides the `struct rwsem` record and the
/// `spinlock_t` typedef; the native `RawRwsem`/`RawSpinlock` are align
/// 8, so the explicit pads carry the 64-byte placement, exactly as
/// bindgen emitted them).
#[repr(C, align(64))]
pub struct VfsSuperblock {
    pub siblings: list_node_t,
    pub fs_type: *mut VfsFsType,
    pub inodes: hlist_t,
    pub inodes_buckets: [hlist_bucket_t; VFS_SUPERBLOCK_HASH_BUCKETS],
    pub flags: VfsSuperblockFlagBits,
    pub parent_sb: *mut VfsSuperblock,
    pub mountpoint: *mut vfs_inode,
    pub device: *mut vfs_inode,
    pub root_inode: *mut vfs_inode,
    /// The driver's superblock-operations vtable — a real Rust trait
    /// object as of P3-10b (16-byte fat pointer; `None` only for a
    /// driver-private superblock whose `mount` callback forgot to set
    /// it, which mount-time validation rejects exactly as it rejected
    /// the old null table pointer). The fat pointer absorbed one `u64`
    /// of the old `_pad0`, keeping `lock` on its cache-line boundary.
    pub ops: Option<&'static dyn SuperblockOps>,
    pub(crate) _pad0: [u64; 6],
    pub lock: crate::bindings::rwsem,
    pub fs_data: *mut c_void,
    pub mount_count: c_int,
    pub refcount: c_int,
    pub orphan_count: c_int,
    pub orphan_list: list_node_t,
    pub(crate) _pad1: [u64; 3],
    pub spinlock: spinlock_t,
    pub block_size: usize,
    pub total_blocks: crate::bindings::uint64,
    pub used_blocks: crate::bindings::uint64,
}

/// `kernel/inc/vfs/vfs_types.h`: `#define VFS_SUPERBLOCK_HASH_BUCKETS 61`
/// (bindgen hardcoded `61usize` from the macro-expanded header).
pub(crate) const VFS_SUPERBLOCK_HASH_BUCKETS: usize = 61;

/// The per-filesystem superblock-operations vtable — wave P3-10b's
/// replacement for the C-style `struct vfs_superblock_ops` fn-pointer
/// table (ops-table redesign, P3-10a `FileOps` precedent: full Rust
/// style, C-compatible interface removed). Implementors are zero-sized
/// unit structs with a `static` instance (`XV6FS_SUPERBLOCK_OPS`,
/// `TMPFS_SUPERBLOCK_OPS`, `DEVTMPFS_SUPERBLOCK_OPS`) installed into
/// [`VfsSuperblock::ops`] by the driver's `mount` callback as
/// `Some(&STATIC)`.
///
/// Slot-nullability mapping (each old `Option<fn>` slot's `None`
/// dispatch behavior is preserved exactly):
///
/// * `alloc_inode`/`get_inode`/`sync_fs`/`unmount_begin` — required
///   methods: `__vfs_superblock_ops_valid` (mount-time validation)
///   used to reject a table missing any of the four, so no attached
///   superblock ever dispatched a missing slot; the trait guarantees
///   them and the validation collapses into the [`VfsSuperblock::ops`]
///   `is_some` check ([`superblock_ops_valid`]).
/// * `add_orphan`/`remove_orphan` — default `Ok(())`: a `None` slot
///   was silently skipped by `vfs_make_orphan`/`vfs_iput` (no call, no
///   warning), and `Ok(())` takes the identical no-warning path.
///   (xv6fs's old always-0 stubs are likewise covered by the default.)
/// * `recover_orphans` — default `Ok(())`; the VFS core has NO dispatch
///   site for it yet (slot preserved from the C table for the future
///   orphan-recovery mount path).
/// * `statfs` — default `Ok(())`: `sys_statfs` treated a `None` slot as
///   "keep the generic `kbuf` fields", which is exactly what a
///   do-nothing `Ok(())` does. (The old callback's positive returns
///   were also passed through as success; no implementor ever returned
///   one, so `KResult<()>` loses nothing.)
/// * `begin_transaction`/`end_transaction` — default `Ok(())`: every
///   dispatch site skipped a `None` slot (transactionless filesystems,
///   i.e. tmpfs/devtmpfs), and a no-op `Ok(())` is behaviorally
///   identical at each of them.
///
/// `alloc_inode`/`get_inode`'s old `*mut vfs_inode` `ERR_PTR` encoding
/// became `KResult<*mut vfs_inode>` (never `Ok(null)` — the VFS core
/// unconditionally initializes the success pointer). Kept as a raw
/// pointer rather than `NonNull` because every consumer immediately
/// stores/casts it as a raw pointer anyway.
///
/// `Sync` supertrait: instances are shared crate-wide as `&'static`
/// references reachable from any CPU.
pub trait SuperblockOps: Sync {
    /// Allocate a fresh in-memory inode for `sb` (returned unlocked,
    /// un-hashed; the VFS core initializes and hashes it).
    ///
    /// # Safety
    /// `sb` must be a live superblock of this driver; caller holds the
    /// superblock write lock.
    unsafe fn alloc_inode(&self, sb: *mut VfsSuperblock) -> KResult<*mut vfs_inode>;

    /// Load inode `ino` from the backend (returned unlocked, un-hashed).
    ///
    /// # Safety
    /// Same contract as [`SuperblockOps::alloc_inode`].
    unsafe fn get_inode(&self, sb: *mut VfsSuperblock, ino: u64) -> KResult<*mut vfs_inode>;

    /// Flush all dirty filesystem state to the backend.
    ///
    /// # Safety
    /// `sb` must be a live superblock of this driver; caller holds the
    /// superblock write lock.
    unsafe fn sync_fs(&self, sb: *mut VfsSuperblock, wait: c_int) -> KResult<()>;

    /// Prepare for unmount (evict unreferenced cached inodes, quiesce
    /// background work).
    ///
    /// # Safety
    /// `sb` must be a live superblock of this driver; caller holds the
    /// superblock write lock.
    unsafe fn unmount_begin(&self, sb: *mut VfsSuperblock);

    /// Persist `inode` into the on-disk orphan journal.
    ///
    /// # Safety
    /// `sb`/`inode` must be live and belong together; caller holds the
    /// superblock write lock and the inode mutex.
    unsafe fn add_orphan(&self, _sb: *mut VfsSuperblock, _inode: *mut vfs_inode) -> KResult<()> {
        Ok(()) // Journal-less filesystem (old `None`-slot skip).
    }

    /// Remove `inode` from the on-disk orphan journal.
    ///
    /// # Safety
    /// Same contract as [`SuperblockOps::add_orphan`].
    unsafe fn remove_orphan(&self, _sb: *mut VfsSuperblock, _inode: *mut vfs_inode) -> KResult<()> {
        Ok(()) // Journal-less filesystem (old `None`-slot skip).
    }

    /// Reclaim orphaned inodes left by a crash. (No VFS dispatch site
    /// exists yet — slot preserved from the C table for the future
    /// orphan-recovery mount path.)
    ///
    /// # Safety
    /// `sb` must be a live superblock of this driver.
    unsafe fn recover_orphans(&self, _sb: *mut VfsSuperblock) -> KResult<()> {
        Ok(())
    }

    /// Fill in filesystem-specific `statfs(2)` fields (the VFS core
    /// pre-fills the generic ones; a driver without the op keeps them).
    ///
    /// # Safety
    /// `sb` must be a live superblock of this driver; `buf` must be a
    /// live kernel `statfs` buffer.
    unsafe fn statfs(&self, _sb: *mut VfsSuperblock, _buf: *mut crate::bindings::statfs) -> KResult<()> {
        Ok(()) // Keep the generic fields (old `None`-slot skip).
    }

    /// Open a metadata transaction (paired with
    /// [`SuperblockOps::end_transaction`]). May sleep; never called
    /// with the superblock or inode locks held.
    ///
    /// # Safety
    /// `sb` must be a live superblock of this driver.
    unsafe fn begin_transaction(&self, _sb: *mut VfsSuperblock) -> KResult<()> {
        Ok(()) // Transactionless filesystem (old `None`-slot skip).
    }

    /// Close the transaction opened by
    /// [`SuperblockOps::begin_transaction`].
    ///
    /// # Safety
    /// `sb` must be a live superblock of this driver with an open
    /// transaction on the calling thread.
    unsafe fn end_transaction(&self, _sb: *mut VfsSuperblock) -> KResult<()> {
        Ok(()) // Transactionless filesystem (old `None`-slot skip).
    }
}

/// The driver ops of a validated superblock. Mount-time validation
/// ([`superblock_ops_valid`]) guarantees every attached superblock has
/// `Some` ops, so a `None` here is a driver bug (the old code would
/// have dereferenced a null table pointer — UB; this panics instead).
#[inline]
pub(crate) fn sb_ops(sb: *mut VfsSuperblock) -> &'static dyn SuperblockOps {
    // SAFETY: every caller passes a non-null, live superblock (their own
    // documented precondition).
    unsafe { (*sb).ops.expect("vfs: superblock ops missing") }
}

// P3-10b layout facts — NATIVE-OWNED, no C mirror exists (bindgen,
// wrapper.h, and every C consumer are gone; these asserts document the
// new truth rather than pinning to a header). `vfs_fs_type_ops`/
// `vfs_superblock_ops` no longer exist as record types (they are the
// `FsTypeOps`/`SuperblockOps` traits now), so their table asserts are
// deleted. `vfs_fs_type` grew 104 -> 112 (its tail `ops` pointer went
// fat; nothing depended on its total size). `vfs_superblock` keeps
// EVERY former offset and its 1472/64 size/align: the fat `ops`
// absorbed one `u64` of the old 7-word `_pad0`, so `lock` stays on the
// 64-byte cache-line boundary at 1152 (the record's alignment
// invariant) and all fields after it are untouched. The
// niche-optimized `Option<&'static dyn ...>` holders stay plain fat
// pointers (16/8).
const _: () = {
    assert!(
        core::mem::size_of::<Option<&'static dyn FsTypeOps>>() == 16,
        "fs_type ops fat pointer size"
    );
    assert!(
        core::mem::size_of::<Option<&'static dyn SuperblockOps>>() == 16,
        "superblock ops fat pointer size"
    );
    assert!(core::mem::size_of::<VfsFsTypeFlagBits>() == 8, "vfs_fs_type anon bitfield size");
    assert!(core::mem::align_of::<VfsFsTypeFlagBits>() == 8, "vfs_fs_type anon bitfield align");
    assert!(core::mem::size_of::<VfsFsType>() == 112, "vfs_fs_type size");
    assert!(core::mem::align_of::<VfsFsType>() == 8, "vfs_fs_type alignment");
    assert!(core::mem::offset_of!(VfsFsType, list_entry) == 0, "vfs_fs_type.list_entry offset");
    assert!(core::mem::offset_of!(VfsFsType, superblocks) == 16, "vfs_fs_type.superblocks offset");
    assert!(core::mem::offset_of!(VfsFsType, kobj) == 32, "vfs_fs_type.kobj offset");
    assert!(core::mem::offset_of!(VfsFsType, flags) == 72, "vfs_fs_type anon bitfield offset");
    assert!(core::mem::offset_of!(VfsFsType, sb_count) == 80, "vfs_fs_type.sb_count offset");
    assert!(core::mem::offset_of!(VfsFsType, name) == 88, "vfs_fs_type.name offset");
    assert!(core::mem::offset_of!(VfsFsType, ops) == 96, "vfs_fs_type.ops offset");
    assert!(core::mem::size_of::<VfsSuperblockFlagBits>() == 8, "vfs_superblock anon bitfield size");
    assert!(
        core::mem::align_of::<VfsSuperblockFlagBits>() == 8,
        "vfs_superblock anon bitfield align"
    );
    assert!(core::mem::size_of::<VfsSuperblock>() == 1472, "vfs_superblock size");
    assert!(core::mem::align_of::<VfsSuperblock>() == 64, "vfs_superblock alignment");
    assert!(core::mem::offset_of!(VfsSuperblock, siblings) == 0, "vfs_superblock.siblings offset");
    assert!(core::mem::offset_of!(VfsSuperblock, fs_type) == 16, "vfs_superblock.fs_type offset");
    assert!(core::mem::offset_of!(VfsSuperblock, inodes) == 24, "vfs_superblock.inodes offset");
    assert!(
        core::mem::offset_of!(VfsSuperblock, inodes_buckets) == 72,
        "vfs_superblock.inodes_buckets offset"
    );
    assert!(core::mem::offset_of!(VfsSuperblock, flags) == 1048, "vfs_superblock anon bitfield offset");
    assert!(core::mem::offset_of!(VfsSuperblock, parent_sb) == 1056, "vfs_superblock.parent_sb offset");
    assert!(
        core::mem::offset_of!(VfsSuperblock, mountpoint) == 1064,
        "vfs_superblock.mountpoint offset"
    );
    assert!(core::mem::offset_of!(VfsSuperblock, device) == 1072, "vfs_superblock.device offset");
    assert!(
        core::mem::offset_of!(VfsSuperblock, root_inode) == 1080,
        "vfs_superblock.root_inode offset"
    );
    assert!(core::mem::offset_of!(VfsSuperblock, ops) == 1088, "vfs_superblock.ops offset");
    assert!(core::mem::offset_of!(VfsSuperblock, lock) == 1152, "vfs_superblock.lock offset");
    assert!(core::mem::offset_of!(VfsSuperblock, fs_data) == 1344, "vfs_superblock.fs_data offset");
    assert!(
        core::mem::offset_of!(VfsSuperblock, mount_count) == 1352,
        "vfs_superblock.mount_count offset"
    );
    assert!(core::mem::offset_of!(VfsSuperblock, refcount) == 1356, "vfs_superblock.refcount offset");
    assert!(
        core::mem::offset_of!(VfsSuperblock, orphan_count) == 1360,
        "vfs_superblock.orphan_count offset"
    );
    assert!(
        core::mem::offset_of!(VfsSuperblock, orphan_list) == 1368,
        "vfs_superblock.orphan_list offset"
    );
    assert!(core::mem::offset_of!(VfsSuperblock, spinlock) == 1408, "vfs_superblock.spinlock offset");
    assert!(
        core::mem::offset_of!(VfsSuperblock, block_size) == 1432,
        "vfs_superblock.block_size offset"
    );
    assert!(
        core::mem::offset_of!(VfsSuperblock, total_blocks) == 1440,
        "vfs_superblock.total_blocks offset"
    );
    assert!(
        core::mem::offset_of!(VfsSuperblock, used_blocks) == 1448,
        "vfs_superblock.used_blocks offset"
    );
};
use crate::proc::proc_shims::{xv6_current_thread, xv6_panic};

// ===========================================================================
// Externs — every cross-module C-ABI symbol this file calls, declared
// locally per this crate's established convention (see `vfs/inode.rs`'s
// externs-block doc). Nearly everything is `safe`: passing a raw pointer
// is never itself unsafe, and every entry point here either null-checks
// internally or is called only with pointers this file already proved
// non-null. `printf` (C-variadic) is the one exception.
// ===========================================================================

// P3-D2a: `scheduler_yield` (proc/sched.rs) is a plain crate-path item
// now that its `extern "C"` redeclaration is gone.
use crate::proc::scheduler_yield;
// P3-D3b: lock/rwsem.rs's entry points (used for `vfs_superblock.lock`)
// are plain safe Rust fns now that their `#[no_mangle]` exports are gone;
// reached by crate path.
use crate::lock::rwsem::{
    rwsem_acquire_read, rwsem_acquire_write, rwsem_init, rwsem_is_write_holding, rwsem_release,
};
// P3-D3b: same for lock/mutex.rs's entry points (inode mutexes and
// `__MOUNT_MUTEX`).
use crate::lock::mutex::{holding_mutex, mutex_init, mutex_lock, mutex_unlock};

// P3-D3c: the spinlock primitives (`vfs_superblock.spinlock`,
// `fs_struct.lock`) are genuinely `unsafe fn`s in `crate::lock::spinlock`
// now that their `#[no_mangle]` exports are gone; thin wrappers preserve
// the `safe fn` facade the old redeclarations asserted.
/// SAFETY: see [`crate::lock::spinlock::spin_init`]'s contract.
fn spin_init(l: *mut spinlock_t, name: *mut c_char) {
    unsafe { crate::lock::spinlock::spin_init(l, name) }
}
/// SAFETY: see [`crate::lock::spinlock::spin_lock`]'s contract.
fn spin_lock(l: *mut spinlock_t) {
    unsafe { crate::lock::spinlock::spin_lock(l) }
}
/// SAFETY: see [`crate::lock::spinlock::spin_unlock`]'s contract.
fn spin_unlock(l: *mut spinlock_t) {
    unsafe { crate::lock::spinlock::spin_unlock(l) }
}

unsafe extern "C" {
    // printf.rs — C-variadic.

    // string.rs.
    safe fn strlen(s: *const c_char) -> usize;
    safe fn strndup(s: *const c_char, n: usize) -> *mut c_char;
    safe fn strncmp(p: *const c_char, q: *const c_char, n: usize) -> c_int;

}

// P3-D3c: the hlist and kobject primitives are genuinely `unsafe fn`s in
// `crate::{hlist,kobject}` now that their `#[no_mangle]` exports are gone;
// this file's original extern declarations asserted `safe fn` (usual
// FFI-facade convention). Thin wrappers preserve that safe facade for the
// unchanged call sites (`hlist_t`/`hlist_func_t`/`kobject` are the same
// bindgen types as the owners' `Hlist`/`HlistFunc`/`Kobject` aliases).
// The `static inline` bucket/entry walkers (`hlist_first_entry`,
// `hlist_next_entry`, `HLIST_FIRST_NODE`, ...) have no external linkage
// in the C original and are reimplemented below, specialized to
// `vfs_superblock.inodes`/`.inodes_buckets`.
/// SAFETY: see [`crate::hlist::hlist_init`]'s contract.
fn hlist_init(hlist: *mut hlist_t, bucket_cnt: u64, func: *mut hlist_func_t) -> i32 {
    unsafe { crate::hlist::hlist_init(hlist, bucket_cnt, func) }
}
/// SAFETY: see [`crate::hlist::hlist_get`]'s contract.
fn hlist_get(hlist: *mut hlist_t, node: *mut c_void) -> *mut c_void {
    unsafe { crate::hlist::hlist_get(hlist, node) }
}
/// SAFETY: see [`crate::hlist::hlist_put`]'s contract.
fn hlist_put(hlist: *mut hlist_t, node: *mut c_void, replace: bool) -> *mut c_void {
    unsafe { crate::hlist::hlist_put(hlist, node, replace) }
}
/// SAFETY: see [`crate::hlist::hlist_pop`]'s contract.
fn hlist_pop(hlist: *mut hlist_t, node: *mut c_void) -> *mut c_void {
    unsafe { crate::hlist::hlist_pop(hlist, node) }
}
/// SAFETY: see [`crate::hlist::hlist_len`]'s contract.
fn hlist_len(hlist: *mut hlist_t) -> usize {
    unsafe { crate::hlist::hlist_len(hlist) }
}
/// SAFETY: see [`crate::kobject::kobject_init`]'s contract.
fn kobject_init(obj: *mut kobject) {
    unsafe { crate::kobject::kobject_init(obj) }
}
/// SAFETY: see [`crate::kobject::kobject_get`]'s contract.
fn kobject_get(obj: *mut kobject) {
    unsafe { crate::kobject::kobject_get(obj) }
}
/// SAFETY: see [`crate::kobject::kobject_put`]'s contract.
fn kobject_put(obj: *mut kobject) {
    unsafe { crate::kobject::kobject_put(obj) }
}

// P3-D3b: proc/workqueue.rs's entry points (the deferred-iput workqueue)
// are plain safe Rust fns now that their `#[no_mangle]` exports are
// gone; reached via the `crate::proc` glob re-export.
use crate::proc::{create_work_struct, free_work_struct, queue_work, workqueue_create};

// P3-D3a: the slab entry points are genuinely `unsafe fn` in
// `crate::mm::slab` now that their `#[no_mangle]` exports are gone; this
// file's original extern declarations asserted `safe fn` (usual FFI
// facade) and typed the cache pointer as the bindgen `slab_cache_t`
// rather than `crate::mm::slab::SlabCache` (same layout — see
// `cffi::raw`'s identical note). Thin cast + safe-facade wrappers
// preserve both.
/// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract.
#[inline]
fn slab_cache_init(
    cache: *mut slab_cache_t, name: *mut c_char, obj_size: usize, flags: u64,
) -> c_int {
    unsafe {
        crate::mm::slab_cache_init(cache as *mut crate::mm::slab::SlabCache, name, obj_size, flags)
    }
}
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    unsafe { crate::mm::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
}
/// SAFETY: `obj` must originate from `slab_alloc` above.
#[inline]
fn slab_free(obj: *mut c_void) {
    unsafe { crate::mm::slab_free(obj) };
}
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_cache_shrink(cache: *mut slab_cache_t, nums: c_int) -> c_int {
    unsafe { crate::mm::slab_cache_shrink(cache as *mut crate::mm::slab::SlabCache, nums) }
}

// `kmm_free` is genuinely `unsafe fn` in `crate::mm::kalloc`;
// `cffi::raw`'s existing thin safe wrapper (identical signature)
// preserves the `safe fn` facade the old redeclaration asserted.
use crate::mm::cffi::raw::kmm_free;

// P3-1C mesh sweep: every submodule below (`inode`/`file`/`fdtable`/
// `tmpfs`/`xv6fs`/`devtmpfs`/`vfs_syscall`) is in scope for this wave, so
// these become plain crate-path imports instead of `extern "C"`
// redeclarations (all identical signatures -- same `crate::bindings::*`
// types this file already imports).
use crate::vfs::devtmpfs::superblock::{devtmpfs_init, devtmpfs_post_mount_populate};
use crate::vfs::fdtable::{__vfs_fdtable_global_init, vfs_fdtable_init};
use crate::vfs::file::{__vfs_file_init, __vfs_file_shrink_cache};
use crate::vfs::inode::{
    __vfs_inode_init, inode_ops, vfs_chroot, vfs_idup, vfs_idup_not_zero, vfs_ilock, vfs_iunlock,
    vfs_iput, vfs_mkdir, vfs_namei,
};
use crate::vfs::tmpfs::superblock::{tmpfs_init, tmpfs_mount_root};
use crate::vfs::vfs_syscall::vfs_mount_path;
use crate::vfs::xv6fs::superblock::{xv6fs_init, xv6fs_mount_root};

// P3-9c: `KArc<vfs_fs_type>` -- see `kernel/kobject.rs`'s `HasKobject`/
// `KArc` doc and the `dev/dev.rs` (`device_t`) / `dev/bio.rs` (`bio`)
// precedents. Used by `vfs_mount` below to replace its manual
// `vfs_get_fs_type`/two-conditional-`vfs_put_fs_type` pairing.
use crate::kobject::{HasKobject, KArc, Kobject};

// `kassert!`'s canonical home is crate root / `crate::kstd` (P3-CS2
// centralization).
use crate::kassert;

// ===========================================================================
// Small helpers: negative-errno constants, ERR_PTR family, mode bits,
// CLONE_FS, list/hlist `static inline` reimplementations.
// ===========================================================================

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

// `err_ptr`/`is_err`/`is_err_or_null`/`ptr_err`'s canonical home is
// `crate::kstd` (P3-CS2 centralization). Note `kstd::ptr_err` returns
// `c_int`, not `isize` — every call site below already casts its result
// to `c_int`/`u64` or compares it against another `c_int` (`neg(...)`),
// so the narrower return type is a no-op change (see `is_eagain_ptr`,
// whose `as isize` comparison cast is dropped accordingly).
use crate::kstd::{is_err_or_null, result_to_errptr, Errno, KResult};

// `uabi/stat.h`'s `S_IF*`/`S_IS*` macros (full set — `__inode_mode_str`
// touches every file type).
const S_IFMT: u32 = 0o170000;
const S_IFIFO: u32 = 0o010000;
const S_IFCHR: u32 = 0o020000;
const S_IFDIR: u32 = 0o040000;
const S_IFBLK: u32 = 0o060000;
const S_IFREG: u32 = 0o100000;
const S_IFLNK: u32 = 0o120000;
const S_IFSOCK: u32 = 0o140000;
#[inline(always)]
fn is_dir(mode: u32) -> bool {
    mode & S_IFMT == S_IFDIR
}
#[inline(always)]
fn is_chr(mode: u32) -> bool {
    mode & S_IFMT == S_IFCHR
}
#[inline(always)]
fn is_blk(mode: u32) -> bool {
    mode & S_IFMT == S_IFBLK
}
#[inline(always)]
fn is_fifo(mode: u32) -> bool {
    mode & S_IFMT == S_IFIFO
}
#[inline(always)]
fn is_sock(mode: u32) -> bool {
    mode & S_IFMT == S_IFSOCK
}
#[inline(always)]
fn is_reg(mode: u32) -> bool {
    mode & S_IFMT == S_IFREG
}
#[inline(always)]
fn is_lnk(mode: u32) -> bool {
    mode & S_IFMT == S_IFLNK
}

/// `uabi/clone_flags.h`'s `CLONE_FS`.
const CLONE_FS: u64 = 0x00200000;

// ---------------------------------------------------------------------------
// `list.h` `static inline` primitives (no external linkage in the C
// original — reimplemented natively, same precedent as `inode.rs`'s
// `ln_init`/`ln_detach` and `file.rs`'s identical helpers).
// ---------------------------------------------------------------------------

/// Mirrors `list_entry_init()`.
///
/// # Safety
/// `head` must point to a live, writable `list_node_t`.
#[inline(always)]
unsafe fn ln_init(head: *mut list_node_t) {
    unsafe {
        (*head).next = head;
        (*head).prev = head;
    }
}

/// Mirrors `list_entry_push_back()`/`list_node_push_back()`.
///
/// # Safety
/// `head` must point to a live, circular `list_node_t` list head; `node`
/// must not already be linked into any list.
#[inline(always)]
unsafe fn ln_push_back(head: *mut list_node_t, node: *mut list_node_t) {
    unsafe {
        let tail = (*head).prev;
        (*node).prev = tail;
        (*node).next = head;
        (*tail).next = node;
        (*head).prev = node;
    }
}

/// Mirrors `list_entry_push_front()`/`list_node_push_front()`.
///
/// # Safety
/// Same as [`ln_push_back`].
#[inline(always)]
unsafe fn ln_push_front(head: *mut list_node_t, node: *mut list_node_t) {
    unsafe {
        let first = (*head).next;
        (*node).prev = head;
        (*node).next = first;
        (*first).prev = node;
        (*head).next = node;
    }
}

/// Mirrors `list_entry_detach()`/`list_node_detach()`.
///
/// # Safety
/// `node` must point to a live, linked `list_node_t`.
#[inline(always)]
unsafe fn ln_detach(node: *mut list_node_t) {
    unsafe {
        let prev = (*node).prev;
        let next = (*node).next;
        (*prev).next = next;
        (*next).prev = prev;
        (*node).prev = node;
        (*node).next = node;
    }
}

// ---------------------------------------------------------------------------
// `hlist.h` `static inline` bucket/entry walkers, specialized to
// `vfs_superblock.{inodes,inodes_buckets}` (the native `VfsSuperblock`'s
// flattened anonymous struct, P3-N5). Unlike the general `hlist_t`
// (whose C `buckets` field is a flexible-array member that
// `kernel/hlist.rs`'s private `bucket_at` reaches via raw pointer
// arithmetic past the header, not exported), the superblock's buckets are
// addressable as a plain fixed-size `[hlist_bucket_t; 61]` array field,
// so this file indexes that array
// directly instead of re-deriving the header-relative arithmetic.
// `hlist_entry_t.list_entry` and `vfs_inode.hash_entry` are both their
// container's first field (offset 0, verified against the bindgen
// layout), so `*mut list_node_t`/`*mut hlist_entry_t`/`*mut vfs_inode`
// are freely interconvertible here without `container_of` arithmetic;
// each cast below is commented with that invariant.
// ---------------------------------------------------------------------------

// P3-N5: aliased to the native `VfsSuperblock`'s own array bound (one
// definition of the value) instead of a second hardcoded 61.
const SB_HASH_BUCKETS: usize = VFS_SUPERBLOCK_HASH_BUCKETS;

/// # Safety
/// `sb` must point to a live, initialized `vfs_superblock`.
#[inline(always)]
unsafe fn sb_buckets_base(sb: *mut vfs_superblock) -> *mut hlist_bucket_t {
    unsafe { (&raw mut (*sb).inodes_buckets) as *mut hlist_bucket_t }
}

/// # Safety
/// `sb` must point to a live, initialized `vfs_superblock`; `idx` must be
/// `< SB_HASH_BUCKETS`.
#[inline(always)]
unsafe fn sb_bucket_at(sb: *mut vfs_superblock, idx: usize) -> *mut hlist_bucket_t {
    unsafe { sb_buckets_base(sb).add(idx) }
}

/// Mirrors `hlist_bucket_first_entry()`, returning the `vfs_inode`
/// directly (`hash_entry`/`list_entry` are both offset 0 — see the
/// section doc).
///
/// # Safety
/// `bucket` must point to a live, circular `hlist_bucket_t` list head.
#[inline(always)]
unsafe fn bucket_first_inode(bucket: *mut hlist_bucket_t) -> *mut vfs_inode {
    unsafe {
        let first = (*bucket).next;
        if first == bucket {
            ptr::null_mut()
        } else {
            first as *mut vfs_inode
        }
    }
}

/// Mirrors `hlist_first_entry()`/`HLIST_FIRST_NODE()`, specialized to
/// `sb->inodes`.
///
/// # Safety
/// `sb` must point to a live, initialized `vfs_superblock`.
unsafe fn sb_inodes_first(sb: *mut vfs_superblock) -> *mut vfs_inode {
    unsafe {
        for i in 0..SB_HASH_BUCKETS {
            let n = bucket_first_inode(sb_bucket_at(sb, i));
            if !n.is_null() {
                return n;
            }
        }
        ptr::null_mut()
    }
}

/// Mirrors `hlist_next_entry()`, specialized to `sb->inodes`.
///
/// # Safety
/// `sb` must point to a live, initialized `vfs_superblock`; `inode` must
/// be currently linked into `sb->inodes`.
unsafe fn sb_inodes_next(sb: *mut vfs_superblock, inode: *mut vfs_inode) -> *mut vfs_inode {
    unsafe {
        // `inode` reinterpreted as `*mut hlist_entry_t`/`*mut list_node_t`
        // (both offset 0 within their respective containers — see the
        // section doc).
        let entry = inode as *mut hlist_entry_t;
        let bucket = (*entry).bucket;
        if bucket.is_null() {
            return ptr::null_mut();
        }
        let node = inode as *mut list_node_t;
        let next = (*node).next;
        if next != bucket {
            return next as *mut vfs_inode;
        }
        let base = sb_buckets_base(sb);
        let idx = (bucket as usize - base as usize) / core::mem::size_of::<hlist_bucket_t>();
        for i in (idx + 1)..SB_HASH_BUCKETS {
            let n = bucket_first_inode(sb_bucket_at(sb, i));
            if !n.is_null() {
                return n;
            }
        }
        ptr::null_mut()
    }
}

// ===========================================================================
// Module statics (all file-private in the C original — no `#[no_mangle]`).
// ===========================================================================

static mut VFS_FS_TYPE_CACHE: slab_cache_t = unsafe { core::mem::zeroed() };
static mut VFS_SUPERBLOCK_CACHE: slab_cache_t = unsafe { core::mem::zeroed() };
static mut VFS_STRUCT_CACHE: slab_cache_t = unsafe { core::mem::zeroed() };
static mut __MOUNT_MUTEX: mutex_t = unsafe { core::mem::zeroed() };
static mut VFS_FS_TYPES: list_node_t = list_node_t { prev: ptr::null_mut(), next: ptr::null_mut() };
static mut VFS_FS_TYPE_COUNT: u16 = 0;
const MAX_FS_TYPES: u16 = 256;

/// Workqueue for deferred `vfs_iput()` operations. `vfs_iput()` can block
/// on the superblock wlock, inode mutex, and filesystem transactions; it
/// must not be called directly from RCU callbacks (deadlock risk against
/// threads waiting on the same locks for a grace period). RCU callbacks
/// queue work here instead.
static mut __VFS_DEFERRED_IPUT_WQ: *mut workqueue = ptr::null_mut();

/// The absolute VFS root inode: a synthesized sentinel with no
/// superblock, data, or operations, serving only as the top of the mount
/// tree. See the module doc for why its embedded lock types are never
/// `mutex_init`-ed.
pub(crate) static mut vfs_root_inode: vfs_inode = unsafe { core::mem::zeroed() };

// ===========================================================================
// Superblock inode-hash function table (`__vfs_superblock_inode_*` in the
// C original — `static`, internal linkage, referenced only by address).
// ===========================================================================

unsafe extern "C" fn sb_inode_hash_fn(node: *mut c_void) -> u64 {
    // SAFETY: `node` is always a live `*mut vfs_inode` when the hash
    // table invokes this (the crate-wide hlist contract).
    unsafe { hlist_hash_uint64((*(node as *mut vfs_inode)).ino) }
}

unsafe extern "C" fn sb_inode_cmp_fn(
    _hlist: *mut hlist_t,
    node: *mut c_void,
    key: *mut c_void,
) -> c_int {
    // SAFETY: same contract as `sb_inode_hash_fn`.
    unsafe {
        let a = (*(node as *mut vfs_inode)).ino;
        let b = (*(key as *mut vfs_inode)).ino;
        if a > b {
            1
        } else if a < b {
            -1
        } else {
            0
        }
    }
}

unsafe extern "C" fn sb_inode_get_node_fn(entry: *mut hlist_entry_t) -> *mut c_void {
    // `hash_entry` is `vfs_inode`'s first field (offset 0).
    if entry.is_null() {
        ptr::null_mut()
    } else {
        entry as *mut c_void
    }
}

unsafe extern "C" fn sb_inode_get_entry_fn(node: *mut c_void) -> *mut hlist_entry_t {
    if node.is_null() {
        ptr::null_mut()
    } else {
        node as *mut hlist_entry_t
    }
}

static SB_INODE_HLIST_FUNCS: hlist_func_t = hlist_func_t {
    hash: Some(sb_inode_hash_fn),
    get_node: Some(sb_inode_get_node_fn),
    get_entry: Some(sb_inode_get_entry_fn),
    cmp_node: Some(sb_inode_cmp_fn),
};

/// Mirrors `hlist_hash_uint64()` (`kernel/inc/hlist.h`, `static inline`,
/// no external linkage).
#[inline(always)]
fn hlist_hash_uint64(key: u64) -> u64 {
    const GOLDEN_RATIO_PRIME_64: u64 = 0x9e37fffffffc0001;
    let ret = key.wrapping_mul(GOLDEN_RATIO_PRIME_64);
    if ret == 0 {
        GOLDEN_RATIO_PRIME_64
    } else {
        ret
    }
}

// ---------------------------------------------------------------------------
// Atomics — mirrors `smp/atomic.h`'s `atomic_oper_cond` family and plain
// `__atomic_*` builtins exactly (see `inode.rs`'s identical helpers and
// their ordering rationale: `Ordering::SeqCst` CAS over an
// `Ordering::Acquire` initial load, matching `atomic_oper_cond`
// verbatim — a line-faithful port, not an independent re-derivation of
// weaker orderings).
// ---------------------------------------------------------------------------

/// # Safety
/// `sb` must point to a live, aligned `vfs_superblock`.
#[inline(always)]
unsafe fn sb_refcount_atomic<'a>(sb: *mut vfs_superblock) -> &'a AtomicI32 {
    unsafe { &*(ptr::addr_of_mut!((*sb).refcount) as *const AtomicI32) }
}
/// # Safety
/// `sb` must point to a live, aligned `vfs_superblock`.
#[inline(always)]
unsafe fn sb_mountcount_atomic<'a>(sb: *mut vfs_superblock) -> &'a AtomicI32 {
    unsafe { &*(ptr::addr_of_mut!((*sb).mount_count) as *const AtomicI32) }
}
/// # Safety
/// `inode` must point to a live, aligned `vfs_inode`.
#[inline(always)]
unsafe fn inode_refcount_atomic<'a>(inode: *mut vfs_inode) -> &'a AtomicI32 {
    unsafe { &*(ptr::addr_of_mut!((*inode).ref_count) as *const AtomicI32) }
}
/// # Safety
/// `fs` must point to a live, aligned `fs_struct`.
#[inline(always)]
unsafe fn fs_refcount_atomic<'a>(fs: *mut fs_struct) -> &'a AtomicI32 {
    unsafe { &*(ptr::addr_of_mut!((*fs).ref_count) as *const AtomicI32) }
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

/// Mirrors `fs.h`'s `static inline vfs_inode_refcount()`.
///
/// # Safety
/// `inode` must be null or a live, aligned `vfs_inode`.
unsafe fn inode_refcount(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() {
        return -1;
    }
    unsafe { inode_refcount_atomic(inode).load(Ordering::SeqCst) }
}

/// Mirrors `fs.h`'s `static inline vfs_superblock_mountcount()`.
///
/// # Safety
/// `sb` must be null or a live, aligned `vfs_superblock`.
unsafe fn superblock_mountcount(sb: *mut vfs_superblock) -> c_int {
    if sb.is_null() {
        return -1;
    }
    unsafe { sb_mountcount_atomic(sb).load(Ordering::SeqCst) }
}

/// Mirrors `fs.h`'s `static inline vfs_inode_is_local_root()`.
///
/// N-R5 (VFS-Arc diagnostic pilot): takes a borrowed `&vfs_inode` instead
/// of a raw `*mut vfs_inode`. The sole caller (`turn_mountpoint`) holds a
/// live, mutex-locked, non-null inode, so the borrow is sound and the
/// null branch the raw form carried collapses (a reference is never null).
/// Only the **set-once** `sb`/`parent` fields are touched here — no Freeze
/// field (`flags`/`ref_count`/…), no loop — so this reference is free of
/// the freeze-noalias hazard that gates the flags-reading paths.
fn inode_is_local_root(inode: &vfs_inode) -> bool {
    if inode.sb.is_null() {
        return false;
    }
    // SAFETY: `sb` is a live superblock (just null-checked); `root_inode`
    // is a set-once pointer field. Only a raw-pointer identity compare
    // against the borrowed inode's address follows — no aliasing/mutation.
    let root = unsafe { (*inode.sb).root_inode };
    if ptr::eq(inode, root) {
        kassert!(
            ptr::eq(inode.parent, inode),
            "vfs_inode_is_local_root: root inode's parent is not itself"
        );
        return true;
    }
    false
}

// ---------------------------------------------------------------------------
// `vfs_private.h`'s `static inline` validity checks (no external linkage
// in the C original).
// ---------------------------------------------------------------------------

/// Mirrors `vfs_private.h`'s `static inline __vfs_inode_valid()`.
///
/// N-R5b (VFS-Arc reference conversion): takes a borrowed `&vfs_inode`
/// instead of a raw `*mut vfs_inode`. Every caller (`vfs_unmount`,
/// `vfs_unmount_lazy`) null-checks its inode and holds the inode mutex
/// before calling, so the borrow is sound and the raw form's null branch
/// collapses (a reference is never null). The fields read here are all
/// now safe through a shared borrow: the `mutex` is reached only via an
/// atomic holder load (`holding_mutex`), `flags.valid()` is a relaxed
/// atomic load (a3a400b), and `sb` is a set-once pointer. The borrow is
/// ephemeral (dropped at return), so it never races a later mutation of
/// the same inode. No non-atomic inode field is read in a loop — free of
/// the freeze-noalias hazard.
fn inode_valid(inode: &vfs_inode) -> c_int {
    // `holding_mutex` reads the mutex holder via a SeqCst atomic load; a
    // raw pointer derived from the shared borrow of the interior-
    // synchronized `mutex` field is sound to hand it.
    if holding_mutex(&raw const inode.mutex as *mut _) == 0 {
        return neg(EPERM);
    }
    if inode.flags.valid() == 0 {
        return neg(EINVAL);
    }
    if !ptr::eq(inode, &raw const vfs_root_inode) {
        let sb = inode.sb;
        // SAFETY: `sb` is the inode's set-once superblock pointer; when
        // non-null it is a live `vfs_superblock` whose `flags` is an
        // atomic bitset read via the accessor's relaxed load.
        if sb.is_null() || unsafe { (*sb).flags.valid() } == 0 {
            crate::kprintln!("__vfs_inode_valid: inode's superblock is not valid");
            return neg(EINVAL);
        }
    }
    0
}

/// Mirrors `vfs_private.h`'s `static inline __vfs_dir_inode_valid_holding()`.
///
/// N-R5b (VFS-Arc reference conversion): borrowed `&vfs_inode` form — the
/// directory-typed sibling of [`inode_valid`]. Both callers (`vfs_mount`,
/// `vfs_get_mnt_rooti`) null-check and hold the inode mutex first, so the
/// ephemeral borrow is sound. Reads only the now-safe set: the `mutex`
/// (via atomic holder load), `flags.valid()` (relaxed atomic), the
/// set-once `mode`/`sb`. No loop, no mutation — freeze-noalias-free.
fn dir_inode_valid_holding(inode: &vfs_inode) -> c_int {
    if holding_mutex(&raw const inode.mutex as *mut _) == 0 {
        return neg(EPERM);
    }
    if inode.flags.valid() == 0 {
        return neg(EINVAL);
    }
    if !is_dir(inode.mode) {
        return neg(EINVAL);
    }
    if !ptr::eq(inode, &raw const vfs_root_inode) {
        let sb = inode.sb;
        // SAFETY: `sb` is the inode's set-once superblock pointer; when
        // non-null it is a live `vfs_superblock` whose `flags` is an
        // atomic bitset read via the accessor's relaxed load.
        if sb.is_null() || unsafe { (*sb).flags.valid() } == 0 {
            return neg(EINVAL);
        }
    }
    0
}

/******************************************************************************
 * Private functions
 *****************************************************************************/

/// Mirrors `__vfs_rooti_init()`.
unsafe fn vfs_rooti_init() {
    unsafe {
        vfs_root_inode = core::mem::zeroed();
        vfs_root_inode.mode = S_IFDIR | 0o755;
        vfs_root_inode.flags.set_valid(1);
    }
}

/// Mirrors `__vfs_register_fs_type_locked()`.
unsafe fn register_fs_type_locked(fs_type: *mut vfs_fs_type) {
    unsafe {
        ln_push_back(&raw mut VFS_FS_TYPES, &raw mut (*fs_type).list_entry);
        (*fs_type).flags.set_registered(1);
        VFS_FS_TYPE_COUNT += 1;
        kassert!(
            VFS_FS_TYPE_COUNT <= MAX_FS_TYPES,
            "Exceeded maximum filesystem types"
        );
    }
}

/// Mirrors `__vfs_unregister_fs_type_locked()`.
unsafe fn unregister_fs_type_locked(fs_type: *mut vfs_fs_type) {
    unsafe {
        ln_detach(&raw mut (*fs_type).list_entry);
        (*fs_type).flags.set_registered(0);
        VFS_FS_TYPE_COUNT -= 1;
        kassert!(
            VFS_FS_TYPE_COUNT <= MAX_FS_TYPES,
            "Filesystem types count underflow"
        );
    }
}

/// Mirrors `__vfs_get_fs_type_locked()`. `vfs_fs_type.list_entry` is the
/// struct's first field (offset 0), so the walk casts `list_node_t`
/// pointers to `*mut vfs_fs_type` directly.
unsafe fn get_fs_type_locked(name: *const c_char) -> *mut vfs_fs_type {
    // SAFETY: `name` is a caller-provided C string; `strlen` reads it.
    let name_len = unsafe { strlen(name) };
    let head = &raw mut VFS_FS_TYPES;
    // N-I1: the raw `(*pos).next` chase is replaced by the safe
    // `ListIterator` (via `list_for_each!`). `vfs_fs_type.list_entry` is at
    // offset 0 (asserted above); the walk runs under the fs-types lock the
    // caller already holds, so no node is inserted/removed under it — a
    // class-(A) read-only walk. Only the per-item field read stays `unsafe`
    // (the item-deref floor: `ListIterator` yields `*mut vfs_fs_type`).
    crate::list_for_each!(head, offset_of!(vfs_fs_type, list_entry), node, {
        let fs_type: *mut vfs_fs_type = node;
        // SAFETY: `fs_type` is a live `vfs_fs_type` linked on `VFS_FS_TYPES`
        // via its offset-0 `list_entry`; `(*fs_type).name` is a stable C
        // string set at registration. `strncmp` reads both C strings.
        if unsafe { strncmp((*fs_type).name, name, name_len) } == 0 {
            return fs_type;
        }
    });
    ptr::null_mut()
}

// `vfs_fs_type.kobj` is *not* the struct's first field (`list_entry` and
// `superblocks` precede it -- see `kernel/inc/vfs/vfs_types.h`), unlike
// `device_t`/`bio`'s offset-0 layout, so this is a genuine field
// projection (`&raw mut (*this).kobj`), not a reinterpret cast.
// SAFETY: `vfs_fs_type.kobj` is a stable, non-moving field,
// `kobject_init`-ed by `vfs_register_fs_type` before any pointer to the
// `vfs_fs_type` is ever handed out via `vfs_get_fs_type`, and live for as
// long as any reference is held (`vfs_fs_type_kobj_release`/
// `kobject_put` is the only thing that can invalidate it, and only once
// the count reaches zero) -- exactly `HasKobject`'s contract.
unsafe impl HasKobject for vfs_fs_type {
    fn kobj_ptr(this: *mut Self) -> *mut Kobject {
        unsafe { &raw mut (*this).kobj }
    }
}

/// Mirrors `__vfs_fs_type_kobj_release()`.
unsafe extern "C" fn vfs_fs_type_kobj_release(kobj: *mut kobject) {
    unsafe {
        let fs_type = crate::mm::cffi::container_of::<vfs_fs_type, kobject>(
            kobj,
            offset_of!(vfs_fs_type, kobj),
        );
        slab_free(fs_type as *mut c_void);
    }
}

/// Mirrors `__vfs_inode_hash_get()`.
unsafe fn inode_hash_get(sb: *mut vfs_superblock, ino: u64) -> *mut vfs_inode {
    unsafe {
        let mut key: vfs_inode = core::mem::zeroed();
        key.ino = ino;
        hlist_get(
            &raw mut (*sb).inodes,
            (&raw mut key) as *mut c_void,
        ) as *mut vfs_inode
    }
}

/// Mirrors `__vfs_inode_hash_add()`.
unsafe fn inode_hash_add(sb: *mut vfs_superblock, inode: *mut vfs_inode) -> *mut vfs_inode {
    unsafe {
        hlist_put(
            &raw mut (*sb).inodes,
            inode as *mut c_void,
            false,
        ) as *mut vfs_inode
    }
}

/// Mirrors `__vfs_init_superblock_structure()`.
unsafe fn init_superblock_structure(sb: *mut vfs_superblock, fs_type: *mut vfs_fs_type) {
    unsafe {
        ln_init(&raw mut (*sb).siblings);
        ln_init(&raw mut (*sb).orphan_list);
        hlist_init(
            &raw mut (*sb).inodes,
            SB_HASH_BUCKETS as u64,
            &SB_INODE_HLIST_FUNCS as *const hlist_func_t as *mut hlist_func_t,
        );
        (*sb).fs_type = fs_type;
        (*sb).orphan_count = 0;
        sb_refcount_atomic(sb).store(0, Ordering::SeqCst);
        sb_mountcount_atomic(sb).store(0, Ordering::SeqCst);
        rwsem_init(
            &raw mut (*sb).lock,
            RWLOCK_PRIO_READ as u64,
            c"vfs_superblock_lock".as_ptr(),
        );
        spin_init(
            &raw mut (*sb).spinlock,
            c"vfs_superblock_spinlock".as_ptr() as *mut c_char,
        );
    }
}

/// Mirrors `__vfs_init_sb_rooti()`.
unsafe fn init_sb_rooti(sb: *mut vfs_superblock) -> c_int {
    unsafe {
        __vfs_inode_init((*sb).root_inode);
        loop {
            // P3-10b: `KResult`-native insert (the old `ERR_PTR`
            // consumption and its dead null -> `ENOENT` branch are
            // gone; `Errno::Again` drives the same retry).
            let inode = match vfs_add_inode_inner(sb, (*sb).root_inode) {
                Ok(i) => i,
                Err(Errno::Again) => {
                    vfs_superblock_unlock(sb);
                    scheduler_yield();
                    vfs_superblock_wlock(sb);
                    if (*sb).flags.valid() == 0 && (*sb).flags.initialized() != 0 {
                        return neg(EINVAL);
                    }
                    continue;
                }
                Err(e) => return e.neg(),
            };
            if inode != (*sb).root_inode {
                vfs_iunlock(inode);
                return neg(EEXIST);
            }
            (*(*sb).root_inode).parent = (*sb).root_inode;
            vfs_iunlock((*sb).root_inode);
            return 0;
        }
    }
}

/// Mirrors `__vfs_superblock_ops_valid()`. The old per-slot checks
/// (`alloc_inode`/`get_inode`/`sync_fs`/`unmount_begin` non-null)
/// collapsed into the [`SuperblockOps`] trait's required methods
/// (P3-10b) — presence of the table is the only remaining question.
unsafe fn superblock_ops_valid(sb: *mut vfs_superblock) -> bool {
    unsafe { (*sb).ops.is_some() }
}

/// Mirrors `__vfs_init_superblock_valid()`.
unsafe fn init_superblock_valid(sb: *mut vfs_superblock) -> bool {
    unsafe {
        if sb.is_null() {
            return false;
        }
        if (*sb).flags.valid() != 0 || (*sb).flags.dirty() != 0 {
            return false;
        }
        if !superblock_ops_valid(sb) {
            return false;
        }
        if !(*sb).mountpoint.is_null() || !(*sb).parent_sb.is_null() {
            return false;
        }
        true
    }
}

/// Mirrors `__vfs_attach_superblock_to_fstype()`. `vfs_superblock.siblings`
/// is the struct's first field (offset 0).
unsafe fn attach_superblock_to_fstype(sb: *mut vfs_superblock) {
    unsafe {
        let fs_type = (*sb).fs_type;
        ln_push_front(&raw mut (*fs_type).superblocks, &raw mut (*sb).siblings);
        (*fs_type).sb_count += 1;
        (*sb).flags.set_registered(1);
        kassert!(
            (*fs_type).sb_count > 0,
            "Filesystem type superblock count overflow"
        );
    }
}

/// Mirrors `__vfs_detach_superblock_from_fstype()`.
unsafe fn detach_superblock_from_fstype(sb: *mut vfs_superblock) {
    unsafe {
        ln_detach(&raw mut (*sb).siblings);
        let fs_type = (*sb).fs_type;
        (*fs_type).sb_count -= 1;
        (*sb).flags.set_registered(0);
        kassert!(
            (*fs_type).sb_count >= 0,
            "Filesystem type superblock count underflow"
        );
    }
}

/// Mirrors `__vfs_turn_mountpoint()`.
unsafe fn turn_mountpoint(mountpoint: *mut vfs_inode) -> c_int {
    unsafe {
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            kassert!(
                rwsem_is_write_holding(&raw mut (*(*mountpoint).sb).lock),
                "Mountpoint inode's superblock lock must be write held to turn into mountpoint"
            );
        }
        kassert!(
            holding_mutex(&raw mut (*mountpoint).mutex) != 0,
            "Mountpoint inode lock must be held to turn into mountpoint"
        );
        if inode_refcount(mountpoint) > 2 {
            return neg(EBUSY);
        }
        if !is_dir((*mountpoint).mode) {
            return neg(ENOTDIR);
        }
        if inode_is_local_root(&*mountpoint) {
            return neg(EBUSY);
        }
        if (*mountpoint).flags.mount() != 0 {
            return neg(EBUSY);
        }
        (*mountpoint).flags.set_mount(1);
        (*mountpoint).dev_mnt.mnt.mnt_rooti = ptr::null_mut();
        (*mountpoint).dev_mnt.mnt.mnt_sb = ptr::null_mut();
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            vfs_superblock_mountcount_inc((*mountpoint).sb);
            vfs_idup(mountpoint);
        }
        0
    }
}

/// Mirrors `__vfs_set_mountpoint()`.
unsafe fn set_mountpoint(sb: *mut vfs_superblock, mountpoint: *mut vfs_inode) {
    unsafe {
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            kassert!(
                rwsem_is_write_holding(&raw mut (*(*mountpoint).sb).lock),
                "Mountpoint inode's superblock lock must be write held to set mountpoint"
            );
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "Superblock lock must be write held to set mountpoint"
        );
        kassert!(
            holding_mutex(&raw mut (*mountpoint).mutex) != 0,
            "Mountpoint inode lock must be held to set mountpoint"
        );
        kassert!(
            (*mountpoint).flags.mount() != 0,
            "Mountpoint inode is not marked as a mountpoint"
        );
        kassert!((*sb).mountpoint.is_null(), "Superblock mountpoint is already set");
        (*sb).mountpoint = mountpoint;
        (*sb).parent_sb = (*mountpoint).sb;
        (*mountpoint).dev_mnt.mnt.mnt_sb = sb;
        (*mountpoint).dev_mnt.mnt.mnt_rooti = (*sb).root_inode;
    }
}

/// Mirrors `__vfs_clear_mountpoint()`.
unsafe fn clear_mountpoint(mountpoint: *mut vfs_inode) {
    unsafe {
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            kassert!(
                rwsem_is_write_holding(&raw mut (*(*mountpoint).sb).lock),
                "Mountpoint inode's superblock lock must be write held to clear mountpoint"
            );
        }
        kassert!(
            holding_mutex(&raw mut (*mountpoint).mutex) != 0,
            "Mountpoint inode lock must be held to clear mountpoint"
        );
        kassert!(
            (*mountpoint).flags.mount() != 0,
            "Mountpoint inode type is not MNT"
        );
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            vfs_superblock_mountcount_dec((*mountpoint).sb);
        }
        (*mountpoint).dev_mnt.mnt.mnt_sb = ptr::null_mut();
        (*mountpoint).dev_mnt.mnt.mnt_rooti = ptr::null_mut();
        (*mountpoint).flags.set_mount(0);
    }
}

const FS_STRUCT_LOCK_NAME: &[u8] = b"fs_struct_lock\0";

/// Mirrors `__vfs_struct_alloc_init()`.
unsafe fn struct_alloc_init() -> *mut fs_struct {
    unsafe {
        let fs = slab_alloc(&raw mut VFS_STRUCT_CACHE) as *mut fs_struct;
        if fs.is_null() {
            return ptr::null_mut();
        }
        ptr::write_bytes(fs, 0, 1);
        spin_init(&raw mut (*fs).lock, FS_STRUCT_LOCK_NAME.as_ptr() as *mut c_char);
        fs_refcount_atomic(fs).store(1, Ordering::Release);
        fs
    }
}

/// Mirrors `__vfs_struct_free()`.
unsafe fn struct_free(fs: *mut fs_struct) {
    unsafe { slab_free(fs as *mut c_void) };
}

/******************************************************************************
 * Files System Type / VFS init Public APIs
 *****************************************************************************/

/// Mirrors `vfs_init()`.
///
/// Locking: none.
pub(crate) extern "C" fn vfs_init() {
    unsafe {
        vfs_rooti_init();
        ln_init(&raw mut VFS_FS_TYPES);
        mutex_init(&raw mut __MOUNT_MUTEX, c"vfs_mount_mutex".as_ptr() as *mut c_char);
        __vfs_fdtable_global_init();
        let mut ret = slab_cache_init(
            &raw mut VFS_SUPERBLOCK_CACHE,
            c"vfs_superblock_cache".as_ptr() as *mut c_char,
            core::mem::size_of::<vfs_superblock>(),
            (crate::bindings::SLAB_FLAG_STATIC | crate::bindings::SLAB_FLAG_DEBUG_BITMAP) as u64,
        );
        kassert!(
            ret == 0,
            "Failed to initialize vfs_superblock_cache slab cache"
        );
        ret = slab_cache_init(
            &raw mut VFS_FS_TYPE_CACHE,
            c"vfs_fs_type_cache".as_ptr() as *mut c_char,
            core::mem::size_of::<vfs_fs_type>(),
            (crate::bindings::SLAB_FLAG_STATIC | crate::bindings::SLAB_FLAG_DEBUG_BITMAP) as u64,
        );
        kassert!(ret == 0, "Failed to initialize vfs_fs_type_cache slab cache");
        ret = slab_cache_init(
            &raw mut VFS_STRUCT_CACHE,
            c"vfs_struct_cache".as_ptr() as *mut c_char,
            core::mem::size_of::<fs_struct>(),
            (crate::bindings::SLAB_FLAG_STATIC | crate::bindings::SLAB_FLAG_DEBUG_BITMAP) as u64,
        );
        kassert!(ret == 0, "Failed to initialize vfs_struct_cache slab cache");
        VFS_FS_TYPE_COUNT = 0;

        // Single-threaded workqueue for deferred iput (max_active=1
        // serializes all deferred iputs, avoiding a thundering-herd on
        // vfs_superblock_wlock). Must happen before any file operation
        // that might use RCU.
        __VFS_DEFERRED_IPUT_WQ =
            workqueue_create(c"vfs_iput_wq".as_ptr(), 1);
        kassert!(!__VFS_DEFERRED_IPUT_WQ.is_null(), "Failed to create vfs_iput workqueue");

        let thread = xv6_current_thread();
        kassert!(!thread.is_null(), "vfs_init must be called from a thread context");
        __vfs_inode_init(&raw mut vfs_root_inode);
        __vfs_file_init();
        (*thread).fs = vfs_struct_init();
        (*thread).fdtable = vfs_fdtable_init();

        // Initialize filesystem types (registers them with VFS).
        tmpfs_init();
        xv6fs_init();
        devtmpfs_init();

        // Mount filesystems.
        tmpfs_mount_root();
        xv6fs_mount_root();

        // Mount tmpfs at /tmp (after chroot to xv6fs).
        ret = vfs_mount_path(c"tmpfs".as_ptr(), c"/tmp".as_ptr(), 4, ptr::null(), 0);
        if ret == 0 {
            crate::kprintln!("tmpfs: mounted at /tmp");
        } else if ret == neg(ENOENT) {
            crate::kprintln!("tmpfs: /tmp directory not found");
        } else {
            crate::kprintln!("tmpfs: failed to mount at /tmp, errno={}", ret);
        }

        // Mount devtmpfs at /dev (auto-populated with device nodes).
        // Ensure /dev directory exists (create it if not present on root fs).
        let mut dev_dir = vfs_namei(c"/dev".as_ptr(), 4);
        if is_err_or_null(dev_dir) {
            let root = vfs_namei(c"/".as_ptr(), 1);
            if !is_err_or_null(root) {
                // vfs_mkdir handles its own locking -- do NOT lock root first.
                dev_dir = vfs_mkdir(root, 0o755, c"dev".as_ptr(), 3);
                vfs_iput(root);
                if !is_err_or_null(dev_dir) {
                    vfs_iput(dev_dir);
                }
            }
        } else {
            vfs_iput(dev_dir);
        }
        ret = vfs_mount_path(c"devtmpfs".as_ptr(), c"/dev".as_ptr(), 4, ptr::null(), 0);
        if ret == 0 {
            crate::kprintln!("devtmpfs: mounted at /dev");
            // Now that the superblock & root inode are fully VFS-initialised,
            // populate the registered device nodes using VFS-level APIs.
            devtmpfs_post_mount_populate();

            // Pre-create /dev/pts directory for PTY slaves.
            let dev_inode = vfs_namei(c"/dev".as_ptr(), 4);
            if !is_err_or_null(dev_inode) {
                let pts_dir = vfs_mkdir(dev_inode, 0o755, c"pts".as_ptr(), 3);
                if !is_err_or_null(pts_dir) {
                    vfs_iput(pts_dir);
                }
                vfs_iput(dev_inode);
            }
        } else if ret == neg(ENOENT) {
            crate::kprintln!("devtmpfs: /dev directory not found");
        } else {
            crate::kprintln!("devtmpfs: failed to mount at /dev, errno={}", ret);
        }

        // Smoke tests: not carried over (dead code -- see module doc /
        // wave report; the C original's call sites here were already
        // commented out).
    }
}

/// Mirrors `vfs_get_deferred_iput_wq()`.
pub(crate) extern "C" fn vfs_get_deferred_iput_wq() -> *mut workqueue {
    unsafe { __VFS_DEFERRED_IPUT_WQ }
}

unsafe extern "C" fn iput_work_func(work: *mut work_struct) {
    unsafe {
        let inode = (*work).data as *mut vfs_inode;
        vfs_iput(inode);
        free_work_struct(work);
    }
}

/// Mirrors `__vfs_queue_deferred_iput()`.
unsafe fn queue_deferred_iput(inode: *mut vfs_inode) {
    unsafe {
        let wq = vfs_get_deferred_iput_wq();
        if wq.is_null() {
            vfs_iput(inode);
            return;
        }
        let work = create_work_struct(Some(iput_work_func), inode as u64);
        if work.is_null() {
            crate::kprintln!(
                "__vfs_queue_deferred_iput: failed to allocate work_struct, falling back to direct vfs_iput"
            );
            vfs_iput(inode);
            return;
        }
        queue_work(wq, work);
    }
}

/// Mirrors `__vfs_shrink_caches()`. Called from `tmpfs`/`xv6fs` smoketest
/// C code when checking for leaks; real external linkage in the C
/// original (declared in `vfs_private.h`, a shared header).
pub(crate) extern "C" fn __vfs_shrink_caches() {
    unsafe {
        slab_cache_shrink(&raw mut VFS_SUPERBLOCK_CACHE, 0x7fffffff);
        slab_cache_shrink(&raw mut VFS_FS_TYPE_CACHE, 0x7fffffff);
        __vfs_file_shrink_cache();
    }
}

/// Mirrors `vfs_fs_type_allocate()`.
///
/// Locking: none.
pub(crate) extern "C" fn vfs_fs_type_allocate() -> *mut vfs_fs_type {
    unsafe {
        let fs_type = slab_alloc(&raw mut VFS_FS_TYPE_CACHE) as *mut vfs_fs_type;
        if fs_type.is_null() {
            return ptr::null_mut();
        }
        ptr::write_bytes(fs_type, 0, 1);
        ln_init(&raw mut (*fs_type).list_entry);
        ln_init(&raw mut (*fs_type).superblocks);
        fs_type
    }
}

pub(crate) extern "C" fn vfs_fs_type_free(fs_type: *mut vfs_fs_type) {
    unsafe { slab_free(fs_type as *mut c_void) };
}

/// Mirrors `vfs_register_fs_type()`.
///
/// Locking: caller must hold the mount mutex via [`vfs_mount_lock`].
pub(crate) extern "C" fn vfs_register_fs_type(fs_type: *mut vfs_fs_type) -> c_int {
    unsafe {
        if holding_mutex(&raw mut __MOUNT_MUTEX) == 0 {
            return neg(EPERM);
        }
        // The old per-slot checks (`mount`/`free` non-null) collapsed
        // into the `FsTypeOps` trait's required methods (P3-10b) —
        // presence of the table is the only remaining question.
        if fs_type.is_null() || (*fs_type).name.is_null() || (*fs_type).ops.is_none() {
            return neg(EINVAL);
        }
        if (*fs_type).sb_count != 0 {
            return neg(EINVAL);
        }
        if (*fs_type).flags.registered() != 0 {
            return neg(EALREADY);
        }
        (*fs_type).kobj.ops.release = Some(vfs_fs_type_kobj_release);
        (*fs_type).kobj.name = c"fs_type".as_ptr();
        kobject_init(&raw mut (*fs_type).kobj);
        if VFS_FS_TYPE_COUNT >= MAX_FS_TYPES {
            return neg(ENOSPC);
        }
        let existing = get_fs_type_locked((*fs_type).name);
        if !existing.is_null() {
            return neg(EEXIST);
        }
        register_fs_type_locked(fs_type);
        0
    }
}

/// Mirrors `vfs_unregister_fs_type()`.
///
/// Locking: caller must hold the mount mutex via [`vfs_mount_lock`].
pub(crate) extern "C" fn vfs_unregister_fs_type(name: *const c_char) -> c_int {
    unsafe {
        if name.is_null() {
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut __MOUNT_MUTEX) == 0 {
            return neg(EPERM);
        }
        let pos = get_fs_type_locked(name);
        if !pos.is_null() {
            unregister_fs_type_locked(pos);
            kobject_put(&raw mut (*pos).kobj);
            return 0;
        }
        neg(ENOENT)
    }
}

/// Mirrors `vfs_mount_lock()`.
pub(crate) extern "C" fn vfs_mount_lock() {
    mutex_lock(unsafe { &raw mut __MOUNT_MUTEX });
}

/// Mirrors `vfs_mount_unlock()`.
pub(crate) extern "C" fn vfs_mount_unlock() {
    mutex_unlock(unsafe { &raw mut __MOUNT_MUTEX });
}

/******************************************************************************
 * Superblock Public APIs
 *****************************************************************************/

/// Mirrors `vfs_mount()`.
pub(crate) extern "C" fn vfs_mount(
    type_: *const c_char,
    mountpoint: *mut vfs_inode,
    device: *mut vfs_inode,
    flags: c_int,
    data: *const c_char,
) -> c_int {
    unsafe {
        let mut fs_type: *mut vfs_fs_type = ptr::null_mut();
        // P3-9c: owns the reference `vfs_get_fs_type` acquires below (if
        // any is ever acquired -- `None` on every early-return path
        // before that point). Its `Drop` replaces the two unconditional
        // `vfs_put_fs_type(fs_type)` calls the pre-P3-9c code needed at
        // the shared epilogue (one per `ret_val != 0`/`== 0` branch) --
        // both exit paths fall out of this same function scope, so a
        // single implicit drop covers what used to be two manual call
        // sites (and would silently start leaking again if a third exit
        // path were ever added without remembering the pairing).
        let mut fs_type_ref: Option<KArc<vfs_fs_type>> = None;
        let mut sb: *mut vfs_superblock = ptr::null_mut();
        let mut ret_val: c_int;

        if type_.is_null() || mountpoint.is_null() {
            crate::kprintln!("vfs_mount: invalid arguments");
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut __MOUNT_MUTEX) == 0 {
            crate::kprintln!("vfs_mount: mount mutex not held");
            return neg(EPERM);
        }

        ret_val = dir_inode_valid_holding(&*mountpoint);
        if ret_val != 0 {
            crate::kprintln!("vfs_mount: mountpoint inode not valid, errno={}", ret_val);
            return ret_val;
        }
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            if !rwsem_is_write_holding(&raw mut (*(*mountpoint).sb).lock) {
                crate::kprintln!("vfs_mount: mountpoint superblock write lock not held");
                return neg(EPERM);
            }
            if (*(*mountpoint).sb).flags.valid() == 0 {
                crate::kprintln!("vfs_mount: mountpoint superblock is not valid");
                return neg(EINVAL);
            }
            if !is_dir((*mountpoint).mode) {
                crate::kprintln!("vfs_mount: mountpoint is not a directory");
                return neg(EINVAL);
            }
        }

        ret_val = turn_mountpoint(mountpoint);
        if ret_val != 0 {
            crate::kprintln!("vfs_mount: failed to turn mountpoint, errno={}", ret_val);
            return ret_val;
        }

        'cleanup: {
            let fs_type_raw = vfs_get_fs_type(type_);
            if fs_type_raw.is_null() {
                crate::kprintln!("vfs_mount: filesystem type '{}' not found", crate::printf::Cs(type_));
                ret_val = neg(ENODEV);
                break 'cleanup;
            }
            // SAFETY: `vfs_get_fs_type`'s success postcondition (one held
            // kobject reference) is exactly `KArc::from_raw`'s
            // precondition.
            let arc = KArc::<vfs_fs_type>::from_raw(fs_type_raw);
            fs_type = KArc::as_ptr(&arc);
            fs_type_ref = Some(arc);
            if (*fs_type).flags.registered() == 0 {
                crate::kprintln!("vfs_mount: filesystem type '{}' not registered", crate::printf::Cs(type_));
                ret_val = neg(ENODEV);
                break 'cleanup;
            }
            // Ask the filesystem type to allocate/initialize a new
            // superblock. Private to the filesystem until attached, so no
            // locking is needed yet. (`ops` is `Some` for every
            // registered type — checked at registration; `sb` stays null
            // on failure, exactly like the old out-param, so the cleanup
            // path below skips the driver-free.)
            match (*fs_type).ops.expect("vfs_mount: registered fs_type without ops")
                .mount(mountpoint, device, flags, data)
            {
                Ok(new_sb) => {
                    sb = new_sb;
                    ret_val = 0;
                }
                Err(e) => {
                    ret_val = e.neg();
                    crate::kprintln!(
                        "vfs_mount: filesystem type '{}' mount failed, errno={}",
                        crate::printf::Cs(type_),
                        ret_val,
                    );
                    break 'cleanup;
                }
            }
            if !init_superblock_valid(sb) {
                crate::kprintln!("vfs_mount: invalid superblock returned by mount");
                ret_val = neg(EINVAL);
                break 'cleanup;
            }
            if (*sb).total_blocks != 0 && (*sb).used_blocks > (*sb).total_blocks {
                crate::kprintln!("vfs_mount: superblock used_blocks exceeds total_blocks");
                ret_val = neg(EINVAL);
                break 'cleanup;
            }
            if (*sb).root_inode.is_null() {
                crate::kprintln!("vfs_mount: superblock has no root inode");
                ret_val = neg(EINVAL);
                break 'cleanup;
            }
            if (*(*sb).root_inode).flags.valid() != 0 {
                crate::kprintln!("vfs_mount: root inode already marked valid");
                ret_val = neg(EINVAL);
                break 'cleanup;
            }
            init_superblock_structure(sb, fs_type);
            vfs_superblock_wlock(sb); // Must hold sb lock to init root inode.
            ret_val = init_sb_rooti(sb);
            if ret_val != 0 {
                crate::kprintln!(
                    "vfs_mount: failed to initialize superblock root inode, errno={}",
                    ret_val,
                );
                break 'cleanup;
            }

            attach_superblock_to_fstype(sb);
            (*sb).device = device;
            set_mountpoint(sb, mountpoint);
            (*(*sb).root_inode).sb = sb;
            ret_val = 0;
        }

        if ret_val != 0 {
            if !sb.is_null() {
                if !(*sb).root_inode.is_null() {
                    inode_ops((*sb).root_inode).free_inode((*sb).root_inode);
                }
                (*fs_type).ops.expect("vfs: registered fs_type without ops").free(sb);
            }
            clear_mountpoint(mountpoint);
            vfs_iunlock(mountpoint);
            if !(*mountpoint).sb.is_null() {
                vfs_superblock_unlock((*mountpoint).sb);
            }
            if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
                vfs_iput(mountpoint);
            }
            // `fs_type_ref` drops here (function return), releasing the
            // reference `vfs_get_fs_type` acquired above -- replaces the
            // old unconditional `vfs_put_fs_type(fs_type)` on this path.
            return ret_val;
        } else if rwsem_is_write_holding(&raw mut (*sb).lock) {
            (*sb).flags.set_initialized(1);
            (*sb).flags.set_valid(1);
            (*sb).flags.set_attached(1);
            vfs_superblock_unlock(sb);
        }
        // `fs_type_ref` drops at the end of this scope, releasing the
        // reference -- replaces the old unconditional
        // `vfs_put_fs_type(fs_type)` on the success path.
        ret_val
    }
}

/// Mirrors `__vfs_evict_unused_inodes()`.
///
/// Locking: caller must hold the superblock write lock.
unsafe fn evict_unused_inodes(sb: *mut vfs_superblock) -> usize {
    unsafe {
        let mut evicted = 0usize;
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "Superblock lock must be write held to evict inodes"
        );

        let mut pos = sb_inodes_first(sb);
        while !pos.is_null() {
            let tmp = sb_inodes_next(sb, pos);
            let inode = pos;

            'skip: {
                if (*inode).ref_count > 1 {
                    break 'skip;
                }
                if (*inode).flags.destroying() != 0 {
                    break 'skip;
                }
                if (*inode).flags.valid() == 0 {
                    break 'skip;
                }
                if (*inode).flags.mount() != 0 {
                    break 'skip;
                }

                vfs_ilock(inode);

                if (*inode).ref_count > 1
                    || (*inode).flags.destroying() != 0
                    || (*inode).flags.valid() == 0
                    || (*inode).flags.mount() != 0
                {
                    vfs_iunlock(inode);
                    break 'skip;
                }

                if (*inode).flags.dirty() != 0 {
                    // P3-10b: a driver without `sync_inode` inherits
                    // the no-op `Ok(())` default (old `None`-slot
                    // skip); the result was ignored here before too.
                    let _ = inode_ops(inode).sync_inode(inode);
                }

                if (*inode).ref_count == 1 {
                    inode_refcount_atomic(inode).fetch_sub(1, Ordering::SeqCst);
                }
                (*inode).flags.set_valid(0);
                vfs_remove_inode(sb, inode);
                vfs_iunlock(inode);

                inode_ops(inode).free_inode(inode);
                evicted += 1;
            }

            pos = tmp;
        }

        evicted
    }
}

/// Mirrors `vfs_unmount()`.
pub(crate) extern "C" fn vfs_unmount(mountpoint: *mut vfs_inode) -> c_int {
    unsafe {
        if mountpoint.is_null() {
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut __MOUNT_MUTEX) == 0 {
            return neg(EPERM);
        }
        if holding_mutex(&raw mut (*mountpoint).mutex) == 0 {
            return neg(EPERM);
        }
        let mut ret_val = inode_valid(&*mountpoint);
        if ret_val != 0 {
            return ret_val;
        }
        if !rwsem_is_write_holding(&raw mut (*(*mountpoint).sb).lock) {
            return neg(EPERM);
        }
        if (*(*mountpoint).sb).flags.valid() == 0 {
            return neg(EINVAL);
        }
        if !is_dir((*mountpoint).mode) {
            return neg(ENOTDIR);
        }
        if (*mountpoint).flags.mount() == 0 {
            return neg(EINVAL);
        }
        let sb = (*mountpoint).dev_mnt.mnt.mnt_sb;
        if sb.is_null() {
            return neg(EINVAL);
        }
        let mounted_inode = (*sb).root_inode;
        if mounted_inode.is_null() {
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut (*mounted_inode).mutex) == 0 {
            return neg(EPERM);
        }
        ret_val = inode_valid(&*mounted_inode);
        if ret_val != 0 {
            return ret_val;
        }
        if !rwsem_is_write_holding(&raw mut (*sb).lock) {
            return neg(EPERM);
        }
        if (*sb).flags.valid() == 0 {
            return neg(EINVAL);
        }
        let mc = superblock_mountcount(sb);
        if mc > 0 {
            crate::kprintln!("vfs_unmount: mount_count={}", mc);
            return neg(EBUSY);
        }
        if (*sb).flags.dirty() != 0 {
            crate::kprintln!(
                "vfs_unmount: sb valid={} dirty={}",
                ((*sb).flags.valid() as c_int as i32 as i64) as u64,
                ((*sb).flags.dirty() as c_int as i32 as i64) as u64,
            );
            return neg(EBUSY);
        }

        // Begin unmounting. (Required trait method as of P3-10b; the
        // old `None`-slot skip had no live instance -- mount-time
        // validation always required the slot.)
        sb_ops(sb).unmount_begin(sb);

        // Evict all unreferenced inodes from the cache before checking.
        evict_unused_inodes(sb);

        // Superblock should have no active inodes except the root inode.
        let remaining_inodes = hlist_len(&raw mut (*sb).inodes);
        if remaining_inodes > 1 {
            crate::kprintln!(
                "vfs_unmount: remaining inodes={} (expected 1 for root)",
                remaining_inodes as u64,
            );
            return neg(EBUSY);
        }
        if remaining_inodes == 1 {
            let only_inode = sb_inodes_first(sb);
            if only_inode != mounted_inode {
                crate::kprintln!(
                    "vfs_unmount: remaining inode is not root (ino={})",
                    (*only_inode).ino as u64,
                );
                return neg(EBUSY);
            }
        }

        // Do NOT call destroy_inode on the root inode during unmount --
        // that would corrupt the on-disk filesystem. Just tear down the
        // in-memory state.
        (*mounted_inode).flags.set_valid(0);
        vfs_remove_inode(sb, mounted_inode);

        detach_superblock_from_fstype(sb);
        clear_mountpoint(mountpoint);

        vfs_iunlock(mounted_inode);
        // Free the root inode (one ref from `set_mountpoint`'s `vfs_idup`
        // plus the creation ref -- freed directly since already removed
        // from cache).
        inode_ops(mounted_inode).free_inode(mounted_inode);
        (*sb).root_inode = ptr::null_mut();

        let fs_type = (*sb).fs_type;
        vfs_superblock_unlock(sb);

        vfs_iunlock(mountpoint);
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) && !(*mountpoint).sb.is_null() {
            vfs_superblock_unlock((*mountpoint).sb);
        }
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            vfs_iput(mountpoint);
        }

        (*fs_type).ops.expect("vfs: registered fs_type without ops").free(sb);

        0
    }
}

/// Mirrors `vfs_make_orphan()`.
///
/// Locking: caller must hold the superblock write lock and the inode
/// mutex.
pub(crate) extern "C" fn vfs_make_orphan(inode: *mut vfs_inode) -> c_int {
    unsafe {
        if inode.is_null() {
            return neg(EINVAL);
        }
        let sb = (*inode).sb;
        if sb.is_null() {
            return neg(EINVAL);
        }

        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "Must hold sb wlock to make orphan"
        );
        kassert!(
            holding_mutex(&raw mut (*inode).mutex) != 0,
            "Must hold inode lock to make orphan"
        );

        if (*inode).flags.orphan() != 0 {
            return 0; // Already orphan.
        }
        if (*inode).n_links != 0 {
            return neg(EINVAL); // Not unlinked yet.
        }

        (*inode).flags.set_orphan(1);
        ln_push_back(&raw mut (*sb).orphan_list, &raw mut (*inode).orphan_entry);
        (*sb).orphan_count += 1;

        // For backend fs: persist to on-disk orphan journal.
        if let Err(e) = sb_ops(sb).add_orphan(sb, inode) {
            crate::kprintln!(
                "vfs: warning: failed to persist orphan inode {}, errno={}",
                (*inode).ino as u64,
                e.neg(),
            );
        }

        0
    }
}

/// Mirrors `__vfs_final_unmount_cleanup()`. Called from `vfs_iput`
/// (`inode.rs`) when the last orphan inode is freed on a detached fs.
pub(crate) extern "C" fn __vfs_final_unmount_cleanup(sb: *mut vfs_superblock) {
    unsafe {
        if sb.is_null() {
            return;
        }

        kassert!(
            (*sb).flags.registered() == 0,
            "__vfs_final_unmount_cleanup: sb still attached"
        );
        kassert!((*sb).orphan_count == 0, "__vfs_final_unmount_cleanup: orphans remain");

        vfs_mount_lock();
        vfs_superblock_wlock(sb);

        if (*sb).flags.registered() != 0 {
            detach_superblock_from_fstype(sb);
        }

        if !(*sb).root_inode.is_null() {
            let rooti = (*sb).root_inode;
            vfs_ilock(rooti);
            // P3-10b: `destroy_inode` is a required trait method (the
            // old `None`-slot skip had no live instance).
            inode_ops(rooti).destroy_inode(rooti);
            (*rooti).flags.set_valid(0);
            vfs_remove_inode(sb, rooti);
            vfs_iunlock(rooti);
            inode_ops(rooti).free_inode(rooti);
            (*sb).root_inode = ptr::null_mut();
        }

        let fs_type = (*sb).fs_type;
        vfs_superblock_unlock(sb);
        vfs_mount_unlock();

        (*fs_type).ops.expect("vfs: registered fs_type without ops").free(sb);
    }
}

/// Mirrors `vfs_unmount_lazy()`.
pub(crate) extern "C" fn vfs_unmount_lazy(mountpoint: *mut vfs_inode) -> c_int {
    unsafe {
        if mountpoint.is_null() {
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut __MOUNT_MUTEX) == 0 {
            return neg(EPERM);
        }
        if holding_mutex(&raw mut (*mountpoint).mutex) == 0 {
            return neg(EPERM);
        }

        let parent_sb = (*mountpoint).sb;
        if !parent_sb.is_null() && !rwsem_is_write_holding(&raw mut (*parent_sb).lock) {
            return neg(EPERM);
        }

        let ret = inode_valid(&*mountpoint);
        if ret != 0 {
            return ret;
        }

        if !is_dir((*mountpoint).mode) {
            return neg(ENOTDIR);
        }
        if (*mountpoint).flags.mount() == 0 {
            return neg(EINVAL);
        }

        let sb = (*mountpoint).dev_mnt.mnt.mnt_sb;
        if sb.is_null() {
            return neg(EINVAL);
        }

        // Phase 1: check for child mounts.
        vfs_superblock_wlock(sb);

        if superblock_mountcount(sb) > 0 {
            vfs_superblock_unlock(sb);
            return neg(EBUSY);
        }

        // Block new operations.
        (*sb).flags.set_unmounting(1);

        // Phase 2: detach from mount tree.
        clear_mountpoint(mountpoint);
        (*sb).mountpoint = ptr::null_mut();
        (*sb).parent_sb = ptr::null_mut();
        (*sb).flags.set_attached(0);
        (*sb).flags.set_valid(0); // Prevent new lookups.

        vfs_iunlock(mountpoint);
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) && !(*mountpoint).sb.is_null() {
            vfs_superblock_unlock((*mountpoint).sb);
        }
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            vfs_iput(mountpoint);
        }

        // Phase 3: sync if needed (backend filesystems).
        if (*sb).flags.backendless() == 0 && (*sb).flags.dirty() != 0 {
            (*sb).flags.set_syncing(1);
            let sret = sb_ops(sb).sync_fs(sb, 1);
            (*sb).flags.set_syncing(0);
            if let Err(e) = sret {
                crate::kprintln!("vfs_unmount_lazy: warning: sync failed, errno={}", e.neg());
            }
        }

        sb_ops(sb).unmount_begin(sb);

        // Phase 4: mark all referenced inodes as orphans.
        let rooti = (*sb).root_inode;
        let mut pos = sb_inodes_first(sb);
        while !pos.is_null() {
            let tmp = sb_inodes_next(sb, pos);
            let inode = pos;
            if inode != rooti && (*inode).ref_count > 0 {
                if (*inode).flags.orphan() == 0 {
                    vfs_ilock(inode);
                    (*inode).flags.set_orphan(1);
                    ln_push_back(&raw mut (*sb).orphan_list, &raw mut (*inode).orphan_entry);
                    (*sb).orphan_count += 1;
                    vfs_iunlock(inode);
                }
            }
            pos = tmp;
        }

        // Phase 5: immediate cleanup if no orphans.
        if (*sb).orphan_count == 0 {
            detach_superblock_from_fstype(sb);

            if !rooti.is_null() {
                vfs_ilock(rooti);
                // P3-10b: required trait method (see
                // `__vfs_final_unmount_cleanup`).
                inode_ops(rooti).destroy_inode(rooti);
                (*rooti).flags.set_valid(0);
                vfs_remove_inode(sb, rooti);
                vfs_iunlock(rooti);
                inode_ops(rooti).free_inode(rooti);
                (*sb).root_inode = ptr::null_mut();
            }

            let fs_type = (*sb).fs_type;
            vfs_superblock_unlock(sb);
            (*fs_type).ops.expect("vfs: registered fs_type without ops").free(sb);
        } else {
            // Orphans exist -- cleanup deferred to vfs_iput.
            vfs_superblock_unlock(sb);
        }

        0
    }
}

/// Mirrors `vfs_get_mnt_rooti()`.
pub(crate) extern "C" fn vfs_get_mnt_rooti(
    mountpoint: *mut vfs_inode,
    ret_rooti: *mut *mut vfs_inode,
) -> c_int {
    unsafe {
        if mountpoint.is_null() || ret_rooti.is_null() {
            return neg(EINVAL);
        }
        let ret_val;
        vfs_ilock(mountpoint);
        ret_val = dir_inode_valid_holding(&*mountpoint);
        if ret_val != 0 {
            vfs_iunlock(mountpoint);
            return ret_val;
        }
        if !is_dir((*mountpoint).mode) {
            vfs_iunlock(mountpoint);
            return neg(ENOTDIR);
        }
        if (*mountpoint).flags.mount() == 0 {
            vfs_iunlock(mountpoint);
            return neg(EINVAL);
        }
        let sb = (*mountpoint).dev_mnt.mnt.mnt_sb;
        if sb.is_null() {
            vfs_iunlock(mountpoint);
            return neg(EINVAL);
        }
        let rooti = (*sb).root_inode;
        if rooti.is_null() {
            vfs_iunlock(mountpoint);
            return neg(EINVAL);
        }
        // Take a reference to root inode BEFORE unlocking mountpoint.
        if !vfs_idup_not_zero(rooti) {
            vfs_iunlock(mountpoint);
            return neg(EINVAL);
        }
        vfs_iunlock(mountpoint);

        // Now we hold a reference, safe to lock.
        vfs_ilock(rooti);
        *ret_rooti = rooti;
        ret_val
    }
}

/// Mirrors `vfs_superblock_rlock()`.
pub(crate) extern "C" fn vfs_superblock_rlock(sb: *mut vfs_superblock) {
    if !sb.is_null() {
        unsafe { rwsem_acquire_read(&raw mut (*sb).lock) };
    }
}

/// Mirrors `vfs_superblock_wlock()`.
pub(crate) extern "C" fn vfs_superblock_wlock(sb: *mut vfs_superblock) {
    if !sb.is_null() {
        unsafe { rwsem_acquire_write(&raw mut (*sb).lock) };
    }
}

/// Mirrors `vfs_superblock_wholding()`.
pub(crate) extern "C" fn vfs_superblock_wholding(sb: *mut vfs_superblock) -> bool {
    if sb.is_null() {
        return false;
    }
    unsafe { rwsem_is_write_holding(&raw mut (*sb).lock) }
}

/// Mirrors `vfs_superblock_unlock()`.
pub(crate) extern "C" fn vfs_superblock_unlock(sb: *mut vfs_superblock) {
    if !sb.is_null() {
        unsafe { rwsem_release(&raw mut (*sb).lock) };
    }
}

/// Mirrors `vfs_superblock_spin_lock()`.
pub(crate) extern "C" fn vfs_superblock_spin_lock(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when acquiring spinlock");
        spin_lock(&raw mut (*sb).spinlock);
    }
}

/// Mirrors `vfs_superblock_spin_unlock()`.
pub(crate) extern "C" fn vfs_superblock_spin_unlock(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when releasing spinlock");
        spin_unlock(&raw mut (*sb).spinlock);
    }
}

/// Mirrors `vfs_superblock_mountcount_inc()`.
pub(crate) extern "C" fn vfs_superblock_mountcount_inc(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when incrementing mount count");
        let cnt = sb_mountcount_atomic(sb).fetch_add(1, Ordering::SeqCst) + 1;
        kassert!(cnt > 0, "Superblock mount count overflow");
    }
}

/// Mirrors `vfs_superblock_mountcount_dec()`.
pub(crate) extern "C" fn vfs_superblock_mountcount_dec(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when decrementing mount count");
        let cnt = sb_mountcount_atomic(sb).fetch_sub(1, Ordering::SeqCst) - 1;
        kassert!(cnt >= 0, "Superblock mount count underflow");
    }
}

/// Mirrors `vfs_superblock_dup()`.
///
/// # Safety notes (preserved from the C original)
/// Uses only atomic operations: does not acquire locks, sleep, or
/// allocate. Critical because `vfs_inode_get_ref()` may call this while
/// holding the inode lock -- any blocking here would risk deadlock.
pub(crate) extern "C" fn vfs_superblock_dup(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when duplicating");
        let ret = sb_refcount_atomic(sb).fetch_add(1, Ordering::SeqCst) + 1;
        kassert!(ret > 0, "Superblock refcount overflow");
    }
}

/// Mirrors `vfs_superblock_put()`.
pub(crate) extern "C" fn vfs_superblock_put(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when putting");
        kassert!(
            !vfs_superblock_wholding(sb),
            "Cannot put superblock while holding its lock"
        );
        kassert!(
            holding_mutex(&raw mut __MOUNT_MUTEX) == 0,
            "Cannot put superblock while holding mount mutex"
        );
        kassert!(
            atomic_dec_unless(sb_refcount_atomic(sb), 0),
            "Superblock refcount underflow"
        );
    }
}

pub(crate) fn vfs_alloc_inode_inner(sb: *mut vfs_superblock) -> KResult<*mut vfs_inode> {
    unsafe {
        if sb.is_null() {
            return Err(Errno::Inval);
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "vfs_alloc_inode: must hold superblock write lock"
        );
        if (*sb).flags.valid() == 0 {
            return Err(Errno::Inval);
        }
        // P3-10b: the driver call and the inode-cache insert are both
        // `KResult`-native now -- the old internal `ERR_PTR` encoding
        // (and its dead `Ok(null)` -> `NoEnt` branch, which had no
        // producer) is gone; `Errno::Again` propagates to the caller's
        // retry loops exactly as the old `EAGAIN` pointer did.
        let inode = sb_ops(sb).alloc_inode(sb)?;
        __vfs_inode_init(inode);
        match vfs_add_inode_inner(sb, inode) {
            Ok(_) => Ok(inode), // locked
            Err(e) => {
                inode_ops(inode).free_inode(inode);
                Err(e)
            }
        }
    }
}

/// Mirrors `vfs_alloc_inode()`.
pub(crate) extern "C" fn vfs_alloc_inode(sb: *mut vfs_superblock) -> *mut vfs_inode {
    result_to_errptr(vfs_alloc_inode_inner(sb))
}

fn vfs_get_inode_inner(sb: *mut vfs_superblock, ino: u64) -> KResult<*mut vfs_inode> {
    unsafe {
        if sb.is_null() {
            return Err(Errno::Inval);
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "vfs_get_inode: must hold superblock write lock"
        );
        if (*sb).flags.valid() == 0 {
            return Err(Errno::Inval);
        }
        // P3-10b: `KResult`-native end to end (see `vfs_alloc_inode_inner`).
        let inode = sb_ops(sb).get_inode(sb, ino)?;
        __vfs_inode_init(inode);
        match vfs_add_inode_inner(sb, inode) {
            Ok(existing) => {
                if existing != inode {
                    // Found existing inode in hash -- free the newly
                    // loaded one.
                    inode_ops(inode).free_inode(inode);
                    return Ok(existing); // locked
                }
                Ok(inode) // locked
            }
            Err(e) => {
                inode_ops(inode).free_inode(inode);
                Err(e)
            }
        }
    }
}

/// Mirrors `vfs_get_inode()`.
pub(crate) extern "C" fn vfs_get_inode(sb: *mut vfs_superblock, ino: u64) -> *mut vfs_inode {
    result_to_errptr(vfs_get_inode_inner(sb, ino))
}

/// Mirrors `vfs_sync_superblock()`.
pub(crate) extern "C" fn vfs_sync_superblock(sb: *mut vfs_superblock, wait: c_int) -> c_int {
    unsafe {
        if sb.is_null() {
            return neg(EINVAL);
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "vfs_sync_superblock: must hold superblock write lock"
        );
        if (*sb).flags.valid() == 0 {
            return neg(EINVAL);
        }
        if (*sb).flags.dirty() == 0 {
            return 0; // Already clean.
        }
        match sb_ops(sb).sync_fs(sb, wait) {
            Ok(()) => {
                (*sb).flags.set_dirty(0);
                0
            }
            Err(e) => e.neg(),
        }
    }
}

/// Mirrors `vfs_get_fs_type()`.
///
/// Locking: caller must hold the mount mutex via [`vfs_mount_lock`].
pub(crate) extern "C" fn vfs_get_fs_type(name: *const c_char) -> *mut vfs_fs_type {
    unsafe {
        if name.is_null() {
            return ptr::null_mut();
        }
        kassert!(
            holding_mutex(&raw mut __MOUNT_MUTEX) != 0,
            "vfs_put_fs_type: must hold mount mutex"
        );
        let fs_type = get_fs_type_locked(name);
        if !fs_type.is_null() {
            kobject_get(&raw mut (*fs_type).kobj);
        }
        fs_type
    }
}

/// Mirrors `vfs_put_fs_type()`.
///
/// Locking: caller must hold the mount mutex via [`vfs_mount_lock`].
// P3-9c: `vfs_mount` (this file's one former caller) now holds its
// `vfs_get_fs_type` reference in a `KArc<vfs_fs_type>` instead, whose
// `Drop` calls the raw `kobject_put` this function wraps directly --
// leaving this header-declared (`kernel/inc/vfs/fs.h`) primitive with no
// live in-tree caller. Kept (not deleted) as the public non-KArc
// entry point for `vfs_get_fs_type`, same precedent as `dev/bio.rs`'s
// `bio_dup`/`dev/cdev.rs`'s `cdev_dup`.
#[allow(dead_code)]
pub(crate) extern "C" fn vfs_put_fs_type(fs_type: *mut vfs_fs_type) {
    unsafe {
        if fs_type.is_null() {
            return;
        }
        kassert!(
            holding_mutex(&raw mut __MOUNT_MUTEX) != 0,
            "vfs_put_fs_type: must hold mount mutex"
        );
        kobject_put(&raw mut (*fs_type).kobj);
    }
}

/// Mirrors `__vfs_dentry_get_self_inode()`.
///
/// Locking: none required -- the parent inode is guaranteed alive as long
/// as the dentry is (VFS always caches ancestor directories).
unsafe fn dentry_get_self_inode(dentry: *mut vfs_dentry) -> *mut vfs_inode {
    unsafe {
        if dentry.is_null() || (*dentry).parent.is_null() {
            return ptr::null_mut();
        }
        if (*(*dentry).parent).sb == (*dentry).sb && (*(*dentry).parent).ino == (*dentry).ino {
            vfs_idup((*dentry).parent);
            return (*dentry).parent;
        }
        ptr::null_mut()
    }
}

/// Mirrors `__vfs_set_parent_from_dentry()`.
///
/// Locking: caller holds the inode lock.
unsafe fn set_parent_from_dentry(inode: *mut vfs_inode, parent: *mut vfs_inode) {
    unsafe {
        if !parent.is_null() && is_dir((*inode).mode) {
            if inode != parent {
                (*inode).parent = parent;
                vfs_idup(parent);
            }
        }
    }
}

/// Mirrors `__vfs_set_name_if_null()`.
///
/// Locking: caller holds the inode lock.
unsafe fn set_name_if_null(inode: *mut vfs_inode, dentry: *mut vfs_dentry) {
    unsafe {
        if is_dir((*inode).mode)
            && (*inode).name.is_null()
            && !(*dentry).name.is_null()
            && (*dentry).name_len > 0
        {
            (*inode).name = strndup((*dentry).name, (*dentry).name_len as usize);
        }
    }
}

/// Mirrors `__vfs_get_dentry_inode_impl()`.
///
/// Locking: caller holds the dentry's superblock read lock on entry; this
/// helper may drop the read lock and acquire the write lock internally.
unsafe fn get_dentry_inode_impl(dentry: *mut vfs_dentry) -> KResult<*mut vfs_inode> {
    unsafe {
        let sb = (*dentry).sb;

        // P3-10b: `KResult`-native cache/load calls end to end (the old
        // internal ERR_PTR consumption, including its dead `Ok(null)`
        // passthrough branches, is gone). `-ENOENT` falls through to
        // the next lookup stage exactly as before; any other error
        // propagates.
        //
        // vfs_get_inode_cached returns with refcount already incremented.
        match vfs_get_inode_cached_inner(sb, (*dentry).ino) {
            Ok(inode) => {
                set_name_if_null(inode, dentry);
                vfs_iunlock(inode);
                return Ok(inode);
            }
            Err(e) if e.neg() != neg(ENOENT) => return Err(e),
            Err(_) => {}
        }

        if !rwsem_is_write_holding(&raw mut (*sb).lock) {
            vfs_superblock_unlock(sb);
            vfs_superblock_wlock(sb);
        }

        if (*sb).flags.valid() == 0 {
            return Err(Errno::Inval);
        }

        match vfs_get_inode_cached_inner(sb, (*dentry).ino) {
            Ok(inode) => {
                set_name_if_null(inode, dentry);
                vfs_iunlock(inode);
                return Ok(inode);
            }
            Err(e) if e.neg() != neg(ENOENT) => return Err(e),
            Err(_) => {}
        }

        let inode = vfs_get_inode_inner(sb, (*dentry).ino)?;

        // "." and ".." are synthesized by VFS and should always hit the
        // cache.
        kassert!(
            !((*dentry).name_len == 1 && *(*dentry).name.offset(0) == b'.' as c_char),
            "__vfs_get_dentry_inode_impl: \".\" should not reach fresh load path"
        );
        kassert!(
            !((*dentry).name_len == 2
                && *(*dentry).name.offset(0) == b'.' as c_char
                && *(*dentry).name.offset(1) == b'.' as c_char),
            "__vfs_get_dentry_inode_impl: \"..\" should not reach fresh load path"
        );

        set_parent_from_dentry(inode, (*dentry).parent);
        set_name_if_null(inode, dentry);
        vfs_iunlock(inode);
        Ok(inode)
    }
}

fn vfs_get_dentry_inode_locked_inner(dentry: *mut vfs_dentry) -> KResult<*mut vfs_inode> {
    unsafe {
        if dentry.is_null() {
            return Err(Errno::Inval);
        }
        if (*dentry).sb.is_null() {
            return Err(Errno::Inval);
        }
        if (*(*dentry).sb).flags.valid() == 0 {
            return Err(Errno::Inval);
        }

        let inode = dentry_get_self_inode(dentry);
        if !inode.is_null() {
            return Ok(inode);
        }

        get_dentry_inode_impl(dentry)
    }
}

/// Mirrors `vfs_get_dentry_inode_locked()`.
pub(crate) extern "C" fn vfs_get_dentry_inode_locked(dentry: *mut vfs_dentry) -> *mut vfs_inode {
    result_to_errptr(vfs_get_dentry_inode_locked_inner(dentry))
}

pub(crate) fn vfs_get_dentry_inode_inner(dentry: *mut vfs_dentry) -> KResult<*mut vfs_inode> {
    unsafe {
        if dentry.is_null() {
            return Err(Errno::Inval);
        }
        if (*dentry).sb.is_null() {
            return Err(Errno::Inval);
        }

        let inode = dentry_get_self_inode(dentry);
        if !inode.is_null() {
            return Ok(inode);
        }

        let sb = (*dentry).sb;
        vfs_superblock_rlock(sb);
        if (*sb).flags.valid() == 0 {
            vfs_superblock_unlock(sb);
            return Err(Errno::Inval);
        }
        let inode = get_dentry_inode_impl(dentry);
        vfs_superblock_unlock(sb);
        inode
    }
}

/// Mirrors `vfs_get_dentry_inode()`.
pub(crate) extern "C" fn vfs_get_dentry_inode(dentry: *mut vfs_dentry) -> *mut vfs_inode {
    result_to_errptr(vfs_get_dentry_inode_inner(dentry))
}

/******************************************************************************
 * Module scope private functions (declared in vfs_private.h; real
 * external linkage -- other C translation units under kernel/vfs/
 * include that header).
 *****************************************************************************/

fn vfs_get_inode_cached_inner(sb: *mut vfs_superblock, ino: u64) -> KResult<*mut vfs_inode> {
    unsafe {
        if sb.is_null() {
            return Err(Errno::Inval);
        }
        if (*sb).flags.valid() == 0 {
            return Err(Errno::Inval);
        }
        let inode = inode_hash_get(sb, ino);
        if inode.is_null() {
            return Err(Errno::NoEnt);
        }
        // CRITICAL: take a reference BEFORE locking to prevent
        // use-after-free. Backendless filesystems keep refcount=0,
        // n_links>0 inodes alive in cache; allow bumping from 0 in that
        // case, otherwise the inode is unreachable.
        if !vfs_idup_not_zero(inode) {
            if (*sb).flags.backendless() != 0
                && (*inode).n_links > 0
                && (*inode).flags.valid() != 0
                && (*inode).flags.destroying() == 0
            {
                inode_refcount_atomic(inode).fetch_add(1, Ordering::SeqCst);
            } else {
                return Err(Errno::NoEnt); // Inode is dying.
            }
        }
        vfs_ilock(inode);
        if (*inode).flags.valid() == 0 || (*inode).flags.destroying() != 0 {
            // Invalidated or being destroyed after being fetched from the
            // cache. Can't call vfs_iput here (caller may hold sb wlock);
            // queue to the workqueue instead.
            vfs_iunlock(inode);
            queue_deferred_iput(inode);
            return Err(Errno::NoEnt);
        }
        Ok(inode)
    }
}

/// Mirrors `vfs_get_inode_cached()`.
///
/// Locking: caller holds the superblock read or write lock for the
/// entire call. On success, the returned inode is locked.
pub(crate) extern "C" fn vfs_get_inode_cached(sb: *mut vfs_superblock, ino: u64) -> *mut vfs_inode {
    result_to_errptr(vfs_get_inode_cached_inner(sb, ino))
}

fn vfs_add_inode_inner(sb: *mut vfs_superblock, inode: *mut vfs_inode) -> KResult<*mut vfs_inode> {
    unsafe {
        if sb.is_null() || inode.is_null() {
            return Err(Errno::Inval);
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "Superblock lock must be write held to add inode"
        );
        if (*sb).flags.valid() == 0 && (*sb).flags.initialized() != 0 {
            return Err(Errno::Inval);
        }
        if !(*inode).sb.is_null() {
            return Err(Errno::Inval);
        }
        if (*inode).flags.valid() != 0 {
            return Err(Errno::Inval);
        }
        let existing = inode_hash_get(sb, (*inode).ino);
        if !existing.is_null() {
            // Check destroying WITHOUT locking (to avoid deadlock -- see
            // the C original's comment: vfs_iput holds the inode lock,
            // releases sb lock, calls destroy_inode; we hold sb lock, so
            // if it's set, the destroying thread has released sb lock and
            // is in destroy_inode).
            if (*existing).flags.destroying() != 0 {
                return Err(Errno::Again);
            }
            vfs_ilock(existing);
            if (*existing).flags.destroying() != 0
                || (*existing).flags.valid() == 0
            {
                vfs_iunlock(existing);
                return Err(Errno::Again);
            }
            return Ok(existing);
        }
        let popped = inode_hash_add(sb, inode);
        if !popped.is_null() {
            xv6_panic(
                c"vfs_add_inode: inode hash add returned existing inode unexpectedly".as_ptr(),
            );
        }
        (*inode).flags.set_valid(1);
        (*inode).sb = sb;
        vfs_ilock(inode);
        Ok(inode)
    }
}

/// Mirrors `vfs_add_inode()`.
///
/// Locking: caller holds the superblock write lock. On success, the
/// returned inode is locked.
pub(crate) extern "C" fn vfs_add_inode(sb: *mut vfs_superblock, inode: *mut vfs_inode) -> *mut vfs_inode {
    result_to_errptr(vfs_add_inode_inner(sb, inode))
}

/// Mirrors `vfs_remove_inode()`.
///
/// Locking: caller holds the superblock write lock and the inode mutex.
pub(crate) extern "C" fn vfs_remove_inode(sb: *mut vfs_superblock, inode: *mut vfs_inode) -> c_int {
    unsafe {
        if sb.is_null() || inode.is_null() {
            return neg(EINVAL);
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "Superblock lock must be write held to remove inode"
        );
        kassert!(
            holding_mutex(&raw mut (*inode).mutex) != 0,
            "Inode lock must be held to remove inode"
        );
        // Allow removal from detached superblocks (lazy unmount cleanup).
        if (*sb).flags.valid() == 0 && (*sb).flags.attached() != 0 {
            return neg(EINVAL);
        }

        let already_destroyed = (*inode).flags.valid() == 0;

        let existing = inode_hash_get(sb, (*inode).ino);
        if existing.is_null() {
            return neg(ENOENT);
        }
        if existing != inode {
            return neg(ENOENT);
        }
        let popped = hlist_pop(
            &raw mut (*sb).inodes,
            inode as *mut c_void,
        ) as *mut vfs_inode;
        if popped != inode {
            xv6_panic(c"vfs_remove_inode: inode hash pop returned unexpected inode".as_ptr());
        }

        if !already_destroyed {
            (*inode).flags.set_valid(0);
        }

        (*inode).sb = ptr::null_mut();
        0
    }
}

/// Mirrors `vfs_release_dentry()`.
pub(crate) extern "C" fn vfs_release_dentry(dentry: *mut vfs_dentry) {
    unsafe {
        if dentry.is_null() {
            return;
        }
        if !(*dentry).name.is_null() {
            kmm_free((*dentry).name as *mut c_void);
            (*dentry).name = ptr::null_mut();
            (*dentry).name_len = 0;
        }
    }
}

/// Mirrors `vfs_struct_init()`.
pub(crate) extern "C" fn vfs_struct_init() -> *mut fs_struct {
    unsafe {
        let fs = struct_alloc_init();
        kassert!(!fs.is_null(), "idle_thread_init: failed to create fs_struct");
        // Preserved 1:1 from the C original: `__vfs_struct_alloc_init`
        // already performs this exact `spin_init`/refcount-store pair;
        // `vfs_struct_init` redoes it (harmless -- both are idempotent
        // full re-initializations of freshly allocated, still-exclusive
        // memory).
        fs_refcount_atomic(fs).store(1, Ordering::Release);
        spin_init(&raw mut (*fs).lock, FS_STRUCT_LOCK_NAME.as_ptr() as *mut c_char);
        (*fs).rooti.sb = ptr::null_mut();
        (*fs).rooti.inode = ptr::null_mut();
        (*fs).cwd.sb = ptr::null_mut();
        (*fs).cwd.inode = ptr::null_mut();
        fs
    }
}

fn vfs_struct_clone_inner(old_fs: *mut fs_struct, clone_flags: u64) -> KResult<*mut fs_struct> {
    unsafe {
        if old_fs.is_null() {
            return Err(Errno::Inval);
        }

        if clone_flags & CLONE_FS != 0 {
            // Share the fs_struct.
            fs_refcount_atomic(old_fs).fetch_add(1, Ordering::SeqCst);
            return Ok(old_fs);
        }

        let new_fs = struct_alloc_init();
        if new_fs.is_null() {
            return Err(Errno::NoMem);
        }

        // Get inode pointers under spinlock, but take references outside
        // (vfs_inode_get_ref may acquire the inode mutex).
        spin_lock(&raw mut (*old_fs).lock);
        let rooti = (*old_fs).rooti.inode;
        let cwdi = (*old_fs).cwd.inode;
        let rooti_ok = !rooti.is_null() && vfs_idup_not_zero(rooti);
        let cwdi_ok = !cwdi.is_null() && vfs_idup_not_zero(cwdi);
        spin_unlock(&raw mut (*old_fs).lock);

        let mut ret;
        if rooti_ok {
            ret = vfs_inode_get_ref(rooti, &raw mut (*new_fs).rooti);
            vfs_iput(rooti);
            if ret != 0 {
                if cwdi_ok {
                    vfs_iput(cwdi);
                }
                vfs_inode_put_ref(&raw mut (*new_fs).rooti);
                vfs_inode_put_ref(&raw mut (*new_fs).cwd);
                struct_free(new_fs);
                return Err(Errno::Raw(ret));
            }
        }
        if cwdi_ok {
            ret = vfs_inode_get_ref(cwdi, &raw mut (*new_fs).cwd);
            vfs_iput(cwdi);
            if ret != 0 {
                vfs_inode_put_ref(&raw mut (*new_fs).rooti);
                vfs_inode_put_ref(&raw mut (*new_fs).cwd);
                struct_free(new_fs);
                return Err(Errno::Raw(ret));
            }
        }
        Ok(new_fs)
    }
}

/// Mirrors `vfs_struct_clone()`.
pub(crate) extern "C" fn vfs_struct_clone(old_fs: *mut fs_struct, clone_flags: u64) -> *mut fs_struct {
    result_to_errptr(vfs_struct_clone_inner(old_fs, clone_flags))
}

/// Mirrors `vfs_struct_put()`.
pub(crate) extern "C" fn vfs_struct_put(fs: *mut fs_struct) {
    unsafe {
        if fs.is_null() {
            return;
        }
        if !atomic_dec_unless(fs_refcount_atomic(fs), 1) {
            vfs_inode_put_ref(&raw mut (*fs).rooti);
            vfs_inode_put_ref(&raw mut (*fs).cwd);
            struct_free(fs);
        }
    }
}

/// Mirrors `vfs_inode_get_ref()`.
pub(crate) extern "C" fn vfs_inode_get_ref(inode: *mut vfs_inode, r: *mut vfs_inode_ref) -> c_int {
    unsafe {
        if inode.is_null() || r.is_null() {
            return neg(EINVAL);
        }
        let sb = (*inode).sb;
        if sb.is_null() {
            return neg(EINVAL);
        }
        // Caller must already hold a reference, so these can't fail.
        vfs_superblock_dup(sb);
        vfs_idup(inode);
        (*r).sb = sb;
        (*r).inode = inode;
        0
    }
}

/// Mirrors `vfs_inode_put_ref()`.
pub(crate) extern "C" fn vfs_inode_put_ref(r: *mut vfs_inode_ref) {
    unsafe {
        if r.is_null() {
            return;
        }
        if !(*r).inode.is_null() {
            vfs_iput((*r).inode);
            (*r).inode = ptr::null_mut();
        }
        if !(*r).sb.is_null() {
            vfs_superblock_put((*r).sb);
            (*r).sb = ptr::null_mut();
        }
    }
}

/// Mirrors `vfs_inode_deref()`.
pub(crate) extern "C" fn vfs_inode_deref(r: *mut vfs_inode_ref) -> *mut vfs_inode {
    if r.is_null() {
        ptr::null_mut()
    } else {
        unsafe { (*r).inode }
    }
}

/******************************************************************************
 * Debug: dump all active inodes
 ******************************************************************************/

fn inode_mode_str(mode: u32) -> *const c_char {
    if is_dir(mode) {
        return c"DIR".as_ptr();
    }
    if is_reg(mode) {
        return c"REG".as_ptr();
    }
    if is_lnk(mode) {
        return c"LNK".as_ptr();
    }
    if is_chr(mode) {
        return c"CHR".as_ptr();
    }
    if is_blk(mode) {
        return c"BLK".as_ptr();
    }
    if is_fifo(mode) {
        return c"FIFO".as_ptr();
    }
    if is_sock(mode) {
        return c"SOCK".as_ptr();
    }
    c"???".as_ptr()
}

/// Mirrors `__dump_sb_inodes()`.
///
/// Locking: caller must hold the superblock read lock.
unsafe fn dump_sb_inodes(sb: *mut vfs_superblock) {
    unsafe {
        let mut inode_count = 0;
        let mut active_count = 0;

        let mut pos = sb_inodes_first(sb);
        while !pos.is_null() {
            inode_count += 1;
            if (*pos).ref_count > 0 {
                active_count += 1;
            }
            pos = sb_inodes_next(sb, pos);
        }

        crate::kprintln!(
            "  Superblock {}: valid={} attached={} backendless={} inodes: total={} active={}",
            crate::printf::Ptr(sb as u64),
            (*sb).flags.valid() as c_int,
            (*sb).flags.attached() as c_int,
            (*sb).flags.backendless() as c_int,
            inode_count,
            active_count,
        );

        pos = sb_inodes_first(sb);
        while !pos.is_null() {
            let inode = pos;
            pos = sb_inodes_next(sb, pos);
            if (*inode).ref_count > 0 || (*inode).n_links > 0 {
                crate::kprint!(
                    "    ino={} type={} ref={} n_links={} valid={} dirty={} destroying={} orphan={}",
                    (*inode).ino as u64,
                    crate::printf::Cs(inode_mode_str((*inode).mode)),
                    (*inode).ref_count,
                    (*inode).n_links,
                    (*inode).flags.valid() as c_int,
                    (*inode).flags.dirty() as c_int,
                    (*inode).flags.destroying() as c_int,
                    (*inode).flags.orphan() as c_int,
                );
                if is_dir((*inode).mode) {
                    if !(*inode).name.is_null() {
                        crate::kprint!(" name=\"{}\"", crate::printf::Cs((*inode).name));
                    }
                    if !(*inode).parent.is_null() {
                        crate::kprint!(" parent_ino={}", (*(*inode).parent).ino as u64);
                    }
                }
                if (*inode).flags.mount() != 0 {
                    crate::kprint!(
                        " [mountpoint mnt_sb={}]",
                        crate::printf::Ptr((*inode).dev_mnt.mnt.mnt_sb as u64),
                    );
                } else if is_chr((*inode).mode) {
                    crate::kprint!(
                        " cdev={}",
                        ((*inode).dev_mnt.cdev as i32 as i64) as u64,
                    );
                } else if is_blk((*inode).mode) {
                    crate::kprint!(
                        " bdev={}",
                        ((*inode).dev_mnt.bdev as i32 as i64) as u64,
                    );
                }
                crate::kprintln!();
            }
        }
    }
}

/// Mirrors `vfs_dump_sb_inodes()`.
pub(crate) extern "C" fn vfs_dump_sb_inodes(sb: *mut vfs_superblock) {
    unsafe {
        if sb.is_null() {
            crate::kprintln!("vfs_dump_sb_inodes: NULL superblock");
            return;
        }

        crate::kprintln!("\n=== VFS Superblock Inode Dump ===");
        let fs_type = (*sb).fs_type;
        if fs_type.is_null() {
            crate::kprintln!("Filesystem type: (null)");
        } else {
            crate::kprintln!("Filesystem type: {}", crate::printf::Cs((*fs_type).name));
        }

        vfs_superblock_rlock(sb);
        dump_sb_inodes(sb);
        vfs_superblock_unlock(sb);

        crate::kprintln!("\n=== End of Superblock Inode Dump ===\n");
    }
}

/// Mirrors `vfs_dump_inodes()`. `vfs_fs_type.list_entry` and
/// `vfs_superblock.siblings` are both their struct's first field (offset
/// 0), so these walks cast `list_node_t` pointers directly (see the
/// hlist-walker section doc for the same reasoning).
pub(crate) extern "C" fn vfs_dump_inodes() {
    crate::kprintln!("\n=== VFS Inode Dump ===");

    // SAFETY: `vfs_mount_lock`/`unlock` are the FFI mount-lock primitives;
    // the whole walk below runs under this lock.
    unsafe { vfs_mount_lock(); }

    let fs_head = &raw mut VFS_FS_TYPES;
    // N-I1: outer raw `(*fpos).next` chase over the fs-type list replaced by
    // the safe `ListIterator`. Class-(A) read-only walk under the mount lock
    // (no fs-type is registered/unregistered under it). Item field reads keep
    // their `unsafe` (item-deref floor). Both walks capture the next node
    // before yielding — the same list-foreach-safe semantics the old manual
    // `fpos = (*fpos).next` before the body provided.
    crate::list_for_each!(fs_head, offset_of!(vfs_fs_type, list_entry), fnode, {
        let fstype: *mut vfs_fs_type = fnode;
        // SAFETY: `fstype` is a live `vfs_fs_type` on `VFS_FS_TYPES`
        // (offset-0 `list_entry`); its `sb_count`/`name`/`superblocks`
        // fields are valid under the held mount lock.
        let (sb_count, name) = unsafe { ((*fstype).sb_count, (*fstype).name) };
        if sb_count == 0 {
            continue;
        }

        crate::kprintln!(
            "\nFilesystem type: {} (superblocks: {})",
            crate::printf::Cs(name),
            sb_count,
        );

        // SAFETY: `superblocks` is a live, initialized list head embedded in
        // `fstype` (offset 16), valid under the mount lock.
        let sb_head = unsafe { &raw mut (*fstype).superblocks };
        // N-I1: inner raw `(*spos).next` chase over the superblock sibling
        // list replaced by the safe `ListIterator`. Class-(A) read-only walk;
        // `dump_sb_inodes` only reads (under the sb's own rlock) and never
        // unlinks a sibling.
        crate::list_for_each!(sb_head, offset_of!(vfs_superblock, siblings), snode, {
            let sb: *mut vfs_superblock = snode;
            // SAFETY: `sb` is a live `vfs_superblock` linked via its offset-0
            // `siblings`; the rlock/dump/unlock trio requires exactly that.
            unsafe {
                vfs_superblock_rlock(sb);
                dump_sb_inodes(sb);
                vfs_superblock_unlock(sb);
            }
        });
    });

    // SAFETY: matched with the `vfs_mount_lock()` above.
    unsafe { vfs_mount_unlock(); }
    crate::kprintln!("\n=== End of Inode Dump ===\n");
}
