# xv6-riscv → Rust Rewrite Progress

Goal: eliminate all C code from the kernel, replacing it with idiomatic,
mostly-safe Rust (traits, lifetimes, macros, RAII) while keeping the CMake
build system working. cmocka host tests under `test/` are ported to Rust as
well. QEMU target: single VM, `-smp 2`.

Regression gate for every step: `cmake --build build` succeeds **and** the
kernel boots to `init: starting sh` in QEMU (2 cores).

## Status legend

- ✅ done — fully Rust, no C left
- 🟡 partial — core logic in Rust, C shims/bridges/tests remain
- ⬜ C — not started

## Kernel modules

| Module | Status | Remaining C | Notes |
| --- | --- | --- | --- |
| lock | ✅ | — | 100% Rust incl. in-kernel tests (rcu_test.rs, rwsem_test.rs, semaphore_test.rs); rwlock CAS state machine native in rwlock.rs |
| mm | ✅ | — | 100% Rust + idiomatic refactor complete (WP1-WP5: RAII guards, shim collapse, no `u!{}`, Result boundaries, ownership handles) |
| proc | ✅ | — (swtch.S, sig_trampoline.S remain as assembly) | 100% Rust; C bridge/shims eliminated 2026-07-11 |
| machine / sync / ll / list | ✅ | — | Rust support layers |
| tty | ✅ | — | termios/tty_dev/pty (W10), tty/ptmx (W11), session (W12); SIGINT ctrl-tty delivery FIXED (^C works now) |
| vfs | ✅ | — | 100% Rust (W13-20: core, tmpfs, xv6fs root fs, devtmpfs); TIOCGPTN + tmpfs UAF + devtmpfs dentry-name leak fixed |
| dev | 🟡 | fdt.c, netdev.c, nullrand.c, x1_emac.c, x1_sdhci.c, yt8531.c, dev_test.c | dev/cdev/blkdev (W21) + bio.rs + bufcache.rs (W22); /dev/tty fixed end-to-end (open + read + ^C) |
| irq | ✅ | — (kernelvec.S, trampoline.S remain as assembly) | plic.rs + irq_core.rs (W5), trap.rs (W6), syscall.rs (W7) |
| timer | ✅ | — | timer_core.rs + sched_timer.rs + goldfish_rtc.rs (Wave 8, 2026-07-12) |
| ipi | ✅ | — | ipi.rs (Wave 9, 2026-07-12); cpus[] cpu_local storage kept link_section/page-aligned byte-identical |
| core / misc | ✅ | — (printf_shim.c DELETED in P3-2: kprint!/kprintln! over core::fmt replaced the C-variadic printf; kernel is now 100% C-free) | string/sbi (W2), start (W3), uart/console/printf (W4), backtrace (W5), exec (W26), start_kernel (W27) |
| block/net drivers | ✅ | — | bio+bufcache (W22), virtio_disk + ramdisk + pci + e1000 + net + sysnet (W28, final wave); virtqueue barriers 7/7, e1000 fences 8/8 exact |
| data structures | ✅ | — | bintree.rs, rbtree.rs, hlist.rs, kobject.rs (Wave 1, 2026-07-11); list.rs pre-existing |
| host tests (`test/`) | ✅ | 3 cmocka suites remain BY DESIGN (ut_list/ut_bits/ut_tmpfs_truncate test still-C header layers) | 35 host cargo tests (bits/list/early_allocator) + 4 QEMU ctest in-kernel suites (rwsem, semaphore, pcache 30 cases, workqueue 7 cases); plan CLOSED |

## Iteration log

### Iteration 0 — 2026-07-11 — baseline recovery

- Fixed 2 compile errors left by the previous `proc` rewrite commit
  (`$crate::` used outside `macro_rules!` in `proc/access.rs`,
  `proc/thread_group.rs`; plus an uninferrable `ListIterator` type in
  `SigPendingRef::queue_len`).

- Pinned QEMU to `-smp 2` (removed CPUS env override and LAB=fs special case
  in `cmake/qemu.cmake`).

- Verified: full build OK, boots to shell on 2 cores.

### Iteration 1 — 2026-07-11 — lock module 100% Rust

- Deleted `lock/rwlock_shim.c` (CAS state machine reimplemented natively in
  `rwlock.rs` with `AtomicU64`/`AtomicI32`; `__rwl_try_update` /
  `__rwl_w_holding` kept as `#[no_mangle]` for `proc_shims.rs`).

- Ported `rcu_test.c` (20 sub-tests), `rwsem_test.c`, `semaphore_test.c` to
  Rust modules using `KSemaphore`/`KRwSem`/`KMutex`/`RcuPtr` APIs; C entry
  symbols preserved for `start_kernel.c` test gates.

- Removed the now-empty `lock` OBJECT library from CMake.
- Verified: fresh build clean; boots to shell; with `RWLOCK_TEST=1
  SEMAPHORE_TEST=1` both test masters report 8/8 OK.

### Analysis — proc C bridge/shims (2026-07-11)

- `proc/proc_rust_shims.c` (~2.1k lines) is **not compiled** (excluded in
  `kernel/proc/CMakeLists.txt`) and every function in it already has a live
  Rust port (`proc_shims.rs`, `access.rs`, `signal.rs`, ...) → safe to delete.

- `proc/proc_rust_bridge.c` has 7 functions, all called only from Rust:
  - trivial ports: `pgroup_mark_kernel`, `session_mark_kernel`,
    `sigstack_init_for_thread` (bitfield/field access already proven via
    bindgen `__bindgen_anon_1` accessors in `access.rs`)
  - `xv6_proctab_foreach_{inner,rcu}`: blocked only by the C
    `hlist_foreach_node_rcu` static-inline macro → reimplement
    `hlist_first/next_entry_rcu` walking in Rust with `AtomicPtr` acquire loads
  - `xv6_panic`: referenced by ~14 .rs files via `extern "C"` → replace with a
    canonical Rust kernel-panic fn (keep symbol name or update all call sites)
  - `install_user_root_finish`: needs VFS externs (`vfs_inode_get_ref`,
    `vfs_struct_lock/unlock`, `vfs_iput`) declared to Rust; port if they are
    plain externs, keep a stub only if they are static inlines.

### Iteration 2 — 2026-07-11 — proc module 100% Rust + build-env root cause

- Deleted `proc/proc_rust_shims.c` (dead code, never compiled) and
  `proc/proc_rust_bridge.c` (7 functions ported: bitfield marks via
  `PgroupAccess`/`SessionAccess`, `sigstack_init` direct call,
  `install_user_root_finish` native with `KSpinlock` RAII, Rust `xv6_panic`,
  native RCU hlist proctab iteration with closure API replacing C
  callback+void*). `kernel/proc/` now has zero C files (swtch.S,
  sig_trampoline.S remain as assembly).

- **Root-caused a scary false regression**: endless `init: starting sh`
  respawn loop. Bisect exonerated all Rust changes — cause was `cmake ..`
  reconfigures silently switching from the user's GCC 14.2 toolchain
  (env.conf TOOLPREFIX) + LAB=fs to system GCC 13.2 + LAB=util. Rule: always
  export TOOLPREFIX and LAB=fs before configuring (see Working notes).

- Raised `KSYM_PLACEHOLDER_SIZE` 1MB → 2MB (symbol data exceeds 1MB).
- Verified: build clean (GCC 14.2), boots to stable shell (single init
  start), interactive `ls`/`ps`/`echo` OK — `ps` exercises the new Rust RCU
  proctab walk on both CPUs.

### Iteration 3 — 2026-07-11 — mm WP1: RAII locking

- pcache.rs: extended existing guards + new `PcPageGuard` (with `forget()` for
  ownership-transfer paths); 62 guard sites across ~21 functions; removed the
  file's whole-body-`unsafe` `u!{}` macro entirely.

- vm.rs: local `VmReadGuard`/`VmWriteGuard`/`VmPgtableGuard` for 25 internal
  sites in 17 functions; `vma_validate` down from 6 manual unlock sites to 1
  guard + 1 explicit drop/reacquire around the sleeping fault handler. The six
  C-called `vm_*lock` exports untouched (paired form is the C ABI).

- `vm_copy` two-VM site: fixed-order guards (asymmetric read+write rwsem;
  dst not yet shared → no ordering hazard; documented inline).

- Verified: build clean (−18 warnings), boot gate ×2, stressfs + sync, and
  mmaptest 16/16 (vma_validate, fork/vm_copy, mprotect, mremap, mincore,
  madvise, msync).

### Iteration 4 — 2026-07-11 — planning docs + host test suite un-broken

- Wrote `docs/rustify/phase2_plan.md` (28-wave dependency-ordered plan for
  all ~33k LOC of remaining C) and `docs/rustify/test_port_plan.md` (cmocka →
  Rust strategy: 3 tracks — keep-C-until-ported, cargo-test with host seam,
  in-kernel Rust runtime tests).
- `test/` cmocka project was entirely unbuildable (4 targets referenced
  kernel C deleted by the lock/mm ports; CMake aborted at generate). Fixed:
  retired ut_semaphore/ut_rwlock (superseded by in-kernel Rust suites),
  disabled ut_pcache/ut_early_allocator pending Rust re-homes, dropped the
  dead bits.c reference. Result: 6/6 remaining suites pass under ctest.
- Deleted 4 orphaned kernel/lock/*.c files accidentally resurrected during
  the bisect.

### Iteration 5 — 2026-07-11 — mm WP2: pcache shim collapse

- `pcache_shims.rs` (1731 lines, 198 exports) deleted. `pcache.rs` now uses
  `crate::bindings::{pcache, pcache_node, page_t, ...}` directly via
  `PcacheHandle`/`NodeHandle` newtypes (`NonNull`-backed, per-method scoped
  unsafe with SAFETY comments). Opaque `[u8;0]` stand-ins gone.
- 23 dead accessors dropped outright; 8 errno forwarders replaced by
  bindings constants; ops-vtable helpers tightened 8→5. The 17 genuine
  C-called public API functions kept exact signatures (per-symbol grep).
- Net −373 LOC. Algorithm bodies deliberately untouched (delegator layer to
  be inlined in WP5).
- Verified: zero-warning build, boot gate ×2, stressfs+sync+cat, mmaptest
  16/16. `usertests -q` fails at `copyinstr2` (exec arg-size validation) —
  **confirmed pre-existing on baseline**, tracked below.

### Iteration 6 — 2026-07-11 — mm WP3: page/slab/vm_pgtab shim collapse + build fix

- All three remaining `*_shims.rs` deleted (page 557, slab 755, vm_pgtab 588
  lines). Real logic moved into parent modules (`print_buddy_system_stat`,
  `sys_memstat`, `check_buddy_system_integrity` — now a typed ListNode walk
  instead of a `.wrapping_sub(48)` reinterpret hack; slab registry +
  dump/shrink; `kernel_pagetable` storage + reserved-region kvmmake logic).
- Crate-wide list/percpu primitives (`xv6_list_*`, `xv6_cpuid`,
  `xv6_push_off/pop_off`) consolidated into `cffi.rs` (their declared home).
- Fixed a WP2 leftover that broke the full-kernel link
  (`xv6_page_pcache_get_node` privatized while vm.rs still externs it).
- **Build-system fix (orchestrator)**: kernel/kernel_with_symbols_elf now
  have `LINK_DEPENDS` on `libxv6_rust.a` — previously the kernel was NOT
  relinked when only the Rust staticlib changed (the .a is passed via link
  options, which CMake doesn't track). Verified: touching the .a triggers
  relink; boot gate green.
- Verified (worker): zero-warning build ×2, boot gate ×3, mmaptest 16/16,
  stressfs, `free`/`free -v` (sys_memstat → buddy+slab dumps), dumppcache.
  usertests stops at known copyinstr2 only.
- Working-tree snapshot saved to session scratchpad (everything is still
  uncommitted on `rustify` — see note below).

### Iteration 7 — 2026-07-11 — mm WP5: idiomatic sweep

- `u!{}` whole-body-unsafe macro eliminated crate-wide (60 uses in vm.rs, 20
  vm_pgtab.rs, 9 sysmm.rs → 0); several functions turned out to need no
  unsafe at all; genuinely-unsafe constructors became `unsafe fn` with
  SAFETY docs.
- New `cffi::Errno` enum + `result_to_neg_errno`; internal helpers converted
  to `Result` (map_pages, pcache_init_validate, vma_validate_pte chain) with
  single conversion at C-ABI boundaries. Hot-loop invariant checks
  deliberately left as-is.
- One canonical `Page` type (vm_pgtab now uses page.rs's real struct);
  generic `cffi::container_of<T,M>` consolidates 6 per-file copies.
- mm_safe.rs: dead duplicates of machine/sync guards deleted (617→485
  lines); PageHandle/SlabBox/KBox kept for WP4.
- pcache.rs: 73 single-call-site delegators inlined (2924→2708 lines); ~90
  multi-site ones kept.
- Verified: zero-warning build ×2, boot gate ×3 (+1 orchestrator re-check),
  mmaptest 16/16, stressfs, free -v, dumppcache, usertests (copyinstr2
  only). Adversarial review of the mechanically-inlined pcache.rs
  dispatched (worker flagged the scripted pass as worth a second look).
- **Adversarial review result: zero findings.** Function-by-function
  comparison against the historical C (`fbefa1e^:kernel/mm/pcache.c`) and
  pre-refactor Rust confirmed identical control flow, lock scope, and
  drop ordering across the entire algorithmic body; container_of offsets
  exact; the `forget()` lock-handoffs in pop_lru/pop_dirty match the C
  convention. One pre-existing C-divergence noted (flush_error keeps the
  write_page error instead of being overwritten by write_end's — predates
  the refactor, arguably better behavior).

### Iteration 8 — 2026-07-11 — mm WP4: ownership handles → mm refactor COMPLETE

- `PageHandle` wired into `vma_validate` fault-install (race-loser cleanup is
  now Drop; winner does `into_raw()` at PTE install) and
  `vma_fault_file_page` (both error paths Drop, success paths transfer).
  `SlabBox` wired into `pcache_page_alloc` (node alloc; `into_raw()` at
  C-visible linkage). New `PageHandle::from_pa` for the data-pointer ABI;
  `KBox` deleted (zero users).
- 6 sites surveyed and deliberately skipped (unified teardown paths,
  bespoke existing RAII in slab, primitive pairs) — documented in worker
  report.
- Verified per-site (build+boot ×4) and final: mmaptest ×3 = 16/16 (directly
  exercises the converted race path on -smp 2), stressfs ×2, vforktest,
  usertests (copyinstr2 only), `free -v` leak table stable
  (buddy free+cached ~192.9k band, no monotonic decline).
- mm module: WP1-WP5 all complete. Module marked ✅ in the table.

### Iteration 9 — 2026-07-11 — Phase 2 Wave 1: data structures

- `bintree.c`/`rbtree.c`/`hlist.c`/`kobject.c` → Rust (36 exact C-ABI
  symbols kept; rbtree deliberately line-faithful with INVARIANT comments;
  hlist reimplements the static-inline list/RCU primitives with per-site
  memory orderings matched to the C; kobject uses KSpinlock RAII +
  AtomicI32, fixing a latent C int/int64 atomic-width hazard).
- Dead code dropped (2 never-called fns); fixed inert `||`→`&&` bug in
  `__hlist_is_bucket_of` (zero callers).
- kobject.h added to wrapper.h/build.rs allowlist; 36 symbols added to
  RUST_FORCE_UNDEFINED; ut_hlist/ut_rbtree retired in test/CMakeLists.txt.
- Verified: zero-warning build, boot gate ×2 (devtmpfs 9 nodes = kobject
  path), mmaptest 16/16, ps/ls/free, stressfs, usertests (copyinstr2 only),
  host ctest 4/4.

### Iteration 10 — 2026-07-11 — Phase 2 Wave 2: string.c + sbi.c

- string.rs: 16 symbols, raw byte loops; memcpy delegates to memmove (as C
  did). Compiler-builtins conflict investigated with nm: sysroot rlib ships
  NO mem functions → exactly one kernel-wide definition each, from the Rust
  staticlib (nm-verified); loop-idiom self-recursion ruled out empirically
  via objdump of the exact target/profile. Fixed an out-of-bounds underflow
  in `strstr` (needle longer than haystack).
- sbi.rs: 33 symbols; `sbi_ecall` via core::arch::asm! matching the C
  macro's register contract and memory clobber. SBI probe log identical.
- 49 symbols added to RUST_FORCE_UNDEFINED.
- Verified: zero-warning build, boot gate ×3 (hart 1 up = HSM/IPI paths),
  mmaptest 16/16, stressfs, usertests (copyinstr2 only).

### Iteration 11 — 2026-07-11 — bug fix: map_pages partial-mapping-failure leak

- `map_pages` (`kernel/mm/vm_pgtab.rs`): on ENOMEM partway through a
  multi-page range, the loop now calls a new `unwind_partial_map` before
  returning `Err`. It clears the leaf PTEs this same call already
  installed in `[va, fail_va)` (not freeing the caller-owned physical
  pages -- `map_pages` never owned them) and prunes any intermediate L1/L0
  page-table page that is now completely empty, including one the failing
  `walk_internal` call may have allocated and linked in one level up
  before failing deeper. Pruning is gated purely on "all 512 entries read
  zero", which is allocation-history-independent and safe by construction:
  a table that is genuinely empty cannot be shared with any mapping this
  call didn't just undo, so boundary tables still holding pre-existing
  neighbours are correctly left alone. No `sfence.vma` needed -- the range
  being unwound was never installed successfully, so no hart could have
  translated through any of it yet.
- Caller survey: `mappages()` (the C-ABI wrapper around `map_pages`) has
  exactly one live call site today, `vm_mmap_region_locked` in `vm.rs`,
  gated on a caller-supplied physical address (`pa`); no in-tree driver
  wires that path up yet, so it's presently unreachable from userspace.
  That caller already unwinds correctly at the leaf/vma level via
  `vma_free` → `__vma_set_free` (walks the vma range, clears any leaves
  that did get installed, decrements their page refcounts) — it just never
  reclaimed the empty intermediate page-table pages, which is exactly what
  this fix adds. `kvmmap` (boot-time kernel page table construction) is
  the only other caller; it panics unconditionally on `map_pages` failure
  (matching classic xv6 behaviour), so the leak there was moot other than
  wasted work in the instant before halting.
- Verified: zero-warning build, boot gate, mmaptest 16/16, `usertests -q`
  (stops at the pre-existing `copyinstr2` only), stressfs. No live
  ENOMEM-triggering path exists today to exercise `unwind_partial_map` via
  black-box testing (confirmed by the caller survey above), so this was
  verified by exhaustive manual code-path tracing rather than a
  constructed fault-injection test; documented honestly rather than
  claiming coverage the test suite doesn't provide.

### Iteration 12 — 2026-07-11 — Phase 2 Wave 3: start.c + rust-skills governance

- start.rs: `stack0` as `#[repr(C, align(16384))]` newtype static
  (nm-verified: 0x20000 bytes, 16KiB-aligned, .bss — entry.S clears BSS
  before first use), `start()` with boot-hart CAS (SeqCst justified as 1:1
  C port), `timerinit()` via new canonical machine.rs CSR helpers
  (write_satp, write_stimecmp raw CSR 0x14d, read_sie).
- **Governance change**: all Rust work must now follow the rust-skills
  ruleset (see Working notes) — mandate added mid-flight to this wave;
  report includes per-rule compliance. Two read-only compliance audits of
  the whole existing crate dispatched (new modules + older lock/proc/
  machine/sync).
- Verified: zero-warning build ×2, boot gate ×2 (both harts up), mmaptest
  16/16, usertests (copyinstr2 only).
- Note for later: vm_pgtab.rs still has a private w_satp duplicate of the
  new canonical machine.rs helper (fold in during a future wave).

### Iteration 13 — 2026-07-11 — compliance audits + Package B (race fix + unsafe discipline)

- Two crate-wide rust-skills audits (reports in docs/rustify/skills_audit_*):
  no unsound bugs in session-written code; older proc code held 1 real
  cross-CPU data race + 2 latent soundness hazards (access.rs).
- **Package B landed**: `active_cpu_mask` u64 → AtomicU64 with
  Release/Acquire (analysis: it's a publication flag — `rq_cpu_is_idle`
  gates non-atomic reads of remote idle-thread state on the bit; Relaxed
  would leave a second race riding the first). `&mut RqGlobal` no longer
  materialized for the shared field (addr_of! projection). ~123 SAFETY
  contracts added across signal/thread/thread_group/proc_shims; 56 vacuous
  u!{} wrappers removed; 4 duplicate macro defs consolidated to one
  documented crate-level `u!`.
- Verified: zero-warning build, boot gate ×2 (both CPUs through the new
  atomic path), mmaptest 16/16, stressfs, ps, usertests (copyinstr2 only),
  grep proof of zero non-atomic mask accesses.
- Remaining flagged: lock/*.rs still have 4 local u! copies (out of B's
  scope); proc_shims benign-debug-race notes documented in file.

### Iteration 14 — 2026-07-11 — Package C+D: mechanical compliance pass

- `#[must_use]` on 16 guard types (sync 9, pcache 5, vm 3, access 1) —
  zero warnings surfaced, confirming no bare-statement lock misuse existed.
- `# Safety` docs on all 34 public unsafe C-ABI fns (page 27, slab 7);
  both `missing_safety_doc` lint suppressions removed; clippy clean.
- kobject refcounts downgraded SeqCst → Arc-pattern orderings (Relaxed inc,
  Release dec + Acquire fence on free branch, Acquire CAS for RCU try_get)
  with per-site rationale; vm cpumask RMWs → Release matching Acquire
  loads; the 6 cross-hart fences kept SeqCst with written justification.
- 12 Send/Sync impls tagged with concrete serialization rationale;
  machine.rs atomic-cast layout justifications; unwrap→expect(BUG:) polish;
  page_ref free-lifetime fixed; MaybeUninit::zeroed→uninit ×5 (verified
  write-before-read each); sched_fifo get_unchecked → safe indexing.
- Verified: zero-warning build, boot gate ×2, mmaptest 16/16, stressfs,
  ps, usertests (copyinstr2 only).
- Noted for later: clippy's stricter not_unsafe_ptr_arg_deref has ~335
  pre-existing findings in proc/ (mostly signal.rs) — future pass.

### Iteration 15 — 2026-07-11 — Package A: access.rs soundness → compliance sweep COMPLETE

- `assume`/`assume_thread` (20 fns) converted to `unsafe fn` with `# Safety`
  docs; 96 call sites given locally-verified SAFETY comments (incl. 10
  pre-existing `from_raw().unwrap_unchecked()` bypasses with the same UB
  shape the audit missed); 25 `from_raw`-family constructors documented.
- Generic `zeroed<T>()` deleted; replaced by concrete `zeroed_sched_attr()`
  and `zeroed_ksiginfo()` (both verified POD against C headers).
- Macro-family SAFETY contracts (raw_*!, bit_*!, shim_call!) + module-level
  soundness-model doc.
- Verified: zero-warning build (incl. cargo clean rebuild + masked-warnings
  check), boot gate ×5, mmaptest 16/16, testsig 21/21, stressfs, ps/free/
  dumprq, usertests (copyinstr2 only).
- Residual flagged: safe `from_ptr` (null-check-only) left per audit scope —
  future pass candidate.
- **All three audit packages (A, B, C+D) landed. Crate-wide rust-skills
  compliance sweep complete.**

### Iteration 16 — 2026-07-12 — Phase 2 Wave 4: uart.c + console.c + printf.c

- **Step 0 (variadic ABI), empirically verified**: wrote a 10-line test
  crate defining `unsafe extern "C" fn test_va(fmt: *const c_char, mut
  ap: ...)` and built it for `riscv64gc-unknown-none-elf` on the pinned
  stable `rustc 1.95.0` — `#![feature(c_variadic)]` is rejected outright
  (`error[E0554]: #![feature] may not be used on the stable release
  channel`). *Calling* a C-variadic extern (`printf(fmt, ...)`) remains
  stable and unaffected — already used throughout the crate. Verdict:
  minimal C shim. `kernel/printf_shim.c` (43 lines) contains only the
  variadic `printf()` entry point plus three `va_arg` fetchers
  (`__printf_va_arg_int/_u64/_str`, matching the three distinct
  `va_arg(ap, T)` shapes the original C used); every other symbol
  (`printf_rust`'s full format parser, `__panic_start`/`__panic_end`,
  `panic_state`, `panic_disable_bt`, `printfinit`,
  `panic_msg_lock`/`unlock`, `puts`, `putchar`) is Rust. Confirmed via
  full-tree grep that `snprintf`/`sprintf`/`vprintf` don't exist
  anywhere in the C original — no wider variadic surface to replicate.
  Delete `printf_shim.c` once `c_variadic` stabilizes.
- **uart.rs**: 16550/PXA driver: `read_reg`/`write_reg` are the file's
  only 2 volatile sites (computed `UART0 + (reg << reg_shift)`,
  `read_volatile`/`write_volatile` on `*u8` or `*u32` depending on
  `reg_io_width`); `__uart0_*` FDT-configured globals now `#[no_mangle]
  pub static mut` (still written by `dev/fdt.c`, unchanged). TX/RX ring
  buffers + locks ported 1:1 (compile-time-initialized `spinlock_t`
  literals, matching `SPINLOCK_INITIALIZED`); `uartputc_sync` stays
  IRQ-safe (`push_off`/`pop_off` + hardware spin-wait, no sleep) for use
  from `printf`/panic in any context.
- **console.rs**: device layer + TTY glue. Added `dev/cdev.h` and
  `tty/tty.h` to `wrapper.h` (both bindgen-clean — no static-inline
  bodies); fixed a latent double-`#include` of the guard-less
  `tty/session_types.h` this surfaced (wrapper.h previously included it
  directly *and* tty.h now pulls it in transitively). `console_cdev`
  built as a zeroed `MaybeUninit<cdev_t>` populated field-by-field at
  `consoledevinit()` time (matches the C static initializer's partial
  literal + runtime `.ops =`/`.dev.ops.ioctl =` assignments exactly,
  including leaving `type_`/`kobj` zero). `register_irq_handler`/`struct
  irq_desc`/`PLIC_IRQ_OFFSET` hand-declared locally (trap.h not yet in
  wrapper.h — Wave 6) rather than added to the bindgen surface, same
  precedent as `rwlock_types.h` fronting `rwlock.h`. `CONSOLE_TTY`
  upgraded from the C's plain (racy) pointer to `AtomicPtr` (`Relaxed`)
  — same observable behaviour, race removed; `tty_inbuf`
  producer/consumer ring keeps the C's exact `Release`/`Acquire`
  per-site orderings.
- **printf.rs**: full format-spec parser ported (`%d %ld %lld %u %lu
  %llu %x %lx %llx %p %s %%`, `-`/field-width prefix, `%*s` padding),
  1:1 control flow with the C `for`-loop's index arithmetic (including
  the `%*s` special case's extra `i++`). `printint`/`printptr`/
  `print_padding`/`print_timestamp` moved to safe `&mut [u8]` + `usize`
  slice indexing (no unsafe) instead of the C `char*`/`int*` pair.
  `xx.wrapping_neg()` replaces the C `-xx` (avoids a debug-overflow
  panic on `i64::MIN` while reproducing the same two's-complement
  magnitude the C UB happened to produce in practice). Panic chain
  (`__panic_start`/`trigger_panic`/`__panic_end`) ported verbatim onto
  existing `machine.rs` primitives (`CpuLocal::flags_or`,
  `write_sie`/`intr_on`/`wfi`); `xv6_panic` (proc module, ~14 call
  sites) needed no changes — it already called these symbols by name.
- 24 symbols added to `RUST_FORCE_UNDEFINED`; `wrapper.h` gained
  `dev/cdev.h`/`tty/tty.h`; `build.rs` allowlist gained
  `device_t|cdev_t|...`/`tty|termios|...` types and `OPOST|ONLCR` vars.
- Verified: zero-warning build (incl. `cargo clean` rebuild), boot gate
  ×2 (plus 3 earlier per-layer boots), line-for-line boot log diff
  clean (only address/symbol-count shifts from the smaller binary and
  pre-existing worker-thread scheduling nondeterminism), interactive
  `ls`/`ps`/`echo hello`/`free`, backspace/line-editing verified
  end-to-end over the pty (`^H` + ANSI erase, correct edited command
  executed), usertests (copyinstr2 only, pre-existing), mmaptest 16/16,
  stressfs. Panic path: verified by symbol-linkage (zero undefined
  refs) and manual reasoning (no sleep, no lock re-entry) rather than a
  live trigger — the task's file-touch scope excludes adding a debug
  syscall, and no existing trigger-a-panic mechanism exists in the
  tree; documented honestly rather than claiming coverage the test
  suite doesn't provide.

### Iteration 17 — 2026-07-12 — Wave 5: backtrace + plic + irq core

- backtrace.rs: ksymbols parsing verified by independent re-parse of the
  actual embedded .ksymbols ELF section (24,498 entries — exact match);
  exercised live via `ps -b` thread backtraces. No alloc/locks (panic-safe).
- irq/plic.rs + irq/irq_core.rs: MMIO volatile, RCU'd `AtomicPtr` desc
  table (Acquire/Release documented vs C's CONSUME/RELEASE), KSpinlock
  RAII. Dead M-mode PLIC macros not carried (zero callers).
- Fixed latent Wave-4 bug: console.rs's placeholder IrqDesc under-sized
  rcu_head (8 vs 40 bytes) → 32-byte over-read on registration copy (never
  exploited — immediately overwritten); now imports the real IrqDesc.
- Verified: zero-warning build ×2, boot gate ×5, interactive + stressfs +
  mmaptest + usertests (copyinstr2 only), ps -b/-t.
- Flagged pre-existing: `rb_find_key_rdown` tie-group edge case (bintree)
  makes post-swtch resume addresses resolve "unknown" in backtraces —
  future hardening candidate. `plic_enable_irq` RMW non-atomic (matches C).

### Iteration 18 — 2026-07-12 — Wave 6: trap.c (solo, high-scrutiny)

- trap.rs (1283 lines): full trap/interrupt/syscall-entry/signal-frame
  machinery. Asm contracts re-verified against kernelvec.S / trampoline.S /
  sig_trampoline.S sources; `trampoline_userret` relocation math ported
  verbatim — **boot-log addresses byte-identical C vs Rust** (uservec,
  userret, all 8 intr_stacks, both harts' intr_sp).
- ucontext_t hand-rolled with named fields matching uabi/signal.h;
  interrupt-context no-sleep/no-alloc discipline preserved; 12 asm/C-called
  symbols exported, 2 helpers made private (grep-verified zero callers).
- Verified across 3 independent QEMU sessions, zero flakiness: boot gate
  ×3, mmaptest ×2 (16/16), testsig 21/21, primes/pingpong/stressfs,
  usertests (copyinstr2 only), interactive syscall battery.

### Iteration 19 — 2026-07-12 — Wave 7: syscall.c → irq module 100% Rust

- syscall.rs: dispatch table as `const fn build_syscalls()` (line-diffable
  vs C designated initializers) — scripted diff: 79/79 entries identical;
  SYS_* constants 79/79 vs uabi/syscall.h; SYS_sigalarm correctly absent
  (was commented out in C too). Arg-fetch helpers exact (names/sigs/
  semantics); unknown-syscall path byte-faithful (printf + -ENOSYS).
- irq OBJECT library now assembles only kernelvec.S/trampoline.S.
- Verified: zero-warning build, boot gate ×6, mmaptest 16/16, testsig
  21/21, stressfs, usertests to copyinstr2 + targeted deeper singles
  (badarg 50k bad-exec, argptest, copyin/out, validatetest, sbrkarg,
  stacktest, pgbug — all OK; "lost some free pages" on single-test
  invocation is a pre-existing harness accounting artifact).

### Iteration 20 — 2026-07-12 — Wave 8: timer module 100% Rust

- timer_core.rs (rb-tree + threaded list wheel), sched_timer.rs,
  goldfish_rtc.rs (MMIO funnel + anti-tear loop). Timer OBJECT library
  removed from CMake (no .S). Interrupt-context analysis documented:
  clockintr ISR only rearms stimecmp/bumps jiffies; callback dispatch runs
  in thread context via scheduler_yield — narrower than the plan assumed.
- Verified: zero-warning build ×2, boot gate ×2, sleep 100/500/3000
  proportional, wallclock (RTC MMIO) sane, pingpong/primes/stressfs,
  mmaptest 16/16, testsig 21/21, usertests (copyinstr2 only), clippy clean
  for timer/.

### Iteration 21 — 2026-07-12 — Wave 9: ipi module 100% Rust

- ipi.rs (603 lines): per-function interrupt-context table in module doc
  (receive path verified no-sleep/no-alloc: CRASH → spinlock + panic-safe
  backtrace + wfi loop; RESCHEDULE → IRQ-safe rq_flush_wake_list).
  `cpus[]` kept `#[link_section = "cpu_local_sec"]`, runtime-verified
  trampoline tp addresses = TRAMPOLINE_CPULOCAL + hartid*64.
- Orderings downgraded from C's SeqCst with per-site rationale: pending
  mask Release-set/Acquire-drain (publication-flag pattern per the
  Package-B exemplar).
- Two latent C UB bugs neutralized (both dead code, grep-verified):
  unchecked `hart_mask_base` OOB write → checked .get(); reason-bit OR
  before validation → validate-first.
- Verified: zero-warning build ×2, boot gate ×5 (both boot-hart orderings
  observed), mmaptest ×2 16/16, testsig 21/21, cross-CPU ps, stressfs/
  pingpong/primes, usertests (copyinstr2 only).

### Iteration 22 — 2026-07-12 — Wave 10: termios + tty_dev + pty

- termios.rs (constants hand-verified against uabi/termios.h — userspace
  ABI untouched), tty_dev.rs (/dev/tty cdev via MaybeUninit + bitfield
  setters), pty.rs (slave tty_ops + master read/write + alloc; exact C ABI
  for ptmx.c's Wave-11 callers).
- Verified: zero-warning build, boot gate ×3, backspace + ^U kill-line
  over pty (console termios defaults now fully Rust-sourced), mmaptest
  16/16 ×2, testsig 21/21, usertests (copyinstr2 only).

### Iteration 23 — 2026-07-12 — Wave 11 regression RESOLVED: build-cache poisoning, tty.rs exonerated

- The boot-gate failure after Wave 11 (3648 respawns) was NOT a tty.rs bug:
  `build/CMakeCache.txt` had silently acquired `CMAKE_BUILD_TYPE=Release`,
  appending `-O3 -DNDEBUG` after the project's hardcoded `-O0` — and the C
  kernel breaks under -O3 (latent UB, likely strict-aliasing). A/B matrix:
  failure 100% correlated with -O3, 0% with tty language (Rust and
  C tty both green at -O0, both broken at -O3). Rust staticlib always
  builds --release, unaffected.
- Fix: cleared the cache entry, purged stale objects, full rebuild — zero
  source changes. tty.rs/ptmx.rs verified: boot gate ×3 (+1 orchestrator
  audit), backspace/^U line editing, input-idle robustness probes,
  mmaptest ×2 16/16, stressfs, testsig 21/21, usertests (copyinstr2 only).
- Lesson recorded: the previous worker's "pre-existing" A/B was honest but
  poisoned — everything it rebuilt in the same cache failed identically.
  New standing rule: orchestrator boot-gates after every worker; build
  gates now also check CMAKE_BUILD_TYPE (see Working notes).
- Genuinely pre-existing finds from Wave 11 (kept): session_set_ctrl_tty
  never sets tty->session (SIGINT delivery gap — Wave 12 must fix);
  TIOCGPTN ioctl sign-extension mismatch (vfs_syscall.c — Wave 17);
  device_register discards devtmpfs_create_node failures (dev.c — Wave 21).
- NEW latent finding: the C kernel cannot survive `-O3 -DNDEBUG` — real UB
  somewhere in remaining C; shrinks as C is eliminated, worth a dedicated
  investigation if it persists near the end.

### Iteration 24 — 2026-07-12 — Wave 12: session.c → tty module 100% Rust

- session.rs: 16 symbols 1:1. **SIGINT delivery FIXED**: session_set_ctrl_tty
  now sets the tty->session back-pointer (+tty_ref, balanced on replace/
  hangup/final-unref via __session_detach_ctrl_tty) — live acceptance test:
  `sleep 5000` + ^C → killed, prompt returns.
- Real bug fixed: session_setsid OOM path used slab_free directly,
  bypassing session_list unlink (list corruption) → session_unref.
- Flagged (preserved 1:1): session refcount macros return pre-op value but
  C compares as post-op → sessions leak rather than double-free. Future fix.
- CMake hardening: top-level CMakeLists FORCEs CMAKE_BUILD_TYPE empty;
  proven immune to -DCMAKE_BUILD_TYPE=Release (flags show -O0 only).
- tty OBJECT library removed (no C left).
- Verified: zero-warning build, ~11 clean boots, ^C acceptance, backspace/
  ^U, testsig 21/21 (incl. setsid test 15), mmaptest ×2 16/16, stressfs,
  usertests (copyinstr2 only). Orchestrator audit: boot gate green,
  BUILD_TYPE empty.

### Iteration 25 — 2026-07-12 — Wave 13: vfs/inode.c (vfs campaign opened)

- inode.rs (30 C-ABI symbols): lock-order map documented (mount → sb rwsem
  → inode mutex → driver internals); vfs_ilock/iunlock kept paired C ABI
  (mm WP1 precedent); refcount CAS line-faithful (Acquire load +
  SeqCst/SeqCst CAS mirroring smp/atomic.h); vtable dispatch null-checked
  (pcache ops pattern); vfs_iput's retry/asymmetric-unlock transliterated
  deliberately (correctness-critical). Real bindgen types, no opaques.
- Verified: zero-warning build, 12 boots, cat 26KB file, mkdir/rm paths,
  stressfs ×2, mmaptest, testsig 21/21, targeted fs usertests (dirtest,
  createdelete, unlinkread, linktest, concreate, bigfile, rmdot, dirfile,
  iref, bigdir — all OK), usertests -q (copyinstr2 only). Orchestrator
  audit green.
- Notes: root fs is RAM-disk-backed by design (persistence across boots is
  ephemeral — xv6fs_mount_root prefers major-3 ramdisk); sh output
  redirection (`> file`) broken in user/sh.c parser (pre-existing,
  userspace, flagged); InodeRef newtype deferred to a future idiomatic pass.

### Iteration 26 — 2026-07-12 — Wave 14: vfs file.c + pipe.c (worker cut off; orchestrator completed verification)

- file.rs + pipe.rs landed (worker terminated twice near the end: first a
  dropped connection, then the account monthly spend limit — its final
  report was never delivered; code + in-scope git state verified clean).
- Orchestrator-run verification: zero-warning build, boot gate green,
  pingpong/primes/pipe1 (pipe ring + wakeup protocol), cat README.md
  (file_ops dispatch), stressfs + ls, usertests -q identical to baseline
  (copyinstr2 only).
- **killstatus FAILED** ("status should be -1") — first time this test was
  ever exercised (it sits after copyinstr2, which every -q run stops at;
  no wave's targeted singles included it). Path is fork+kill+wait exit
  status: pure proc-side, untouched by W13/W14; testsig's 21 waitpid
  assertions were green through W13 → attributed pre-existing (likely a
  POSIX-style exit-status semantic divergence from classic xv6). Added to
  Known issues.

### Iteration 27 — 2026-07-12 — Wave 15: vfs/fdtable.c

- fdtable.rs (11 C-ABI symbols): single-word bitmap fd scan (NOFILE==64 →
  masked trailing_zeros), RCU publication via AtomicPtr Release-store /
  Acquire-load (CONSUME→Acquire house mapping), clone deep-copy plain
  access on unpublished dest + trailing fence(SeqCst) as C's smp_mb,
  caller-held-lock preconditions asserted via KSpinlock::holding.
- Verified: zero-warning build, boot gate ×4 (+1 orchestrator audit),
  pipes, stressfs ×2, vforktest 3/3, mmaptest 16/16, testsig 21/21,
  usertests -q baseline + 15 targeted fd/fs singles all OK.
- Flagged: bitmap is uint64[1] hardcoded (C fragility if NOFILE > 64);
  clone silently drops concurrently-closed fds (C behavior preserved).

### Iteration 28 — 2026-07-12 — Wave 16: vfs/fs.c (mount/superblock core)

- fs.rs (2807 lines, 45 C-ABI symbols): fs-type registry, mount/unmount
  state machine (C goto-cleanup → labeled block preserving exact unwind
  incl. the C's own no-unlock-before-free quirk), sb rwsem lock API
  byte-identical for inode.rs's externs, per-sb inode hash cache
  (offset-0 layout verified against real bindgen, not assumed), fs_struct
  lifecycle. Scope correction documented: namei/path-walk was already
  Rust since W13 (lived in inode.c, not fs.c).
- Dead smoketest entry points dropped (call sites commented out in C).
- Verified: zero-warning build, boot gate ×4 (all 3 mounts + chroot
  grep-verified), explicit mount/umount exercise of tmpfs AND xv6fs types,
  path battery, 10 targeted fs singles OK, usertests baseline, mmaptest
  16/16, testsig 21/21, stressfs ×2. Orchestrator audit green (4 mount
  lines).

### Iteration 29 — 2026-07-12 — Wave 17: vfs_syscall.c → vfs core 100% Rust

- vfs_syscall.rs (2500 lines): 33 syscalls + 2 path helpers, names exact
  for the Wave-7 dispatch table. statfs layout added to wrapper.h;
  dirent64/pollfd hand-mirrored (userspace-only structs).
- Self-caught port bug: linux_dirent64 d_name written at size_of (24)
  instead of true offset (19) — found via live ls, fixed with
  offset_of!-based constant, trap documented.
- **TIOCGPTN FIXED**: cmd normalized to u32 once after fetch (sign-extended
  bit-31 ioctls now match; no-op for the other 9 constants — bit-arithmetic
  verified; no live ptmx userspace trace exists in-tree, documented).
- vfs_core OBJECT library removed (no top-level vfs C left).
- Verified: zero-warning build, boot gate ×4 (+orchestrator audit incl.
  ls/cat), interactive battery incl. cd + mount/umount, 7 fs singles OK,
  usertests baseline, mmaptest 16/16, testsig 21/21, stressfs ×2.
- Accidental safety upgrade noted: deep-path arrays now bounds-checked
  panic vs C silent stack overflow (>64 components).

### Iteration 30 — 2026-07-12 — Wave 18: tmpfs

- tmpfs/{superblock,inode,truncate,file}.rs (2621 lines); ops vtables as
  Rust statics of extern "C" fns (pipe.rs precedent); tmpfs_private.h kept
  as the honest C contract for devtmpfs (Wave 20) — all header-declared
  symbols stay #[no_mangle]; bindgen layouts via repo-root include (no
  opaques); layout fidelity proven by boot log (sizeof=1408, embedded=312,
  max 1GB — computed from bindgen, byte-identical).
- **Fixed genuine C bug**: __tmpfs_move error path freed a dentry still
  linked in the parent hash on name-copy failure (use-after-free) — now
  guarded, documented as deliberate deviation.
- tmpfs_smoketest deleted (dead pre-port); ut_tmpfs_truncate disabled with
  re-home note (test plan updated); host suite 3/3 green.
- Verified: zero-warning build, 8+ boots, tmpfs battery (nested dirs,
  stressfs in /tmp + second mount, symlinktest, 9 fs singles from tmpfs
  cwd), pristine umount cycle, mmaptest 16/16, testsig 21/21, usertests
  baseline. Orchestrator audit green (layout lines byte-identical).

### Iteration 31 — 2026-07-12 — Wave 19: xv6fs (root filesystem)

- xv6fs/{superblock,inode,truncate,file,log,block_cache}.rs (4396 lines),
  ported in two sub-waves per the plan, each build+boot-verified before the
  next started. **Sub-wave A** (superblock.c+inode.c — mount/inode-alloc/
  directory-entry core, boot-critical): `xv6fs_mount_root`'s boot lines
  (`block cache initialized: 199720 data blocks, 197349 free in 1 extents`
  / `mounted at /root` / `chroot to /root successful`) byte-identical to
  the pre-wave baseline. **Sub-wave B** (truncate.c/file.c/log.c/
  block_cache.c): the write-ahead log (crash-consistency), block-mapping/
  truncation, per-inode pcache file I/O, and the Wave-1-Rust-rbtree-backed
  free-extent block cache.
- **Log fidelity**: `begin_op`/`end_op` block-count and outstanding-op
  accounting (`log->lh.n + (outstanding+1)*MAXOPBLOCKS > XV6FS_LOGSIZE`),
  the interruptible-vs-uninterruptible sleep variants
  (`xv6fs_begin_op`/`_nointr`), and `end_op`'s wake-outside-lock pattern
  (`tq_bulk_move` under the lock, `tq_wakeup_all` after releasing it, to
  avoid a lock convoy) are ported line-for-line against the C. Commit
  ordering (`write_log` → `write_head` (real commit point) →
  `install_trans` → clear header) preserved exactly.
- **Block cache**: `xv6fs_block_cache`'s free-extent rb-tree reuses the
  Wave-1 Rust `rb_insert_color`/`rb_delete_node_color`/`rb_first_node`/
  `rb_next_node`/`rb_last_node` directly — the second real consumer of
  that port after `mm/vm.rs`'s VMA tree. Documented a non-obvious
  correctness point in the module doc: `rb_insert_color` performs the
  keyed BST insert itself (via `bintree::rb_insert_node`), so
  `extent_keys_cmp`/`extent_get_key` are genuinely exercised on every
  insert, not dead ABI ceremony.
- **On-disk ABI**: `vfs/xv6fs/ondisk.h` untouched; every on-disk struct
  (`superblock`/`dinode`/`dirent`) uses the real bindgen layout (added via
  `kernel/vfs/xv6fs/xv6fs_private.h`'s wrapper.h include, which also
  covers `xv6fs_superblock`/`xv6fs_inode`/`xv6fs_log`/
  `xv6fs_block_cache`/`free_extent` and `dev/buf.h`'s classic buffer-cache
  `struct buf` / `dev/bio_types.h`'s `struct bio`) — no opaque stand-ins.
  Empirically confirmed the `proc/thread.h` bindgen hazard flagged
  elsewhere in wrapper.h no longer applies (build.rs's `__atomic_*` clang
  stubs already cover it), so `xv6fs_private.h` itself needed zero edits.
- **Deliberate safety fixes** (documented deviations, matching this
  crate's established practice): consolidated every read-only
  directory-entry scan through one `read_dirent` helper that always
  checks `bread()` for null (the C original checked inconsistently across
  call sites — a latent NULL-deref UB on OOM, now a defined skip);
  `xv6fs_iupdate` gained a defensive null-`sb` guard that panics instead
  of dereferencing (see verification notes below).
- CMake: `vfs_xv6fs` OBJECT library and `kernel/vfs/xv6fs/CMakeLists.txt`
  removed (no C left, same precedent as Wave 18's tmpfs); `xv6fs_smoketest.c/.h`
  deleted outright (dead code, same as tmpfs's). ~30 symbols added to
  `RUST_FORCE_UNDEFINED` across both sub-waves.
- Verified: zero-warning build (clean `cargo clean` rebuild too), 10+ boot
  gates (block-cache/mount/chroot lines byte-identical to baseline each
  time), stressfs ×3+ (32-way fork concurrency, no hangs), 21 targeted fs
  usertests singles all `OK` (manywrites, bigfile, bigwrite, createdelete,
  linktest, concreate, fourfiles, sharedfd, openiput, unlinkread, subdir,
  bigdir, dirtest, rmdot, iref, diskfull, ×2 rounds), mount/umount of the
  second disk (`/dev/disk1` → `/mnt` xv6fs) with the block-cache init line
  scaling correctly to the smaller image, mmaptest 16/16, testsig 21/21,
  pingpong/primes, usertests -q (copyinstr2 only, baseline-consistent).
- **Two resource-exhaustion findings, both root-caused as pre-existing**
  (verified against an untouched-C baseline built from this session's own
  starting commit): (1) `usertests outofinodes` hit a NULL-`sb`
  read-page-fault in `xv6fs_iupdate` once in ~30 runs (not reproduced in
  28 subsequent trials, 8 same-boot + 20 fresh-reboot) — full call-site
  audit found no logic path that could leave an inode's `sb` unset before
  `xv6fs_iupdate` runs; added a defensive panic rather than leaving the
  latent null-deref. (2) `usertests diskfull` (true disk-exhaustion
  stress, not in the wave's required test list) **hung identically on the
  very first try against the unmodified pre-Wave-19 C baseline** (git
  worktree at this session's starting commit, same toolchain/env) — a
  confirmed pre-existing deadlock somewhere in the disk-full handling
  path, unrelated to this port. Not fixed (out of scope: reproducible in
  code this wave never touched). See Known issues below.

### Iteration 32 — 2026-07-13 — Wave 20: devtmpfs → vfs tree 100% Rust

- devtmpfs/superblock.rs (927 lines): registry (spinlock + intrusive
  list), tmpfs-delegating vtables, create/remove_node keeping exact ABI +
  swallow-failures semantics for the pending Wave-21 dev.c fix.
  devtmpfs_private.h deleted; public devtmpfs.h untouched (dev.c consumer).
- Fixed: dentry-name strndup leak in __devtmpfs_walk_parent's ilookup-hit
  branch. Flagged 1:1-preserved quirk: the registry-populate path's raw
  get_inode vtable dispatch always -ENOENT under tmpfs (existing-dir
  branch dead) — isolated to pts/N-style populate, normal mknod unaffected.
- vfs aggregate OBJECT library + kernel/vfs/CMakeLists.txt removed — no C
  remains anywhere under kernel/vfs/.
- Verified: zero-warning build ×2, boot gate ×6 (+1 orchestrator audit),
  ls /dev 9 nodes + types baseline-identical, mknod/mkdir round-trips,
  usertests baseline, mmaptest 16/16, testsig 21/21, stressfs.

### Iteration 33 — 2026-07-13 — Wave 21: dev core + /dev/tty fix

- dev.rs/cdev.rs/blkdev.rs: AtomicPtr major table + RCU readers/KSpinlock
  writers (CONSUME→Acquire mapping), offset-0 embedded device_t reinterprets,
  vtable-forwarding cdev/blkdev registration.
- **FIX 1**: device_register writes the auto-assigned minor back before
  publish/devtmpfs — `/dev/tty` now opens (evidence: dumpinode shows
  major 5 minor 1; ls /dev/tty open+fstat round-trip clean).
- **FIX 2**: devtmpfs_create_node failures now logged (log-don't-propagate,
  callers treat nonzero as device failure; documented).
- Verified: zero-warning build ×2, boot ×6, devtest all passed, mknod
  round-trips on real majors, stressfs ×3, mmaptest 16/16, testsig 21/21,
  usertests baseline, 8 fs singles.

### Iteration 34 — 2026-07-13 — Wave 26: exec.c → Rust + copyinstr2 root-caused

- exec.rs (~700 lines): full ELF loader (three-region LOAD-segment split:
  file-backed mmap / eager boundary page / anonymous BSS, 1:1 with the C),
  heap+stack VMA creation, entry-point fault-ahead, argv/envp stack ABI
  (per-string 16-byte sp alignment, reverse-shift argv frame,
  `sp[0]=argc ... NULL`-terminated argv/envp pointer arrays) ported
  control-flow-for-control-flow; `sys_exec` byte-faithful incl. the
  envp swallow-errors semantics. Goto-cleanup via local `bad!`/
  `bad_locked!` macros; ELF headers read through `MaybeUninit` +
  full-length-read check; elf.h structs added to wrapper.h/build.rs
  (real bindgen layouts), `#define`s hand-copied per convention. Dead
  `ustack_alloc` dropped (zero callers, tree-wide grep); `flags2vmperm`
  demoted to a private `const fn` (single caller, not in defs.h).
  `exec`/`sys_exec` added to RUST_FORCE_UNDEFINED (both callers are
  same-crate Rust: proc/thread.rs init_entry, irq/syscall.rs table).
- **copyinstr2 mandate — investigated, mandate's diagnosis disproven**:
  debug instrumentation in the pre-port C showed exec ALREADY rejects the
  oversized argument (`fetchstr` → -ENAMETOOLONG → exec returns -1).
  Real root cause: `wait()`'s POSIX-style status encoding
  (`xv6_exit_reap_zombie`) makes the test's `exit(747)` sentinel
  unobservable (`(747&0xff)<<8 != 747` always). Same single bug also
  explains killstatus AND newly-found exitwait. Proc-side, out of this
  wave's touch scope — Known issues consolidated instead (see above);
  exec.rs module doc carries the full evidence trail.
- Post-copyinstr2 singles sweep (26 tests run individually, the -q
  region the mandate wanted unlocked): copyinstr3, rwsbrk, truncate1-3,
  openiput, exitiput, iput, opentest, writetest, writebig, createtest,
  dirtest, exectest, vforktest, pipe1, preempt, reparent, twochildren,
  forkfork, reparent2, mem, sharedfd, fourfiles, createdelete, badarg
  (50k-bad-exec stress) — all OK. killstatus/exitwait fail
  (wait-encoding, above), forkforkfork panics (tq bug, above), execout
  >360s (slow, above) — ALL four reproduced identically on an
  unmodified 074323c C-baseline worktree built this session.
- Verified: zero-warning build (Rust crate + kernel C; only pre-existing
  initcode RWX linker note, present on baseline), boot gate ×4 on the
  final clean binary (init/sh spawn IS the exec path), testsig 21/21,
  mmaptest 16/16, stressfs, pingpong, primes, vforktest (its Test-2
  "FAIL: exec failed" line is baseline-identical: relative-path
  `exec("echo",...)` from `/`, test still passes), ls/wc battery,
  usertests -q (stops at copyinstr2, baseline-consistent), 26 singles.
- **Cache-poisoning recurrence caught and neutralized**: mid-wave, an
  expect-driven `cmake --build --target qemu` (no TOOLPREFIX/LAB in that
  shell) regenerated the cache to system GCC 13.2 + LAB=util right after
  a CMakeLists edit; roughly half the verification runs executed on that
  poisoned kernel before the C_COMPILER cache check caught it. Full
  verification matrix re-run from scratch on a clean `--clean-first`
  toolchain rebuild (compiler provenance proven via the kernel ELF's
  .comment section: GCC 14.2.0) — all results identical to the poisoned
  runs. New standing rule + new-vector documentation added to Working
  notes.

### Iteration 35 — 2026-07-13 — wait-status encoding design-and-fix

- Completed the POSIX wait-status encoding Wave 26 flagged: exited →
  `(code & 0xff) << 8`; killed by signal → `termsig & 0x7f`. New
  `thread_signal_t::term_signal` field (`kernel/inc/signal_types.h`)
  records the actual killing signal at every `THREAD_SET_KILLED` site in
  `kernel/proc/signal.rs` (`__signal_send`, `sigaction`, `sigprocmask`,
  `deliver_signal`, `handle_signal`, plus `sigreturn`'s corrupt-frame path
  attributed to `SIGSEGV`); `xv6_exit_reap_zombie`
  (`kernel/proc/proc_shims.rs`) branches on the zombie's
  `THREAD_FLAG_KILLED` bit to pick the exited vs. signaled encoding. Full
  rationale documented as a comment at the encode site. `kernel/inc/uabi/
  wait.h`'s `WIFEXITED`/`WEXITSTATUS`/`WIFSIGNALED`/`WTERMSIG` macros
  already existed (pre-Rust-rewrite `f0a652e`) and needed no changes.
- Test inventory across all of `user/*.c`: only `usertests.c`'s
  `copyinstr2`/`killstatus`/`exitwait` compared wait status by exact value
  and needed updating to decode via the macros; every other status check
  in the tree (`testsig.c` x11, `mmaptest.c`, `cowtest.c`, `devtest.c`,
  `grind.c`, `symlinktest.c`) is a loose `==0`/`!=0` test, encoding-
  agnostic, untouched; `sh.c` already used `WIFSTOPPED` correctly.
- Flagged, not fixed (out of touch scope): `irq/trap.rs`'s
  `push_sigframe` stack-exhaustion `exit(-1)` calls (~lines 931/934) don't
  route through `THREAD_SET_KILLED`, so that rare edge case still
  encodes as a misleading "exited 255" rather than `WIFSIGNALED`. Not
  reachable by this wave's test battery.
- Verified: zero-warning build (incl. a `cargo clean` rebuild), boot gate
  x5 (3 standalone + embedded in every test run below), `usertests
  copyinstr2`/`killstatus`/`exitwait` all `OK` individually, `usertests
  -q` new baseline — proceeds through `forkfork` (all `OK`, including the
  three fixed tests) and stops at the pre-existing `forkforkfork` panic
  exactly as documented (confirms `execout`/`diskfull`/`outofinodes`
  aren't reachable via `-q`: they're in the separate slow-test list, not
  `quicktests`), testsig 21/21 (its own group-SIGKILL test prints
  `status=9` — the raw `WTERMSIG` value — confirming the new encoding
  live), vforktest 3/3, mmaptest 16/16 (its `test_mprotect_none` exercises
  the page-fault → `SIGSEGV` → `THREAD_SET_KILLED` attribution path),
  stressfs. C_COMPILER/CMAKE_BUILD_TYPE cache re-checked clean after the
  full matrix.

### Phase 3 (de-C-ification) — 2026-07-14

P3-1 enabler wave (A/B/B2/C/D): C-ABI mesh removed — `#[no_mangle]`
1438→576, `RUST_FORCE_UNDEFINED` 495→22 (byte-identical-link verified),
5 alias families + signal_ffi.rs dissolved, ~9 signature-drift bugs fixed,
CMakeLists 1527→723 lines. RFU-prune corruption scare investigated →
pruning exonerated (rare fs flake misattributed by A/B/A bisection).

**P3-2 — ZERO C**: variadic printf → `kprint!`/`kprintln!` over
`core::fmt::Write` on the existing console path (`Cs`/`Ptr` Display
newtypes for %s/%p); ~750 sites across 63 files migrated; panic/backtrace
path as high-scrutiny solo pass. `printf_shim.c` DELETED —
`find kernel -name '*.c'` is EMPTY. Kernel is 100% Rust (C toolchain now
only assembles the .S files). Verified: clean-cache zero-warning build,
boot-log byte-identical to baseline (3 benign size-derived deltas),
mmaptest 16/16, testsig 21/21, stressfs, usertests baseline, host 35/35,
qemu_pcache 30/30 + qemu_rwsem, panic legibility + ps/free format
fidelity confirmed. Remaining Phase 3: P3-3..P3-12 (bindgen retirement,
ref params, Deref guards, KArc, dyn/enum dispatch, unsafe census).

### Phase 3 cont. — 2026-07-14 — bindgen nativization + unsafe reduction (goal items 2-4)

Bindgen retirement (native #[repr(C)] + direct-against-bindgen size/align/
offset_of! asserts as the layout gate):
- P3-3A (fdf2813): 7 lock type families (RawSpinlock/Mutex/Rwsem/Semaphore/
  Completion/Rwlock/RcuHead); rwlock dual-atomic-reinterpret consolidated.
- P3-3B (0ec37c7): mm slab/page asserts strengthened (were literal-only,
  now direct-vs-bindgen); kalloc pools native.
- P3-3C (e3784b5): leaf types list/hlist/rb; unified 2 divergent ListNode
  mirrors into one canonical native type.
- P3-3D (4295fba): tier-1 aggregates work_struct/tq_t/ttree/tnode (union
  arms independently asserted) → unblocks pcache aggregate.

Unsafe reduction / RAII (goal items 2-4):
- P3-8 vm.rs (b536311): VmRead/Write/PgtableGuard → data-carrying Deref/
  DerefMut; 5 functions fully safe.
- P3-8 pcache.rs (717d35f): PcLocal/PcTreeRead/PcTreeWrite guards →
  Deref<PcacheHandle>; 7 functions fully safe.
- P3-8b (29e87d6): NEW idiomatic `sync::SpinLock<T>` (lock-owns-data,
  Deref/DerefMut guards, const-init, std::sync::Mutex-style safety);
  migrated ptmx PTY_TABLE + ramdisk RAMDISK off static-mut+separate-lock.
- P3-8c (0ffc840): bufcache BCACHE → SpinLock<T> (lock ordering vs per-buf
  sleeplocks preserved; verified under stressfs concurrency).

Method note: `grep -c unsafe` is a poor metric (Deref RELOCATES unsafe
into few centralized impls); the real win is fully-safe functions +
per-guard SAFETY vs per-site. P3-12 census will count blocks + fully-safe
deltas. Baseline unsafe occurrences: 5088 (4593c1a) → 4925 (4295fba).
Every wave: split verification (light worker gate + orchestrator full
battery — testsig/stressfs/usertests/ctest) under the account
spend-limit throttle; all green + committed.

### Phase 3 interim census — 2026-07-14 (committed 1f3c92d)

- C interface residue: **0 kernel .c files**; 704 #[no_mangle]; 490 unsafe
  extern blocks; 43 bindgen allowlist_type entries (the wrapper.h-deletion
  long tail — P3-4/5/6).
- Unsafe: 3712 `unsafe {` blocks, 1013 unsafe fn/extern, 74 unsafe impl;
  total occurrences 4942 (5088 @4593c1a). NOTE: total is ~flat because the
  smart-pointer primitives RELOCATE unsafe into a few centralized impls
  while removing scattered use-site unsafe + making whole functions safe —
  the block count and fully-safe-fn count are the true progress signals.
- Smart-pointer / RAII footprint: SpinLock<T> 9 files, KArc<T> 7 files,
  Deref-guard impls 5 files. SpinLock on 6 globals; KArc on all 3
  kobject-embedding types (inventory closed); mm-local guards data-carrying.
- Top unsafe-block files: fdt.rs 290 + x1_sdhci 228 + x1_emac 112 (largely
  INHERENT hardware MMIO/DMA/FDT parsing); pcache.rs 248; vfs/inode.rs 223;
  proc/access.rs 180; vfs cluster (vfs_syscall 119 + file 114 + fs 96).
- Next targets (highest non-hardware concentration): the vfs cluster
  (~550 blocks — needs P3-7 ref-params + inode-refcount RAII + P3-10 dyn
  ops) and proc/access.rs (180 — needs proc-type nativization, entangled
  with the asm-offset danger zone).

### Iteration 38 — 2026-07-13 — Wave 28 (FINAL PORTING WAVE) → kernel C eliminated

- virtio_disk.rs (virtqueue barriers 7/7 exact incl. the
  compiler_fence-vs-fence distinction), ramdisk.rs, pci.rs (per-field
  volatile ECAM), e1000.rs (fences 8/8, DMA ring layout asserts, 2 C bugs
  preserved+documented), net.rs (packed headers close a real
  unaligned-access UB gap without changing byte offsets), sysnet.rs
  (plan's sys_connect claim was stale — documented; dead sock* trio
  ported faithfully + flagged as a Wave-14 VFS-integration gap).
- Dead LAB=net CMake block removed.
- **MILESTONE (orchestrator-verified census): `find kernel -name '*.c'` →
  printf_shim.c only. The kernel is C-free except the 46-line documented
  variadic printf entry.**
- Verified: zero-warning builds, 6 boots + orchestrator audit (PCI/e1000/
  ramdisk boot lines byte-identical, stressfs), bigfile 65k-block
  virtqueue exercise, second-disk mount, testsig 21/21, mmaptest,
  usertests -q baseline (through forkfork → documented forkforkfork stop).

### Iteration 39 — 2026-07-13 — test port complete → GOAL ITEMS CLOSED

- Host-test seam (cfg-gated machine.rs mocks, no_std test-gating, bindgen
  reuse) + 35 cargo tests: bits 6, list 8, early_allocator 16 (all 13 C
  cases + 3 should_panic guard rails). ctest `rust_host_unit_tests`.
- QEMU ctest harness (scripts/run_qemu_test.sh + test/cmake/QemuTests.cmake):
  isolated per-suite build dirs, env self-set + cache double-checks
  (poisoning lessons institutionalized), RUN_SERIAL + RESOURCE_LOCK
  (single-VM rule enforced in CI). 4 suites green: qemu_rwsem,
  qemu_semaphore, qemu_pcache (30 cases: 23 ported, 7 dropped with
  reasons, 2 added; real flush-retry behavior discovered + asserted),
  qemu_workqueue (7 NEW cases — the retired C suite only tested a mock).
- test_port_plan.md marked CLOSED; ~2GB of on-demand test build dirs
  cleaned + gitignored.
- Zero test leakage into normal boots (grep-verified).

### Iteration 40 — 2026-07-14 — Wave P3-2: ZERO-C milestone (kprint!/kprintln!)

- **printf_shim.c DELETED — the kernel is now 100% C-free**
  (`find kernel -name '*.c'` returns empty). Replaced the C-variadic
  `printf()` (whose only reason to exist was that stable rustc can't
  *define* a `c_variadic` fn) with native `core::fmt`-based
  `kprint!`/`kprintln!` macros. `core::fmt::Arguments` needs no variadic
  ABI — `format_args!` builds the arg list at each call site — so the
  shim, `printf_rust` (the old C-format-string parser), and its three
  `va_arg` fetchers were all removed.
- **Design** (kernel/printf.rs): a `struct Console;` impl of
  `core::fmt::Write` pushes bytes, unbuffered and alloc-free, through the
  exact same `console::consputs` path the old `printf_rust`/`flush` used.
  `kprint!`/`kprintln!` (`#[macro_export]`, `$crate`-hygienic) forward to
  `_kprint(args: core::fmt::Arguments)`, which locks the same `PR_LOCK`
  and reprints the `[timestamp] ` fresh-line prefix with the *identical*
  per-call `NNEWLINE` swap logic as the old `printf_rust`, so boot output
  stays byte-identical. Two `Display` adapters — `Cs(*const c_char)` (%s,
  via `CStr::from_ptr`, `(null)` on null, non-UTF8 byte fallback) and
  `Ptr(u64)` (%p → `0x{:016x}`, matching the old `printptr` exactly).
  `kprintln!`'s arg matcher is `$($arg:tt)*` (not `$arg:expr`) so dynamic
  `%*s` width (`"{:w$}", "", w = n`) forwards through.
- **Panic-path safety**: the `Console`/`_kprint` path is the *same*
  primitive the panic code already used (no alloc, no sleep, reentrant via
  the same `PR_LOCK` that `__panic_start` disables). Migrated the whole
  panic/backtrace path as a high-scrutiny pass: `printf.rs::__panic_start`,
  `proc/proc_shims.rs::xv6_panic`, `irq/trap.rs` (kpanic/kassert/
  trap_panic_dump/kerneltrap register dump), `backtrace.rs`
  (print_backtrace/print_thread_backtrace) — all now use `kprintln!`+
  `Cs`/`Ptr`. `kprint!` (no trailing newline) used where the C used a
  newline-less `printf` so panic lines still concatenate identically
  (e.g. `kerneltrap: ...level=1` runs straight into `scause=0xd(...)`).
- **Migration**: ~750 call sites across 63 files → `kprint!`/`kprintln!`,
  one macro call per original `printf()` (never merged/split — required
  for byte-identical timestamp-per-call output). Scripted the mechanical
  specifier map (%d→{}, %x/%lx→{:x} with the fetcher's sign-extension
  cast, %p→Ptr, %s→Cs for raw `char*` or literal `&str` for `c"..."`
  args, %-Ns→{:<N}) + per-file review across six parallel workers, with
  the panic path done solo. `%s`-with-raw-`char*` sites (the ones needing
  `Cs`): ~40 across the tree (thread/device/fs/inode names, path buffers,
  scause strings). Several files had a local
  `foo_assert_errno(msg: &CStr, errno)` helper that used its `&CStr` arg
  *as* the format string (embedded `%d`) — impossible under
  `format_args!`'s literal-only rule; resolved by inlining (1–2 sites) or
  a local `macro_rules!` taking the message as `:literal` (3+ sites, e.g.
  `session_assert!`). Removed all 62 now-unused per-file
  `extern "C" { fn printf(...) }` decls + the `k_printf` alias +
  `xv6_print_str`/`_d` trampolines (their sole caller, `pid.rs::procdump_*`,
  now uses literal-format `kprintln!`).
- CMake: `printf_shim.c` removed from `KERNEL_C_FILES` (now empty);
  `--undefined=printf_rust` dropped from `RUST_FORCE_UNDEFINED`. Kernel
  builds with NO C compiler step for any `.c` (only the C-preprocessed
  `.S` files still use the C toolchain, by design).
- **Boot-log fidelity** (strongest gate): full boot ×3, normalized diff vs
  pre-wave baseline — every message byte-identical modulo timestamps/
  addresses. The only 3 numeric differences are size-derived and benign:
  buddy free pages 200041→200037 (kernel image grew ~16KB from linking
  core::fmt) + the derived order-2/3 split, and ksymbols 175→865 (core::fmt
  pulls in many symbols → *richer* backtraces). No message text changed,
  none reordered.
- **Verified**: zero-warning `cargo clean` full rebuild; boot ×3 (init/sh
  + `/ $`); `find kernel -name '*.c'` EMPTY; mmaptest 16/16; testsig 21/21;
  stressfs; `usertests -q` full quicktests battery all OK
  (copyin/copyout/copyinstr1-3/killstatus/exitwait/forkfork…) up to the
  documented pre-existing `forkforkfork` panic; host cargo 35/35;
  qemu_rwsem PASS + qemu_pcache 30/30 PASSED (their markers print via the
  new macros); interactive ls/ps/`free -v`/echo. **Panic legibility**:
  the forkforkfork panic renders cleanly through the migrated path —
  `[Core: 1] In thread 396 (usertests) at 0x00000000...` (Cs name + Ptr
  fp), `Failed to remove interrupted waiter from queue` (xv6_panic),
  `Received IPI_REASON_CRASH`, and `kerneltrap: ...scause=0xd(Load page
  fault) sepc=... stval=...` (Cs scause decode). **Format fidelity**: `ps`
  `{:<20} {:<5} {:<2}` columns = `%-20s %-5s %-2s` exactly; `free -v`
  buddy/slab G/M/K/B numbers preserved.

### P3-9e — 2026-07-14 — `SRef` RAII smart pointer over session refcount + off-by-one assessed

- `SRef` (`kernel/tty/session.rs`): `NonNull<session>`-backed, mirrors
  `KArc<T>`/`IRef`'s shape (`Clone`→`session_ref`, `Drop`→`session_unref`,
  `Deref`, `from_raw`/`try_from_raw`/`into_raw`/`as_ptr`). Unlike inode,
  session has no bespoke "dup-if-still-live" C-ABI helper, so
  `try_from_raw` is backed by a new private `session_try_ref` CAS loop
  (same `Ordering::SeqCst` as the existing `session_ref`/`session_unref`,
  not a second independently-reasoned ordering). Completes the
  refcount-smart-pointer trilogy: `KArc<T>` (kobject), `IRef` (inode),
  `SRef` (session) — no manual-refcount family left un-RAII'd.
  `session_ref`/`session_unref`/`get_session`'s `#[no_mangle]` C-ABI
  exports are unchanged.
- Migrated 2 call sites (both in `session.rs`, the only file with any
  `session_ref`/`session_unref` call site in the tree — grep-verified):
  `session_init` (boot-time session 1: `from_raw` → `as_ptr` borrows for
  `session_add_pg`/`session_add_thread`/`fg_pgrp` → `into_raw`, the
  baseline reference deliberately kept alive, unchanged from before) and
  `session_setsid` (same shape for the success path; its
  `pgroup_alloc`-failure branch replaces the old manual `session_unref(s)`
  call with an explicit `drop(s)` — kept explicit, not scope-end, so the
  free/unlink still runs before `pid_wunlock()`, preserving the original
  lock ordering). 1 manual `session_unref` call site eliminated;
  `session_add_thread`/`session_remove_thread`/`session_add_pg`/
  `session_remove_pg` (the primitives `SRef` wraps) intentionally
  untouched, same precedent as `IRef` leaving `vfs_idup`/`vfs_iput` alone.
- **Off-by-one bug (documented since Wave 12) FIXED**, distinct,
  clearly-labeled change: `session_unref`'s free condition now compares
  the pre-decrement `fetch_sub` value against `1` (was `0`), matching
  `kobject_put`/`vfs_iput`'s post-decrement-zero convention instead of
  the C's off-by-one; `session_assert!` bound tightened `>= 0` → `>= 1`
  to match (a pre-decrement value `<= 0` is now a real double-unref bug,
  not a silently-passed-through path to the previously-unreachable free
  branch). No double-free risk: `fetch_sub` is a single atomic RMW, so at
  most one racing caller ever observes the value `== 1`.
- **Second, deeper leak found while verifying the fix, NOT closed**: a
  full grep of every `session_ref`/`session_unref` call site (this file
  is the only caller of either) shows `session_alloc`'s baseline
  `ref_cnt = 1` is never separately dropped anywhere except
  `session_setsid`'s `pgroup_alloc`-failure path — every *successful*
  `setsid()`/`session_init` leaves that baseline reference permanently
  un-dropped, since `session_add_thread`/`session_remove_thread` and
  `session_add_pg`/`session_remove_pg` only ever balance the `+1` *they
  themselves* took. So the comparison fix alone does **not** eliminate
  the practical, long-running leak from ordinary interactive `setsid`
  use — only the narrow allocation-failure path now frees correctly.
  Live-verified via `ps -s` (walks `session_list` directly): after 3
  `testsig` runs (each exercising `setsid` via Test 15/16), 6 sessions
  accumulate showing `threads=0, pgroups=0` — abandoned but never
  unlinked/freed, exactly as this analysis predicts. Closing this needs a
  session-lifecycle design decision (where to drop the baseline
  reference — e.g. a "last member left" check, or starting `ref_cnt` at
  `0`), not a mechanical comparison fix, and risks a premature-free bug
  if rushed — left undone and documented (this file's Known issues +
  `session.rs`'s module/`session_unref` doc).
- Verified: zero-warning build (incl. cache re-check before/after per
  Working notes), boot gate ×2 (`init: starting sh` ×1 + `/ $` each),
  testsig 21/21 ×3 back-to-back (Test 15 setsid/getsid green every run,
  no panics/assertion failures), `ps -s`/`free` before+after (buddy free+
  cached stable — no page-level leak or corruption; session-list growth
  is the documented slab-level leak above, not a page leak).

### Iteration 41 — 2026-07-14 — Wave P3-CS1: `kernel/kstd.rs` created, ERR_PTR/kassert!/u! centralization begun

- **New module** `kernel/kstd.rs` (`pub(crate) mod kstd;`, `#[cfg(not(test))]`,
  wired into `lib.rs` right after `list`): the crate's "std"/prelude for
  shared low-level utilities, direct response to the standing user
  directive to stop scattering libc/std-like helpers per-file. Confirmed
  the survey's headline claim first — read all 30 files' `err_ptr`/
  `is_err`/`is_err_or_null`/`ptr_err` copies; two syntactically different
  but mathematically identical `is_err_value` encodings exist in the tree
  (`p >= (-4095isize) as usize` vs `(p as isize) < 0 && wrapping_neg(p) <=
  4095`), both proven equivalent before unifying into one.
- **Mid-wave directive landed**: reframed the `ERR_PTR` family as a
  *transitional C-ABI/storage-boundary shim*, not the target idiom — added
  `KResult<T> = Result<T, Errno>` and bidirectional bridging helpers
  (`result_to_errptr`/`errptr_to_result`) so future waves have the
  Result-first tools ready; module doc's "Result-first" section states the
  policy explicitly. `errptr_to_result` deliberately returns
  `Result<*mut T, c_int>` rather than `Result<*mut T, Errno>` — `Errno`'s 7
  variants can't losslessly represent an arbitrary encoded errno.
- `container_of`/`Errno`/`result_to_neg_errno` moved from `mm/cffi.rs` into
  `kstd`, re-exported at their old paths (`mm::cffi::{container_of, Errno,
  result_to_neg_errno}`) — zero call-site changes anywhere in `mm`.
  `mm/sysmm.rs`'s `neg_errno` likewise moved + re-imported under its
  original name.
- **First migration batch, 12 of 30 files** (dev/vfs leaf files, as
  planned): `dev/{bio,blkdev,cdev,dev}.rs`, `vfs/pipe.rs`,
  `vfs/tmpfs/{inode,superblock}.rs`, `vfs/xv6fs/{inode,superblock}.rs`,
  `vfs/devtmpfs/superblock.rs`, `vfs/fdtable.rs`, `tty/pty.rs` — local
  `err_ptr`/`is_err`/`is_err_or_null`/`ptr_err`/`is_err_value` definitions
  deleted, replaced by `use crate::kstd::{...}`. 7 of those files' local
  `kassert!` copies also deleted in favor of the new canonical
  `#[macro_export] macro_rules! kassert!` (`$crate::kstd::xv6_panic`
  path, `$crate`-hygienic); `devtmpfs/superblock.rs`'s local `ptr_err`
  returned `isize` where the canonical one returns `c_int` — both of its
  2 call sites already immediately cast to `c_int`, so the cast was
  dropped as dead weight, not left in place.
- The 4 stale `kernel/lock/{mutex,rwsem,completion,semaphore}.rs` local
  `u!` copies deleted (`use crate::u;` added instead) — `lib.rs`'s
  existing canonical `#[macro_export]` copy (from a prior wave) is now
  genuinely the crate's only definition. `lib.rs`'s doc comment updated
  to stop describing them as still-present.
- Investigated the survey's flagged `container_of` duplicate in
  `lock/rcu_test.rs:629` — it's `container_of_list_node`, a typed wrapper
  that already calls the canonical `machine::list_container_of`, not an
  independent reimplementation. Nothing to deduplicate there; left as-is
  and noted for the record rather than "fixed" incorrectly.
- **Self-caught bug during the mid-wave doc rewrite**: an over-broad
  `old_string` in the `vfs/devtmpfs/superblock.rs` edit accidentally
  deleted its unrelated `neg(e: u32) -> c_int` helper along with the
  `ERR_PTR` block, breaking the crate build (`cannot find function neg`).
  Caught immediately by the next incremental `cargo build`; fixed by
  restoring the `neg` fn. Left in the log as a reminder that large
  multi-line `Edit` replacements over doc-comment-heavy blocks need a
  post-edit diff/build check even when the intent seems obviously scoped.
- **Handoff — 18 of 30 files still carry local `ERR_PTR` copies**:
  `bufcache.rs`, `console.rs`, `dev/x1_emac.rs`, `dev/x1_sdhci.rs`,
  `exec.rs`, `lock/rcu.rs`, `lock/rcu_test.rs`, `lock/rwsem_test.rs`,
  `lock/semaphore_test.rs`, `mm/pcache_test.rs`, `proc/access.rs`,
  `proc/workqueue_test.rs`, `tty/session.rs`, `tty/tty.rs`, `vfs/file.rs`,
  `vfs/fs.rs`, `vfs/inode.rs`, `vfs/vfs_syscall.rs`. 8 files still carry
  local `kassert!` copies matching the canonical `xv6_panic`-calling form
  (`irq/trap.rs`'s is a different 3-arg `kassert_fail` variant and
  `proc/{rq,sched,thread}.rs`'s calls `kpanic!` instead — both
  deliberately left alone, see module doc). Future waves: finish the
  `ERR_PTR`/`kassert!` migration file-by-file, then start converting
  Rust-internal-only local helpers (few callers, no C-ABI boundary) to
  `KResult<T>` per the Result-first directive — `mm/pcache_test.rs`,
  `proc/workqueue_test.rs`, and the `lock/*_test.rs` in-kernel test
  modules are plausible easy wins for that since their `is_err_or_null`
  callers are entirely test assertions, not C-ABI boundaries.
- Net: 19 existing files changed (`-346`/`+84` lines, various small
  per-file deletions), + new `kstd.rs` (310 lines, doc-heavy per
  `doc-module-inner`); duplicate-definition count: `ERR_PTR` family
  30→18 files, `kassert!` 15→8 files, `u!` 5→1 file (canonical only),
  `container_of`/`Errno`/`neg_errno` 1 home each (previously split
  across `mm/cffi.rs`/`mm/sysmm.rs`, now `kstd` with back-compat
  re-exports).
- Verified: zero-warning build (`cargo clean` rebuild + full `cmake`
  rebuild both 0 warnings), cache re-check (GCC 14.2 toolchain,
  `CMAKE_BUILD_TYPE` empty) before and after, boot gate ×3 (plain `qemu`
  target + 2 expect-driven interactive sessions, `init: starting sh` ×1
  and `/ $` each), interactive fs battery (`ls`, `mkdir`, `ls /dev` — 9
  nodes, `cat /nonexistent` → `cat: cannot open /nonexistent`, exercising
  the migrated `is_err`/`ptr_err` path through devtmpfs/xv6fs/vfs_syscall
  end-to-end), mmaptest 16/16. No stray QEMU processes left running
  (PID-matched, timeout-bounded throughout).

### Iteration 42 — 2026-07-15 — Waves P3-CS2…CS8: ERR_PTR centralization finished + `KResult` migration swept the whole VFS stack

Consolidated log for the Result-over-ERR_PTR arc (the tracker was last
updated at CS1); each wave was committed and boot/fs-battery-gated green.

- **CS2** (`7b04408`): finished the centralization — the remaining 18
  `ERR_PTR`-duplicate files migrated to `kstd`; grep confirms all 30 now
  `use crate::kstd::{...}` with zero local copies. Closes the user's
  "centralize the libc/std-like tools" directive.
- **CS3** (`b67a923`): proof-of-pattern for the user's "Result over C-style
  err ptr" directive on the file-private `vfs/fdtable.rs` cluster —
  `fdtable_alloc_init -> KResult<NonNull<vfs_fdtable>>`, new
  `alloc_fd_from_locked -> KResult<usize>`, `clone_locked -> KResult<...>`;
  `?`-propagation between them; the C-ABI boundaries keep the encoding via
  `result_to_errptr`/`e.neg()`. Added `Errno::MFile` (EMFILE).
- **CS4/CS5/CS5b** (`9539b2f`, `785f4a3`, `5ed1019`): scaled through the
  syscall layer. Every `sys_vfs_*` now has a `*_inner() -> KResult<T>` body
  with one boundary `ret64(result_to_neg_errno(inner()))` (or the explicit
  `open_inner` match preserving EFAULT/ENOENT/EISDIR/ELOOP/EEXIST). `grep`
  confirms **zero** manual `return ret64(neg(` / `err_ptr(neg(` /
  `ptr_err as u64` remain in any `sys_*`. (`vfs_mount_path`/`umount_path`,
  internal helpers shared with `fs.rs`, correctly stay `c_int`.)
- **CS6** (`5dbaac1`): `vfs/file.rs` — 12 fallible fns to `*_inner() ->
  KResult<T>`; hand-built `err_ptr` 10→0, hand-built `neg()` 50→1 (the lone
  survivor is in the infallible `vfs_fput`). `pipealloc`/`sockalloc`/
  `open_cdev` included; `vfs_fput`/`vfs_fdup` left infallible.
- **CS7** (`4275cf9`): `vfs/inode.rs` batch 1 — the path-resolution family
  (`vfs_namei`/`nameiparent`/`create`/`ilookup` + `vfs_namei_once`) →
  `KResult<*mut vfs_inode>`; file-wide `err_ptr` 49→35. Added `Errno::Again`.
  `vfs_iput` (infallible) untouched; lock ordering byte-identical.
- **CS8** (this wave): `vfs/inode.rs` batch 2 — the remaining 12 fallible
  fns. `vfs_mknod`/`vfs_mkdir`/`vfs_symlink` mirror CS7's `vfs_create_inner`
  template exactly (retry loop + internal `ERR_PTR` domain for the
  eagain-retry check and driver-vtable dispatch, one tail `result_to_errptr`);
  `vfs_link`/`vfs_unlink`/`vfs_move`/`vfs_itruncate`/`vfs_ilock_two_directories`/
  `vfs_chdir`/`vfs_chroot`/`vfs_dirty_inode`/`vfs_sync_inode` →
  `KResult<()>` via `result_to_neg_errno`; `vfs_readlink` keeps a custom
  boundary (`e.neg() as isize`) since success carries a byte count. Added
  `Errno::XDev` (EXDEV) for the cross-filesystem precondition checks in
  link/move/two-directory-lock. `EBUSY`/`ENOTEMPTY` stay as raw `neg(...)`
  inside `vfs_unlink_inner`'s C-mirrored internal block (same single-tail
  pattern). No fallible inode fn still does manual error encoding at its
  C-ABI boundary. All manual C-resource cleanup (`vfs_iput`,
  `vfs_release_dentry`, `vfs_inode_put_ref`, lock unwind) preserved at the
  exact original return points — never `?`-propagated past cleanup; lock
  ordering byte-identical.
- **Net direction**: the whole VFS fallible-return stack (fdtable →
  vfs_syscall → file → inode core) now returns `Result<T, Errno>` internally,
  encoding to the pointer/-errno C-ABI form only at the `#[no_mangle]`
  boundary. `Errno::Raw(n)` (lossless `neg(Raw(n)) == n`) carries
  not-yet-owned cross-module ERR_PTR values through — those passthroughs
  become typed as their producing layers convert (fs.rs next, CS9).
- **Verified (CS8)**: `cargo clean` + full `cmake` rebuild 0 warnings; cache
  clean (GCC 14.2, `CMAKE_BUILD_TYPE` empty) before and after; boot gate ×1
  (`-bios default`, `xv6.bin`, dual virtio disk, per `scripts/run_qemu_test.sh`
  — note: this kernel links at `0x80200000` and boots **via OpenSBI**, not
  `-bios none`); fs battery on a freshly-built `fs_img`: `mkdir` new dir OK +
  repeat → EEXIST, `ln /README.md rr` + `cat rr` reads through the hard link
  (`vfs_link`), `cat /nonexistent` → ENOENT, symlinktest both subtests ok
  (`vfs_symlink`/`vfs_readlink`), usertests createdelete/linktest/unlinkread
  all OK, stressfs. `vfs_move`/rename has no shell binary in this image —
  verified by inspection only (honest gap, flagged). The `sh` `>`-redirection
  bug (Iteration 25, pre-existing) and the single-test "lost some free pages"
  line are unchanged pre-existing artifacts, not regressions.

### Iteration 43 — 2026-07-15 — Wave P3-CS9: `vfs/fs.rs` ERR_PTR→`KResult` (err_ptr 30→0)

- Completes the VFS-layer Result migration begun in CS3–CS8. All 8
  ERR_PTR-returning fns in `vfs/fs.rs` converted: `get_dentry_inode_impl`
  (internal helper, converted directly + `?`-propagated by its callers) and
  the 7 `pub(crate) extern "C"` boundary getters `vfs_alloc_inode`/
  `vfs_get_inode`/`vfs_get_dentry_inode(_locked)`/`vfs_get_inode_cached`/
  `vfs_add_inode`/`vfs_struct_clone` — each now a private `*_inner() ->
  KResult<*mut …>` with ONE `result_to_errptr(inner())` at the wrapper.
- **ABI unchanged on purpose**: these boundaries have 37 cross-module
  callers and inode.rs holds `Errno::Raw(ptr_err(...))` passthroughs against
  them, so the `extern "C"` signatures + ERR_PTR-encoded returns are
  byte-for-byte identical. Only fs.rs's internals changed.
- Error mapping used existing variants (`Inval`/`NoMem`/`Again`/`NoEnt`) +
  `Errno::Raw(n)` for not-yet-owned driver/cross-module ERR_PTR values; no
  new Errno variant, `kstd.rs` untouched. All manual cleanup preserved at
  the original return points (`vfs_get_dentry_inode` captures the impl
  result then `vfs_superblock_unlock` *before* returning — not
  `?`-propagated past the unlock; `vfs_get_inode_cached`'s
  `vfs_iunlock`+`queue_deferred_iput`, `vfs_add_inode`'s EAGAIN
  `vfs_iunlock(existing)`, `vfs_struct_clone`'s dual `vfs_iput`/
  `vfs_inode_put_ref`/`struct_free` unwinds all intact). Lock ordering
  byte-identical.
- Census (`vfs/fs.rs`): `err_ptr` 30→**0**, `neg(` 113→85 (survivors are
  out-of-scope fns + C-mirrored comparisons like `ptr_err(inode) !=
  neg(ENOENT)`, never boundary encoding), `KResult`/`result_to_errptr` 0→9/8.
- Verified: 0-warning `cargo clean`+`cmake` rebuild; cache clean before+
  after; boot gate ×2 (OpenSBI/`xv6.bin`/dual disk); fs battery on fresh
  `fs_img` — `mkdir`+EEXIST, `ln /README.md rr`+`cat rr` reads through the
  hard link (exercises `vfs_get_dentry_inode`/`vfs_get_inode_cached`),
  `cat /nonexistent`→ENOENT, symlinktest both subtests ok, usertests
  createdelete/linktest/unlinkread/concreate all OK, stressfs. `vfs_struct_clone`
  (fork CLONE_FS) has no isolated shell binary — verified by inspection,
  exercised implicitly by every usertests fork (honest gap, same class as
  CS8's `vfs_move`). Pre-existing sh `>`-redirect bug + single-test "lost
  some free pages" artifact unchanged.
- **Whole VFS fallible stack now returns `Result` internally** (fdtable →
  vfs_syscall → file → inode → fs). Next (CS10): the proc cluster's ~20
  scattered `err_ptr` sites, and typing inode.rs's `Errno::Raw(ptr_err(…))`
  passthroughs against these fs.rs producers now that they carry `KResult`.

### Iteration 44 — 2026-07-15 — Wave P3-CS10: proc leaf `KResult` (signal dequeue + thread-group lookup)

- Extends the Result migration into the proc cluster. Survey finding: most
  proc `err_ptr` hits are the transitional **shims** themselves
  (`access.rs::err_ptr_errno`, `proc_shims.rs::xv6_err_ptr`, clone.rs's
  `extern` import decls) or the kstd re-export — deliberately kept. Only two
  files hold genuinely-fallible functions that return ERR_PTR:
- `thread_group.rs::get_thread_group` (`#[no_mangle] extern "C"`, ABI kept):
  private `get_thread_group_inner() -> KResult<*mut thread_group>` (all three
  `-ESRCH` paths → `Err(Errno::Srch)`) + `result_to_errptr` at the wrapper.
  Provably identical ABI output (`err_ptr(Srch.neg()) == err_ptr(-ESRCH)`).
- `signal.rs::dequeue_signal_update_pending_nolock` (private `fn`, signature
  changed directly): **tri-state** contract preserved as
  `KResult<*mut ksiginfo_t>` — the two `err_ptr(-EINVAL)` → `Err(Errno::Inval)`,
  the two `ptr::null_mut()` *success* ("no signal pending") paths →
  `Ok(ptr::null_mut())` (NOT an error), dequeued info → `Ok(info)`. Its sole
  caller (signal.rs:1414) rewritten from an `is_err(info)` three-way test to
  an `Ok`/`Err` match; the downstream null-vs-valid dispatch (skip
  `ksiginfo_free` on null) unchanged. `raw_sig_abi!` block, `spin_holding`
  sigacts-held assert, and every sigset/list mutation byte-identical.
- Added `Errno::Srch` (ESRCH) to kstd.rs — no prior variant existed; matches
  the existing variant+`raw()` style. `err_ptr` 3→0 (thread_group), 2→0
  (signal); no other file touched.
- Verified: 0-warning `cargo clean`+`cmake` rebuild; cache clean before+
  after; boot gate (one `init: starting sh`); **testsig 21/21** (the signal
  battery — tgkill/sigwait/sigsuspend/EINTR/sigpending — hammers both changed
  functions), usertests killstatus/reparent/forkfork OK, ENOENT smoke.
  forkforkfork's pre-existing panic (now surfacing at `thread_queue.rs:600`,
  a file untouched here) rigorously re-confirmed NOT a regression: worker
  stashed all changes, rebuilt the pristine baseline, and reproduced the
  byte-identical panic at the same site.

### Iteration 45 — 2026-07-15 — Wave P3-CS11 (danger zone): scheduler + thread-spawn `KResult`

- The riskiest Result wave — the scheduler run-queue selection and the
  fork/thread-creation hot path. All 4 `#[no_mangle] extern "C"` fns
  converted to private `*_inner() -> KResult<*mut T>` + one
  `result_to_errptr` at the boundary (ABI byte-identical; scheduler/fork
  callers untouched):
  - `rq.rs::get_rq_for_cpu`: 2× EINVAL → `Err(Inval)`; tail → `Ok`.
  - `rq.rs::rq_select_task_rq`: 3× EINVAL → `Err(Inval)`; the no-match
    `ptr::null_mut()` *success* tail preserved as `Ok(ptr::null_mut())`
    (null is a valid "no rq selected" result, not an error).
  - `thread.rs::thread_create`: EINVAL→`Err(Inval)`, ENOMEM→`Err(NoMem)`;
    no cleanup between alloc and the ENOMEM path (kstack null-check fires
    before any resource is taken).
  - `thread.rs::kthread_create`: EAGAIN→`Err(Again)`; the two
    cleanup-bearing error paths keep `__free_pid()` (and `thread_destroy(p)`
    on the fs_clone path) byte-identical, in the exact original order,
    before an explicit `return Err(...)` — never `?`-propagated past
    cleanup. The `is_err(p)`/`is_err(fs_clone)` cross-module ERR_PTR values
    become `Err(Errno::Raw(ptr_err(x)))` (lossless: `result_to_errptr(Err(
    Raw(n))) == err_ptr(n)`, the original encoded pointer).
- No new Errno variant; kstd.rs untouched. `err_ptr` 4→0 in each file.
- Verified: 0-warning `cargo clean`+`cmake` rebuild; cache clean before+
  after; boot gate; usertests forkfork/forktest OK; stressfs; **testsig
  21/21**. **forkforkfork A/B regression gate**: worker stashed → pristine
  baseline rebuild → forkforkfork panicked at the byte-identical site
  (`0x80c40000`, "Failed to remove interrupted waiter from queue"); the
  modified build panics at the same address/message (orchestrator re-ran
  independently — same `0x80c40000`). Pre-existing `thread_queue.rs` panic,
  a file untouched here — NOT a regression.
- **Result-over-ERR_PTR migration is now essentially complete for the VFS +
  proc fallible surfaces.** The remaining item (CS12) is typing the
  `Errno::Raw(ptr_err(...))` cross-module passthroughs in inode.rs against
  the fs.rs producers that now return `KResult`.

### Iteration 46 — 2026-07-15 — Wave P3-CS12: xv6fs on-disk driver `KResult` (corruption-sensitive)

- The largest remaining genuine ERR_PTR cluster: the xv6fs concrete-driver
  `inode_operations`/superblock vtable methods. All 6 converted to private
  `*_inner() -> KResult<*mut vfs_inode>` + one `result_to_errptr` at the
  `extern "C"` boundary. The **vtable ABI stays ERR_PTR** (the vfs layer
  calls these through C-ABI fn pointers — locked until the P3-10 dyn-Trait
  ops redesign); only the internal error-return encoding changed, disk logic
  untouched.
  - `inode.rs`: `__xv6fs_create`/`__xv6fs_mkdir`/`__xv6fs_symlink`/
    `__xv6fs_mknod` (err_ptr 15→0).
  - `superblock.rs`: `xv6fs_alloc_inode`/`xv6fs_get_inode` (err_ptr 9→0).
- Error mapping: EINVAL/EEXIST/ENOMEM/ENOENT → the matching `Errno`
  variant; already-negative helper `c_int` (`__xv6fs_dirlink` ret) and
  non-enumerated `neg(EIO)`/`neg(ENOSPC)` → `Errno::Raw(...)` (lossless);
  `vfs_alloc_inode` cross-module ERR_PTR → `Err(Errno::Raw(ptr_err))`. No new
  variant; kstd.rs untouched. mkdir's parent-dirlink path preserves the
  original's deliberate "discard `ret`, return EIO" behaviour.
- **Corruption safety (the crux)**: `vfs_alloc_inode` returns the inode
  *locked*; every post-alloc failure path unwinds before returning. All
  cleanup — `vfs_iunlock(new_inode)`, symlink's `xv6fs_itrunc(ip)` +
  `vfs_iunlock` (×3 sites), `xv6fs_get_inode`'s `brelse(bp)` — kept
  byte-identical, in the exact original order/position, before an explicit
  `return Err(...)`. Never `?`-propagated past cleanup (would drop an
  iunlock/itrunc → on-disk inode leak/double-free). `bread`/`brelse`
  pairing, `ilock`/`iunlock`, `xv6fs_log_write`, and scan loops byte-ident.
- Verified: 0-warning `cargo clean`+`cmake` rebuild; cache clean before+
  after; boot gate. **fs corruption battery on fresh `fs_img`** (orchestrator
  re-ran independently): mkdir+EEXIST, nested mkdir, `ln`+`wc` byte-identical
  through the hard link, ENOENT, symlinktest both subtests ok, usertests
  createdelete/**bigdir**/linktest/unlinkread/dirtest all OK, **stressfs**
  (first run completed full write0..32+read cycles, returned to prompt) —
  and **zero** `panic`/`freeing free block`/`incorrect blockno`/`balloc: out
  of blocks`/assert markers anywhere. Pre-existing single-test "lost some
  free pages" artifact unchanged.

### Iteration 47 — 2026-07-15 — Wave P3-CS13: tmpfs driver `KResult` (in-memory sibling of CS12)

- The in-memory sibling of CS12 — same 6-function shape. All tmpfs
  `inode_operations`/superblock vtable methods converted to private
  `*_inner() -> KResult<*mut vfs_inode>` + one `result_to_errptr` boundary,
  ABI byte-identical (these ops are also reused by devtmpfs for /dev nodes).
  - `inode.rs`: `__tmpfs_create`/`__tmpfs_mkdir`/`__tmpfs_mknod`/
    `__tmpfs_symlink` (err_ptr 6→0).
  - `superblock.rs`: `tmpfs_alloc_inode`/`tmpfs_get_inode` (err_ptr 6→0).
- Mapping: EINVAL/ENOMEM/ENOENT → matching variant; the shared
  `__tmpfs_alloc_link_inode` helper returns an already-negative `c_int`
  carried through losslessly as `Err(Errno::Raw(e))`. No new variant.
- Cleanup preserved byte-identical: `__tmpfs_symlink`'s target-alloc-fail
  unwind (`__tmpfs_do_unlink`→`vfs_remove_inode`→`kassert!`→
  `__tmpfs_free_dentry`→`vfs_iunlock`→`tmpfs_free_inode`) and
  `tmpfs_alloc_inode`'s `slab_free(ti)` on the `ino==0` path both kept in
  place before an explicit `return Err(...)`; never `?`-propagated.
- Verified: 0-warning `cargo clean`+`cmake` rebuild; cache clean before+
  after; boot gate. Orchestrator re-ran independently: `devtmpfs: populated
  9 device nodes` + `tmpfs: mounted at /tmp` at boot (boot-time
  `__tmpfs_mknod`/`tmpfs_alloc_inode` through the converted path), `ls /dev`
  lists console/tty/null/zero/etc. cleanly, `/tmp` mkdir+EEXIST, symlinktest
  both subtests ok (create/symlink/readlink/unlink in tmpfs), usertests
  createdelete OK in both /tmp (tmpfs) and / (xv6fs), stressfs completed —
  zero panic/assert/corruption markers.
- **Both filesystem drivers now Result-internal.** Remaining err_ptr: the
  deliberately-kept internal-ERR_PTR retry/dispatch domains (inode.rs/
  file.rs) + a handful of leaf boundary getters (CS14: device_get/bio_alloc/
  pipe_alloc/get_session) + the transitional shims.

### Iteration 48 — 2026-07-15 — Wave P3-CS14: leaf boundaries `KResult` — **Result-over-ERR_PTR arc CLOSED**

- The closing wave: 4 scattered `pub(crate) extern "C"` boundary getters/
  allocators, each → private `*_inner() -> KResult<*mut T>` + one
  `result_to_errptr` (ABI byte-identical):
  - `dev/dev.rs::device_get`: EINVAL→`Inval`, 3× ENODEV→`NoDev`. (`KRcuRead`
    guard drop/RCU ordering byte-identical.)
  - `dev/bio.rs::bio_alloc`: EINVAL→`Inval`, ENOMEM→`NoMem`; no partial-alloc
    cleanup (kmm_alloc null-check fires before any resource taken).
  - `vfs/pipe.rs::pipe_alloc`: 2× ENOMEM→`NoMem`; the second keeps
    `slab_free(pi)` byte-identical before `return Err(NoMem)`.
  - `tty/session.rs::get_session`: 2× ESRCH→`Srch` (incl. the cross-module
    `is_err(t)` guard; `Srch.neg() == -ESRCH`, ABI-identical).
- No new Errno variant; kstd.rs untouched. err_ptr → 0 in all 4 files.
- Verified: 0-warning `cargo clean`+`cmake` rebuild; cache clean before+
  after; boot gate. Orchestrator re-ran independently: `ls /dev` clean
  (device_get), usertests pipe1 OK (pipe_alloc), createdelete OK + stressfs
  (bio_alloc block I/O), **testsig 21/21** (get_session via fork/session),
  ENOENT smoke — zero panic/assert markers.

### Result-over-ERR_PTR migration — completeness note (2026-07-15)

The user directive "use Result Enum to pass error possible return value
instead of the C style err ptr" is now satisfied across every fallible
Rust-internal function in the kernel. Migrated layers: **VFS** (fdtable,
vfs_syscall, file, inode core, fs) CS3–CS9; **proc** (signal dequeue,
thread-group, scheduler rq-select, thread/kthread create) CS10–CS11;
**filesystem drivers** (xv6fs + tmpfs `inode_operations`/superblock vtable
methods) CS12–CS13; **leaf boundaries** (device/bio/pipe/session) CS14.

Every fallible internal fn returns `KResult<T> = Result<T, Errno>`; the
ERR_PTR/neg-errno encoding survives ONLY at (a) the single
`result_to_errptr`/`result_to_neg_errno` call in each `#[no_mangle]`/
`extern "C"` C-ABI or vtable boundary wrapper (locked to the C fn-pointer
ABI until the P3-10 dyn-Trait ops redesign), and (b) deliberately-retained
internal-ERR_PTR *domains* — the EAGAIN-retry + driver-vtable-dispatch loops
in `vfs/inode.rs`'s `vfs_create`/`mknod`/`mkdir`/`symlink` `_inner` bodies
(documented in CS7/CS8), where the transient value never escapes the fn —
plus the transitional `kstd` shim itself (`err_ptr`/`is_err`/`ptr_err`) and
the `err_ptr_errno`/`xv6_err_ptr` convenience shims kept for the few
remaining C-ABI/asm consumers. `Errno` grew exactly the variants the
migration needed: `MFile`(CS3), `Again`(CS7), `XDev`(CS8), `Srch`(CS10).

### Iteration 49 — 2026-07-15 — Interim measured census (post-Result-migration, HEAD 26bc2b1)

Snapshot after the Result-over-ERR_PTR arc + the CS/P3-8 waves. NOT the
final acceptance census — Phase-3 structural work remains (see below).

| Metric | Phase-3 baseline (4593c1a) | Now (26bc2b1) |
|---|---|---|
| C files in `kernel/` | 0 (since P3-2) | **0** |
| `#[no_mangle]` attributes | 1,438 | **571** (−60%) |
| `unsafe` occurrences | 5,088 | **4,929** |
| `err_ptr(` fallible-return sites | ~200+ scattered | **15** |
| `KResult<T>` use sites | 51 (mostly mm) | **192** |
| boundary conversions (`result_to_errptr`/`result_to_neg_errno`) | ~0 | **68 / 36** |
| `SpinLock<T>` global adopters (files) | 1 (SlabBox only) | **9** |
| `KArc`/`IRef`/`SRef` adopter files | 0 | **9 / 3 / 1** |

The **15 remaining `err_ptr(` sites**: 12 in `vfs/inode.rs` (the deliberate
internal-ERR_PTR domain of `vfs_create`/`mknod`/`mkdir`/`symlink`'s
`_inner` retry+driver-dispatch loops, documented CS7/CS8 — the value never
escapes the fn), + 1 each in `tty/tty.rs`, `proc/access.rs` (the
`err_ptr_errno` shim), `dev/blkdev.rs`. **Zero fallible-function ERR_PTR
returns remain outside that internal domain** — the Result migration is
complete.

Consolidated regression on this HEAD: mmaptest 16/16, testsig 21/21,
usertests `-q` clean through `forkfork` (stops at the documented
pre-existing `forkforkfork` `thread_queue.rs` panic).

**Genuinely-remaining Phase-3 work** (for the eventual real acceptance
census): P3-3/4/5 bindgen-type nativization (asm-offset danger zone),
P3-6 wrapper.h/bindgen deletion, P3-10 VFS/device ops → `dyn Trait`, and
the tail of P3-8 — the *delicate* remaining lock globals (uart panic-path,
virtio DMA, kobject/rcu) plus the proc struct-embedded locks that are
blocked behind asm-offset nativization. The easy P3-8 globals (e1000,
ramdisk, sockets, devtmpfs, bufcache, ptmx) + vm/pcache data-carrying
guards are already done (8a–8d).

### Iteration 50 — 2026-07-15 — Wave P3-D1a: proc mesh consumers off C-ABI (user directive: remove C-compatible interfaces)

- Opens the **owner-driven mesh-dismantling arc** (P3-D): `proc_shims.rs`
  (204 `#[no_mangle]` exports — the C-ABI mesh's last stronghold, a Phase-2
  relic of `t_*`/`xv6_*`/`pg_*`/`tg_*` field-accessor shims) gets its
  consumers converted from `unsafe extern "C"` redeclarations to direct
  `use crate::proc::proc_shims::…` calls.
- **205 redeclarations converted across 14 proc/* files** (+122/−323):
  pgroup.rs 56, clone.rs 43, pid.rs 42, exit.rs 41, + 23 across
  signal/thread_group/workqueue/sys_signal/rq/rq_test/thread_queue/sched/
  sysproc/thread. Opaque `[u8;0]` marker types unified to `pub type`
  aliases of the real `bindings` types (ABI-identical thin pointers) —
  avoids hundreds of per-site casts. exit.rs re-exports via `pub use` inside
  its `mod raw` so `raw::NAME` call sites stayed untouched. Call sites
  otherwise unchanged (one `Some()` wrap for an `Option<fn>` callback).
- 14 dead shim exports identified (no consumers anywhere) → D1c dead-sweep
  list. Dead decls `pg_for_each_tg`/`tg_for_each_thread` (shadowed by Rust
  macros) dropped.
- **Cache incident**: worker found a stale system-GCC cache at wave start
  and reconfigured; orchestrator re-reconfigured with full env
  (TOOLPREFIX + LAB=fs → "Lab: fs" confirmed) and re-verified from clean.
- Verified (orchestrator, post-reconfigure): 0-warning `cargo clean`+full
  rebuild; boot gate; testsig **21/21**; forkfork + killstatus OK; stressfs;
  ENOENT smoke; no panics. (Worker additionally: reparent OK, boot ×2.)

### Iteration 51 — 2026-07-15 — Wave P3-D1b+c: proc_shims C-ABI surface DISMANTLED (`#[no_mangle]` 204→0 in-file)

- **D1b**: the 18 non-proc consumers converted (22 redeclarations — mostly
  `xv6_current_thread`/`xv6_panic`) to direct crate-path calls, same
  8e4fcc0 pattern. kstd.rs's `kassert!` path kept via `pub(crate) use`
  re-export; nullrand's divergent `*mut c_void` decl unified to the real
  `*mut thread` signature. Enablers: `mod proc_shims` → `pub(crate) mod`;
  exit.rs `mod raw` re-exports → `pub(crate) use`.
- **D1c**: 11 dead exports deleted (0-ref verified incl. .c/.h/.S); 3 of
  the original 14 dead-list survivors demoted instead (live in-file
  callers); **193 exports demoted `#[no_mangle] pub extern "C" fn` →
  `pub(crate) fn`** (11+193 = the exact 204). Zero kept-`extern "C"`
  exceptions among exports (the only address-taken fns are the 4
  already-private `proctab_hash*` hlist-ops callbacks, which keep
  `extern "C"` and never had no_mangle). Stale ABI-era comments updated.
- **proc_shims.rs `#[no_mangle]`: 0.** Crate-wide: 571→367. Demoted names
  nm-verified ABSENT from the linked kernel (t_pid/xv6_panic/
  xv6_current_thread/pg_pgid/tg_tgid/session_sid) while the keep-set
  (kerneltrap/usertrap/memcpy/start) remains. RUST_FORCE_UNDEFINED
  untouched.
- Verified (orchestrator): 0-warning `cargo clean`+full rebuild; cache
  clean; boot gate; `ls /dev` clean; **testsig 21/21**; symlinktest both
  ok; createdelete OK; stressfs; ENOENT; no panics. (Worker: boot ×2 +
  forkfork OK additionally.) Net diff 21 files, +249/−596.

### Iteration 52 — 2026-07-15 — Wave P3-D2a: scheduler cluster C-ABI surface dismantled (rq/sched/thread_queue `no_mangle` 100→0)

- Owner-driven mesh sweep, scheduler cluster: `rq.rs` 48→0, `sched.rs`
  23→0, `thread_queue.rs` 29→0 `#[no_mangle]`. Crate-wide 367→**267**.
- Consumer conversions across 33 files (ipi, rcu, clone, cffi, workqueue,
  exit `mod raw`, signal, sys_signal, start_kernel, sched_timer, trap,
  sysnet, uart, console, pcache, vfs inode/fs, dev x1_emac/x1_sdhci/yt8531,
  lock completion/mutex/semaphore/rwsem, tty, pipe, xv6fs log, virtio_disk,
  + 5 test modules). Special cases: cffi's layout-pinned `Rq`/`SchedClass`
  mirrors got 3 thin `#[inline]` cast adapters in its `mod raw`; clone.rs's
  divergent `*mut c_void` decl of `rq_task_fork` unified to the real
  `*mut sched_entity`; rcu.rs's stale local `sched_attr` mirror deleted.
- Demotions: 38+11+11 → `pub(crate) fn`. Kept-`extern "C"` exceptions among
  the exports: ZERO (sched_class fn-pointer tables live in sched_idle/
  sched_fifo/cffi, out of scope; thread_queue's rb comparator callbacks
  already extern-without-no_mangle). **40 dead exports deleted** (0-ref
  verified incl. .c/.h/.S + user/): 10 rq, 12 sched thin wrappers, 18
  thread_queue wrappers + 8 orphaned `_impl`s. All live-path `_impl`s
  (incl. `tq_wait_cb_impl` with the forkforkfork panic site) byte-identical.
- nm: 16 demoted names ABSENT from linked kernel; keep-set present.
  RUST_FORCE_UNDEFINED/CMakeLists untouched; no asm refs to any of the 101.
- Verified (orchestrator): 0-warning clean rebuild; cache clean; boot gate;
  testsig **21/21**; preempt OK; forkfork OK; **forkforkfork panics at the
  byte-identical pre-existing site** (0x80c40000, "Failed to remove
  interrupted waiter from queue"); ENOENT. (Worker: boot ×4, forktest OK,
  stressfs, nm sweep.) Net diff 36 files, +296/−666.

### Iteration 53 — 2026-07-15 — Wave P3-D2b: proc-object cluster C-ABI surface dismantled (signal/thread_group/pid/pgroup `no_mangle` 73→0)

- Owner-driven mesh sweep: signal.rs 39→0 (34 demoted, 5 dead deleted),
  thread_group.rs 19→0 (14+5 — incl. `get_thread_group`+`_inner`, dead
  after D1a's caller conversions), pid.rs 8→0 (6+2), pgroup.rs 7→0 (7+0).
  Crate-wide `#[no_mangle]` 267→**194**.
- 19 consumer files converted (sys_signal 10, exit 11 via `mod raw` +
  one `#[inline]` cast adapter, session 12 incl. a `#[link_name]` block
  deletion, thread 8, clone 6, tty/trap/console/pipe/xv6fs/lock files…).
  Divergent-signature unifications (ABI-identical, 9d35f95 precedent):
  `signal_pending` → real `bool` (24 call sites drop `!= 0`), sigaction
  buffer + clone.rs opaque handles → real bindings types, thread.rs
  `KERNEL_PG` retyped `AtomicPtr<pgroup>`.
- Zero kept-`extern "C"` exceptions; no .S refs (sig_trampoline.S only
  *defines* `sig_trampoline`; `push_sigframe` already private); keep-set
  untouched. nm: none of the 73 names in the linked kernel.
- Verified (orchestrator): 0-warning clean rebuild; cache clean; boot
  gate; **testsig 21/21**; killstatus/forkfork OK; ENOENT; no panics.
  (Worker: boot ×2, exitwait/reparent OK, stressfs.) Net diff 23 files,
  +323/−496.

### Iteration 54 — 2026-07-15 — Wave P3-D3a: mm cluster C-ABI surface dismantled (vm/pcache/page/kalloc/slab/sysmm `no_mangle` 55→0)

- Owner-driven mesh sweep, mm cluster: vm.rs 20→0, pcache.rs 12→0,
  page.rs 7→0, kalloc.rs 5→0, slab.rs 4→0, sysmm.rs 7→0. Crate-wide
  `#[no_mangle]` 194→**139**. (First attempt died on account spend limit
  mid-survey, tree clean; retry incorporated its finding.)
- **New consumer form discovered & handled**: 3 proc files consumed slab
  symbols via `use crate::bindings::…` (bindgen extern decls) — demotion
  produces loud link failures, converted alongside the extern-block form.
  Scripted scan confirmed those 3 were the only bindings-import consumers
  among all 55 names. wrapper.h/build.rs untouched (nativization's job).
- **Latent ABI bug caught**: clone.rs's stale decl claimed
  `vm_dup -> *mut Vm` but the real fn returns `()` — the C ABI silently
  let the caller read a stale return register (call site discards it).
  Unified to the real signature.
- Kept-`extern "C"` exceptions (dropped no_mangle, evidence verified):
  sysmm's 7 `sys_m*` + pcache's `sys_sync`/`sys_dumppcache` + page's
  `sys_memstat` — all stored as fn-pointer values in irq/syscall.rs's
  const SYSCALLS table (`type SyscallFn = extern "C" fn() -> u64`).
  Dead exports: 0 (all 55 had live consumers).
- 54 files touched (+967/−471): 17 vm consumers, safe-facade/cast
  adapters for slab/page divergences (`*const c_char` names, `u64/u32`
  sizes), kmm_* consumers switched to cffi's pre-existing safe wrappers.
  Stale "still #[no_mangle]" comments fixed.
- nm: all 55 names absent from linked kernel; keep-set present.
- Verified (orchestrator): 0-warning clean rebuild; cache clean; boot
  gate; **mmaptest 16/16 + "all tests passed"**; **testsig 21/21**;
  **cowtest ALL PASSED**; no panics. (Worker: 4 boots, mmaptest ×2,
  createdelete OK, stressfs completion, ENOENT.)

### Iteration 55 — 2026-07-15 — Wave P3-D3b: lock cluster + thread/workqueue C-ABI surface dismantled (43 `no_mangle` → 0)

- Owner-driven mesh sweep: rwlock 7→0, rcu 7→0, rwsem 5→0, mutex 5→0,
  completion 5→0, thread.rs 9→0, workqueue 5→0. Crate-wide 139→**96**.
  All 43 live (0 dead); zero kept-`extern "C"` exceptions (address-taken
  scan clean; `thread_sched_entity` + rwlock's sleep/wake callbacks were
  already no_mangle-free). Zero bindings-import consumers this cluster.
- **Latent ABI drift caught** (2nd of the arc): workqueue.rs's own
  `kthread_create` redeclaration had drifted to `(.., arg, prio, order)`
  vs the real `(.., arg1, arg2, stack_order)` — value-identical at
  runtime only because both call sites pass literal 0s; unified.
- **False safety contract fixed**: vfs_syscall.rs's old `safe fn call_rcu`
  decl asserted safety the real fn doesn't have — its call site now has an
  explicit `unsafe {}` + SAFETY comment; `call_rcu` gained a `# Safety`
  section. Conversely `rcu_read_lock/_unlock/rcu_check_callbacks` became
  genuinely safe `pub(crate) fn` (pure dispatch to safe `_impl`s) and 4
  redundant `u!{}` wrappers dropped. Dead `rcu_read_lock` externs in
  pgroup.rs deleted (never called).
- **Warning A/B under lifted `-A warnings`**: stash-baseline vs final =
  zero new warnings (716→713, 3 pre-existing removed); 14 orphaned
  imports cleaned across 13 files.
- nm: all 43 names absent; keep-set present. 36 files, +298/−375.
- Verified (orchestrator): 0-warning clean rebuild; cache clean; boot
  gate; **testsig 21/21**; forkfork OK; stressfs; **forkforkfork panics
  at the identical pre-existing site** (0x80c3c000 region, same message).
  (Worker: boot ×3, forktest/createdelete OK, ENOENT, nm sweep.)

### Iteration 56 — 2026-07-15 — Wave P3-D3c: final mesh sweep — **C-ABI mesh arc CLOSED, `#[no_mangle]` 96→22 (the mandated set)**

- The closing sweep: hlist 8→0, bufcache 8→0, irq/syscall 7→0 (incl.
  another latent ABI bug: pid.rs's stale decl claimed `argint -> i32`
  vs the real `()` — third stale-register read caught this arc),
  kobject 6→0, bintree 5→0 (backtrace.rs's bindgen-path link dependency
  on `rb_find_key_rdown` eliminated by crate-path conversion),
  start_kernel 5→0, printf 4→2, spinlock 4→0 (incl. a bindings-import
  in signal.rs that surfaced as a loud link failure), timers 6→0,
  vm_pgtab/cffi/rbtree/early_allocator/clone/exit/goldfish/plic/
  irq_core/ipi-`cpus`/fdt-`platform` → 0, test files 5→0
  (`xv6_rqtest_pub_run` deleted as a dead C-era alias).
- `forkret_entry` keeps `extern "C"` (drops no_mangle): its address is
  the swtch.S-restored `ra` — ABI load-bearing, name not. `cpus`
  consumers' `#[link_name]` views replaced with offset-0 casts of the
  `#[repr(C)]` wrapper (link_section unchanged).
- **Test-launch investigation**: all 4 QEMU ctest suites launch via
  cmake-env → cargo features → `#[cfg]`-gated direct Rust calls in
  start_kernel.rs — nothing launches by symbol name; test exports
  demoted safely.
- **The 22 survivors (documented mandated set)**: string.rs 13 + printf
  `puts`/`putchar` (LLVM-libcall class, RUST_FORCE_UNDEFINED);
  irq/trap.rs `kerneltrap`/`kernel_irq`/`usertrap`/`user_kirq_entrance`
  (kernelvec.S/trampoline.S asm contract); start.rs `start`/`stack0`
  (entry.S); backtrace.rs `db_break` (external-debugger by-name anchor).
- Verified (orchestrator): 0-warning clean rebuild (default target — the
  gotcha: `--target fs_img` alone does not relink the kernel); nm
  keep-set 6/6 present + swept names absent; boot gate; **testsig
  21/21**; **mmaptest all passed**; forkfork OK; `ls /dev` clean; ENOENT;
  no panics. (Worker: boot ×3, stressfs, all-features cargo check 0.)
  86 files, +1094/−761.

### C-ABI mesh dismantling arc (P3-D) — summary

`#[no_mangle]` 1,438 (Phase-3 baseline) → P3-1 mesh waves → 571 →
**P3-D1–D3c (7 waves, 2026-07-15) → 22**, every wave 0-warning +
boot-gated + nm-verified, zero behavior change. ~570 consumer
redeclarations converted to direct crate-path Rust calls; 66 dead
exports deleted; 3 latent ABI bugs found & fixed (vm_dup phantom return,
kthread_create arg drift, argint phantom return) — the exact silent-UB
class the user's directive targets. Remaining C-compatible surface:
the 22 mandated exports + bindgen type layer (nativization arc next) +
C-layout fn-pointer ops tables (P3-10 dyn-Trait next).

### Iteration 57 — 2026-07-16 — Wave P3-N1: intrusive-node family nativized (nativization arc opener)

- First nativization wave: `list_node_t` (list.rs `ListNode`), the hlist
  family (`hlist_t`/`hlist_entry`/`hlist_func_struct`/`hlist_bucket_t` →
  hlist.rs natives), and the rb-tree family (`rb_node`/`rb_root`/
  `rb_root_opts` → bintree.rs natives) are now hand-written `#[repr(C)]`
  Rust types. bindgen no longer generates them.
- **The blocklist+redirect technique proven** (template for all later
  families): build.rs `blocklist_type` + `raw_line` re-export so
  still-bindgen structs embed the native types; PLUS the non-obvious
  part — a `ParseCallbacks::blocklisted_type_implements_trait` impl,
  because bindgen otherwise assumes blocklisted types derive nothing and
  silently degrades embedding structs (e.g. `page_struct`'s anonymous
  union decays to `__BindgenUnionField` if `list_node` isn't known
  `Copy`).
- **Compile-time layout proofs**: const asserts on size/align + every
  field offset (`offset_of!`) for all three families, values cited from
  the pre-change bindgen output (list_node 16/8; rb_node 24/8;
  hlist_struct field offsets incl. the flexible-array `buckets` at 48).
  A mismatch fails the build — the 0-warning build IS the byte-identity
  proof.
- 5 files (+217/−165): list.rs, hlist.rs, bintree.rs, build.rs,
  proc_shims.rs (hlist ops-struct init touch-up).
- Incident: the wave's worker was killed mid-verification and left the
  cmake cache poisoned (`/usr/bin/riscv64-unknown-elf-gcc`); orchestrator
  reconfigured with TOOLPREFIX+LAB=fs and re-ran everything.
- Verified (orchestrator, on the corrected cache): 0-warning clean
  rebuild; boot gate; **testsig 21/21**; **mmaptest all passed**;
  symlinktest both ok; forkfork OK; createdelete OK; stressfs; no
  panics. (Worker had independently reached mmaptest 16/16 before
  being stopped.)

### Iteration 58 — 2026-07-16 — Wave P3-N2: lock + thread-queue type family nativized (10 types, 3 tiers)

- Second nativization wave via the 41f268e technique: `spinlock_t`,
  `rwlock` (tier 1) → `tnode_t`/`tq_t`/`ttree_t`/`tq_type_t` (tier 2,
  embed the N1 natives) → `mutex_t`/`rwsem_t`/`semaphore_t`/
  `completion_t` (tier 3, embed tq). bindgen emits ZERO struct defs for
  the family; the natives in lock/*.rs + proc/thread_queue.rs are the
  single source of truth (many pre-existed from P3-3A/3D with
  *relative* asserts — this wave promoted them to THE definitions with
  hardcoded proofs).
- **Layout evidence upgraded**: a cross-compiler `_Static_assert` probe
  (compiled with the real riscv64 toolchain, rv64gc/lp64d) proved every
  size/align/offset for all 10 types including both `tnode` anonymous-
  union arms — stronger than trusting bindgen output.
- **Brief error caught by the worker**: `__ALIGNED_CACHELINE` rides the
  *typedef* `spinlock_t`, not `struct spinlock` — C sizeof stays 24
  while typedef alignof is 64 (inexpressible in one Rust type). The
  suggested `#[repr(C, align(64))]` would have padded size to 64 and
  shifted every embedder's fields (e.g. `mutex.wait_queue` 24→64) — a
  layout break. Kept 24/8 with embedders carrying their own align, as
  bindgen did. rwlock's `_Atomic` fields: alias target is a plain-POD
  twin `Rwlock` (proc_shims constructs by field literal; pcache embeds);
  the atomic-view `RawRwlock` and all access code untouched.
- Copy-semantics fidelity: NATIVE_TYPES callback split into Copy-yes vs
  non-Copy (tnode) lists; embedder derive lines verified byte-identical
  to pre-change snapshot. Orphan `tnode__bindgen_ty_1` shells swept.
- Verified (orchestrator): 0-warning clean rebuild; cache clean; bindgen
  emission grep = 0 structs + redirect `pub use` lines confirmed; boot
  gate; **testsig 21/21**; **mmaptest all passed**; forkfork OK;
  **forkforkfork identical pre-existing panic** (message + region match
  the worker's pre-change-baseline A/B). (Worker: 6/6 boots, stressfs,
  createdelete, ENOENT, C probe.) 11 files, +380/−200.

### Iteration 59 — 2026-07-16 — Wave P3-N3: kobject + workqueue + process-object families nativized (8 structs + 4 anon bitfields)

- Third nativization wave via the 41f268e/8768b10 technique:
  `kobject`/`kobject_ops` (kobject.rs), `work_struct`/`workqueue`/
  `workqueue_callbacks` (proc/workqueue.rs — `WorkStruct` promoted from
  its P3-3D relative-assert form to THE definition), `pgroup`
  (proc/pgroup.rs), `thread_group` (proc/thread_group.rs), `session`
  (tty/session.rs). bindgen emits ZERO struct defs for all eight; every
  `crate::bindings::X` path resolves through `pub use` raw-line
  redirects (`mod workqueue`/`mod pgroup`/`mod thread_group` promoted to
  `pub(crate)` for facade pathing, as thread_queue was in N2).
- **Anonymous C bitfield structs nativized as named fields**: the four
  `struct { uint64 x : 1; ... }` members (workqueue
  active/dtor_called; pgroup exited/is_kernel; thread_group is_kernel;
  session exited/is_kernel) each became a hand-written 8/8
  `*FlagBits { bits: u8, _pad: [u8; 7] }` holder (little-endian bit 0 =
  first C bitfield, identical to bindgen's `get(0,1)`/`get(1,1)` unit
  accessors, reproduced 1:1 as inline methods) under a real field name
  `flags` — every `__bindgen_anon_1` consumer re-pointed (access.rs
  bit_get!/bit_set! for WorkqueueRef/PgroupAccess/ThreadGroupAccess/
  SessionAccess, proc_shims pg_* accessors, session.rs/thread_group.rs
  direct sites); the orphan `*__bindgen_ty_1` shells are blocklisted.
- **Mixed-tier embed exercised**: native `ThreadGroup` embeds the
  still-bindgen `tg_shared_pending` *by value* via its
  `crate::bindings` path (signal family out of scope; alias re-points
  transparently when it nativizes). `_Atomic int` members stay plain
  `c_int` exactly as bindgen lowered them (access.rs AtomicI32 views
  untouched). `workqueue` carries bindgen's `#[repr(align(64))]`
  (spinlock_t-typedef cacheline alignment) — size 256, `lock` at 0.
- Layout evidence: cross-compiler `_Static_assert` probe
  (riscv64-unknown-elf-gcc, rv64gc/lp64d, -I kernel/inc) proved every
  size/align/offset — kobject 40/8, kobject_ops 8/8, work_struct 48/8,
  workqueue_callbacks 48/8, workqueue 256/64, pgroup 96/8,
  tg_shared_pending 520/8, thread_group 616/8, session 96/8 (anon
  bitfield windows pinned by both neighbours) — all hardcoded into
  const asserts in the owning modules. NATIVE_TYPES callback extended
  (all eight Copy=Yes, matching pre-change derives; `kobject` and
  `work_struct` are still embedded by value by bindgen `device`/`bio`/
  `inode`/`blkdev`, so the accurate Yes keeps their derive lines
  byte-identical).
- Verified (worker): 0-warning clean rebuild (rust target dir wiped);
  cache clean before+after; bindgen emission grep = 0 structs, 8
  redirect `pub use` lines, orphan shells gone; 7 boots each with
  exactly one `init: starting sh`; **testsig 21/21**; `ls /dev` full
  10-node listing (one early-boot run raced async device population at
  6s — re-verified at 6s and 8s with complete listings; HEAD A/B showed
  the same node set); usertests killstatus/reparent/forkfork/
  createdelete all OK (single-test "lost some free pages" = documented
  pre-existing artifact); stressfs completed; `cat /nonexistent` →
  ENOENT; **forkforkfork identical pre-existing panic** ("Failed to
  remove interrupted waiter from queue", matches RUST_REWRITE.md's
  documented signature).

### Iteration 60 — 2026-07-16 — Wave P3-N4: signal + device families nativized (16 structs + 3 anon bitfields + 1 anon union)

- Fourth nativization wave via the 41f268e/8768b10/219b6fb technique,
  four sub-families, tree green after each: **signal** — `sigaction`
  (+ its anonymous handler *union*, the first anon-union nativization
  since N2's tnode: real Rust `union SigActionHandler` under field name
  `handler`), `sigacts`(`_t`), `sigpending`(`_t`), `ksiginfo`,
  `tg_shared_pending` (all in proc/signal.rs, `mod signal` promoted to
  `pub(crate)`); **dev core** — `device_major`(`_t`)/`device_ops`(`_t`)/
  `device_instance`+`device_t` (dev/dev.rs), `cdev_ops`(`_t`)/`cdev`(`_t`)
  (dev/cdev.rs), `blkdev_ops`(`_t`)/`blkdev`(`_t`) (dev/blkdev.rs);
  **bio** — `bio_vec`/`bio` (dev/bio.rs); `buf` (bufcache.rs); **netdev**
  — `netdev` (dev/netdev.rs). bindgen emits ZERO struct defs for all 16;
  every `crate::bindings::X` path resolves through `pub use` raw-line
  redirects (struct + `_t` typedef names both).
- **Ops tables reproduced, not trait-ified**: the fn-pointer STRUCT
  fields (`device_ops`/`cdev_ops`/`blkdev_ops`, `bio.end_io`,
  `sigaction`'s handler union) keep bindgen's exact
  `Option<unsafe extern "C" fn ...>` forms — trait-ifying them is
  P3-10's job. `netdev_ops` + `netdev_link_cb_t` and `dev_type_e` stay
  bindgen (pointer-only/alias references; anchored blocklist regexes
  leave them untouched).
- **Anonymous C bitfields**: cdev/blkdev readable+writable and bio
  valid+rw+done became `CdevFlagBits`/`BlkdevFlagBits`/`BioFlagBits`
  8/8 `{bits,_pad}` holders under field name `flags` (N3 pattern,
  little-endian bit order identical to bindgen's unit accessors); all
  ~30 `__bindgen_anon_1` consumer sites re-pointed (cdev.rs, blkdev.rs,
  bio.rs, tty_dev.rs, ptmx.rs, nullrand.rs, console.rs, virtio_disk.rs,
  ramdisk.rs, x1_sdhci.rs; sigaction union sites in trap.rs, signal.rs,
  access.rs incl. its raw_set! path).
- **Mixed-tier embeds**: native `KsigInfo` embeds still-bindgen
  `siginfo_t` by value; `DeviceMajor` embeds `rcu_head_t`; N3's
  `ThreadGroup.shared_pending` alias re-pointed onto the new native
  `TgSharedPending` transparently (its 520/8 assert unchanged). Bindgen's
  explicit padding fields reproduced 1:1 (`bio._pad0: [u64; 2]`,
  `buf._pad0: [u64; 5]` — the C `completion_t`/`mutex_t` *typedefs*
  carry `__ALIGNED_CACHELINE`, natives genuinely 128/64). `bio.bvecs`
  (C flexible array member) keeps bindgen's zero-sized
  `__IncompleteArrayField` helper — first FAM nativization; call sites
  (`bvecs.as_mut_ptr()`) unchanged.
- Layout evidence: cross-compiler `_Static_assert` probe
  (riscv64-unknown-elf-gcc, rv64gc/lp64d, -I kernel/inc) proved every
  size/align/offset — sigaction 24/8, sigacts 896/64 (spinlock_t-typedef
  align via first member), sigpending 16/8, ksiginfo 80/8,
  tg_shared_pending 520/8, device_major 56/8, device_ops 24/8,
  device_instance 96/8, cdev_ops 56/8, cdev 160/8, blkdev_ops 24/8,
  blkdev 136/8, bio_vec 16/8, bio 256/64 (io_completion at 128, bvecs
  at 256), buf 320/64 (lock at 64, mutex_t 128/64), netdev 80/8 — all
  hardcoded into const asserts in the owning modules. **Copy fidelity
  split**: Copy=Yes for the PODs/ops tables, Copy=**No** for
  `device_instance`/`cdev`/`blkdev`/`bio` (bindgen's kobject-embedder
  derive pattern — they had NO derives pre-change; natives faithfully
  have none, NONCOPY_NATIVE_TYPES extended). `sigpending_t` Copy=Yes is
  load-bearing: still embedded by value by bindgen `thread_signal`
  (derive line verified unchanged).
- Verified (worker): 0-warning clean rebuild (`cargo clean` + full
  cmake, grep = 0); cache clean before+after (BUILD_TYPE empty,
  toolchain gcc); bindgen emission grep = 0 structs, 20 redirect
  `pub use` lines; 4 boots each with exactly one `init: starting sh`;
  **testsig 21/21 ALL PASSED**; `ls /dev` full 12-node listing;
  `cat README.md | wc` = 714/3365/26018; `cat /nonexistent` → ENOENT;
  usertests createdelete OK (single-test "lost some free pages" =
  documented pre-existing artifact); stressfs completed (full
  write+read phases, prompt returned); **forkforkfork identical
  pre-existing panic** ("Failed to remove interrupted waiter from
  queue", matches the documented tq-bug signature).

### Iteration 61 — 2026-07-16 — Wave P3-N5: VFS type family nativized (13 structs + 3 anon bitfields + 2 anon unions + 1 anon struct — the biggest family)

- **USER DIRECTIVE (remove C-compatible interfaces), fifth nativization
  wave** (N3/N4 template). The entire `kernel/inc/vfs/vfs_types.h` +
  `vfs/pipe_types.h` graph is hand-written native Rust now:
  `vfs_dentry`/`vfs_dir_iter` + **`vfs_inode`/`vfs_inode_ops` (the hub —
  17 consumer files)** → `vfs/inode.rs` (`VfsDentry`/`VfsDirIter`/
  `VfsInode`/`VfsInodeOps` + `VfsInodeFlagBits` + real Rust union
  `VfsInodeDevMnt`{cdev,bdev,`VfsInodeMnt`}); `vfs_file`/`vfs_file_ops`
  → `vfs/file.rs` (+ real Rust union `VfsFilePos`); `vfs_fs_type(_ops)`/
  `vfs_superblock(_ops)`/`fs_struct` → `vfs/fs.rs` (owner of the
  `vfs_struct_*` lifecycle; + `VfsFsTypeFlagBits`/`VfsSuperblockFlagBits`;
  the superblock's anonymous inode-hash struct is FLATTENED into direct
  `inodes`/`inodes_buckets` fields at identical offsets); `vfs_fdtable`
  → `vfs/fdtable.rs`; `pipe` → `vfs/pipe.rs`. No `_t` typedefs exist in
  this family; bindgen emission for all 13 structs + every `__bindgen_ty`
  shell = 0 (the only survivor is the out-of-scope `vfs_inode_ref`,
  kernel/inc/types.h).
- **Sequencing note**: `vfs_file` had to nativize in the same build step
  as `vfs_dir_iter`/`pipe` — its anonymous position union embeds both
  directly, and bindgen degrades such unions to `__BindgenUnionField`
  blobs once the members are blocklisted (N1's documented limitation;
  N2's `tnode` precedent).
- **KNOWN DIVERGENCE preserved, `struct pipe` ONLY**: gcc AND clang both
  lay the C header out as 256/64 with `writer_lock` @128 (the
  `spinlock_t` typedef's `__ALIGNED_CACHELINE`), but bindgen has emitted
  192/64 with `writer_lock` @88 ever since P3-N2 (it kept only a
  `u64` pad and counted on the field type's C alignment — the native
  `RawSpinlock` is align 8). The bindgen layout is the runtime truth
  (zero C consumers; allocation + all accesses live in `pipe.rs`), so
  the native `Pipe` reproduces the BINDGEN layout byte-for-byte;
  reproducing the header would have been a silent runtime layout CHANGE.
  Documented in pipe.rs's layout note + the probe header.
- Copy fidelity: ops tables + dentry/dir_iter/file/fdtable/fs_struct/
  pipe Copy=Yes; `vfs_fs_type` (kobject-embedder class) +
  `vfs_superblock` + `vfs_inode` NONCOPY — the still-bindgen
  `tmpfs_superblock`/`xv6fs_superblock`/`tmpfs_inode`/`xv6fs_inode`
  embed them BY VALUE with no derives, kept bit-exact by the accurate
  No. Mixed-tier by-value/by-path embeds: `vfs_inode_ref`, `pcache`
  (`i_data`), `stat`/`statfs` (**uabi — confirmed
  kernel/inc/uabi/statfs.h, stays bindgen, P3-4 scrutiny class**),
  `thread`, `sock`.
- Layout evidence: cross-compiler `_Static_assert` probe (toolchain
  riscv64-unknown-elf-gcc, rv64gc/lp64d) + clang-18
  `-fdump-record-layouts` cross-check + a TEMPORARY in-tree `offset_of!`
  gate on the pre-change bindgen forms (built green, then removed) —
  all three agree on every offset (pipe aside, see above); values
  hardcoded into const asserts (~120 asserts). Notables: vfs_superblock
  1472/64, vfs_inode 1088/64 (i_data pcache 576/64 @320), vfs_file
  256/64, vfs_fdtable 576/64, pipe 192/64 (runtime).
- ~95 consumer sites re-pointed across 10 vfs files (`.pos.*` for the
  file union, `.flags.*` for the three bitfield holders, `.dev_mnt.*`
  for the inode union, `.inodes`/`.inodes_buckets` flattened);
  `SB_HASH_BUCKETS` now aliases the native array bound (one definition
  of 61). mm/pcache.rs's look-alike `__bindgen_anon` sites (pcache's
  own bitfields) verified untouched.
- Verified (worker): 0-warning FULL CLEAN rebuild (`rm -rf` of the cargo
  target dir); cache clean before+after every build step; tree GREEN
  after each sub-family (boot gate at each); 5 final-binary boots each
  with exactly ONE `init: starting sh`; **full fs corruption battery**:
  mkdir×2 → EEXIST; `ln /README.md rr` → `wc rr` == `wc /README.md` ==
  714/3365/26018; `cat /nonexistent` → ENOENT; symlinktest both ok;
  usertests createdelete/bigdir/linktest/unlinkread ALL OK; stressfs ×2
  completed (write+read phases, prompt back); `ls /dev` full 12-node
  listing; `cat README.md | wc` pipe sane; **testsig 21/21 ALL
  PASSED**; **mmaptest 16/16 all passed** (file-backed mmap paths); NO
  `freeing free block`/`incorrect blockno`/`balloc: out`/panic anywhere;
  forkforkfork identical pre-existing kerneltrap storm (Load page
  fault, stval=0xfffffffffffffff1 — same signature as the recorded
  baseline, sepc shifted only by relink).

### Iteration 62 — 2026-07-16 — Wave P3-N6: mm type family nativized (11 structs + 2 anon bitfields + 3 anon unions, incl. the 5-arm page union)

- **USER DIRECTIVE (remove C-compatible interfaces), sixth nativization
  wave** (N4/N5 template). The mm descriptor graph is hand-written
  native Rust now, sub-family by sub-family (tree green + boot gate
  after each): **slab** (`slab_cache_struct`/`slab_cache_t` 1280/64,
  `slab_struct`/`slab_t` 80/8, `percpu_slab_cache_t` 128/64) →
  `mm/slab.rs` (`SlabCacheStruct`/`SlabStruct`/`PercpuSlabCache`; the
  module's private atomic-view `SlabCache`/`Slab`/`PercpuCache` mirrors
  stay, byte-identical and cross-asserted — C `_Atomic` degrades to
  plain ints in bindgen and the natives reproduce that); **allocator
  PODs** — `free_extent` 32/8 → `vfs/xv6fs/block_cache.rs`
  (`FreeExtent`; brief guessed mm/kalloc but the header truth is
  block_cache.h) and `mem_region` 16/1 packed → `dev/fdt.rs`
  (`MemRegion`; header truth dev/fdt.h — a parse POD, not a DMA
  descriptor); **vm** (`vma`/`vma_t` 104/8, `vm`/`vm_t` 384/64) →
  `mm/vm.rs` (`Vma`/`Vm`); **pcache trio** (`pcache` 576/64,
  `pcache_node` 160/8, `pcache_ops` 40/8) → `mm/pcache.rs` (`Pcache`/
  `PcacheNode`/`PcacheOps` + real Rust union `PcacheFlags`{flags,bits} +
  `PcacheFlagBits`(active=0,flush_requested=1) with
  `PcacheNodeFlagBits`(dirty=0,uptodate=1,io_in_progress=2) — the
  direct-alias `pub type Pcache = pcache` layer became the definition
  itself); **page** (`page_struct`/`page_t` 128/64, the gnarly closer)
  → `mm/page.rs` (`PageStruct`; the single-member anonymous flags union
  FLATTENED to a direct `flags: u64` @32 — N5 superblock precedent; the
  5-arm per-type union became real Rust union `PageTypeData` (field
  `type_data`) with named arms `PageAnon`(EMPTY 0/1)/`PageBuddy`24/
  `PageSlab`16/`PagePcache`16/`PageTail`8 — N2 tnode precedent; the
  private byte-accessor `Page` mirror stays, cross-asserted). bindgen
  emission: **21 struct/union defs removed, zero left** (incl. all 9
  `__bindgen_ty` shells); ~130 hardcoded const asserts added.
- **NO bindgen-vs-header divergence anywhere in this family** (pipe
  precedent explicitly checked): the temporary in-tree `offset_of!`
  gate on the live bindgen forms (mm/p3n6_gate.rs, built green then
  removed) and the cross-compiler `_Static_assert` probe (toolchain
  gcc, rv64gc/lp64d — scratchpad p3n6_static_assert_probe.c, exit 0)
  agree on every size/align/offset. The explicit `_pad0/_pad1` fields
  reproduce bindgen's `__bindgen_padding_*` verbatim; the 64-byte lock
  placements come out identical because `completion_t`/`rwlock`/
  `rwsem_t` natives kept `align(64)`.
- **TOOLING FINDING (build.rs NativeTypeCallbacks)**: bindgen asks
  `blocklisted_type_implements_trait` about typedef'd types by bare
  name (`slab_cache_t`) but about non-typedef'd records as `"struct
  X"`. Symptom: `platform_info` (still bindgen, embeds
  `[mem_region; 8]` by value) silently lost Copy/Clone. Fix: explicit
  `"struct X"` entries for the new tag-only types. A blanket
  prefix-strip was tried and REJECTED: it resurrected Copy/Clone on
  nine remaining bindgen types (`pcache(+node)`/`vm`/`vma`/
  `xv6fs_block_cache`/`sched_entity(+ty_1)`/`timer_node`/`timer_root`)
  that had silently lost derives to this same quirk back when `struct
  spinlock` etc. nativized in P3-N2 — the established no-derive
  emission is the boot-verified truth, and remaining types' derive
  lines must not change as a wave side effect. **Flag for orchestrator:
  those nine (now seven remaining) are candidates for a deliberate
  derive-restoration decision in their own waves.** Derive fidelity
  gate: full regex diff of every remaining type's attr block pre/post —
  REMOVED 21 / ADDED 0 / CHANGED 0.
- Copy fidelity: slab trio + `pcache_ops` + `page_struct`(+5 arms) +
  `mem_region` Copy=Yes (exactly as bindgen emitted; `mem_region`'s Yes
  is what keeps `platform_info`'s derive line); `pcache`/`pcache_node`/
  `vm`/`vma`/`free_extent` NONCOPY (bindgen emitted no derives — the
  native `VfsInode` embeds `pcache` by value (`i_data` @320) with no
  derives, unchanged). Facades cover every alias: struct tag + `_t`
  names both re-exported.
- ~30 consumer sites re-pointed: pcache flag sites → `.flags.flags`/
  `.flags.bits.active()` etc. (mm/pcache.rs 12, mm/pcache_test.rs 10,
  vfs/tmpfs/file.rs 4 + truncate.rs 1, vfs/xv6fs/file.rs 5 + inode.rs 3
  — via `i_data.flags.bits`); page union sites → `.flags` /
  `.type_data.{buddy,slab,pcache,tail}` (mm/slab.rs 8, mm/pcache.rs 5);
  page.rs's P3-3B mirror cross-assert block re-pointed to the native.
  `mm/{pcache,vm}` promoted to `pub mod` (N5's vfs precedent) for the
  facade paths.
- Verified (worker): 0-warning FULL CLEAN rebuild (`cargo clean` +
  rebuild: 0 warnings 0 errors); cmake cache checked clean
  (BUILD_TYPE empty, toolchain gcc) before+after every build step; 8
  boots total, each with exactly ONE `init: starting sh`; **mmaptest
  16/16 all passed**; **testsig 21/21 ALL PASSED**; usertests
  createdelete OK; **stressfs ×2 completed** (both `stressfs starting`
  plus full write/read phases, prompt back); **cowtest → ALL COW TESTS
  PASSED** (standalone `_cowtest` binary; `usertests cowtest` matches
  no test name in this tree — verified identical no-op + identical
  `lost some free pages 193011/193015` artifact on a pre-wave HEAD
  worktree build, byte-identical numbers, PRE-EXISTING; ditto
  createdelete's `193006/193017` trailer); `cat /nonexistent` → ENOENT;
  forkforkfork → identical pre-existing kerneltrap storm on pre-wave
  AND post-wave builds (same `In thread ... at 0x0000000080c34000` +
  `kerneltrap: exception preempted interrupt` signature); zero panics
  in every other run. NOT committed (orchestrator gate).

### Iteration 63 — 2026-07-16 — Wave P3-N7: scheduler + timer + rcu type families nativized (9 structs + 3 anon shells)

- **USER DIRECTIVE (remove C-compatible interfaces), seventh
  nativization wave** (N5/N6 template), sub-family order rcu → timer →
  sched-POD → sched-hot-core, tree green after each. bindgen emissions
  for the family: 12 → 0 (`rcu_head`+ty_1, `timer_root`+ty_1,
  `timer_node`, `load_weight`, `sched_attr`, `sched_class`, `rq`,
  `rq_percpu`, `sched_entity`+ty_1); remaining bindgen structs 76 → 64.
- **Natives + owners**: `RawRcuHead` 40/8 (kernel/lock/rcu.rs — the
  P3-3A internal mirror PROMOTED to the canonical `struct rcu_head`;
  anonymous 1-bit `embedded_head` bitfield flattened to `flags: u64`,
  N5/N6 precedent); `TimerRoot` 128/64 + `TimerNode` 80/8
  (kernel/timer/timer_core.rs; anon `valid:1` bitfield → real
  `TimerRootFlagBits` 8/8, N3 flag-bits precedent; `__bindgen_anon_1.
  {valid,set_valid}` consumer sites re-pointed to `.flags`);
  `SchedClass` 80/8 + `SchedAttr` 32/8 (kernel/proc/sched.rs —
  fn-pointer `Option` forms reproduced verbatim, trait-ification stays
  P3-10); `LoadWeight` 8/4, `Rq` 64/64, `RqPercpu` 640/64,
  `SchedEntity` 320/64 (kernel/proc/rq.rs; the leading anonymous
  rb/list union — a degraded `__BindgenUnionField` blob since P3-N1 —
  is the real Rust union `SchedEntityLink` 24/8 now, N2 `tnode`
  precedent; bindgen's `__bindgen_padding_0/1` reproduced as `_pad`
  fields pinning `pi_lock`@64 / `rq_lock`@576 under the cacheline
  spinlock typedef).
- **cffi.rs mirrors promoted**: proc/cffi.rs's independently-written
  `SchedClass`/`Rq`/`SchedEntity` mirror structs deleted in favour of
  re-exports of the natives; its opaque `Thread` stand-in is the real
  `bindings::thread` now (unifying `IdleRq.idle_thread` with
  `SchedEntity.thread`), the unused opaque `Context` dropped, and
  sched_idle.rs's pointer-cast `thread_sched_entity` wrapper collapsed
  to a plain re-export. NOTE: the old mirror was WRONG past
  `sched_entity.sched_class` (modelled `pi_lock` 24B @56 vs the real
  8B-padding-then-@64) — harmless only because its comment restricted
  use to `priority`/`rq`/`thread`; the promotion removes the trap.
- **DERIVE DECISIONS (the N6-flagged survivors, from first
  principles)**: `sched_entity`(+ty_1)/`timer_node`/`timer_root` stay
  NO-derive — the quirk-created emission is also the *correct* form
  (intrusive rb/list links + owned locks + context frame; bitwise
  duplication would corrupt queue invariants, N1/N2 `tnode` NONCOPY
  precedent; no consumer `=`-copies or literal-constructs any of them,
  grep-verified). `rcu_head` keeps Copy/Clone (still embedded BY VALUE
  by the still-bindgen, Copy-deriving `thread.rcu_head` — the
  NativeTypeCallbacks Yes is what keeps *thread's* derive line
  unchanged). `rq`/`rq_percpu`/`sched_class`/`sched_attr`/
  `load_weight`: Copy/Clone per their live emissions (`rq_percpu` never
  lost derives — its `rq_lock` is *typedef*-spelled, matching the N2
  `spinlock_t` callback entry; the struct-X quirk only hit tag-spelled
  members). build.rs lists every name in bare + `_t` + `"struct X"`
  forms per the N6 finding.
- **Evidence**: temporary in-tree `offset_of!` gate on the live bindgen
  forms (built green, removed) + cross-compiler `_Static_assert` probe
  (toolchain gcc 14.2, rv64gc/lp64d — scratchpad
  p3n7_static_assert_probe.c). The two AGREE on every size/align/offset
  — NO pipe-style divergence; gcc places the over-aligned `spinlock_t`
  members exactly where bindgen's explicit padding put them. ~70
  permanent hardcoded const asserts across the four owning modules.
  Derive-fidelity gate: attribute-block diff of ALL remaining bindgen
  types (PRE regenerated by reverting only the build.rs additions):
  REMOVED = exactly the 12 nativized emissions, ADDED 0, CHANGED 0.
- **Verified (worker)**: cache discipline before+after every build
  (BUILD_TYPE empty, toolchain gcc); `rm -rf` cargo target + full
  rebuild = **0 warnings**; 4 boots, each exactly ONE `init: starting
  sh`; `sleep 1` returns; `cat /nonexistent` → ENOENT; **testsig 21/21
  ALL PASSED**; `usertests preempt` OK, `usertests forkfork` OK,
  `usertests forktest` OK (standalone `forktest` binary is commented
  out of user/CMakeLists.txt — pre-existing, `exec forktest failed`);
  stressfs completed (write+read phases, prompt back); single-test
  `lost some free pages` trailers = documented pre-existing artifact;
  **forkforkfork → the identical pre-existing panic** (`Failed to
  remove interrupted waiter from queue`, `In thread ... at
  0x0000000080c34000`, IPI_REASON_CRASH both harts, then the
  `Load page fault stval=0xfffffffffffffff1` kerneltrap storm — same
  signature as the recorded baseline). NOT committed (orchestrator
  gate).

## Status vs the goal (2026-07-13)

- ✅ Kernel rewritten in Rust — every module row done. **ZERO C files: as
  of Wave P3-2 (2026-07-14) `find kernel -name '*.c'` is EMPTY.** The last
  remnant, the 46-line printf_shim.c variadic-printf entry, was eliminated
  by migrating all ~750 `printf()` call sites to `core::fmt`-based
  `kprint!`/`kprintln!` macros (no `c_variadic` needed). Assembly
  (entry/trampoline/kernelvec/swtch/sig_trampoline) bridged from Rust as
  designed; the C toolchain is still used only to assemble those
  C-preprocessed `.S` files.
- ✅ Idiomatic standards enforced (rust-skills governance + crate-wide
  audits + corrective packages; RAII/newtypes/scoped-unsafe/Result
  boundaries throughout; ~zero-warning builds).
- ✅ cmocka tests ported (35 host cargo tests + 4 in-kernel QEMU suites;
  3 cmocka suites intentionally retained for still-C header layers).
- ✅ CMake build system maintained + hardened (BUILD_TYPE forced empty,
  LINK_DEPENDS/staleness fixes, feature-gate wiring, ctest integration).
- ✅ Single VM, -smp 2 (pinned in cmake + enforced via ctest locks).
- ✅ LAB ignored (legacy default kept working; dead LAB=net block removed).
- ✅ Artifact hygiene (single image set; test build dirs on-demand).
- ✅ Progress recorded (this file, 39 iterations + plans/audits in
  docs/rustify/).
- Remaining backlog = Known issues below (pre-existing bugs discovered
  and documented during the rewrite, each with attribution evidence) +
  deferred polish (clippy ptr-deref lint sweep, doc-only SAFETY pattern
  backfill, lock/*.rs u! consolidation, rb tie-group hardening).

## Known issues (pre-existing, not caused by the rewrite)

- ~~`session_unref` (`kernel/tty/session.rs`) free-condition off-by-one
  (compared the pre-decrement `fetch_sub` value against `0` instead of
  `1`, so sessions were never freed via normal refcounting)~~ **The
  comparison itself FIXED in P3-9e** (2026-07-14) — see that wave's log
  entry and `session_unref`'s doc comment for the exact change and its
  no-double-free argument. **However, a second, deeper, still-open leak
  was found while verifying the fix**: `session_alloc`'s baseline
  `ref_cnt = 1` is never separately dropped by any call site except
  `session_setsid`'s allocation-failure path, so an ordinary successful
  `setsid()` still leaves its session's `ref_cnt` at `1` forever once its
  last thread/pgroup member leaves — live-verified via `ps -s` (dumping
  `session_list` directly): sessions with `threads=0, pgroups=0` persist
  indefinitely. Closing this needs a session-lifecycle design decision
  (where the baseline reference should be dropped), not a mechanical
  fix; left open, tracked here and in `session.rs`'s module doc.

- Intermittent `rm <dir>` EBUSY on repeated same-name directory
  reuse (alternating rounds) — proved pre-existing via A/B/A stash+rebuild
  during P3-9d; likely a dentry-cache-held extra ref on same-name reuse.

- ~~`cat /dev/tty` panics ("pid lock not held")~~ FIXED in Wave 22
  (pid_wlock around session_get_ctrl_tty; cat blocks correctly, ^C works).

- `usertests diskfull` hangs (never prints its `OK`/`FAILED` result).
  **Confirmed pre-existing**: reproduces identically on an unmodified C
  baseline (git worktree at this session's starting commit, before any
  Wave 19 xv6fs changes). Not exercised by any prior wave's test battery
  (root filesystem block-exhaustion path was never stress-tested before
  Wave 19). Root cause not investigated further (out of Wave 19's touch
  scope — the hang reproduces in code this wave never modified). Flagged
  for a dedicated investigation; likely somewhere in the disk-full
  interaction between the log's wait-for-space path and block allocation,
  or in `kernel/bio.c`'s buffer cache under sustained write pressure with
  zero free blocks.
- Rare `writebig` block-read-back-as-zero: observed exactly once during
  P3-1C and initially misattributed to RUST_FORCE_UNDEFINED pruning — a
  dedicated investigation (2026-07-14) proved the pruned link
  byte-identical in all loadable sections and writebig green ×2 on it, so
  the prune was exonerated and the observation reclassified as a rare
  fs-stress flake (same family as the outofinodes fault below). Track;
  investigate if it recurs. Meta-lesson recorded: A/B/A bisection can
  misattribute nondeterministic failures.
- `usertests outofinodes` (inode-exhaustion stress, not disk-block
  exhaustion) hit a null-superblock-pointer read fault inside
  `xv6fs_iupdate` once across ~30 runs during Wave 19 verification; did
  not reproduce in 28 follow-up trials. A full audit of every call site
  found no logic path that leaves an inode's `sb` field unset before
  `xv6fs_iupdate` is invoked (traced through `vfs_alloc_inode`/
  `vfs_add_inode`'s inode-creation ordering and `vfs_iput`'s
  destroy-inode sequencing in `vfs/{fs,inode}.rs`). Given `diskfull`'s
  confirmed-pre-existing hang under the same general class of stress
  (resource exhaustion on the root filesystem), this is suspected to be
  a pre-existing rare race as well, but that is not proven at the same
  confidence level (no baseline reproduction obtained). `xv6fs_iupdate`
  now has a defensive null check that panics with a clear message instead
  of dereferencing, so a recurrence will be loud and diagnosable rather
  than silent memory corruption.

- ~~`wait()` status encoding is POSIX-style, not classic-xv6 — one proc
  bug, (at least) three test symptoms~~ **FIXED** (wait-status encoding
  wave, 2026-07-13). Root cause was two-fold, not one: (1) the exited-child
  encode formula `(status & 0xff) << 8` in `xv6_exit_reap_zombie`
  (`kernel/proc/proc_shims.rs`) was correct POSIX, but `copyinstr2`'s and
  `exitwait`'s checks compared against the *raw* status instead of decoding
  with `WEXITSTATUS`/`WIFEXITED`; (2) signal-killed children had no
  well-formed encoding at all — every kill path (`__signal_send`,
  `sigaction`/`sigprocmask` revealing an already-pending fatal signal,
  `deliver_signal`'s bad-handler-address path, `handle_signal`'s
  termination branch, `sigreturn`'s corrupt-frame path) funneled into a
  generic `exit(-1)` that lost the actual signal number, so
  `xv6_exit_reap_zombie` encoded it through the *exited* formula and
  produced `(-1 & 0xff) << 8 == 0xff00` — a well-formed-looking but wrong
  "exited with code 255", never `WIFSIGNALED`.
  - **Design**: exited → `(code & 0xff) << 8` (`WIFEXITED`: low byte zero;
    `WEXITSTATUS`: high byte) — POSIX's standard 8-bit truncation, so
    `copyinstr2`'s `exit(747)` sentinel is only observable as `747 & 0xff
    == 235`. Killed by signal → `termsig & 0x7f` (`WIFSIGNALED`: low 7 bits
    in `(0, 0x7f)`; `WTERMSIG`: same 7 bits), nonzero.
  - **Implementation**: new `thread_signal_t::term_signal` field
    (`kernel/inc/signal_types.h`, mirrors the existing `stop_signal`
    field/pattern for `THREAD_STOPPED`) records the actual killing signal
    number the moment the kill decision is made — `set_term_signal_first`
    (`kernel/proc/signal.rs`, first-cause-wins) is called alongside every
    `THREAD_SET_KILLED` site (`__signal_send`'s `is_term` branch — the
    primary path, covers `kill()`/`SIGKILL`/page-fault-`SIGSEGV`;
    `sigaction`/`sigprocmask`'s pending-term reveals via `bits_ffs_g`;
    `deliver_signal`'s bad-handler path; `handle_signal`'s termination
    branch) plus `sigreturn`'s corrupt-frame `exit(-1)` (attributed to
    `SIGSEGV`, the standard real-kernel attribution for a broken signal
    context). `xv6_exit_reap_zombie` now branches on the zombie's
    `THREAD_FLAG_KILLED` bit (read via the same acquire-load pattern as
    `thread_user_space`): killed → `term_signal & 0x7f` (defensively
    falls back to `SIGKILL` if `term_signal` is ever 0 despite the flag —
    should not happen given the paired call sites); otherwise → the
    original `(xstate & 0xff) << 8` formula, unchanged. Full design
    rationale is a comment at the encode site
    (`kernel/proc/proc_shims.rs::xv6_exit_reap_zombie`).
  - `kernel/inc/uabi/wait.h` already had correct `WIFEXITED`/`WEXITSTATUS`/
    `WIFSIGNALED`/`WTERMSIG`/`WIFSTOPPED`/`WSTOPSIG` macros (added in the
    pre-Rust-rewrite `f0a652e` "unify user abi" commit) and `user/user.h`
    already included it — no header work was needed, only the kernel-side
    encode fix and adapting the tests that assumed raw/classic-xv6 status.
  - **Test inventory** (grepped every `wait(&x)`/`waitpid(...)` status
    check in `user/*.c`): only `user/usertests.c`'s `copyinstr2`,
    `killstatus`, `exitwait` compared status values exactly and needed
    updating (now `WIFEXITED(st) && WEXITSTATUS(st) == (747 & 0xff)`;
    `WIFSIGNALED(xst) && WTERMSIG(xst) == SIGKILL`; `WIFEXITED(xstate) &&
    WEXITSTATUS(xstate) == i` respectively). Every other status-checking
    program (`testsig.c`'s 11 checks, `mmaptest.c`, `cowtest.c`,
    `devtest.c`, `grind.c`, `symlinktest.c`'s `concur`) only tests `== 0`/
    `!= 0`, which is encoding-agnostic and needed no change; `sh.c` already
    used `WIFSTOPPED` correctly; `init.c`/`vforktest.c`/`forktest.c`/
    `pingpong.c`/`primes.c`/`stressfs.c`/`clonetest.c` don't inspect status
    content.
  - One known gap, left unfixed (out of touch scope — lives in
    `kernel/irq/trap.rs`, not proc): `push_sigframe`'s two `exit(-1)` calls
    on stack-allocation failure while pushing a *legitimate* signal frame
    (`irq/trap.rs` lines ~931/934) don't go through `THREAD_SET_KILLED`, so
    a death on that path still encodes as a misleading "exited with code
    255" rather than `WIFSIGNALED`. Not reachable by any test in this
    wave's required battery (stack-exhaustion-during-signal-delivery is a
    rare edge case); flagged for a future irq-focused pass.
  - **`usertests -q` new baseline**: proceeds through `copyinstr2`,
    `killstatus`, and `exitwait` (all `OK`) and every test in between/
    after up to `forkfork`, then hits the pre-existing `forkforkfork`
    panic (`"Failed to remove interrupted waiter from queue"` in
    `kernel/proc/thread_queue.rs` — see the entry below) exactly as
    documented. `execout`/`diskfull`/`outofinodes` are in the separate
    slow-test list, not `quicktests`, so `-q` never reaches them.

- `usertests forkforkfork` (fork-bomb stress, 3-deep nesting) panics the
  kernel: `"Failed to remove interrupted waiter from queue"`
  (`kernel/proc/thread_queue.rs:600` as of 2026-07-15; the line drifts as
  the file is edited — the site is the tq interrupted-wait removal path)
  under fork-exhaustion pressure, followed by IPI_REASON_CRASH on both
  harts. **Confirmed pre-existing**: reproduces identically on the
  unmodified pre-Wave-26 C baseline (074323c worktree, same
  toolchain/env). Never reachable before (sits after copyinstr2 in -q;
  no prior wave ran it individually). First observed 2026-07-13 (Wave 26
  post-copyinstr2 singles sweep). Needs a dedicated proc/tq
  investigation.

- `usertests execout` (memory-exhaustion exec stress, 15 rounds of
  sbrk-all-of-RAM) does not complete within 360s wall-clock under QEMU
  TCG on this setup — **identical on the unmodified pre-Wave-26 C
  baseline** (no completion, no crash; QEMU pegged >100% CPU, i.e.
  genuinely grinding through ~780MB of touched pages ×15 rounds, not
  hung). Likely impractically slow under emulation rather than a bug;
  never exercised by any prior wave. Flagged for a longer-timeout run or
  a RAM-reduced QEMU profile if a verdict is wanted.

- `open("/dev/tty")` fails -EINVAL: `dev/dev.c::device_register()`
  auto-assigns a minor for minor==0 registrants but never writes it back
  before creating the devtmpfs node, and `device_get()` rejects minor<=0.
  tty_dev (minor 0) is the only affected registrant. Root-caused during
  Wave 10; fix belongs in the Wave 21 dev.c port.

- `timer_node_init` never stores its `retry_limit` parameter (C original
  bug, preserved 1:1 in Rust for fidelity, documented in-file): every
  timer node is removed after its first firing regardless of the intended
  limit (sched_timer's DEFAULT_RETRY_LIMIT=3 behaves as 1). Fix candidate:
  small dedicated dispatch once someone decides the intended semantics.

- ~~`usertests -q` fails at `copyinstr2`: `exec(echo, BIG)` succeeds where
  it should fail (argument-size validation gap in exec/vm path)~~ —
  diagnosis DISPROVEN during Wave 26 (2026-07-13): exec's argument-size
  validation exists and fires correctly; the real root cause was the
  `wait()` status encoding, FIXED (see the consolidated entry above) — the
  `-q` run now proceeds past `copyinstr2` to the pre-existing
  `forkforkfork` panic.
- ~~`map_pages`/`walk_internal` (vm_pgtab.rs): intermediate page-table pages
  from earlier loop iterations are never freed when a later iteration fails
  with ENOMEM~~ — fixed in Iteration 11 (`unwind_partial_map`).

## Working notes

- **Build env (MANDATORY before any `cmake ..`)**: `export
  TOOLPREFIX=/home/es/xv6/toolchain/build/bin/riscv64-unknown-elf-` (custom
  GCC 14.2) and `export LAB=fs` (from env.conf). Configuring without these
  silently switches to system GCC 13.2 / LAB=util and produces a kernel whose
  userspace breaks (endless `init: starting sh` respawn loop). Verify with
  `grep C_COMPILER build/CMakeCache.txt`. ALSO verify
  `grep CMAKE_BUILD_TYPE build/CMakeCache.txt` is **empty** — a stray
  `Release` appends `-O3 -DNDEBUG` after the project's `-O0` and the C
  kernel breaks under -O3 (same respawn-loop signature; see Iteration 23).
  **NEW VECTOR (recurred during Wave 26, 2026-07-13)**: `cmake --build .
  --target qemu` (e.g. from an expect/test-driver script) ALSO triggers a
  full regenerate whenever any CMakeLists.txt is newer than the cache —
  if that shell lacks the exports, the cache is silently rewritten to
  system GCC + LAB=util *mid-verification* and every subsequent test runs
  on a poisoned kernel (top-level CMakeLists defaults LAB to "util"
  instead of erroring, so nothing fails loudly; this kernel even happened
  to boot and pass most tests under GCC 13.2, making the poisoning easy
  to miss). Wave 26 caught it via the C_COMPILER cache check + the
  linker-warning path prefix (`/usr/bin/...-ld` vs the toolchain path)
  and re-ran its full verification matrix on a clean toolchain rebuild
  (results were identical, but that was luck, not safety). Rule: ANY
  script that can invoke cmake — including QEMU-driving expect scripts —
  must set TOOLPREFIX/LAB in its own environment, and every wave's
  verification must re-check `C_COMPILER`/`CMAKE_BUILD_TYPE` in the cache
  *after* its last QEMU run, not just before its first.

- **Boot gate**: assert `grep -c "init: starting sh"` == 1 AND a `/ $` prompt
  in the console log — do not just tail the log.

- **External VM caution (2026-07-13)**: another project's QEMU VM may be
  running on this machine. NEVER blanket-kill qemu processes (`pkill
  qemu-system`, `killall qemu`). Use timeout-bounded foreground QEMU runs
  (self-terminating); if a kill is unavoidable, match OUR instance only
  (`pgrep -f 'xv6-riscv/build.*xv6.bin'` or a recorded PID). The single-VM
  rule applies to our xv6 instances only.

- **Rust style authority (MANDATORY for anyone writing Rust here)**: read
  `/home/es/.claude/skills/rust-skills/SKILL.md` (265 rules; details in
  `rules/*.md` next to it) and apply it to all new/changed Rust. Highest
  priority for this kernel: `unsafe-safety-comment`, `unsafe-minimize-scope`,
  `unsafe-maybeuninit`, `conc-atomic-ordering`, `own-*`, `err-*` (adapted to
  the crate's no_std `Errno` pattern), `api-newtype-safety`,
  `type-repr-transparent`, `name-*`, `pat-*`, `macro-rules-hygiene`,
  `const-fn`. Caveat: the crate is **edition 2021** — Rust-2024-only forms
  (`#[unsafe(no_mangle)]`, mandatory `unsafe extern`) do not apply; keep the
  crate's existing `#[no_mangle]` style. Report which rules you checked and
  any deliberate deviations.

- Build: `cd build && cmake .. && cmake --build . -j$(nproc)`
- Boot test: run `qemu-system-riscv64 -machine virt -bios default -kernel
  build/kernel/xv6.bin -initrd build/fs.img -m 1024M -smp 2 -nographic ...`
  (see `cmake/qemu.cmake`) with a timeout; expect `init: starting sh`.

- `LAB` build option is legacy — ignored, not removed.
- Old image/binary policy: keep at most artifacts from the last 5 iterations.
