# Phase 3 — de-C-ification plan (audit 2026-07-13, baseline 4593c1a)

Goal: remove all C external code/interfaces, reduce unsafe, redesign key
parts with Rust features (smart pointers, traits), eliminate C patterns.

Baseline numbers: 1,438 `#[no_mangle]` exports (→ target ~15: asm-referenced
+ printf_shim + test-launch entries); 1,679 extern-block-imported fns (→ 0);
724 printf call sites across 63 files; 5,088 `unsafe` occurrences (top:
fdt.rs 348, x1_sdhci.rs 274, pcache.rs 250, vfs/inode.rs 242, access.rs 239);
615 `pub extern "C" fn` with raw-pointer params (the ~385 clippy
not_unsafe_ptr_arg_deref backlog dissolves with the mesh sweep); 36
MaybeUninit-static once-init sites in 23 files; 26 lock-guard types of which
only SlabBox carries Deref (data-carrying guards = biggest unsafe lever);
98 container_of sites; 1,226 c_int-returning fns vs 51 Result.

## Waves

- **P3-1 (master enabler) — STATUS: COMPLETE (2026-07-14, sub-wave D
  landed)**: module visibility (`pub(crate)`) + extern-block mesh removal
  + no_mangle demotion per caller survey (cross-check RUST_FORCE_UNDEFINED,
  .S files' symbol refs — the asm-mandated set: kerneltrap, kernel_irq,
  usertrap, usertrapret, user_kirq_entrance, __user_kirq_return,
  push_sigframe, restore_sigframe, start, stack0, start_kernel, swtch,
  timervec-equivalents...; printf_shim imports; test-launch symbols).
  Parameter types unchanged (that's P3-7).
  Sub-waves: (A) mm+lock+machine/sync/ll/list, (B) proc+irq+timer+ipi,
  (C) vfs+tty, (D) dev+drivers+root files + RUST_FORCE_UNDEFINED prune +
  lib.rs lint-allow removal.
  **Final crate-wide numbers (mesh sweep complete, all four sub-waves)**:
  `#[no_mangle]` 1,438 (Phase 3 baseline) → 664 (start of D) → 576 (D
  landed); hand-written `unsafe extern "C" { ... }` blocks 508 → 500 (D's
  own files' blocks mostly shrank rather than vanished — many still
  redeclare symbols from modules genuinely out of D's touch scope, e.g.
  `kobject_init`/`kmm_alloc`/`spin_lock`, left alone per the crate's
  established "don't touch a caller for a symbol you don't own" rule).
  `RUST_FORCE_UNDEFINED` 495 (start of D) → 22 (final prune, verified
  safe by relinking both kernel targets from the same `libxv6_rust.a`
  with the maximal current no_mangle set vs. the 22-entry keep-set and
  byte-diffing every `SHF_ALLOC` section — identical). Remaining
  `unsafe extern "C"` inventory crate-wide: printf_shim.c's own two
  imports (`printf_rust` + the reverse `printf`/`__printf_va_arg_*`
  direction), the 4 asm-mandated + `start`/`stack0` + libc-shaped
  mem*/str*/puts/putchar names, the `.S`-defined symbols (`swtch`,
  `__switch_noreturn`, `sig_trampoline`, `trampoline`/`uservec`/
  `userret`), and — the largest remaining category — same-crate
  cross-module calls in mm/lock/proc/vfs/tty files that are individually
  still `#[no_mangle]` for a documented reason (another Rust file not yet
  swept in *this* symbol's owning wave, or the "widely-shared data
  anchor" class: `cpus`, `platform`, `sockets`). Full accounting in the
  P3-1D worker report.
- **P3-2 (zero C)**: kprint!/kprintln! over core::fmt::Write on the existing
  console path (no alloc — panic-path-safe); scripted+reviewed rewrite of
  724 sites; panic/backtrace path as high-scrutiny solo pass; delete
  printf_shim.c + externs. → ZERO C in kernel.
- **P3-3**: bindgen-native types for ~40 kernel-internal type families
  (locks, pcache/slab/vm, rq/sched_entity, tq/workqueue, kobject,
  hlist/rb/list nodes, vfs_dentry/fs_struct, tmpfs_*, device/cdev/blkdev,
  netdev) with size/align/offset_of asserts before header deletion.
- **P3-4**: uabi (userspace ABI — extra scrutiny) + ondisk/virtio/e1000/pci
  DMA layouts (fence placement re-verified) as native #[repr(C)].
- **P3-5 (danger zone)**: replace gen_asm_offsets.py with a Rust
  offset_of!-based generator for trapframe/context/cpu_local; scripted
  old-vs-new header diff BEFORE any boot; testsig is the key gate.
- **P3-6**: delete wrapper.h + bindgen + unused kernel/inc headers (keep
  uabi/* consumed by user/mkfs).
- **P3-7**: raw-pointer params → &T/&mut T/Option<&T>/handles (dissolves the
  clippy backlog); start with the vfs/inode+pcache+fs+file+vfs_syscall
  cluster.
- **P3-8 (biggest unsafe lever)**: data-carrying lock guards — Lock<T> owns
  its data (UnsafeCell<T>), guards get Deref/DerefMut tied to guard
  lifetime; retrofit all 26 guard types; mm/vm+pcache first.
- **P3-9**: kobject → KArc<T: KobjectRelease> (Clone/Drop replacing
  get/put/try_get triples) for devices/inodes/sessions.
- **P3-10 (highest design risk, last redesign)**: (a) VFS ops + device ops →
  &'static dyn Trait (genuine runtime polymorphism; 29 static ops tables);
  (b) sched_class → enum SchedClass { Idle, Fifo }; tty_ops → enum dispatch
  (closed set); netdev/pcache ops → generic static dispatch. Verdict: of 8
  vtable families only 2 need dyn.
- **P3-11 (rolling)**: typed intrusive-collection wrapper (98 container_of
  sites); callback+void* → typed closures (workqueue/irq/timer — fix the
  documented timer retry_limit bug in the same pass); MaybeUninit statics →
  KOnceCell (36 sites); Result migration beyond mm (proc/vfs/tty/dev
  internal chains; syscall boundary keeps -errno).
- **P3-CS (USER-MANDATED, 2026-07-14, priority)**: (a) centralize the
  scattered std/libc-like helpers into one `kernel/kstd.rs` module — the
  ERR_PTR family (30 per-file copies!), container_of, Errno/neg_errno, one
  kassert!, one u!; per-file copies deleted. (b) **Result over ERR_PTR**:
  fallible Rust-internal fns migrate to `KResult<T> = Result<T, Errno>`
  (or Result<NonNull<T>, Errno>); the kstd ERR_PTR family is a
  TRANSITIONAL shim for C-ABI/storage boundaries only. This upgrades
  P3-11's Result item to a user directive and supersedes any wave design
  that would spread err-pointer returns further.
- **P3-12 (acceptance)**: re-run the unsafe census + clippy count, diff vs
  baseline — the measured "how much unsafe did we remove" report.

## Danger zones (every touching wave re-verifies)

- trapframe/context/cpu_local: asm-offset-locked; byte-identical boot-log
  discipline + gen_asm_offsets diff.
- uabi/*: userspace-silent-breakage class; size/offset diff before delete.
- xv6fs ondisk: silent corruption class; fs battery mandatory.
- virtio/e1000 DMA + fences: Wave-28 exactness must survive retyping
  (volatile + explicit fences, never plain field access).
- thread_queue.rs/rq.rs waves: re-run forkforkfork repro as courtesy check.

## Standard gates (unchanged)

Zero-warning build (+cargo clean once/wave), boot ×3+, mmaptest 16/16,
testsig 21/21, stressfs, usertests -q baseline (forkforkfork stop), host
cargo 35/35, 4 QEMU ctest suites, cache checks before AND after.
