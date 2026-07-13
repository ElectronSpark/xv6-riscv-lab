# xv6-riscv Kernel Rustification — Phase 2 Porting Plan (audit 2026-07-11)

Scope: every remaining `.c` under `kernel/` (~33,131 live LOC / 65 files).
Assembly (`entry.S`, `kernelvec.S`, `trampoline.S`, `swtch.S`,
`sig_trampoline.S`) stays; it is bridged from Rust via `extern "C"` symbol
contracts. Gate for every wave: build + QEMU boot (`-smp 2`) with
`grep -c "init: starting sh"` == 1 and a `/ $` prompt.

## 0. Cross-cutting facts every worker needs

- **CMake wiring**: top-level `.c` in `KERNEL_C_FILES`; each subdir (irq/,
  dev/, timer/, tty/, vfs/*, ipi/) is an OBJECT library. When a directory's C
  is gone, delete its `add_subdirectory()` plumbing (precedent: lock/, proc/).
  **Every wave must append its new public `#[no_mangle]` symbols to
  `RUST_FORCE_UNDEFINED` in kernel/CMakeLists.txt** or the linker may silently
  drop them.
- **Syscall dispatch** (`irq/syscall.c`): plain C array of `extern uint64
  sys_x(void)` pointers — no registration; Rust supplies syscalls by exact
  `#[no_mangle]` name (pattern: proc/sysproc.rs, mm/sysmm.rs).
  `fetchaddr/fetchstr/argraw/argint/argint64/argaddr/argstr` are called from
  ~90 sites — keep names/signatures exact through the syscall.c port.
- **panic/printf chain**: Rust `xv6_panic` → C `__panic_start`/`printf`/
  `__panic_end` (printf.c) → console.c → uart.c. `printf` is **variadic**
  (`format(printf,1,2)`), called from dozens of C files and Rust. Port needs
  nightly `c_variadic`/`VaList` or a thin C-ABI forwarding shim — resolve
  before Wave 4 starts.
- **wrapper.h coverage gaps** (add per-wave as needed, never invent `[u8;0]`
  opaques): `vfs/fs.h`, `vfs/pipe.h`, `vfs/vfs_types.h`,
  `dev/{cdev,dev,blkdev,bio,netdev}.h`, `tty/{tty,termios,tty_types}.h`,
  `kobject.h`.
- **uapi/user/mkfs**: user/ and mkfs/ stay C and include `kernel/inc/types.h`,
  `param.h`, `uabi/*.h`, `vfs/xv6fs/ondisk.h` by path — these headers are ABI
  surface and must remain valid C headers throughout.
- **Host tests** (test/CMakeLists.txt): separate CMake project compiling some
  kernel C directly; already stale (references deleted lock/mm C). Breaking
  ut_hlist/ut_rbtree/ut_pcache in Wave 1 is expected collateral — noted, not
  blocking. (The whole suite gets ported to Rust separately.)
- **Two "bio" concepts**: `kernel/bio.c` = classic xv6 buffer cache
  (bread/bwrite/brelse); `kernel/dev/bio.c` = Linux-style block-I/O request
  object. Different layers — don't conflate.

## 1. Boot-order & assembly contracts

- `entry.S` → `call start` (start.c: `void start(int hartid, void *fdt_base)`;
  `stack0` layout/symbol must be preserved — idle_thread_init recovers stack
  base by masking sp).
- `start_kernel.c` init order is a real invariant — reproduce verbatim
  (early_allocator → kobject → printf → fdt → sbi → ksymbols → kinit → kvminit
  → ... → userinit → idle_thread_init → sched_timer_init; then thread-context
  post-init: consoledevinit → ... → vfs_init → install_user_root).
- `kernelvec.S`/`trampoline.S` ↔ trap.c: fixed asm-visible names
  (`kerneltrap`, `kernel_irq`, `usertrap`, `usertrapret`,
  `user_kirq_entrance`, `__user_kirq_return`, `push_sigframe`,
  `restore_sigframe`) and the `trampoline_userret = TRAMPOLINE + (userret -
  trampoline)` relocation math computed in trapinit().
- Linker-script symbols (`_entry`, `_bss*`, `etext`, `_trampoline*`,
  `_cpulocal_start`, `_rodata*`, `_data*`, `_ksymbols_*`, `_kernel_image_end`,
  `_sigtrampoline`) keep working unchanged from Rust.
- **MMIO-heavy files** (volatile counts): x1_sdhci (15), yt8531 (11), x1_emac
  (9), e1000 (7), console (4), goldfish_rtc (3), uart/pci (2 each),
  virtio_disk (1 + virtqueue ordering protocol). Use
  read_volatile/write_volatile + fences.

## 2. QEMU vs Orange-Pi verifiability

- `dev/{yt8531,x1_emac,x1_sdhci}.c` are compiled always but probe nothing on
  QEMU virt — **compile-verify only**; consider host-side unit tests for
  register logic. Flag as lower confidence.
- Network path (pci/e1000/net/sysnet): our standard boot command DOES pass
  -netdev/-device e1000, so PCI probe runs; full traffic verification is
  optional (`sysnet.c::sockinit()` is an empty stub today).
- Everything else is on the default QEMU boot path and boot-verifiable.

## 3. Wave plan

Phase A — foundations:
1. **Wave 1**: bintree.c → rbtree.c (rbtree calls bintree; port bintree
   first), hlist.c, kobject.c. `rb_next_node`/`rb_find_key_rdown` already
   called from mm/vm.rs — mmaptest double-verifies. Add kobject.h to
   wrapper.h.
2. **Wave 2**: string.c + sbi.c (leaf; sbi has `wfi` + `ecall` macros →
   core::arch::asm!).
3. **Wave 3**: start.c (entry.S bridge; reuse machine.rs CSR helpers).

Phase B — console/panic chain:
4. **Wave 4**: uart.c → console.c → printf.c bottom-up, one worker. Resolve
   variadic-printf ABI first. Highest fan-out wave.

Phase C — irq:
5. **Wave 5**: backtrace.c (needs W1) + irq/plic.c + irq/irq.c.
6. **Wave 6**: irq/trap.c ALONE — preserve asm contract exactly; verify
   syscalls + page faults + signal delivery, not just boot.
7. **Wave 7**: irq/syscall.c — table becomes Rust static; keep arg-fetch
   helper names exact.

Phase D — timer/ipi:
8. **Wave 8**: timer/timer.c (needs W1) + sched_timer.c + goldfish_rtc.c.
9. **Wave 9**: ipi/ipi.c ALONE — interrupt context, audit for sleep/alloc.

Phase E — tty:
10. **Wave 10**: termios.c + tty_dev.c + pty.c.
11. **Wave 11**: tty.c + ptmx.c (core line discipline).
12. **Wave 12**: session.c (turns sysproc.rs::sys_setsid extern into native).

Phase F — vfs (largest):
13. **Wave 13**: vfs/inode.c (add vfs/fs.h, vfs_types.h to wrapper.h).
14. **Wave 14**: vfs/file.c + vfs/pipe.c.
15. **Wave 15**: vfs/fdtable.c.
16. **Wave 16**: vfs/fs.c (XL; sub-split: mount/superblock registry vs path
    lookup).
17. **Wave 17**: vfs/vfs_syscall.c (XL; after tty W12 and dev W21).
18. **Wave 18**: tmpfs (superblock+inode, then file+truncate).
19. **Wave 19**: xv6fs (superblock+inode, then file+truncate+log+block_cache)
    — root fs, boot-verify aggressively.
20. **Wave 20**: devtmpfs/superblock.c (reuses tmpfs_private.h).
    Deferred: tmpfs/xv6fs smoketests (entry points commented out) — port last
    or convert to native tests.

Phase G — dev:
21. **Wave 21**: dev/dev.c + cdev.c + blkdev.c (kobject in device_t).
22. **Wave 22**: dev/bio.c + kernel/bio.c (same worker, separate commits).
23. **Wave 23**: dev/fdt.c ALONE (XL, earliest-boot; a break bricks boot).
24. **Wave 24**: dev/nullrand.c + dev/netdev.c.
25. **Wave 25**: dev/{yt8531,x1_emac,x1_sdhci}.c (compile-verify only).
    Deferred: dev/dev_test.c (dead call site).

Phase H — finale:
26. **Wave 26**: exec.c (use native Rust VFS APIs; verify with usertests).
27. **Wave 27**: start_kernel.c (verbatim init order).
28. **Wave 28**: virtio_disk.c + ramdisk.c, then pci/e1000/net/sysnet.

## Appendix — LOC by area

data structures 1294; core/misc 2935; irq 1031; timer 662; ipi 231; tty 1843;
vfs core 7744 + tmpfs 4189 + xv6fs 4705 + devtmpfs 488; dev 6085;
drivers 1924. Total ≈ 33,131.
