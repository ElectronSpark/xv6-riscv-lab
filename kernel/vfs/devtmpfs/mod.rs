//! devtmpfs — Rust port of `kernel/vfs/devtmpfs/superblock.c` (Phase 2
//! Wave 20, see `docs/rustify/phase2_plan.md`). This is the **last C file
//! in the entire `kernel/vfs/` tree** — after this wave, vfs core (Waves
//! 13-17), tmpfs (Wave 18), xv6fs (Wave 19), and devtmpfs (this wave) are
//! all 100% Rust.
//!
//! devtmpfs reuses tmpfs's inode/dentry infrastructure (allocation,
//! directory hashing, the `vfs_inode_ops` vtable) but registers as its
//! own `vfs_fs_type` and auto-populates `/dev` with the device nodes
//! that have been registered so far via [`superblock::devtmpfs_create_node`].
//! See [`superblock`]'s module doc for the full design (registry list,
//! locking, the tmpfs-contract reuse via a local `unsafe extern "C"`
//! block, and the C-ABI surface still consumed by `kernel/dev/dev.c`
//! (still C, Wave 21) through `kernel/inc/devtmpfs.h`, which is
//! unchanged by this port).

pub mod superblock;
