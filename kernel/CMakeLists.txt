set(KERNEL_BINARY kernel)
set(KERNEL_DECOMPILED kernel.asm)
set(KERNEL_SYMBOLS kernel.sym)

# ==============================================================================
# Platform configuration
# ==============================================================================
# PLATFORM is set by the top-level CMakeLists.txt:
#   - qemu (default): KERNEL_BASE=0x80200000
#   - orangepi:       KERNEL_BASE=0x00200000
# ==============================================================================
if(ARCH STREQUAL "x86_64")
    set(KERNEL_BASE "0x00100000")
    set(KERNEL_OFFSET "0xFFFF800000000000")
elseif(PLATFORM STREQUAL "orangepi")
    set(KERNEL_BASE "0x00200000")
    set(KERNEL_OFFSET "0xFFFFFFC000000000")
else()
    set(KERNEL_BASE "0x80200000")
    set(KERNEL_OFFSET "0xFFFFFFC000000000")
endif()

message(STATUS "Building kernel for lab: $ENV{LAB}")
message(STATUS "Platform: ${PLATFORM}, kernel base: ${KERNEL_BASE}, page offset: ${KERNEL_OFFSET}")

# Core kernel sources in this directory (excluding subdirectories).
set(KERNEL_C_FILES
    ${KERNEL_DIR}/start_kernel.c
    ${KERNEL_DIR}/string.c
    ${KERNEL_DIR}/string_selftest.c
    ${KERNEL_DIR}/bio.c
    ${KERNEL_DIR}/exec.c
    ${KERNEL_DIR}/virtio_disk.c
    ${KERNEL_DIR}/virtio_gpu.c
    ${KERNEL_DIR}/virtio_input.c
    ${KERNEL_DIR}/virtio_snd.c
    ${KERNEL_DIR}/ramdisk.c
    ${KERNEL_DIR}/ksymbols.c
    ${KERNEL_DIR}/kde_ready_trace.c
    ${KERNEL_DIR}/e1000.c
    ${KERNEL_DIR}/virtio_net.c
    ${KERNEL_DIR}/net.c
    ${KERNEL_DIR}/pci.c
    ${KERNEL_DIR}/sysnet.c
    ${KERNEL_DIR}/hlist.c
    ${KERNEL_DIR}/bintree.c
    ${KERNEL_DIR}/rbtree.c
    ${KERNEL_DIR}/maple_tree.c
    ${KERNEL_DIR}/xarray.c
    ${KERNEL_DIR}/kobject.c
    ${KERNEL_DIR}/bits.c
    ${KERNEL_DIR}/accounting.c
    ${KERNEL_DIR}/power.c
    ${KERNEL_DIR}/cmdline.c
    ${KERNEL_DIR}/coredump.c
)

if(DEFINED ENV{RWLOCK_TEST})
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/lock/rwlock_test.c)
endif()

if(DEFINED ENV{SEMAPHORE_TEST})
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/lock/semaphore_test.c)
endif()

set(KERNEL_SCAN_OBJS
    ${KERNEL_DIR}/console.c
    ${KERNEL_DIR}/printf.c
    ${KERNEL_DIR}/klog.c
    ${KERNEL_DIR}/uart.c
    ${KERNEL_DIR}/diag.c
)

if(DEFINED ENV{KSCAN})
    list(APPEND KERNEL_SCAN_OBJS ${KERNEL_DIR}/kscan.c)
endif()

if($ENV{LAB} STREQUAL "lock")
    list(APPEND KERNEL_C_FILES ${KERNEL_DIR}/stats.c ${KERNEL_DIR}/sprintf.c)
endif()

set(KERNEL_S_FILES)

# Generate asm-offsets.h before building anything else
add_subdirectory(${KERNEL_DIR}/inc)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding -DUSE_LWIP=1 ")
include_directories(${CMAKE_SOURCE_DIR}/arch/${ARCH}/inc)

# Pull in subdirectory object libraries.
add_subdirectory(${CMAKE_SOURCE_DIR}/arch/${ARCH} ${CMAKE_BINARY_DIR}/arch/${ARCH})
add_subdirectory(${KERNEL_DIR}/irq)
add_subdirectory(${KERNEL_DIR}/dev)
add_subdirectory(${KERNEL_DIR}/lock)
add_subdirectory(${KERNEL_DIR}/mm)
add_subdirectory(${KERNEL_DIR}/proc)
add_subdirectory(${KERNEL_DIR}/timer)
add_subdirectory(${KERNEL_DIR}/vfs)
add_subdirectory(${KERNEL_DIR}/ipi)
add_subdirectory(${KERNEL_DIR}/tty)
add_subdirectory(${KERNEL_DIR}/lwip_port)
add_subdirectory(${KERNEL_DIR}/lwext4_port)
add_subdirectory(${KERNEL_DIR}/daemons)
add_subdirectory(${KERNEL_DIR}/gdbstub)
add_subdirectory(${KERNEL_DIR}/kqueue)
add_subdirectory(${KERNEL_DIR}/ipc)

set_source_files_properties(${KERNEL_C_FILES} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_SCAN_OBJS} PROPERTIES LANGUAGE C)
set_source_files_properties(${KERNEL_S_FILES} PROPERTIES LANGUAGE C)

set(KERNEL_OBJS ${KERNEL_S_FILES} ${KERNEL_C_FILES} ${KERNEL_SCAN_OBJS})

# Object libraries to link
set(KERNEL_OBJ_LIBS
    $<TARGET_OBJECTS:arch_${ARCH}>
    $<TARGET_OBJECTS:irq>
    $<TARGET_OBJECTS:dev>
    $<TARGET_OBJECTS:lock>
    $<TARGET_OBJECTS:mm>
    $<TARGET_OBJECTS:proc>
    $<TARGET_OBJECTS:vfs_core>
    $<TARGET_OBJECTS:vfs_tmpfs>
    $<TARGET_OBJECTS:vfs_xv6fs>
    $<TARGET_OBJECTS:vfs_devtmpfs>
    $<TARGET_OBJECTS:vfs_procfs>
    $<TARGET_OBJECTS:vfs_sysfs>
    $<TARGET_OBJECTS:ipi>
    $<TARGET_OBJECTS:timer>
    $<TARGET_OBJECTS:tty>
    $<TARGET_OBJECTS:lwip>
    $<TARGET_OBJECTS:ext4fs>
    $<TARGET_OBJECTS:daemons>
    $<TARGET_OBJECTS:gdbstub>
    $<TARGET_OBJECTS:kqueue_core>
    $<TARGET_OBJECTS:ipc>
)

set(KERNEL_LINK_LIBS arch_${ARCH} irq dev lock mm proc timer vfs tty lwip ext4fs daemons gdbstub kqueue_core ipc)

# ==============================================================================
# Linker scripts (generated from kernel.ld.in template)
# ==============================================================================
# Two linker scripts are generated:
#   kernel_plain.ld  — for the plain kernel (no symbol index, idx size = 0)
#   kernel_sym.ld    — for kernel_with_symbols_elf (idx size computed from
#                      DWARF line entries in the plain kernel)
#
# The .ksymbols_idx section reserves BSS space for __ksymbols_t rb-tree nodes.
# Its size is computed at build time by scripts/gen_kernel_ld.py, which counts
# DWARF decoded-line entries and multiplies by sizeof(__ksymbols_t).
# ==============================================================================
if(ARCH STREQUAL "x86_64")
    set(KERNEL_LD_IN ${KERNEL_DIR}/kernel_x86_64.ld.in)
else()
    set(KERNEL_LD_IN ${KERNEL_DIR}/kernel.ld.in)
endif()

# Plain kernel linker script (no symbol index needed)
set(KERNEL_SYMBOLS_IDX_SIZE "0")
set(KERNEL_LD_PLAIN ${CMAKE_CURRENT_BINARY_DIR}/kernel_plain.ld)
configure_file(${KERNEL_LD_IN} ${KERNEL_LD_PLAIN} @ONLY)

# Computed linker script (generated at build time after plain kernel is linked)
set(GEN_KERNEL_LD_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/gen_kernel_ld.py)
set(KERNEL_LD_SYM ${CMAKE_CURRENT_BINARY_DIR}/kernel_sym.ld)

# ==============================================================================
# Plain kernel (without embedded symbols)
# ==============================================================================

# Common compile definitions for optional kernel tests
set(KERNEL_OPTIONAL_COMPILE_DEFS "")
if(DEFINED ENV{RWLOCK_TEST})
    list(APPEND KERNEL_OPTIONAL_COMPILE_DEFS RWAD_WRITE_TEST=1)
endif()
if(DEFINED ENV{SEMAPHORE_TEST})
    list(APPEND KERNEL_OPTIONAL_COMPILE_DEFS SEMAPHORE_RUNTIME_TEST=1)
endif()

add_executable(${KERNEL_BINARY} ${KERNEL_OBJS})
add_dependencies(${KERNEL_BINARY} generate_asm_offsets)
target_sources(${KERNEL_BINARY} PRIVATE ${KERNEL_OBJ_LIBS})
target_link_options(${KERNEL_BINARY} PRIVATE -z max-page-size=4096 -T ${KERNEL_LD_PLAIN})
target_link_libraries(${KERNEL_BINARY} PRIVATE ${KERNEL_LINK_LIBS})
if(KERNEL_OPTIONAL_COMPILE_DEFS)
    target_compile_definitions(${KERNEL_BINARY} PRIVATE ${KERNEL_OPTIONAL_COMPILE_DEFS})
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
# Phase 1: Build kernel with a placeholder .ksymbols section
# Phase 2: Generate symbols from the placeholder kernel and embed them
#
# Both .ksymbols placeholder size and .ksymbols_idx size are computed from
# DWARF decoded-line entries in the plain kernel via estimate_symbol_size.py.
# ==============================================================================
set(ESTIMATE_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/estimate_symbol_size.py)
set(KSYM_PLACEHOLDER_ASM ${CMAKE_CURRENT_BINARY_DIR}/ksymbols_placeholder.S)

# Compute .ksymbols size from plain kernel DWARF data and generate placeholder
add_custom_command(
    OUTPUT ${KSYM_PLACEHOLDER_ASM}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
            auto ${CMAKE_OBJDUMP} $<TARGET_FILE:${KERNEL_BINARY}>
            ${KSYM_PLACEHOLDER_ASM}
    COMMENT "Computing .ksymbols size and generating placeholder"
    DEPENDS ${KERNEL_BINARY}
            ${CMAKE_SOURCE_DIR}/scripts/gen_ksymbols_placeholder.py
            ${ESTIMATE_SCRIPT}
)

add_library(ksymbols_placeholder OBJECT EXCLUDE_FROM_ALL ${KSYM_PLACEHOLDER_ASM})
add_dependencies(ksymbols_placeholder generate_asm_offsets)
set_source_files_properties(${KSYM_PLACEHOLDER_ASM} PROPERTIES GENERATED TRUE LANGUAGE C)

# Compute .ksymbols_idx size from plain kernel's DWARF data, then generate
# the final linker script for the symbols kernel.
add_custom_command(
    OUTPUT ${KERNEL_LD_SYM}
    COMMAND python3 ${GEN_KERNEL_LD_SCRIPT}
            ${KERNEL_LD_IN}
            ${KERNEL_LD_SYM}
            ${CMAKE_OBJDUMP}
            $<TARGET_FILE:${KERNEL_BINARY}>
            ${KERNEL_BASE}
            ${KERNEL_OFFSET}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${KERNEL_BINARY} ${GEN_KERNEL_LD_SCRIPT} ${KERNEL_LD_IN} ${ESTIMATE_SCRIPT}
    COMMENT "Computing .ksymbols_idx size and generating kernel_sym.ld"
)
add_custom_target(kernel_sym_ld DEPENDS ${KERNEL_LD_SYM})

# Phase 1: Kernel with placeholder (uses computed linker script)
add_executable(kernel_with_symbols_elf EXCLUDE_FROM_ALL ${KERNEL_OBJS})
add_dependencies(kernel_with_symbols_elf generate_asm_offsets ksymbols_placeholder kernel_sym_ld)
target_sources(kernel_with_symbols_elf PRIVATE ${KERNEL_OBJ_LIBS} $<TARGET_OBJECTS:ksymbols_placeholder>)
target_link_options(kernel_with_symbols_elf PRIVATE -z max-page-size=4096 -T ${KERNEL_LD_SYM})
target_link_libraries(kernel_with_symbols_elf PRIVATE ${KERNEL_LINK_LIBS})
if(KERNEL_OPTIONAL_COMPILE_DEFS)
    target_compile_definitions(kernel_with_symbols_elf PRIVATE ${KERNEL_OPTIONAL_COMPILE_DEFS})
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
if(ARCH STREQUAL "x86_64")
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:kernel_with_symbols_elf> xv6.payload.bin
        COMMAND python3 ${CMAKE_SOURCE_DIR}/arch/x86_64/scripts/make_linux_x86_image.py xv6.payload.bin xv6.bin
        COMMAND python3 ${CMAKE_SOURCE_DIR}/arch/x86_64/scripts/verify_linux_x86_boot_header.py xv6.bin
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating xv6.bin (${PLATFORM})"
        DEPENDS kernel_with_symbols $<TARGET_FILE:kernel_with_symbols_elf>
                ${CMAKE_SOURCE_DIR}/arch/x86_64/scripts/make_linux_x86_image.py
                ${CMAKE_SOURCE_DIR}/arch/x86_64/scripts/verify_linux_x86_boot_header.py
    )
else()
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:kernel_with_symbols_elf> xv6.bin
        COMMAND python3 ${CMAKE_SOURCE_DIR}/arch/riscv/scripts/patch_image_size.py xv6.bin
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating xv6.bin (${PLATFORM})"
        DEPENDS kernel_with_symbols $<TARGET_FILE:kernel_with_symbols_elf>
    )
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.sym
    COMMAND ${CMAKE_COMMAND} -E copy ${KERNEL_SYM_FINAL} xv6.sym
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating xv6.sym"
    DEPENDS ${KERNEL_SYM_FINAL}
)

# ==============================================================================
# Compressed kernel image (optional, for U-Boot deployment)
# ==============================================================================
if(ENABLE_COMPRESSION AND GZIP)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin.gz
        COMMAND ${GZIP} -9 -k -f xv6.bin
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Compressing xv6.bin -> xv6.bin.gz"
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin
    )
    set(KERNEL_BIN_COMPRESSED ${CMAKE_CURRENT_BINARY_DIR}/xv6.bin.gz)
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
