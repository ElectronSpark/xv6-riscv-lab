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

#[path = "sync/sync.rs"]
mod sync;

#[path = "lock/mod.rs"]
mod lock;

#[path = "mm/mod.rs"]
mod mm;

#[path = "proc/mod.rs"]
mod proc;

// Re-export so the symbols end up in the staticlib's exported symbol table.
pub use mm::*;
pub use proc::*;
pub use lock::spinlock::*;
pub use lock::completion::*;
pub use lock::semaphore::*;
pub use lock::mutex::*;
pub use lock::rwsem::*;
pub use lock::rwlock::*;
pub use lock::rcu::*;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    // The code in this crate is currently panic-free (only const data tables).
    // If a panic ever reaches here, halt the hart.
    loop {
        unsafe { core::arch::asm!("wfi") }
    }
}
