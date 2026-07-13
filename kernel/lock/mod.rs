//! Rust port of the kernel lock primitives (kernel/lock/*).
//!
//! Each submodule corresponds to a single C source file that previously
//! lived alongside this directory. The Rust port preserves the C ABI of
//! the exported symbols byte-for-byte so existing callers do not need
//! to be touched.

pub mod spinlock;
pub mod completion;
pub mod semaphore;
pub mod mutex;
pub mod rwsem;
pub mod rwlock;
pub mod rcu;

// Runtime test suites, ported 1:1 from the deleted `kernel/lock/*_test.c`
// files. Each module exposes a single `#[no_mangle] extern "C"` launch
// entry point, preserving the original symbol name for its C caller in
// `kernel/start_kernel.c`.
pub mod semaphore_test;
pub mod rwsem_test;
pub mod rcu_test;
