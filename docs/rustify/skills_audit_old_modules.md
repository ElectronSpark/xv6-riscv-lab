# rust-skills compliance audit — older modules (lock, proc, machine, sync, ll, list) — 2026-07-11

Verdict: corrective dispatch warranted, 3 packages. lock/ + sync.rs structure
is solid; risk concentrates in proc/access.rs, the undisciplined `u!{}`
macro in proc/, and one real concurrency bug in rq.rs.

## HIGH findings

- **H-01** proc/access.rs (162 unsafe blocks, 0 SAFETY comments): document
  the raw_get!/raw_set!/raw_mut_ptr!/bit_get!/bit_set! macros (one SAFETY
  contract each at definition) + `# Safety` on `unsafe fn from_raw`.
- **H-02** proc/access.rs ~20 `assume()` constructors: safe fn doing
  `unsafe { from_raw(p).unwrap_unchecked() }` — UB if p null; ~150 call
  sites, none justified. Fix: make `assume` an `unsafe fn` with `# Safety`
  docs (update call sites), or keep safe with debug_assert + documented
  invariant.
- **H-03** proc/access.rs:104: `pub fn zeroed<T>() -> T` — safe,
  unconstrained generic mem::zeroed. UB for &T/NonNull/niche enums. Only
  instantiated with POD `sched_attr` today (latent). Fix: make `unsafe fn`
  with `# Safety` ("all-zero bit pattern must be valid for T") or bound it.
- **H-04** sync/sync.rs: NONE of the 9 RAII guards (KSpinGuard,
  KSpinPairGuard, KMutexGuard, KRwsem*Guard, KRwlock*Guard incl. Irq
  variants) have `#[must_use]` — `lock();` with dropped guard compiles
  silently. Lower-level guards in lock/*.rs already have it. Fix: add
  `#[must_use = "lock is released immediately if the guard is dropped"]`.
- **H-05** proc/rq.rs:105-111,830-841 — **real data race**:
  `RqGlobal::active_cpu_mask` is plain u64 in an UnsafeCell; `|=` RMW from
  `rq_cpu_activate` (per-CPU bring-up, no lock) + unlocked cross-CPU reads.
  Fix: AtomicU64 (fetch_or/load, Relaxed unless it must synchronize other
  state — worker to analyze), stop handing out `&mut RqGlobal` for
  cross-CPU fields.

## MEDIUM findings

- **M-01/M-02** `u!{}` blanket-unsafe macro: disciplined in lock/ (SAFETY
  before each use) but 0-justification in signal.rs (39), thread.rs (21),
  thread_group.rs (9), proc_shims.rs (~115 of 126). Backfill SAFETY
  comments; consider one crate-level macro definition instead of 8 copies.
- **M-03** `xxx_lock_ref(p).unwrap().lock()` at FFI entry points
  (thread.rs:353+, sched.rs:376+, signal.rs:482+, thread_queue.rs:397+) —
  replace unwrap with descriptive expect or explicit null-check panic.
- **M-04** 6+ `unsafe impl Sync for XxxCell` without justification
  (rq.rs:83,95; thread_group.rs:208; sched.rs:188; workqueue.rs:128;
  thread_queue.rs:120; signal.rs:328). Add one-line SAFETY stating the
  serialization mechanism (cf. proc_shims.rs:1366 ProcTableCell).
- **M-05** machine.rs:828-886 atomic-reinterpret casts lack the layout
  justification that spinlock.rs:54-58 has — add same comment or factor
  through the documented helper.

## LOW findings

- **L-01** sched_fifo.rs:40,44 get_unchecked on class index — add SAFETY or
  use safe indexing (not hot enough to matter).
- **L-02** list.rs:40,52 — one-line SAFETY refs to ListIterator::new's docs.
- **L-03** private `*_impl -> c_int` helpers could be Result internally —
  idiom note only, no action.

## Dispatch packages

- **Package A**: access.rs remediation (H-01, H-02, H-03 + SpinLockGuard
  must_use). Single file + call-site updates. Largest.
- **Package B**: u!{} discipline backfill (M-01, M-02) + rq.rs race fix
  (H-05). proc/ subsystem; race fix needs careful scheduler review.
- **Package C**: sync.rs must_use (H-04), M-03 expect messages, M-04 Sync
  comments, M-05 machine.rs comments, L-01/L-02. Low-risk quick wins.

Positive examples to imitate: lock/spinlock.rs (8/8 SAFETY, per-op
orderings mapped to C builtins), lock/mutex+rwsem discipline of `u!{}`,
rcu.rs ordering protocol (0 SeqCst, intentional Acquire/Release).
