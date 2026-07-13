# cmocka test suite → Rust port plan (audit 2026-07-11)

Current state: `cmake -S test` FAILS at generate — `ut_semaphore`,
`ut_rwlock`, `ut_bits`, `ut_pcache` reference kernel C files deleted by the
lock/mm Rust ports, and CMake aborts on empty add_executable, blocking ALL
suites. `ut_early_allocator_main.c` `#include`s the deleted
`kernel/mm/early_allocator.c`. No ctest/CI wiring exists — suite is dark.

## Per-suite disposition

| Suite | Subject | Status | Destination |
| --- | --- | --- | --- |
| ut_list | list.h C macros (not list.rs) | builds | keep C cmocka until list.h consumers are gone |
| ut_hlist | hlist.c (still C) | builds | keep C cmocka; retire when hlist.c ports (Phase-2 Wave 1) |
| ut_rbtree | rbtree.c+bintree.c (still C) | builds | keep C cmocka; retire at Wave 1 |
| ut_tmpfs_truncate | vfs/tmpfs/truncate.c (Rust since Wave 18) | disabled 2026-07-12 | re-home as in-kernel Rust runtime test or cargo-test host suite |
| ut_bits | bits.h inline ops (builtins path; never needed bits.c) | broken CMake ref only | fix ref now; add `#[cfg(test)]` tests in mm/bits.rs |
| ut_early_allocator | deleted early_allocator.c | broken | port to cargo-test once host seam exists; delete C test |
| ut_semaphore | deleted lock C | broken | DROP — superseded by in-kernel lock/semaphore_test.rs; wire QEMU ctest |
| ut_rwlock | deleted lock C | broken | DROP — superseded by lock/rwsem_test.rs; wire QEMU ctest |
| ut_pcache | deleted mm/pcache.c | broken | new in-kernel mm/pcache_test.rs (35 tests incl. 4 concurrency) — do NOT host-mock |
| ut_workqueue | mock-only (never tested real impl!) | builds but worthless | new in-kernel proc/workqueue_test.rs; retire C suite |

## Host-test seam (prerequisite for cargo-test track)

- kernel/.cargo/config.toml pins riscv64gc target; crate is #![no_std]
  staticlib with panic handler and unconditional RISC-V asm! in
  machine/machine.rs → `cargo test` impossible today.
- Seam: `#[cfg(target_arch = "riscv64")]` real vs host-mock machine module;
  `#![cfg_attr(not(test), no_std)]`; panic handler `#[cfg(not(test))]`;
  `host-test` feature; ctest entry runs `cargo test --target
  x86_64-unknown-linux-gnu` explicitly (must not inherit pinned target).
- In-crate `#[cfg(test)]` modules, NOT a separate test crate (no pub API
  surface — everything is #[no_mangle]).

## In-kernel runtime test track

Existing pattern: lock/{semaphore,rwsem,rcu}_test.rs +
`*_launch_tests()` called from start_kernel.c under SEMAPHORE_RUNTIME_TEST /
RWAD_WRITE_TEST defines (env vars SEMAPHORE_TEST/RWLOCK_TEST at configure).
Extend with mm/pcache_test.rs + proc/workqueue_test.rs and a ctest harness
that boots QEMU with the define, greps UART log for ALL TESTS PASSED marker.

## Phased effort

0. Fix test/CMakeLists.txt so configure works again (drop bits.c ref,
   comment out ut_semaphore/ut_rwlock/ut_pcache like ut_page, disable
   ut_early_allocator) — restores 5 suites. [S]
1. host-test seam (machine.rs cfg-gating, no_std gating, ctest→cargo). [M]
2. `#[cfg(test)]` tests for mm/bits.rs, list.rs, mm/early_allocator.rs. [S–M]
3. QEMU ctest wiring for semaphore/rwsem in-kernel tests. [M]
4. mm/pcache_test.rs + proc/workqueue_test.rs in-kernel suites. [L]

Notes: test/src/wrappers/{scheduler,syscall}_wrappers.c are dead files;
ut_page already permanently disabled pre-existing.

`kernel/vfs/tmpfs/tmpfs_smoketest.c`/`.h` (Phase 2 Wave 18): dead code
before this port started (call sites already commented out — see
Wave 16's report and `docs/rustify/phase2_plan.md` §0) — deleted
outright rather than ported. No test coverage lost (it was unreachable).

`kernel/dev/dev_test.c` (Phase 2 Wave 24): dead code — its one call site
(`dev_table_test();` in `kernel/start_kernel.c`) was already commented
out before this port started. Deleted outright, same precedent as the
tmpfs/xv6fs smoketests above, rather than porting ~450 lines of RCU
device-table stress tests (concurrent readers/writers, rapid
register/unregister, grace-period races) that nothing in the tree
exercises. Its coverage overlaps significantly with `user/devtest.c`
(userspace, exercised live at Wave 21 — all passed) for the
functional/ABI surface; the *stress* angle (concurrent RCU readers vs.
writers hammering `device_register`/`device_unregister`) has no current
equivalent. Re-home candidate: an in-kernel Rust runtime test module
(`kernel/dev/dev_test.rs`, same pattern as
`lock/{semaphore,rwsem,rcu}_test.rs`) gated behind a `DEV_TABLE_TEST`
env-var define, wired into `start_kernel.c`'s existing
`SEMAPHORE_TEST`/`RWLOCK_TEST`-style conditional-compile block — should
be picked up whenever `kernel/dev/dev.rs`'s RCU device table next gets
non-trivial changes, or as a dedicated small task otherwise.
