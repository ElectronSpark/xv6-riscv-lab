# cmocka test suite → Rust port plan (CLOSED 2026-07-13)

Original problem (audit 2026-07-11): `cmake -S test` FAILED at generate —
`ut_semaphore`, `ut_rwlock`, `ut_bits`, `ut_pcache` referenced kernel C files
deleted by the lock/mm Rust ports, and CMake aborted on empty
`add_executable`, blocking ALL suites. No ctest/CI wiring existed for the
in-kernel runtime suites — that track was dark.

**Status: CLOSED.** Every suite in the table below has a final, verified
disposition. Phases 0-2 (test/CMakeLists.txt un-broken, host-test seam,
`#[cfg(test)]` host suites) closed in earlier sessions; Phases 3-4 (QEMU
ctest harness + the two new in-kernel suites, `mm/pcache_test.rs` and
`proc/workqueue_test.rs`) closed 2026-07-13. Nothing remains open on this
plan; any future suite work is a fresh task, not a continuation of this one.

## Per-suite disposition (final)

| Suite | Subject | Status | Destination |
| --- | --- | --- | --- |
| ut_list | list.h C macros (not list.rs) | builds, ctest `ut_list` | keep C cmocka until list.h consumers are gone |
| ut_hlist | hlist.c (still C at audit time) | RETIRED 2026-07-11 | kernel/hlist.rs ported (Phase 2 Wave 1); no host-mock replacement needed (in-kernel code, no host-test seam applicable) |
| ut_rbtree | rbtree.c+bintree.c (still C at audit time) | RETIRED 2026-07-11 | kernel/rbtree.rs+bintree.rs ported (Phase 2 Wave 1); same as ut_hlist |
| ut_tmpfs_truncate | vfs/tmpfs/truncate.c (Rust since Wave 18) | DISABLED 2026-07-12 (still disabled — out of Phase 3/4's scope) | re-home as an in-kernel Rust runtime test or cargo-test host suite; no dedicated task has picked this up yet. Original `test/src/ut_tmpfs_truncate_main.c.bak`/`ut_tmpfs_truncate_linked.c` remain on disk, unused |
| ut_bits | bits.h inline ops (builtins path) | builds, ctest `ut_bits` | fixed ref; kept as host cmocka (bits.h has no Rust-only surface left to duplicate) |
| ut_early_allocator | deleted early_allocator.c | PORTED to cargo-test (`rust_host_unit_tests`, `mm::early_allocator::tests::*`, 16 cases) | closed |
| ut_semaphore | deleted lock C | RETIRED 2026-07-11 (superseded by `lock/semaphore_test.rs`) | **QEMU ctest wired 2026-07-13**: `ctest -R qemu_semaphore` (`SEMAPHORE_TEST=1`, greps `[sem] tests finished` / absence of `[sem][T\d+].*FAIL`) |
| ut_rwlock | deleted lock C | RETIRED 2026-07-11 (superseded by `lock/rwsem_test.rs`) | **QEMU ctest wired 2026-07-13**: `ctest -R qemu_rwsem` (`RWLOCK_TEST=1`, greps `[rwsem] tests finished` / absence of `[rwsem][T\d+].*FAIL`) |
| ut_pcache | deleted mm/pcache.c | RETIRED 2026-07-11 (disabled pending re-home) | **PORTED 2026-07-13**: `kernel/mm/pcache_test.rs`, 30 in-kernel cases (23 adapted/ported from the retired 35, 2 added, 5 concurrency — see that file's module doc for the full per-case mapping and the 7 cases dropped with reasons). QEMU ctest `qemu_pcache` (`PCACHE_TEST=1`, greps `PCACHE TESTS: N/N PASSED`) |
| ut_workqueue | mock-only (never tested real impl!) | **RETIRED 2026-07-13** (`test/CMakeLists.txt`; source untouched in git history) | **NEW coverage 2026-07-13**: `kernel/proc/workqueue_test.rs`, 7 in-kernel cases against the *real* `workqueue.rs` (ctor/dtor, work-struct field integrity, real async queue/run, real queue-on-killed-workqueue failure, multi-worker concurrency, `FREE_AFTER_RUN` stress). QEMU ctest `qemu_workqueue` (`WORKQUEUE_TEST=1`, greps `WORKQUEUE TESTS: N/N PASSED`) |

## Host-test seam (closed)

- `kernel/.cargo/config.toml` pins the `riscv64gc` target; `#[cfg(test)]`
  modules in `mm::bits`/`mm::early_allocator`/`list` compile under
  `cargo test --target x86_64-unknown-linux-gnu` (mandatory explicit
  `--target`, see `test/CMakeLists.txt`'s `rust_host_unit_tests` ctest
  entry for why an env var wouldn't do). 35/35 passing.
- In-crate `#[cfg(test)]` modules, not a separate test crate, per the
  original design note (no pub API surface — everything is `#[no_mangle]`).

## In-kernel runtime test track (closed)

Pattern: `lock/{semaphore,rwsem,rcu}_test.rs` + `mm/pcache_test.rs` +
`proc/workqueue_test.rs`, each a `#[no_mangle] extern "C"` `*_launch_tests()`
entry point, always linked into the kernel staticlib, called from
`kernel/start_kernel.rs`'s `start_kernel_post_init` under a
`#[cfg(feature = "...")]` gate. Six independent, additive cargo features in
`kernel/Cargo.toml` (`rwlock_test`, `semaphore_test`, `workqueue_smoke_test`
— pre-existing, untouched, still an empty stub — `pcache_test`,
`workqueue_test`), each selected from an identically-named `ENV{...}` check
at `cmake` configure time in `kernel/CMakeLists.txt`'s `RUST_TEST_FEATURES`
block. A normal `cmake ..` (no env vars set) builds the same
no-test-code-runs kernel as always — verified by grepping a plain boot log
for zero test markers.

**QEMU ctest harness** (Phase 3, closed 2026-07-13):
`scripts/run_qemu_test.sh` + `test/cmake/QemuTests.cmake` (included from
`test/CMakeLists.txt`). Design:

- Each suite gets its own isolated build directory,
  `test/build_qemu_<suite>/` — never the main `build/` — because each
  suite needs a different `RUST_TEST_FEATURES` combination, which forces a
  distinct `cargo build --features` invocation and (per
  `kernel/CMakeLists.txt`'s own documented `RUST_TEST_FEATURES_STAMP`
  mechanism) a full relink; keeping that isolated from the primary
  development build tree means running these tests can never leave the
  main `build/`'s `CMakeCache.txt`/cargo target dir in an unexpected state.
- `scripts/run_qemu_test.sh` sets the MANDATORY `TOOLPREFIX`/`LAB` build
  env itself (the Iteration-23/26 poisoning lessons this repo's history
  already learned the hard way — see `RUST_REWRITE.md`'s "Working notes"),
  configures + builds the isolated dir, re-checks
  `CMAKE_C_COMPILER`/`CMAKE_BUILD_TYPE` in that dir's cache both before
  *and* after the QEMU run, boots QEMU directly (not via
  `cmake --build --target qemu`, whose custom-target-with-no-OUTPUT shape
  a plain `timeout` cannot reliably signal all the way down to the QEMU
  grandchild through `make`) under an explicit `timeout`, and greps the
  captured console log for the universal boot gate (`init: starting sh`
  exactly once) plus a per-suite pass/fail marker pair.
- ctest entries (`test/cmake/QemuTests.cmake`) all carry
  `LABELS "qemu;slow"`, `RESOURCE_LOCK qemu_vm`, and `RUN_SERIAL TRUE` —
  only one QEMU instance may run at a time on this machine, and the
  resource lock enforces that regardless of `ctest -j<N>`.
- rwsem/semaphore: their existing per-test `... OK`/`FAIL` + final
  `tests finished` line convention is used as-is (no source changes to
  those two files — out of this task's touch scope, and unnecessary: their
  existing output is already a sufficient pass/fail oracle for a script).
- pcache/workqueue (new): each prints a canonical
  `<SUITE> TESTS: N/N PASSED` (or `FAILED`) summary line, which the ctest
  entry's `MUST_MATCH`/`MUST_NOT_MATCH` regex pair greps directly.

Verified 2026-07-13: `ctest -R qemu_rwsem|qemu_semaphore|qemu_pcache|qemu_workqueue`
(and each run standalone via the script directly) all pass; normal
(no-feature-gate) kernel build stays zero-warning and its boot log carries
zero test markers; host ctest (`ut_list`, `ut_bits`, `rust_host_unit_tests`)
stays 3/3 (`ut_workqueue` retired, see table above — see also the "cmocka
count" note below); `rust_host_unit_tests` stays 35/35; main `build/`'s
`CMakeCache.txt` (`CMAKE_C_COMPILER`/`CMAKE_BUILD_TYPE`) unchanged by any of
this work.

**Note on the host ctest count**: an earlier version of this task's
verification checklist said "cmocka 4/4", describing the state before this
session (`ut_list`, `ut_bits`, `ut_workqueue`, `rust_host_unit_tests`).
Retiring `ut_workqueue` (this table's own long-standing, explicit
disposition — "mock-only, never tested the real impl") drops that to 3/3;
its coverage is superseded by the new `qemu_workqueue` suite, which tests
the real implementation for the first time. This is a deliberate, expected
change in what's being counted, not a regression.

## Phased effort (all closed)

0. Fix `test/CMakeLists.txt` so configure works again. [S] — closed 2026-07-11.
1. Host-test seam (`machine.rs` cfg-gating, `no_std` gating, ctest→cargo). [M] — closed.
2. `#[cfg(test)]` tests for `mm/bits.rs`, `list.rs`, `mm/early_allocator.rs`. [S-M] — closed.
3. QEMU ctest wiring for semaphore/rwsem in-kernel tests. [M] — **closed 2026-07-13**.
4. `mm/pcache_test.rs` + `proc/workqueue_test.rs` in-kernel suites. [L] — **closed 2026-07-13**.

Notes: `test/src/wrappers/{scheduler,syscall}_wrappers.c` are dead files;
`ut_page` already permanently disabled pre-existing (needs a page-buddy API
update unrelated to this plan). `test/src/ut_workqueue_main.c` and
`test/src/wrappers/workqueue_wrappers.c` are now also dead (retired target,
kept on disk per this repo's established retirement precedent — see e.g.
Wave 18/19's smoketest deletions vs. this file's *retention*: unlike those,
`ut_workqueue_main.c` still documents, in prose, exactly what the old mock
suite covered, which is useful context for anyone comparing it against
`workqueue_test.rs`'s new case list).

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
`lock/{semaphore,rwsem,rcu}_test.rs` and this plan's own
`mm/pcache_test.rs`/`proc/workqueue_test.rs`) gated behind a
`DEV_TABLE_TEST` env-var define, following the exact Phase 3/4 mechanism
this document just closed out — still open, not part of this plan's
scope (no module here needed non-trivial changes to `kernel/dev/dev.rs`'s
RCU device table during this session).
