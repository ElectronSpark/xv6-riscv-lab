# Full-Rust-Native Redesign Plan (2026-07-18)

Synthesis of primary-source research (Redox, Rust-for-Linux, Tock, Theseus,
Hubris) + the current-kernel gap census, into a target architecture and a
phased, behaviour-preserving execution plan for eliminating the remaining
C-transliteration patterns from this kernel.

## 1. Honest scope: what "full Rust native" means

Every shipping Rust kernel keeps an irreducible `unsafe` floor. Theseus is
~38k LOC with **72** unsafe sites ("most for port I/O / special registers");
Rust-for-Linux and Redox both concentrate unsafe at asm/MMIO/page-table/FFI
seams and make everything above them safe. So "native, no compromise" here
means: **no gratuitous C-transliteration left — object graphs, refcounts,
locking, dispatch, and error handling are idiomatic and safe; `unsafe` is a
thin, audited, `// SAFETY:`-commented floor at the hardware/asm boundary**,
not literally zero. A kernel claiming zero unsafe has hidden it in a HAL.

**The liberating finding (Rust-for-Linux report):** our C-shaped surface is
*self-inflicted*, not FFI-forced. R4L's raw pointers and `Opaque<T>` exist to
wrap a live C kernel they cannot change. We deleted bindgen — our raw
pointers and `as_native` shims reinterpret **types we already own in Rust**.
Nothing forces them. So the fix is to *stop passing the pointer*, not wrap it;
we can go further than R4L (real `Arc`/`Mutex`/slotmap, not FFI compromises).

## 2. Current state (gap census, HEAD db6911c)

| Surface | Count | Target |
|---|---|---|
| raw-pointer `pub(crate) fn` signatures | 364 | typed refs / keys / `Arc` |
| whole-body `unsafe`/`u!{}` blocks | 251 | small audited `unsafe {}` cores |
| `as_native`/`from_bindings` reinterpret shims | 138 | typed refs / `MappedPages`-style MMIO |
| `container_of` intrusive-collection sites | 103 | slotmap side-tables / `intrusive-collections` |
| manual atomic refcount sites | ~497 | `Arc`/`Weak` + one `AlwaysRefCounted` pair |
| total `unsafe` blocks | 5,084 | → low hundreds (asm/PTE/MMIO/DMA floor) |
| LOC (Rust) | ~96,900 | — |

Already idiomatic (the base to extend): KArc/IRef/SRef (28/52/46 sites),
`SpinLock<T>` (24), `Box`/`Arc` (17/30 — `alloc` is available), all ops
tables are `dyn Trait`/enum (P3-10), Result<T,Errno> throughout, bindgen gone.

Object graph today: every relationship is raw `*mut` — `thread.{parent,
thread_group,pgroup,session,vm,fs}`, `inode.{parent,sb,dentry}`. The proc
table is already a fixed `NPROC` slot array (`__proc_table`, pid-slot
alloc/free) — a slotmap in all but name.

## 3. Target architecture (decided, per subsystem)

### 3a. Process/thread family → generational slotmap + typed keys  [FLAGSHIP]
`thread`/`proc`/`pgroup`/`session` move into `SlotMap`s; relationships become
`Copy` typed keys (`Tid`/`Pid`/`Pgid`/`Sid`), not `*mut`. Back-edges are keys
(no `Weak`, no cycles, no `Pin`). Liveness = generation check → stale key
returns `None`, not UB. Run/wait/scheduler membership leaves the thread
struct and becomes `SecondaryMap<Tid, _>` / `Vec<Tid>` side-tables (Theseus's
"shrink the god-struct" lesson) — this retires the `container_of` run/wait
queues. *Why not Arc here:* generation answers "is this pid alive?" precisely
where a zombie's `Arc` cannot; avoids per-edge atomics; smallest leap from the
existing table. Source: Theseus §4, slotmap docs, Hubris bounded-table lesson.
**Highest-risk phase** — touches the scheduler and the asm-adjacent thread
pointer; slot values are address-stable while alive (asm `current` ptr still
valid), but this is the phase that most warrants a pilot + checkpoint.

### 3b. VFS family → `Arc<T>` + `Weak<T>` back-edges
`dentry→inode` = `Arc<Inode>`; `inode→superblock`, `dentry→parent` = `Weak`.
Lifetime is genuinely last-reference-wins and shared across the tree with no
natural index. `Arc` *is* the atomic refcount, freed in `Drop` exactly once —
the home for eliminating the ~497 manual refcount sites in the fs layer.
Source: Redox (`Arc<RwLock<T>>` + strong-in-registry/Weak-elsewhere), Theseus.
Composes with 3a (a slotmap may hold `Arc`s). **Corruption-sensitive** — fs
core; full fs battery per wave.

### 3c. Refcount unification → one `KArc<T>` + one `AlwaysRefCounted`/`ARef`
Two blessed buckets: `KArc<T>` (Rust-owned allocation+count) and an
`AlwaysRefCounted`-style `unsafe trait` + `ARef<T>` handle (count embedded in
a `#[repr(C)]` struct, get/put via Clone/Drop) — fold KArc/IRef/SRef into
these and collapse the manual `fetch_add`/`fetch_sub` sites. Residual unsafe:
~2 lines per type (the inc/dec bodies), audited once. Source: R4L
`AlwaysRefCounted`+`ARef`, "Arc in the Linux kernel".

### 3d. Locking → lock-owns-data `SpinLock<T>`/`Mutex<T>` (+ optional lock levels)
Extend the existing `SpinLock<T>` to the whole lock surface; the data lives
*inside* the lock, reachable only via a `Guard` (Deref). `LockedBy<T,U>` for
"field guarded by another object's lock". Optional: adopt `ordered-locks`-style
compile-time lock levels (`L1..Ln`) — this would have made **P3-BUG1's
lock-ordering hazard a compile error**. Source: Redox `Mutex<L,T>`+`ordered-locks`,
R4L `Lock<T,B>`/`Guard`/`LockedBy`. **High friction** (relocating data into
locks) — stage after 3e/pointer work simplifies the call graph.

### 3e. Signatures → typed references  [cascades, but see the Freeze hazard]
The always-valid-exclusive `*mut T`→`&mut T`/`&T` subset (Option<&T> for
nullable). Mechanical, compiler-proven, and each conversion *deletes* a reason
for `unsafe` and auto-shrinks 3c/3d/whole-body-unsafe. User memory stays a
typed non-derefable wrapper (`UserPtr`/`UserSlice`, direction in the type).
Source: all three reports rank this #1 by surface÷risk.

**CRITICAL CONSTRAINT (proven by the N-R1 pilot, 2026-07-18):** `&T` / `&mut T`
must NOT be used for a type that is (a) plain-data `Freeze` (no `UnsafeCell`
interior — e.g. our `#[derive(Copy)]` `Raw*` lock bodies) AND (b) mutated
concurrently by another hart while a reference is held. A `&Freeze` param gets
LLVM `readonly`+`noalias`, so the optimizer legally hoists a field read out of
a spin/wait loop → the loop never re-observes another hart's write → **boot
hang / lost wakeup**. `&mut` doesn't help (still `noalias`). Such a type can
only take `&` AFTER it gains `UnsafeCell` interior mutability (which is `!Copy`
and forces migrating every by-value embedder) — i.e. **3d must precede 3e for
shared-mutable concurrently-accessed types.** 3e applies directly only to
*genuinely exclusive / non-concurrently-shared* leaf types. The lock-primitive
family therefore moves to 3d, not 3e.

### 3f. MMIO / reinterpret shims → `MappedPages`-style owned wrapper + typed regs
One audited kstd module: owned mapped-region type with lifetime-bound,
bounds-checked `as_type::<T>()` accessors (Theseus `MappedPages`), plus
`#[repr(transparent)]` register newtypes / `tock-registers`-style volatile
wrappers. Collapses the 138 `as_native` shims + scattered `read_volatile`
into a handful of audited functions. Source: Theseus, tock-registers, safe-mmio.

### 3g. `container_of` → slotmap side-tables (default) / `intrusive-collections` (hot paths)
Most sites become `SecondaryMap`/`Vec<Key>` once 3a lands. For the few
genuinely hot intrusive paths, the `intrusive-collections` crate (Adapter
macro hides the offset math; lists + RB-trees) or R4L `ListArc`+`Pin`.
Reports agree: avoid hand-rolled `Pin`-intrusive as the default (hardest,
still leans on unsafe) — prefer side-tables at xv6 scale.

## 4. The irreducible `unsafe` floor (stays, but named + audited + thin)
Inline asm (context switch, CSR, trap entry/exit, `sfence.vma`); the trap/
context frame reinterpret; page-table entry writes (behind a safe `Vmspace`
API); MMIO / device registers / DMA descriptors (behind 3f wrappers); early
boot before the allocator; the inc/dec bodies of 3c and the lock `Backend` of
3d; the one syscall-ABI `mux`/`demux` errno boundary. Target: push the ~5,084
blocks toward Theseus-scale double/low-triple digits.

## 5. Phased execution plan (each wave: behaviour-preserving, full-battery gate)

Ordered by surface÷risk (research consensus). Every wave keeps the standard
gate: 0-warning clean build, boot ×3, testsig 21/21, mmaptest 16/16, fs
battery + stressfs where fs-touching, forkforkfork OK (post-BUG1), cache
discipline, commit green.

- **N-R1 (pilot, DONE — commit 5a0edad):** piloted 3e on the lock family and
  **discovered the Freeze/`noalias`-hoist hazard** (see §3e CRITICAL
  CONSTRAINT): `&`-conversion of these shared-mutable `Copy` types hangs at
  boot. Reverted the conversion; delivered the sound subset (34 redundant
  whole-body `u!{}` wrappers removed, byte-identical codegen). Lock family
  reassigned to 3d. Net lesson: 3e requires the target type be exclusive OR
  already `UnsafeCell`-interior.
- **N-R2 (revised):** roll 3e typed-references through **genuinely-exclusive /
  non-concurrently-shared leaf types** only (per-call transient objects,
  owned-by-caller args, single-hart-local structs) — screen each candidate
  against the Freeze hazard (is the type `Freeze` AND mutated by another hart
  during the borrow? if yes, defer to 3d). Start where a subsystem's args are
  provably exclusive; leave shared-mutable types for their lock-owns-data wave.
- **N-R3:** 3f — the `MappedPages`-style MMIO/reinterpret kstd wrapper +
  `#[repr(transparent)]` register types; migrate virtio/uart/plic/e1000.
- **N-R4:** 3c — unify refcounts into `KArc` + `AlwaysRefCounted`/`ARef`;
  fold the manual sites (non-fs first).
- **N-R5 (corruption-sensitive):** 3b — VFS `Arc`/`Weak` graph; retire the fs
  manual refcounts. Full fs corruption battery.
- **N-R6 (HIGH risk, pilot + checkpoint):** 3a — proc/thread slotmap + typed
  keys, one subsystem at a time (pgroup/session before thread/proc), pushing
  run/wait membership to side-tables (3g). forkforkfork A/B + scheduler
  battery mandatory. **Checkpoint with the user before the thread/proc core.**
- **N-R7:** 3d — lock-owns-data crate-wide + optional lock levels; **includes
  the lock-primitive family reassigned from N-R1** (give `Raw*` lock bodies
  `UnsafeCell` interior — necessarily `!Copy` — and migrate the by-value
  embedders `bio.io_completion`/`pcache.flush_completion`/`tty.completion`/…
  to hold the lock-owning type; only then can they take `&` params safely);
  3g cleanup of any residual `container_of`.
- **N-R8:** de-smear remaining whole-body `unsafe` to audited blocks; final
  census vs this baseline (target: unsafe floor at asm/PTE/MMIO/DMA only).

## 5b. N-R6 design detail (proc/thread slotmap) — decided 2026-07-18

Constraints found in the current kernel:
- **Objects must NOT move.** `current` = `cpu_local.proc` (`CPU_LOCAL_PROC`
  offset 0, read via `tp`) is a raw `*mut Thread` that asm/kernel dereferences;
  Thread also owns a kstack and is context-switched. A `Vec`-backed `slotmap`
  moves values on growth → forbidden for these objects.
- **No external crates** (kernel has zero deps since bindgen was deleted; keep
  it that way) → **hand-roll** the primitive, don't pull `slotmap`.
- Relationship access is mediated by the `access.rs` accessor layer
  (`raw_get!`/`raw_set!`, `ThreadAccess`/`SessionAccess`/…), NOT scattered
  field reads — so conversion is concentrated in the accessors.
- The proc table is today an **intrusive hlist** (Thread embeds
  `proctab_entry` @448, pid→bucket→thread) — both a `container_of` site and
  the central index; convert it LAST, not in the pilot.

**The primitive: `kstd::GenTable<T, K>` (hand-rolled, #![no_std], fixed cap).**
A fixed-capacity `[Slot; N]` where `Slot = { generation: u32, ptr:
Option<NonNull<T>> }`. Objects keep their existing stable slab/page allocation;
the table stores a **stable pointer**, never the object by value (so nothing
moves — asm-safe). A typed key `K = { index: u32, generation: u32 }` (newtypes
`Tid`/`Pid`/`Sid`/`Pgid`). `insert(ptr) -> K` takes a free slot, bumps its
generation; `get(k) -> Option<NonNull<T>>` returns `None` if `slot.generation
!= k.generation` (use-after-free → checked `None`, not UB); `remove(k)` frees
the slot and bumps generation so all outstanding keys go stale. Relationship
fields change `*mut Thread` → `Option<Tid>`; following an edge is a
generation-checked `table.get`. Run/wait/scheduler membership leaves the Thread
struct into `Tid`-keyed side-tables (3g), retiring the intrusive queues.

**Staging (safest → riskiest; each battery-gated; CHECKPOINT before the core):**
- **N-R6a (pilot):** build `GenTable<T,K>` in kstd (+ compile-time asserts, doc,
  the generation-check semantics) and prove it end-to-end on the **session**
  family (leaf, job-control, bounded consumers, well-understood from P3-BUG3):
  session storage → a `GenTable<Session, Sid>`; `*mut session` handles that
  cross subsystem edges → `Sid` keys with generation-checked lookup; keep
  thread/proc/pgroup UNTOUCHED. Proves the primitive + the refcount-liveness
  replacement (a stale `Sid` returns `None`, the generational analogue of the
  P3-BUG3 baseline-drop) on a contained subsystem.
- **N-R6b/c (FOLDED INTO N-R6d — finding 2026-07-18):** pgroup and
  thread_group are NOT cleanly isolable before the thread core. `get_pgroup
  (pgid)` = `get_pid_thread(pgid)` then follow `thread.pgroup` — pgroup has no
  independent registry (unlike session's `session_list`); both pgroup and
  thread_group track membership via **intrusive thread lists** (Tid-coupled).
  So their lookup (proc table) and membership (Tid keys) both require the
  thread core. They convert *with* N-R6d, not before. The session pilot
  (N-R6a) was the one cleanly-isolable subsystem — the primitive is proven;
  the remaining N-R6 work is one large coupled core wave.
- **N-R6d (CHECKPOINT with user, HIGH risk):** thread/proc core — Thread
  relationship fields → `Tid`, the intrusive proc-table hlist → a `GenTable`
  index, run/wait queues → `Tid` side-tables. `current`/`cpu_local.proc` stays
  a raw `*mut Thread` (asm boundary). forkforkfork A/B + full scheduler battery.

## 5c. N-R6d core execution: internal sub-staging (user chose full core 2026-07-18)

Executed as small, individually boot-gated sub-steps (revert-on-regression),
NOT big-bang. `pid` stays the userspace identity (getpid/kill/wait unchanged);
threads gain an internal generational `Tid` + `GenTable<Thread>` registration;
the intrusive proc-table hlist stays for pid-lookup until the LAST sub-step.
- **6d-1 (foundation):** add `Tid = GenKey<TID_TAG>` + `static THREAD_TABLE:
  GenTable<Thread, NPROC, TID_TAG>`; register at `thread_create`, remove at
  `thread_destroy` (bumps generation). Convert the FIRST relationship edge
  `thread.parent: *mut Thread` → `Option<Tid>` (resolved via the table) to
  exercise it. Everything else stays `*mut`. Gate: reparent/wait + forkfork.
- **6d-2:** convert `thread.pgroup`/`thread.session`/`thread.thread_group`
  edges → keys (Pgid/Sid/Tgid via their tables); wire `session.fg_pgrp` and
  `pgroup.session` (deferred from the pilot) to keys.
- **6d-3:** pgroup + thread_group **membership** (intrusive thread lists) →
  `Tid`-keyed side-tables; retire those `container_of` sites.
- **6d-4:** scheduler **run/wait** queues → `Tid` side-tables / key-carrying
  nodes (the tq_t stays, but carries `Tid` not raw thread ptr). forkforkfork
  A/B + full scheduler battery mandatory.
- **6d-5:** migrate `get_pid_thread`/pid-lookup off the intrusive hlist (pid
  stored in Thread + a pid→Tid resolution or table scan) and **retire the
  proc-table hlist** (`Thread.proctab_entry`). `current`/`cpu_local.proc`
  stays raw `*mut Thread` (asm boundary — the irreducible floor).

## 6. Risk tiers & discipline
- **Low (mechanical, compiler-proven):** N-R1/N-R2/N-R8 signature & unsafe-scope work.
- **Medium:** N-R3/N-R4 (new abstractions, wide but local edits).
- **High (architectural):** N-R5 (fs corruption class) and especially **N-R6**
  (scheduler/asm-adjacent) — pilot on one subsystem, A/B every delicate gate,
  checkpoint before the proc/thread core.
- Same orchestrator discipline as the de-C-ification + bug-fix arcs: delegate
  implementation to workers, orchestrator re-verifies every wave against the
  full battery, cache-first audits, honest reclassification over fabricated
  wins, never chase a metric past correctness (`refify-ceiling` memory).
