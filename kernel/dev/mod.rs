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
//! `kernel/dev/fdt.c` -> [`fdt`] (Phase 2 Wave 23, see
//! `docs/rustify/phase2_plan.md`): the Flattened Device Tree parser and
//! platform-config applier. Runs at the earliest point in boot (only
//! `early_allocator`/`kobject`/`printf` are up) -- see that module's own
//! doc comment for the full written-globals inventory and the two C bugs
//! discovered and preserved/fixed during the port.
//!
//! `kernel/dev/nullrand.c` -> [`nullrand`] and `kernel/dev/netdev.c` ->
//! [`netdev`] (Phase 2 Wave 24, see `docs/rustify/phase2_plan.md`):
//! `/dev/null`/`/dev/zero`/`/dev/random` cdev registrants, and the
//! network-device registration/lookup table `kernel/e1000.c` (still C)
//! and `kernel/net.c` consume by exact C ABI. `kernel/dev/dev_test.c`
//! (dead code -- its one call site was already commented out in
//! `start_kernel.c`) was deleted outright rather than ported, same
//! precedent as the tmpfs/xv6fs smoketests in Waves 18/19.
//!
//! `kernel/dev/yt8531.c` -> [`yt8531`], `kernel/dev/x1_emac.c` ->
//! [`x1_emac`], `kernel/dev/x1_sdhci.c` -> [`x1_sdhci`] (Phase 2 Wave 25,
//! see `docs/rustify/phase2_plan.md`): the Orange-Pi-RV2-only drivers --
//! the last C in `kernel/dev/`. Every entry point `start_kernel.c` calls
//! (`X1EmacSoftc::init`/`SdhciSoftc::init`) probes nothing on QEMU's `-machine
//! virt` (no matching FDT node) -- **compile-verify + boot-no-regression
//! only** for this trio, see each module's own doc for the full
//! lower-confidence accounting and what real hardware would be needed to
//! verify functionally.
//!
//! `kernel/dev/` is now 100% Rust -- `kernel/dev/CMakeLists.txt` and its
//! `add_subdirectory()` in `kernel/CMakeLists.txt` are removed (same
//! precedent as `lock/`, `timer/`, `ipi/`, `tty/`, `vfs/`).

pub mod dev;
pub mod cdev;
pub mod blkdev;
pub mod bio;
pub mod fdt;
pub mod nullrand;
pub mod netdev;
pub mod yt8531;
pub mod x1_emac;
pub mod x1_sdhci;
