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
pub(crate) mod proc_shims;
pub mod access;
mod sched_idle;
mod sched_fifo;
mod sysproc;
mod sys_signal;
// P3-N3: `pub(crate)` so `crate::bindings`' raw-line
// `pub use crate::proc::pgroup::Pgroup as pgroup;` redirect can path here.
pub(crate) mod pgroup;
mod pid;
mod clone;
mod exit;
// P3-N3: `pub(crate)` so `crate::bindings`' raw-line
// `pub use crate::proc::workqueue::Workqueue as workqueue;` (etc.)
// redirects can path here — same pattern as `thread_queue` (P3-N2).
pub(crate) mod workqueue;
// In-kernel runtime test suite for the real `workqueue` module (Phase 4,
// docs/rustify/test_port_plan.md), re-homing the retired
// `test/src/ut_workqueue_main.c` mock-only cmocka suite. Exposes a single
// `#[no_mangle] extern "C"` launch entry point
// (`workqueue_test_launch_tests`); only linked in, never called unless
// `kernel/start_kernel.rs`'s `#[cfg(feature = "workqueue_test")]` gate is
// enabled.
mod workqueue_test;
mod rq_test;
// P3-N3: `pub(crate)` so `crate::bindings`' raw-line
// `pub use crate::proc::thread_group::ThreadGroup as thread_group;`
// redirect can path here.
pub(crate) mod thread_group;
pub(crate) mod thread_queue;
// pub(crate) (P3-N9): `build.rs` injects `pub use crate::proc::thread::
// Thread as thread;` facade re-export into `crate::bindings`, which
// needs the module nameable from the crate root (N2/N3/N4 precedent).
pub(crate) mod thread;
// pub(crate) (P3-N7): `build.rs` injects `pub use crate::proc::sched::
// SchedClass as sched_class;` (+ sched_attr) and `pub use crate::proc::
// rq::Rq as rq;` (+ rq_percpu/sched_entity/load_weight) facade
// re-exports into `crate::bindings`, which needs the module paths to be
// visible from the crate root (same pattern as thread_queue/signal).
pub(crate) mod sched;
pub(crate) mod rq;
// pub(crate) (P3-N4): `build.rs` injects `pub use crate::proc::signal::
// SigAction as sigaction;` (+ sigacts/sigpending/ksiginfo/
// tg_shared_pending) facade re-exports into `crate::bindings`, which
// needs the module nameable from the crate root (N2/N3 precedent).
pub(crate) mod signal;

pub use sched_idle::*;
pub use sched_fifo::*;
pub use sysproc::*;
pub use sys_signal::*;
pub use pgroup::*;
pub use pid::*;
pub use clone::*;
pub use exit::*;
pub use workqueue::*;
pub use workqueue_test::*;
pub use rq_test::*;
pub use thread_group::*;
pub use thread_queue::*;
// P3-D3b: thread.rs's last `pub` items were demoted to `pub(crate)`
// with its `#[no_mangle]` exports gone, so the glob re-export is
// `pub(crate)` too (same set of names visible crate-wide as before).
pub(crate) use thread::*;
pub use sched::*;
pub use rq::*;
pub use signal::*;
