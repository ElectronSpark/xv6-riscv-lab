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
mod kalloc;
mod early_allocator;
mod pcache;
mod vm_pgtab;
mod vm;
pub mod mm_safe;

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
