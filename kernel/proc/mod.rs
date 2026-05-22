//! Rust port of `kernel/proc/`.
//!
//! Layout mirrors the C subdirectory: each submodule replaces one
//! `kernel/proc/*.c` file, exporting the same C ABI symbols (via
//! `#[no_mangle] extern "C"`) so the rest of the kernel keeps linking
//! unchanged. The corresponding `.c` is removed from
//! `kernel/proc/CMakeLists.txt` once its Rust port lands.
//!
//! Submodules
//! ----------
//! * [`cffi`]       — shared mirror types + extern declarations for the
//!                    C kernel symbols every submodule needs.
//! * [`sched_idle`] — port of `sched_idle.c` (idle scheduler class).
//! * [`sched_fifo`] — port of `sched_fifo.c` (FIFO scheduler class).

mod cffi;
mod proc_shims;
pub mod access;
mod sched_idle;
mod sched_fifo;
mod sysproc;
mod sys_signal;
mod pgroup;
mod pid;
mod clone;
mod exit;
mod workqueue;
mod rq_test;
mod thread_group;
mod thread_queue;
mod thread;
mod sched;
mod rq;
mod signal;
mod signal_ffi;

pub use sched_idle::*;
pub use sched_fifo::*;
pub use sysproc::*;
pub use sys_signal::*;
pub use pgroup::*;
pub use pid::*;
pub use clone::*;
pub use exit::*;
pub use workqueue::*;
pub use rq_test::*;
pub use thread_group::*;
pub use thread_queue::*;
pub use thread::*;
pub use sched::*;
pub use rq::*;
pub use signal::*;
pub use signal_ffi::*;
