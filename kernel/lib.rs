//! Rust components linked into the xv6-riscv kernel.
//!
//! This crate is compiled as a `staticlib` (`libxv6_rust.a`) for the
//! `riscv64gc-unknown-none-elf` target and linked into the kernel ELF.
//! It must remain `#![no_std]` and free of any runtime dependencies.
//!
//! ## Layout
//!
//! All Rust source lives under `kernel/`:
//!
//! * [`machine`] — low-level CSR / inline-asm / per-CPU primitives
//!   (`kernel/machine/`). The lowest layer; consumed by everything else.
//! * [`sync`] — RAII guards on top of the kernel lock primitives
//!   (`kernel/sync/`).
//! * [`lock`] — Rust ports of the kernel lock primitives
//!   (`kernel/lock/`).
//! * [`mm`] — memory management subsystem (`kernel/mm/`).
//!
//! `machine`, `sync`, `lock`, and `mm` are sibling modules of this
//! crate root — none of them is a submodule of another.

#![no_std]
#![allow(non_upper_case_globals)]
// `page` and `slab` both define private mirror types named `ListNode` and
// `Spinlock`. Re-exporting both modules with `pub use mod::*;` triggers
// ambiguous-glob warnings. The actual exported symbols come from the
// `#[no_mangle] pub unsafe extern "C" fn ...` declarations inside each
// module — not from these re-exports — so the collision is cosmetic.
#![allow(ambiguous_glob_reexports)]
// Bindgen emits `unsafe extern "C" { pub fn X(...) }` for every kernel symbol,
// while our hand-rolled `mod ffi`/`mod raw` blocks redeclare a subset using the
// Rust 2024 `unsafe extern "C" { pub safe fn X(...) }` form so that they are
// callable from safe code. Both declarations resolve to the same C symbol at
// link time — they only differ in Rust's view of caller-side safety — so the
// `clashing_extern_declarations` lint produces ~60 redundant warnings here.
#![allow(clashing_extern_declarations)]
// Bindgen generates several opaque anonymous-struct types (e.g. the empty
// `thread__bindgen_ty_1`) that appear as `*mut spinlock_t` / `*mut rwsem_t`
// arguments. Our hand-rolled `mod ffi`/`mod raw` blocks redeclare those same
// C functions; rustc then warns that the empty struct is not FFI-safe. The C
// side only ever sees opaque pointers, so the warning is noise here.
#![allow(improper_ctypes)]

/// Crate-level "blanket unsafe" shim: `u! { EXPR }` expands to
/// `unsafe { EXPR }`.
///
/// # Contract
///
/// This macro does not, by itself, make anything sound — it is a
/// call-site marker used throughout the older `kernel/proc/*.rs` C-ABI
/// ports (a holdover from the mechanical C→Rust conversion, predating
/// this crate's per-block `unsafe {}` + `// SAFETY:` discipline). Every
/// invocation site is required to carry its own `// SAFETY:` comment
/// (or, for a whole-function-body wrap where the body turns out to
/// contain no unsafe operation, the wrapper should be removed instead of
/// documented) explaining why the operations inside are sound — exactly
/// as for a hand-written `unsafe { ... }` block. See
/// `kernel/lock/mutex.rs` / `kernel/lock/rwsem.rs` for the canonical
/// style: SAFETY comments on the small, single-purpose unsafe blocks
/// (raw-pointer field projections, reborrows), not necessarily on a
/// pure pass-through wrapper around already-safe calls.
///
/// Several `kernel/lock/*.rs` files (not reachable from this
/// consolidation due to file-touch scoping in the pass that introduced
/// this macro) still carry their own identical local copy; this
/// crate-level definition is the canonical one for any *new* use.
#[macro_export]
macro_rules! u {
    ($($tokens:tt)*) => {
        unsafe { $($tokens)* }
    };
}

/// Auto-generated bindings for selected kernel C types and constants.
/// Produced at build time by `build.rs` via `bindgen` from `wrapper.h`.
#[allow(
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    dead_code,
    improper_ctypes,
    clippy::all
)]
pub mod bindings {
    include!(concat!(env!("OUT_DIR"), "/kernel_bindings.rs"));
}

#[path = "machine/machine.rs"]
mod machine;

#[path = "list.rs"]
pub mod list;

#[path = "bintree.rs"]
pub mod bintree;

#[path = "rbtree.rs"]
pub mod rbtree;

#[path = "hlist.rs"]
pub mod hlist;

#[path = "kobject.rs"]
pub mod kobject;

// kernel/{string,sbi}.rs (Phase 2 Wave 2 — leaf modules, ported from the
// .c files of the same name).
#[path = "string.rs"]
pub mod string;

#[path = "sbi.rs"]
pub mod sbi;

// kernel/start.rs (Phase 2 Wave 3 — entry.S's `call start` target and the
// `stack0` per-hart boot-stack array, ported from kernel/start.c).
#[path = "start.rs"]
pub mod start;

// kernel/uart.rs (Phase 2 Wave 4 — 16550/PXA UART driver, ported from
// kernel/uart.c).
#[path = "uart.rs"]
pub mod uart;

// kernel/console.rs (Phase 2 Wave 4 — console device layer, ported from
// kernel/console.c).
#[path = "console.rs"]
pub mod console;

// kernel/printf.rs (Phase 2 Wave 4 — printf/panic machinery, ported
// from kernel/printf.c; printf()'s variadic entry point stays in
// printf_shim.c).
#[path = "printf.rs"]
pub mod printf;

// kernel/backtrace.rs (Phase 2 Wave 5 — kernel symbol table + frame-pointer
// backtrace walking, ported from kernel/backtrace.c; needs the Wave 1
// bintree/rbtree modules).
#[path = "backtrace.rs"]
pub mod backtrace;

#[path = "sync/sync.rs"]
mod sync;

#[path = "lock/mod.rs"]
mod lock;

#[path = "mm/mod.rs"]
mod mm;

#[path = "proc/mod.rs"]
mod proc;

// kernel/irq/{irq_core,plic}.rs (Phase 2 Wave 5 — irq/plic.c + irq/irq.c)
// + kernel/irq/trap.rs (Phase 2 Wave 6 — irq/trap.c) + kernel/irq/syscall.rs
// (Phase 2 Wave 7 — irq/syscall.c). The `irq` module is now 100% Rust
// except kernelvec.S/trampoline.S (assembly, see kernel/irq/CMakeLists.txt).
#[path = "irq/mod.rs"]
mod irq;

// kernel/timer/{timer_core,sched_timer,goldfish_rtc}.rs (Phase 2 Wave 8 —
// timer/timer.c + timer/sched_timer.c + timer/goldfish_rtc.c). The `timer`
// module is now 100% Rust (it never had any assembly sources).
#[path = "timer/mod.rs"]
mod timer;

// kernel/ipi.rs (Phase 2 Wave 9 — ipi/ipi.c: IPI send/receive machinery,
// plus the `cpus[]` per-CPU array and cpus_init/mycpu_init/
// get_cpu_active_mask bring-up bookkeeping the C original shared the file
// with). The `ipi` directory now has no C sources left, see
// kernel/ipi/CMakeLists.txt.
#[path = "ipi.rs"]
mod ipi;

// kernel/tty/{termios,tty_dev,pty}.rs (Phase 2 Wave 10 — termios.c +
// tty_dev.c + pty.c) + kernel/tty/{tty,ptmx}.rs (Phase 2 Wave 11 —
// tty.c core line discipline + ptmx.c /dev/ptmx VFS glue). session.c
// remains C (Wave 12) and still lives alongside these files in
// kernel/tty/, see kernel/tty/CMakeLists.txt.
#[path = "tty/mod.rs"]
mod tty;

// kernel/vfs/inode.rs (Phase 2 Wave 13 -- vfs/inode.c) and
// kernel/vfs/{file,pipe}.rs (Phase 2 Wave 14 -- vfs/file.c + vfs/pipe.c).
// The rest of kernel/vfs/ (fs.c, fdtable.c, vfs_syscall.c, tmpfs/, xv6fs/,
// devtmpfs/) remains C for now; see kernel/vfs/CMakeLists.txt and
// kernel/vfs/mod.rs.
#[path = "vfs/mod.rs"]
mod vfs;

// kernel/dev/dev.rs (Phase 2 Wave 21 -- dev/dev.c: kobject-backed, RCU
// protected major/minor device table; includes two mandated bug fixes,
// see that module's doc) + kernel/dev/cdev.rs (dev/cdev.c: character
// device dispatch) + kernel/dev/blkdev.rs (dev/blkdev.c: block device
// dispatch + bio submission) + kernel/dev/bio.rs (Phase 2 Wave 22 --
// dev/bio.c: the Linux-style ref-counted block-I/O request object) +
// kernel/dev/fdt.rs (Phase 2 Wave 23 -- dev/fdt.c: the FDT parser) +
// kernel/dev/nullrand.rs + kernel/dev/netdev.rs (Phase 2 Wave 24) +
// kernel/dev/{yt8531,x1_emac,x1_sdhci}.rs (Phase 2 Wave 25 -- the
// Orange-Pi-RV2-only drivers, the last C in kernel/dev/; probe nothing
// on QEMU virt, see that module's doc). kernel/dev/ is now 100% Rust;
// see the NOTE in kernel/CMakeLists.txt (no kernel/dev/CMakeLists.txt).
#[path = "dev/mod.rs"]
mod dev;

// kernel/bufcache.rs (Phase 2 Wave 22 -- kernel/bio.c: the classic xv6
// `struct buf` disk-block cache: bread/bwrite/brelse/bwrite_async/
// bsync/bpin/bunpin, keyed by (dev, blockno) through the Wave-1 Rust
// hlist). NOT the same layer as kernel/dev/bio.c -> kernel/dev/bio.rs
// (dev::bio, the Linux-style bio request object above) -- see
// kernel/bufcache.rs's module doc for the distinction. Named
// bufcache.rs, not bio.rs, specifically to avoid a file-name collision
// with that sibling module; every C-ABI symbol it exports keeps its
// original name unchanged.
mod bufcache;

// Re-export so the symbols end up in the staticlib's exported symbol table.
pub use mm::*;
pub use proc::*;
pub use irq::irq_core::*;
pub use irq::plic::*;
pub use irq::syscall::*;
pub use irq::trap::*;
pub use backtrace::*;
pub use bintree::*;
pub use rbtree::*;
pub use hlist::*;
pub use kobject::*;
pub use string::*;
pub use sbi::*;
pub use start::*;
pub use uart::*;
pub use console::*;
pub use printf::*;
pub use lock::spinlock::*;
pub use lock::completion::*;
pub use lock::semaphore::*;
pub use lock::mutex::*;
pub use lock::rwsem::*;
pub use lock::rwlock::*;
pub use lock::rcu::*;
pub use ipi::*;
pub use tty::termios::*;
pub use tty::tty_dev::*;
pub use tty::pty::*;
pub use tty::tty::*;
pub use tty::ptmx::*;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    // The code in this crate is currently panic-free (only const data tables).
    // If a panic ever reaches here, halt the hart.
    loop {
        unsafe { core::arch::asm!("wfi") }
    }
}
