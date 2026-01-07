set(KERNEL_BINARY kernel)
set(KERNEL_DECOMPILED kernel.asm)
set(KERNEL_SYMBOLS kernel.sym)

if(NOT DEFINED ENV{LAB})
    message(FATAL_ERROR "Environment variable LAB is not set")
else()
    message(STATUS "Building kernel for lab: $ENV{LAB}")
endif()

# Core kernel sources in this directory (excluding subdirectories).
set(KERNEL_C_FILES
    ${KERNEL_DIR}/start_kernel.c
    ${KERNEL_DIR}/string.c
    ${KERNEL_DIR}/bio.c
    ${KERNEL_DIR}/pipe.c
    ${KERNEL_DIR}/exec.c
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
    ${KERNEL_DIR}/sbi.c
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
)

# Generate asm-offsets.h before building anything else
add_subdirectory(${KERNEL_DIR}/inc)

# Pull in subdirectory object libraries.
add_subdirectory(${KERNEL_DIR}/interruption)
add_subdirectory(${KERNEL_DIR}/dev)
add_subdirectory(${KERNEL_DIR}/lock)
add_subdirectory(${KERNEL_DIR}/mm)
add_subdirectory(${KERNEL_DIR}/proc)
add_subdirectory(${KERNEL_DIR}/vfs)

set_source_files_properties(${KERNEL_C_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_SCAN_OBJS} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_S_FILES} PROPERTIES LANGUAGE C)

set(KERNEL_OBJS
    ${KERNEL_S_FILES}
    ${KERNEL_C_FILES}
    ${KERNEL_SCAN_OBJS}
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding ")

# Add kernel executable from top-level objects; link in subdir object libraries below.
add_executable(${KERNEL_BINARY} ${KERNEL_OBJS})

# Make sure asm-offsets.h is generated before building kernel
add_dependencies(${KERNEL_BINARY} generate_asm_offsets)

# Object libraries do not automatically link; add their objects explicitly.
target_sources(${KERNEL_BINARY} PRIVATE
    $<TARGET_OBJECTS:interruption>
    $<TARGET_OBJECTS:dev>
    $<TARGET_OBJECTS:lock>
    $<TARGET_OBJECTS:mm>
    $<TARGET_OBJECTS:proc>
    $<TARGET_OBJECTS:vfs_core>
    $<TARGET_OBJECTS:vfs_tmpfs>
    $<TARGET_OBJECTS:vfs_xv6fs>
)

# Use target_link_options to set linker flags so they appear in <LINK_FLAGS>
target_link_options(${KERNEL_BINARY} PRIVATE -z max-page-size=4096 -T ${KERNEL_DIR}/kernel.ld)
target_link_libraries(${KERNEL_BINARY} PRIVATE interruption dev lock mm proc vfs)

# Note: USE_LIBSBI is not applicable for S-mode kernels.
# libsbi is for M-mode firmware (OpenSBI itself), not for S-mode OS kernels.
# S-mode kernels call SBI services via ecall instruction, which is already
# implemented in kernel/sbi.c. The OpenSBI build (OPENSBI_MODE=build) provides
# the fw_jump.elf firmware that QEMU loads to handle these SBI calls.

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