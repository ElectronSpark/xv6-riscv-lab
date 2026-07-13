# rust-skills compliance audit — new modules (Waves 1-3 + mm post-refactor) — 2026-07-11

Verdict: NO unsound bugs. Two tiers: mm WP1-WP5 files (vm.rs, vm_pgtab.rs,
slab.rs, cffi.rs, mm_safe.rs, sysmm.rs) near-fully compliant, several
exemplary (start.rs, mm_safe.rs, sbi.rs; slab.rs best-in-class atomics).
Wave 1 files (bintree/rbtree/hlist/kobject) + string.rs use a "doc-only"
SAFETY convention (fn-level # Safety, whole-body unsafe, no per-block
comments) — defensible but short of the rule. Concrete gaps below.

## HIGH

- **N-01** page.rs:1300-1656 — 27 `pub unsafe extern "C" fn` (entire public
  C-ABI surface: page_buddy_init, __page_alloc/free, page_lock_*,
  page_ref_*, __pa_to_page, ...) have ZERO `# Safety` docs, and line 23's
  `#![allow(clippy::missing_safety_doc)]` suppresses the lint. Fix: add
  docs, remove/scope the allow.
- **N-02** slab.rs:1194-1485 — same for 7 public fns (slab_cache_init/
  create/destroy/shrink, slab_alloc/free/free_noshrink).
- **N-03** mm_safe.rs:321 `SlabCacheRef::alloc<T>` — `# Safety` doc omits
  "T must be valid when all-zero-bits" (zero-init generic; UB for bool/
  enum/NonZero/reference T). Currently unused (only alloc_uninit is
  called). Fix doc or remove.

## MEDIUM

- **N-04** kobject.rs (9 sites): refcount ops all SeqCst, unjustified →
  Relaxed inc / Release dec + Acquire on free branch, or documented rationale.
- **N-05** vm.rs:535,551: cpumask fetch_or/fetch_and SeqCst vs Acquire-only
  loads on the same field — align or document. (Also: undocumented SeqCst
  fences at vm.rs:393,564,2656; page.rs:826,836,925 — one-line rationale each.)
- **N-06** #[must_use] missing on 15/17 guard types: sync.rs all 9
  (K*Guard family — same as old-audit H-04), pcache.rs 5 (PcGlobalGuard,
  PcLocalGuard, PcTreeReadGuard, PcTreeWriteGuard, PcPageGuard), vm.rs 3
  (VmReadGuard, VmWriteGuard, VmPgtableGuard). mm_safe.rs's PageHandle/
  SlabBox already compliant.
- **N-07** pcache.rs module doc overstates SAFETY coverage (algorithm body
  1700+ has 41 unsafe blocks, 3 commented) — fix doc or add comments.
- **N-08** doc-only SAFETY pattern in Wave-1 files: selectively add inline
  comments to multi-deref fns (rb_delete_key, hlist_put/pop[_rcu]);
  one-liners may stay doc-only by convention.
- **N-09** 7 untagged `unsafe impl Send/Sync` (kobject.rs:58, pcache.rs:214,
  slab.rs:500,503, vm.rs:615,2991, mm_safe.rs:160) — reformat rationale as
  `// SAFETY:` above each impl.

## LOW

- N-10 page.rs try_into().unwrap() ×5 → expect("BUG: ...").
- N-11 page.rs:1296 page_ref free lifetime 'a from raw pointer — bind properly.
- N-12 MaybeUninit::zeroed() where uninit() expresses intent (kobject.rs:71,
  pcache.rs:231-234, vm.rs:618).
- N-13 string.rs:78 vestigial #![allow(clippy::missing_safety_doc)] — remove.
- N-14 expect() messages lack BUG: prefix (hlist.rs, bintree.rs).

## Consolidated corrective plan (merged with old-modules audit)

- **Package A** (old audit): access.rs soundness remediation (H-01..H-03).
- **Package B** (old audit, DISPATCHED): rq.rs race + u!{} discipline.
- **Package C+D merged**: all mechanical compliance fixes across both
  audits — sync.rs/pcache.rs/vm.rs #[must_use] (15 types), page.rs+slab.rs
  Safety docs (N-01/N-02), N-03 doc fix, SeqCst justify-or-downgrade
  (kobject, vm/page fences), Send/Sync SAFETY tags (both audits' lists),
  machine.rs atomic-cast comments (old M-05), unwrap→expect polish (old
  M-03, N-10, N-14), sched_fifo/list.rs one-liners (old L-01/L-02), N-11,
  N-12, N-13. Single worker, zero behavior change except documented
  ordering downgrades in kobject refcounts (optional — may keep SeqCst
  with rationale).
