set(KERNEL_BINARY kernel)
set(KERNEL_DECOMPILED kernel.asm)
set(KERNEL_SYMBOLS kernel.sym)

if(NOT DEFINED ENV{LAB})
    message(FATAL_ERROR "Environment variable LAB is not set")
else()
    message(STATUS "Building kernel for lab: $ENV{LAB}")
endif()

# Separate .S and .c files for kernel source files
set(KERNEL_C_FILES
    ${KERNEL_C_FILES}
    ${KERNEL_DIR}/start_kernel.c
    ${KERNEL_DIR}/string.c
    ${KERNEL_DIR}/main.c
    ${KERNEL_DIR}/trap.c
    ${KERNEL_DIR}/syscall.c
    ${KERNEL_DIR}/bio.c
    ${KERNEL_DIR}/pipe.c
    ${KERNEL_DIR}/exec.c
    ${KERNEL_DIR}/plic.c
    ${KERNEL_DIR}/virtio_disk.c
    ${KERNEL_DIR}/backtrace.c
    ${KERNEL_DIR}/e1000.c
    ${KERNEL_DIR}/net.c
    ${KERNEL_DIR}/pci.c
    ${KERNEL_DIR}/sysnet.c
    ${KERNEL_DIR}/hlist.c
    ${KERNEL_DIR}/bintree.c
    ${KERNEL_DIR}/rbtree.c
    ${KERNEL_DIR}/kobject.c
    ${KERNEL_DIR}/bits.c
)

set(KERNEL_C_FILES
    ${KERNEL_C_FILES}
    ${KERNEL_DIR}/vfs/fs.c
    ${KERNEL_DIR}/vfs/inode.c
    ${KERNEL_DIR}/vfs/file.c
    ${KERNEL_DIR}/vfs/fdtable.c
    ${KERNEL_DIR}/vfs/vfs_syscall.c
)

set(KERNEL_C_FILES
    ${KERNEL_C_FILES}
    ${KERNEL_DIR}/vfs/tmpfs/superblock.c
    ${KERNEL_DIR}/vfs/tmpfs/inode.c
    ${KERNEL_DIR}/vfs/tmpfs/truncate.c
    ${KERNEL_DIR}/vfs/tmpfs/file.c
    ${KERNEL_DIR}/vfs/tmpfs/tmpfs_smoketest.c
)

set(KERNEL_C_FILES
    ${KERNEL_C_FILES}
    ${KERNEL_DIR}/vfs/xv6fs/superblock.c
    ${KERNEL_DIR}/vfs/xv6fs/inode.c
    ${KERNEL_DIR}/vfs/xv6fs/file.c
    ${KERNEL_DIR}/vfs/xv6fs/log.c
    ${KERNEL_DIR}/vfs/xv6fs/xv6fs_smoketest.c
)

set(KERNEL_C_FILES
    ${KERNEL_C_FILES}
    ${KERNEL_DIR}/lock/mutex.c
    ${KERNEL_DIR}/lock/semaphore.c
    ${KERNEL_DIR}/lock/semaphore_test.c
    ${KERNEL_DIR}/lock/rwlock.c
    ${KERNEL_DIR}/lock/rwlock_test.c
    ${KERNEL_DIR}/lock/spinlock.c
    ${KERNEL_DIR}/lock/completion.c
)

set(KERNEL_C_FILES
    ${KERNEL_C_FILES}
    ${KERNEL_DIR}/mm/kalloc.c
    ${KERNEL_DIR}/mm/page.c
    ${KERNEL_DIR}/mm/slab.c
    ${KERNEL_DIR}/mm/vm.c
    ${KERNEL_DIR}/mm/pcache.c
)

set(KERNEL_C_FILES
    ${KERNEL_C_FILES}
    ${KERNEL_DIR}/proc/proc.c
    ${KERNEL_DIR}/proc/proc_queue.c
    ${KERNEL_DIR}/proc/sched.c
    ${KERNEL_DIR}/proc/signal.c
    ${KERNEL_DIR}/proc/sys_signal.c
    ${KERNEL_DIR}/proc/sysproc.c
    ${KERNEL_DIR}/proc/workqueue.c
    ${KERNEL_DIR}/proc/timer.c
    ${KERNEL_DIR}/proc/sched_timer.c
)

set(KERNEL_C_FILES
    ${KERNEL_C_FILES}
    ${KERNEL_DIR}/dev/dev.c
    ${KERNEL_DIR}/dev/cdev.c
    ${KERNEL_DIR}/dev/blkdev.c
    ${KERNEL_DIR}/dev/bio.c
)

if(DEFINED ENV{RWLOCK_TEST})
    set(KERNEL_C_FILES
        ${KERNEL_C_FILES}
        ${KERNEL_DIR}/lock/rwlock_test.c
    )
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    set(KERNEL_C_FILES
        ${KERNEL_C_FILES}
        ${KERNEL_DIR}/lock/semaphore_test.c
    )
endif()

set(KERNEL_SCAN_OBJS
    ${KERNEL_DIR}/start.c
    ${KERNEL_DIR}/console.c
    ${KERNEL_DIR}/printf.c
    ${KERNEL_DIR}/uart.c
)

if(DEFINED ENV{KSCAN})
    set(KERNEL_SCAN_OBJS
        ${KERNEL_SCAN_OBJS}
        ${KERNEL_DIR}/kscan.c
    )
endif()

if($ENV{LAB} STREQUAL "lock")
    set(KERNEL_C_FILES
        ${KERNEL_C_FILES}
        ${KERNEL_DIR}/stats.c
        ${KERNEL_DIR}/sprintf.c
    )
endif()

if($ENV{LAB} STREQUAL "net")
    set(KERNEL_C_FILES
        ${KERNEL_C_FILES}
        ${KERNEL_DIR}/e1000.c
        ${KERNEL_DIR}/net.c
        ${KERNEL_DIR}/pci.c
    )
endif()

set(KERNEL_S_FILES
    ${KERNEL_DIR}/entry.S
    ${KERNEL_DIR}/kernelvec.S
    ${KERNEL_DIR}/trampoline.S
    ${KERNEL_DIR}/proc/swtch.S
    ${KERNEL_DIR}/proc/sig_trampoline.S
)

set_source_files_properties(${KERNEL_C_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_SCAN_OBJS} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_S_FILES} PROPERTIES LANGUAGE C)

set(KERNEL_OBJS
    ${KERNEL_S_FILES}
    ${KERNEL_C_FILES}
    ${KERNEL_SCAN_OBJS}
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding ")

# Add kernel executable
add_executable(${KERNEL_BINARY} ${KERNEL_OBJS})

# Use target_link_options to set linker flags so they appear in <LINK_FLAGS>
target_link_options(${KERNEL_BINARY} PRIVATE -z max-page-size=4096 -T ${KERNEL_DIR}/kernel.ld)

if(DEFINED ENV{RWLOCK_TEST})
    target_compile_definitions(${KERNEL_BINARY} PRIVATE
        RWAD_WRITE_TEST=1
    )
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    target_compile_definitions(${KERNEL_BINARY} PRIVATE
        SEMAPHORE_RUNTIME_TEST=1
    )
endif()

# decompile the kernel binary
add_custom_target(${KERNEL_DECOMPILED}
    COMMAND ${CMAKE_OBJDUMP} -S ${KERNEL_BINARY} > ${KERNEL_DECOMPILED}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Decompiling kernel binary to ${KERNEL_DECOMPILED}"
    DEPENDS ${KERNEL_BINARY}
)

# extract symbols from the kernel binary
add_custom_target(${KERNEL_SYMBOLS}
    COMMAND ${CMAKE_OBJDUMP} -t ${KERNEL_BINARY} | sed '1,/SYMBOL TABLE/d\; s/ .* / /\; /^$$/d' > ${KERNEL_SYMBOLS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Extracting symbols from kernel binary to ${KERNEL_SYMBOLS}"
    DEPENDS ${KERNEL_BINARY}
)

# Add these targets as dependencies to ALL so they're built by default
add_custom_target(kernel_all ALL DEPENDS ${KERNEL_BINARY} ${KERNEL_DECOMPILED} ${KERNEL_SYMBOLS})