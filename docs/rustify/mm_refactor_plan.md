# mm module — idiomatic-Rust refactor plan (audit 2026-07-11)

Produced by a read-only audit of `kernel/mm/*.rs` (13,439 lines, 16 files).
Regression gate for every package: full CMake build + QEMU boot (`-smp 2`) to
`init: starting sh`.

## Key findings

- Two porting styles coexist. Mature (`page.rs`, `slab.rs`, `kalloc.rs`,
  `early_allocator.rs`, `vm_pgtab.rs`): real `#[repr(C)]` structs, fine-grained
  `unsafe`, some `sync::KSpinlock` RAII already. Transliterated (`pcache.rs` +
  `pcache_shims.rs`, `vm.rs`, `*_shims.rs`): opaque `[u8;0]` struct stand-ins,
  whole-body `unsafe` via a `macro_rules! u` macro **defined identically in 8
  files and used 425×**, 94 manual lock/unlock pairs in `pcache.rs` alone,
  zero `Result<_,_>` in the entire module.
- **376 `#[no_mangle]` exports; only 81 are called from remaining C code**
  (verified by grep of all kernel .c/.h). The other 295 are Rust→Rust
  round-trips through the C ABI and can become safe Rust APIs.
  Re-verify per-symbol with grep before deleting any export.
- `pcache_shims.rs` has ~180 pure field accessors with zero C callers —
  exists only because `pcache.rs` uses opaque stand-ins instead of
  `crate::bindings::pcache` directly (the model `vm.rs` already follows).
- Three nominally-distinct opaque `Page` types (`pcache.rs:30`,
  `vm_pgtab.rs:63`, vs the real `page.rs:71`) — latent type confusion.
- `mm_safe.rs` is well-designed (`PageHandle`, `SlabBox`, `KBox`, `SpinGuard`)
  but entirely **dead code** (zero external callers).
- Worst single function: `vm.rs:1588-1706 vma_validate` — 118-line `u!{}`
  block, 6 unlock sites, sleep+relock mid-loop.

## Work packages (order: WP1 → WP2 → WP3 → WP5 → WP4)

### WP1 — RAII locking in pcache.rs / vm.rs (LOW risk) ✅ order 1
Replace ~94 manual `xv6_pcache_spin_lock/_unlock`, `tree_rlock/...` pairs in
`pcache.rs` and ~30 internal `vm_wlock/vm_pgtable_lock/...` pairs in `vm.rs`
with `sync::KSpinlock`/`KRwlock` guards (pattern: `slab.rs:701,859`).
Exported lock symbols keep their C-callable paired form; only internal Rust
call sites change. Fixes `vma_validate` unlock-site bug class. Use
`lock_two()` (sync.rs) for the two-VM address-order site `vm.rs:2120-2156`.

### WP2 — Collapse pcache_shims.rs into pcache.rs (MEDIUM risk) ✅ order 2
Delete opaque stand-ins in `pcache.rs:28-33`; use `crate::bindings::{pcache,
pcache_node, page_t, ...}` directly. Replace ~180 accessor round-trips with
safe impl methods on a `PcacheHandle(NonNull<bindings::pcache>)` newtype.
Move real logic (globals_init, registry, container_of, panic helper) into
pcache.rs. Delete the 8 `xv6_e*` errno forwarders (use bindings consts).
Verify with filesystem-heavy workload (pcache = block cache).

### WP3 — Collapse page/slab/vm_pgtab shims (MEDIUM risk) ✅ order 3
Delete pure accessors; move real logic into parent modules:
`print_buddy_system_stat`, `check_buddy_system_integrity`, `sys_memstat`
(page_shims → page.rs); slab registry + `slab_dump_all`/`slab_shrink_all`
(slab_shims → slab.rs); `kernel_pagetable` storage + reserved-region init
(vm_pgtab_shims → vm_pgtab.rs). C-called symbols (`slab_dump_all`,
`slab_shrink_all`, `print_buddy_system_stat`, `sys_memstat`) keep signatures.

### WP5 — Result conversion + macro/newtype sweep (LOW-MED risk) ✅ order 4
Internal helpers → `Result<T, Errno>`, convert to -errno only at the 81
C-ABI boundary fns (pattern: `sysmm.rs neg_errno`). Remove all
`macro_rules! u` definitions in favor of scoped `unsafe {}`. Unify
`container_of` into one generic helper / intrusive-list iterator. Resolve
mm_safe vs sync guard duplication (prefer sync::KSpinlock; delete or wire up
mm_safe duplicates). One canonical `Page` type.

### WP4 — RAII ownership handles (MED-HIGH risk, LAST) ✅ order 5
Wire `PageHandle`/`SlabBox` into real alloc/free sites (`vm.rs:1645-1658`
fault race-loser cleanup; pcache_node alloc). Changes ownership-transfer
points (`into_raw` where pages move to C/page-table ownership) — land one
call site at a time, verify concurrent-fault path under -smp 2.

## C-called symbol groups (must keep #[no_mangle], 81 total)
walk/walkaddr/mappages/uvm*/kvm* (pgtable core); vm_* lifecycle + syscalls
(vm_init/dup/put/copy*/mmap*/munmap*/mprotect/mremap/msync/madvise/mincore/
locks/cpu_online/...); vma_alloc/free/merge/split/validate; sys_mmap etc.
syscall entries; pcache public API (init/get/put/read/flush/sync/...);
slab_dump_all/slab_shrink_all; print_buddy_system_stat.
