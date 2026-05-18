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
