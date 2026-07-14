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
# P3-1C mesh sweep (2026-07-13): a caller-survey-driven prune of the
# vfs/tty/console/uart/printf entries here (265 of them -- every symbol
# demoted from `#[no_mangle]` in that wave) was attempted and then FULLY
# REVERTED after it was empirically caught causing silent data corruption:
# with the pruned list, `usertests writebig` (a single ~52 MB file spanning
# xv6fs's double-indirect block range) read back block 53403's content as
# 0 instead of 53403, both mid-`usertests -q` and in isolation -- reverting
# just this list (keeping every P3-1C code change) made it pass again,
# byte-for-byte matching the pre-wave baseline.
#
# DEDICATED INVESTIGATION (2026-07-14): the suspected linker mechanism
# (archive-member pulling / --gc-sections dropping a section with no
# live symbol reference) does NOT exist in this build. Evidence:
#   - `libxv6_rust.a` has exactly ONE member for the crate itself
#     (`xv6_rust-*.rcgu.o`, ~1.3 MB, 1842 defined symbols) -- codegen-
#     units=1 + `lto=true` collapse the *entire* kernel crate (incl.
#     core/alloc) into a single post-LTO object; only compiler-builtins
#     ships as ~400 separate per-intrinsic members. There is no
#     multi-member split for xv6_rust's own code that a pruned
#     `--undefined` could fail to pull in independently of another.
#   - `kernel.dir/link.txt` shows no `--gc-sections` anywhere in this
#     repo's link flags, so nothing culls sections after an object is
#     included.
#   - `--undefined=SYM` here only forces archive extraction of that one
#     atomic member; once ANY of the (492 total, 227 post-prune) forced
#     names resolves into it -- and plenty not in the demoted-265 set
#     do (`printf_rust`, `start`, `kerneltrap`, `usertrap`, ...) -- the
#     *whole* member is linked in either way, so a prune restricted to
#     symbols already reached via ordinary intra-crate Rust calls cannot
#     change which machine code is present.
#   - Reproduced directly: relinked both the plain `kernel` and
#     `kernel_with_symbols_elf` targets by hand with the 265 entries
#     removed from this exact list (same already-built `libxv6_rust.a`,
#     no source changes). `.text`/`.rodata`/`.data`/`.bss`/all three
#     trampoline sections came out byte-identical to the unpruned link
#     (only `.symtab`/`.strtab`, non-loaded ELF metadata, differed in
#     size). Built a full `xv6.bin` from the pruned link and booted it
#     in an isolated QEMU instance twice against a copy of the real
#     fs.img: `usertests writebig` reported `OK` both times, matching
#     two baseline runs. The originally observed corruption did not
#     reproduce.
# Conclusion: pruning entries for symbols reached only via ordinary
# same-crate Rust calls (the P3-1C 265-symbol set) is NOT, by this
# link's own mechanics, capable of causing the writebig symptom -- the
# P3-1C bisection almost certainly hit a confound (this repo has a
# documented recurring build-cache-poisoning hazard, see RUST_REWRITE.md
# "Working notes", and independently a documented rare/flaky failure
# under fs stress, "outofinodes") rather than a true A/B difference from
# this list. Still: do NOT blanket-prune this list without re-verifying
# with the byte-diff method above (relink both ways from one
# `libxv6_rust.a`, diff every `SHF_ALLOC` section) *before* trusting a
# black-box test battery alone -- it is strictly stronger and orders of
# magnitude cheaper than a multi-minute writebig run per candidate.
set(RUST_FORCE_UNDEFINED
    # bits.c port
    # slab.c port
    --undefined=slab_cache_init
    --undefined=slab_cache_shrink
    --undefined=slab_alloc
    --undefined=slab_free
    # page.c port (buddy allocator)
    --undefined=__page_alloc
    --undefined=__page_free
    --undefined=page_alloc
    --undefined=page_free
    --undefined=__pa_to_page
    --undefined=__page_to_pa
    # sysmm.c port (mm syscalls)
    --undefined=sys_mmap
    --undefined=sys_munmap
    --undefined=sys_mprotect
    --undefined=sys_mremap
    --undefined=sys_msync
    --undefined=sys_mincore
    --undefined=sys_madvise
    # kalloc.c port
    --undefined=kinit
    --undefined=kalloc
    --undefined=kfree
    --undefined=kmm_alloc
    --undefined=kmm_free
    # early_allocator.c port
    --undefined=early_allocator_init
    --undefined=early_alloc_align
    # pcache.c port
    --undefined=pcache_global_init
    --undefined=pcache_init
    --undefined=pcache_teardown
    --undefined=pcache_get_page
    --undefined=pcache_put_page
    --undefined=pcache_flush
    --undefined=pcache_read_page
    --undefined=pcache_mark_page_dirty
    --undefined=pcache_discard_blk
    --undefined=sys_sync
    --undefined=sys_dumppcache
    # vm.c V1 port (pagetable primitives)
    --undefined=kvmmap
    --undefined=kvminit
    --undefined=kvminithart
    # vm.c V2 first slice (rb-tree cmp + cpumask + remote sfence)
    --undefined=vm_cpu_online
    --undefined=vm_cpu_offline
    # vm.c V2 second slice (slab pool inits, vma alloc/free, trapframe map)
    # vm.c V3 first slice (flag converters + refcount)
    --undefined=vm_dup
    # vm.c V3 second slice (lock wrappers)
    --undefined=vm_rlock
    --undefined=vm_runlock
    --undefined=vm_wlock
    --undefined=vm_wunlock
    # vm.c V3 third slice (lifecycle)
    --undefined=vm_put
    # vm.c V3 fourth slice (vm_init)
    --undefined=vm_init
    # vm.c V3 fifth slice
    --undefined=vm_find_area
    --undefined=either_copyin
    --undefined=either_copyout
    # vm.c V4 first slice
    # vm.c V4 second slice
    # vm.c V4 third slice
    # vm.c V4 d/e/f slices
    --undefined=vma_validate
    # vm.c V5: copy/copyin/copyout/copyinstr, heap/stack create/grow,
    # mmap region helpers, mprotect/mremap/msync/mincore/madvise,
    # thread stack alloc/free, top-level mmap/munmap, vm_copy.
    --undefined=vm_copy
    --undefined=vm_copyout
    --undefined=vm_copyin
    --undefined=vm_copyinstr
    --undefined=vm_try_growstack
    --undefined=vm_growheap
    --undefined=vm_mmap_region_locked
    # vm.c eliminated — residual policy lives in Rust
    # rcu.c port
    --undefined=rcu_init
    --undefined=rcu_cpu_init
    --undefined=rcu_read_lock
    --undefined=rcu_read_unlock
    --undefined=rcu_check_callbacks
    --undefined=call_rcu
    --undefined=rcu_kthread_start_cpu
    # bintree.c port
    --undefined=rb_first_node
    --undefined=rb_last_node
    --undefined=rb_next_node
    --undefined=rb_find_key_rup
    --undefined=rb_find_key_rdown
    # rbtree.c port
    --undefined=rb_insert_color
    --undefined=rb_delete_node_color
    # hlist.c port
    --undefined=hlist_init
    --undefined=hlist_get
    --undefined=hlist_put
    --undefined=hlist_pop
    --undefined=hlist_len
    --undefined=hlist_get_rcu
    --undefined=hlist_put_rcu
    --undefined=hlist_pop_rcu
    # kobject.c port
    --undefined=kobject_global_init
    --undefined=kobject_init
    --undefined=kobject_get
    --undefined=kobject_try_get
    --undefined=kobject_put
    --undefined=kobject_refcount
    # string.c port
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
    # sbi.c port
    --undefined=sbi_send_ipi
    --undefined=sbi_probe_extensions
    --undefined=sbi_start_secondary_harts
    # start.c port (entry.S's `call start` target + per-hart boot stack)
    --undefined=start
    --undefined=stack0
    # uart.c port (16550/PXA UART driver)
    --undefined=__uart0_mmio_base
    --undefined=__uart0_irqno
    --undefined=__uart0_clock
    --undefined=__uart0_baud
    --undefined=__uart0_reg_shift
    --undefined=__uart0_reg_io_width
    --undefined=uartinit
    --undefined=uartputc
    --undefined=uartputs
    --undefined=uartputs_nb
    --undefined=uart_tx_wait
    --undefined=uartputc_sync
    --undefined=uartstart
    --undefined=uartgetc
    --undefined=uartgets
    --undefined=uartintr
    # console.c port
    --undefined=consputc
    --undefined=consputs
    --undefined=consolewrite
    --undefined=consoleread
    --undefined=consoleintr
    --undefined=consoleinit
    --undefined=consoledevinit
    # printf.c port (printf() itself stays in printf_shim.c, calling
    # into printf_rust below)
    --undefined=printf_rust
    --undefined=panic_state
    --undefined=panic_disable_bt
    --undefined=__panic_start
    --undefined=trigger_panic
    --undefined=__panic_end
    --undefined=printfinit
    --undefined=panic_msg_lock
    --undefined=panic_msg_unlock
    --undefined=puts
    --undefined=putchar
    # backtrace.c port
    --undefined=ksymbols_init
    --undefined=print_backtrace
    --undefined=print_thread_backtrace
    --undefined=bt_search
    --undefined=db_break
    # irq/plic.c port. P3-1B mesh sweep (2026-07-13): `plicinit`/
    # `plicinithart`/`plic_claim`/`plic_complete`/`plic_enable_irq` are all
    # called only from other in-scope files via direct crate-path `use`
    # (`start_kernel.rs`, `irq/irq_core.rs`) -- demoted to `pub(crate)`,
    # flags removed. `__plic_mmio_base` keeps its flag: still written by
    # `dev/fdt.rs` (out of scope) and read via `extern` by
    # `mm/vm_pgtab.rs` (out of scope).
    --undefined=__plic_mmio_base
    # irq/irq.c port. P3-1B: `irq_desc_init`/`do_irq` are called only from
    # other in-scope files via direct crate-path `use` (`start_kernel.rs`,
    # `irq/trap.rs`) -- demoted, flags removed. `unregister_irq_handler`
    # has zero callers anywhere (matches the C original) -- demoted with
    # `#[allow(dead_code)]`, flag removed. `register_irq_handler` keeps
    # its flag: still called via `extern` by out-of-scope files
    # (`timer/timer_core.rs`, `timer/goldfish_rtc.rs`, `console.rs`,
    # `dev/x1_emac.rs`, `e1000.rs`, `virtio_disk.rs`).
    --undefined=register_irq_handler
    # irq/trap.c port (Phase 2 Wave 6). P3-1B mesh sweep: `trapinit`/
    # `trapinithart` (called from `start_kernel.rs`), `usertrapret`
    # (called from `proc/{clone,thread}.rs`), `push_sigframe`/
    # `restore_sigframe` (called from `proc/signal.rs`, via a typed thin
    # wrapper for the ucontext-mirror-type mismatch), and `enter_irq`/
    # `exit_irq` (no caller outside this file -- `kernel_irq`/
    # `user_kirq_entrance`, both below) are now demoted to `pub(crate)`,
    # all referenced via direct crate-path `use`/wrappers rather than
    # `extern` redeclarations -- flags removed. `kerneltrap`/`kernel_irq`/
    # `usertrap`/`user_kirq_entrance` stay: the asm contract
    # (kernelvec.S's `call kerneltrap`/`call kernel_irq`,
    # trampoline.S's `.dword usertrap`/`.dword user_kirq_entrance`)
    # requires these exact link names.
    --undefined=kerneltrap
    --undefined=kernel_irq
    --undefined=usertrap
    --undefined=user_kirq_entrance
    # irq/syscall.c port (Phase 2 Wave 7). P3-1B: `syscall` is called only
    # from `irq/trap.rs` (direct crate-path `use`) -- demoted, flag
    # removed. `fetchaddr`/`fetchstr`/`argraw`/`argint`/`argint64`/
    # `argaddr`/`argstr` keep their flags: still called via `extern` by
    # out-of-scope files (`mm/sysmm.rs`, `mm/page.rs`,
    # `vfs/vfs_syscall.rs`).
    --undefined=fetchaddr
    --undefined=fetchstr
    --undefined=argraw
    --undefined=argint
    --undefined=argint64
    --undefined=argaddr
    --undefined=argstr
    # timer/timer.c port (Phase 2 Wave 8) -- timer wheel core.
    # P3-1B mesh sweep (2026-07-13): `__clint_timer_irqno` had zero readers
    # outside this file (grep-verified) -- demoted to `pub(crate)`, flag
    # removed. `timer_node_init`/`timer_add`/`timer_remove`/`timer_tick`/
    # `timer_init` are called only from `timer/sched_timer.rs` via a direct
    # crate-path `use` (already mesh-clean, no `extern` block existed) --
    # demoted to `pub(crate)`, flags removed; ordinary same-crate
    # reachability from `sched_timer_init`/`__do_timer_tick` (both still
    # forced below) keeps them linked in. `get_jiffs`/`__timebase_frequency`
    # keep their flags: still read via `extern` by out-of-scope files
    # (`mm/pcache.rs`, `lock/rcu_test.rs`, `lock/spinlock.rs`,
    # `machine/machine.rs`). `__jiff_ticks` is still written by `dev/fdt.rs`
    # and `start.rs` (both out of scope), which already creates a real
    # undefined-symbol demand -- no flag needed for it either (unchanged
    # from before this wave).
    --undefined=__timebase_frequency
    --undefined=get_jiffs
    # timer/sched_timer.c port (Phase 2 Wave 8) -- scheduler-tick consumer
    # + sleep/timeout API. P3-1B mesh sweep: `sched_timer_tick`/
    # `__do_timer_tick`/`sched_timer_init` (called from `timer_core.rs`/
    # `proc/sched.rs`/`start_kernel.rs`, all direct crate-path `use`) and
    # `sched_timer_add_deadline`/`sched_timer_add` (zero callers anywhere,
    # `#[allow(dead_code)]`) are demoted, flags removed.
    # `sleep_ms_interruptible` is called only from `proc/sysproc.rs`
    # (direct crate-path `use`) -- demoted, flag removed. `sched_timer_set`/
    # `sched_timer_done`/`sleep_ms` keep their flags: still called via
    # `extern` by out-of-scope files (`lock/{mutex,rwsem,completion,
    # semaphore}.rs`, `console.rs`, `dev/*.rs`, `vfs/vfs_syscall.rs`, ...).
    --undefined=sched_timer_set
    --undefined=sched_timer_done
    --undefined=sleep_ms
    # timer/goldfish_rtc.c port (Phase 2 Wave 8) -- RTC MMIO driver.
    # P3-1B mesh sweep: `__goldfish_rtc_irqno` (zero readers outside this
    # file), `goldfish_rtc_read_ns`/`_read_sec`/`_set_alarm_ns`/
    # `_set_alarm_sec`/`_clear_alarm`/`_irq_enable`/`_get_alarm_count`
    # (called only from `proc/sysproc.rs`/internally) and
    # `goldfish_rtc_init` (no live caller at all, `#[allow(dead_code)]`,
    # see this file's module doc) are demoted, flags removed.
    # `__goldfish_rtc_mmio_base` keeps its flag: still read via `extern`
    # by `mm/vm_pgtab.rs` (out of scope).
    --undefined=__goldfish_rtc_mmio_base
    # ipi/ipi.c port (Phase 2 Wave 9) -- IPI send/receive + per-CPU
    # bring-up bookkeeping. P3-1B mesh sweep: `ipi_init`/`cpus_init`/
    # `mycpu_init` (called from `start_kernel.rs`, direct crate-path
    # `use`), `ipi_send_single` (called from `proc/{sched,signal,
    # thread_group}.rs`, direct crate-path `use`), `ipi_send_mask`/
    # `ipi_send_all_but_self` (no caller outside this file) and
    # `get_cpu_active_mask` (zero callers anywhere, `#[allow(dead_code)]`)
    # are demoted, flags removed. `ipi_send_all` keeps its flag: still
    # called via `extern` by `printf.rs` (out of scope). `cpus` (the
    # per-CPU array) keeps its flag: still read via `extern` by
    # `lock/rcu.rs`/`mm/vm_pgtab.rs` (out of scope; `proc/rq.rs`/
    # `irq/trap.rs`, in scope, still read it via their own typed `extern`
    # too -- left untouched, seeing no benefit to converting a read of an
    # already-forced, already-public static purely for mesh-cosmetic
    # reasons).
    --undefined=ipi_send_all
    --undefined=cpus
    # tty/termios.c + tty/tty_dev.c + tty/pty.c port (Phase 2 Wave 10).
    # `termios_init_default`/`ttydevinit`/`pty_alloc`/`pty_master_read`/
    # `pty_master_write` are all called from tty.c/ptmx.c/start_kernel.c's
    # Rust successors (tty/tty.rs, tty/ptmx.rs, kernel/start_kernel.rs --
    # same-crate calls, no longer automatic linker demand); `pty_dealloc` has no live
    # caller anywhere (dead code carried over unchanged from the C
    # original, whose only reference was its own declaration too).
    # Forced anyway per the plan's "every new public symbol" rule.
    --undefined=termios_init_default
    --undefined=ttydevinit
    --undefined=pty_alloc
    --undefined=pty_dealloc
    --undefined=pty_master_read
    --undefined=pty_master_write
    # tty/tty.c + tty/ptmx.c port (Phase 2 Wave 11). tty_* are called
    # from console.rs/tty_dev.rs/pty.rs (still-live extern "C" call
    # sites) and from ptmx.rs (same-crate direct Rust calls); tty_init/
    # ptmxinit are called from kernel/start_kernel.rs (same-crate Rust
    # since Wave 27). Forced anyway per the plan's "every new public
    # symbol" rule.
    --undefined=tty_init
    --undefined=tty_alloc
    --undefined=tty_free
    --undefined=tty_ref
    --undefined=tty_unref
    --undefined=tty_input
    --undefined=tty_read
    --undefined=tty_poll
    --undefined=tty_write
    --undefined=tty_output
    --undefined=tty_ioctl
    --undefined=tty_open
    --undefined=tty_close
    --undefined=tty_hangup
    --undefined=ptmxinit
    # tty/session.c port (Phase 2 Wave 12) -- session lifecycle,
    # controlling-tty management, setsid/getsid. `session_init` is
    # called from `kernel/start_kernel.rs` (same-crate Rust since Wave
    # 27)/`proc/thread.rs`; the rest are called only from other Rust files' own
    # `extern` declarations (`console.rs`, `proc/{clone,exit,pgroup,
    # pid,sysproc,thread}.rs`, `tty/{tty,tty_dev}.rs`) -- without these
    # flags the linker could drop the whole module. `session_hangup` has
    # no live caller anywhere yet (matches the C original -- see
    # session.rs's module doc); forced anyway per the plan's "every new
    # public symbol" rule. `session_list` is the global session list
    # static, read via `proc/proc_shims.rs`'s own typed `extern`
    # declaration of the same symbol (same precedent as `cpus` in the
    # ipi.c port).
    --undefined=session_init
    --undefined=session_alloc
    --undefined=session_ref
    --undefined=session_unref
    --undefined=session_add_thread
    --undefined=session_remove_thread
    --undefined=session_add_pg
    --undefined=session_remove_pg
    --undefined=session_set_ctrl_tty
    --undefined=session_get_ctrl_tty
    --undefined=session_set_fg_pgid
    --undefined=session_get_fg_pgid
    --undefined=session_hangup
    --undefined=session_setsid
    --undefined=session_getsid
    --undefined=get_session
    --undefined=session_list
    # vfs/inode.c port (Phase 2 Wave 13) -- inode cache/refcount/locking
    # core + vfs_namei path resolution. All 30 symbols are called from
    # other C translation units still in kernel/vfs/ (fs.c, file.c,
    # vfs_syscall.c, tmpfs/, xv6fs/, devtmpfs/) or from Rust
    # (proc/thread.rs's install_user_root uses vfs_iput/vfs_namei via its
    # own local extern declarations) -- without these flags the linker
    # could drop the whole object file from the static archive.
    --undefined=__vfs_inode_init
    --undefined=vfs_ilock
    --undefined=vfs_ilock_trylock
    --undefined=vfs_iunlock
    --undefined=vfs_inode_check_valid
    --undefined=vfs_idup
    --undefined=vfs_idup_not_zero
    --undefined=vfs_iput
    --undefined=vfs_dirty_inode
    --undefined=vfs_sync_inode
    --undefined=vfs_ilookup
    --undefined=vfs_dir_iter
    --undefined=vfs_dir_isempty
    --undefined=vfs_readlink
    --undefined=vfs_create
    --undefined=vfs_mknod
    --undefined=vfs_link
    --undefined=vfs_unlink
    --undefined=vfs_mkdir
    --undefined=vfs_move
    --undefined=vfs_symlink
    --undefined=vfs_itruncate
    --undefined=vfs_ilock_two_nondirectories
    --undefined=vfs_ilock_two_directories
    --undefined=vfs_iunlock_two
    --undefined=vfs_chdir
    --undefined=vfs_chroot
    --undefined=vfs_curdir
    --undefined=vfs_curroot
    --undefined=vfs_namei
    --undefined=vfs_nameiparent
    # vfs/file.c + vfs/pipe.c port (Phase 2 Wave 14) -- vfs_file
    # lifecycle/dispatch and the pipe ring buffer. Called from other C
    # translation units still in kernel/vfs/ (fs.c, vfs_syscall.c,
    # tmpfs/, xv6fs/, devtmpfs/), from kernel/vfs/fdtable.rs, from
    # kernel/mm/vm.rs (vfs_fput/vfs_fdup), from kernel/exec.rs, and
    # (pipe_*) from kernel/tty/tty.rs, kernel/tty/pty.rs,
    # kernel/console.rs, and kernel/start_kernel.rs (pipe_init,
    # same-crate Rust since Wave 27) --
    # without these flags the
    # linker could drop the whole object file from the static archive.
    --undefined=__vfs_file_init
    --undefined=__vfs_file_shrink_cache
    --undefined=vfs_fileopen
    --undefined=vfs_fput
    --undefined=vfs_fdup
    --undefined=vfs_ioctl
    --undefined=vfs_fileread
    --undefined=vfs_filestat
    --undefined=vfs_filewrite
    --undefined=vfs_filelseek
    --undefined=truncate
    --undefined=vfs_pipealloc
    --undefined=vfs_custom_fd_alloc
    --undefined=vfs_sockalloc
    --undefined=pipe_init
    --undefined=pipe_alloc
    --undefined=pipe_close
    --undefined=pipe_set_flags
    --undefined=pipe_get_flags
    --undefined=pipe_read
    --undefined=pipe_write
    --undefined=pipe_open
    # vfs/fdtable.c port (Phase 2 Wave 15) -- per-thread fd table
    # (alloc/dealloc/clone/put, RCU-protected lookup, close-on-exec).
    # Called from other C translation units still in kernel/vfs/ (fs.c,
    # vfs_syscall.c) and kernel/exec.rs, and from kernel/proc/{clone,exit}.rs
    # / kernel/mm/vm.rs / kernel/vfs/file.rs via their own local extern
    # declarations -- without these flags the linker could drop the whole
    # object file from the static archive.
    --undefined=__vfs_fdtable_global_init
    --undefined=vfs_fdtable_alloc_fd_from
    --undefined=vfs_fdtable_alloc_fd
    --undefined=vfs_fdtable_init
    --undefined=vfs_fdtable_clone
    --undefined=vfs_fdtable_put
    --undefined=vfs_fdtable_get_file
    --undefined=vfs_fdtable_dealloc_fd
    --undefined=vfs_fdtable_get_fdflags
    --undefined=vfs_fdtable_set_fdflags
    --undefined=vfs_fdtable_close_on_exec
    # vfs/fs.c port (Phase 2 Wave 16) -- filesystem-type registry,
    # mount/unmount state machine, superblock lock/refcount/mountcount
    # API (the `vfs_superblock_{r,w}lock` pair `inode.rs` already calls
    # as an extern), per-superblock inode hash cache, dentry-to-inode
    # resolution, and `fs_struct` lifecycle. Called from other C
    # translation units still in kernel/vfs/ (vfs_syscall.c, tmpfs/,
    # xv6fs/, devtmpfs/), from kernel/start_kernel.rs (vfs_init,
    # same-crate Rust since Wave 27), and from
    # kernel/proc/{thread,clone,exit}.rs via their own local extern
    # declarations -- without these flags the linker could drop the whole
    # object file from the static archive. `tmpfs_smoketest_start`/
    # `__tmpfs_smoketest_kthread` were not carried over (dead code: zero
    # live callers, the C original's own call site was already commented
    # out -- see fs.rs's module doc).
    --undefined=vfs_init
    --undefined=vfs_get_deferred_iput_wq
    --undefined=__vfs_shrink_caches
    --undefined=vfs_fs_type_allocate
    --undefined=vfs_fs_type_free
    --undefined=vfs_register_fs_type
    --undefined=vfs_unregister_fs_type
    --undefined=vfs_mount_lock
    --undefined=vfs_mount_unlock
    --undefined=vfs_mount
    --undefined=vfs_unmount
    --undefined=vfs_unmount_lazy
    --undefined=vfs_make_orphan
    --undefined=__vfs_final_unmount_cleanup
    --undefined=vfs_get_mnt_rooti
    --undefined=vfs_superblock_rlock
    --undefined=vfs_superblock_wlock
    --undefined=vfs_superblock_wholding
    --undefined=vfs_superblock_unlock
    --undefined=vfs_superblock_spin_lock
    --undefined=vfs_superblock_spin_unlock
    --undefined=vfs_superblock_mountcount_inc
    --undefined=vfs_superblock_mountcount_dec
    --undefined=vfs_superblock_dup
    --undefined=vfs_superblock_put
    --undefined=vfs_alloc_inode
    --undefined=vfs_get_inode
    --undefined=vfs_sync_superblock
    --undefined=vfs_get_fs_type
    --undefined=vfs_put_fs_type
    --undefined=vfs_get_inode_cached
    --undefined=vfs_add_inode
    --undefined=vfs_remove_inode
    --undefined=vfs_release_dentry
    --undefined=vfs_get_dentry_inode_locked
    --undefined=vfs_get_dentry_inode
    --undefined=vfs_struct_init
    --undefined=vfs_struct_clone
    --undefined=vfs_struct_put
    --undefined=vfs_inode_get_ref
    --undefined=vfs_inode_put_ref
    --undefined=vfs_inode_deref
    --undefined=vfs_dump_inodes
    --undefined=vfs_dump_sb_inodes
    --undefined=vfs_root_inode
    # vfs/vfs_syscall.c port (Phase 2 Wave 17) -- every VFS-facing
    # syscall entry point (referenced only from the Wave-7 Rust
    # dispatch table, kernel/irq/syscall.rs's own extern block) plus
    # the two kernel-internal mount/unmount path helpers
    # (vfs_mount_path/vfs_umount_path) called from kernel/vfs/fs.rs's
    # vfs_init() via its own local extern declaration -- without these
    # flags the linker could drop the whole object file from the
    # static archive.
    --undefined=sys_vfs_dup
    --undefined=sys_vfs_dup2
    --undefined=sys_vfs_read
    --undefined=sys_vfs_write
    --undefined=sys_vfs_close
    --undefined=sys_vfs_fstat
    --undefined=sys_vfs_lseek
    --undefined=sys_vfs_ftruncate
    --undefined=sys_vfs_fcntl
    --undefined=sys_vfs_stat
    --undefined=sys_vfs_lstat
    --undefined=sys_vfs_access
    --undefined=sys_vfs_readlink
    --undefined=sys_vfs_rename
    --undefined=sys_vfs_open
    --undefined=sys_vfs_mkdir
    --undefined=sys_vfs_mknod
    --undefined=sys_vfs_unlink
    --undefined=sys_vfs_link
    --undefined=sys_vfs_symlink
    --undefined=sys_vfs_chdir
    --undefined=sys_getcwd
    --undefined=sys_vfs_pipe
    --undefined=sys_vfs_connect
    --undefined=sys_getdents
    --undefined=sys_chroot
    --undefined=vfs_mount_path
    --undefined=sys_mount
    --undefined=vfs_umount_path
    --undefined=sys_umount
    --undefined=sys_dumpinode
    --undefined=sys_vfs_ioctl
    --undefined=sys_tcgetattr
    --undefined=sys_tcsetattr
    --undefined=sys_statfs
    --undefined=sys_vfs_poll
    # vfs/tmpfs/{superblock,inode,truncate,file}.c port (Phase 2 Wave 18)
    # -- the first filesystem *driver* (as opposed to vfs core) to move
    # to Rust. `tmpfs_init`/`tmpfs_mount_root` are called from
    # `vfs/fs.rs`'s `vfs_init()` (real demand already); `tmpfs_alloc_inode`/
    # `tmpfs_get_inode`/`tmpfs_sync_fs`/`tmpfs_unmount_begin`/
    # `tmpfs_alloc_superblock`/`tmpfs_free`/`tmpfs_make_directory` are
    # called from `kernel/vfs/devtmpfs/superblock.c` (still C, real
    # demand already, via its own `#include "../tmpfs/tmpfs_private.h"`);
    # the rest (`tmpfs_free_inode`, `tmpfs_free_symlink_target`,
    # `tmpfs_shrink_caches`, `__tmpfs_truncate`,
    # `__tmpfs_migrate_to_allocated_blocks`, `tmpfs_inode_pcache_init`,
    # `tmpfs_inode_pcache_teardown`, `tmpfs_open`) are reachable today only
    # through this crate's own `tmpfs_inode_ops`/`tmpfs_file_ops` vtable
    # data (address-of relocations, not call instructions) or from a
    # sibling tmpfs submodule -- forced anyway per the plan's "every new
    # public symbol" rule, and because every one of them is also declared
    # `extern` in `tmpfs_private.h` (kept as an honest C-ABI contract for
    # Wave 20's devtmpfs work; see kernel/vfs/tmpfs/mod.rs's module doc).
    --undefined=tmpfs_init
    --undefined=tmpfs_mount_root
    --undefined=tmpfs_alloc_inode
    --undefined=tmpfs_free_inode
    --undefined=tmpfs_alloc_superblock
    --undefined=tmpfs_free
    --undefined=tmpfs_get_inode
    --undefined=tmpfs_sync_fs
    --undefined=tmpfs_unmount_begin
    --undefined=tmpfs_shrink_caches
    --undefined=tmpfs_make_directory
    --undefined=tmpfs_free_symlink_target
    --undefined=__tmpfs_truncate
    --undefined=__tmpfs_migrate_to_allocated_blocks
    --undefined=tmpfs_inode_pcache_init
    --undefined=tmpfs_inode_pcache_teardown
    --undefined=tmpfs_open
    # vfs/xv6fs/*.c port (Phase 2 Wave 19) -- the root filesystem, ported
    # in two sub-waves (see the RUST_CRATE_SOURCES comment above).
    # `xv6fs_init`/`xv6fs_mount_root` are called from `vfs/fs.rs`'s
    # `vfs_init()`/`kernel/start_kernel.rs` (same-crate Rust since Wave
    # 27). Every symbol
    # below is also declared `extern` in `xv6fs_private.h`/
    # `block_cache.h` (unchanged C-ABI contracts), so all are forced per
    # the plan's "every new public symbol" rule even where today's only
    # caller is a sibling xv6fs submodule (reached via a plain Rust-path
    # call, not a `call`/relocation the linker would otherwise see from
    # this force-list's own vantage point until something upstream roots
    # it -- `xv6fs_init`/`xv6fs_mount_root` are that root).
    # Sub-wave A (superblock.c + inode.c):
    --undefined=xv6fs_init
    --undefined=xv6fs_mount_root
    --undefined=xv6fs_alloc_inode
    --undefined=xv6fs_get_inode
    --undefined=xv6fs_sync_fs
    --undefined=xv6fs_unmount_begin
    --undefined=xv6fs_free
    --undefined=xv6fs_shrink_caches
    --undefined=xv6fs_inode_pcache_init
    --undefined=xv6fs_iupdate
    --undefined=xv6fs_sync_inode
    --undefined=xv6fs_dirty_inode
    --undefined=xv6fs_destroy_inode
    --undefined=xv6fs_free_inode
    --undefined=xv6fs_open
    --undefined=__xv6fs_inode_cache
    # Sub-wave B (truncate.c + file.c + log.c + block_cache.c):
    --undefined=xv6fs_bmap
    --undefined=xv6fs_bmap_read
    --undefined=xv6fs_itrunc
    --undefined=xv6fs_truncate
    --undefined=xv6fs_initlog
    --undefined=xv6fs_begin_op
    --undefined=xv6fs_begin_op_nointr
    --undefined=xv6fs_end_op
    --undefined=xv6fs_log_write
    --undefined=xv6fs_bcache_init
    --undefined=xv6fs_bcache_destroy
    --undefined=xv6fs_bcache_mark_free
    --undefined=xv6fs_bcache_find_free_block
    --undefined=xv6fs_bcache_find_free_block_near
    --undefined=xv6fs_bcache_free_count
    # vfs/devtmpfs/superblock.c port (Phase 2 Wave 20) -- the last C file
    # in kernel/vfs/. `devtmpfs_init`/`devtmpfs_post_mount_populate` are
    # called from `vfs/fs.rs`'s `vfs_init()` (real demand already);
    # `devtmpfs_create_node`/`devtmpfs_remove_node`/
    # `devtmpfs_populate_devices` are declared `extern` in the still-C
    # `kernel/inc/devtmpfs.h` (`devtmpfs_create_node`/`_remove_node` are
    # called from `kernel/dev/dev.rs`, ported to Rust in Wave 21 below;
    # `devtmpfs_populate_devices` only self-called today but forced
    # anyway per the plan's "every new public symbol declared in a
    # still-live header" rule, same precedent as tmpfs's/xv6fs's own
    # force-list entries above).
    --undefined=devtmpfs_init
    --undefined=devtmpfs_post_mount_populate
    --undefined=devtmpfs_create_node
    --undefined=devtmpfs_remove_node
    --undefined=devtmpfs_populate_devices
    # dev/dev.c + cdev.c + blkdev.c port (Phase 2 Wave 21) -- the
    # kobject-backed, RCU-protected major/minor device table
    # (kernel/dev/dev.rs) plus the character/block dispatch layers on top
    # of it (kernel/dev/cdev.rs, kernel/dev/blkdev.rs). Every symbol below
    # is declared `extern` in the still-live, unchanged C headers
    # `kernel/inc/dev/{dev,cdev,blkdev}.h` and called from still-C
    # drivers/callers (console.c-successor console.rs, tty_dev.rs,
    # ptmx.rs, nullrand.c-successor nullrand.rs, virtio_disk.c,
    # ramdisk.c, x1_sdhci.c-successor x1_sdhci.rs -- Wave 25), so all are
    # forced per the plan's "every new public symbol" rule. (`dev_test.c`,
    # formerly also a caller, was dead code -- deleted in Wave 24, see
    # that wave's entry below.)
    --undefined=dev_table_init
    --undefined=device_get
    --undefined=device_dup
    --undefined=device_put
    --undefined=device_register
    --undefined=device_unregister
    --undefined=dev_ioctl
    --undefined=dev_for_each_device
    --undefined=cdev_get
    --undefined=cdev_dup
    --undefined=cdev_put
    --undefined=cdev_register
    --undefined=cdev_unregister
    --undefined=cdev_read
    --undefined=cdev_write
    --undefined=blkdev_get
    --undefined=blkdev_dup
    --undefined=blkdev_put
    --undefined=blkdev_register
    --undefined=blkdev_unregister
    --undefined=blkdev_submit_bio
    # dev/bio.c port (Phase 2 Wave 22) -- the Linux-style ref-counted
    # block-I/O request object (kernel/dev/bio.rs). Declared `extern` in
    # the still-live, unchanged `kernel/inc/dev/bio.h` and called from
    # `kernel/dev/blkdev.rs` (bio_validate), `kernel/bufcache.rs`
    # (bio_alloc/bio_add_seg/bio_release), still-C drivers (virtio_disk.c,
    # ramdisk.c), and `kernel/dev/x1_sdhci.rs` (Wave 25, a same-crate Rust
    # call now, but `bio_validate` is reached via `blkdev.rs` -- forced
    # per the plan's "every new public symbol" rule regardless, since
    # virtio_disk.c/ramdisk.c are still-C callers of this same symbol set.
    # `bio_dup` has no live caller today
    # (forced anyway, same precedent as `devtmpfs_populate_devices`
    # above -- it is part of the header's public contract).
    --undefined=bio_alloc
    --undefined=bio_add_seg
    --undefined=bio_dup
    --undefined=bio_release
    --undefined=bio_validate
    # bio.c port (Phase 2 Wave 22) -- the classic xv6 struct buf
    # disk-block cache (kernel/bufcache.rs; not kernel/dev/bio.rs above
    # -- see that file's module doc). Declared `extern` in the
    # still-live, unchanged `kernel/inc/defs.h` and called from
    # `kernel/vfs/xv6fs/*.rs` (bread/brelse/bwrite/bwrite_async/bsync/
    # bpin/bunpin) and `kernel/start_kernel.rs` (binit, same-crate Rust
    # since Wave 27) -- forced per the
    # plan's "every new public symbol" rule. `bdirty_count` has no live
    # caller today (forced anyway, same precedent as `bio_dup` above).
    --undefined=binit
    --undefined=bread
    --undefined=bwrite
    --undefined=bwrite_async
    --undefined=bsync
    --undefined=brelse
    --undefined=bpin
    --undefined=bunpin
    # dev/fdt.c port (Phase 2 Wave 23) -- the FDT parser + platform-config
    # applier (kernel/dev/fdt.rs). `fdt_early_scan_memory`/`fdt_init`/
    # `fdt_apply_platform_config` are called from `kernel/start_kernel.rs`
    # (same-crate Rust since Wave 27). The rest (`fdt_dump`/`fdt_walk`/
    # `fdt_node_lookup`/`fdt_path_lookup`/`fdt_compat_lookup`/
    # `fdt_compat_next`/`fdt_phandle_lookup`) have no live caller today
    # (the one potential call site, `fdt_walk(fdt_base)` in
    # `kernel/start_kernel.rs`, is commented out) but stay declared `extern` in
    # the still-live, unchanged `kernel/inc/dev/fdt.h` -- forced anyway,
    # same precedent as `bio_dup`/`devtmpfs_populate_devices` above (part
    # of the header's public contract). `fdt_valid`/`fdt_totalsize` are
    # declared in the header too but were never real C symbols (only
    # same-named macros local to the old `fdt.c` translation unit,
    # verified by grep) -- not forced, since nothing was ever linkable
    # there to preserve.
    --undefined=fdt_early_scan_memory
    --undefined=fdt_init
    --undefined=fdt_apply_platform_config
    --undefined=fdt_dump
    --undefined=fdt_walk
    --undefined=fdt_node_lookup
    --undefined=fdt_path_lookup
    --undefined=fdt_compat_lookup
    --undefined=fdt_compat_next
    --undefined=fdt_phandle_lookup
    --undefined=platform
    # dev/nullrand.c + dev/netdev.c port (Phase 2 Wave 24).
    # `nullranddevinit` is called from `kernel/start_kernel.rs`
    # (same-crate Rust since Wave 27); `random_fill_bytes` is called from
    # `kernel/proc/sysproc.rs`'s `sys_getrandom` (same-crate Rust call,
    # forced anyway per the plan's "every new public symbol declared in
    # a still-live header" rule -- `kernel/inc/defs.h` still declares
    # it). `netdev_init` is called from `kernel/start_kernel.rs`;
    # `netdev_register`/`netdev_set_link` from still-C `kernel/e1000.c`
    # and (Wave 25 on) same-crate Rust `kernel/dev/x1_emac.rs` --
    # `e1000.c` alone already justifies forcing these; `netdev_get_default`
    # from still-C `kernel/net.c`. `netdev_get_by_index`/
    # `netdev_get_by_name`/`netdev_set_link_callback` have no live
    # caller today but stay declared `extern` in the still-live,
    # unchanged `kernel/inc/dev/netdev.h` -- forced anyway, same
    # precedent as `bio_dup`/`devtmpfs_populate_devices` above (part of
    # the header's public contract).
    --undefined=nullranddevinit
    --undefined=random_fill_bytes
    --undefined=netdev_init
    --undefined=netdev_register
    --undefined=netdev_get_default
    --undefined=netdev_get_by_index
    --undefined=netdev_get_by_name
    --undefined=netdev_set_link
    --undefined=netdev_set_link_callback
    # dev/{yt8531,x1_emac,x1_sdhci}.c port (Phase 2 Wave 25) -- the last C
    # in kernel/dev/ (kernel/dev/{yt8531,x1_emac,x1_sdhci}.rs).
    # `x1_emac_init`/`x1_sdhci_init` are called from
    # `kernel/start_kernel.rs` (same-crate Rust since Phase 2 Wave 27) --
    # forced per the plan's "every new public symbol" rule regardless.
    # `mdio_read`/`mdio_write`/`yt8531_init`/`yt8531_poll_link`/
    # `yt8531_wait_autoneg` (declared `extern` in the now C-caller-less
    # `kernel/inc/dev/yt8531.h`) are reached only via direct same-crate
    # Rust calls from `kernel/dev/x1_emac.rs` -- ordinary Rust codegen
    # reachability from the two forced entry points below already keeps
    # them linked in, so they are deliberately NOT added here (no still-C
    # caller exists to justify it, unlike the header-contract precedents
    # elsewhere in this list).
    --undefined=x1_emac_init
    --undefined=x1_sdhci_init
    # exec.c port (Phase 2 Wave 26) -- kernelvec.S/trampoline.S never
    # reference these by symbol name, and (with exec.c gone) nothing in
    # the remaining C tree references them either: both callers are
    # same-crate Rust (`kernel/proc/thread.rs`'s `init_entry` calls `exec`
    # to launch `/bin/init` at boot; `kernel/irq/syscall.rs`'s dispatch
    # table calls `sys_exec` via its own function-pointer entry) --
    # without these flags the linker could drop the whole module.
    --undefined=exec
    --undefined=sys_exec
    # start_kernel.c port (Phase 2 Wave 27) -- the boot orchestrator.
    # `start_kernel` is called only from `kernel/start.rs` (same-crate
    # Rust, `entry.S -> start() -> start_kernel()`); `start_kernel_post_init`
    # only from `kernel/proc/thread.rs`'s `init_entry`. Both forced per the
    # plan's "every new public symbol" rule, same precedent as `exec`/
    # `sys_exec` above. `__physical_memory_start`/`__physical_memory_end`/
    # `__physical_total_pages` are DATA symbols (not functions) whose sole
    # definition now lives in `kernel/start_kernel.rs`, read via `extern`
    # by a dozen other files (`printf.rs`, `ipi.rs`, `mm/vm_pgtab.rs`,
    # `mm/page.rs`, `dev/fdt.rs`, `proc/sysproc.rs`, ...) but never
    # referenced *by name* from outside the xv6_rust archive -- same class
    # of concern as the page.c port's `--undefined=__buddy_pools` /
    # `--undefined=xv6_total_managed_pages` above, forced for the same
    # reason.
    --undefined=start_kernel
    --undefined=start_kernel_post_init
    --undefined=__physical_memory_start
    --undefined=__physical_memory_end
    --undefined=__physical_total_pages
    # virtio_disk.c + ramdisk.c port (Phase 2 Wave 28, sub-wave A) --
    # the final porting wave's block drivers (kernel/virtio_disk.rs,
    # kernel/ramdisk.rs). `virtio_disk_init`/`ramdisk_init` are called
    # from `kernel/start_kernel.rs`'s `start_kernel_post_init`
    # (same-crate Rust since Wave 27); `__virtio_mmio_base`/
    # `__virtio_irqno` are DATA symbols read via `extern` by
    # `kernel/dev/fdt.rs` (`fdt_apply_platform_config`, populates them)
    # and `kernel/mm/vm_pgtab.rs`/`kernel/dev/fdt.rs`'s own `kvmmake`
    # path, but never referenced *by name* from outside the xv6_rust
    # archive -- same class of concern as `start_kernel.c`'s
    # `__physical_memory_start` above, forced for the same reason.
    --undefined=virtio_disk_init
    --undefined=ramdisk_init
    --undefined=__virtio_mmio_base
    --undefined=__virtio_irqno
    # pci.c + e1000.c + net.c + sysnet.c port (Phase 2 Wave 28, sub-wave
    # B) -- the final porting wave's network drivers
    # (kernel/pci.rs/e1000.rs/net.rs/sysnet.rs). `pci_init` is called
    # from `kernel/start_kernel.rs`'s boot-hart main init (same-crate
    # Rust since Wave 27); `sockinit` from `start_kernel_post_init`.
    # `__pcie_ecam_mmio_base` is read via `extern` by `kernel/mm/
    # vm_pgtab.rs` (`kvminit`)/`kernel/dev/fdt.rs`; `__e1000_pci_mmio_base`
    # likewise by `vm_pgtab.rs`; `__e1000_pci_irqno` has no other
    # referencer today but stays declared `extern` in the still-live
    # `kernel/inc/dev/e1000_dev.h` -- forced anyway, same precedent as
    # `bio_dup`/`devtmpfs_populate_devices` elsewhere in this list (part
    # of the header's public contract). `e1000_init` is called from
    # `kernel/pci.rs` (same-crate Rust since this same wave);
    # `e1000_transmit` only via the `netdev_ops.transmit` function
    # pointer today but stays declared `extern` in the still-live
    # `kernel/inc/defs.h`, same precedent. `net_rx`/`mbufalloc`/
    # `mbuffree` are called from still-C-turned-Rust `kernel/dev/
    # x1_emac.rs` (Wave 25) and this wave's own `e1000.rs`;
    # `net_tx_udp` from `kernel/sysnet.rs`'s `sockwrite`.
    # `mbufpull`/`mbufpush`/`mbufput`/`mbuftrim`/`mbufq_pushtail`/
    # `mbufq_pophead`/`mbufq_empty`/`mbufq_init` are declared `extern` in
    # the still-live `kernel/inc/dev/net.h` -- `mbufq_init` has a real
    # same-crate caller (`kernel/vfs/file.rs`'s `vfs_sockalloc`, Wave
    # 14), the rest are reached only via ordinary intra-file Rust
    # codegen from `net_rx`/`net_tx_udp` (already forced above) except
    # where no live caller exists at all, forced anyway per the header-
    # contract precedent. `sockclose`/`sockread`/`sockwrite` are
    # declared `extern` in `kernel/inc/defs.h` but have no live caller
    # anywhere in the tree (see `kernel/sysnet.rs`'s module doc for the
    # full investigation) -- forced anyway, same header-contract
    # precedent; `sockrecvudp` is called from `kernel/net.rs`'s
    # `net_rx_udp` (same-crate Rust since this same wave); `sock_lock`/
    # `sockets` are DATA symbols read/written via `extern` by
    # `kernel/vfs/file.rs`'s `vfs_sockalloc`.
    --undefined=pci_init
    --undefined=sockinit
    --undefined=__pcie_ecam_mmio_base
    --undefined=__e1000_pci_mmio_base
    --undefined=__e1000_pci_irqno
    --undefined=e1000_init
    --undefined=e1000_transmit
    --undefined=net_rx
    --undefined=net_tx_udp
    --undefined=mbufalloc
    --undefined=mbuffree
    --undefined=mbufpull
    --undefined=mbufpush
    --undefined=mbufput
    --undefined=mbuftrim
    --undefined=mbufq_init
    --undefined=mbufq_pushtail
    --undefined=mbufq_pophead
    --undefined=mbufq_empty
    --undefined=sockclose
    --undefined=sockread
    --undefined=sockwrite
    --undefined=sockrecvudp
    --undefined=sock_lock
    --undefined=sockets
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