set(KERNEL_BINARY kernel)
set(KERNEL_DECOMPILED kernel.asm)
set(KERNEL_SYMBOLS kernel.sym)

if(NOT DEFINED ENV{LAB})
    message(FATAL_ERROR "Environment variable LAB is not set")
else()
    message(STATUS "Building kernel for lab: $ENV{LAB}")
endif()

# ==============================================================================
# Platform configuration
# ==============================================================================
# Set PLATFORM to select target:
#   - qemu (default): KERNEL_BASE=0x80200000
#   - orangepi: KERNEL_BASE=0x00200000
# ==============================================================================
if(NOT DEFINED PLATFORM)
    set(PLATFORM "qemu")
endif()

if(PLATFORM STREQUAL "orangepi")
    set(KERNEL_BASE "0x00200000")
else()
    set(KERNEL_BASE "0x80200000")
endif()

# Size of parsed symbol index (for rb-tree nodes)
# ~39000 entries @ 80 bytes each = ~3MB
set(KERNEL_SYMBOLS_IDX_SIZE "0x300000")

message(STATUS "Platform: ${PLATFORM}, kernel base: ${KERNEL_BASE}")

# Core kernel sources in this directory (excluding subdirectories).
# NOTE: exec.c has been rewritten in Rust (kernel/exec.rs -- Phase 2
# Wave 26: ELF loader + argv/envp stack machinery + sys_exec). Its symbols
# are linked in via the xv6_rust staticlib below.
# NOTE: start_kernel.c has been rewritten in Rust (kernel/start_kernel.rs
# -- Phase 2 Wave 27: the boot orchestrator -- init order carried over
# verbatim, see that file's module doc). Its symbols (start_kernel,
# start_kernel_post_init, __physical_memory_start/__physical_memory_end/
# __physical_total_pages) are linked in via the xv6_rust staticlib below.
# The RWAD_WRITE_TEST/SEMAPHORE_RUNTIME_TEST/WORKQUEUE_RUNTIME_SMOKE_TEST
# `target_compile_definitions` gates that used to apply to this file are
# gone too -- see RUST_TEST_FEATURES below, which replaces them with cargo
# features passed to the xv6_rust build.
# NOTE: virtio_disk.c and ramdisk.c have been rewritten in Rust
# (kernel/virtio_disk.rs, kernel/ramdisk.rs -- Phase 2 Wave 28, sub-wave
# A: the final porting wave's block drivers). Their symbols
# (virtio_disk_init/__virtio_mmio_base/__virtio_irqno, ramdisk_init) are
# linked in via the xv6_rust staticlib below.
# NOTE: e1000.c, net.c, pci.c, and sysnet.c have been rewritten in Rust
# (kernel/e1000.rs, kernel/net.rs, kernel/pci.rs, kernel/sysnet.rs --
# Phase 2 Wave 28, sub-wave B: the final porting wave's network drivers).
# Their symbols are linked in via the xv6_rust staticlib below. After
# this wave, the only C left under kernel/ (any depth) is printf_shim.c
# (see its own NOTE below) -- a documented, deliberate remnant (stable
# Rust cannot *define* a C-variadic function).
set(KERNEL_C_FILES
    # NOTE: hlist.c, bintree.c, rbtree.c, and kobject.c have been rewritten
    # in Rust (kernel/hlist.rs, kernel/bintree.rs, kernel/rbtree.rs,
    # kernel/kobject.rs — Phase 2 Wave 1). Their symbols are linked in via
    # the xv6_rust staticlib below.
    # NOTE: bits.c has been rewritten in Rust. The lookup tables it provided
    # (__uint8_bits_count, __uint8_trailing_zeros, __uint8_leading_zeros,
    # __uint8_inverse) are now produced by the `xv6_rust` staticlib and
    # whole-archive-linked into the kernel below.
    # NOTE: string.c and sbi.c have been rewritten in Rust (kernel/string.rs,
    # kernel/sbi.rs — Phase 2 Wave 2). Their symbols (including
    # memset/memcpy/memmove/memcmp, called from virtually every remaining C
    # file) are linked in via the xv6_rust staticlib below.
    # NOTE: uart.c has been rewritten in Rust (kernel/uart.rs — Phase 2
    # Wave 4). Its symbols are linked in via the xv6_rust staticlib below.
    # NOTE: console.c has been rewritten in Rust (kernel/console.rs —
    # Phase 2 Wave 4). Its symbols are linked in via the xv6_rust
    # staticlib below.
    # NOTE: printf.c has been rewritten in Rust (kernel/printf.rs — Phase
    # 2 Wave 4), EXCEPT for the C-variadic entry point `printf()` itself:
    # stable rustc 1.95 cannot *define* a C-variadic function
    # (`c_variadic` is nightly-only — verified empirically). printf_shim.c
    # is the minimal last-resort C remnant (the variadic `printf()` entry
    # point plus three one-line va_arg fetchers); every other symbol from
    # printf.c (printf_rust's real formatter, __panic_start/__panic_end,
    # panic_state, panic_disable_bt, printfinit, panic_msg_lock/unlock,
    # puts, putchar) is linked in via the xv6_rust staticlib below. Delete
    # this file once c_variadic stabilizes.
    ${KERNEL_DIR}/printf_shim.c
    # NOTE: backtrace.c has been rewritten in Rust (kernel/backtrace.rs —
    # Phase 2 Wave 5). Its symbols are linked in via the xv6_rust
    # staticlib below.
    # NOTE: bio.c (the classic xv6 struct buf disk-block cache) has been
    # rewritten in Rust (kernel/bufcache.rs — Phase 2 Wave 22; named
    # bufcache.rs, not bio.rs, to avoid colliding with the unrelated
    # kernel/dev/bio.rs — see that file's module doc). Its symbols
    # (binit/bread/bwrite/bwrite_async/bsync/bdirty_count/brelse/bpin/
    # bunpin) are linked in via the xv6_rust staticlib below.
)

# ==============================================================================
# Rust components (staticlib whole-archive linked into the kernel)
# ==============================================================================
set(RUST_CRATE_DIR ${KERNEL_DIR})
set(RUST_TARGET_TRIPLE riscv64gc-unknown-none-elf)
set(RUST_PROFILE release)
set(RUST_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/rust)
set(RUST_STATICLIB ${RUST_BUILD_DIR}/${RUST_TARGET_TRIPLE}/${RUST_PROFILE}/libxv6_rust.a)

file(GLOB_RECURSE RUST_CRATE_SOURCES
    ${RUST_CRATE_DIR}/Cargo.toml
    ${RUST_CRATE_DIR}/lib.rs
    ${RUST_CRATE_DIR}/list.rs
    # kernel/{bintree,rbtree,hlist,kobject}.rs (Phase 2 Wave 1 — data
    # structures module, ported from the .c files of the same name).
    ${RUST_CRATE_DIR}/bintree.rs
    ${RUST_CRATE_DIR}/rbtree.rs
    ${RUST_CRATE_DIR}/hlist.rs
    ${RUST_CRATE_DIR}/kobject.rs
    # kernel/{string,sbi}.rs (Phase 2 Wave 2 — leaf modules, ported from
    # the .c files of the same name).
    ${RUST_CRATE_DIR}/string.rs
    ${RUST_CRATE_DIR}/sbi.rs
    # kernel/start.rs (Phase 2 Wave 3 — entry.S's `call start` target and
    # the `stack0` per-hart boot-stack array, ported from kernel/start.c).
    ${RUST_CRATE_DIR}/start.rs
    # kernel/uart.rs (Phase 2 Wave 4 — 16550/PXA UART driver, ported from
    # kernel/uart.c).
    ${RUST_CRATE_DIR}/uart.rs
    # kernel/console.rs (Phase 2 Wave 4 — console device layer, ported
    # from kernel/console.c).
    ${RUST_CRATE_DIR}/console.rs
    # kernel/printf.rs (Phase 2 Wave 4 — printf/panic machinery, ported
    # from kernel/printf.c; printf()'s variadic entry point stays in
    # printf_shim.c, see the KERNEL_C_FILES comment above).
    ${RUST_CRATE_DIR}/printf.rs
    # kernel/backtrace.rs (Phase 2 Wave 5 — kernel symbol table +
    # frame-pointer backtrace walking, ported from kernel/backtrace.c).
    ${RUST_CRATE_DIR}/backtrace.rs
    ${RUST_CRATE_DIR}/mm/*.rs
    ${RUST_CRATE_DIR}/lock/*.rs
    ${RUST_CRATE_DIR}/machine/*.rs
    ${RUST_CRATE_DIR}/sync/*.rs
    # kernel/proc/ is now 100% Rust (its last two C files,
    # proc_rust_shims.c / proc_rust_bridge.c, were ported away); track its
    # sources here too so CMake reruns cargo when they change.
    ${RUST_CRATE_DIR}/proc/*.rs
    # kernel/irq/{irq_core,plic}.rs (Phase 2 Wave 5 — irq/plic.c +
    # irq/irq.c) + kernel/irq/trap.rs (Phase 2 Wave 6 — irq/trap.c) +
    # kernel/irq/syscall.rs (Phase 2 Wave 7 — irq/syscall.c). The `irq`
    # module has no C sources left, see kernel/irq/mod.rs /
    # kernel/irq/CMakeLists.txt.
    ${RUST_CRATE_DIR}/irq/*.rs
    # kernel/timer/{timer_core,sched_timer,goldfish_rtc}.rs (Phase 2 Wave 8
    # — timer/timer.c + timer/sched_timer.c + timer/goldfish_rtc.c). The
    # `timer` module has no C sources left, see kernel/timer/mod.rs.
    ${RUST_CRATE_DIR}/timer/*.rs
    # kernel/ipi.rs (Phase 2 Wave 9 — ipi/ipi.c: IPI send/receive
    # machinery + the cpus[]/cpus_init/mycpu_init/get_cpu_active_mask
    # per-CPU bring-up bookkeeping the C original shared the file with).
    # The kernel/ipi/ directory no longer exists (single top-level file,
    # same precedent as uart.rs/console.rs/printf.rs).
    ${RUST_CRATE_DIR}/ipi.rs
    # kernel/tty/{termios,tty_dev,pty}.rs (Phase 2 Wave 10 — termios.c +
    # tty_dev.c + pty.c), kernel/tty/{tty,ptmx}.rs (Phase 2 Wave 11 —
    # tty.c + ptmx.c), kernel/tty/session.rs (Phase 2 Wave 12 —
    # session.c). kernel/tty/ has no C sources left (see
    # kernel/tty/mod.rs); there is no kernel/tty/CMakeLists.txt / OBJECT
    # library for it (same precedent as lock/, timer/, ipi/, proc/).
    ${RUST_CRATE_DIR}/tty/*.rs
    # kernel/vfs/inode.rs (Phase 2 Wave 13 — vfs/inode.c),
    # kernel/vfs/{file,pipe}.rs (Phase 2 Wave 14 — vfs/file.c +
    # vfs/pipe.c), kernel/vfs/fdtable.rs (Phase 2 Wave 15 —
    # vfs/fdtable.c), kernel/vfs/fs.rs (Phase 2 Wave 16 — vfs/fs.c), and
    # kernel/vfs/vfs_syscall.rs (Phase 2 Wave 17 — vfs/vfs_syscall.c).
    ${RUST_CRATE_DIR}/vfs/*.rs
    # kernel/vfs/tmpfs/{superblock,inode,truncate,file}.rs (Phase 2 Wave
    # 18 — vfs/tmpfs/*.c, the first filesystem *driver*, as opposed to
    # vfs core, to move to Rust). kernel/vfs/tmpfs/ has no C sources left
    # (tmpfs_smoketest.c/.h deleted outright, dead code); its
    # tmpfs_private.h header stays — not because any C file still
    # includes it (kernel/vfs/devtmpfs/superblock.c, its original reason
    # for staying, was itself ported to Rust in Wave 20), but because
    # kernel/vfs/tmpfs/*.rs's own bindgen layouts (struct tmpfs_inode /
    # tmpfs_superblock / tmpfs_sb_private / tmpfs_dentry) still need it;
    # see wrapper.h's own comment.
    ${RUST_CRATE_DIR}/vfs/tmpfs/*.rs
    # kernel/vfs/xv6fs/{superblock,inode,truncate,file,log,block_cache}.rs
    # (Phase 2 Wave 19 — the root filesystem, ported in two sub-waves:
    # sub-wave A = superblock.c + inode.c, the mount/inode-alloc/
    # directory-entry core, build+boot-verified standalone before
    # sub-wave B started; sub-wave B = truncate.c (block mapping +
    # truncation), file.c (per-inode pcache + vfs_file_ops), log.c (the
    # write-ahead log / crash-consistency mechanism), block_cache.c (the
    # Wave-1 Rust-rbtree-indexed free-extent cache)). kernel/vfs/xv6fs/
    # has no C sources left (xv6fs_smoketest.c/.h deleted outright, dead
    # code, same precedent as Wave 18's tmpfs_smoketest.c); its
    # xv6fs_private.h/block_cache.h headers stay (kernel/wrapper.h
    # includes them directly for bindgen; see wrapper.h's own comment).
    ${RUST_CRATE_DIR}/vfs/xv6fs/*.rs
    # kernel/vfs/devtmpfs/superblock.rs (Phase 2 Wave 20 —
    # vfs/devtmpfs/superblock.c, the last C file in kernel/vfs/). Reuses
    # tmpfs's inode/dentry infrastructure via the tmpfs_private.h C-ABI
    # contract kept alive since Wave 18 specifically for this wave (see
    # kernel/vfs/devtmpfs/superblock.rs's module doc). kernel/vfs/
    # devtmpfs/ has no C sources left (devtmpfs_private.h, its own
    # private header, deleted outright — its sole consumer was
    # superblock.c; the *public* kernel/inc/devtmpfs.h stays unchanged,
    # kernel/dev/dev.c, still C — Wave 21, includes it directly). With
    # this wave, kernel/vfs/ as a whole has no C sources left at any
    # level — there is no more kernel/vfs/CMakeLists.txt / `vfs` OBJECT
    # library (same precedent as lock/, timer/, ipi/, tty/, proc/).
    ${RUST_CRATE_DIR}/vfs/devtmpfs/*.rs
    # kernel/dev/{dev,cdev,blkdev}.rs (Phase 2 Wave 21 — dev/dev.c +
    # cdev.c + blkdev.c) + kernel/dev/bio.rs (Phase 2 Wave 22 —
    # dev/bio.c, the Linux-style bio request object; not to be confused
    # with kernel/bufcache.rs below). Retroactively added here (Wave 21
    # omitted it from this glob by oversight — cargo itself always
    # compiles the true `mod`-declared source tree regardless of this
    # list, which only drives CMake's rebuild-on-change tracking for the
    # custom `cargo build` command below).
    ${RUST_CRATE_DIR}/dev/*.rs
    # kernel/bufcache.rs (Phase 2 Wave 22 — kernel/bio.c, the classic xv6
    # struct buf disk-block cache; see that file's module doc for why
    # it isn't named bio.rs).
    ${RUST_CRATE_DIR}/bufcache.rs
    # kernel/exec.rs (Phase 2 Wave 26 — exec.c: ELF loader + argv/envp
    # stack machinery + sys_exec).
    ${RUST_CRATE_DIR}/exec.rs
    # kernel/start_kernel.rs (Phase 2 Wave 27 — start_kernel.c: the boot
    # orchestrator; see that file's module doc for the init-order/hart-
    # sync/test-gate contracts).
    ${RUST_CRATE_DIR}/start_kernel.rs
    # kernel/virtio_disk.rs + kernel/ramdisk.rs (Phase 2 Wave 28, sub-wave
    # A — the final porting wave's block drivers, ported from
    # kernel/virtio_disk.c + kernel/ramdisk.c).
    ${RUST_CRATE_DIR}/virtio_disk.rs
    ${RUST_CRATE_DIR}/ramdisk.rs
    # kernel/pci.rs + kernel/e1000.rs + kernel/net.rs + kernel/sysnet.rs
    # (Phase 2 Wave 28, sub-wave B — the final porting wave's network
    # drivers, ported from kernel/pci.c + kernel/e1000.c + kernel/net.c +
    # kernel/sysnet.c; the last non-printf_shim.c C removed from
    # kernel/).
    ${RUST_CRATE_DIR}/pci.rs
    ${RUST_CRATE_DIR}/e1000.rs
    ${RUST_CRATE_DIR}/net.rs
    ${RUST_CRATE_DIR}/sysnet.rs)

find_program(CARGO_EXECUTABLE cargo REQUIRED)

# Pass the kernel C include search paths to bindgen (consumed by build.rs).
# Keep this in sync with the include flags in the top-level CMakeLists.txt.
set(RUST_BINDGEN_INCLUDES
    "${CMAKE_SOURCE_DIR};${KERNEL_DIR}/inc;${CMAKE_BINARY_DIR}/kernel/inc")

# ==============================================================================
# In-kernel test-suite launch gates (RWLOCK_TEST / SEMAPHORE_TEST /
# WORKQUEUE_RUNTIME_SMOKE_TEST), wired through to xv6_rust as cargo
# features.
#
# Before Phase 2 Wave 27, `start_kernel.c` gated the *calls* to
# `rwsem_launch_tests()` / `semaphore_launch_tests()` /
# `workqueue_runtime_smoke_test()` (all three suites themselves are Rust,
# linked in unconditionally, exactly like the old always-built
# kernel/lock/*_test.c object files) behind `#ifdef RWAD_WRITE_TEST` /
# `#ifdef SEMAPHORE_RUNTIME_TEST` / `#ifdef WORKQUEUE_RUNTIME_SMOKE_TEST`,
# themselves `target_compile_definitions`'d onto the KERNEL_BINARY /
# kernel_with_symbols_elf targets from these same three ENV checks. That
# mechanism only worked because start_kernel.c was a plain C target
# source -- `target_compile_definitions` has no effect on the separate
# `cargo build` custom command below. Wave 27 ported start_kernel.c to
# Rust (kernel/start_kernel.rs), so the gates are now three Cargo
# features (`rwlock_test`/`semaphore_test`/`workqueue_smoke_test`,
# declared in kernel/Cargo.toml) selected via `cargo build --features`,
# computed from the exact same three ENV checks -- `RWLOCK_TEST=1
# SEMAPHORE_TEST=1 cmake ..` keeps working end-to-end unchanged from the
# outside.
set(RUST_TEST_FEATURES "")
if(DEFINED ENV{RWLOCK_TEST})
    list(APPEND RUST_TEST_FEATURES "rwlock_test")
endif()
if(DEFINED ENV{SEMAPHORE_TEST})
    list(APPEND RUST_TEST_FEATURES "semaphore_test")
endif()
if(DEFINED ENV{WORKQUEUE_RUNTIME_SMOKE_TEST})
    message(STATUS "WORKQUEUE_RUNTIME_SMOKE_TEST enabled")
    list(APPEND RUST_TEST_FEATURES "workqueue_smoke_test")
endif()
# Phase 4 (docs/rustify/test_port_plan.md) in-kernel test suites:
# kernel/mm/pcache_test.rs + kernel/proc/workqueue_test.rs. Same mechanism,
# new independent env vars/features -- additive to (not a replacement for)
# the three gates above.
if(DEFINED ENV{PCACHE_TEST})
    message(STATUS "PCACHE_TEST enabled")
    list(APPEND RUST_TEST_FEATURES "pcache_test")
endif()
if(DEFINED ENV{WORKQUEUE_TEST})
    message(STATUS "WORKQUEUE_TEST enabled")
    list(APPEND RUST_TEST_FEATURES "workqueue_test")
endif()
list(JOIN RUST_TEST_FEATURES "," RUST_TEST_FEATURES_STR)

# CMake's `Unix Makefiles` generator (this project's generator) does not
# notice that only a custom command's *command line* changed between two
# `cmake ..` reconfigures -- it only reruns a custom command whose
# `DEPENDS` files are newer than its `OUTPUT`. Toggling
# RWLOCK_TEST/SEMAPHORE_TEST/WORKQUEUE_RUNTIME_SMOKE_TEST and
# reconfiguring changes RUST_TEST_FEATURES_STR (hence the `--features`
# argument below) without touching any source file, so without this
# stamp a stale libxv6_rust.a would silently keep the previous gate
# state. `file(GENERATE)` only updates the output file's mtime when its
# content actually changes, so the stamp -- and therefore a cargo
# rebuild -- only fires when the resolved feature list actually differs.
set(RUST_TEST_FEATURES_STAMP ${RUST_BUILD_DIR}/test_features.stamp)
file(GENERATE OUTPUT ${RUST_TEST_FEATURES_STAMP} CONTENT "${RUST_TEST_FEATURES_STR}\n")

add_custom_command(
    OUTPUT ${RUST_STATICLIB}
    # Force-remove the previous artifact before invoking cargo, and
    # unconditionally `touch` it afterwards. Both empirically verified
    # necessary (not just defensive), via direct `cargo build`/`md5sum`/
    # `stat` A/B testing outside CMake: cargo keeps fingerprinted build
    # products for *every* feature combination it has ever seen in this
    # target-dir, not just the most recent one. When a `cmake ..`
    # reconfigure cycles back to a previously-seen combination (e.g.
    # RWLOCK_TEST=1 build, then a plain build, then RWLOCK_TEST=1 again),
    # cargo recognizes the fingerprint match and re-serves the
    # byte-identical cached artifact -- correct content, but *not* with a
    # freshly-advanced mtime (rm+recreate wasn't sufficient by itself:
    # cargo's cache-hit copy can preserve an old cached timestamp rather
    # than stamping "now"). CMake's own `LINK_DEPENDS` staleness check on
    # ${RUST_STATICLIB} (see the plain/symbols kernel targets below) relies
    # entirely on that mtime advancing past the *previous* link's mtime, so
    # a same-content-stale-mtime miss there silently skips relinking the
    # kernel executable, leaving the wrong feature combination's binary in
    # place even though this custom command itself did rerun. The trailing
    # `touch` guarantees "now" unconditionally, regardless of cargo's
    # internal cache/hardlink behavior.
    COMMAND ${CMAKE_COMMAND} -E rm -f ${RUST_STATICLIB}
    COMMAND ${CMAKE_COMMAND} -E env
            "XV6_KERNEL_INCLUDES=${RUST_BINDGEN_INCLUDES}"
            "LAB=$ENV{LAB}"
            ${CARGO_EXECUTABLE} build --${RUST_PROFILE}
            --target ${RUST_TARGET_TRIPLE}
            --target-dir ${RUST_BUILD_DIR}
            --manifest-path ${RUST_CRATE_DIR}/Cargo.toml
            --features "${RUST_TEST_FEATURES_STR}"
    COMMAND ${CMAKE_COMMAND} -E touch ${RUST_STATICLIB}
    DEPENDS ${RUST_CRATE_SOURCES}
            ${RUST_CRATE_DIR}/build.rs
            ${RUST_CRATE_DIR}/wrapper.h
            ${RUST_TEST_FEATURES_STAMP}
    COMMENT "Building xv6_rust staticlib (${RUST_TARGET_TRIPLE}, ${RUST_PROFILE}, features=[${RUST_TEST_FEATURES_STR}])"
    VERBATIM
)

add_custom_target(xv6_rust DEPENDS ${RUST_STATICLIB})

set(KERNEL_SCAN_OBJS
    # NOTE: start.c has been rewritten in Rust (kernel/start.rs — Phase 2
    # Wave 3). Its symbols (start, stack0) are linked in via the
    # xv6_rust staticlib below.
    # NOTE: printf.c has been rewritten in Rust (kernel/printf.rs +
    # printf_shim.c — Phase 2 Wave 4, see the KERNEL_C_FILES comment).
)

if(DEFINED ENV{KSCAN})
    list(APPEND KERNEL_SCAN_OBJS ${KERNEL_DIR}/kscan.c)
endif()

if($ENV{LAB} STREQUAL "lock")
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/stats.c ${KERNEL_DIR}/sprintf.c)
endif()

# NOTE: the LAB=net duplicate-append block (previously here: `if($ENV{LAB}
# STREQUAL "net") list(APPEND KERNEL_C_FILES e1000.c net.c pci.c)
# endif()`) is removed — dead even before this wave (e1000.c/net.c/pci.c
# were already unconditionally in KERNEL_C_FILES above, so this was a
# harmless but pointless duplicate `list(APPEND ...)`; noted in the
# Phase 2 plan §3 as cleanup-while-touching-this-file). Now that
# e1000.c/net.c/pci.c are deleted (Phase 2 Wave 28, sub-wave B), the
# block would also reference nonexistent files.

set(KERNEL_S_FILES ${KERNEL_DIR}/entry.S)

# Generate asm-offsets.h before building anything else
add_subdirectory(${KERNEL_DIR}/inc)

# Pull in subdirectory object libraries.
# NOTE: kernel/lock has no C sources anymore (fully ported to Rust; see
# kernel/lock/*.rs, linked in via the xv6_rust staticlib above), so there
# is no add_subdirectory(${KERNEL_DIR}/lock) / OBJECT library for it.
# NOTE: kernel/timer has no C sources anymore either (fully ported to Rust
# in Phase 2 Wave 8; see kernel/timer/*.rs, linked in via the xv6_rust
# staticlib above) and never had any assembly sources, so there is no
# add_subdirectory(${KERNEL_DIR}/timer) / OBJECT library for it (same
# precedent as lock/, proc/).
# NOTE: kernel/ipi has no C sources anymore either (fully ported to Rust in
# Phase 2 Wave 9; see kernel/ipi.rs, linked in via the xv6_rust staticlib
# above) and never had any assembly sources, so the kernel/ipi/ directory
# itself is gone (same precedent as lock/, timer/, proc/) -- there is no
# add_subdirectory(${KERNEL_DIR}/ipi) / OBJECT library for it.
# NOTE: kernel/tty has no C sources anymore either (fully ported to Rust:
# termios.c/tty_dev.c/pty.c in Phase 2 Wave 10, tty.c/ptmx.c in Wave 11,
# session.c in Wave 12; see kernel/tty/*.rs, linked in via the xv6_rust
# staticlib above) and never had any assembly sources, so there is no
# add_subdirectory(${KERNEL_DIR}/tty) / OBJECT library for it (same
# precedent as lock/, timer/, ipi/, proc/).
# NOTE: kernel/vfs has no C sources anymore either, at any level (fully
# ported to Rust: vfs core in Phase 2 Waves 13-17, tmpfs/ in Wave 18,
# xv6fs/ in Wave 19, devtmpfs/ in Wave 20 — the last C file in the whole
# vfs tree; see kernel/vfs/*.rs and kernel/vfs/{tmpfs,xv6fs,devtmpfs}/*.rs,
# linked in via the xv6_rust staticlib above) and never had any assembly
# sources, so there is no add_subdirectory(${KERNEL_DIR}/vfs) / OBJECT
# library for it, and no kernel/vfs/CMakeLists.txt (same precedent as
# lock/, timer/, ipi/, tty/, proc/).
# NOTE: kernel/dev has no C sources left anywhere either (fully ported to
# Rust: dev/cdev/blkdev core in Phase 2 Wave 21, bio.c in Wave 22, fdt.c in
# Wave 23, nullrand.c/netdev.c in Wave 24, and the Orange-Pi-only
# yt8531.c/x1_emac.c/x1_sdhci.c -- the last C in kernel/dev/ -- in Wave 25;
# see kernel/dev/*.rs, linked in via the xv6_rust staticlib above) and
# never had any assembly sources, so there is no
# add_subdirectory(${KERNEL_DIR}/dev) / OBJECT library for it, and no
# kernel/dev/CMakeLists.txt (same precedent as lock/, timer/, ipi/, tty/,
# vfs/, proc/).
add_subdirectory(${KERNEL_DIR}/irq)
add_subdirectory(${KERNEL_DIR}/proc)

set_source_files_properties(${KERNEL_C_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_SCAN_OBJS} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_S_FILES} PROPERTIES LANGUAGE C)

set(KERNEL_OBJS ${KERNEL_S_FILES} ${KERNEL_C_FILES} ${KERNEL_SCAN_OBJS})

# Object libraries to link
# NOTE: vfs_tmpfs is gone (Phase 2 Wave 18), vfs_xv6fs is gone (Phase 2
# Wave 19), and vfs_devtmpfs is gone too (Phase 2 Wave 20 — kernel/vfs/
# has no C sources left anywhere; see the add_subdirectory NOTE above).
# NOTE: dev is gone too (Phase 2 Wave 25 — kernel/dev/ has no C sources
# left anywhere; see the add_subdirectory NOTE above).
set(KERNEL_OBJ_LIBS
    $<TARGET_OBJECTS:irq>
    $<TARGET_OBJECTS:proc>
)

set(KERNEL_LINK_LIBS irq proc)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding ")

# Generate linker script from template
set(KERNEL_LD_IN ${KERNEL_DIR}/kernel.ld.in)
set(KERNEL_LD ${CMAKE_CURRENT_BINARY_DIR}/kernel.ld)
configure_file(${KERNEL_LD_IN} ${KERNEL_LD} @ONLY)

# Symbols that must be force-pulled from the Rust staticlib so the linker
# keeps the corresponding object files. Avoid dragging in Rust's
# compiler-builtins (built with medlow code model).
#
# P3-1D FINAL PRUNE (2026-07-14): with the mesh sweep crate-wide complete
# (P3-1A/B/C/D), every symbol below that was kept only because *another
# Rust file* still reached it via an `extern "C"` redeclaration has had
# that redeclaration converted to a direct crate-path `use` -- the
# ordinary same-crate reference no longer needs a linker-visible `extern`
# name at all (codegen-units=1 + lto=true resolves it inside the single
# post-LTO compilation unit, well before the object ever reaches `ld`).
# That leaves exactly one class of symbol that genuinely still needs
# forcing: one whose *name* is referenced from outside this crate's own
# Rust source -- a `.S` file (`call`/`.dword` by literal symbol name), the
# one remaining C translation unit (`printf_shim.c`'s `printf_rust`
# call), or the LLVM libcall-recognition hazard documented below. Pruning
# any symbol NOT in that class is safe *by this link's own mechanics*,
# independent of whether the symbol is still `#[no_mangle]` in its
# defining file for other (Rust-level, not `ld`-level) reasons -- see the
# investigation below for why.
#
# Verification method (per the 2026-07-14 investigation two entries down):
# relink both kernel targets from the *same* already-built
# `libxv6_rust.a` with the pruned flags (no source rebuild), and byte-diff
# every `SHF_ALLOC` section against the unpruned link. Identical sections
# prove the prune cannot have dropped any code -- do this before trusting
# a black-box test battery alone. This wave's relink was byte-identical
# in `.text`/`.rodata`/`.data`/`.bss`/all three trampoline sections (only
# `.symtab`/`.strtab`, non-loaded ELF metadata, differed in size); full
# battery green on the pruned binary. See the worker report for the
# before/after symbol counts.
#
# DEDICATED INVESTIGATION (2026-07-14, P3-1C aftermath): the suspected
# linker mechanism (archive-member pulling / `--gc-sections` dropping a
# section with no live symbol reference) does NOT exist in this build.
# Evidence:
#   - `libxv6_rust.a` has exactly ONE member for the crate itself
#     (`xv6_rust-*.rcgu.o`), codegen-units=1 + `lto=true` collapse the
#     *entire* kernel crate into a single post-LTO object; only
#     compiler-builtins ships as separate per-intrinsic members. There is
#     no multi-member split for xv6_rust's own code that a pruned
#     `--undefined` could fail to pull in independently of another.
#   - `kernel.dir/link.txt` shows no `--gc-sections` anywhere in this
#     repo's link flags, so nothing culls sections after an object is
#     included.
#   - The keep-set below (asm-referenced four, `start`/`stack0`, and
#     `printf_rust`) already creates a genuine undefined-symbol demand
#     from objects `ld` links *before* it ever opens the archive
#     (`entry.o`'s `call start`, `kernelvec.o`'s `call kerneltrap`/`call
#     kernel_irq`, `trampoline.o`'s `.dword usertrap`/`.dword
#     user_kirq_entrance`, `printf_shim.o`'s `printf_rust` call) --
#     ordinary archive-member resolution pulls the one atomic member in
#     for that reason alone, with or without any `--undefined` flag.
#     Once pulled in, every symbol the member defines is present in the
#     final link regardless of whether it has its own `--undefined` flag.
#   - Reproduced directly (P3-1C's writebig corruption, see git history
#     for the full account): relinked both kernel targets by hand with a
#     265-entry prune from the same already-built `libxv6_rust.a` --
#     `.text`/`.rodata`/`.data`/`.bss`/all three trampoline sections came
#     out byte-identical to the unpruned link; `usertests writebig`
#     reported `OK` twice on the pruned binary. The originally observed
#     corruption did not reproduce -- root-caused as a confound (this
#     repo's documented build-cache-poisoning hazard, or an independent
#     rare fs-stress flake), not a true effect of this list.
#
# Remaining keep-set, five classes:
#   1. Asm-referenced (`.S` files `call`/`.dword` these by literal name --
#      grep-verified against `entry.S`/`kernelvec.S`/`trampoline.S`/
#      `swtch.S`/`sig_trampoline.S`, the only five `.S` files in the tree):
#      `kerneltrap`/`kernel_irq` (kernelvec.S `call`), `usertrap`/
#      `user_kirq_entrance` (trampoline.S `.dword`, address taken for the
#      trampoline page's kernel/irq entry slots), `start` (entry.S `call`),
#      `stack0` (entry.S `la sp, stack0`). `swtch.S`/`sig_trampoline.S`
#      reference no Rust symbol by name (pure register-shuffle asm; the
#      C/Rust side calls *them*, not the reverse).
#   2. `printf_shim.c`'s one import: `printf_rust` (verified by reading
#      the file -- its only `extern` declaration).
#   3. Libc-shaped names (`mem*`/`str*`/`puts`/`putchar`): LLVM's
#      TargetLibraryInfo can synthesize a call to a symbol of one of
#      these exact names via loop-idiom/builtin recognition even with no
#      corresponding Rust source call -- empirically confirmed for
#      `strdup` in P3-1A (a demotion attempt broke the build with no
#      offending call site anywhere in the diff). Kept as a class rather
#      than re-litigating each name individually.
#   4. `stack0` also doubles as class 1 above (listed once).
#   5. No further "linker-pull anchor" case was found -- every
#      MMIO-base/IRQ-number/other data `static` this crate publishes is
#      reached only by other Rust files via ordinary `use`s or (for the
#      handful still `#[no_mangle]` for the documented "widely-shared
#      data anchor, no benefit purely for mesh-cosmetic reasons" reason --
#      `cpus`, `platform`, `sockets`) via Rust-level `extern "C"` blocks
#      that resolve inside the single post-LTO member regardless of any
#      `--undefined` flag (class 1's real references already pull that
#      member in).
set(RUST_FORCE_UNDEFINED
    # Class 3: libc-shaped names (string.rs port). LLVM libcall
    # recognition can synthesize a call to any of these by name with no
    # traceable Rust source call site -- kept as a class (see the
    # module-level comment above), not because each one is individually
    # still referenced by name from outside this crate's Rust source.
    --undefined=memset
    --undefined=memcmp
    --undefined=memmove
    --undefined=memcpy
    --undefined=strcmp
    --undefined=strncmp
    --undefined=strncpy
    --undefined=safestrcpy
    --undefined=strlen
    --undefined=strnlen
    --undefined=strtok_r
    --undefined=strndup
    --undefined=strdup
    --undefined=puts
    --undefined=putchar
    # Class 1: asm-referenced (entry.S).
    --undefined=start
    --undefined=stack0
    # Class 1: asm-referenced (kernelvec.S `call`, trampoline.S `.dword`).
    --undefined=kerneltrap
    --undefined=kernel_irq
    --undefined=usertrap
    --undefined=user_kirq_entrance
    # Class 2: printf_shim.c's one import.
    --undefined=printf_rust
)

# ==============================================================================
# Plain kernel (without embedded symbols)
# ==============================================================================
add_executable(${KERNEL_BINARY} ${KERNEL_OBJS})
add_dependencies(${KERNEL_BINARY} generate_asm_offsets xv6_rust)
target_sources(${KERNEL_BINARY} PRIVATE ${KERNEL_OBJ_LIBS})
target_link_options(${KERNEL_BINARY} PRIVATE -z max-page-size=4096 -T ${KERNEL_LD}
    ${RUST_FORCE_UNDEFINED}
    ${RUST_STATICLIB})
target_link_libraries(${KERNEL_BINARY} PRIVATE ${KERNEL_LINK_LIBS})
# The staticlib is passed via link *options* (for ordering), which CMake does
# not track for staleness — without this, the kernel is not relinked when
# only libxv6_rust.a changes.
set_target_properties(${KERNEL_BINARY} PROPERTIES LINK_DEPENDS ${RUST_STATICLIB})

# NOTE: the RWLOCK_TEST/SEMAPHORE_TEST/WORKQUEUE_RUNTIME_SMOKE_TEST test
# gates used to be applied here via target_compile_definitions (consumed
# by start_kernel.c's #ifdefs). Since Phase 2 Wave 27 (start_kernel.c ->
# kernel/start_kernel.rs), there is no C target source left that
# references those macros -- the gates are now cargo features passed to
# the xv6_rust build itself, see RUST_TEST_FEATURES above.

# Decompile the kernel binary
add_custom_target(${KERNEL_DECOMPILED}
    COMMAND ${CMAKE_OBJDUMP} -S ${KERNEL_BINARY} > ${KERNEL_DECOMPILED}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Decompiling kernel binary"
    DEPENDS ${KERNEL_BINARY}
)

# Generate symbol file for plain kernel
add_custom_target(${KERNEL_SYMBOLS}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen_addrline.sh ${CMAKE_OBJDUMP} ${KERNEL_BINARY} ${KERNEL_SYMBOLS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating symbols"
    DEPENDS ${KERNEL_BINARY}
)

# ==============================================================================
# Kernel with embedded symbols (two-phase build)
# ==============================================================================
set(KSYM_PLACEHOLDER_SIZE "0x200000")
set(KSYM_PLACEHOLDER_ASM ${CMAKE_CURRENT_BINARY_DIR}/ksymbols_placeholder.S)

add_custom_command(
    OUTPUT ${KSYM_PLACEHOLDER_ASM}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
            ${KSYM_PLACEHOLDER_SIZE} ${KSYM_PLACEHOLDER_ASM}
    COMMENT "Generating ${KSYM_PLACEHOLDER_SIZE} byte .ksymbols placeholder"
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
)

add_library(ksymbols_placeholder OBJECT EXCLUDE_FROM_ALL ${KSYM_PLACEHOLDER_ASM})
add_dependencies(ksymbols_placeholder generate_asm_offsets)
set_source_files_properties(${KSYM_PLACEHOLDER_ASM} PROPERTIES GENERATED TRUE)

# Kernel with placeholder
add_executable(kernel_with_symbols_elf EXCLUDE_FROM_ALL ${KERNEL_OBJS})
add_dependencies(kernel_with_symbols_elf generate_asm_offsets ksymbols_placeholder xv6_rust)
target_sources(kernel_with_symbols_elf PRIVATE ${KERNEL_OBJ_LIBS} $<TARGET_OBJECTS:ksymbols_placeholder>)
target_link_options(kernel_with_symbols_elf PRIVATE -z max-page-size=4096 -T ${KERNEL_LD}
    ${RUST_FORCE_UNDEFINED}
    ${RUST_STATICLIB})
target_link_libraries(kernel_with_symbols_elf PRIVATE ${KERNEL_LINK_LIBS})
# Relink when only libxv6_rust.a changes (see matching note on the plain
# kernel target above).
set_target_properties(kernel_with_symbols_elf PROPERTIES LINK_DEPENDS ${RUST_STATICLIB})

# NOTE: see the matching NOTE on the plain kernel target above -- the test
# gates are cargo features now (RUST_TEST_FEATURES), not
# target_compile_definitions on this target.

# Generate symbols from kernel with placeholder
set(KERNEL_SYM_FINAL ${CMAKE_CURRENT_BINARY_DIR}/kernel_with_symbols_elf.sym)
add_custom_command(
    OUTPUT ${KERNEL_SYM_FINAL}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen_addrline.sh ${CMAKE_OBJDUMP}
            $<TARGET_FILE:kernel_with_symbols_elf> ${KERNEL_SYM_FINAL}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating symbols from kernel with placeholder"
    DEPENDS kernel_with_symbols_elf
)

# Embed symbols into kernel
add_custom_target(kernel_with_symbols
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/embed_ksymbols.py
            $<TARGET_FILE:kernel_with_symbols_elf> ${KERNEL_SYM_FINAL} --elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Embedding symbols into kernel ELF"
    DEPENDS kernel_with_symbols_elf ${KERNEL_SYM_FINAL}
            ${CMAKE_SOURCE_DIR}/scripts/embed_ksymbols.py
)

# ==============================================================================
# Final outputs: xv6.bin and xv6.sym
# ==============================================================================
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:kernel_with_symbols_elf> xv6.bin
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/patch_image_size.py xv6.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating xv6.bin (${PLATFORM})"
    DEPENDS kernel_with_symbols $<TARGET_FILE:kernel_with_symbols_elf>
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.sym
    COMMAND ${CMAKE_COMMAND} -E copy ${KERNEL_SYM_FINAL} xv6.sym
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating xv6.sym"
    DEPENDS ${KERNEL_SYM_FINAL}
)

# ==============================================================================
# Compressed kernel image (optional)
# ==============================================================================
# Both QEMU and U-Boot (booti) support gzip-compressed kernel images.
# The compressed image is named xv6.bin.gz
# ==============================================================================
if(ENABLE_COMPRESSION)
    find_program(GZIP gzip)
    if(GZIP)
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin.gz
            COMMAND ${GZIP} -9 -k -f xv6.bin
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Compressing xv6.bin -> xv6.bin.gz"
            DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
        )
        set(KERNEL_BIN_COMPRESSED ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin.gz)
    endif()
endif()

add_custom_target(kernel_all ALL
    DEPENDS kernel_with_symbols ${KERNEL_DECOMPILED}
            ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
            ${CMAKE_CURRENT_BINARY_DIR}/xv6.sym
            $<$<BOOL:${ENABLE_COMPRESSION}>:${KERNEL_BIN_COMPRESSED}>
)

# Export variables needed by boot/CMakeLists.txt
set(KERNEL_BIN xv6.bin PARENT_SCOPE)
set(KERNEL_BIN_GZ xv6.bin.gz PARENT_SCOPE)
set(KERNEL_SYM_DEPLOY xv6.sym PARENT_SCOPE)
set(ENABLE_COMPRESSION ${ENABLE_COMPRESSION} PARENT_SCOPE)