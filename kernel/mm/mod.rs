//! Memory management subsystem (`kernel/mm/`).
//!
//! This module groups all the mm-internal submodules and re-exports
//! their public C-ABI symbols at this module's root so that the crate
//! root can pull them through one `pub use mm::*;`.
//!
//! Sibling subsystems (`machine`, `sync`, `lock`) live alongside `mm`
//! at the crate root — `mm` does not own them.

mod bits;
pub(crate) mod cffi;
pub(crate) mod page;
pub(crate) mod slab;
mod sysmm;
// pub(crate) so cross-file callers can name `crate::mm::kalloc::Kmem`
// directly (KERNEL-OO wave: kalloc.rs's top-level fns became `impl Kmem`
// associated fns, no longer glob-reexportable as bare free fns at every
// call site that needs the fully-qualified path).
pub(crate) mod kalloc;
// P3-D3c: `pub(crate)` so `dev/fdt.rs`/`start_kernel.rs` can import
// `early_alloc*` by crate path (their `extern "C"` redeclarations are gone).
pub(crate) mod early_allocator;
// P3-N6: `pub` so `build.rs`'s bindings-facade `pub use crate::mm::
// {pcache,vm}::...` re-exports can name them (same promotion the vfs
// modules got in P3-N5).
pub mod pcache;
mod vm_pgtab;
pub mod vm;
pub mod mm_safe;

// In-kernel runtime test suite, ported from the retired
// `test/src/ut_pcache_main.c` cmocka suite (Phase 4,
// docs/rustify/test_port_plan.md). Exposes a single
// `#[no_mangle] extern "C"` launch entry point (`pcache_launch_tests`),
// same pattern as `lock/{semaphore,rwsem}_test.rs`; only linked-in code,
// never called unless `kernel/start_kernel.rs`'s
// `#[cfg(feature = "pcache_test")]` gate is enabled.
pub mod pcache_test;

// Re-export the C-ABI exports so the crate root can flatten them.
pub use bits::*;
pub use page::*;
pub use slab::*;
pub use sysmm::*;
pub use kalloc::*;
pub use early_allocator::*;
pub use pcache::*;
pub use vm_pgtab::*;
pub use vm::*;
