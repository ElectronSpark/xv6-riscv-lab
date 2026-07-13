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
set(KERNEL_C_FILES
    ${KERNEL_DIR}/start_kernel.c
    ${KERNEL_DIR}/bio.c
    ${KERNEL_DIR}/exec.c
    ${KERNEL_DIR}/virtio_disk.c
    ${KERNEL_DIR}/ramdisk.c
    ${KERNEL_DIR}/e1000.c
    ${KERNEL_DIR}/net.c
    ${KERNEL_DIR}/pci.c
    ${KERNEL_DIR}/sysnet.c
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
    ${RUST_CRATE_DIR}/vfs/devtmpfs/*.rs)

find_program(CARGO_EXECUTABLE cargo REQUIRED)

# Pass the kernel C include search paths to bindgen (consumed by build.rs).
# Keep this in sync with the include flags in the top-level CMakeLists.txt.
set(RUST_BINDGEN_INCLUDES
    "${CMAKE_SOURCE_DIR};${KERNEL_DIR}/inc;${CMAKE_BINARY_DIR}/kernel/inc")

add_custom_command(
    OUTPUT ${RUST_STATICLIB}
    COMMAND ${CMAKE_COMMAND} -E env
            "XV6_KERNEL_INCLUDES=${RUST_BINDGEN_INCLUDES}"
            "LAB=$ENV{LAB}"
            ${CARGO_EXECUTABLE} build --${RUST_PROFILE}
            --target ${RUST_TARGET_TRIPLE}
            --target-dir ${RUST_BUILD_DIR}
            --manifest-path ${RUST_CRATE_DIR}/Cargo.toml
    DEPENDS ${RUST_CRATE_SOURCES}
            ${RUST_CRATE_DIR}/build.rs
            ${RUST_CRATE_DIR}/wrapper.h
    COMMENT "Building xv6_rust staticlib (${RUST_TARGET_TRIPLE}, ${RUST_PROFILE})"
    VERBATIM
)

add_custom_target(xv6_rust DEPENDS ${RUST_STATICLIB})

# NOTE: the semaphore/rwsem/rcu runtime test suites are now ported to Rust
# (kernel/lock/{semaphore,rwsem,rcu}_test.rs) and linked in unconditionally
# through the xv6_rust staticlib below, exactly like the old always-built
# kernel/lock/*_test.c object files. `start_kernel.c` still gates the
# *calls* to `rwsem_launch_tests()` / `semaphore_launch_tests()` behind the
# RWAD_WRITE_TEST / SEMAPHORE_RUNTIME_TEST compile definitions (see
# ENV{RWLOCK_TEST} / ENV{SEMAPHORE_TEST} below), so there is nothing left
# to conditionally append to KERNEL_C_FILES here.

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

if($ENV{LAB} STREQUAL "net")
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/e1000.c ${KERNEL_DIR}/net.c ${KERNEL_DIR}/pci.c)
endif()

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
add_subdirectory(${KERNEL_DIR}/irq)
add_subdirectory(${KERNEL_DIR}/dev)
add_subdirectory(${KERNEL_DIR}/proc)

set_source_files_properties(${KERNEL_C_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_SCAN_OBJS} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_S_FILES} PROPERTIES LANGUAGE C)

set(KERNEL_OBJS ${KERNEL_S_FILES} ${KERNEL_C_FILES} ${KERNEL_SCAN_OBJS})

# Object libraries to link
# NOTE: vfs_tmpfs is gone (Phase 2 Wave 18), vfs_xv6fs is gone (Phase 2
# Wave 19), and vfs_devtmpfs is gone too (Phase 2 Wave 20 — kernel/vfs/
# has no C sources left anywhere; see the add_subdirectory NOTE above).
set(KERNEL_OBJ_LIBS
    $<TARGET_OBJECTS:irq>
    $<TARGET_OBJECTS:dev>
    $<TARGET_OBJECTS:proc>
)

set(KERNEL_LINK_LIBS irq dev proc)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding ")

# Generate linker script from template
set(KERNEL_LD_IN ${KERNEL_DIR}/kernel.ld.in)
set(KERNEL_LD ${CMAKE_CURRENT_BINARY_DIR}/kernel.ld)
configure_file(${KERNEL_LD_IN} ${KERNEL_LD} @ONLY)

# Symbols that must be force-pulled from the Rust staticlib so the linker
# keeps the corresponding object files. Avoid dragging in Rust's
# compiler-builtins (built with medlow code model).
set(RUST_FORCE_UNDEFINED
    # bits.c port
    --undefined=__uint8_bits_count
    --undefined=__uint8_trailing_zeros
    --undefined=__uint8_leading_zeros
    --undefined=__uint8_inverse
    # slab.c port
    --undefined=slab_cache_init
    --undefined=slab_cache_create
    --undefined=slab_cache_destroy
    --undefined=slab_cache_shrink
    --undefined=slab_alloc
    --undefined=slab_free
    --undefined=slab_free_noshrink
    # page.c port (buddy allocator)
    --undefined=page_buddy_init
    --undefined=__page_alloc
    --undefined=__page_free
    --undefined=page_alloc
    --undefined=page_free
    --undefined=page_lock_acquire
    --undefined=page_lock_release
    --undefined=page_lock_assert_holding
    --undefined=page_lock_assert_unholding
    --undefined=__page_ref_inc
    --undefined=__page_ref_dec
    --undefined=page_ref_inc
    --undefined=page_ref_dec
    --undefined=page_ref_inc_unlocked
    --undefined=page_ref_dec_unlocked
    --undefined=page_refcnt
    --undefined=page_ref_count
    --undefined=__pa_to_page
    --undefined=__page_to_pa
    --undefined=managed_page_base
    --undefined=page_buddy_stat
    --undefined=__check_page_pointer_in_range
    --undefined=__buddy_pools
    --undefined=xv6_buddy_pool_count
    --undefined=xv6_pcpu_cache_count
    --undefined=xv6_buddy_pool_lock_range_all
    --undefined=xv6_buddy_pool_unlock_range_all
    --undefined=xv6_total_managed_pages
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
    --undefined=kmm_shrink_all
    --undefined=get_total_free_pages
    --undefined=slab_t_desc_alloc
    --undefined=slab_t_desc_free
    --undefined=slab_cache_t_alloc
    --undefined=slab_cache_t_free
    # early_allocator.c port
    --undefined=early_allocator_init
    --undefined=early_alloc_align
    --undefined=early_alloc
    --undefined=early_alloc_end_ptr
    # pcache.c port
    --undefined=pcache_global_init
    --undefined=pcache_init
    --undefined=pcache_teardown
    --undefined=pcache_get_page
    --undefined=pcache_put_page
    --undefined=pcache_invalidate_page
    --undefined=pcache_flush
    --undefined=pcache_sync
    --undefined=pcache_read_page
    --undefined=pcache_mark_page_dirty
    --undefined=pcache_invalidate_blk
    --undefined=pcache_discard_blk
    --undefined=pcache_shrink_caches
    --undefined=sys_sync
    --undefined=sys_dumppcache
    --undefined=dump_pcache_stats
    --undefined=dump_all_pcache_stats
    # vm.c V1 port (pagetable primitives)
    --undefined=walk
    --undefined=walkaddr
    --undefined=kvmmap
    --undefined=mappages
    --undefined=uvmunmap
    --undefined=uvmcreate
    --undefined=freewalk
    --undefined=uvmfree
    --undefined=kvminit
    --undefined=kvminithart
    --undefined=dump_pagetable
    --undefined=vm_dump_flags
    # vm.c V2 first slice (rb-tree cmp + cpumask + remote sfence)
    --undefined=__vma_cmp
    --undefined=__cma_get_key
    --undefined=vm_cpu_online
    --undefined=vm_cpu_offline
    --undefined=vm_get_cpumask
    --undefined=vm_remote_sfence
    # vm.c V2 second slice (slab pool inits, vma alloc/free, trapframe map)
    --undefined=__vma_pool_init
    --undefined=__vm_pool_init
    --undefined=__vma_alloc
    --undefined=__vma_free
    --undefined=__vma_set_free
    --undefined=__vm_unmap_trapframe
    --undefined=__vm_map_trampoline
    --undefined=__vma_copy
    # vm.c V3 first slice (flag converters + refcount)
    --undefined=vma2pte_flags
    --undefined=pte2vma_flags
    --undefined=vm_dup
    # vm.c V3 second slice (lock wrappers)
    --undefined=vm_rlock
    --undefined=vm_runlock
    --undefined=vm_wlock
    --undefined=vm_wunlock
    --undefined=vm_pgtable_lock
    --undefined=vm_pgtable_unlock
    # vm.c V3 third slice (lifecycle)
    --undefined=vm_put
    --undefined=__vm_destroy
    # vm.c V3 fourth slice (vm_init)
    --undefined=vm_init
    # vm.c V3 fifth slice
    --undefined=vm_find_area
    --undefined=either_copyin
    --undefined=either_copyout
    # vm.c V4 first slice
    --undefined=vm_find_free_range
    # vm.c V4 second slice
    --undefined=vma_split
    # vm.c V4 third slice
    --undefined=vma_merge
    # vm.c V4 d/e/f slices
    --undefined=vma_alloc
    --undefined=vma_free
    --undefined=vma_validate
    # vm.c V5: copy/copyin/copyout/copyinstr, heap/stack create/grow,
    # mmap region helpers, mprotect/mremap/msync/mincore/madvise,
    # thread stack alloc/free, top-level mmap/munmap, vm_copy.
    --undefined=vm_copy
    --undefined=vm_copyout
    --undefined=vm_copyin
    --undefined=vm_copyinstr
    --undefined=vm_createheap
    --undefined=vm_createstack
    --undefined=vm_growstack
    --undefined=vm_try_growstack
    --undefined=vm_growheap
    --undefined=vm_mmap_region_locked
    --undefined=vm_mmap_region
    --undefined=vm_munmap_region
    --undefined=vm_mprotect
    --undefined=vm_mremap
    --undefined=vm_msync
    --undefined=vm_mincore
    --undefined=vm_madvise
    --undefined=vm_alloc_thread_stack
    --undefined=vm_free_thread_stack
    --undefined=vm_mmap
    --undefined=vm_munmap
    # vm.c eliminated — residual policy lives in Rust
    --undefined=dump_vm
    --undefined=xv6_vm_rb_init_vm_tree
    --undefined=xv6_vm_validate_pte
    --undefined=xv6_vm_call_vma_fault
    # rcu.c port
    --undefined=rcu_init
    --undefined=rcu_cpu_init
    --undefined=rcu_read_lock
    --undefined=rcu_read_unlock
    --undefined=rcu_is_watching
    --undefined=rcu_note_context_switch
    --undefined=rcu_check_callbacks
    --undefined=call_rcu
    --undefined=rcu_process_callbacks
    --undefined=synchronize_rcu
    --undefined=rcu_barrier
    --undefined=synchronize_rcu_expedited
    --undefined=rcu_kthread_wakeup
    --undefined=rcu_kthread_start_cpu
    --undefined=rcu_kthread_start
    # bintree.c port
    --undefined=rb_brother
    --undefined=__rb_node_link
    --undefined=rb_first_node
    --undefined=rb_last_node
    --undefined=rb_next_node
    --undefined=rb_prev_node
    --undefined=__rb_replace_node
    --undefined=__rb_transplant
    --undefined=__rb_find_key_link
    --undefined=rb_find_key_rup
    --undefined=rb_find_key_rdown
    --undefined=rb_find_key
    --undefined=rb_insert_node
    --undefined=rb_delete_key
    --undefined=__rb_rotate_left
    --undefined=__rb_rotate_right
    # rbtree.c port
    --undefined=rb_insert_color
    --undefined=rb_delete_node_color
    --undefined=rb_delete_key_color
    # hlist.c port
    --undefined=hlist_node_in_list
    --undefined=hlist_init
    --undefined=hlist_get_node_hash
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
    --undefined=kobject_count
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
    --undefined=strcat
    --undefined=strtok_r
    --undefined=strtok
    --undefined=strstr
    --undefined=strndup
    --undefined=strdup
    # sbi.c port
    --undefined=sbi_get_spec_version
    --undefined=sbi_get_impl_id
    --undefined=sbi_get_impl_version
    --undefined=sbi_probe_extension
    --undefined=sbi_get_mvendorid
    --undefined=sbi_get_marchid
    --undefined=sbi_get_mimpid
    --undefined=sbi_set_timer
    --undefined=sbi_send_ipi
    --undefined=sbi_remote_hfence_i
    --undefined=sbi_remote_hfence_vma
    --undefined=sbi_remote_hfence_vma_asid
    --undefined=sbi_remote_hfence_gvma_vmid
    --undefined=sbi_remote_hfence_gvma
    --undefined=sbi_remote_hfence_vvma_asid
    --undefined=sbi_remote_hfence_vvma
    --undefined=sbi_hart_start
    --undefined=sbi_hart_stop
    --undefined=sbi_hart_get_status
    --undefined=sbi_hart_suspend
    --undefined=sbi_system_reset
    --undefined=sbi_shutdown
    --undefined=sbi_reboot
    --undefined=sbi_probe_extensions
    --undefined=sbi_ext_is_available
    --undefined=sbi_ext_name
    --undefined=sbi_error_str
    --undefined=sbi_hart_state_str
    --undefined=sbi_print_version
    --undefined=sbi_start_secondary_harts
    --undefined=sbi_console_putchar
    --undefined=sbi_console_puts
    --undefined=sbi_console_getchar
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
    # irq/plic.c port
    --undefined=__plic_mmio_base
    --undefined=plicinit
    --undefined=plicinithart
    --undefined=plic_claim
    --undefined=plic_complete
    --undefined=plic_enable_irq
    # irq/irq.c port
    --undefined=irq_desc_init
    --undefined=register_irq_handler
    --undefined=unregister_irq_handler
    --undefined=do_irq
    # irq/trap.c port (Phase 2 Wave 6) -- kernelvec.S/trampoline.S call
    # these by symbol name (`call`/`.dword`), start_kernel.c calls
    # trapinit()/trapinithart(), and proc/{clone,thread}.rs call
    # usertrapret(); proc/signal.rs calls push_sigframe()/
    # restore_sigframe(). enter_irq()/exit_irq() are part of trap.h's
    # declared API surface even though no current caller other than this
    # file's own kernel_irq()/user_kirq_entrance() reaches them.
    --undefined=trapinit
    --undefined=trapinithart
    --undefined=kerneltrap
    --undefined=kernel_irq
    --undefined=enter_irq
    --undefined=exit_irq
    --undefined=usertrap
    --undefined=usertrapret
    --undefined=user_kirq_entrance
    --undefined=__user_kirq_return
    --undefined=push_sigframe
    --undefined=restore_sigframe
    # irq/syscall.c port (Phase 2 Wave 7) -- kernelvec.S/trampoline.S never
    # reference these by symbol name, and (with syscall.c gone) nothing in
    # the remaining C tree references them either: `irq/trap.rs`'s
    # usertrap() -> syscall() call and the dispatch table's own function-
    # pointer references to the still-C sys_vfs_*/sys_exec/etc. are both
    # intra-staticlib (Rust calling Rust), so without these flags the
    # linker could drop the whole module.
    --undefined=syscall
    --undefined=fetchaddr
    --undefined=fetchstr
    --undefined=argraw
    --undefined=argint
    --undefined=argint64
    --undefined=argaddr
    --undefined=argstr
    # timer/timer.c port (Phase 2 Wave 8) -- timer wheel core.
    # `__clint_timer_irqno`/`__timebase_frequency` have no live C reader
    # anymore (timer.c deleted); `get_jiffs`/`timer_add`/`timer_remove`/
    # `timer_tick`/`timer_node_init` are called only by other Rust files
    # (`timer/sched_timer.rs` intra-module, `lock/*.rs`/`proc/sysproc.rs`
    # cross-module via their own `extern` declarations) -- without these
    # flags the linker could drop the whole module. `timer_init` and
    # `__jiff_ticks` are omitted: `timer_init` is reached transitively from
    # `sched_timer_init` (forced below) and `__jiff_ticks` is still written
    # by `dev/fdt.c` (a live C translation unit), which already creates a
    # real undefined-symbol demand.
    --undefined=__clint_timer_irqno
    --undefined=__timebase_frequency
    --undefined=get_jiffs
    --undefined=timer_node_init
    --undefined=timer_add
    --undefined=timer_remove
    --undefined=timer_tick
    # timer/sched_timer.c port (Phase 2 Wave 8) -- scheduler-tick consumer
    # + sleep/timeout API. `sched_timer_init`/`sleep_ms` are also called
    # directly from `start_kernel.c`, which already demands them; forced
    # anyway per the plan's "every new public symbol" rule. `__do_timer_tick`
    # is called only from `proc/sched.rs`'s own `extern` declaration.
    --undefined=sched_timer_tick
    --undefined=__do_timer_tick
    --undefined=sched_timer_set
    --undefined=sched_timer_done
    --undefined=sleep_ms
    --undefined=sleep_ms_interruptible
    --undefined=sched_timer_init
    --undefined=sched_timer_add_deadline
    --undefined=sched_timer_add
    # timer/goldfish_rtc.c port (Phase 2 Wave 8) -- RTC MMIO driver.
    # `goldfish_rtc_init` has no live caller at all (`start_kernel.c`'s call
    # site is commented out, unchanged by this port -- see
    # `goldfish_rtc.rs`'s module doc); the rest are called only from
    # `proc/sysproc.rs`'s own `extern` declaration (`goldfish_rtc_read_ns`)
    # or have no external caller yet either.
    --undefined=__goldfish_rtc_mmio_base
    --undefined=__goldfish_rtc_irqno
    --undefined=goldfish_rtc_read_ns
    --undefined=goldfish_rtc_read_sec
    --undefined=goldfish_rtc_set_alarm_ns
    --undefined=goldfish_rtc_set_alarm_sec
    --undefined=goldfish_rtc_clear_alarm
    --undefined=goldfish_rtc_irq_enable
    --undefined=goldfish_rtc_get_alarm_count
    --undefined=goldfish_rtc_init
    # ipi/ipi.c port (Phase 2 Wave 9) -- IPI send/receive + per-CPU
    # bring-up bookkeeping. `ipi_init`/`cpus_init`/`mycpu_init` are called
    # from `start_kernel.c` (a live C translation unit, real demand
    # already); `ipi_send_single`/`ipi_send_all` are called only from
    # other Rust files' own `extern` declarations
    # (`proc/sched.rs`/`proc/signal.rs`/`proc/thread_group.rs`,
    # `printf.rs`); `ipi_send_mask`/`ipi_send_all_but_self` and
    # `get_cpu_active_mask` have no live caller anywhere yet. `cpus` (the
    # per-CPU array) is read by `proc/rq.rs`/`lock/rcu.rs`/`irq/trap.rs`/
    # `mm/vm_pgtab.rs` via their own typed `extern` declarations of the
    # same symbol. Forced anyway per the plan's "every new public symbol"
    # rule -- without these flags the linker could drop the whole module.
    --undefined=ipi_init
    --undefined=ipi_send_single
    --undefined=ipi_send_mask
    --undefined=ipi_send_all_but_self
    --undefined=ipi_send_all
    --undefined=cpus_init
    --undefined=mycpu_init
    --undefined=get_cpu_active_mask
    --undefined=cpus
    # tty/termios.c + tty/tty_dev.c + tty/pty.c port (Phase 2 Wave 10).
    # `termios_init_default`/`ttydevinit`/`pty_alloc`/`pty_master_read`/
    # `pty_master_write` are all called from the still-C tty.c/ptmx.c/
    # start_kernel.c (real demand already); `pty_dealloc` has no live
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
    # ptmxinit are called from start_kernel.c. Forced anyway per the
    # plan's "every new public symbol" rule.
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
    # called from `start_kernel.c`/`proc/thread.rs` (real demand
    # already); the rest are called only from other Rust files' own
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
    # kernel/mm/vm.rs (vfs_fput/vfs_fdup), from kernel/exec.c, and
    # (pipe_*) from kernel/tty/tty.rs, kernel/tty/pty.rs,
    # kernel/console.rs, and kernel/start_kernel.c (pipe_init) --
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
    # vfs_syscall.c) and kernel/exec.c, and from kernel/proc/{clone,exit}.rs
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
    # xv6fs/, devtmpfs/), from kernel/start_kernel.c (vfs_init), and from
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
    # `vfs_init()`/`start_kernel.c` (real demand already). Every symbol
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
    # ptmx.rs, nullrand.c, virtio_disk.c, ramdisk.c, x1_sdhci.c,
    # dev_test.c), so all are forced per the plan's "every new public
    # symbol" rule.
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

if(DEFINED ENV{RWLOCK_TEST})
    target_compile_definitions(${KERNEL_BINARY} PRIVATE RWAD_WRITE_TEST=1)
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    target_compile_definitions(${KERNEL_BINARY} PRIVATE SEMAPHORE_RUNTIME_TEST=1)
endif()

if(DEFINED ENV{WORKQUEUE_RUNTIME_SMOKE_TEST})
    message(STATUS "WORKQUEUE_RUNTIME_SMOKE_TEST enabled")
    target_compile_definitions(${KERNEL_BINARY} PRIVATE WORKQUEUE_RUNTIME_SMOKE_TEST=1)
endif()

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

if(DEFINED ENV{RWLOCK_TEST})
    target_compile_definitions(kernel_with_symbols_elf PRIVATE RWAD_WRITE_TEST=1)
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    target_compile_definitions(kernel_with_symbols_elf PRIVATE SEMAPHORE_RUNTIME_TEST=1)
endif()

if(DEFINED ENV{WORKQUEUE_RUNTIME_SMOKE_TEST})
    target_compile_definitions(kernel_with_symbols_elf PRIVATE WORKQUEUE_RUNTIME_SMOKE_TEST=1)
endif()

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