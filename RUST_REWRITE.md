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
| core / misc | 🟡 | start_kernel.c, printf_shim.c (variadic `printf()` C remnant) | string.rs + sbi.rs (Wave 2), start.rs (Wave 3), uart.rs + console.rs + printf.rs (Wave 4), backtrace.rs (Wave 5), exec.rs (Wave 26) done; boot path bridges entry.S |
| block/net drivers | ⬜ | bio.c, virtio_disk.c, ramdisk.c, e1000.c, pci.c, net.c, sysnet.c | |
| data structures | ✅ | — | bintree.rs, rbtree.rs, hlist.rs, kobject.rs (Wave 1, 2026-07-11); list.rs pre-existing |
| host tests (`test/`) | ⬜ | cmocka suites | port to Rust (cargo test or equivalent harness) |

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

## Known issues (pre-existing, not caused by the rewrite)

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
    panic (`"Failed to remove interrupted waiter from queue"`,
    `kernel/proc/thread_queue.rs:421` — see the entry below) exactly as
    documented. `execout`/`diskfull`/`outofinodes` are in the separate
    slow-test list, not `quicktests`, so `-q` never reaches them.

- `usertests forkforkfork` (fork-bomb stress, 3-deep nesting) panics the
  kernel: `"Failed to remove interrupted waiter from queue"`
  (`kernel/proc/thread_queue.rs:421`, tq interrupted-wait removal path)
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
