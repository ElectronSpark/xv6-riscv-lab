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
//!
//! ## Host-test seam (`cargo test`)
//!
//! `cargo build`/the CMake kernel build always compile this crate for
//! `riscv64gc-unknown-none-elf` as a `#![no_std]` staticlib (see
//! `.cargo/config.toml`'s pinned `[build] target` and `kernel/CMakeLists.txt`'s
//! `cargo build --target riscv64gc-unknown-none-elf` invocation). That build
//! is **entirely unaffected** by anything below: every item in this section
//! is behind `#[cfg(test)]`/`#[cfg(not(test))]`, and `cfg(test)` is never
//! true for an ordinary `cargo build` or the CMake custom command — only for
//! `cargo test`.
//!
//! `cargo test` needs a *host* binary (`cargo test --target
//! x86_64-unknown-linux-gnu`, since `.cargo/config.toml` otherwise pins the
//! bare riscv target — see that file's comment). Two things stand in the
//! way of that:
//!
//! 1. `#![no_std]` — lifted under `cfg(test)` via `#![cfg_attr(not(test),
//!    no_std)]` below, so `cargo test` gets an ordinary `std` build (test
//!    harness, `Vec`/`Box`/`vec!` available to test code) while the real
//!    kernel build is untouched.
//! 2. Riscv-only `core::arch::asm!` bodies scattered through the crate.
//!    Most of the crate (proc/irq/vfs/dev/tty/timer/ipi/the block+network
//!    drivers/…) calls into these, directly or transitively, and none of it
//!    has ever been built for any target other than riscv64 — porting all of
//!    it to be host-buildable is a much larger undertaking than this seam
//!    (see `docs/rustify/test_port_plan.md`'s Phase 3/4). This crate instead
//!    takes the smallest change that unblocks real host-test coverage today:
//!    * [`machine`] (this crate's *only* concentrated point of raw
//!      arch-specific asm primitives) gets a `#[cfg(target_arch =
//!      "riscv64")]` real body / `#[cfg(not(target_arch = "riscv64"))]`
//!      host-mock sibling for each of its asm-backed primitives (documented
//!      per-function in that file). Everything else in `machine` (the typed
//!      `CpuLocal`/`ThreadRef`/list-entry helpers, which are plain pointer
//!      code, no asm) is unconditionally portable and needs no gating.
//!    * The module tree actually compiled under `cfg(test)` is a *reduced*
//!      set: [`bindings`], [`machine`], [`list`], and a `cfg(test)`-only
//!      inline `mm` shim (declared further down) that exposes exactly
//!      `mm::bits` and `mm::early_allocator` — the three suites covered by
//!      this phase (see `docs/rustify/test_port_plan.md` Phase 2) — plus a
//!      small hand-written mock of the *pure* (non-asm) subset of
//!      `mm::cffi` that `early_allocator.rs` needs (`ListNode` +
//!      `xv6_list_{init,is_empty,push_front,pop_front}` + `panic_bytes`).
//!      The real `mm/cffi.rs` is never touched or compiled for host tests —
//!      it also contains a few `core::arch::asm!`-backed primitives
//!      (`xv6_cpuid`/`xv6_push_off`/`xv6_pop_off`, unrelated to
//!      `early_allocator.rs`) that would otherwise block a host build of
//!      that file; reimplementing the four pure list primitives (identical
//!      logic, no asm) sidesteps that without editing the real module.
//!      Every other module (proc, irq, vfs, dev, tty, timer, ipi, the block
//!      + network drivers, sync, lock, mm's other submodules, …) is
//!      `#[cfg(not(test))]`-gated out of the `cargo test` build entirely —
//!      each has its own local riscv asm and/or deep transitive
//!      dependencies on `machine`'s per-CPU state, and bringing all of that
//!      to a host target is out of this phase's scope (would require
//!      touching dozens of already-ported files well beyond this task's
//!      touch-list). This is a deliberate, documented scope decision, not an
//!      oversight — see the worker report for the phase that introduced this
//!      seam.
//!
//! Bindgen (`build.rs`) needs **no changes** for this: it already generates
//! `bindings` by pointing libclang at the riscv64/lp64d target explicitly
//! (`clang_arg("-target").clang_arg("riscv64-unknown-elf")`, independent of
//! whatever target `cargo` itself is building for), so the emitted struct
//! layouts are identical LP64 layouts whether the *build script* runs to
//! produce a riscv or a host artifact — a host `cargo test` reuses the exact
//! same generated `kernel_bindings.rs` shape unchanged.

#![cfg_attr(not(test), no_std)]
#![allow(non_upper_case_globals)]
// P3-1D re-audit: tried removing this; still fires 81 times, entirely
// inside `mm/mod.rs`'s and `proc/mod.rs`'s own `pub use submodule::*;`
// re-export lists (not this file's own re-exports) -- e.g. `mm::page`/
// `mm::pcache` both defining `Page`, `proc::sysproc`/`proc::pid`/
// `proc::sys_signal`/`proc::workqueue`/`proc::thread_queue` all defining
// local mirrors of `argint`/`argaddr`/`exit`/`xv6_current_thread`/etc.
// Every one of those name collisions predates this wave (mm/proc were
// P3-1A/B territory) and the actual exported symbol comes from each
// site's own `#[no_mangle]` declaration, not from which glob wins the
// name -- the collision is cosmetic, matching the original comment's
// reasoning, just relocated (it was never really about `page`/`slab`'s
// `ListNode`/`Spinlock`, that specific pair no longer collides).
// Untangling the mm/proc submodule re-export structure is out of this
// wave's touch scope.
#![allow(ambiguous_glob_reexports)]
// P3-1D re-audit: down from ~60 warnings (pre-mesh-sweep) to exactly one
// clash class now that the mesh sweep is crate-wide complete. Bindgen's
// generated declaration for `printf` (`kernel_bindings.rs`, from the real
// C prototype `int printf(char *fmt, ...)`, non-`const`) takes `*mut
// c_char`; this crate's ~70 hand-written per-file `fn printf(fmt: *const
// c_char, ...)` local redeclarations (the established convention, since
// this function never writes through `fmt`) all agree with each other
// but not with bindgen's -- both resolve to the same C symbol at link
// time, only the pointer const-ness/caller-side-safety view differs.
// Narrowing every one of the ~70 call sites to bindgen's `*mut` would
// widen their signature for no behavioural gain (the read-only
// convention is intentional) and is out of this wave's touch scope.
#![allow(clashing_extern_declarations)]
// P3-1D re-audit: tried removing this too -- still fires 382 times (up
// from ~60 pre-mesh-sweep denominator context; the mesh sweep didn't
// touch this class at all, it's unrelated to extern-block redeclaration
// count). Bindgen generates several opaque anonymous-struct types (e.g.
// the empty `thread__bindgen_ty_1`) that appear as `*mut spinlock_t` /
// `*mut rwsem_t` arguments. Our hand-rolled `mod ffi`/`mod raw` blocks
// redeclare those same C functions; rustc then warns that the empty
// struct is not FFI-safe. The C side only ever sees opaque pointers, so
// the warning is noise here. Fixing this at the root (real bindgen
// layouts for every lock-primitive type family, replacing the opaque
// stand-ins) is explicitly P3-3's charter ("bindgen-native types for
// ~40 kernel-internal type families"), not this wave's.
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
/// This is the crate's one canonical definition; `kernel/lock/{mutex,
/// rwsem,completion,semaphore}.rs` used to carry their own identical
/// local copies (a file-touch-scoping leftover from the pass that
/// introduced this macro) — removed in P3-CS1, which also added
/// `use crate::u;` to each of those files.
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

// `machine` and `list` are the only two "real" (`#[path = ...]` onto the
// actual production file) modules kept unconditional -- both are made fully
// host-portable (see `machine.rs`'s own doc for its `cfg(target_arch)` seam;
// `list.rs` has no arch-specific code at all, only a dependency on
// `bindings`, which is itself host-portable -- see the crate doc above).
#[path = "machine/machine.rs"]
mod machine;

#[path = "list.rs"]
pub mod list;

// kernel/kstd.rs — the kernel's "std"/prelude: canonical ERR_PTR family,
// `container_of`, `Errno`/`neg_errno`, `kassert!` (Phase 3 wave P3-CS1,
// see that module's doc for the centralization rationale). Depends only
// on `bindings` (unconditional) and the `xv6_panic` C-ABI symbol, so it
// follows the same `#[cfg(not(test))]` gating as every module below it
// that isn't part of the host-test seam.
#[cfg(not(test))]
#[path = "kstd.rs"]
pub(crate) mod kstd;

// Every module below this point is excluded from the `cargo test` (host)
// build -- see the crate-level doc comment's "Host-test seam" section for
// why. None of this is reachable from `cfg(test)` code; the `mm` shim a
// little further down is the `cfg(test)` replacement for exactly the two
// submodules ([`mm::bits`], [`mm::early_allocator`]) this phase's suites
// need.
#[cfg(not(test))]
#[path = "bintree.rs"]
pub mod bintree;

#[cfg(not(test))]
#[path = "rbtree.rs"]
pub mod rbtree;

#[cfg(not(test))]
#[path = "hlist.rs"]
pub mod hlist;

#[cfg(not(test))]
#[path = "kobject.rs"]
pub mod kobject;

// kernel/{string,sbi}.rs (Phase 2 Wave 2 — leaf modules, ported from the
// .c files of the same name).
#[cfg(not(test))]
#[path = "string.rs"]
pub mod string;

#[cfg(not(test))]
#[path = "sbi.rs"]
pub mod sbi;

// kernel/start.rs (Phase 2 Wave 3 — entry.S's `call start` target and the
// `stack0` per-hart boot-stack array, ported from kernel/start.c).
#[cfg(not(test))]
#[path = "start.rs"]
pub mod start;

// kernel/uart.rs (Phase 2 Wave 4 — 16550/PXA UART driver, ported from
// kernel/uart.c).
#[cfg(not(test))]
#[path = "uart.rs"]
pub mod uart;

// kernel/console.rs (Phase 2 Wave 4 — console device layer, ported from
// kernel/console.c).
#[cfg(not(test))]
#[path = "console.rs"]
pub mod console;

// kernel/printf.rs (Phase 2 Wave 4 — printf/panic machinery, ported
// from kernel/printf.c; printf()'s variadic entry point stays in
// printf_shim.c).
#[cfg(not(test))]
#[path = "printf.rs"]
pub mod printf;

// kernel/backtrace.rs (Phase 2 Wave 5 — kernel symbol table + frame-pointer
// backtrace walking, ported from kernel/backtrace.c; needs the Wave 1
// bintree/rbtree modules).
#[cfg(not(test))]
#[path = "backtrace.rs"]
pub mod backtrace;

#[cfg(not(test))]
#[path = "sync/sync.rs"]
mod sync;

#[cfg(not(test))]
#[path = "lock/mod.rs"]
mod lock;

#[cfg(not(test))]
#[path = "mm/mod.rs"]
mod mm;

// `cfg(test)`-only replacement for `mm`: exposes exactly the two submodules
// this phase's host-test suites cover, `bits` and `early_allocator`, at the
// same `crate::mm::*` path the real module uses (so neither ported file
// needed a single import changed). See the crate doc's "Host-test seam"
// section for why this exists instead of `#[path = "mm/mod.rs"] mod mm;`
// unconditionally.
#[cfg(test)]
mod mm {
    // NOTE: paths here are relative to `mm/` (this inline module's implied
    // directory, following the same "child modules resolve under a
    // directory named after their parent" rule file-based `mod mm;` would
    // use), not to `kernel/` -- i.e. this is `kernel/mm/bits.rs`, not
    // `kernel/mm/mm/bits.rs`.
    #[path = "bits.rs"]
    pub mod bits;

    #[path = "early_allocator.rs"]
    pub mod early_allocator;

    /// Host-only mock of the small, architecture-independent subset of the
    /// real `crate::mm::cffi` (see `mm/cffi.rs`) that `mm::early_allocator`
    /// needs: the intrusive-list primitives and the panic helper. The real
    /// `cffi.rs` is never compiled here -- besides the two things below, it
    /// also defines `xv6_cpuid`/`xv6_push_off`/`xv6_pop_off`, each backed by
    /// `core::arch::asm!` reading riscv CSRs/`tp`, which `early_allocator.rs`
    /// never calls but which would otherwise block a host build of that
    /// file wholesale. Every function below is a byte-for-byte reimplementation
    /// of its real `cffi.rs` counterpart (plain circular-doubly-linked-list
    /// pointer surgery, no arch-specific code to begin with) -- this mock
    /// changes no behavior, it only avoids pulling in the asm-bearing
    /// neighbors in the same file.
    pub mod cffi {
        use core::ptr;

        /// Mirror of `crate::mm::cffi::ListNode` (same `#[repr(C)]` layout:
        /// two raw pointers). A distinct type on purpose -- this mock is
        /// never used in the same build as the real `cffi.rs`, so there is
        /// no risk of the two being confused for one another.
        #[repr(C)]
        pub struct ListNode {
            pub prev: *mut ListNode,
            pub next: *mut ListNode,
        }

        impl ListNode {
            #[inline]
            pub const fn new() -> Self {
                Self { prev: ptr::null_mut(), next: ptr::null_mut() }
            }
        }

        pub mod raw {
            use super::ListNode;

            /// SAFETY (all four functions below): callers pass a valid,
            /// exclusively-accessible `ListNode` -- identical contract to
            /// the real `cffi::raw` functions of the same name.
            pub fn xv6_list_init(entry: *mut ListNode) {
                unsafe {
                    (*entry).next = entry;
                    (*entry).prev = entry;
                }
            }

            pub fn xv6_list_is_empty(head: *const ListNode) -> core::ffi::c_int {
                unsafe { if (*head).next == head as *mut ListNode { 1 } else { 0 } }
            }

            fn list_insert_after(prev: *mut ListNode, new: *mut ListNode) {
                unsafe {
                    let nxt = (*prev).next;
                    (*new).next = nxt;
                    (*new).prev = prev;
                    (*nxt).prev = new;
                    (*prev).next = new;
                }
            }

            pub fn xv6_list_push_front(head: *mut ListNode, entry: *mut ListNode) {
                list_insert_after(head, entry);
            }

            pub fn xv6_list_pop_front(head: *mut ListNode) -> *mut ListNode {
                if xv6_list_is_empty(head) != 0 {
                    return core::ptr::null_mut();
                }
                let first = unsafe { (*head).next };
                unsafe {
                    let p = (*first).prev;
                    let n = (*first).next;
                    (*p).next = n;
                    (*n).prev = p;
                }
                xv6_list_init(first);
                first
            }
        }

        /// Host mock of `crate::mm::cffi::panic_bytes`. The real function
        /// prints via the kernel's `printf`/`__panic_end` and halts the
        /// hart; on the host there is no hart to halt and no kernel
        /// `printf`, so this raises an ordinary Rust panic carrying the same
        /// message instead -- letting `#[should_panic]` host tests observe
        /// `early_allocator.rs`'s guard-rail paths (invalid range, bad
        /// alignment, corrupted chunk magic) directly, which the real
        /// panic-and-halt path cannot be observed from within a test at
        /// all.
        pub fn panic_bytes(msg: &[u8]) -> ! {
            let text = msg.strip_suffix(b"\0").unwrap_or(msg);
            panic!(
                "{}",
                core::str::from_utf8(text).unwrap_or("early_allocator: fatal (non-utf8 message)")
            );
        }
    }
}

#[cfg(not(test))]
#[path = "proc/mod.rs"]
mod proc;

// kernel/irq/{irq_core,plic}.rs (Phase 2 Wave 5 — irq/plic.c + irq/irq.c)
// + kernel/irq/trap.rs (Phase 2 Wave 6 — irq/trap.c) + kernel/irq/syscall.rs
// (Phase 2 Wave 7 — irq/syscall.c). The `irq` module is now 100% Rust
// except kernelvec.S/trampoline.S (assembly, see kernel/irq/CMakeLists.txt).
#[cfg(not(test))]
#[path = "irq/mod.rs"]
mod irq;

// kernel/timer/{timer_core,sched_timer,goldfish_rtc}.rs (Phase 2 Wave 8 —
// timer/timer.c + timer/sched_timer.c + timer/goldfish_rtc.c). The `timer`
// module is now 100% Rust (it never had any assembly sources).
#[cfg(not(test))]
#[path = "timer/mod.rs"]
mod timer;

// kernel/ipi.rs (Phase 2 Wave 9 — ipi/ipi.c: IPI send/receive machinery,
// plus the `cpus[]` per-CPU array and cpus_init/mycpu_init/
// get_cpu_active_mask bring-up bookkeeping the C original shared the file
// with). The `ipi` directory now has no C sources left, see
// kernel/ipi/CMakeLists.txt.
#[cfg(not(test))]
#[path = "ipi.rs"]
mod ipi;

// kernel/tty/{termios,tty_dev,pty}.rs (Phase 2 Wave 10 — termios.c +
// tty_dev.c + pty.c) + kernel/tty/{tty,ptmx}.rs (Phase 2 Wave 11 —
// tty.c core line discipline + ptmx.c /dev/ptmx VFS glue). session.c
// remains C (Wave 12) and still lives alongside these files in
// kernel/tty/, see kernel/tty/CMakeLists.txt.
#[cfg(not(test))]
#[path = "tty/mod.rs"]
mod tty;

// kernel/vfs/inode.rs (Phase 2 Wave 13 -- vfs/inode.c) and
// kernel/vfs/{file,pipe}.rs (Phase 2 Wave 14 -- vfs/file.c + vfs/pipe.c).
// The rest of kernel/vfs/ (fs.c, fdtable.c, vfs_syscall.c, tmpfs/, xv6fs/,
// devtmpfs/) remains C for now; see kernel/vfs/CMakeLists.txt and
// kernel/vfs/mod.rs.
#[cfg(not(test))]
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
#[cfg(not(test))]
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
#[cfg(not(test))]
mod bufcache;

// kernel/exec.rs (Phase 2 Wave 26 -- exec.c: ELF loader + argv/envp stack
// machinery + sys_exec, ported now that the whole vfs tree and mm/vm.rs
// are Rust). Sits directly in kernel/ (no subdirectory), matching
// bufcache's own precedent -- no `#[path = ...]` needed since the module
// name matches the file name. `exec`/`sys_exec` are called only from
// other same-crate Rust (`proc/thread.rs`, `irq/syscall.rs`), which
// declare their own local `extern "C"` for them, so no `pub use exec::*`
// is needed either (same precedent as `vfs`/`dev`/`tty`/`bufcache`).
#[cfg(not(test))]
mod exec;

// kernel/start_kernel.rs (Phase 2 Wave 27 -- start_kernel.c: the boot
// orchestrator that calls nearly every other subsystem's `_init()` entry
// point in a documented, invariant order; also the canonical definition
// site of `__physical_memory_start`/`__physical_memory_end`/
// `__physical_total_pages`, extern-declared by many other files). Sits
// directly in kernel/ (no subdirectory), same precedent as bufcache.rs/
// exec.rs -- no `pub use` needed, its `#[no_mangle]` C-ABI symbols
// (`start_kernel`, called by `start.rs`; `start_kernel_post_init`, called
// by `proc/thread.rs`) are found via the exported symbol table regardless
// of Rust-level visibility.
#[cfg(not(test))]
mod start_kernel;

// kernel/virtio_disk.rs + kernel/ramdisk.rs (Phase 2 Wave 28, sub-wave A
// -- the final porting wave's block drivers, ported from
// kernel/virtio_disk.c + kernel/ramdisk.c). Sit directly in kernel/ (no
// subdirectory), same precedent as bufcache.rs/exec.rs/start_kernel.rs --
// no `pub use` needed, their `#[no_mangle]` C-ABI symbols
// (`virtio_disk_init`/`ramdisk_init`, called by `start_kernel.rs`;
// `__virtio_mmio_base`/`__virtio_irqno`, extern-declared by
// `dev/fdt.rs`/`mm/vm_pgtab.rs`) are found via the exported symbol table
// regardless of Rust-level visibility.
#[cfg(not(test))]
mod virtio_disk;
#[cfg(not(test))]
mod ramdisk;

// kernel/pci.rs + kernel/e1000.rs + kernel/net.rs + kernel/sysnet.rs
// (Phase 2 Wave 28, sub-wave B -- the final porting wave's network
// drivers, ported from kernel/pci.c + kernel/e1000.c + kernel/net.c +
// kernel/sysnet.c; after this wave the only C left under kernel/ is
// printf_shim.c). Same no-subdirectory/no-`pub use` precedent as above;
// `pci_init`/`sockinit` are called by `start_kernel.rs`,
// `__pcie_ecam_mmio_base`/`__e1000_pci_mmio_base` are extern-declared by
// `mm/vm_pgtab.rs`, `SOCKETS` (Wave P3-8d: `sock_lock`/`sockets` migrated
// to one `crate::sync::SpinLock<*mut sock>`) is locked directly by
// `vfs/file.rs` via a plain crate-path `use` (see `sysnet.rs`'s module
// doc), and `mbufalloc`/`mbuffree`/`net_rx` are extern-declared by
// `dev/x1_emac.rs`.
#[cfg(not(test))]
mod pci;
#[cfg(not(test))]
mod e1000;
#[cfg(not(test))]
mod net;
#[cfg(not(test))]
mod sysnet;

// Re-export so the symbols end up in the staticlib's exported symbol table.
// Entirely `cfg(not(test))`: none of these modules exist in the `cfg(test)`
// tree (see the crate doc's "Host-test seam" section), and host tests don't
// need a C-ABI exported symbol table at all -- they call into `mm::bits`/
// `mm::early_allocator`/`list` directly by Rust path.
#[cfg(not(test))]
pub use mm::*;
#[cfg(not(test))]
pub use proc::*;
#[cfg(not(test))]
pub use irq::irq_core::*;
#[cfg(not(test))]
pub use irq::plic::*;
#[cfg(not(test))]
pub use irq::syscall::*;
#[cfg(not(test))]
pub use irq::trap::*;
#[cfg(not(test))]
pub use backtrace::*;
#[cfg(not(test))]
pub use bintree::*;
#[cfg(not(test))]
pub use rbtree::*;
#[cfg(not(test))]
pub use hlist::*;
#[cfg(not(test))]
pub use kobject::*;
#[cfg(not(test))]
pub use string::*;
#[cfg(not(test))]
pub use sbi::*;
#[cfg(not(test))]
pub use start::*;
#[cfg(not(test))]
pub use uart::*;
#[cfg(not(test))]
pub use console::*;
#[cfg(not(test))]
pub use printf::*;
#[cfg(not(test))]
pub use lock::spinlock::*;
#[cfg(not(test))]
pub use lock::completion::*;
#[cfg(not(test))]
pub use lock::semaphore::*;
#[cfg(not(test))]
pub use lock::mutex::*;
#[cfg(not(test))]
pub use lock::rwsem::*;
#[cfg(not(test))]
pub use lock::rwlock::*;
#[cfg(not(test))]
pub use lock::rcu::*;
#[cfg(not(test))]
pub use ipi::*;
#[cfg(not(test))]
pub use tty::termios::*;
#[cfg(not(test))]
pub use tty::tty_dev::*;
#[cfg(not(test))]
pub use tty::pty::*;
#[cfg(not(test))]
pub use tty::tty::*;
#[cfg(not(test))]
pub use tty::ptmx::*;

// `cfg(test)` never reaches this: `cargo test` links a normal `std` test
// harness binary with its own panic runtime (see the crate doc's
// "Host-test seam" section), so this crate must not also register one --
// `#[panic_handler]` is only legal (and only needed) in the real `no_std`
// kernel build.
#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    // The code in this crate is currently panic-free (only const data tables).
    // If a panic ever reaches here, halt the hart.
    loop {
        unsafe { core::arch::asm!("wfi") }
    }
}
