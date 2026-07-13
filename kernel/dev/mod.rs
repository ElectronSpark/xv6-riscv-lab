//! Device subsystem core -- Rust port of `kernel/dev/dev.c` -> [`dev`],
//! `kernel/dev/cdev.c` -> [`cdev`], `kernel/dev/blkdev.c` -> [`blkdev`]
//! (Phase 2 Wave 21, see `docs/rustify/phase2_plan.md`).
//!
//! `dev` owns the kobject-backed, RCU-protected major/minor device
//! table; `cdev`/`blkdev` are thin dispatch layers on top of it for
//! character and block devices respectively (both embed `device_t` as
//! their first field and reinterpret-cast to it, matching the C
//! original's `(device_t *)dev` casts).
//!
//! Two pre-existing bugs (see `RUST_REWRITE.md`'s Known issues) are
//! fixed by this port -- documented in full in `dev`'s module doc:
//!
//! 1. `device_register()` never wrote an auto-assigned minor (the
//!    `minor == 0` sentinel) back into `dev->minor`, so devices like
//!    `/dev/tty` (registered with `minor = 0`) got a devtmpfs node
//!    encoding the wrong (invalid) minor number and could not be
//!    opened.
//! 2. `device_register()` silently discarded `devtmpfs_create_node()`'s
//!    return value.
//!
//! `kernel/dev/bio.c` -> [`bio`] (Phase 2 Wave 22, see
//! `docs/rustify/phase2_plan.md`): the Linux-style ref-counted block-I/O
//! request object (`struct bio` + `bio_vec` segments). **Not the same
//! layer** as `kernel/bio.c` -> `crate::bufcache` (the classic xv6
//! `struct buf` disk-block cache) -- see [`bio`]'s module doc for the
//! distinction.
//!
//! The rest of `kernel/dev/` (fdt.c, netdev.c, nullrand.c, the
//! x1_*/yt8531 drivers, dev_test.c) remains C for now -- see
//! `kernel/dev/CMakeLists.txt`.

pub mod dev;
pub mod cdev;
pub mod blkdev;
pub mod bio;
