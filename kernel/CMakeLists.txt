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
    ${KERNEL_DIR}/string.c
    ${KERNEL_DIR}/bio.c
    ${KERNEL_DIR}/exec.c
    ${KERNEL_DIR}/virtio_disk.c
    ${KERNEL_DIR}/ramdisk.c
    ${KERNEL_DIR}/backtrace.c
    ${KERNEL_DIR}/e1000.c
    ${KERNEL_DIR}/net.c
    ${KERNEL_DIR}/pci.c
    ${KERNEL_DIR}/sysnet.c
    ${KERNEL_DIR}/hlist.c
    ${KERNEL_DIR}/bintree.c
    ${KERNEL_DIR}/rbtree.c
    ${KERNEL_DIR}/kobject.c
    # NOTE: bits.c has been rewritten in Rust. The lookup tables it provided
    # (__uint8_bits_count, __uint8_trailing_zeros, __uint8_leading_zeros,
    # __uint8_inverse) are now produced by the `xv6_rust` staticlib and
    # whole-archive-linked into the kernel below.
    ${KERNEL_DIR}/sbi.c
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
    ${RUST_CRATE_DIR}/mm/*.rs
    ${RUST_CRATE_DIR}/lock/*.rs
    ${RUST_CRATE_DIR}/machine/*.rs
    ${RUST_CRATE_DIR}/sync/*.rs)

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

if(DEFINED ENV{RWLOCK_TEST})
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/lock/rwlock_test.c)
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/lock/semaphore_test.c)
endif()

set(KERNEL_SCAN_OBJS
    ${KERNEL_DIR}/start.c
    ${KERNEL_DIR}/console.c
    ${KERNEL_DIR}/printf.c
    ${KERNEL_DIR}/uart.c
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
add_subdirectory(${KERNEL_DIR}/irq)
add_subdirectory(${KERNEL_DIR}/dev)
add_subdirectory(${KERNEL_DIR}/lock)
add_subdirectory(${KERNEL_DIR}/proc)
add_subdirectory(${KERNEL_DIR}/timer)
add_subdirectory(${KERNEL_DIR}/vfs)
add_subdirectory(${KERNEL_DIR}/ipi)
add_subdirectory(${KERNEL_DIR}/tty)

set_source_files_properties(${KERNEL_C_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_SCAN_OBJS} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_S_FILES} PROPERTIES LANGUAGE C)

set(KERNEL_OBJS ${KERNEL_S_FILES} ${KERNEL_C_FILES} ${KERNEL_SCAN_OBJS})

# Object libraries to link
set(KERNEL_OBJ_LIBS
    $<TARGET_OBJECTS:irq>
    $<TARGET_OBJECTS:dev>
    $<TARGET_OBJECTS:lock>
    $<TARGET_OBJECTS:proc>
    $<TARGET_OBJECTS:vfs_core>
    $<TARGET_OBJECTS:vfs_tmpfs>
    $<TARGET_OBJECTS:vfs_xv6fs>
    $<TARGET_OBJECTS:vfs_devtmpfs>
    $<TARGET_OBJECTS:ipi>
    $<TARGET_OBJECTS:timer>
    $<TARGET_OBJECTS:tty>
)

set(KERNEL_LINK_LIBS irq dev lock proc timer vfs tty)

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
set(KSYM_PLACEHOLDER_SIZE "0x100000")
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